# UCN V5 Cluster M09 全量自审报告（2026-08-23）

## 结论

**M09 = AUDIT HOLD / BLOCKED BY M08 WAIT EXTERNAL（受限实验软件范围）。**

09-01..11 已完整实现并完成分项自审。外审发现的 M09-R01（缺失 protected-voter coverage entry 被错误当作可 grace 的短暂状态）已整改并获外部复审 GO。M09 仍不申请 M05 解封、M10 开工或产品放行，因为其 M08 依赖仍为 `WAIT EXTERNAL`。

## 实现闭环

| 任务 | 自审结论 | 代码证据 |
|---|---|---|
| 09-01 双缓冲 | committed/staging 与 canonical/no-write 合同 | `ucn_cluster_backup_mirror.*` |
| 09-02 SnapshotEpoch | exact BackupEpoch + snapshot + Config ref | `ucn_cluster_backup_mirror.*` |
| 09-03 BEGIN | assigned Head / Epoch / Config / `sequence=0` gate，只开 staging | `ucn_cluster_backup_sync.*` |
| 09-04 Member/END | `MEMBER=1..N` / `END=N+1`、count/hash/nonce/coverage 后 atomic swap | `ucn_cluster_backup_sync.*` |
| 09-05 READY | source + committed SnapshotEpoch + final proof 的纯验证 | `ucn_cluster_backup_sync.*` |
| 09-06 Delta | exact committed base、既有 member freshness-only、连续 sequence、gap resync | `ucn_cluster_backup_sync.*` |
| 09-07 Coverage | Stable/Joint old/new voter sets 全部 ADMITTED | `ucn_cluster_backup_sync.*` |
| 09-08 Grace | only-SUSPECT wrap-safe grace、REMOVED/missing immediate fence、超时永久 ineligible | `ucn_cluster_backup_sync.*` |
| 09-09 No-wrap | snapshot rotate、generation exhaustion→M13 | `ucn_cluster_backup_mirror.*` |
| 09-10 Profile | 完整 Target eligibility + `head_score DESC,node_id ASC` 确定性筛选 | `ucn_cluster_backup_profile.*` |
| 09-11 Failure | BEGIN/Member/END/READY/Delta/Config refresh 矩阵 | `test_cluster_backup_sync.c` |

## 横向安全复核

1. **提交原子性**：只有 `commit_staging_exact()` 的 candidate copy 经所有前置 proof 后写 committed；所有 rejected Member/END/READY/Delta 保持 output/committed 不变。
2. **身份/配置绑定**：Snapshot、READY、Delta 都要求完整 BackupEpoch 和 Config ID/phase/hash；Config refresh 中的旧 staging END 不能提交。
3. **刷新并发边界**：staging 活动时拒绝 Delta；gap 只置 resync flag，不使用跳号数据。Delta 还只允许更新已提交 member 的动态 freshness，不能新增成员、改变 static eligibility 或降低 nonce。
4. **Coverage 语义（R01）**：首次完成是 Config-protected voters 的一跳 ADMITTED 合同；legacy/non-voter 不被误计为 quorum/coverage 证明。committed 后仅**显式** `SUSPECT` 进入 grace；Core 已确认的 protected-voter `REMOVED` 或**缺失的 protected-voter coverage entry** 都立即永久取消 takeover eligibility，恢复 ADMITTED 不能复活该 assignment。
5. **无回绕**：serial boundary 上拒绝新 snapshot；generation 只能递增，接近边界的 assignment 失败并等待 M13 Rekey。
6. **候选资格**：Profile 完整核对 Runtime/member eligibility、Head capability、Core admission、cooldown/blacklist、wire/capability/capacity/score，排序固定为 `head_score DESC,node_id ASC`；它仍只是 pure model。
7. **无提前 Authority**：model 没有 Vote、quorum certificate、persist new Term、Head announcement 或 `ucn_cluster_t` 接线。

## 测试与工具链

| 门禁 | 结果 |
|---|---|
| Windows GCC Debug Full / Lite / Nano | 各 16/16 PASS |
| Windows GCC Debug Service OFF | 37/37 PASS |
| Windows GCC Release（含 Cluster/scale simulation） | 37/37 PASS |
| Windows GCC config-contract | 19/19 PASS |
| Windows user product-config | 17/17 PASS |
| WSL GCC 11.4 ASan/UBSan | 37/37 PASS |
| Windows GCC 14.2 `-fanalyzer -Wall -Wextra -Werror` | 37/37 PASS |
| `git diff --check` | PASS（仅既有 CRLF 提示） |
| 64-node clean simulator | `converged_ms=8920` |
| 64-node impaired simulator | `m03_isolation=not_requested`（M09 不把它作为隔离结论） |

## 资源与隔离

- 新 object 的 Release `.text`：mirror `5008 B`、sync `7728 B`、profile `704 B`；`.data/.bss` 均为 0。这是 archive object 证据，不是最终 MCU 链接 Flash 数字。
- 三个 value model 都是 caller-owned，未嵌入 `ucn_cluster_t`；Release simulator 观察到 `object_bytes=1584`，只说明当前 Host Cluster 结构未直接扩张，不能替代 MCU RAM/stack 实测。
- 扫描确认 production Cluster、legacy Backup handler 和 public Cluster struct 无新 API 调用。默认 v4 encoder 仍为关闭；v4 encoder=1 仍仅在既有 codec/host/release-gate 测试 target。

## 明确保留的边界

- M08 仍为 `SELF-AUDIT PASS / WAIT EXTERNAL`；M05 顶层 `AUDIT HOLD` 继续有效。
- 没有 production v4 RX/TX/FSM 接线、Authority、M10 Vote/Takeover/Certificate、Flash/掉电、MCU RAM/stack 或实机互通测试。
- `VOLATILE_TEST`/Host state-model 成功不等于持久化安全或硬件时序证明。

## 最终自审纠偏

最终横向审阅没有把首版分项结论直接当作通过，而是发现并修正了四项合同偏差后重新跑完整矩阵：

1. **控制序列**：BEGIN 原先没有强制 `0`，END 也可能接受最后一条 Member 的 sequence；现已钉死为 `0 / 1..N / N+1`，且加入 nonzero BEGIN、错误 END、空 Snapshot END 的 no-write 回归。
2. **Coverage**：原先把 `SUSPECT` 与已 Core 确认 `REMOVED` 都放入 grace；现已区分为前者可恢复、后者即时永久 fence。
3. **Backup rank / Profile**：原先只做 wire/capacity 并按 Node ID 选择；现已按 Target 资格全集拒绝，并固定 `head_score DESC,node_id ASC`。
4. **Delta 静态边界**：原先完整 `member` value 可让 Delta 新增 member 或替换其 static fields；现已收紧为既有 member 的动态 freshness-only 更新，并拒绝 static mutation/nonce rollback。

同一轮还发现 Lite/Nano 仅因间接 include 才缺少 score 上限，已改为 Profile header 直接依赖 canonical Cluster score 定义，并由 Lite/Nano、Service OFF、Release、ASan/UBSan 与 analyzer 矩阵共同复证。

## 外审整改 R01：Coverage 缺失不允许借用 grace

外审确认：原 `update_coverage()` 在 initial-ready 不成立且没有显式 `REMOVED` 时，会把 canonical 但不完整的 coverage view 误入 grace。现已将 protected voter 的 coverage 分类为 `ADMITTED / SUSPECT / REMOVED / missing`：只有至少一个明确 `SUSPECT`、同时没有 `REMOVED/missing` 时才进入 grace；`REMOVED` 与 `missing` 都立即清 grace、永久设置 `takeover_ineligible`。Stable `{1,2,3}` 的缺失 `3`、Joint `C_old={1,2,3}, C_new={1,2,4}` 的缺失 `4`，以及恢复完整 ADMITTED 后仍不可复活均已回归覆盖并获 **R01 外部复审 GO（受限范围）**。详见 `UCN_V5_Cluster_M09_R01_Coverage缺失整改自审报告_2026-08-23.md`。

## 已签署 R01 的外审核对项

1. 复核 `SYNC_BEGIN=0`、`SYNC_MEMBER=1..N`、`SYNC_END=N+1` 的 Config change、coverage fail、hash/count/sequence 的 no-write 与 candidate atomicity。
2. 复核 READY 和 Delta 是否能被旧 Snapshot、错误 source、staging refresh 或 hash replay 绕过；特别核对 Delta 无法新增 member、改变 static eligibility 或回退 nonce。
3. 复核只有 explicit SUSPECT 才进入 coverage grace；特别复核 Stable/Joint 的 missing voter 与 REMOVED immediate fence，以及一旦 ineligible 后不可由 ADMITTED/普通 flap 重新启用。
4. 复核 no-wrap boundary 与 M13 交界，及完整 Strict profile/`head_score DESC,node_id ASC` 仅是选择合同、未授予 Authority。
5. 再次确认 production v4 RX/TX/FSM、default encoder 和 M10 Takeover 仍未接入；并确认 M08 仍为 `WAIT EXTERNAL`，不把 M09 R01 修复误解为后续里程碑授权。
