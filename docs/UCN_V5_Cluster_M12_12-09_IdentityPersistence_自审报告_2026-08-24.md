# UCN V5 Cluster M12 12-09 Recovery Identity Persistence 分项自审报告（2026-08-24）

## 1. 范围与结论

本报告只处理 CLV2-12-09（P0）: 持久化恢复身份，避免重启后复用旧 Recovery ID/nonce。结论: SELF-AUDIT PASS（任务或分支: boot incarnation 已满足 ID 不复用）。

## 2. 完成定义逐条对照（任务表 CLV2-12-09）

| 要求 | 证据 | 结论 |
|---|---|---|
| persist round/lineage 或至少 boot incarnation + tombstone | boot incarnation（04-09）持久化并回流 provider 请求; incarnation 编入 Recovery ID（12-02 链式 mix） | PASS（或分支） |
| 避免重启后复用旧 Recovery ID/nonce | 重启 replay 测试: Boot2 incarnation N+1 派生 id2 异于 id1 | PASS |
| 重启 replay 测试 | cluster_persist_test_recovery_identity_restart | PASS |

## 3. 关键设计说明

- ID 不复用的机制链: M04 04-09 持久化 incarnation（REQUIRED init 前严格递增）, 12-02 链式 mix 混入 incarnation, 每 boot 新 ID。
- RAM-only 边界: round/lineage 不持久化（v3 292B 记录固定，扩展归 M13 schema v4）; 重启后 round 从 0 起、退避有界。
- 恢复域退役 tombstone 复用 M13 Rekey 语义，恢复域专用 tombstone 需要 schema 扩展，明确后置。

## 4. 测试与门禁证据

- 新测试 1 组（两 boot 全链）; FULL Debug / ASan+UBSan All UCN tests passed; OBSERVED 29 零违例; Golden 8b80b08、mismatch 0; cluster_bytes 1616; -Werror、diff --check 干净。

## 5. 边界

- nonce 跨重启不持久化: 旧轮帧靠新 ID 域隔离 + 12-06 轮绑定（重启后同岛仍以新轮收敛）。
