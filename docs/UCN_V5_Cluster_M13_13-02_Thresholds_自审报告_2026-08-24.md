# CLV2-M13 13-02 Thresholds 自审报告

## 结论

`CLV2-13-02` 为 **CODE COMPLETE / SELF-AUDIT PASS**，可进入 13-03。

## 合同

- `term == threshold`：Cluster Rekey。
- `config_id == threshold`：Cluster Rekey。
- 存在 Backup 且 `backup_generation == threshold`：Cluster Rekey。
- 仅 `snapshot_id == threshold`：先分配下一 Backup generation，并从 snapshot 1 做全量同步。
- Snapshot 和 Backup generation 同时耗尽：Rekey 优先，不提供假的 generation continuation。
- 所有 present serial 必须位于 `1..threshold`；absent Backup/Snapshot 对应字段必须为零。

## 对抗与验证

- threshold-1 不触发；三个 threshold trigger 分别精确置位。
- Snapshot-only rotation 与 generation-exhausted Rekey 分支分别覆盖。
- `threshold+1`、absent Backup 携带非零 generation 均拒绝且 decision 保持逐字节不变。
- MSVC `ucn_cluster_rekey_tests 1/1`；`UINT32_MAX→1` 模式扫描无命中；差异检查仅既有 CRLF 提示。

## 限制

现有 Backup mirror 中 guarded `candidate.backup_generation++` 仍由 threshold 前置条件保护，不会回绕；13-09 会把这类 raw increment 也统一替换并加入静态 CI gate。
