# UCN V5 Cluster M12 RecoveryLineage 全量自审报告（2026-08-24）

> **M12.3 复审补正（2026-08-24）：** 本文保留为 M12 首轮全量自审历史证据；其“全部 10 个节点 PASS”结论已被后续 M12.1/M12.2/M12.3 复审覆盖，不能继续作为当前放行结论。当前有效结论见 `UCN_V5_Cluster_M12.3_全体复审整改与自审报告_2026-08-24.md`：运行期整改自审通过，但 M12 整体仍为 `AUDIT HOLD / WAIT EXTERNAL`，`12-09` 仍为 PARTIAL。

## 1. 结论

首轮 M12 全部 10 个节点（CLV2-12-01..12-10）曾逐节点自评 PASS；该历史结论已被 M12.1/M12.2/M12.3 复审补正，不代表当前整体 PASS。
工作区 ucn-wt-m35（分支 wt/m35），不提交、不推送；M05 顶层 AUDIT HOLD、M08 WAIT EXTERNAL、M10 待复审均不解除；不接入 v4/Authority/Adapter 生产路径。

## 2. 里程碑门禁逐条

| 门禁 | 证据 | 结论 |
|---|---|---|
| Recovery 使用新 cluster_id | 12-02: provider 派生（链式 mix 全 lineage+incarnation+对象轮），validity 拒绝 0/broadcast/parent；套件断言恢复 ID != parent | PASS |
| 同 lineage 先比较 parent term/config | 12-04: 纯比较器 parent_term DESC -> parent_config_id DESC -> score DESC -> node_id ASC；DECLARE 仲裁同父才比 term | PASS |
| 连续失败退避升级 | 12-03: base 翻倍封顶 16 倍 + 确定性抖动 + max 钳制；TTL/仲裁失败 round++ | PASS |

## 3. 禁止事项逐条

| 禁止 | 证据 | 结论 |
|---|---|---|
| Recovery 使用 parent cluster_id | 全库无 cluster_id = parent 赋值；恢复 ID 仅经 cluster_make_next_recovery_id | PASS |
| 忽略 parent term/config | parent_term 用于退避抖动/rank/DECLARE term 镜像；parent_config_id 用于 ID mix 与 rank | PASS |
| TTL 后固定间隔自旋 | M01 的 node_id 取模零值退化已删除（仅注释残留）；指数+抖动+非零校验 | PASS |

## 4. 门禁证据

- FULL Debug / ASan+UBSan / LITE / NANO 四 profile: All UCN tests passed；ucn_cluster_authority_tests 全过。
- OBSERVED-PAIRS: 30（12-04 起 29，12-10 套件后 30），VIOLATION 0；Golden 8b80b08 逐字节不变、trace mismatch 0。
- cluster_bytes: 1616（12-01 起 1584+32: lineage 5 字段 + reset deadline + 配置 3 字段，含对齐；相对 M03 基线 1136 累计 +480，Host x64 Debug 观测，MCU 待测）。
- -Werror 全构建零告警；git diff --check 干净。

## 5. 资源账

| 阶段 | cluster_bytes | 增量 |
|---|---|---|
| M11 基线（5a237a0 之后） | 1584 | - |
| 12-01 lineage 字段+绑定 | 1608 | +24 |
| 12-03 配置+reset deadline | 1616 | +8 |
| 12-04..10 | 1616 | 0（线字段复用既有尾词/枚举，无结构增长） |

## 6. 分项报告索引

- 12-01 UCN_V5_Cluster_M12_12-01_RecoveryLineageState_自审报告_2026-08-24.md
- 12-02 UCN_V5_Cluster_M12_12-02_RecoveryID_自审报告_2026-08-24.md
- 12-03 UCN_V5_Cluster_M12_12-03_BackoffRound_自审报告_2026-08-24.md
- 12-04 UCN_V5_Cluster_M12_12-04_RecoveryRank_自审报告_2026-08-24.md
- 12-05 UCN_V5_Cluster_M12_12-05_AuthorityScope_自审报告_2026-08-24.md
- 12-06 UCN_V5_Cluster_M12_12-06_RoundBinding_自审报告_2026-08-24.md
- 12-07 UCN_V5_Cluster_M12_12-07_StablePrecedence_自审报告_2026-08-24.md
- 12-09 UCN_V5_Cluster_M12_12-09_IdentityPersistence_自审报告_2026-08-24.md
- 12-10 UCN_V5_Cluster_M12_12-10_RecoverySuite_自审报告_2026-08-24.md
- OP 记录: OP-360..OP-369（docs/00-项目管理/01-项目操作记录.md）
- 实施计划: UCN_V5_Cluster_M12_RecoveryLineage_连续实施计划_2026-08-24.md

## 7. 已知边界（外部审计应复核）

- 12-08 未单列分项报告（P1，与 OP-367 一并记录；门禁同）。
- 12-09（M12.1 外审 MAJOR-4 后）: 正式降级 PARTIAL / boot-ID non-reuse only；重启入站 replay 保护（恢复域 tombstone）归 M13。另 M12.1 已修 MAJOR-1（durable Recovery Term 一致，新 operation 类 RECOVERY_CREATE_COMMIT）、MAJOR-2（成员当前赢家 fencing）、MAJOR-3（同 parent config 前向刷新）、MINOR（12-08 可见远端多数措辞）——见 OP-371。
- 12-04 线上仲裁只含（parent_term DESC, node_id ASC）: v3 线无 score/config 通道，全契约比较器服务本地/v4 排序。
- 12-05 的 M07/M10 实验 owner 以 ucn_cluster_recovery_scoped 为调用方契约（无 ucn_cluster_t 引用的 caller-owned 模块）。
