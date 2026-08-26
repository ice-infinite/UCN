# UCN V5 Cluster M14 / 14-01 Phase 唯一状态源自审报告

> 日期：2026-08-24  
> 状态：`CODE COMPLETE / SELF-AUDIT PASS / WAIT M14 EXTERNAL REVIEW`

## 1. 完成范围

- `ucn_cluster_t.phase` 是 Cluster 生命周期与 Role 的唯一运行时状态源。
- 删除运行时 `role` 镜像、Shadow mapper、Legacy derive/validator/sync，以及由 Phase 可推导的 `backup_ready`、`backup_syncing`、`backup_takeover_active`、`recovery_eligible` 等布尔镜像。
- `ucn_cluster_get_role()` 只调用 `ucn_cluster_phase_to_role()`；初始化以外的 Phase 写入只允许通过 `cluster_transition()`。
- Backup assignment 的“尚有发送工作”不是生命周期状态，改为从已有有界计数器 `backup_assign_remaining` 推导，避免重新制造状态镜像。

## 2. 自审中发现并关闭的问题

1. 首版把 assignment pending 错误地按 Phase 推导，导致 `HEAD_STABLE` 的周期性 `BACKUP_ASSIGN` 刷新停止。已改为从 `backup_assign_remaining != 0` 推导，Golden failover 恢复。
2. 旧 fault-drop fixture 在启用 drop 前没有打开 partition 邻接矩阵，实际形成全分区；旧的 READY+TAKEOVER 双布尔状态掩盖了该测试缺陷。fixture 已改为先建立完整拓扑，再注入丢包。
3. 删除 `recovery_eligible` 后必须保留 serial exhaustion 的不可恢复语义。新增 `RECOVERY_SERIAL_EXHAUSTED` 原因，并让 Recovery Observe 在 round/nonce 耗尽时进入 `DETACHED_OBSERVE`，禁止回绕或继续伪装为可恢复。
4. Golden 仅有两行预期语义差异：节点从 `BACKUP_READY` 进入 `BACKUP_TAKEOVER` 时，ready 投影由 1 变为 0；不再允许同一节点同时处于 Ready 与 Takeover。

## 3. 自动门禁

- 新增 `tools/check_cluster_phase_source.py`：拒绝 retired 字段、retired Shadow 符号、直接 `cluster->role` 和初始化/transition 之外的 Phase 赋值。
- 门禁作为 `ucn_cluster_phase_source_gate` 注册到 CTest。
- 源码扫描确认生产 Phase 直接赋值只剩初始化与 transition commit；Role 只作为消息、候选或只读投影存在。

## 4. 验证结果

| 配置 | 结果 |
|---|---:|
| GCC Full | 21/21 |
| GCC Lite | 19/19 |
| GCC Nano | 19/19 |
| GCC Full / Service OFF | 19/19 |
| Phase source gate | PASS |
| No-wrap source gate | PASS |
| `git diff --check` | 无空白错误；仅既有 CRLF 提示 |

Host Full 观测 `cluster_bytes=1608`，相对 M13 的 1616 B 减少 8 B。该数字不是 MCU RAM/栈实测。

## 5. 边界与结论

本项只关闭双状态源和迁移期 Shadow 债务，不解除 M05 的生产 v4 `AUDIT HOLD`，也不追认 M13 外审。结论为：`CLV2-14-01` 软件范围自审通过，可进入 14-02；仍等待 M14 最终统一外审。
