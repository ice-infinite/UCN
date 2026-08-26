# UCN V5 Cluster M12 12-07 Stable Precedence 分项自审报告（2026-08-24）

## 1. 范围与结论

本报告只处理 CLV2-12-07（P0）：任意合法 Stable Head 优先于 Recovery。结论：SELF-AUDIT PASS。

## 2. 完成定义逐条对照（任务表 CLV2-12-07）

| 要求 | 证据 | 结论 |
|---|---|---|
| 任意合法 Stable Head 优先于 Recovery | RECOVERY_HEAD 既有有序让位 + 成员 reclaim 门（STABLE_RECLAIM） | PASS |
| Recovery Head/Member 有序 Join Stable Head，不比较 score 阻止 | 测试 (a)：score=100/capacity=0 仍 reclaim；(c)(d)：RECOVERY_HEAD 有序 STEPPING_DOWN | PASS |
| Stable reclaim 测试 | cluster_test_recovery_stable_precedence（4 组） | PASS |

## 3. 关键设计说明

- reclaim 条件只认 parent lineage（candidate cluster_id 等于 parent_cluster_id 且 term 不小于 parent_term）；03-06 已拒更旧 Term，故条件内即为合法稳定继任者。
- 与 11-08 的关系：成员冻结只约束 score 型外簇切换；reclaim 是 lineage 驱动的有序 JOIN，优先级在其之前。
- reason 枚举 +1（STABLE_RECLAIM=32，COUNT=33）：编译期静态断言与映射测试同步。

## 4. 测试与门禁证据

- 新测试 4 组；既有恢复/成员回归全过。FULL Debug / ASan+UBSan All UCN tests passed；OBSERVED 29 零违例；Golden 8b80b08、mismatch 0；cluster_bytes 1616；-Werror、diff --check 干净。

## 5. 边界

- 异父稳定头的合并仍归 M11 handover（本项只覆盖 parent lineage 的 reclaim）。
