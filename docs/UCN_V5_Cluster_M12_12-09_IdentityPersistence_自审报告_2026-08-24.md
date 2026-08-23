# UCN V5 Cluster M12 12-09 Recovery Identity Persistence 分项自审报告（2026-08-24）

## 0. 外审裁决与正式降级（CLV2-M12.1 MAJOR-4，2026-08-24）

外审裁决: 本项此前自评 PASS（或分支 boot incarnation）不成立——任务原文是 persist boot incarnation + tombstone，而本项只实现了 boot incarnation。
正式处理（外审选项 B）: 12-09 降级为 PARTIAL / boot-ID non-reuse only；重启入站 replay 保护（恢复域 tombstone）正式归 M13（需要 schema v4）。M12 不得宣称 12-01..12-10 字面全部完成。
生效后的准确结论: PARTIAL（boot-ID 跨重启不复用已闭环并有重启 replay 测试；重启入站 replay 保护未实现，归 M13）。

---

## 1. 范围与结论（原始自审，供对照）

本报告原始结论已由上节降级取代；以下为 2026-08-24 初始自审正文，保留作证据链。

## 2. 完成定义逐条对照（任务表 CLV2-12-09，原始）

| 要求 | 证据 | 原始结论 | 外审后结论 |
|---|---|---|---|
| persist round/lineage 或至少 boot incarnation + tombstone | boot incarnation（04-09）持久化并回流 provider 请求；incarnation 编入 Recovery ID | PASS（或分支） | PARTIAL（缺 + tombstone） |
| 避免重启后复用旧 Recovery ID/nonce | 重启 replay 测试: Boot2 incarnation N+1 派生 id2 异于 id1 | PASS | PASS（仅 ID 不复用） |
| 重启 replay 测试 | cluster_persist_test_recovery_identity_restart | PASS | PASS（ID 维度） |

## 3. 关键设计说明（原始）

- ID 不复用的机制链: M04 04-09 持久化 incarnation（REQUIRED init 前严格递增），12-02 链式 mix 混入 incarnation，每 boot 新 ID。
- RAM-only 边界: round/lineage 不持久化（v3 292B 记录固定，扩展归 M13 schema v4）；重启后 round 从 0 起、退避有界。
- 恢复域退役 tombstone 复用 M13 Rekey 语义，恢复域专用 tombstone 需要 schema 扩展，明确后置。

## 4. 测试与门禁证据（原始）

- 新测试 1 组（两 boot 全链）；FULL Debug / ASan+UBSan All UCN tests passed；OBSERVED 29 零违例；Golden 8b80b08、mismatch 0；cluster_bytes 1616；-Werror、diff --check 干净。

## 5. 边界

- nonce 跨重启不持久化: 旧轮帧靠新 ID 域隔离 + 12-06 轮绑定（重启后同岛仍以新轮收敛）。
