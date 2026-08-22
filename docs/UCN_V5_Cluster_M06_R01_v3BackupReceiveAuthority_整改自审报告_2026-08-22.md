# CLV2-06-R01：生产 v3 Backup/Takeover 接收侧权限整改自审报告

日期：2026-08-22  
状态：**DONE / 外部复审 GO（受限软件范围）**。

## 1. 外审问题核实

问题属实。M06-06 原先只限制了 Head 侧 `assign_backup()`：v3 provisional 不会被选为 Backup；但生产 `ucn_cluster_receive()` 仍会把有效 32 B v3 Type 8、10..15、18、19 分派给旧 Backup/Takeover handler。因此 self `BACKUP_ASSIGN` 能进入 `BACKUP_SYNCING`，镜像同步能进入 `BACKUP_READY`，后续可触发 takeover；non-self assignment 也能安装 `known_backup_*` 并接受 `HEAD_TAKEOVER`。

这违反 M06 的边界：在真实 v4 Config/CommittedVoterSet RX owner 进入前，v3 不得授予或消费 Backup/Takeover 权威。

## 2. 整改策略

在 `src/extended/ucn_cluster.c` 的 `ucn_cluster_receive_inner()` 中增加唯一入口 fence：

1. 先完成现有 v3 codec decode 与 source/header structural 校验；
2. 若类型为 Type 8、10、11、12、13、14、15、18 或 19，production archive 立即返回 `UCN_ERR_ACCESS`；
3. fence 位于 `cluster_now()`、`messages_received`、shadow 同步及所有 handler 调用之前，故拒绝不会改变 role、known Backup、mirror、deadline、Head、Term 或统计；
4. 不依赖 `primary_member_is_protected_voter()`，因为 M06 还没有可由接收者验证的 committed-v4 Config/self voter proof；
5. 未来 M07 只能在真实、持久化、可验证的 committed-v4 Config owner 后替换此临时 fence。

额外包含 Type 18/19（resync/reject）：它们同样消费或影响 Backup assignment 生命周期，不能留下旁路。

## 3. Host 测试桥隔离

历史 `ucn_tests` 与 `ucn_cluster_sim` 需要保留旧 Current-FSM v3 模型，故只有它们各自自编译的源副本定义 private macro：

| target | 宏 | 自编译源 | 作用 |
|---|---|---|---|
| production `ucn_cluster` archive | 无 | 无额外 copy | v3 authority frame 一律拒绝 |
| `ucn_cluster_membership_model_tests` | 无 | 链接 production archive | 验证产品语义 |
| `ucn_tests` | `UCN_CLUSTER_ENABLE_TEST_HOOKS=1` | runtime + membership copy | 历史单元回归 |
| `ucn_cluster_sim` | `UCN_CLUSTER_LEGACY_V3_TEST_BRIDGE=1` | runtime + membership copy | 历史规模/failover 模型 |

新 model 源文件含编译期 `#error`：若任何 bridge macro 泄漏到它，构建立即失败。

## 4. 生产 archive 回归

`test_cluster_membership_model.c` 新增的回归只通过公开 API 建立 Cluster、同步 ADMITTED peer、编码合法 v3 32 B frame，并调用 `ucn_cluster_receive()`。它覆盖：

- Type 8、10..15、18、19 均返回 `UCN_ERR_ACCESS`；
- Type10 的 self 与 non-self assignment；
- 完整 Type12 `BEGIN → data → END`；
- 后续 Type8 `HEAD_TAKEOVER`；
- 每个拒绝都对完整 `ucn_cluster_t` 执行 `memcmp`，证明没有 role、`known_backup_*`、mirror、deadline、Head、Term、stats 或 shadow 写回；另故意制造 shadow desync，确认 RX wrapper 也不会借拒绝路径修复它。

## 5. 自审验证

| 门禁 | 实际结果 |
|---|---|
| Windows GCC Debug Full/Lite/Nano | 各 `4/4` PASS |
| Windows GCC 配置契约 / 用户产品配置 | `7/7` / `5/5` PASS |
| Windows GCC Service OFF / Release Full | 各 `27/27` PASS |
| WSL GCC ASan/UBSan / `-fanalyzer` | 各 `4/4` PASS |
| 64-node Host clean / fast head-failover | 收敛；fast recovery `2590 ms` |
| 生产 RX/Adapter v4 调用扫描 | 无 v4 production RX/TX/FSM/Adapter 调用 |
| `git diff --check` | PASS；仅既有 CRLF 提示 |

## 6. 未放行范围

本整改只封闭 **v3 Backup/Takeover 接收侧**，不代表：v4 production RX/TX、Config Commit、Joint Config、certificate/quorum、Authority、真实 Flash/掉电、MCU RAM/Flash/栈或实机互通已经完成。R01 已由外审签署 GO，M06 可标记 DONE；**M05 顶层仍继续 `AUDIT HOLD`**，不因此放行任何生产 v4 接线或实机声明。

## 7. 外部复审签字摘要

外部复审确认：production RX 对受限 v3 类型的拒绝位于所有状态、计时、统计和 shadow 同步之前；回归直接链接 production `ucn_cluster` archive、未启用测试宏，并逐帧验证 `UCN_ERR_ACCESS` 与完整 `ucn_cluster_t` 无写回。`ucn_cluster_sim` 与 `ucn_tests` 的 legacy bridge 均保持 target-private，未泄漏到 production archive。因此 R01 的 P0 闭环，M06 获得 **GO（受限软件范围）**。
