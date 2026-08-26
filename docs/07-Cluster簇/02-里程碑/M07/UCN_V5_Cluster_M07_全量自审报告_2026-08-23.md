# UCN V5 Cluster M07 全量自审报告

日期：2026-08-23  
结论：**SELF-AUDIT PASS / 等待外部审计**。  
审计边界：M07 只实现 caller-owned Config/Joint/Persistence 值对象与明确命名的实验集成；M05 顶层 **AUDIT HOLD** 未解除。

## 1. 范围与硬边界

本轮完整覆盖 `CLV2-07-00..12`。默认产品仍：

- 不启用 v4 encoder；`UCN_CLUSTER_WIRE_V4_ENCODER_ENABLED=1` 仅出现在两个独立 codec/Host dual-stack 测试 target。
- 不在 production `ucn_cluster.c` 接入 v4 codec、Config owner、Config transaction、Authority 或 Adapter。
- 不将 `VOLATILE_TEST` 作为掉电安全证明；M04 REQUIRED Provider 的 submit/poll/load exact-journal 仍是实验 owner 的唯一 durable proof。
- 不宣称真实 Flash/双槽物理原子性、MCU RAM、掉电波形或实机 v3/v4 互通已验证。

## 2. 分项闭环

| 项 | 自审结论 | 关键安全结果 |
|---|---|---|
| 07-00 | PASS（历史基线） | Record v2 provenance 使 legacy v1 PREPARED migration 不会误清 v2 新事务；当前 writer 已由 M10 append-only 升级为 v3，v1/v2 继续只读。 |
| 07-01 | PASS | Stable/Joint Config canonical value model，checked-next Config ID。 |
| 07-02 | PASS | 固定单实例 transaction；并发、损坏 ACK bitmap、deadline/retry fail-closed。 |
| 07-03/04 | PASS | ADD 只接纳 v4 provisional/non-voting；REMOVE 在 Commit 前保留 C_old voter。 |
| 07-05 | PASS | 同 txid C_old/C_new 双 quorum，单 Node delta。 |
| 07-06 | PASS | Prepare/Commit 先 submit，再 completion/load exact journal proof。 |
| 07-07/08 | PASS | Backup staging 与 durable Joint gate 均精确绑定 C_old/C_new、txid、ACK/quorum。 |
| 07-09 | PASS | Commit/Abort durable terminal；ADD 仅 Commit 后 voting，REMOVE 仅 Commit 后清空。 |
| 07-10 | PASS | Config ID 到 serial threshold 后不得回绕，明确 `UCN_ERR_EXHAUSTED`。 |
| 07-11 | PASS | CRC32 双槽 body 与 M04 refs 强绑定；PREPARED 只恢复 C_old，torn/missing body fail-closed。 |
| 07-12 | PASS | 删除 M06 legacy bridge；simulator 直连 production archive，v3 join 仅 provisional。 |

每项的定向证据见同目录 `UCN_V5_Cluster_M07_00...12_*自审报告_2026-08-23.md`。

## 3. 关键交叉核验

1. **voter 变化不能 auto-commit。** production-archive 公共 RX 回归证明 v3 `JOIN_REQUEST` 只能创建 `PROVISIONAL/non-voting/v3` member，带 deadline，既不进入 `active_voter_set`、也不成为 protected voter/Backup。ADD/REMOVE 的唯一变更点在 07-09 durable Commit。
2. **持久化顺序。** Prepare、Commit、Abort 均由实验 Config owner 做 `submit → completion → load + exact journal`。无 quorum、submit failure、错 txid/ref、body torn/missing 都保持 caller output/runtime 不写回。
3. **重启与历史格式。** schema v1 PREPARED 仅可走受控 legacy migration；schema v2 PREPARED 不会被误 abort。M10 后正常 writer 为 schema v3，v1/v2 都只读；这不改变 Config body recovery 将 active C_old、staged C_new 与 M04 Record ref 分离验证的 M07 结论。
4. **无回绕与双分母。** proposal/config-tx/quorum 测试覆盖 `threshold-1 → threshold` 的最后合法跳，拒绝下一 Joint；Joint Commit 同时需要 old/new quorum。
5. **M05 隔离。** `src/extended/ucn_cluster.c` 对 v4 codec/Config owner 无引用；默认 encoder 保持关闭。仍保留的 `UCN_CLUSTER_ENABLE_TEST_HOOKS` 只属于 M01 historical Current-FSM fixture，不是 public/product、M07 Config target 或 simulator 配置。

## 4. 验证矩阵

| 门禁 | 实际结果 |
|---|---|
| Windows GCC Full | `12/12` |
| Windows GCC Lite | `12/12` |
| Windows GCC Nano | `12/12` |
| Windows GCC Full, Service OFF | `12/12` |
| Config contract | `15/15` |
| User product header smoke | `13/13` |
| Full + production archive simulator | `23/23` |
| WSL ASan/UBSan | `12/12` |
| WSL GCC `-fanalyzer` | `12/12` |
| `git diff --check` | PASS；仅既有 CRLF 提示 |

Simulator 的 23 项包含 64/256/1000 clean/impaired、64 mobility/score-shift、group=2/4/8 isolation。旧 v3 `head-failover` 被刻意从 CTest 删除；它依赖已被 M06-R01 围栏的 v3 Takeover，不可作为当前恢复能力。

## 5. 外审应重点复核

- 是否存在任何绕过 `ucn_cluster_config_persist_owner` 的 Config voter installation，或 Commit/Abort 后 ref/body/journal 不一致。
- v1/v2 PREPARED 边界在双槽 reload、异步 `PENDING` 和重启下是否仍 fail-closed。
- ADD/REMOVE 的 exact replay、deadline Abort、joint old/new quorum 与 Backup-required policy 是否互相一致。
- 删除 bridge 后是否仍有 production/archive target 可以获得 legacy auto-commit；生产 v3 Backup/Takeover fence 是否保持早于所有状态写入。
- M05 隔离是否持续成立：不得把本轮实验 API 误接入 production v4 RX/TX/FSM、Authority 或默认 encoder。

## 6. 结论和未完成范围

M07 的软件实验范围已连续实施并完成自审，可提交外部审计。它**不是**生产放行，也不授权进入 M08：须由外审确认后再决定后续阶段。真实 Flash/掉电、MCU 资源、Config wire owner、Config ACK/Commit 广播、Backup mirror、v4 takeover certificate、Authority/quorum enforcement 仍为后续里程碑工作。
