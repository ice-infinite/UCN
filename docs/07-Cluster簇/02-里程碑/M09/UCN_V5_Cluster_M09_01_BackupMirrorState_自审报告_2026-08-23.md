# UCN V5 Cluster M09-01 Backup 双缓冲状态自审报告（2026-08-23）

## 范围

本项仅建立 M09 的 fixed-storage value model，不解析或发送 v4 frame，不接入 Authority/Takeover，也不修改 M06 对生产 v3 Backup 控制帧的拒绝边界。

## 实现

- 新增 `ucn_cluster_backup_mirror_t`：`committed_members`、`staging_members`、`committed_valid`、`staging_active`。
- `begin_staging()` 只清 staging，保留 committed；`abort_staging()` 也只清 staging。
- `committed_valid == false` 或 `staging_active == false` 时，对应 table 必须全字节 canonical zero；任何脏状态均 fail-closed，不能开启/取消 staging。
- 新增 `ucn_cluster_member_role_storage_t` union，尺寸等于 Backup pair；Head/Member 逻辑模型不需要被设计成 Runtime table 再叠加一对 mirror。
- 未暴露 commit/swap：09-04 必须在 exact BackupEpoch、Snapshot ID、sequence、count/hash、Config 与 coverage 全部通过后才会引入原子交换入口。

## 定向验证

| 项目 | 结果 |
|---|---|
| reset 全字节 canonical | PASS |
| begin/abort 保留 committed | PASS |
| invalid mirror 无写回 | PASS |
| Role-state union 尺寸合同 | PASS |
| Windows GCC Full | PASS，38/38 |
| Windows GCC Lite | PASS，38/38 |
| Windows GCC Nano | PASS，28/28 |
| Windows GCC Service OFF | PASS，14/14 |

## 自审结论

`CLV2-09-01` 为 **CODE COMPLETE / SELF-AUDIT PASS（受限 value-model）**。

仍未把该 union 直接切换到 `ucn_cluster_t` 的旧 Backup handler：当前旧 handler 的同步帧没有 M09 必需的 SnapshotEpoch/Config proof，提前接线会让“模型存在”被误解为“已可接管”。09-02..09-04 完成后才允许在受控实验路径整合。

M05 保持 `AUDIT HOLD`；M08 外部审计仍待完成；没有 production v4 RX/TX/FSM、Authority、Takeover Commit、Flash/掉电或实机结论。
