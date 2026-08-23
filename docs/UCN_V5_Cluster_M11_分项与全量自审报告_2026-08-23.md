# UCN V5 Cluster M11：分项与全量自审报告（2026-08-23）

> 当前结论：**M11 外部复审 GO（受限实验范围）**。
> 本报告不解除 M05 `AUDIT HOLD`，也不改变 M08 `WAIT EXTERNAL`、M10 外审等待状态，更不授权启动 M12。

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
| 11-02 candidate | 固定 4 slot，保存 foreign identity、nonce、score samples、size/capacity、wire/capability、Config 与时间状态。 | stale nonce 返回 `UCN_ERR_REPLAY` 且 table 不写；过期按有界时间回收。 |
| 11-03 迟滞 | required samples、improvement%、Head minimum tenure、hold-down 都在 independent candidate model。 | 临界样本不足、hold-down 未过期均不可发起；reverse score 重置 samples。 |
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
| Windows MSVC Full Debug + config contract，M11 ON | 41/41 |
| Windows MSVC Full Release，M11 ON | 41/41 |
| Windows MSVC Lite Debug + config contract，M11 ON | 20/20 |
| Windows MSVC Nano Debug + config contract，M11 ON | 20/20 |
| Windows MSVC Service-OFF Debug + config contract，M11 ON | 20/20 |
| WSL GCC 13.3 + ASan/UBSan，M11 ON | 38/38 |
| WSL GCC 13.3 + `-fanalyzer -Wall -Wextra -Wpedantic -Werror`，M11 定向 target | 1/1 |
| Windows default product，M11 OFF | `ucn_cluster.lib` 构建成功；精确 M11 API symbol scan 为空 |
| whitespace | `git diff --check` 通过；仅出现既有 CRLF 提示 |

Windows 编译会报告既有 CP936/C4819 Unicode 警告；本轮未新增 warning-as-error 失败。所有上述 CTest 均为 Host 软件验证，不能替代 MCU 运行结果。

## 6. 源码边界核对

- `CMakeLists.txt`：M11 archive 只有 option 开启时创建，且为 `EXCLUDE_FROM_ALL`；其 target 才导出 `UCN_CLUSTER_HANDOVER_EXPERIMENTAL_ENABLED=1`。
- `src/extended/ucn_cluster.c`、默认 Adapter 和 production `ucn_cluster` source 没有 `ucn_cluster_handover_*` API 调用。
- v4 encoder 的默认关闭状态未修改；M11 不改变 v3 生产消息固定长度的合同。
- 默认 v3 路径只收紧：Member score 不跳槽；Backup score 不产生 Term++ election；不会新增 Authority。

## 7. 交付判定

外部复审已确认 R06-B 闭环，M11 获得 **外部复审 GO（受限实验范围）**。该签字只覆盖 caller-owned、default-OFF 的 Handover value model、默认 v3 score fail-closed 收敛及对应 Host 软件回归。

它不等于生产放行：re-entry Fence 只是 caller-owned RAM 实验模型的不可逆约束，不能防护掉电或原始内存破坏。未来生产接线仍须由 M04 持久化/reload 与真实 Authority Owner 共同保证；M05 继续 `AUDIT HOLD`，M08 继续 `WAIT EXTERNAL`，M10 仍待外审，M12 尚未开始。不得据此接入 production v4 RX/TX/FSM、Authority、Adapter 或默认 encoder，也不得宣称 Flash/掉电/MCU/无线/多跳或跨版本互通已经验证。
