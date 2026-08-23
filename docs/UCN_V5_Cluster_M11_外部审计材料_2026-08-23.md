# UCN V5 Cluster M11：外部审计材料（2026-08-23）

> 历史外部复审结论：**R01–R06-B GO（受限实验范围）**。
> 当前状态：外部逐提交复审已签 R07（连续合格样本）PASS；其同时发现 R08 旧 proposal/replay 共用域的 ABA history-loss。R08-A 已完成自审，M11 处于 **AUDIT HOLD / WAIT EXTERNAL RE-REVIEW**。
> 审计范围始终仅限 M11 caller-owned、default-OFF 实验模型；不是 production protocol、实机或掉电签字。

## 1. 审计边界

必须保持：

- M05：`AUDIT HOLD`；
- M08：`WAIT EXTERNAL`；
- M10：等待外部复审；
- M11：default-OFF 实验 archive，不接入 production v4 RX/TX/FSM、Authority、Adapter 或默认 encoder。

re-entry Fence 是 caller-owned RAM 的实验模型约束，不是掉电或原始内存破坏防护。生产接线仍须由 M04 持久化/reload 与真实 Authority Owner 保证。

不应因本轮签字而批准真实 Handover 帧、Head Authority、持久 Epoch、Flash/掉电、MCU RAM/Flash、无线、多跳或跨版本互通结论。

## 2. 审计对象

| 类别 | 文件 |
|---|---|
| 实验 public API | `include/ucn/ucn_cluster_handover.h` |
| 实验 value model | `src/extended/cluster/ucn_cluster_handover.c` |
| 定向回归 | `tests/test_cluster_handover.c` |
| 构建隔离 | `CMakeLists.txt` |
| 默认产品 score 围栏 | `src/extended/cluster/ucn_cluster_merge.c`、`src/extended/ucn_cluster.c` |
| 默认产品回归 | `tests/test_cluster.c`、`tests/test_cluster_persist.c` |
| 计划/自审 | `UCN_V5_Cluster_M11_MergeHandover_连续实施计划_2026-08-23.md`、`UCN_V5_Cluster_M11_分项与全量自审报告_2026-08-23.md` |

## 3. 必查安全合同

1. **foreign Term 不可比较**：`cluster_id` 不同只允许 Merge candidate，不得影响 local Authority/Term；`A/T2` 与 `B/T100` 是明确反例。
2. **可行性先于 READY**：target capacity、v4 format、`BACKUP|JOINT_CONFIG|PERSISTENCE`、Config/Backup policy 任一不成立都不能 Ready。
3. **有序撤权**：exact Ready 前不得 revoke/Stepdown/Commit；trace 必须为 `READY → revoke → Stepdown → Commit`。
4. **撤权后安全失败**：Stepdown/Commit 前超时或目标丢失不能恢复 old Authority，只能 Observe/Recovery；`TARGET_COMMITTED` 同样受 deadline 约束，且在 M04 Provider `submit→reload` 证明接线前，caller-supplied equal Epoch 不能使 target ready/Authority。
5. **同簇特殊合同**：target Term 必须 exact next，target Head 是 confirmed Backup，Config 不变；Backup READY 本身没有 Authority。
6. **Stepdown Config 不可变**：RFC4 Type 9 只绑定 old/target Epoch、txid/nonce，不能携带 target Config，也不能被用来改变成员 Config。
7. **候选迟滞不可伪造**：`score_samples` 只允许统计同一 hysteresis context 和 local qualification context 下连续达到 threshold 的 fresh score；低分或 context 改变必须清零，eligible 也不得消费 stale samples。
8. **R08-A replay/history 不可 ABA**：nonce replay namespace 只绑定 full Epoch + Config ID/hash，且只接受严格前进的 namespace；capacity、size、capabilities、wire format 与 Backup policy 是可逆 hysteresis context，改变时只重置 sample/first-seen，不能重置 nonce high-water。D1→D2→D1 的旧 nonce 必须 no-write `UCN_ERR_REPLAY`。
9. **旧 v3 路径无旁路**：Member score 不能直接 LEAVE/Join；Backup score 不能直接 Term++；已持久化的 legacy async challenge continuation 不得复活。
10. **物理隔离**：default product archive 无 M11 object/API；生产 Cluster/Adapter 无 M11 API 调用；v4 encoder 默认仍关闭。
11. **RFC/公共值边界**：Type 26/27/28 的 typed nonce 必为零且不参加 identity，只有 Type 9/29 使用 nonce；所有 policy duration 通过 `ucn_duration_is_valid()`，Term/Config/txid 不得超过 serial rotation threshold；corrupt public transaction（特别是 `trace_count > 8`）必须零写拒绝。
10. **撤权 transaction 不可重开**：`revoke_authority()` 必须写入 implementation-owned 的不可逆 re-entry Fence；已 `AUTHORITY_REVOKED`、`STEPDOWN_SENT`、`COMMIT_SENT` 的对象，以及任何携带非法非零 Fence 的对象，调用 public `transaction_reset()` 都必须 no-op。随后在任何 Begin 参数下都必须保持逐字节不变，且 `local_authority_active` 不得由 Begin 或 Reset→Begin 重新置位。合法 Begin 参数配合此类对象必须返回 `UCN_ERR_STATE`；非法非零 Fence 必须 fail-closed。

## 4. 推荐复现命令

以下为本轮自审使用的等价命令；外审请在独立 scratch/build 目录执行。

```powershell
# Windows：Full + config contract + M11 archive
cmake -S . -B build_external_m11 -DUCN_BUILD_CLUSTER_HANDOVER_EXPERIMENTAL=ON `
  -DUCN_BUILD_CLUSTER_TAKEOVER_EXPERIMENTAL=OFF -DUCN_BUILD_CONFIG_CONTRACT_TESTS=ON
cmake --build build_external_m11 --config Debug --parallel 4
ctest --test-dir build_external_m11 -C Debug --output-on-failure

# 默认产品隔离：M11 OFF
cmake -S . -B build_external_default -DUCN_BUILD_CLUSTER_HANDOVER_EXPERIMENTAL=OFF `
  -DUCN_BUILD_CLUSTER_TAKEOVER_EXPERIMENTAL=OFF -DUCN_BUILD_TESTS=OFF
cmake --build build_external_default --config Debug --parallel 4

# 静态差异检查
git diff --check
rg -n "ucn_cluster_handover" src include -g '!ucn_cluster_handover.c' -g '!ucn_cluster_handover.h'
```

```bash
# WSL/Linux GCC：sanitizer
cmake -S . -B build_external_m11_asan -DUCN_BUILD_CLUSTER_HANDOVER_EXPERIMENTAL=ON \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer'
cmake --build build_external_m11_asan -j4
ctest --test-dir build_external_m11_asan --output-on-failure -j4

# WSL/Linux GCC：static analyzer
cmake -S . -B build_external_m11_analyzer -DUCN_BUILD_CLUSTER_HANDOVER_EXPERIMENTAL=ON \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS='-fanalyzer -Wall -Wextra -Wpedantic -Werror'
# 本轮自审的 analyzer 证据是 M11 定向 target；保留 -j1 以避免
# ccache/并行归档偶发性干扰被误判为源码结果。
cmake --build build_external_m11_analyzer --target ucn_cluster_handover_tests -j1
ctest --test-dir build_external_m11_analyzer -R '^ucn_cluster_handover_tests$' --output-on-failure
```

## 5. 建议的对抗探针

- foreign Cluster 大 Term/高 score 连续输入到 Member、Backup、Head；断言无 Authority、Term、LEAVE/Join 写入。
- 未 Ready 直接 revoke/Stepdown/Commit；错误 source/role/txid/nonce/old Epoch/target Epoch/Config 的 Prepare/Ready/Commit；每次断言 transaction 和 output 无写。
- 同簇：不同 Config、跳 Term、未确认 Backup、Backup 提前 Authority；均拒绝。
- 同簇：Backup 的实际 `local_epoch` 必须仍是 old Epoch；它只能携带已接纳的 `expected_target_epoch` 作为 proposal，不能在 Commit/persist 前把 target Epoch 当作本地已生效 Epoch。
- Ready 后撤权、Stepdown/Commit 前超时；old Head 不得恢复 Authority。
- Member 接到 foreign Stepdown 时，old Config 与 target Config 不同仍只能开始 Join target；任何非零 Stepdown Config 字段必须拒绝。
- Type 26/27/28 的 nonzero typed nonce 必须拒绝且不改 transaction/output；Type 9 的 nonzero fence nonce 必须保留。`INT32_MAX+1` duration、Term/Config/txid rotation threshold 之外的输入必须无 deadline/transaction 写入。
- target 收到 Commit 后，不调用 `step()` 也不能在 deadline 后继续 Commit 或自称 durable；未接线 Provider receipt/reload 前 `target_authority_ready()` 必为 false。设置 `trace_count=9` 或非法 trace/state 后，所有 public transaction API 不得读越界或写 output。
- Begin→Ready→Revoke 后先调用 Reset 再 Begin，以及 Stepdown-sent、Commit-sent 两阶段的 Reset→Begin，均必须 `UCN_ERR_STATE`、逐字节 no-write 且不恢复 local Authority；另应尝试非法非零 Fence，确认 validator fail-closed。
- Provider PENDING 后触发历史 `BACKUP_CHALLENGE` action；必须得到 `UCN_ERR_UNSUPPORTED` 且不写 durable/RAM 状态。
- default archive 符号/链接扫描；production `ucn_cluster_receive()` 不能因任何 v3 score 输入自主换 Cluster。
- R08-A：D1 `nonce100` → D2（capacity/size/capability/wire/Backup-policy 任一变化）`nonce101` → old D1 `nonce50/51`，后两帧必须 `UCN_ERR_REPLAY` 且 table 完整不写；真新 Epoch/Config `nonce1` 可接收，其后旧 namespace 即便 nonce 更大仍必须拒绝。

## 6. 自审结果供比对

| 矩阵 | 自审结果 |
|---|---:|
| MSVC Full Debug + config contract / Release（R08-A 后） | 各 41/41 |
| MSVC Lite Debug + config contract | 20/20 |
| MSVC Nano Debug + config contract | 20/20 |
| MSVC Service-OFF Debug + config contract | 20/20 |
| WSL GCC 13.3 ASan/UBSan（R08-A 后） | 41/41 |
| WSL GCC 13.3 `-fanalyzer -Wall -Wextra -Wpedantic -Werror` | M11 定向 target 1/1 |

说明：Windows Full Debug/Release、Lite/Nano/Service-OFF 定向 target、WSL sanitizer 与 analyzer 已在 R08-A 后重跑；Windows 矩阵以 M11 archive 显式 ON、M10 archive OFF 执行；WSL sanitizer 是独立 GCC Debug 构建。分析器本轮只构建/执行 M11 定向 target，不能误记为全量 analyzer 结果。

R08-A 仅完成自审，等待外部复审；后续若发现默认生产路径出现 v4/M11 接线、任何 score→Authority/Term/Join 旁路，或 Stepdown 绕过 Config/Authority 合同，必须保持/恢复 `AUDIT HOLD`。无论如何，本材料不授权进入 M12。
