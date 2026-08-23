# UCN V5 Cluster M10：外部审计材料（2026-08-23）

**状态：PENDING EXTERNAL RE-REVIEW。** 首轮外审发现 R31–R34 并否决原自审基线；本文件现整理整改后的复现材料，未包含外部 GO 结论。

## 1. 审计对象与范围

- 分支：`codex/v5-adaptive-wire`
- 实施前提交基线：`f316bc4`
- 新增受控模块：`include/ucn/ucn_cluster_takeover*.h`、`src/extended/cluster/ucn_cluster_takeover*.c`、`tests/test_cluster_takeover.c`
- 受影响持久化模块：`ucn_cluster_persist.[ch]` 及现有 runtime/config writer 对 schema v3 的写入更新。

审计目标是 caller-owned M10 实验模型，不是 production cluster 协议接线。请先确认 `UCN_BUILD_CLUSTER_TAKEOVER_EXPERIMENTAL=OFF` 的默认产品没有 M10 object、M10 API 调用、v4 encoder enable 或 production Authority 变化。

M10 header 还要求 `UCN_CLUSTER_TAKEOVER_EXPERIMENTAL_ENABLED=1`；这个定义仅由独立 CMake target 的 `PUBLIC` usage requirement 提供。裸包含必须在编译期拒绝，而不是留到默认产品链接时才失败。

## 2. 不得放宽的验收合同

1. VoteId 必须完整绑定 `{cluster, old_term, proposed_term, config_id, backup_id, generation, snapshot_id}`。
2. v1/v2 partial vote 绝不能成为 M10 durable vote/quorum proof。
3. Stable 要 old quorum，Joint 要 old/new 双 quorum；分母、顺序和 bitmap 来自 frozen canonical Config。
4. self vote 与 proposed Epoch 都必须先经 Provider `submit → completion → load + exact journal/record proof`。
5. PENDING、Provider failure、record mismatch、reentry、duplicate/conflict、certificate fragment 缺失/错绑、timeout/impossible 都不得产生 Head-ready、Authority 或 wire send。
6. old Primary 只能在完整同簇 higher-term certificate 后 permanent fence；不得以 score/source/count shortcut 接受。
7. default product 仍无 v4 production RX/TX/FSM/Authority/Adapter 接线，M05 继续 `AUDIT HOLD`。
8. current Active Epoch 的完整 M10 VoteId 必须围栏通用 `EPOCH_COMMIT`；`EPOCH_DURABLE` 必须是单向终态；只有 current Epoch 的 Vote 能阻止新 Vote，历史 Vote 仅是可替换的审计记录。

## 3. 建议复现命令

Windows GCC 受控 Full：

```powershell
cmake -S . -B build_m10_external -DUCN_BUILD_TESTS=ON -DUCN_BUILD_CLUSTER_TAKEOVER_EXPERIMENTAL=ON
cmake --build build_m10_external --parallel
ctest --test-dir build_m10_external --output-on-failure
```

WSL sanitizer：

```bash
cmake -S . -B build_m10_external_asan \
  -DUCN_BUILD_TESTS=ON -DUCN_BUILD_CLUSTER_TAKEOVER_EXPERIMENTAL=ON \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer'
cmake --build build_m10_external_asan -j
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build_m10_external_asan --output-on-failure
```

默认 archive 隔离检查：

```powershell
cmake -S . -B build_m10_product_external -DUCN_BUILD_TESTS=ON
cmake --build build_m10_product_external --parallel
ar t .\build_m10_product_external\libucn_cluster.a | Select-String takeover
```

最后一条命令应无输出；显式实验构建的 `libucn_cluster_takeover_experimental.a` 应只列出 `ucn_cluster_takeover.c.obj` 与 `ucn_cluster_takeover_persist.c.obj`。

## 4. 建议的对抗检查

- 修改 v3 VoteId extension 的任一字节，要求 CRC/record match 拒绝；
- 用 v1/v2 280 B partial vote 调用 M10 builder/matcher，要求拒绝且 output 哨兵不变；
- Stable/Joint 模式分别尝试缺 Backup self bit、超范围 bit、重复 fragment、错 Config/snapshot/txid/CRC；
- 在 Provider `load`、`submit`、`poll` 回调中递归 init/begin/step，要求返回 `UCN_ERR_STATE` 且零二次写入；
- 在 `UINT32_MAX` 附近测试 deadline 与 impossible quorum；
- 扫描 `src/extended/ucn_cluster.c`、`src/adapters`、`include/ucn/ucn_cluster.h`，不得出现 M10 API 调用。

## 5. R31–R34 整改复审包

首轮外审结论已确认有效，先前“10-11 覆盖充分”的表述已撤回。请以以下对抗回归重新核对整改，而不要沿用首轮自审结论：

1. **R31 / generic Epoch bypass：** 完整 current-Epoch M10 VoteId 落盘后，构造普通 `EPOCH_COMMIT`（任意 Head、严格 Term+1）。`request_admit()` 必须为 `REJECTED`，committed state 不得变化；匹配的 `TAKEOVER_EPOCH_COMMIT` 仍可成功。
2. **R32 / durable terminal：** Epoch 成功落盘并标记 `EPOCH_DURABLE` 后，分别执行未到期 `step(150)`、到期 `step(1000)`、迟到 vote、对**未投票节点 1**的 unreachable 与 exact durable replay。前四项不得改写 transaction 或解除 Head-ready；选择节点 1 确保拒绝来自 terminal gate，而不是旧 vote/unreachable bitmap overlap；exact replay 仅可无副作用返回成功。
3. **R33 / Vote rotation：** 首次 M10 dedicated Epoch commit 后，使用新 Snapshot 和新 old/proposed term 开第二轮 full Vote；历史 v2 partial Vote 也必须可被首个 v3 full Vote 原子替换。反之，current Active Epoch 已有 Vote 仍必须拒绝。
4. **R34 / 覆盖真实性：** 上述三组均已写入 `tests/test_cluster_takeover.c`；R32 的未到期 step 与未投票 unreachable 均为与旧逻辑可区分的独立断言。复跑 Full/Lite/Nano/Service-OFF、Release、config-contract、产品默认关闭与 WSL sanitizer/analyzer 矩阵。10-11 只声称软件 fault/reorder 模型，未声称物理掉电或实机验证。

整改后的内部状态仅为 `CODE COMPLETE / SELF-AUDIT PASS / WAIT EXTERNAL RE-REVIEW`。M05 顶层 `AUDIT HOLD`、M08 `WAIT EXTERNAL`、默认关闭 M10 archive 和所有 production v4/Authority 边界不因本材料而改变。

## 6. 非本轮宣称范围

本轮没有验证真实 Flash/掉电撕裂、MCU RAM/stack、ESP32 或其他硬件、无线/串口多跳，也没有实现真实 v4 wire RX/TX、身份认证、Authority/FSM 或 Head frame 发送。审计结果不得把受控软件模型误写为生产协议或实机完成。
