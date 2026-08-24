# CLV2-M13 13-06 Persistence Ordering 自审报告

## 结论

`CLV2-13-06` 为 **CODE COMPLETE / SELF-AUDIT PASS（受限实验软件范围）**。

## 实现

- 新增独立 `ucn_cluster_rekey_persist_owner_t`，只在 default-OFF `ucn_cluster_rekey_experimental` 中可用。
- 顺序固定为：ID history durable proof → `REKEY_PREPARE` submit → completion → exact load/reload → ACK collection → old Stable quorum → Authority revoke/Fence → `REKEY_COMMIT` submit → exact reload → Commit 可输出。
- `load/submit/poll` 进入外部 Provider 前建立回调门和 `io_active`；递归 Init/Prepare/Commit/Abort/Poll 全部拒绝。
- 同步、异步 Pending、多次 Pending、Provider failure、reload 不一致均不释放虚假 durable promise；持久化失败永久 Fence。
- ACK deadline 到期会在 Provider I/O 前终止，不能在超时后提交 PREPARE。

## 定向反例

- PREPARE/COMMIT 在 exact reload 前不能生成 Wire 输出。
- Provider callback 重入无二次 submit、无 transaction 改写。
- submit failure 使 transaction、M08 Authority 和 Cluster persistence 同时 fail-closed。
- 丢失 ACK 后只允许持久化 exact Abort；旧 txid 不能复用，新 txid 可重新发起。

## 边界

本项验证 Host Provider 合同和状态机顺序，不等于真实 Flash 双槽、断电切断或擦写寿命验证。生产 v4 RX/TX/FSM 仍未接入。
