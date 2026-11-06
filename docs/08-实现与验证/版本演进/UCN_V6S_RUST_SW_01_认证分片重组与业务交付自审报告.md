# RUST-SW-01 第二段：认证分片、重组与业务交付

> 日期：2026-10-14；分支 `v6-simplified-rust`，`03eb7eb` 加完整工作树。
> 状态：本段 Host 软件 SELF-REVIEW PASS；RUST-SW-01 顶层仍 IN PROGRESS，等待外审。
> 不访问 COM、不烧录/复位、不提交或推送。不是 MCU、物理 Bearer 或生产放行。

## 1. 本段目标与边界

[上一段](UCN_V6S_RUST_SW_01_Transfer控制分派与缓存回复自审报告.md)已经接入认证 Setup、
反向反馈和固定缓存回复，但尚未把实际收到的业务分片接入重组。本段继续同一个任务，
不建立另一套 Wire，也不让 Transport Owner 直接调用 Security、Adapter 或 Service。

现在实际接通的是 **已发布 C1 Parent + H0 直达 + 受保护 OneWay Transfer**：

```text
Foundation 双槽/Witness/reload -> 发布完整 Parent
认证 raw Setup -> 固定 RX/Receipt -> 受保护 Setup ACK
源端分片 -> Security 保护 -> Adapter TX/目标原子 RX
        -> Security verify + 当前 ACL + Replay 候选
        -> 精确 Parent/Link/Session + 分片与完整摘要预检
        -> 预留 SACK -> Replay commit -> 无失败重组提交
目标 SACK -> Security 保护 -> Adapter TX/源 RX -> 认证反馈 -> TX 保持 Sending
Complete -> 当前权限/时限/输出/Terminal 槽预检 -> 复制业务原消息一次 -> Delivered
Terminal -> Security/Adapter -> 源端认证并精确提交 -> TX Done
```

Runtime/Coordinator 编排跨 Owner 调用；固定大小 Bridge 只保存接收绑定和分片暂存。
Common Header 仍 3 B，C1 字段、13 B 分片前缀、SACK/Terminal Body、AAD/Tag 不增大。
测试成功路径使用实际认证 O1/HMAC；入口允许 O2，但本段没有独立证明新管线上的所有
AEAD Provider/套件组合，更没有生产密码实现验收。

业务交付指“完整原消息复制到调用方 Buffer”，不是外部执行器已经执行、业务回调成功、
RPC Result、耐久 Operation 结果或重启后 at-most-once。Flow、多跳和 RPC 在此入口明确拒绝。

## 2. 入口、调用顺序与模块职责

| 入口 | 调用者/职责 | 成功后保留的状态 |
| --- | --- | --- |
| `TransferOwner::prepare_fragment` | Coordinator 调用；只读验证身份、几何、证据、时限和假设加入本片后的完整摘要 | 返回独占 `FragmentAdmission`；尚未写字节、bitmap、phase |
| `FragmentAdmission::sack/commit` | 预留预计 SACK 后，Coordinator 先提交 Replay，再提交线性借用 | 无回调/无分配复制本片，冻结认证证据；最后一片只进入 Complete |
| `TransferBridge::receive` | 对实际 raw packet 执行 Security/ACL、精确 Parent/Link 与反馈容量预检 | 不透明 `ReceivedTransfer`、SACK ReplyHandle、fresh/complete |
| `ControlReplies::seal/submit/retire_attempt` | Coordinator 保护一次回复，提交真实 Adapter API，并核对物理完成 | 冻结字节、计数器、Session/完整 LinkHandle；Token 明确退休后才释放 |
| `TransferBridge::retire_feedback` | SACK 确定本地发送且 Token 退休后显式调用 | 释放该临时通知槽；不删除 RX 字节和认证证据 |
| `TransferBridge::deliver` | 当前 Parent/Session/ACL/Link/时间复核；输出容量和 Terminal 槽先预检 | 一次原消息复制、Delivered、固定 Terminal 回复义务 |
| `TransferBridge::stop/discard/retire` | 停止新接纳；本地拒绝尚未领取的 Complete；凭据过期后退休逻辑 RX | 不隐式清除独立 Reply/Adapter 的物理义务 |

线性预检通过 Rust 独占借用锁住 RX 状态；不复制整个 Transfer Owner，不引入与最大
消息大小相等的临时 Owner 栈副本。丢弃未提交的预检对象不改变 RX。

最终摘要使用同一个 `UCN6-TRANSFER-MESSAGE-V1` 规范摘要，按“已有前半段 + 新片 +
已有后半段”流式校验。摘要不匹配时新片没有写入，旧片也不清除，原序号仍可重新接纳。

每个接收实例冻结完整 Parent、SessionHandle、LinkHandle、Service 和接纳时刻。
分片使用原 Setup 的截止；延迟接纳、重复、最后一片和业务交付均不延长 lifetime。
`now < 原接纳时刻`、截止等值、过期凭据、错代际及相同 generation 的不同 Link 均拒绝。

## 3. 有界反馈、重复与交付语义

### 3.1 SACK 不是最终交付回执

SACK 只确认通过认证且安全写入的分片。所有 bitmap 均收到后 Sender 仍是 Sending；
只有最终目标的完整 Terminal 身份和结果通过认证、匹配已冻结的 Setup 才能进入 Done。
RX Complete 也不写成功 Receipt，成功只在实际 copy/release 交付后写入。

### 3.2 临时 SACK 与保留 Terminal 区分

每个 Receiver 最多绑定一个待处理 SACK；尚有 SACK 义务时，新片背压返回 NoSpace，
不消费新 Replay、不覆盖旧义务。Caller 必须完成 `seal -> submit -> retire_attempt ->
retire_feedback`，再继续接纳。参考成功测试用三槽容纳 Setup、一个 SACK 和 Terminal，
并非为全部分片永久分配一槽回复。

确定本地发送完成的 SACK 可以显式释放；如果目标随后收到已认证的精确重复，按仍保留
的分片证据重新生成当前累计 SACK，使用新的 C0 transaction。这是新通知，不改旧密文、
不复用旧 nonce。尚未退休的通知则复用原 Reply，不能突破原重试间隔、次数或截止。

窗口按 `index/32*32` 绑定。同一个 Reply 的完整 SACK Body 必须与该重复片对应的当前
窗口一致；窗口不同返回 NoSpace，不把 window 32 的通知冒充 window 0 的确认。

Terminal 保留到原截止，精确重复只让原缓存按策略重发，同字节/transaction/Tag，不
复制第二次业务 Buffer。Receipt 另有独立保留期，不允许延长消息的业务执行时限。

### 3.3 交付、丢弃和退休是三件事

`deliver` 先检查当前授权和输出大小，再预留 Terminal，最后复制原业务字节。
输出不足、权限撤销、Reply 满、未完成、重复交付和过期都保持输出与 Complete 状态不变。

`discard` 只允许尚未领取的 Complete：清零数据、置 Aborted，保留负向 Receipt；不能
丢弃 AppHeld/Delivered，也不是远端取消证明。停止后仍可显式丢弃和做退休清理。

逻辑 RX 的 Receipt 过期后可以退休，但 Reply 的 Token 由独立 Owner 继续持有。
Driver 不取消或处于未知状态时不得清除；后续确定取消/完成并退休成功后才能回收。

## 4. 每小项自审与整改

| 小项 | 自审关注点与实际修正 | 回归 |
| --- | --- | --- |
| 预检原子性 | 最后一片在复制前计算假想完整摘要；线性 Guard 丢弃零写；Complete 不再提前产生成功 Receipt | Transport 新增预检 drop/错误最后摘要/正确提交回归 |
| raw 接纳 | 先 Security/ACL，再业务预检/预留 SACK，最后 Replay 与无失败提交；精确 Parent 与当前 Session 双复核 | 坏 Tag、错 Service、无回复槽、错几何、错误 digest 后同序号正确包仍成功 |
| 累计反馈 | 采用一个有界临时 SACK，而非每片持久缓存；禁止跨窗口复用 | 原始 SACK 认证反馈、index 0/32 与旧窗口重复对抗 |
| 业务交付 | 将 Complete 与 Delivered 分开，Terminal 资源必须先于输出写入；交付时重新执行 ACL | 短 Buffer、Terminal 满、撤销 Policy、重复交付均零写 |
| 终态重试 | 原缓存重发而非重新交付或重新分配 C0 counter | Terminal 丢失、认证重复触发同字节重发、源端仅此时 Done |
| 停止与释放 | 增加未领取 Complete 的显式 discard；不把 stop/逻辑退休当成物理取消证明 | stop、首次取消失败、Token 保留、第二次成功才退休 |
| 裁剪与测试真实性 | 修正资源示例的 feature-off 引用；容量测试保持满 Setup 缓存，显式装配第二反馈池，禁止倒退测试时钟来假造可用容量 | 13 裁剪组合、原 Setup 缓存保持 AwaitSeal、新反馈池受限容量 |

上述小项完成后又做整体正向和失败生命周期两轮回读：第一轮按 Foundation→认证→
预检→Replay→重组→SACK→交付→Terminal；第二轮按重复→撤权/超时→stop→取消失败→
退休→旧 Handle 拒绝。没有将本段自审扩大为“整个协议不存在 P0/P1”。

## 5. 正式回归与全量软件证据

新增 `rust/crates/ucn-runtime/tests/transfer_data.rs` 五组测试：

1. 实际 Foundation Parent、认证 raw Setup、源/目标 Adapter、乱序分片、累计 SACK、
   一次交付、Terminal 丢失后的同字节重试及凭据/Handle 退休。
2. 认证、ACL、容量、几何与最后完整摘要失败均不提交分片或消费 Replay；正确原序号可恢复。
3. 密码正确但同序号不同内容、相同代际的不同 Link、过期交付和过期重复都失败关闭。
4. stop 后不能交付；丢弃/逻辑退休仍保留未决物理反馈；首次取消失败不丢 Token。
5. 33 片消息的跨 SACK 窗口重复不得错配；旧窗口通知可在上一通知物理退休后重新生成。

第1组走完整 raw Setup；第5组为聚焦窗口边界使用 typed Setup，不能单独称为全链路
集成证据。Driver、持久化介质和密码装配均为 Host 测试实现，不冒充真实 Flash/开发板。
Transport 另新增1组预检回归；原6组控制分派回归继续通过。

命令：`python tools/v6/run_rust_software_matrix.py --cargo
C:\Users\y2389_4rq4ld9\.cargo\bin\cargo.exe`。

| 门禁 | 本轮结果 |
| --- | --- |
| Workspace Debug / Release / MSRV 1.85 | 各 183/183 PASS |
| 本段 runtime data 回归 / Transport 新回归 | 5/5 + 1/1，已经包含在 183 中 |
| 原 Transfer 控制分派回归 | 6/6，已经包含在 183 中 |
| Clippy `-D warnings` / rustfmt | PASS |
| Optional Feature/依赖门禁 | 20 packages / 13 combinations / 3 isolated optional Owners PASS |
| C/Rust/Python 独立 Codec Oracle | 24/24 PASS；运行当前 native C，不是 Rust FFI |
| Cortex-M no_std 软/硬浮点 | 两 target compile PASS |
| ESP32-S3 xtensa | compile-only PASS，不访问串口或开发板 |
| 文档与链接门禁 | 133 documents / 317 links PASS |

11 项矩阵全部 exit=0，370 个源码/合同依赖开始/结束一致，`source_stable=true`；
整体回读后再次复算370项 SHA256，差异0。原始副本：
[software-matrix-sw01-transfer-data.json](../软件证据/Rust/2026-10-14/software-matrix-sw01-transfer-data.json)，
SHA256=`49AE2A6DFB1C397002A1D832597B78A63186C135DD87BE14A3E84B5CD37752D5`。
保留上一段177项的原始证据，没有覆盖它；后续源码修改必须重新运行矩阵。

Host 资源例（固定泛型装配，不是整机 Profile 或 MCU 实测）：

| 类型 | 配置 | 字节数 |
| --- | --- | --- |
| TransferBridge | `<1,128>` / `<2,256>` / `<4,512>` | 496 / 976 / 1944 |
| ControlReplies | `<1,128>` / `<2,256>` / `<4,512>` | 512 / 1256 / 3520 |
| TransferOwner | 资源示例 Nano / Lite / Full | 2368 / 15272 / 58680 |
| ControlCoordinator | `<2,128>` | 584 |

将 Reply 内完整命令替换为最小验证原因，使三档缓存相比上一段减少72/144/288 B。
新 Bridge 只在启用 Transfer 并显式静态装配时占用资源；不扩大普通 Core 帧或默认单帧
Owner。未测 MCU 最坏调用栈、运行期栈水位、目标链接 Flash、吞吐或功耗。

本轮没有新跑 C 全量 CTest、TSan/Miri/ASan，不复用历史结果充当当前证据。
已跟踪差异执行 `git diff --check`；新建未跟踪源码/测试/报告另查尾随空白。

## 6. 剩余工作与下一步

- Parent 的线上发布/安装协议尚未接入；本段使用真实 Foundation 发布的已装配 Parent。
- 分片 Source 侧实际发送由参考 Coordinator 编排；完整自主重传/调度与统一动态 Runtime
  仍需后续工作，不把这次接收端 Bridge 说成全量发送 Runtime。
- Service RPC、Operation Query/Receipt/Cancel 的真实持久化编排，以及 Time 四报文统一
  分派尚未接通；下一软件小项优先 Service/Operation，继续逐项和整体自审。
- Flow、多跳返程、Group/Cluster 完整线上协议及 C/Rust 全运行时互操作仍按任务表推进。
- 真实 Flash/掉电/Witness、生产密码 Provider、ESP32/物理 Bearer/打点、性能和长稳均待
  用户重新授权实机阶段；本轮没有产生这些能力的硬件证据。

本段 SELF-REVIEW PASS 不解除外审、实机或生产 HOLD，不标 RUST-SW-01 DONE。
