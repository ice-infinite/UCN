# UCN V5 Cluster M07-04 Removal Proposal 自审报告

日期：2026-08-23  
范围：`CLV2-07-04`；仅实现 remove transaction 的受控值层规划。

## 安全契约

- `ucn_cluster_config_member_mark_removing()` 只接受合法的 v4 `COMMITTED/voting` member，转换后保持 `voting=true`，仅 status 变为 `REMOVING`。
- `ucn_cluster_config_tx_begin_remove_marked()` 要求该 member 仍属于 Stable C_old；生成的 Joint 中 C_old 原样保留该 voter，C_new 才排除它。
- 不能借此删除 Head，且不会改 active voter set 或调用任何 cluster owner。

## 定向反例

| 场景 | 结果 |
|---|---|
| `{1,4,9}` 删除 REMOVING `9` | Joint C_old `{1,4,9}`，C_new `{1,4}`；Head `4` ACK 映射正确 |
| 传入仍是 COMMITTED、未标记 member | `UCN_ERR_ARGUMENT`，transaction 无写回 |
| 试图删除 Head `4` | `UCN_ERR_ARGUMENT`，transaction 无写回 |
| 已标记 member 在 proposal 后 | 输入 member 完整保持，仍 `REMOVING/voting=true` |

## 隔离与验证

无 production LEAVE/lease timeout RX、Config wire、active voter install、Authority 或 Provider 调用。Windows GCC Full/Lite/Nano 与 WSL ASan/UBSan CTest 各 `7/7` 通过；`git diff --check` 无空白错误，仅已有 CRLF 提示。

## 结论

`CLV2-07-04`：**CODE COMPLETE / SELF-AUDIT PASS**。连续两个成员的删除不会由本模块直接缩小当前 C_old 分母；07-05 再独立实现并验证双 quorum helper。M05 顶层 `AUDIT HOLD` 不变。
