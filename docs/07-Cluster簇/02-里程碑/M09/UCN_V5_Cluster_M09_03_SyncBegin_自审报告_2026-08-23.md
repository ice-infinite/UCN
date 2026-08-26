# UCN V5 Cluster M09-03 strict SYNC_BEGIN 自审报告（2026-08-23）

## 范围

本项引入 wire-agnostic 的 `ucn_cluster_backup_sync_owner_t`。它不是生产 RX handler：不接受原始帧、不发送 ACK/READY、不授予 Authority，也不暴露 Takeover。

## 实现合同

- owner 固定持有一个 assigned `BackupEpoch` 和 canonical Active `ConfigState`。
- `SYNC_BEGIN` 只有同时满足 `source == assigned_head`、exact BackupEpoch、`config_id + phase + hash` 与 Active Config 匹配、且控制 sequence 严格等于 `0` 时才可能开启 staging。
- begin 复用 mirror 的 strict serial gate：有 committed snapshot 时只接受同 BackupEpoch 的新 snapshot；staging 已开不可被覆盖。
- 有效 begin 只更新 staging table/Epoch；committed table/Epoch 与 `committed_valid` 不变。
- init 不读取 prior owner storage，避免调用者传入新栈 storage 时的未初始化读取；成功 reassignment 用 canonical candidate 原子替换，因此旧 mirror 不会跨 Epoch 继承。

## 定向验证

| 检查 | 结果 |
|---|---|
| 刷新中 begin 保留 committed member bytes | PASS |
| staging Epoch exact 记录 | PASS |
| 非 Head source 拒绝且 owner 无写回 | PASS |
| Epoch 不一致/Config hash 不一致拒绝且 owner 无写回 | PASS |
| 已开 staging 不可覆盖 | PASS |
| reassignment 清空 committed/staging mirror | PASS |
| nonzero BEGIN 拒绝且 owner 无写回 | PASS |
| 最终 Full/Lite/Nano CTest | 各 16/16 PASS |
| 最终 Service OFF CTest | 37/37 PASS |
| production handler 隔离 grep | PASS，无调用 |
| `git diff --check` | PASS（仅既有 CRLF 提示） |

## 自审结论

`CLV2-09-03` 为 **CODE COMPLETE / SELF-AUDIT PASS（受限 sync owner）**。

09-04 仍必须实现 sequence/count/hash/member nonce/coverage 验证后才能打开 atomic swap；当前 staging 与 committed 都没有 M10 Takeover 输入资格。M05 `AUDIT HOLD` 和 M08 外审等待不变。
