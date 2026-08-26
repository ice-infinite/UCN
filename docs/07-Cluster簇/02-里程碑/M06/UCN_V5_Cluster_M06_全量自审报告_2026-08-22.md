# UCN V5 Cluster M06 全量自审报告

日期：2026-08-22  
范围：`CLV2-06-01` 至 `CLV2-06-09`  
最终结论：**DONE / 外部复审 GO（受限软件范围）**。

本报告只说明软件自审已经完成；不等价于生产发布、真实 MCU RAM/Flash、掉电恢复或 v4 真实互通通过。

> 外审在初版报告后发现生产 v3 RX 仍可进入 Backup/Takeover handler。该 P0 已按 R01 整改并由外审签署 GO：production RX 在任何状态、计时、统计和 shadow 写入前拒绝 v3 Type 8、10..15、18、19；production archive 回归不启用测试宏，legacy bridge 保持 target-private。整改细节以 `UCN_V5_Cluster_M06_R01_v3BackupReceiveAuthority_整改自审报告_2026-08-22.md` 为准。**本签字不解除 M05 顶层 `AUDIT HOLD`。**

## 1. 分项自审结论

| 子项 | 自审结论 | 关键闭环 |
|---|---|---|
| 06-01 | PASS | member status/record 合法域、canonical empty record、显式转换表。 |
| 06-02 | PASS | `primary_members` 固定表替代无名数组；Runtime 与 Backup mirror 的含义可区分。 |
| 06-03 | PASS | canonical voter set：排序、无重复、FNV-1a hash、contains/quorum 与覆盖 Head 的 64-bit logical bitmap。 |
| 06-04 | PASS | 未来 RX Owner 的 post-validation helper 只生成 v4 provisional，绝不改 voter set、Backup 或 Authority。 |
| 06-05 | PASS | provisional 有独立 deadline；边界到期清理、重复接纳不续期、容量可复用。 |
| 06-06 | PASS | 生产 v3 是 bounded non-voting provisional；Backup/takeover/Recovery protected voter 只接受 committed/voting/v4。 |
| 06-07 | PASS | status/voting/config_id 仅经 summary 只读投影；错误路径不写 caller output。 |
| 06-08 | PASS | Runtime 与包含 Head 的 Voter capacity 分离；拒绝原因可诊断，future commit preflight 无副作用。 |
| 06-09 | PASS | 历史 v3 auto-commit 只留在 Host 测试副本，生产 archive/model 无该宏。 |

## 2. 本次总审发现并修复

生产 v3 语义收紧后，旧 64-node `head-failover` simulator 使用的是历史 v3 Current-FSM 模型，最初使 Service-OFF 的两个 failover CTest 失败。产品逻辑不应为仿真回退，因此修复为：

- `ucn_cluster` production archive：不定义 bridge macro，v3 始终是 `PROVISIONAL/non-voting`；
- `ucn_cluster_membership_model_tests`：链接 production archive，用于验证该严格语义；
- `ucn_tests`：仅其自编译 membership copy 定义 `UCN_CLUSTER_ENABLE_TEST_HOOKS=1`；
- `ucn_cluster_sim`：仅其 Host copy 定义 `UCN_CLUSTER_LEGACY_V3_TEST_BRIDGE=1`，保留旧规模/failover 模型。

两个 bridge 都是 target-private、无公共配置/API 入口；它们不构成产品兼容承诺。`CLV2-07-12` 必须删除这两个 legacy auto-commit 分支。

同时修正了 `ucn_cluster_membership.h` 的过期注释：产品 v3 不再被表述为 committed/voting。

## 3. 最终软件验证矩阵

| 门禁 | 实际结果 |
|---|---|
| Windows GCC Debug Full | `4/4` PASS |
| Windows GCC Debug Lite | `4/4` PASS |
| Windows GCC Debug Nano | `4/4` PASS |
| Windows GCC 配置契约 | `7/7` PASS |
| Windows GCC 用户产品配置 | `5/5` PASS |
| Windows GCC Service OFF（含规模/故障场景） | `27/27` PASS |
| 全新 Windows GCC Release Full（含规模/故障场景） | `27/27` PASS |
| WSL GCC ASan/UBSan Full | `4/4` PASS |
| WSL GCC `-fanalyzer` Full | `4/4` PASS |
| `git diff --check` | PASS；仅既有 CRLF 提示 |

64-node Host simulator 的额外实际结果：clean 在 `8920 ms` 收敛；default head-failover 的 recovery 为 `9850 ms`；fast-fixed head-failover 的 recovery 为 `2590 ms`。这些结果使用测试桥，证明历史模拟回归仍受控，**不**证明产品 v3 有 voter/Backup 权限。

## 4. 资源与边界检查

- Full Host x64 `sizeof(ucn_cluster_t)=1552 B`；相对 M03 `1136 B` 为 `+416 B`。M06-03..09 相对 M06-02 的聚合变化为 `+160 B`。详见 `docs/results/cluster_m04_resource_delta.md`。
- `src/extended/ucn_cluster.c` 中无 v4 RX/TX/dispatcher/FSM 调用；Adapter/transport 中也无 Cluster v4 调用。
- `UCN_CLUSTER_WIRE_V4_ENCODER_ENABLED=1` 只出现于两个独立 codec/Host dual-stack 测试 target；生产默认 fail-closed。
- `cluster_admit_verified_v4_provisional_member()` 和 voter preflight 的调用点仅在 membership model 测试；没有生产 wire RX 接线。
- 生产路径中没有新的 `voting=true` 或 committed member 写入；唯一 v3 auto-commit 在两个显式 Host test bridge 中。

## 5. 仍然禁止的事项

M05 顶层仍为 `AUDIT HOLD`。M06 没有实现或放行：

- production v4 40 B RX/TX/dispatcher/FSM；
- Config Commit、Joint Config、voter set 真正变更、quorum certificate 或 Authority；
- 将 capability/diagnostic 结果当作 Head、Backup 或 voter 资格；
- 真实 Flash/双槽掉电、板级 MCU RAM/Flash/栈或真实 v4 节点互通。

因此，下一步是由外部审计按本报告、任务表和源代码检查 M06；外审未通过前，不进入 M07 的真实 Config transaction，也不解除 M05 的生产边界。
