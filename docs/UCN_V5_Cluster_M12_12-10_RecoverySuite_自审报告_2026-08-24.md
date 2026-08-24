# UCN V5 Cluster M12 12-10 Recovery Suite 分项自审报告（2026-08-24）

> **M12.3 补正：** 本报告的 suite PASS 只覆盖当时的软件场景。节点重启项仅证明固定短向量中的 boot incarnation 会改变默认 ID，不证明 32-bit ID 硬唯一，也不关闭重启入站 replay、Recovery scope 或 serial no-wrap；这些边界分别归 `CLV2-13-11..13`。

## 1. 范围与结论

本报告只处理 CLV2-12-10（P0）: Recovery 套件（Safety-4 与 Liveness-4/5）。结论: SELF-AUDIT PASS。

## 2. 完成定义逐条对照（任务表 CLV2-12-10）

| 场景 | 套件位置 | 结论 |
|---|---|---|
| Primary+Backup 同死 | 场景 1: 节点 0/1 死亡, 幸存者 2/3 走宽限超时离簇 | PASS |
| 多候选 | 场景 1: 双 head-capable 幸存者, rank 仲裁收敛 | PASS |
| 两个同 lineage island | 场景 1: 同 parent A/1 收敛为单域 | PASS |
| 两个异 lineage island | 场景 4: parent 1 vs 2 双头并存不合并 | PASS |
| TTL 循环 | 场景 2: round 递增、lineage 留存、新轮新 ID | PASS |
| Stable reclaim | 场景 3: parent A/2 归来全员收敛 | PASS |
| 节点重启 | 12-09 `cluster_persist_test_recovery_identity_restart`（固定向量中 incarnation 驱动 ID 变化；非硬唯一证明） | PARTIAL，结构性边界归 M13 |
