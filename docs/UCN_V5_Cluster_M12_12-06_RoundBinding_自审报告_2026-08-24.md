# UCN V5 Cluster M12 12-06 Recovery Membership Round Binding 分项自审报告（2026-08-24）

## 1. 范围与结论

本报告只处理 CLV2-12-06（P1）：Declare/ACK 绑定 lineage/round、旧轮 replay、成员租约与重复 ACK 幂等。结论：SELF-AUDIT PASS。

## 2. 完成定义逐条对照（任务表 CLV2-12-06）

| 要求 | 证据 | 结论 |
|---|---|---|
| 保留 Declare/ACK 真成簇能力 | 既有 recovery_forms_cluster 等全部回归不变通过 | PASS |
| 消息绑定 lineage/round | DECLARE 已有（recovery id 即 lineage 派生、parent 词、nonce 即轮）；ACK 新增 nonce+parent 回显 | PASS |
| 旧 round declare/ack replay | 头侧 ACK nonce 门 REPLAY；成员侧同源旧 nonce 门 REPLAY（next_nonce 单调性为判定依据） | PASS |
| 成员 lease 与重复 ACK 幂等 | 同轮重复 ACK 仅刷新租约、ack_count 不增（既有机制 + 新测试钉桩） | PASS |

## 3. 关键设计说明

- 旧轮判定的单调性依据：recovery_nonce 来自 next_nonce（每节点单调递增），同源（known_recovery_source）下 nonce 更小即可判定为延迟旧轮。
- 兼容边界：nonce/parent 为零的旧帧保持 legacy 容忍（既有 M01 测试与 staged 场景不变）。
- 12-02 的每轮新恢复 ID 与 12-06 的 nonce 门双保险：即使 ID 不可判序，nonce 仍可判旧轮。

## 4. 测试与门禁证据

- 新测试 cluster_test_recovery_round_binding（5 组）；既有恢复全部回归通过。
- FULL Debug / ASan+UBSan All UCN tests passed；OBSERVED 29 零违例；Golden 8b80b08、mismatch 0；cluster_bytes 1616；-Werror、diff --check 干净。

## 5. 边界

- nonce 回绕：next_nonce 无回绕护栏（既有行为）；12-09/13 的 serial 纪律覆盖 Term/generation/config，nonce 属抗重放令牌、按既有边界。
