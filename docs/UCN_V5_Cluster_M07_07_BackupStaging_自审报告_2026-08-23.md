# UCN V5 Cluster M07-07 Backup Staging 自审报告

日期：2026-08-23  
范围：`CLV2-07-07`；仅实现 Config Commit 前的 Backup staging eligibility 值模型。

## 合同

- Backup 必须是 v4 `COMMITTED/voting` Runtime member；本模块不自行选择或安装 Backup。
- 有 Backup 时，transaction 的 exact C_new identity 和 txid 必须先 stage，再由该 Backup 本人 ACK；两个值任一不同都不能形成 HA ready。
- 无 Backup 时，policy `require_backup_for_config=false` 可以允许安全但非 HA 的路径，结果永远是 `ha_ready=false`；policy 为 true 则 fail-closed。

## 定向反例

| 场景 | 结果 |
|---|---|
| 无 Backup、policy 不要求 | allowed=true，ha_ready=false |
| 无 Backup、policy 要求 | stage/commit 均拒绝 |
| 有 Backup 但未 ACK | allowed=false |
| 非 Backup source 或变造 digest | `UCN_ERR_ARGUMENT`，gate 完整无写回 |
| exact Backup + exact txid/digest ACK | allowed=true，ha_ready=true |

## 验证与边界

Windows GCC Full/Lite/Nano 与 WSL ASan/UBSan CTest 各 `10/10` 通过。此模块无 Adapter、RX/TX、Authority、Takeover 或 production Backup FSM 调用；`git diff --check` 无空白错误，仅已有 CRLF 提示。

`CLV2-07-07`：**CODE COMPLETE / SELF-AUDIT PASS**。它表示 Config 对 Backup 复制条件的真实输入门，不是“无 Backup 也能 takeover”的承诺；M09/M10/M08 分别仍拥有 mirror、certificate/Takeover 与 Authority 语义，M05 顶层 `AUDIT HOLD` 不变。
