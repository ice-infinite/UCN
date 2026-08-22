# CLV2-07-12 删除 legacy auto-commit 自审报告

日期：2026-08-23  
范围：M07 最后一项；只收紧 Host 模拟与 production-archive 回归，不启用 production v4 RX/TX/FSM、Authority、Adapter 或默认 v4 encoder。

## 目标

`JOIN_REQUEST` 在 v3 生产控制面中只可占用受限的 Runtime provisional slot；它不得把成员直接变成 voter，也不得给其 Backup/Takeover authority。任何 v4 voter 的新增、删除或状态转换仍只能由 07-01..11 的显式 Config transaction + persistence owner 实验路径完成。

## 修改

- 删除 CMake 中 `UCN_CLUSTER_LEGACY_V3_TEST_BRIDGE`：`ucn_cluster_sim` 不再重编译 `ucn_cluster.c`/membership，也不再获得 legacy auto-commit 定义；它直接链接 production `ucn_cluster` archive。
- 删除历史 v3 bridge 宏的源码分支。保留的 `UCN_CLUSTER_ENABLE_TEST_HOOKS` 只服务于既有 M01 Current-FSM fixture binary；它不是 public/product 配置，也不被 M07 Config tests、simulator 或 production archive 使用。
- 删除依赖 v3 Backup/Takeover 的 `head-failover` CTest 断言。v3 authority RX 已在 M06-R01 fail-closed，而 v4 frozen-Config takeover certificate 属于 M10；保留该场景并把其成功当作当前能力会形成虚假的安全结论。
- 新增 production-archive `JOIN_REQUEST` 回归：public RX 后 v3 member 必须为 `PROVISIONAL/non-voting/v3`，有 deadline，不进入 `active_voter_set`，不能成为 protected voter 或 Backup。

## 自审证据

1. `rg UCN_CLUSTER_LEGACY_V3_TEST_BRIDGE CMakeLists.txt include src tests tools` 无匹配；显式 M06 bridge 已不存在。
2. `handle_join_accept()` 的实现范围没有 `voting` 写入；生产 `JOIN_REQUEST` 回归直接经 public RX 验证其创建的 member 是 `PROVISIONAL/non-voting`。
3. Windows GCC Full（`UCN_BUILD_CLUSTER_SIM=ON`）重新 configure、build、CTest：`23/23` 通过，包含 64/256/1000 clean+impaired、64 mobility/score-shift 和 group=2/4/8 isolation。
4. 单独执行被移除的旧 `head-failover` 场景：不收敛。这是预期 fail-closed 结果，因为它尝试依赖已围栏的 v3 Backup/Takeover control；该结果不作为 M07 的性能或恢复能力宣称。

## 结论与限制

`CLV2-07-12` 为 **CODE COMPLETE / SELF-AUDIT PASS**。M07 的 Config value/transaction/persistence 实验路径已不依赖 M06 simulator bridge；production v3 ingress 仍只产生 provisional Runtime state。

本项不实现 `MEMBER_PROVISIONAL` production v4 phase、v4 Config wire RX/TX、Config ACK/Commit 广播、Authority、Backup/Takeover certificate 或真实 Flash/掉电。它们分别留在 M05/M08/M09/M10 和 MCU 实机门禁中；M05 顶层 **AUDIT HOLD** 保持不变。
