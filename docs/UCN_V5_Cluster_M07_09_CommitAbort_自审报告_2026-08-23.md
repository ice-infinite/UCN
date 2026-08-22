# UCN V5 Cluster M07-09 Config Commit / Abort 自审报告

日期：2026-08-23  
范围：`CLV2-07-09`；明确命名的实验 Config/Joint/Persistence 集成模块。

## 完成的合同

`CONFIG_COMMIT` 只能在 M04 Record 已完成 `submit -> load + exact journal`
证明、且实验 Backup staging gate 允许时应用。新增提案把
`PROVISIONAL/non-voting` 原子改为 `COMMITTED/voting`；删除提案才把
`REMOVING/voting` 槽清为 canonical empty。

超时路径先提交新的 M04 `CONFIG_ABORT` durable operation：保留 Stable
`C_old`、保留已完成 txid、清空 staging `C_new`。只有 reload 证明 Abort
后，Joint runtime 才返回 `C_old`；新增成员保持 provisional，删除成员恢复
`COMMITTED/voting`。

Record 引用统一为 `Stable(C_new)`：Joint 仅是 `C_old/C_new` 的临时
envelope，不能被当作最终 committed Config identity。Prepare、Backup
staging、Joint enter、Commit 和重放均使用同一个派生的 `C_new` Stable ref。

## 自审发现并整改

初版将 Joint 全体序列的摘要写入 `staging_config`/`committed_config`。
这会使 Commit 后 Stable `C_new` 的摘要与持久化摘要不同，进而让重试或
重启恢复无法严格匹配。已新增 `ref_from_joint_new()`，先 canonical promote
为 Stable `C_new` 再生成 ref，并在所有实验 Config owner/gate 使用该规则。

重复的 Commit/Abort 只允许匹配同一 txid、同一 terminal operation journal
和同一 Config identity。该重放不会再次 submit、不会再次改 Runtime/member；
任一不同 operation、txid、Config 或终态成员表示均 fail-closed。

## 定向验证

- ADD Commit：`C_old {1,4,9}` 到 `C_new {1,4,9,21}`，成员仅在 durable
  Commit 后成为 `COMMITTED/voting`；重复 Commit 无额外 Provider submit、
  Runtime/member 逐字节不变。
- REMOVE Commit：删除 `9` 后 Stable `C_new` 为 `{1,4}`，原 REMOVING 槽变为
  canonical empty。
- ADD/REMOVE Abort：到 deadline 的 Abort 先持久化；ADD 留在 provisional，
  REMOVE 恢复 committed/voting，active Config 回到 `C_old`。
- 异步 Abort：`PENDING` 时 `durable=false`，只有 poll 后的 reload proof
  返回 `ACTION_ABORT/durable=true`；不允许提前恢复 `C_old`。
- Windows GCC Full/Lite/Nano 与 WSL Full ASan/UBSan 均为 `11/11` CTest 通过。

## 边界

没有默认 v4 encoder、production v4 RX/TX/FSM、Authority、Takeover、Adapter
或产品 Backup mirror 接线。本项没有把 `VOLATILE_TEST` 作为掉电证明；完整
Config body 的双槽/重启/撕裂写恢复仍由 `CLV2-07-11` 完成。M05 顶层
`AUDIT HOLD` 不变。

`CLV2-07-09`：**CODE COMPLETE / SELF-AUDIT PASS**。
