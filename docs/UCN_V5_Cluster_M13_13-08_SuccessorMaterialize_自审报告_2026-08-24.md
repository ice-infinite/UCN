# CLV2-M13 13-08 Successor Materialize 自审报告

## 结论

`CLV2-13-08` 为 **CODE COMPLETE / SELF-AUDIT PASS（受限实验软件范围）**。

## 实现

- 只有 transaction 已到 `EPOCH_DURABLE/COMMITTED`、旧 Authority 已撤销、且 durable successor/Tombstone exact match 时，才生成完整 `ucn_cluster_rekey_successor_state_t`。
- successor 固定使用新 Cluster ID、Term 1、Stable Config ID/generation 1；旧 Stable voter set 作为新 Stable voter set。
- Backup 在 PREPARE 时冻结；若存在则 Backup generation、membership sequence、snapshot generation、snapshot ID 都从 1 开始。
- 成员表一次性由冻结 voter profile 生成，全部为 committed/voting/v4；不会从当前 RAM Backup 或后到 profile 拼接部分状态。

## 定向反例

- durable successor 不匹配、Authority 未撤销、transaction 未到 durable 终态时 output 完整不写。
- PREPARE 后当前 Cluster 改换 Backup，不影响冻结的 successor Backup。
- 输出的 Epoch、Config、VoterSet、MemberTable 与 serial 初值同时通过完整合法性校验。

## 边界

本项输出不可分割的候选值对象，不直接改写生产 `ucn_cluster_t`，也不授予 successor Authority；真实生产安装属于 M14 集成/实机范围。
