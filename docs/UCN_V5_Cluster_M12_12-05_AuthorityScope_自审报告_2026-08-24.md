# UCN V5 Cluster M12 12-05 Recovery Authority Scope 分项自审报告（2026-08-24）

## 1. 范围与结论

本报告只处理 CLV2-12-05（P0）：Recovery Head 只拥有 recovery-local authority。结论：SELF-AUDIT PASS。

## 2. 完成定义逐条对照（任务表 CLV2-12-05）

| 要求 | 证据 | 结论 |
|---|---|---|
| Recovery Head 只能拥有 recovery-local authority | M08 init 最前拒绝恢复域绑定；authority_active 无恢复域来源 | PASS |
| 禁止发布为 parent Stable Authority | M08 Owner 拒绝 + Federation 恢复域无 Directory 注册（撤回保留） | PASS |
| 禁止对旧 cluster_id 执行 takeover/config commit | v3 由 epoch/证明门按构造 fail-closed（BACKUP_ASSIGN/HEAD_TAKEOVER 钉桩）；M07/M10 实验 owner 以谓词为契约 | PASS |
| Federation/Directory 标记 Recovery scope | publish_one_locator 恢复域门 + 撤回例外 | PASS |

## 3. 关键设计说明

- 谓词 ucn_cluster_recovery_scoped 是 12-05 的统一判定点；所有恢复域不得获得稳定权威的 enforcement 都以它为前提。
- 恢复域内自身的 authority 语义：recovery-local（控制域内部一致），与 parent Stable Authority 是两个空间，永不混淆。
- 历史 takeover（R02/R03）是 parent 到 recovery 的合法 reclaim 方向，与 recovery 到 parent 夺取相反，未被本项收紧。

## 4. 测试与门禁证据

- test_cluster.c 谓词真值表 + 两处 v3 钉桩；test_cluster_authority.c 恢复域绑定拒绝（双角色）。
- FULL Debug / ASan+UBSan All UCN tests passed；authority 定向 target 全过；OBSERVED 29 零违例；Golden 8b80b08、mismatch 0；cluster_bytes 1616；-Werror、diff --check 干净。

## 5. 边界

- M07 Config commit / M10 takeover 的 enforcement 依赖调用方遵守谓词契约（caller-owned 实验 owner，无 ucn_cluster_t 引用）。
- 12-09 持久化后恢复域标识跨重启同样适用（recovery_cluster_id 持久化或 tombstone）。
