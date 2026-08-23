# UCN V5 Cluster M12 外部审计材料（2026-08-24）

## 1. 审计对象

- 里程碑: CLV2-M12 RecoveryLineage（任务表 L747-772）。
- 基线: 9386dca（M11 R08-B 修复后）；全部改动位于工作区 E:\File\MESH\ucn-wt-m35（分支 wt/m35），未提交、未推送。
- 授权: 用户指令接替完成 M12，逐节点自审通过后进入下一节点，全量自审通过后整理外部审计材料。

## 2. 变更清单（相对 9386dca）

git diff --stat 摘要:
- include/ucn/ucn_cluster.h: lineage 字段/rank API/recovery_scoped 谓词/min_recovery_peers 配置/STABLE_RECLAIM reason/REQUEST 扩展。
- src/extended/ucn_cluster.c: lineage 捕获/重置/绑定/谓词 + id 入口重构 + 恢复站点接线。
- src/extended/cluster/ucn_cluster_recovery.c: 指数退避+抖动+round++、rank 比较器、DECLARE/ACK lineage/round 绑定与仲裁重写、min_recovery_peers 门槛。
- src/extended/cluster/ucn_cluster_merge.c: 成员 reclaim 门（STABLE_RECLAIM）。
- src/extended/cluster/ucn_cluster_codec_v3.c: DECLARE 尾词 parent、ACK 回显 nonce+parent。
- src/extended/cluster/ucn_cluster_authority.c: 恢复域绑定最前拒绝。
- src/extended/ucn_cluster_federation.c: 恢复域 Directory 发布门（撤回保留）。
- src/extended/cluster/ucn_cluster_membership.c: JOIN_ACCEPT 武装 lineage reset。
- tests/test_cluster.c: 10 个新测试函数（12-01..12-10）+ reason 计数同步。
- tests/test_cluster_authority.c / test_cluster_persist.c: 权威范围拒绝 + 重启 replay。

## 3. 逐节点验收映射

| 节点 | 分项报告 | OP | 关键测试 |
|---|---|---|---|
| 12-01 lineage 状态 | 12-01 报告 | OP-369 | cluster_test_recovery_lineage_capture |
| 12-02 Recovery ID | 12-02 报告 | OP-361 | cluster_test_recovery_id_uniqueness |
| 12-03 退避/round | 12-03 报告 | OP-362 | cluster_test_recovery_backoff_and_reset |
| 12-04 rank | 12-04 报告 | OP-363 | cluster_test_recovery_rank(+arbitration) |
| 12-05 权威范围 | 12-05 报告 | OP-364 | cluster_test_recovery_scope + authority 测试 |
| 12-06 round 绑定 | 12-06 报告 | OP-365 | cluster_test_recovery_round_binding |
| 12-07 Stable 优先 | 12-07 报告 | OP-366 | cluster_test_recovery_stable_precedence |
| 12-08 隔离策略 | OP-367 | OP-367 | cluster_test_recovery_isolation_policy |
| 12-09 持久化 | 12-09 报告 | OP-368 | cluster_persist_test_recovery_identity_restart |
| 12-10 套件 | 12-10 报告 | OP-369 | cluster_test_recovery_suite_m12 |

## 4. 复现步骤

cd ucn-wt-m35
cmake -B build -DUCN_PROFILE=FULL -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j4 && ./build/ucn_tests
（ASan 加 -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"；LITE/NANO 换 profile；ucn_cluster_authority_tests 独立 target）

## 5. 预期结果

- All UCN tests passed；OBSERVED-PAIRS count=30、VIOLATION 0；Golden blob 8b80b087c554708e8538ee2db23f545167b31554、trace mismatch 0；cluster_bytes=1616；-Werror 零告警；git diff --check 干净。
