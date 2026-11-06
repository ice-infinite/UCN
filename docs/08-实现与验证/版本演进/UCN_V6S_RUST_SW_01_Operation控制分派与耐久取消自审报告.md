# RUST-SW-01 第三段：Operation 控制分派与耐久取消

> 日期：2026-10-14；分支 `v6-simplified-rust`，HEAD `03eb7eb` 加完整工作树。
> 范围：Rust Host 软件、共同摘要合同、native C/Rust/Python 探针与目标编译。
> 状态：本段逐项及整体自审 PASS；RUST-SW-01 仍 IN PROGRESS，不是外审或生产 GO。
> 无串口访问、烧录、复位、实机操作或提交推送；保留原有 C/Rust 工作树。

## 1. 接续目标与实际完成范围

[第二段](UCN_V6S_RUST_SW_01_认证分片重组与业务交付自审报告.md)接通受保护 C1/H0 分片、
固定重组和一次业务 Buffer 交付。本段接通 **C0/H0 直达 Operation Query、Cancel 和
Receipt 观察**，不把认证入队、传输完成或取消请求当作执行器结果。

原业务 Operation Journal 和 Security Session 必须已经由真实 Foundation reload 发布。
Runtime 按完整客户端/服务端 Principal、地址、Binding、Realm、Session/Key/Policy、
Security fingerprint、Operation、Service、业务 Opcode 和 Request digest 匹配实例。
只检查短 Operation ID、Service 或摘要不够；pending 还冻结完整 DurableContract，
相同 OperationKey 不能替换成另一个 Journal Domain/Owner/Schema。

本段使用现有非零业务 Opcode 的控制合同；typed ServiceKey 的 Data opcode=0 不会被
擅自转成另一种线上身份。Flow、多跳、动态 Session、Group 和 Cluster 不借此自动接线。

## 2. 成功路径与取消状态机

```text
实际 Security verify + Opcode ACL + Replay -> 固定 ControlCommand
                        |
                 OperationCoordinator
                        |
               当前身份/ACL/完整 Journal key 复核
                        |
          +-------------+----------------------------------+
          |                                                |
  Query / 已确认终态的 Cancel                    Prepared / Executing 的 Cancel
          |                                                |
  读取已确认 terminal phase/result                 预留 AwaitCommit 回复 + continuation
  预留固定 Receipt Reply                           冻结取消 Record Requirement
  退休命令                                         Foundation submit -> 有界 step/poll
          |                                        双槽/Marker/Witness/reload Proof
          |                                        确认 Journal -> 退休 Proof
          |                                        再核对当前 ACL/Session/原截止
          |                                        解锁 AwaitSeal -> 退休命令
          +---------------------+--------------------------+
                                |
                      Security seal 一次 -> Adapter TX
                      完成事实 -> 精确物理 Token 退休
                                |
                      Client 认证/完整身份匹配 -> Watch
```

| 当前确认状态 | 本地执行器交接事实 | Cancel 的耐久目标 | 可以承诺什么 |
| --- | --- | --- | --- |
| Prepared | 尚未交接 | AbortedNoEffect，status=`UCN_ERR_CANCELLED` | reload 之前不承诺取消成功；之后可以证明该实例未执行 |
| Executing | 本次启动尚未 handoff | AbortedNoEffect | 已落盘 Executing 不等于执行器已观察；仍需取消 Record reload |
| Executing | 已 handoff，或从持久化恢复 | InDoubt，status=`UCN_ERR_IN_DOUBT` | 不能宣布无副作用、撤销执行或重新执行 |
| CommittedResult / AbortedNoEffect / InDoubt / Tombstoned | 已确认观察 | 不改 Journal，返回现有 Receipt | 仅报告该时点的已确认状态，不倒退或伪造新终态 |
| 任意 pending / fault | 未完成证明 | 保留命令或失败收口 | 不发成功回执，不强行重试外部副作用 |

Query 不能把 Prepared/Executing 包装成 terminal Receipt；没有冻结的 pending-status Wire
就返回 State 并保留命令，由 Coordinator 明确拒绝或稍后处理，不创造新的线上格式。

## 3. 接口职责与调用顺序

| 入口 | 职责与调用顺序 | 错误后的义务 |
| --- | --- | --- |
| `OperationOwner::key/contract/receipt` | 从已安装 Owner 获取完整身份；仅确认的 terminal 状态生成 Receipt | pending/nonterminal/fault 不发布观察 |
| `OperationCoordinator::dispatch` | 当前 Security/ACL → 全键匹配 → 原截止 → 固定 reply/continuation → 准备 Record | 满表或错域不改 Journal；失败不退休命令；临时预留的回复失败时撤回，代际不回退 |
| `submit` | 路由 `operation.requirement()` → Foundation 接纳 → 绑定精确 Handle | 接纳的 Handle 先保存；错误不能丢 I/O obligation |
| Foundation `step` | caller 给予有界预算；同步或 Pending 的 Driver/Provider 事实逐步推进 | 未形成 Proof 不等于成功 |
| `finish` | 精确 Proof → Owner confirm → Proof retire → 当前授权/时限 → Receipt 精确比较 → 解锁回复 → ack | 回读后即使超时或撤权，耐久状态仍成立，禁止回滚；阻塞回复和命令保留 |
| `reject_confirmed` | 显式拒绝已确认但不可发送的响应 | 未确认或仍有 Proof Holder 时禁止丢弃；已确认 Journal 不恢复旧状态 |
| `retire_failed` | 仅接受精确 Failed 请求；先 fence Journal，再退休失败 Handle、阻塞 reply 和命令 | 旧 Journal 不恢复执行权限，后续需要产品恢复流程 |
| `pending_reply/view` | 只读查看 Holder；Handle 不等于可发送权限 | AwaitCommit 不能 seal、submit、通过重复逃逸或在超时后直接回收 |
| `dispatch_operation_receipt` | Client 当前最终 Server/全键/方向/Watch 截止核对 → 保留观察 → ack | 错身份、冲突或过期不改 Watch，也不退休命令 |
| `OperationWatch::verify_result` | 对单独取得的 status/正文复算结果摘要 | 48 B Receipt 本身没有业务正文，不凭它构造 RPC Result |

取消的 deadline 为 `ControlCommand.accepted_at + timeout_us`，不是分派、提交或重复请求
时重新起算。continuation 保存已观察时间高水位；submit/finish 不接受倒退时间。
已接纳的取消可以在 stop 后做安全 Journal/Proof 退休，但 stop 不清空所有权或解锁执行。

跨模块调用均在 Runtime/Coordinator。Operation Owner 不调用 Security、Adapter、Driver
或 Persistence Owner；只暴露不可变 Requirement 和消费精确 Proof。没有整个 Owner 的
事务栈副本、无堆分配，所有 continuation/reply 数量由静态装配决定。

## 4. 共同结果摘要规则

原合同给出了 Receipt 的 `result_digest` 字段，却没有唯一的计算式。本段把其规范输入
登记在 `protocol/contracts/v6-shared-v1.json`，没有增加任何 Wire 字节。

```text
domain = ASCII("UCN6-OPERATION-RESULT-V1")，不含 NUL
body = OperationWireKey[28]
     || terminal_phase:u8
     || application_status:i32 big-endian
     || payload_length:u16 big-endian
     || exact_payload
result_digest = SHA256(domain || u32be(body_length) || body)[0..16]
```

Body 固定部分35 B；phase 只允许3/4/5/6，只有 phase=3 可以带正文。
未知状态、非法键、非 Committed phase 带正文、超过 u16 长度均拒绝。
该摘要不是 Foundation Record digest，也不混入 RAM Handle、本地 Link 或本地分配代际。
Receipt 的完整认证上下文仍由 C0 AAD/Session 与运行时全键检查约束。

native C、Rust、Python 分别组装相同键、phase=3、status=-12、payload=`ok`，得到相同摘要。
这证明共同算法与字节合同；C 探针调用现有 native digest utility，**不代表 C 生产
Operation dispatcher 已经消费了这套 Receipt，也不等于 C/Rust 全运行时互操作**。

## 5. 逐小项自审与实际整改

| 小项 | 主要自审和整改 | 正式门禁 |
| --- | --- | --- |
| 摘要与 Owner 观察 | 补唯一结果摘要；仅真实 terminal reload 后 `receipt` 可用 | 全字段变异、非法 phase/正文、pending/nonterminal/terminal 回读；25组三方 Oracle |
| 运行时身份绑定 | 分派及响应时检查完整双端身份；取消继续冻结 DurableContract，而非只看相同逻辑 OperationKey | 错 request digest/opcode/Binding、另一个 Journal Domain、权限 facts 变化及早于接纳的提交 |
| 分派方向 | 整体回读发现terminal快捷分支误把入站Receipt当作Query；新增只允许Query/Cancel的前置门禁 | 新对抗先复现错误的`Ok(Reply)`，整改后`Unsupported`，队列和Journal保留 |
| 取消耐久门禁 | 新增 AwaitCommit，回复先预留但不可保护/发送；Proof 确认和退休后才解锁 | pending seal/submit/超时回收均拒绝，错误提前 ack 也不能经缓存绕过 |
| 状态与崩溃 | 区分本次尚未 handoff 与恢复的 Executing；未知外部副作用只进入 InDoubt | 实际 handoff 和 OPS1 恢复两分支，均不再签发执行权限 |
| 超时/失败收口 | 已 durable 的取消不能被超时撤销；显式 reject；回读损坏先围栏再逐层退休 | 原截止等值、失效 policy facts、异步回读、raw slot 损坏、Witness 不推进和失败退休 |
| 回复与 Client 观察 | 原密文重试；Watch 只是认证状态观察，不擅自完成 RPC/Operation | 丢失后同字节/counter 重发、错误结果摘要、冲突观察、Watch 等值过期 |
| 裁剪与资源 | `operation-control` 单独启用；普通 `service-control` 不引入 Operation/durable 代码 | 14裁剪组合；Realtime network 的 Foundation 依赖与 Operation 依赖分别核对 |
| 回复字节容量 | Journal变更前按地址宽度预检C0前缀+48B正文+16B Origin Tag | A1需要93B，A3需要97B；不足不能先提交Journal，单位边界回归通过 |

整体自审第一轮按“认证→当前键→容量→Record→reload→回复→观察”正向回读；第二轮
按“重复→换域/撤权→超时/stop→回读失败→围栏→精确退休”逆向回读。
本段不声称整个协议 P0/P1 为0，也不将 typed RPC/Executor fixture 当成原业务 Request
已经通过统一 raw runtime 接通。

## 6. 软件验证证据

完整命令：`python tools/v6/run_rust_software_matrix.py --cargo
C:\Users\y2389_4rq4ld9\.cargo\bin\cargo.exe`。

| 门禁 | 本轮状态 |
| --- | --- |
| Operation Runtime 新回归 | 7/7 PASS |
| Service 新增摘要/terminal reload 回归 | 2/2 PASS，分别位于 Control 和 Operation 测试 |
| Workspace Debug / Release / MSRV 1.85 | 各193/193 PASS |
| Clippy `-D warnings` / rustfmt | PASS |
| Optional Feature/依赖 | 20 packages / 14 combinations / 3 isolated optional Owners PASS |
| native C/Rust/Python Oracle | 25/25 PASS |
| Cortex-M 两 target / ESP32-S3 | 三target compile-only PASS，未操作目标板 |
| 回执容量单位边界 | 1/1 PASS；已包含在193中 |
| Host资源 / 源码哈希 | 下表和原始矩阵；372项最终哈希差异0 |
| 文档与链接 | 134 documents / 321 links PASS |

最终11项矩阵全部exit=0，372个源码/合同依赖开始/结束一致，`source_stable=true`，
整体回读后逐项SHA256复算差异0。新增7组runtime、2组Service和1组容量边界已包含在193，
不重复累计。原始副本：
[software-matrix-sw01-operation-control.json](../软件证据/Rust/2026-10-14/software-matrix-sw01-operation-control.json)，
SHA256=`951F3FC629DAF338DA314BED4B7940146F46C8A7FEB7BEA03120203AEEB1E795`。
保留前两段的独立原始证据，不覆盖它们。方向和容量整改前的192项PASS不代表最终193项快照。

Host `OperationCoordinator<1>`为504 B、`OperationWatch`为192 B；原Reply缓存示例
`<1,128>/<2,256>/<4,512>`仍为512/1256/3520 B，没有额外加入整份Journal或结果正文。
这些是当前Host布局，不是整机Profile RAM、目标ELF Flash或最坏栈；新增状态只在
`operation-control`及显式静态装配时存在，普通Core不依赖它。
文档/链接门禁和操作记录已同步；差异检查及新增文件尾随空白检查通过。
本轮没有运行实机、Flash掉电或生产密码验收，没有新跑 C 全量 CTest、TSan/Miri/ASan。
修改 native C 探针和共用 JSON/Oracle，没有新增修改 C 生产协议实现。

## 7. 仍未完成与下一软件项

- Service RPC 的原始 Request/Result/Error 接纳、业务交接、C1 Control 角色和可靠发送
  仍需统一 runtime 接线；本段 Journal 的 PREPARED/EXECUTING 建立和执行器结果由既有
  typed API 与实际 Foundation 测试建立，不冒充原 Request 已 raw 接通。
- Query 返回的是认证时点观察；InDoubt 日后可对账升级，旧观察不能声称最新状态，也不
  允许 Client 自动重执行。新的 Query 应创建新的 Watch，不能覆盖已保留的冲突观察。
- `OperationWatch` 不自动设置 ServiceOwner 为 Complete，不接收业务结果正文，不承担
  durable result ACK/GC；相应线上合同和执行器对账仍归后续软件小项。
- 准确结果需由产品 Executor 查询/对账保障；Flash故障围栏后的恢复和真实原子性需要
  产品层与实机证明，本段失败退休不会自动解除围栏或恢复权限。
- Parent线上发布、发送端自主调度、Flow/动态/多跳、Time、Group/Cluster 和全运行时
  C/Rust互操作仍按任务表推进。下一项继续 Service RPC，不进入实机阶段。

RUST-SW-01 仍 IN PROGRESS，外审、实机和生产 HOLD；普通 Header/Body/Tag 没有变大。
