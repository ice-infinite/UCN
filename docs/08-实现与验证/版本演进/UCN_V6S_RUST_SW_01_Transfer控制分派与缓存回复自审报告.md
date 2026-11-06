# RUST-SW-01 第一段：Transfer 控制分派与缓存回复

> 日期：2026-10-14。范围：Rust Host 软件与编译验证。
> 状态：本段 SELF-REVIEW PASS；RUST-SW-01 顶层仍 IN PROGRESS，外审/生产 HOLD。
> 不访问串口、不烧录或复位、不提交推送；保留已有 C/Rust 工作树。

## 1. 本段完成什么

此前 `ControlCoordinator` 只能认证、严格解码并排入固定队列；认证接纳不是业务完成。
本段将 C1 Parent 的 Transfer Setup 和反向反馈接到独立的 Transfer Owner，再把 Setup
回复接到真实 Security 和 Adapter，形成 Host 可运行的建立/确认控制闭环。

```text
实际 Foundation 双槽/Witness/reload -> ParentOwner 发布 C1 Parent
Adapter 原子 RX -> Security verify/ACL -> Replay commit -> 不变 ControlCommand
Coordinator -> 精确 Parent 与当前 Session/身份复核
            -> 预留固定 Reply 槽 -> Transfer 接纳 -> 命令退休
Reply -> Security 构造并保护一次反向 C0 -> 冻结 Origin 字节
      -> 固定 Link/Adapter Token -> Driver submit -> 完成事实 -> 物理退休
认证重复请求 -> 原 Reply 缓存 -> 同字节重发，不再调用 Transfer 接纳
反向 ACK/SACK/Abort/Terminal -> 精确 TX Parent/Setup -> Transfer 状态提交
```

这些调用均由 Runtime/Coordinator 编排；Transport 没有调用 Security、Adapter、Driver、
Service 或其他业务 Owner。普通 Header、共同 Body、AAD/Tag 格式没有改变。

## 2. 新入口与职责

| 位置/入口 | 职责 | 错误路径 |
| --- | --- | --- |
| `ucn-runtime/control.rs`：`receive_cached` | 实际密码认证及 ACL 后区分新命令、仍在等待的精确重复、缓存回复 | 没有命令/缓存证据的 Replay 拒绝；新命令满不消费 Replay；坏 Tag/ACL 不触及缓存 |
| `ucn-runtime/transfer_control.rs`：`dispatch_transfer_setup` | 只使用已经持久化发布的 ParentOwner；复核 Realm、两端 Principal/Binding、Session/Policy/Key/Context；预留回复再接纳 Setup | 错 Parent、失效 Session、回复/重组满，命令保留，不驱逐旧消息 |
| 同文件：`dispatch_transfer_feedback` | 归一反向 Key，按完整 Parent/Transfer ID 查 TX；精确 Setup key、分片预算、终态代码 | 冲突/过期/错误方向不退休命令，TX 状态不变 |
| `ucn-runtime/reply.rs`：`seal/submit` | 回复只保护一次；发送时再检查授权、租期、原 Session/Key 和精确 Link | Provider 失败保留未保护义务；NOT_SUBMITTED 仅退休物理 Token，保留原回复 |
| 同文件：`retire_attempt/cancel_attempt/retire_cache` | 先查看、退休成功、再清 Holder；缓存只能过期且无物理 Holder 才回收 | Driver 不取消、InDoubt、退休失败都保留精确 Token；stop 不清空义务 |
| `ucn-transport/transfer.rs`：`find_tx/find_rx/control_feedback/receive_setup_at` | 完整父域查找、精确反馈、冻结接纳时刻 | 不按短 ID 或 digest 单独选择实例，不允许延迟分派刷新时限 |

Reply 生命周期为 `AwaitSeal -> Ready -> InFlight -> Cached`；确定未提交/确定发送失败后可
回到 Ready，但必须服从原截止、重试间隔和总尝试上限。确定发送成功只说明本地 Driver
完成，不证明远端已收到。Cached 只有再次收到通过认证的精确重复请求才重发。

缓存表、命令表和所有字节数组均编译期有界；满载不驱逐。大型缓存由调用方静态装配。
Reply seal 时固定整个 `LinkHandle`，不只固定相同数值的 Link Generation；重开或切换到
另一个 Link 不能让旧缓存静默恢复发送权限。Origin Key/Session 变化也不能重新解释旧字节。

## 3. 逐项自审与实际整改

### 3.1 分派时不能重置 Setup 的计时起点

命令认证接纳在 t=21，分派在 t=40，Setup lifetime=1 ms。正确截止必须为 1021，不是
1040。新增 `receive_setup_at(admitted_at, now)`，起点来自不可伪造的 ControlCommand，
并检查 `admitted_at <= now < deadline`。相同 Setup 继续使用已有 RX 的原截止。

### 3.2 终态代码必须和身份一样不可变

以前 typed `terminal(bool)` 只保留成功/失败；线上完整 Terminal 还包含 state/result。
新增控制反馈路径先精确核对完整 Parent 和从冻结 Setup 推导的 key，再保留 state/result。
重复相同终态幂等，改变结果码拒绝，不能改 DONE。每槽只保存非冗余的 state/result 和
Abort reason，不再次复制完整 Parent/Setup/digest，避免为控制接线膨胀业务槽。

### 3.3 Link 重开不是物理退休证明

对抗测试发现 Adapter 重开后保留 `Completed + Failure(InDoubt)`，而 `tx_retire` 原来
会释放它。本轮修正为返回 InDoubt，等待 Driver 迟到的精确完成事实后才能退休。
相应冲突完成与 Unknown 提交回归一并更新，禁止把“旧实例已围栏”误写为“副作用已证明”。

## 4. 新增六组对抗回归

`rust/crates/ucn-runtime/tests/transfer_dispatch.rs` 的六组回归涵盖：

1. 实际 Foundation 发布 Parent、Adapter 原子 RX、实际 HMAC/ACL、Setup 分派、反向 ACK
   保护/Adapter TX、完成事件，以及 ACK 丢失后原字节重发；Origin counter 只使用一次。
2. 错 Parent/失效 Session、满 Reply 表、满命令队列中的旧缓存查询、延迟分派不续租。
3. stop 后首次 Driver 不取消，Token 和缓存仍保留；第二次成功后才退休。
4. 合法认证 Peer 的错误 generation、fragment budget、Setup digest 和冲突终态代码。
5. 连续 NOT_SUBMITTED 的尝试上限、截止等值拒绝、槽复用 Handle ABA，以及 Unknown /
   reopen / 迟到完成证明；旧缓存不得改绑到新 Link。
6. 已过期的排队 Setup、认证来源/方向错误，都不能启动 RX。

既有 Adapter 回归另外钉住：重开后的未知 Holder 不能退休，精确迟到完成仍可解决它。
本段两个正反回读分别覆盖“认证→资源→状态→发送”和“重复→超时→stop→释放→复用”。

## 5. 验证结果

执行命令：`python tools/v6/run_rust_software_matrix.py --cargo
C:\Users\y2389_4rq4ld9\.cargo\bin\cargo.exe`。本轮完整运行结果如下：

| 门禁 | 本轮结果 |
| --- | --- |
| Workspace Debug / Release / MSRV 1.85 | 各 177/177 PASS |
| 新增 Transfer 分派与缓存对抗回归 | 6/6 PASS，包含在上面的 177 中，不重复累计 |
| Clippy `-D warnings` / rustfmt | PASS |
| Feature/依赖裁剪 | 13 组合 PASS；新增 Runtime 无功能、Transfer-only、Service-only、Realtime-only 的真实测试和 lib Clippy |
| C/Rust/Python Codec Oracle | 24/24 PASS，执行当前 native C，而非 Rust FFI |
| Cortex-M 软/硬浮点 no_std | 两 target 编译 PASS |
| ESP32-S3 xtensa | compile-only PASS，未操作开发板 |
| 文档检查 | 132 documents / 313 links PASS |

11 项矩阵均退出 0，开始/结束 367 个源码/合同依赖一致，`source_stable=true`。
原始副本：[software-matrix-sw01-transfer-control.json](../软件证据/Rust/2026-10-14/software-matrix-sw01-transfer-control.json)，
SHA256=`C0E5F47523E165227B97C43C88C76829608F91128A6B3659FF25DBF9CA1E76BC`。
文档/操作记录不在该源码快照中；后续修改源码必须重跑矩阵，不能仅保留旧 PASS。

Host `sizeof`：Transfer Nano/Lite/Full 为 2368/15272/58680 B；
`ControlCoordinator<2,128>` 为 584 B；Reply 缓存 `<1,128>/<2,256>/<4,512>` 为
584/1400/3808 B。均为该 Host 编译器布局，不是固定 Profile 的整机 RAM 或 MCU 最坏栈。
Reply 表只有明确装配时才存在，不扩大普通 Core；终态元数据相比旧 Transfer 每档增加
16/32/64 B，不复制完整响应 key 到每个 Transfer 槽。

本段未新跑 C 全配置 CTest、TSan/Miri/ASan，也没有真实 Flash/密码/时间戳验证；不复用
上一轮 C 108/108 或历史实机数字冒充本轮证据。`git diff --check` 无空白错误，仅 CRLF 提示；
新增未跟踪源码及报告另做尾随空白检查。

## 6. 尚未完成，不得借本段放行

- 本段只覆盖 **C1 Parent、受保护 C0、H0、直达控制事务**；当前 Origin 与每跳 Peer 仍
  未形成统一动态 Runtime，Flow feedback / 多跳返程不借用这里的单 Peer 身份。
- 分片的完整 raw Security→重组→业务交付→自动 SACK/Terminal 生成尚未在统一 Runtime
  接通；已有 typed 分片 Owner 测试不等同于完整 raw 分片管线。
- Parent 控制发布协议、Service RPC、Operation Query/Receipt/Cancel 的真实 Foundation
  编排，以及 Time 四报文统一命令分派仍需后续软件小项。
- C/Rust 的完整运行时互操作、目标 ELF 最坏栈/资源、真实 Flash/掉电、生产密码 Provider、
  ESP32/物理 Bearer/硬件打点、性能和长稳均未由本段证明。

下一项继续接入认证分片与业务交付，再推进 Service/Operation；不转入实机或新的功能模块。
