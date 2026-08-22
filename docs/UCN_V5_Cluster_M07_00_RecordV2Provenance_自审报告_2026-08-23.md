# CLV2-07-00：Record v2 与 PREPARED 来源区分自审报告

日期：2026-08-23  
状态：**CODE COMPLETE / SELF-AUDIT PASS；外审并入 M07 final。**

## 1. 关闭的问题

R23 的 `LEGACY_PREPARED_ABORT` 原本只能依赖“Record 中存在 PREPARED”判断。未来 M07 Config 或 M13 Rekey 重新开放真正的 PREPARED 后，这会把新事务误当成历史遗留并在启动时删除。

本项将物理 Record writer 提升为 schema v2，并让 decode 保留 `record_schema_version` provenance：

| 输入 | 启动处理 |
|---|---|
| schema v1 + Config/Rekey PREPARED | 仅此组合允许 R23 `LEGACY_PREPARED_ABORT`；原子变为 v2、transaction NONE、incarnation 严格前进。 |
| schema v2 + Config PREPARED | 不允许 legacy abort；当前 M07 owner 尚未接线时 fail-closed，事务保持原样。 |
| schema v2 + Rekey PREPARED | 不允许 legacy abort；M13 owner 未实现时 fail-closed，事务保持原样。 |
| 无 PREPARED 的 v1 | 下一个正常 runtime write 升级为 v2。 |

## 2. 实现核对

- `ucn_cluster_persist_record_encode()` 只接受并写出 schema v2；测试中的 v1 仅由 test-only fixture 模拟历史物理记录。
- decoder 严格只接受 v1/v2，向 Runtime state 写入 provenance；未知 schema 仍返回 `UCN_ERR_VERSION`。
- `cluster_persistence_begin_state()` 在提交前强制 `next_state.record_schema_version=v2`，避免任一正常 runtime 路径继续写 v1。
- `LEGACY_PREPARED_ABORT` admission 强制 `committed=v1`、`next=v2`；Config/Rekey Prepare/Commit 的新 transaction 必须是 v2。
- controlled boot 只会清理 v1 PREPARED；v2 PREPARED 在 M07/M13 owner 还未实现恢复 continuation 时返回 `UCN_ERR_STATE`，不提交、不发送、不修改记录。

## 3. 定向回归

- writer 输出 header schema=`2`；public writer 收到 logical v1 state 返回 `UCN_ERR_CONFIG`；
- v1 fixture decode 回报 v1 provenance，v1 Config/Rekey PREPARED 都能执行一次 R23 migration；
- v2 Config PREPARED encode/decode 后启动返回 `UCN_ERR_STATE`，保持 v2、PREPARED 和 boot incarnation 不变；
- legacy abort 的 raw admission 只接受 `v1 → v2`，generic replay 对 PREPARED 仍拒绝；
- 历史 R23 双槽 torn-write 回归继续覆盖“旧 v1 PREPARED 保留、下一次启动重试迁移”。

## 4. 实际验证

| 门禁 | 结果 |
|---|---|
| Windows GCC Full | `4/4` PASS |
| Windows GCC Lite | `4/4` PASS |
| Windows GCC Nano | `4/4` PASS |
| WSL ASan/UBSan | `4/4` PASS |
| `git diff --check` | PASS；仅既有 CRLF 提示 |

## 5. 限制与下一步

07-00 只建立新旧 PREPARED 的不可混淆 schema 边界，尚未开放 Config 或 Rekey 的恢复 continuation。07-01/02 将建立 Config State/transaction owner；M07 中才可让 v2 Config PREPARED 进入明确的 Resume/Abort/Commit 路径。M13 前 v2 Rekey PREPARED 继续 fail-closed。

M05 顶层继续 `AUDIT HOLD`：默认产品仍不能启用 v4 encoder、production v4 RX/TX/FSM、Authority 或 Adapter 接线；本项不构成真实 Flash、掉电或 MCU 实机验证。
