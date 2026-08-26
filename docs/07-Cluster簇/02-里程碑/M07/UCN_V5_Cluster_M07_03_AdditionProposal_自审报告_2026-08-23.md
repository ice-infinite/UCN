# UCN V5 Cluster M07-03 Addition Proposal 自审报告

日期：2026-08-23  
范围：`CLV2-07-03`；仅实现 v4 provisional 成员到 Config-add transaction 的有界值层规划。

## 实现结论

- `ucn_cluster_config_tx_begin_add_provisional()` 仅接受合法、已占用、v4、`PROVISIONAL/non-voting` 的远端 member。
- 由 Stable C_old 拷贝并追加该 Node ID，创建 checked-next Joint C_new；Head 必须在 C_old 内，随后其 ACK 同时记录到 old/new bitmap。
- 调用只写入 caller-owned `config_tx`；传入 Runtime member 为 `const`，不会被标为 `COMMITTED` 或 `voting=true`。

## 定向反例与回退

| 场景 | 结果 |
|---|---|
| Head 不在 C_old | `UCN_ERR_ARGUMENT`，transaction 不写回 |
| v3 或非 PROVISIONAL candidate | `UCN_ERR_ARGUMENT`，transaction 不写回 |
| C_old 已达到 voter capacity | `UCN_ERR_ARGUMENT`，transaction 不写回 |
| 传入合法 `{1,4,9}` + member `21` | 生成 Joint `{1,4,9,21}`，Head `4` 只写对应 old/new ACK bit |

首次 Full 构建时，新增测试的断言宏被补丁转义为两个反斜杠，编译器正确拒绝该测试源；修复为合法 C99 宏后，重新运行所有矩阵。该问题未进入库实现或生产构建产物。

## 隔离检查与验证

- 无 `ucn_cluster_receive()`、`ucn_cluster_step()`、Cluster transmit、v4 codec、Adapter 或 persistence Provider 调用。
- Windows GCC Full/Lite/Nano：各 `7/7` CTest 通过。
- WSL Full ASan/UBSan：`7/7` CTest 通过。
- `git diff --check`：无空白错误；仅已有 CRLF 提示。

## 结论

`CLV2-07-03`：**CODE COMPLETE / SELF-AUDIT PASS**。后续 07-04 才构造 removal proposal；Config wire 广播、真实 v4 RX/TX/FSM、voter install、quorum 与 persistence 仍未接入，M05 顶层 `AUDIT HOLD` 不变。
