# UCN V5 Cluster M11：分项与全量自审报告（2026-08-23）

> 当前结论：**M11 DONE / R01–R08-B EXTERNAL RE-REVIEW PASS / LIMITED EXPERIMENTAL GO**。
> R01–R07 的既有外部复审 GO 仅覆盖各自签字范围；R08 原设计因 ABA history-loss 已撤回并由 R08-A 重做，R08-B 再关闭 expiry 删除 replay-history 的生命周期旁路。此签字不解除 M05 `AUDIT HOLD`，也不改变 M08 `WAIT EXTERNAL`、M10 外审等待状态，更不授权启动 M12。

## 1. 自审范围与冻结边界

M11 的目标是把跨簇 Merge 与同簇 Planned Leadership Transfer 分开建模，并移除默认 v3 路径中会基于 score 自主改变 Cluster/Term 的行为。

允许的交付物只有：

- `ucn_cluster_handover_experimental`：caller-owned、default-OFF 的静态实验 archive；
- 其严格的 public include 编译门；
- 默认产品中对旧 v3 Member score 跳槽与 Backup score→Term++ challenge 的 fail-closed 收敛。

明确未做：v4 production codec/RX/TX/FSM 接线、真实帧发送、Adapter 接线、Authority 写入、持久 Epoch、quorum/certificate、Flash/掉电、MCU/无线/多跳实测。实验模型只产生 typed value，不能作为生产 Authority 依据。

## 2. 分项自审

| 项目 | 关键实现与自审结论 | 定向证据 |
|---|---|---|
| 11-00 范围围栏 | `UCN_BUILD_CLUSTER_HANDOVER_EXPERIMENTAL=OFF`；header 未经 target 的 public macro 直接 `#error`；默认 archive 不编入该 object。 | 默认 `ucn_cluster.lib` 的精确 M11 transaction/candidate/offer/member/feasibility symbol scan 为空；header OFF/ON 编译探针分别失败/成功。 |
| 11-01 offer 分类 | `ucn_cluster_handover_offer_classify()` 只看 `cluster_id`；foreign 不调用 Epoch Term 比较。 | `A/T2`、`B/T100` 仍是 `FOREIGN_MERGE`。 |
| 11-02 candidate | 固定 4 live slot + 固定 4 replay tombstone；保存 full Epoch、Config、nonce、score samples、size/capacity、wire/capability、Backup policy 与时间状态；Epoch+Config 是单调 replay namespace，其他 remote 字段是 hysteresis context。 | stale nonce 在同 Epoch/Config namespace 返回 `UCN_ERR_REPLAY` 且 table 不写；只有严格前进的 Epoch/Config 可从低 nonce 开始；context 改变只清 sample/first-seen；expiry 只结束 live activity，history 不时间回收。 |
| 11-03 迟滞 | required samples、improvement%、Head minimum tenure、hold-down 都在 independent candidate model；sample 只统计连续达标的 score，且绑定 local score、improvement、required samples/capabilities。 | 临界样本不足、低分 packet、这些 qualification input 改变、hold-down 未过期均不可发起；reverse score 清零 samples。 |
| 11-04 feasibility | READY 前检查 target capacity、wire format=4、`BACKUP|JOINT_CONFIG|PERSISTENCE`、Config 与 Backup policy。 | capacity/capability/wire 拒绝均不建立 Ready；无法承接时旧簇保持稳定。 |
| 11-05 事务/replay | Prepare/Ready/Commit 绑定完整 old/target Epoch、target Config、mode、txid；Type 26..28 的 nonce 必为零，Type 9/29 才携带撤权后生成的 nonce；retry 只在 Prepare；重复 exact 输入幂等。 | duplicate Prepare 返回同一 Ready；duplicate Ready/Commit 不推进第二次；错 role/identity/nonce、非法 duration/serial 均零写拒绝。 |
| 11-06 权威顺序 | Losing Head trace 强制 `READY → authority=false → Stepdown → Commit`；没有 READY 不可撤权。 | 越序调用返回状态错误；撤权后超时进入 `ABORTED + recovery_observe_required`，不恢复旧 Authority。 |
| 11-07 Stepdown | Member/Provisional/Backup 检查 old/target Epoch、mode、old Head、txid/nonce。 | 不要求成员缓存 READY；target lost 只转 Observe；严格 Type 9 语义，携带非零 Config 字段即拒绝且 output 不写回。 |
| 11-08 移除 Member 自主跳槽 | v3 Member 对非 current Head score 样本只清 `better_samples`。 | foreign `B/T100` 重复输入后仍保持 `A/T2`、无 `JOIN_PENDING`、无发送队列写入。 |
| 11-09 同簇 Planned Transfer | same cluster 仅 `old.term + 1`、different Head、confirmed Backup、unchanged frozen Config；READY sender 为 Backup 且不代表 Authority。接收 context 分离实际 `local_epoch=old` 和已接纳 `expected_target_epoch`，拒绝 Backup 提前把自己当成 target Head。旧 challenge 变为 no-write `UCN_ERR_UNSUPPORTED`。 | Config 不同、未 confirmed Backup、premature target Epoch 均拒绝；Ready 前超时保留旧 Head；legacy test/persist path 均逐字节不写。 |
| 11-10 组合故障 | 外国 candidate、抖动、重复、retry、目标/成员故障、Backup/Provisional、同簇迁移组合在独立 executable。 | `tests/test_cluster_handover.c` 覆盖安全完成或保持旧簇/Observe 的两种合法终态。 |

## 3. 初版自审遗漏与 R01–R06 整改

首次外审指出，初版自审没有把 frozen RFC4 的 Type 26..28 nonce 边界、所有时间/serial 域、target-side durable proof 和 caller-owned public transaction corruption 当成独立对抗输入。该遗漏属实，故原“WAIT EXTERNAL”结论撤回并保持 `AUDIT HOLD`。

整改后的固定合同如下：

- `HANDOVER_PREPARE/READY/COMMIT` 的 typed `stepdown_nonce` 固定为零，也不参与 transaction identity；Losing Head 仅在已验证 READY 且撤权时接收 nonce，Type 9/29 才使用它。
- policy 的 head tenure、hold-down、retry 与 transaction timeout 全部经 `ucn_duration_is_valid()`；Term、Config ID、txid 统一限制在 `UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD` 内。
- `TARGET_COMMITTED` 被 deadline 管理；M11 尚未有 M04 Provider `submit → reload` 证明，因而相同 Epoch 的 caller input 一律不能变成 durable/Authority，API fail-closed 返回 `UCN_ERR_UNSUPPORTED`。
- 每个公开 transaction 入口先做 bounded `transaction_is_valid()`；它限制 `trace_count <= 8`，验证 state/trace、Epoch、Config、txid、deadline、nonce phase，并使 corrupt value 保持 zero-write。

`tests/test_cluster_handover.c` 已加入 Type 26/27/28 nonce 对照与伪造拒绝、`INT32_MAX+1` duration、rotation threshold 之外的 Term/Config/txid、target commit timeout/no-fake-durable，以及 `trace_count=9`、非法 trace enum、非法 state 的 output/transaction no-write 回归。R01–R05 已随 M11 获得 **外部复审 GO（受限实验范围）**。

复审随后确认 R06：此前 `transaction_begin()` 在参数验证成功后无条件清零 caller-owned object，能够覆盖已 `revoke_authority()` 的有效 transaction 并重建 `local_authority_active`。现已将 Begin 收紧为只接受零表示；`AUTHORITY_REVOKED`、`STEPDOWN_SENT`、`COMMIT_SENT` 三个阶段的二次 Begin 都返回 `UCN_ERR_STATE`，transaction 逐字节保持且 Authority 不会复活。

R06-B 进一步确认 public `transaction_reset()` 也不能成为旁路：`revoke_authority()` 现在写入 implementation-owned 的 re-entry Fence，Fence 只允许为零或固定实现值并纳入 transaction validator。任何非零 Fence 的 Reset 都必须 no-op，故伪造值也只能 fail-closed；`Revoke→Reset→Begin`、`Stepdown→Reset→Begin`、`Commit→Reset→Begin` 均保持完整对象、保持 `local_authority_active=false` 并返回 `UCN_ERR_STATE`。外部复审已确认 R06-B 闭环，R01–R06-B 随 M11 获得 **外部复审 GO（受限实验范围）**。

R06-B 后已重新执行本报告第 5 节的 Full Debug/Release、Lite、Nano、Service-OFF 与 WSL ASan/UBSan 矩阵；定向 `-fanalyzer` 也以 R06-B 后源码重建通过。

## 3.1 R07/R08：Candidate 连续合格样本与旧 Proposal 域

后续复审核实：过去 candidate 把所有 fresh nonce 都累加到 `score_samples`，并只以 `{cluster_id, head_node_id}` 判定 proposal/replay 域。这两项结论属实，可能使低分 packet 被累计成资格，或让新 Epoch/Config/compatibility proposal 继承旧 nonce history 与迟滞样本。

已完成并经外审确认的 R07 收紧：

- score 仅在达到 improvement threshold 时递增；低于阈值立即清零。`candidate_is_eligible()` 同时核对最新 local score、improvement percent、required samples 与 required capabilities，故这些 qualification input 改变后不能利用旧样本。
- 原 R08 曾把 full Epoch、Config ID/hash、cluster size/capacity、capabilities、wire format 以及 Backup policy 共用为 nonce reset 域。外审确认其不能保留可逆 context 震荡后的历史 nonce（D1→D2→D1），故此项**不可签字**，由 3.2 的 R08-A 替代。

## 3.2 R08-A：Replay namespace 与 Hysteresis context 分离

R08-A 已完成如下实现和自审：

- **Replay namespace** 只绑定 full Epoch + Config ID/hash。只有 stored Epoch 严格低于 incoming Epoch，或完全相同 Epoch 内 Config ID 严格前进，才可建立新 namespace 并接受低 nonce。旧 Epoch/Config、相同 Config ID 不同 hash、或同 namespace 的相同/较低 nonce 均在写入前返回 `UCN_ERR_REPLAY`。
- **Hysteresis context** 绑定 cluster size、available capacity、capabilities、wire format 和 Backup-policy compatibility。任意该类字段变化必须携带高于当前 high-water 的 nonce，并清 `score_samples`、重置 `first_seen_ms`；local qualification context 的变化仍遵循同一只清样本规则。因此 reversible context 永远不会清 nonce history。
- 新增 R08-A 回归逐项改变五个 remote context 字段，分别断言样本只从 1 重新开始；钉死 `D1/100 → D2(capacity=7)/101 → old D1/50,51` 两个历史包均 `UCN_ERR_REPLAY` 且 table 字节不写；然后验证新 `D1/103` 可作为新的高 nonce context 样本。另覆盖真新 Epoch 的 nonce `1`、同 Epoch Config ID 严格前进的 nonce `1` 都允许；旧 Config 或相同 Config ID 的冲突 hash、以及旧 Epoch 即便 nonce 更高也不可复活。

本轮验证：Windows MSVC Full Debug `41/41`、Full Release `41/41`；Lite、Nano、Service-OFF 的 M11 定向 target 各 `1/1`；WSL GCC 13.3 ASan/UBSan `41/41`，以及 `-fanalyzer -Wall -Wextra -Wpedantic -Werror` 的 M11 定向 target `1/1`。默认 `M11=OFF` 的 `ucn_cluster.lib` 独立构建成功且 production Cluster/Adapter source scan 无 handover API。既有 CP936/C4819 编码警告未升级为错误。

R08-A 的同槽 ABA 已获外部复审确认；但随后外审发现 candidate expiry 会整体清槽，删除 nonce high-water 和 hold-down，故由 R08-B 继续保持 `AUDIT HOLD`。R08-B 也不构成 production protocol、实机、掉电、Authority 或 MCU 结论。

## 3.3 R08-B：Expiry 与 replay-history / hold-down 生命周期分离

外部审计确认 R08-A 不能单独覆盖 `candidate_expire()`：D1 的 live candidate 若在 nonce `100/101` 后到期，旧实现会 `memset` 该 slot，使同 namespace 的 `50/51` 重新成为 fresh input，也会在原 hold-down 未到期时提前解除 anti-ping-pong fence。

本轮自审的实现与对抗结论：

- candidate 新增 `active`：expiry 只终止 activity 与连续样本，不能清空它所属 replay namespace；在有 tombstone 容量时，历史移动到静态 tombstone，live slot 才可释放。
- tombstone 固定为 4 条，记录 exact Epoch、Config ID/hash、nonce high-water 与 `hold_down_until_ms`，且没有基于时间的回收路径。新 offer 只有 nonce 更高或 Epoch/Config 严格前进才可恢复 live candidate。
- D1 `100/101 → expiry → 50/51` 均返回 `UCN_ERR_REPLAY` 且 table 逐字节不变；D1 `102` 重新进入 live table 后 sample 恰为 1，`103` 才能满足 required samples，且 `hold_down_until_ms=151` 在 `now=124` 仍阻止资格、到 `152` 才解除。
- tombstone 已满时，expiry 保留原 candidate 的 inactive history；将其余 live slot 也到期后，新 identity `UCN_ERR_NO_SPACE` 且完整 table 不写。这是没有 signed freshness/incarnation 证明时的有意 fail-closed 策略，不将任何 tombstone 静默逐出。

R08-B 已完成定向、Full Debug/Release、Lite/Nano/Service-OFF 以及 WSL sanitizer/analyzer 自审，并获得对 `9386dca` 的外部复审 PASS。M11 candidate replay/hysteresis chain 已闭环，M11 可标记为受限实验范围 GO。

## 4. 既有自审中发现并关闭的问题

初版 typed Stepdown 复用了 Prepare/Ready/Commit 的 target Config 字段，并尝试拿成员当前 Config 比较它。跨簇 Merge 中成员当前配置属于 old Cluster，这会错误拒绝合法的 Stepdown，也可能让实现误以为 Stepdown 可改变 Config。

已整改：

- 仅 Type 26/27/28 在 typed message 中填 target Config；Type 9 Stepdown 的两个字段固定为零；
- Member 只检查其当前 Config 本身有效，Stepdown 不以 target Config 重配置；
- 新回归使用 `old Config=(10,11)`、`target Config=(20,21)`，证明跨簇 Stepdown 正常接收；非零 Stepdown Config 字段被拒绝且 result 保持不写回。

这与 RFC4 §6.1 的 Type 9 定义一致。

随后还关闭了一处同簇角色表述偏差：READY 接收 context 现在显式区分已安装的 `local_epoch` 与本次允许的 `expected_target_epoch`。同簇 Backup 的实际 local Epoch 必须仍为 old Epoch；若它在 Commit/persist 前拿 target Epoch 冒充本地状态，Prepare 被 `UCN_ERR_STATE` 拒绝且 transaction 不写。这保证“Backup READY 无 Authority”不仅是角色字段约定，也是状态输入合同。

## 5. 全量验证

| 环境/配置 | 结果 |
|---|---:|
| Windows MSVC Full Debug + config contract，M11 ON（R08-B 后） | 41/41 |
| Windows MSVC Full Release，M11 ON（R08-B 后） | 41/41 |
| Windows MSVC Lite Debug + config contract，M11 ON | 20/20 |
| Windows MSVC Nano Debug + config contract，M11 ON | 20/20 |
| Windows MSVC Service-OFF Debug + config contract，M11 ON | 20/20 |
| Windows MSVC Lite，M11 ON（R08-B 定向 target） | 1/1 |
| Windows MSVC Nano，M11 ON（R08-B 定向 target） | 1/1 |
| Windows MSVC Service-OFF，M11 ON（R08-B 定向 target） | 1/1 |
| WSL GCC 13.3 + ASan/UBSan，M11 ON（R08-B 后） | 41/41 |
| WSL GCC 13.3 + `-fanalyzer -Wall -Wextra -Wpedantic -Werror`，M11 定向 target | 1/1 |
| Windows default product，M11 OFF（R08-B 后） | `ucn_cluster.lib` 构建成功；production Cluster/Adapter source scan 无 handover API |
| whitespace | `git diff --check` 通过；仅出现既有 CRLF 提示 |

Windows 编译会报告既有 CP936/C4819 Unicode 警告；本轮未新增 warning-as-error 失败。所有上述 CTest 均为 Host 软件验证，不能替代 MCU 运行结果。

## 6. 源码边界核对

- `CMakeLists.txt`：M11 archive 只有 option 开启时创建，且为 `EXCLUDE_FROM_ALL`；其 target 才导出 `UCN_CLUSTER_HANDOVER_EXPERIMENTAL_ENABLED=1`。
- `src/extended/ucn_cluster.c`、默认 Adapter 和 production `ucn_cluster` source 没有 `ucn_cluster_handover_*` API 调用。
- v4 encoder 的默认关闭状态未修改；M11 不改变 v3 生产消息固定长度的合同。
- 默认 v3 路径只收紧：Member score 不跳槽；Backup score 不产生 Term++ election；不会新增 Authority。

## 7. 交付判定

R01–R08-B 的外部复审均已确认闭环；`9386dca` 的 R08-B 外审确认 high-water、hold-down、expiry 后 old nonce、fresh higher nonce restart、ABA 与 tombstone/candidate-history saturation 均 PASS（BLOCKER/MAJOR/MINOR 为 0）。M11 当前为 **DONE / LIMITED EXPERIMENTAL GO**。非阻塞 NIT：后续 production integration 前补 tombstone strict-forward direct regression，并禁止普通 runtime 以 `candidate_table_reset()` 清理 replay history。

它不等于生产放行：re-entry Fence 只是 caller-owned RAM 实验模型的不可逆约束，不能防护掉电或原始内存破坏。未来生产接线仍须由 M04 持久化/reload 与真实 Authority Owner 共同保证；M05 继续 `AUDIT HOLD`，M08 继续 `WAIT EXTERNAL`，M10 仍待外审，M12 尚未开始。不得据此接入 production v4 RX/TX/FSM、Authority、Adapter 或默认 encoder，也不得宣称 Flash/掉电/MCU/无线/多跳或跨版本互通已经验证。
