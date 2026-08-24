# UCN V5 Cluster M12.2：Recovery Lineage Adoption 整改计划与自审报告（2026-08-24）

> **后续复审说明：** M12.2 的 lineage adoption 与 exact-Term 修复仍保留，但其自审范围已由 M12.3 全体复审扩展。当前有效整体结论见 `UCN_V5_Cluster_M12.3_全体复审整改与自审报告_2026-08-24.md`；M12 继续 `AUDIT HOLD / WAIT EXTERNAL`。

## 1. 外审结论与范围

`1fb4521` 的 M12.1 外审确认原 4 个 MAJOR 和 1 个 MINOR 已关闭，但发现新的 MAJOR：节点接受带 lineage 的 `RECOVERY_DECLARE` 后未把其 parent ID/Term 采用到本地。M12 因而保持 `AUDIT HOLD`，不得进入 M13。

本轮只实施三项：

1. 校验、rank 和 phase transition 成功后，Recovery join 采用可信 `parent_cluster_id` 和 forward-only `parent_term`；不从 v3 DECLARE 伪造 `parent_config_id`。
2. `RECOVERY_DECLARE` 同源同 nonce 的 lease refresh 绑定 exact `{cluster_id, term, parent_cluster_id}`；`RECOVERY_ACK` 绑定 exact `{cluster_id, term, head, nonce, parent_cluster_id}`。
3. 固定 parentless late survivor、higher-Term lineage adoption 和 DECLARE/ACK wrong-Term 无权限状态副作用回归；随后重跑全矩阵与边界扫描。

## 2. 不变量

- Adoption 只能发生在消息已通过格式、角色、同/异 parent、winner rank 和 phase transition 检查之后；任一拒绝路径不得写 parent lineage。
- `parent_cluster_id == 0` 时可采用非零 message parent 与其 Term；相同 parent 只允许提高 `parent_term`；不同非零 parent 维持既有不交叉比较/不写规则。
- `parent_config_id` 与 `recovery_round` 均保持已有值（或零）：v3 Recovery DECLARE 没有 Config 或 round 采用证据。
- exact current round 的 DECLARE/ACK 必须包含相同 Term；错 Term 的包不能刷新 lease、加入 member 或增加 ACK。
- M12.1 原有 durable Recovery Epoch、winner fence、Config forward-only、12-09 PARTIAL 和 isolation wording 的闭环不可回退。

## 3. 验收

| 编号 | 对抗场景 | 预期 |
|---|---|---|
| A | parentless Member 接受 H1/A/T9，再收同 parent 同 Term loser H2 | local lineage 变为 A/T9；H2 `REPLAY`，继续跟随 H1 |
| B | A/T8 Member 接受 A/T9 的更优 Recovery Head | current Head、`cluster.term` 与 `parent_term` 均前进至 9；Config 不被伪造 |
| C | same source/cluster/nonce/parent，但 wrong-Term DECLARE | `REPLAY`，不刷新 lease 或修改 Recovery 身份（允许 stale 统计计数） |
| D | same cluster/head/nonce/parent，但 wrong-Term ACK | `REPLAY`，不分配 member、不增加 ACK、不刷新 lease（允许 stale 统计计数） |

## 4. 分项自审

| 任务 | 自审核对 | 结果 |
|---|---|---|
| `12.2-01` | `recovery_adopt_lineage_from_declare()` 位于所有格式、parent-domain、winner rank 与 phase transition gate 之后、join 写入之前；只采用非零 parent ID 和 forward-only Term，不写 `parent_config_id` 或 `recovery_round`。A/B 回归分别覆盖 parentless H1→delayed H2（并锁住 Config/round）和 A/T8→A/T9→后续 lineage capture。 | PASS |
| `12.2-02` | 同 source/nonce 的 DECLARE refresh 同时比对 recovery ID、Term 和 parent；ACK 在分配 member/ACK 计数之前比对 Term。C/D 回归覆盖 wrong-Term DECLARE lease 不刷新、wrong-Term ACK 零 member/ACK 写入以及正确 ACK 正常通过（replay 统计允许增加）。 | PASS |
| `12.2-03` | 复核 M12.1 durable Recovery Term、winner fence、Config forward-only、12-09 PARTIAL 与 isolation wording 未回退；修改生产 Recovery 模块中无 v4/Adapter/Authority 新引用。 | PASS |

## 5. 验证证据

- Windows MSVC Full Debug：`41/41` CTest 通过；定向 `ucn_tests` 通过，Golden trace 比对通过，`OBSERVED-PAIRS=30` 且无违反项。
- Windows MSVC Full Release：`41/41` CTest 通过。
- Windows MSVC Lite：`41/41` CTest 通过；Nano：`31/31` 通过；Service-Off：`41/41` 通过。
- WSL GCC ASan/UBSan：`41/41` CTest 通过；WSL GCC `-Wall -Wextra -Werror -fanalyzer`：`38/38` 通过。
- `git diff --check` 通过（仅仓库既有 CRLF 转换提示）；变更仅位于 Recovery handler、其生产回归和 M12.2 台账，未接入 v4、Authority、Adapter 或 M13。

## 6. 状态

**CODE COMPLETE / SELF-AUDIT PASS / WAIT EXTERNAL RE-REVIEW / AUDIT HOLD**。

本轮只关闭外审提出的 lineage adoption 与 exact-Term binding 缺口；不扩大 12-09 PARTIAL，也不解除 M05/M08/M10 边界，更不授权进入 M13。外部复审应重点复核 A–D 对抗路径及 adoption 是否严格位于所有拒绝 gate 之后。
