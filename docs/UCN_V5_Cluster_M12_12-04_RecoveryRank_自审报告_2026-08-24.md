# UCN V5 Cluster M12 12-04 Recovery Rank 分项自审报告（2026-08-24）

## 1. 范围与结论

本报告只处理 CLV2-12-04（P0）：同 lineage 先比较 parent term/config，异 lineage 走普通 Merge。结论：SELF-AUDIT PASS。

## 2. 完成定义逐条对照（任务表 CLV2-12-04）

| 要求 | 证据 | 结论 |
|---|---|---|
| 同 parent：parent_term DESC、parent_config_id DESC、score DESC、node_id ASC | ucn_cluster_recovery_rank_compare（recovery.c 纯函数，公共 API）+ 6 组单元测试 | PASS |
| 不同 parent 走普通 Merge | 比较器 UNRANKABLE；仲裁对异父帧直接忽略（跨父收敛归 M11 handover） | PASS |
| T9 island 不被 T8 高 score 压制 | 单元测试 (1)：T9/score100 胜 T8/score9000 双向 | PASS |

## 3. 关键设计说明

- 线协议扩展：DECLARE 尾 12B 原零填充词承载 recovery_parent_cluster_id（帧长 32B 不变）；旧帧读 0 走 M01 旧仲裁回退，全量既有回归不变。
- term 语义：DECLARE.term 在有 lineage 时镜像 parent_term（恢复控制域的 epoch term 即父代权威），无 lineage 回退 1。
- 仲裁可用维度：v3 线无 score/config 通道，线上仲裁按 parent_term 降序、node 升序；全契约比较器服务本地/v4 排序（M11 handover candidate 排序可复用）。
- 未起退避（nonce 为 0）的节点仍总是接受（M01 规则保留）。

## 4. 测试与门禁证据

- 新测试 cluster_test_recovery_rank + cluster_test_recovery_rank_arbitration；既有 M01 恢复仲裁测试（legacy 路径）全数不变通过。
- FULL Debug All UCN tests passed；ASan/UBSan 零报错；Golden 8b80b08、trace mismatch 0；cluster_bytes 1616；-Werror 干净；diff --check 干净。
- OBSERVED-PAIRS 30→29：消失对 14→4（RECOVERY_OBSERVE→MEMBER_ACTIVE 复合）被 lineage-rank 时序的新复合替代；0 违例、golden 不变，可归因。

## 5. 边界

- score/config 维度的线上仲裁留待 v4（M05 解冻后）；当前比较器契约已完整，线上用可用子集。
- 12-06 将完成 Declare/ACK 的 round 绑定与旧轮 replay。
