# Cluster Target 与持久 Authority

## 1. 定位

Cluster 是可选的管理与规模化层，不是 Core 转发的必需条件。它把成员、Head/Backup、配置、
接管、合并、恢复与 Rekey 统一到一个 v6 Target FSM；旧 Current/Target shadow、v3/v4 Cluster
Wire、Mixed Version 和 legacy Record 均不属于当前实现。

## 2. Epoch 与 Config

Cluster Epoch 绑定 Cluster ID、Term、Head 和身份代际。Term 只在同 Cluster 内比较，跨 Cluster
不能用数字大小决定权威。Config 有稳定态和 Joint 态；变更必须经过 `C_old → C_joint → C_new`，
分别验证旧/新 voter quorum，不能用一次普通 Commit 绕过 Joint。

成员记录的 provisional、committed、voting eligibility 与能力分开。只有认证、已提交且属于
当前 Config 的 voter 进入 quorum。v3 legacy 节点不存在于 v6 当前树，也不会被授予 Backup。

## 3. 持久化与 Authority

创建 Epoch、投票、Config、Takeover、Handover、Recovery、Rekey 在发送 ACK/Advertise/
Commit 或开放 Authority 前必须 durable。Provider success 后仍要 reload/校验 Operation Journal；
PENDING 期间 Owner 的相关收发和状态推进被 Fence。

Cluster Record 还必须与独立 rollback witness 的单调 Record Generation 一致；提交采用
witness-first，之后 reload canonical Record 精确比较。启动遇到旧 Record 回放、witness 超前/
落后或撕裂写一律 Fault。ACK、Vote、Ready 与 Directory/Tunnel 的语义只能从已认证 canonical
Payload 解码，不能用调用方另传结构替换。

Authority 不是缓存一次后永久有效。每次 Head 副作用、发送和 Federation 发布前都以当前时刻
preflight：检查 Epoch、Config、quorum、lease、phase、Fence 和 Owner budget。安装新 Config
先撤旧授权，再计算候选 quorum；失败保持无 Authority。

成员与 Authority 还必须在使用点检查对应 Discovery/Capability 半开 Deadline；刚好到期即
撤销资格，不能等待后台周期清扫继续使用旧能力。

## 4. Backup 与 Takeover

Backup 必须拥有与 Head 一致的 committed Config、成员快照覆盖和必要能力。缺失 protected
voter 或 REMOVED 立即永久取消本次 assignment；只有显式 SUSPECT 可进入有限 grace。

Takeover VoteId 完整绑定 Cluster/Epoch、Backup Generation、candidate、Config snapshot 和
事务 ID。同一当前 Epoch 的冲突 vote 拒绝，历史 Epoch vote 可在新事务原子替换。证书验证
完成且新 Epoch durable 后才进入单向终态；迟到 vote、unreachable 或通用 Epoch Commit 不能
改写结果。

## 5. Handover、Recovery 与 Rekey

计划切换和跨 Cluster 合并使用双 Epoch 事务，角色、READY、STEPDOWN/COMMIT 的验证责任按
模式区分。撤权写入不可逆 Fence；同一事务对象不能通过 reset/begin 恢复旧 Authority。

Recovery 在无法证明旧权威安全时创建明确新域，不跨 Cluster 比较 Term。Rekey 原子写 successor
identity 和 tombstone；退休 ID 不得通过普通 cluster create 重用。Directory/Tunnel 都绑定
完整代际并受固定容量与超时约束。

## 6. 能力与限制

软件模型已覆盖单簇 Target FSM 和 1k/10k 分组模拟，但未完成真实 Flash 掉电、无线分区、
多簇长稳与目标 MCU 资源实测。Cluster Feature 打开不自动提供 Storage Provider、真实时钟、
密钥或产品选举策略；缺少这些依赖时必须保持无 Authority。
