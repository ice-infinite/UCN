# CLV2-M13 13-07 Tombstone / Replay 自审报告

## 结论

`CLV2-13-07` 为 **CODE COMPLETE / SELF-AUDIT PASS（受限实验软件范围）**。

## 实现

- `REKEY_COMMIT` 的 successor Active/Max Epoch、successor Config、committed Rekey、boot incarnation 与旧 Epoch Tombstone 位于同一 Record v4 snapshot。
- Tombstone 固定绑定 `{retired_epoch, replacement_cluster_id, rekey_transaction_id}`。
- `ucn_cluster_rekey_tombstone_admit_frame()` 对退休 Cluster ID 域的所有结构合法 Type 1..33 均返回 `UCN_ERR_REPLAY`；不会比较不同 Cluster 的 Term。
- Record encode/decode 后 Tombstone 语义保持不变；旧 Cluster 即使携带更大的数值 Term 也不能复活。

## 定向反例

- Commit 前无 Tombstone；Commit reload 后旧 Cluster 帧立即拒绝。
- 重启 decode 后仍拒绝旧 Cluster 帧。
- retired Cluster + 大 Term 仍按身份域拒绝，不走跨 Cluster Term 比较。

## 边界

当前 Record 仅保存一个 committed Rekey/Tombstone，第二次 Rekey 在没有 lineage/history 扩展前保守 fail-closed。多代退休集合与真实 Flash 掉电验证留给 M14 产品化门禁。
