# UCN V5 Cluster M07-10 Config ID No-wrap 自审报告

日期：2026-08-23  
范围：`CLV2-07-10`；Config serial boundary 的纯值层/实验 planner 门禁。

## 合同

`config_id` 的合法域是 `1..UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD`。
Stable Config 达到 reserved boundary 仍可被读取、持久化和诊断，但它是
`rekey_required`，不允许再在相同 Cluster identity 下创建下一份 Joint
Config。M13 尚未实现时，Add/Remove proposal 都明确返回
`UCN_ERR_EXHAUSTED`，不回绕到 `1`，也不伪造 Rekey。

## 验证

- `threshold - 1` 的 Stable 可产生唯一的 `threshold` Joint，并 promote 成
  Stable；此 Stable 的 `rekey_required=true`。
- `threshold` Stable 不能再创建 Joint，输出保持不变。
- 处于 boundary 的 Add 与 Remove planner 都返回 `UCN_ERR_EXHAUSTED`，
  transaction 不写回；不存在 `MAX -> 1` 路径。
- Windows GCC Full/Lite/Nano 与 WSL Full ASan/UBSan 均为 `11/11` CTest 通过。

## 限制

`rekey_required` 是给未来 M13 owner 的显式 hand-off，不是本项执行的
Rekey、更不是 Authority、wire 或 persistent cluster identity mutation。
默认产品 v4 encoder/RX/TX/FSM、Adapter 和 M05 顶层 `AUDIT HOLD` 均未改变。

`CLV2-07-10`：**CODE COMPLETE / SELF-AUDIT PASS**。
