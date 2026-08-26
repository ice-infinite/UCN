# UCN V5 Cluster M09-R01 Coverage 缺失整改自审报告（2026-08-23）

## 状态

**DONE / 外部复审 GO（受限范围）。** R01 已获签署；M09 整体仍因 M08 `WAIT EXTERNAL` 保持 `AUDIT HOLD`。

本报告只关闭外审发现的 Coverage 缺失 P1；不解除 M05 `AUDIT HOLD`，不改变 M08 `WAIT EXTERNAL`，不授权 M10、production v4 RX/TX/FSM、Authority 或实机结论。

## 外部复审结果

外审已确认 `missing` 在 grace 前被识别、`takeover_ineligible` 先于后续 `ADMITTED` 输入拒绝，且 Stable、Joint 与恢复 `ADMITTED` 不复活回归完整。R01 因此签署为 **GO（受限范围）**；本签署不替代 M08 的外审依赖，也不构成 M09/M05 的整体放行。

## 问题核实

问题属实。原 `ucn_cluster_backup_sync_owner_update_coverage()` 先检查 initial-ready，失败后只把显式 `REMOVED` 视为立即失效。一个格式合法、排序正确、但缺少 protected voter 的 coverage view 因此会落入通用 grace 分支，错误保留 `takeover_eligible=true`。

## 整改合同

对 Active Stable/Joint Config 的每个 protected voter，Coverage 只能归入四类：

| 状态 | 行为 |
|---|---|
| `ADMITTED` | 全体均为该状态时维持 eligible。 |
| `SUSPECT` | 仅该显式状态可 arm wrap-safe grace。 |
| `REMOVED` | 立即清 grace、永久 `takeover_ineligible`。 |
| `missing` | 立即清 grace、永久 `takeover_ineligible`；不能当作 SUSPECT。 |

一旦进入 `takeover_ineligible`，后续传入完整 `ADMITTED` Coverage 仍返回 `UCN_ERR_STATE`；只有新的 Backup assignment 初始化才会重新获得资格。

## 实现与回归

- 新增 protected voter 的 `missing` / `SUSPECT` 分类 helper，并在 Coverage 更新前优先处理 `missing || REMOVED`；无明确 `SUSPECT` 的其他异常情况同样 fail-closed。
- 新增 Stable `{1,2,3}`：缺失 `3` 立即失效，随后恢复全 `ADMITTED` 仍不可复活。
- 新增 Joint `C_old={1,2,3}`、`C_new={1,2,4}`：仅缺 C_new voter `4` 即立即失效；随后恢复完整 Joint `ADMITTED` 视图仍不可复活。
- 既有 `SUSPECT` grace、`REMOVED` immediate fence 与 wrap-safe deadline 回归保持通过。

## 本地验证

| 门禁 | 结果 |
|---|---|
| Windows GCC Debug Full / Lite / Nano | 各 16/16 PASS |
| Windows GCC config-contract / 产品配置 | 19/19、17/17 PASS |
| Windows GCC Release / Service OFF | 各 37/37 PASS |
| Windows GCC 14.2 `-fanalyzer -Wall -Wextra -Werror` | 37/37 PASS |
| RflySim-20.04 WSL GCC 11.4 ASan/UBSan | 37/37 PASS |
| `git diff --check` | PASS（仅既有 CRLF 提示） |

## 外审复核重点

1. Stable/Joint 缺失 protected voter 必须立即失效，不能 arm grace。
2. 显式 `SUSPECT` 仍是唯一允许 grace 的状态。
3. `REMOVED/missing` 之后完整 ADMITTED 不得复活旧 assignment。
4. 再确认 M08 仍为 `WAIT EXTERNAL`，以及 M09 modules 仍未接入旧 v3 handler 或 production v4 路径。
