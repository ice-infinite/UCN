# 不变量、Safety/Liveness 与诊断

> 文档级别：`NORMATIVE`
> 实现状态：`PARTIAL（Current 与默认关闭实验层级见正文）`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：Cluster 公共头、生产源码、独立 Archive、CMake 与测试
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：部分 ESP32-S3 实测；v4/掉电/完整多 Bearer 未发布验收

## Invariant Engine

运行期 invariant 检查把角色、Phase、Epoch、Config、VoterSet、Authority、Fence、Persistence、Backup 和事务终态组合验证。失败时优先撤权或 fail-closed，不能为了继续运行而自动修补权威状态。

它解决的不是“参数是否合法”这一类局部问题，而是判断多个子系统组合后是否仍然代表同一个、可证明的权威事实。例如 `role=HEAD` 本身不能证明节点具有发送权；只有 Epoch、Committed Config、Lease、Quorum、Persistence 与 Fence 同时满足，Authority 才能成立。

### 检查层级

| 层级 | 典型输入 | 必须回答的问题 | 失败动作 |
| --- | --- | --- | --- |
| 结构合法性 | ID、Term、Config、Role、计数 | 字段是否处于协议合法域 | 拒绝输入，输出不写回 |
| 身份一致性 | Active Epoch、Max Epoch、Head、Cluster ID | 这些字段是否描述同一权威域 | 撤权或进入 Fence |
| 配置一致性 | Stable/Joint Config、VoterSet | quorum 的计算集合是否明确且 canonical | 禁止 ACK/Commit/Authority |
| 时间一致性 | Lease、Grace、事务 deadline | 当前时间下证明是否仍有效 | 先刷新/撤权，再处理业务 |
| 持久化一致性 | durable Record、operation journal、runtime continuation | RAM 声称是否已有 durable 证据 | fail-closed，不发送 promise |
| 终态一致性 | committed/aborted/retired/fenced | 不可逆状态是否被重新打开 | 原子拒绝且保持对象不变 |

检查必须放在产生副作用之前：RX 不得先写成员表再检查 Authority；TX 不得先消耗 token 再发现 Lease 过期；Provider 回调不得在 `io_active` 门建立前执行。诊断检查也遵守相同顺序，但只返回观察结果，不负责修复状态。

### 何时执行

- 初始化与持久化 reload 后：验证 Record、Active/Max Epoch、Vote、Config、Tombstone 的组合；
- 每次 Owner step 前：刷新时间相关 Lease、Grace、deadline 与 Authority；
- 每个权威 RX/TX 入口前：执行当前时刻的 Authority preflight；
- Config、Takeover、Handover、Recovery、Rekey 状态转换前后：分别校验旧状态允许转换、结果状态满足不变量；
- Debug/测试构建中：允许调用完整 invariant 检查生成定位信息，但不得据此放宽生产门禁。

## Safety 示例

- 不同 Cluster 不比较 Term；
- 同一 Epoch 不做冲突 Vote；
- 未 durable 的状态不发送 promise；
- 无 quorum 不保留 Authority；
- fenced/retired identity 不恢复旧权威；
- Joint Commit 不绕过双 quorum；
- Backup coverage 缺失不进入 takeover；
- v3 控制面不建立 v4 Backup/Takeover Authority；
- Wire 长度、版本、角色和保留位严格拒绝；
- 异步 Provider 重入不产生第二次状态写入。

### Safety 与业务可用性的取舍

UCN 选择“宁可暂时没有 Head，也不能同时存在两个合法 Head”。因此下列现象可能是正确行为，而不是缺陷：

- 存储无法确认 Vote durable 时，节点不发 ACK；
- Lease 已过期但网络尚未完成重新选举时，节点停止权威发送；
- Joint Config 只有旧集合或新集合达到多数时，仍不能 Commit；
- Backup Snapshot 缠绕、缺成员或 Config 不匹配时，Backup 永久失去本轮 takeover 资格；
- Wire v4 能解析但生产 FSM 未授权时，帧仍不进入 Authority 路径。

判断实现是否安全，不能只看“最后是否恢复通信”，还要检查故障窗口内是否曾经错误发出 ACK、Advertise、Commit、Head 消息，或写入了与 durable Record 不一致的 Directory/成员状态。

## Liveness 边界

Safety 优先可能导致节点保持 DETACHED、FENCED 或 AUDIT HOLD 状态。软件模型只能证明在假定时钟、消息和存储合同下可推进；真实丢包、分区、Flash 故障和长期抖动必须通过仿真与实机验证。

Liveness 的成立依赖明确前提：至少存在可通信的 quorum、单调时间在规定半范围内工作、Provider 最终返回成功或明确失败、控制面没有长期被业务数据饿死、候选节点满足 capability/coverage 条件。任何一个前提长期不成立，协议允许停留在安全状态，而不承诺一定选出 Head。

### 典型停滞与恢复路径

| 现象 | 常见原因 | 正确恢复路径 |
| --- | --- | --- |
| 长期 DETACHED | 看不到合法 Advertise，或没有选举 quorum | 恢复邻接、等待 backoff，重新开始新簇选举 |
| FENCED | Lease/quorum 丢失、持久化或不变量失败 | 先恢复证据，再走明确的 Observe/Recovery，不可直接恢复旧 Authority |
| Config PREPARED | Commit 条件不足或重启 | reload transaction，按 txid 恢复、Abort 或完成，不能跳过 Joint |
| Backup ineligible | coverage 缺失/REMOVED | 由 Head 重新 assignment 并完成新 Snapshot，旧 assignment 不复活 |
| Recovery backoff | 多节点竞争或稳定权威仍存在 | 稳定 Authority 优先；退避到期后才重新创建/加入 |

### Safety/Liveness 测试应分开记录

Safety 测试关注“绝不能发生什么”，通常用负向帧、掉电点、重入、乱序和时间边界证明零副作用；Liveness 测试关注“满足前提后最终是否前进”，需要记录收敛时间、重试次数、控制流量和最终角色。一个测试全绿不能同时替代这两类证据。

## 诊断

诊断至少输出 Role、Phase、Epoch、Config/Joint、quorum、Authority、Fence、persistence pending/fault、Backup coverage、Recovery/Rekey 状态和关键计数。诊断 API 只读，不得隐式续租或改变选举。

### 推荐诊断快照

一次快照至少应包含：

- 本节点：Node ID、Role、Phase、当前时间、最近 step 时间；
- 权威域：Active Epoch、Max Seen Epoch、Head ID、Term、Fence 原因；
- 配置域：Stable/Joint、`C_old/C_new`、generation、canonical voter 数、旧/新 quorum 结果；
- Lease：本地 Authority Lease、各 voter freshness、Grace deadline；
- 持久化：mode、schema、generation、pending operation/ID、最近错误、faulted 状态；
- Backup：assigned ID/generation、Snapshot sequence、coverage、takeover eligibility；
- 事务：Config/Takeover/Handover/Recovery/Rekey phase、txid、deadline、最后拒绝原因；
- 计数：非法帧、版本不兼容、quorum 丢失、撤权、Provider 失败、重试、超时。

### 采集规则

诊断必须从 Owner 一致性视图读取，或由 Owner 复制到只读 snapshot。ISR、其他任务和远程查询不得直接遍历正在变化的成员表。计数器使用饱和累加，避免回绕后误判；敏感字段（密钥、完整密文、认证材料）不得进入普通诊断。

### 故障定位顺序

1. 先看 Wire/版本/角色是否被拒绝；
2. 再看 Epoch 与 Config 是否属于同一 domain；
3. 检查 Lease、quorum 与 Fence 为什么未授权；
4. 检查 persistence pending/fault 和 operation journal；
5. 最后检查 Backup、事务 deadline 与控制预算。

该顺序可以避免把“根本没通过准入”的问题误判为选举或链路性能问题。
