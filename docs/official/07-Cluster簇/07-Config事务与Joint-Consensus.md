# Config 事务与 Joint Consensus

> 文档级别：`NORMATIVE`
> 实现状态：`PARTIAL（Current 与默认关闭实验层级见正文）`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：Cluster 公共头、生产源码、独立 Archive、CMake 与测试
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：部分 ESP32-S3 实测；v4/掉电/完整多 Bearer 未发布验收

## 状态路径

配置变更必须经过：

```text
C_old -> PREPARED(C_new, txid) -> C_joint(C_old, C_new) -> COMMITTED(C_new)
                                    |
                                  ABORT
```

Prepare、Joint、Commit、Abort 都绑定 transaction ID 和完整 Config 引用。不能仅凭相同 txid 接受不同的 staging Config。

## Commit 门禁

Commit 前必须同时满足：

- durable Prepare 与 live transaction 匹配；
- durable Joint 证明存在，重启后不能从 PREPARED 直接跳 Commit；
- `C_old`、`C_new` 双 quorum 均满足；
- 若策略要求 Backup，Backup ID、txid、C_new、staged 和 ACK source 精确匹配；
- Config/Rekey 不并发；
- serial/generation/config ID 合法前进。

所有门禁必须在 Provider I/O 前执行。Runtime apply 再做一次同样的精确核对，避免持久层和 RAM 状态错绑。

## 时间与重入

deadline 使用统一回绕安全时间代数；超过合法 duration 的配置直接拒绝。Provider 的 load/submit/poll 以及递归 owner init 都由 callback gate 防重入。

## 当前状态

M07 已完成受限实验软件范围的外审，但仍不是默认产品 v4 Config FSM。生产接线、真实掉电和跨版本互通继续受 M05 `AUDIT HOLD` 约束。

## 为什么不能直接把 C_old 换成 C_new

如果一次从 `{A,B,C}` 直接切到 `{A,D,E}`，网络分区可能让旧集合的 `{B,C}` 和新集合的 `{D,E}` 各自形成多数，产生两个 Authority。Joint 阶段要求旧、新集合同时多数，把两个配置的安全交集带过切换窗口。

## 四阶段的 durable/runtime 边界

1. **Prepare**：冻结 txid、C_old、C_new/staging，持久化后才发 Prepare ACK；
2. **Joint**：持久化 C_joint 证明并在 runtime 安装双集合；
3. **Commit**：确认双 quorum 和可选 Backup exact gate，再持久 C_new；
4. **Apply**：reload/journal 验证后原子更新 runtime Config/Authority；
5. **Abort**：仅匹配 txid+C_new 的活动事务可清理，不能用同 txid 清另一个 staging。

“双 ACK 后直接 Commit”绕过 Joint 是 P0，不是性能优化。

## Config 身份和单调性

Config 引用至少绑定 config ID、generation、voter set/digest。新 txid、generation、config ID 使用统一 checked-next，不允许复用/回退。相同 txid 不同 C_new 是冲突；相同完整状态才可幂等重放。

## Backup Gate

策略要求 Backup 时，Commit 前检查：Backup 已 stage 同一 txid+C_new、ACK 来自绑定 Backup ID、mirror/能力满足。检查在 Provider submit 前执行，runtime apply 再二次核对，旧 ACK gate 不能错绑新事务。

## 重启恢复

Record 中必须区分 PREPARED、JOINT、COMMITTED/ABORTED。重启若只有 PREPARED，不能猜“既然多数 ACK 可能发过就直接 Commit”；需要重新证明/恢复或 Abort。Durable Joint 才允许进入后续 Commit门禁。

## 超时与 Abort

Deadline 用回绕安全 helper。超时后停止产生新承诺，按当前 durable phase执行合法 Abort/恢复。Abort 不能删除已 committed C_new，也不能与 Rekey PREPARED 并发。

## 示例

`C_old={1,2,3}`，`C_new={1,3,4}`：Joint 需要 old 至少 2 票、新至少 2 票。节点 1+2 只满足 old，不可 Commit；1+4 只满足 new，也不可；1+3 同时属于两边，可分别满足两个 2/3 quorum。

## 验证清单

- [ ] 无 durable Joint 时零 Provider I/O Commit；
- [ ] C_old/C_new 分别 quorum；
- [ ] txid/C_new/config digest 全绑定；
- [ ] Backup 缺失/错 ID/旧 ACK 在持久化前拒绝；
- [ ] Config/Rekey 并发拒绝；
- [ ] Prepare/Joint/Commit/Abort 各阶段重启恢复；
- [ ] deadline wrap 和 callback reentry；
- [ ] 默认产品仍未接入实验 Config FSM。
