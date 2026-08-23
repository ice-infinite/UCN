# UCN V5 Cluster M12 12-03 Recovery Round/Backoff 分项自审报告（2026-08-24）

## 1. 范围与结论

本报告只处理 CLV2-12-03（P1）：bounded exponential observe/backoff + deterministic jitter + 稳定加入后重置。结论：SELF-AUDIT PASS。

## 2. 完成定义逐条对照（任务表 CLV2-12-03）

| 要求 | 证据 | 结论 |
|---|---|---|
| bounded exponential observe/backoff + deterministic jitter | compute_recovery_backoff（ucn_cluster_recovery.c）：base 翻倍封顶 16 倍、clamp max；jitter=mix(parent, round, node) 派生、上限 base/4 | PASS |
| TTL/选举失败 round++ | stepdown_recovery_head() 中 recovery_round++（TTL 与仲裁失败共用） | PASS |
| 稳定加入 Stable Cluster 持续一段时间后才 reset | JOIN_ACCEPT 站点武装 lineage_reset_deadline_ms；MEMBER step 到期 cluster_lineage_reset()；detach 取消 | PASS |
| 分区抖动不会高频自旋 | 指数退避+抖动；M01 零退避自旋消除（既有测试场景 f 改写） | PASS |

## 3. 关键决策与整改记录

- base 大于 max 的配置：钳制而非拒绝（自审中发现原校验会拒绝既有小 max 测试配置）。
- 抖动确定性：只依赖 (parent, round, node)——同节点同轮重复计算一致，异节点/异轮去同步。
- round 上界：退避指数封顶 min(round, 4)；round 本身单调增长无回绕风险（ID 唯一性另由对象轮保证）。
- 稳定加入判定：JOIN_ACCEPT 只来自稳定 Head（恢复域 Join 走 DECLARE/ACK），因此单站点武装即可，无歧义。

## 4. 测试与门禁证据

- 新测试 cluster_test_recovery_backoff_and_reset（6 组）+ 既有 01-04f 场景 (f) 按 M12 语义改写。
- FULL Debug All UCN tests passed；ASan/UBSan 零报错；OBSERVED-PAIRS 30 零违例；Golden 8b80b08、trace mismatch 0；cluster_bytes 1616；-Werror 干净；diff --check 干净。

## 5. 边界

- 12-09 持久化 round/lineage（当前 RAM-only；重启后从 incarnation 起新 ID）。
- reset 周期默认 30s（FAST 15s）；产品可覆盖。
