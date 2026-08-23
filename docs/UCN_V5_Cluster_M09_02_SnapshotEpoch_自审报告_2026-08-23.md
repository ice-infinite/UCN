# UCN V5 Cluster M09-02 SnapshotEpoch 与 Config 绑定自审报告（2026-08-23）

## 范围

本项只扩展 M09 的 Backup mirror value-model。没有 v4 frame 编解码或生产 RX/TX/FSM 接线，没有 Authority、Vote、quorum、M10 Takeover 或持久化写入行为。

## 合同

```text
BackupEpoch = {
  cluster_id, term, head_node_id, backup_node_id, backup_generation
}
SnapshotEpoch = {
  BackupEpoch, snapshot_id, config_id, config_phase, config_hash
}
```

- 0、broadcast ID、相同 Head/Backup、0/超阈值 serial 全部非法。
- `config_id/phase/hash` 必须由有效 canonical `ConfigState` 派生；仅有相同 ID 但 phase 或 hash 不同不算匹配。
- `committed_valid` 或 `staging_active` 为真时，对应 Epoch 不得为空或脏值。
- 已有 committed snapshot 时，只接收 exact 相同 BackupEpoch 的严格递增 snapshot；相同/旧值为 replay，不同 BackupEpoch 必须由未来 assignment owner 显式 reset/reassign。
- active staging 不能被后续 begin 覆盖；abort 清除 staging member table 和 Epoch metadata，committed 不变。

## 定向验证

| 检查 | 结果 |
|---|---|
| BackupEpoch ID/serial/角色有效域 | PASS |
| Snapshot 与 Config 的 exact hash/phase 绑定 | PASS |
| staging Epoch 只写 staging，不改 committed | PASS |
| active staging overwrite 拒绝且无写回 | PASS |
| committed snapshot replay 拒绝且全对象不变 | PASS |
| strict newer snapshot 被接受 | PASS |
| 残缺 `committed_valid`/Epoch 状态 fail-closed | PASS |
| Windows GCC Full/Lite/Nano | 各 14/14 PASS |
| Windows GCC Service OFF | 14/14 PASS |
| `git diff --check` | PASS（仅既有 CRLF 提示） |

## 隔离复核

`src/extended/ucn_cluster.c` 与旧 `src/extended/cluster/ucn_cluster_backup.c` 未引用本模块；默认 v4 encoder、production v4 RX/TX/FSM、Authority 与 M10 Takeover Commit 均未改变。

## 自审结论

`CLV2-09-02` 为 **CODE COMPLETE / SELF-AUDIT PASS（受限 value-model）**。09-03 才会新增严格 SYNC_BEGIN owner，09-04 才会开放受 proof 保护的 atomic swap；当前没有任何 committed mirror 可作为 M10 输入。

M05 保持 `AUDIT HOLD`；M08 仍等待外部审计；没有 Flash/掉电、MCU 或实机结论。
