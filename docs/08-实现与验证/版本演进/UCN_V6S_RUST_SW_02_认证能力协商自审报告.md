# RUST-SW-02 第一段：认证能力协商实施与自审

## 1. 候选与范围

日期：2026-10-14。分支 `v6-simplified-rust`，基线 `03eb7eb` 加完整工作树；不是只看
已跟踪差异。未提交、推送、操作实机或枚举 COM/USB。

本轮按用户授权补共同 Control 正文和 C/Rust 实现，保持 MCU-first、固定容量、Owner
独立、Coordinator 编排、普通数据头不增大。Obsidian 原则只读核对，实际代码为最终依据。

当前结论：六个小项、两轮整体回读和最终11项软件矩阵自审通过；不宣称顶层完成或外审 GO。
RUST-SW-02 仍 IN PROGRESS。实现只覆盖已经认证准入的单 Peer/直连 H0，不代替 JOIN。

权威共同合同：[认证协商、字段、费用、生命周期](../../10-理论与规划/建议方案/UCN_v6_C_Rust共用合同收口/06-Capability认证协商与精确事务共同合同.md)。

## 2. 小项与逐项自审

| 小项 | 实现内容 | 自审证据与边界 |
| --- | --- | --- |
| CAP-01 | Record68/Summary24/Query20/Select59/Ack75、0107 QUERY | C/Rust/Python 三种独立实现逐字节 48 组通过；BE/精确长度/保留位和零写回；不是 Rust 调 C FFI |
| CAP-02 | 源视角交集和完整 SELECT transcript | local=SELECT Source；目标交换双方记录身份后比较；canonical152 绑定双方身份、Session/Key/Policy、Service、原 SELECT C0 transaction；12 类因果字段变异必须改变摘要 |
| CAP-03 | 实际认证命令路由与固定资源 | Root 自己领取原子 RX，实际 Security/ACL/Replay、Role/Q2/Hop1 后分派；错 Tag/无 Opcode ACL/错 Role/早到或错摘要 ACK 拒绝 |
| CAP-04 | 有限握手、缓存重发和退休 | 丢 Query、两端 Record、ACK；初次 NOT_SUBMITTED；同 tx/op 的 frame 逐字节相同；发送/退休预算1可收敛，独立游标不饿死 |
| CAP-05 | 同 Session 高水位/撤销/到期 | C 到期只通知一次不清槽；REVOKE 保留历史；同代际不续租且撤销后不复活；跨代只接受 checked-next；容量满不隐式驱逐 |
| CAP-06 | stop/Expiry/UNKNOWN 和裁剪 | 固定原 deadline，到达即 Fence；确定在途取消失败保留 Token后重试；UNKNOWN/InDoubt 不伪造退休；capability-only/OFF 编译与依赖门禁纳入最终矩阵 |

## 3. 真实链路，不是伪造 opened DTO

[CapabilityRuntime](../../../rust/crates/ucn-runtime/src/capability_control.rs) 的输入是当前实际
Security/Adapter，而非调用方声称 authenticated 的结构。测试从真实 Foundation 内存
Provider 的写入、回读、Proof 退休与 Session 发布开始；两端独立 Owner 和计数器通过
Fake Driver 输出完整认证帧，再经目标实际 atomic RX/Security open/ACL/Replay 处理。

流程 HELLO→QUERY→ADVERTISE A→ADVERTISE B→SELECT→ACK。所有阶段使用 C0，Q2，
BestEffort，HopLimit=1，O1/O2；普通 One-Way Data 0 字节新增。A1/H0 的六帧为
69/65/113/113/104/120 B，无丢包共 584 B。只支持能承载完整 ACK 的当前 Link。

测试 HMAC Provider 不代表生产密码实现；O2 本模块结构路径存在，不等于本轮实测 AES-GCM
或 ChaCha20-Poly1305 握手。Root 的已确认交集仅提供 Plan 输入，不换 Key、不授 Authority。

C 已实现同合同 Codec、Catalog 和实际认证 handler，包括 QUERY 的 Role/Body 与目标
SELECT 的源视角重建；没有声称 C 已实现这个自动握手状态机。三方 Oracle 不证明整套
C↔Rust Runtime 网络互通。

## 4. 两轮整体回读与整改

第一轮围绕 Wire、Owner 和 Snapshot：

- 分离 QUERY 与 REVOKE；双方采用唯一 Capability BestEffort policy，原六类 Reliable
  输入不保留自动兼容。Registry 97→98，低频新 Opcode 不增大普通头。
- SELECT 不再比较反向本地视角；精确 ACK 绑定原请求事务，不能由字段相同但因果不同的
  另一帧替代。同代 Record 增加字段精确相等，digest 仅预筛选。
- C 清扫/REVOKE 原先会丢历史，已改为保留同 Session 高水位。显式父 Session
  invalidation 是可信生命周期操作，不由网络 REVOKE 冒充。

第二轮围绕预算、物理生命周期和发布面：

- 拆开 drive/retire 游标，避免同一预算1序列跳过请求 Token；Fence 同时关闭新 Command/
  Reply 接纳，停止保留的 Pending 也必须排空才能 quiescent。
- 退休先查看实际 Adapter 状态，不把任何 ERR_STATE 都当“尚未完成”；确定在途与
  InDoubt 分开，后者保持隔离。到期 reply 走显式 cancel/retire，不能在物理持有时删缓存。
- 永久错误命令不留在唯一 Pending 槽，仅资源 NoSpace 保留；回复资源先预留。
- 全量 Clippy 最初发现示例 wildcard import，Feature OFF 又发现示例缺 cfg，已逐项修正。
  Windows GBK 打印失败会遮蔽原始诊断，矩阵工具现输出 UTF-8。
- E盘构建缓存耗尽不是协议回归。可重建增量缓存转存到
  `D:\Temp\UCN-Cargo-Cache-20261014-Capability\incremental`，没有删除源码或原始证据；
  最终矩阵显式 `CARGO_INCREMENTAL=0`，环境写入原始结果，不能把失败的旧矩阵算通过。

## 5. 验证结果

C 当前已成功重建：Windows GCC Full 全量 108/108；Lite/Nano 本轮 Capability/
Composition/Control Wire 定向各3/3。不是 Fresh 全工具链矩阵，也没有本轮 C MCU 栈证明。

最终 Rust 验证（均为本轮最终代码重跑，不沿用早期中断结果）：

| 门禁 | 结果 |
| --- | --- |
| Workspace Debug / Release / MSRV 1.85 | 各264/264 |
| 全量 Clippy / fmt | PASS，Clippy `-D warnings` |
| Cortex soft/hard `no_std` | 两目标 check PASS |
| ESP32-S3 | compile-only PASS，未运行/烧录 |
| Feature/OFF/独立依赖组合 | 23组合 PASS，capability-only不引入Service/Realtime/Group/Cluster或durable业务模块 |
| C/Rust/Python 原生独立 Codec | 48组 PASS |
| 原始矩阵/来源快照 | 11配置全部exit=0，396项依赖稳定，独立SHA复算差异0 |

`CapabilityRuntime<'static,128>` Host sizeof=3528 B、alignment=8；这不是整个 Node 的RAM，
不含外置Security/Adapter/Ingress/Driver/Scratch/计数器，更不是MCU任务栈或Flash实测值。
关闭增量缓存只影响构建资源，不降低编译警告或测试要求。

[不可变原始矩阵](../软件证据/Rust/2026-10-14/software-matrix-sw02-capability-final.json)
SHA256：`D002FC0C392FA30F4E9F4B1A807FF443445CD807CA93825D85E12330AED59CFA`。
已另用PowerShell逐项独立复算396项SHA，0差异；历史软件证据未覆盖或改写。

台账收口后的文档门禁142篇/353链接、API索引318、diff-check及本轮5个新增手写文件尾随
空白检查通过。真实Git暂存区为空。没有把未跟踪文件排除出来源快照。

## 6. 仍开放的工作

Dynamic Admission 的认证握手/持久化发布、动态 Route/Flow 原 traffic origin 与 causal
transaction 的逐跳保留、多 Peer、自动再次协商、C 自动 Root 仍是软件工作。
Parent/Time 全 C Root、Group/Cluster 控制与全模块共同调度/运行时互通也不能算完成。

本轮不验证真实 Flash/Witness、断电、生产密码 Provider、ESP32 运行、物理 Bearer、精度、
性能功耗、24小时长稳、目标最坏栈或本轮 TSan/Miri。保持外审、实机和生产 HOLD。
