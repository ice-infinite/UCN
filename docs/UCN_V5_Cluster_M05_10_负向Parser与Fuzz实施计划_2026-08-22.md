# CLV2-05-10 负向 Parser 与 Fuzz 实施计划（2026-08-22）

## 授权边界

05-09 已获外部受限 GO。开始 05-10 时，M05 整体仍为 `AUDIT HOLD`：只强化隔离 v4 codec 的输入拒绝与无副作用证明，不接入 Cluster 生产 RX/TX/FSM、资格、Authority 或 encoder。

## 目标

为 Type 1..33 建立可重复的负向验证与 bounded mutation fuzz：任何未知版本、长度、Type、Role、flag、保留字节、无效 Epoch/Node ID 或 type payload 非法域都必须 fail-closed，且不写 caller output、不创建 pending state、不改变 Cluster 状态。

## 子项

| 子项 | 内容 | 验收 |
|---|---|---|
| 10-A | 盘点现有 raw structural gate 与 type-specific semantic gate，形成每 Type 可构造的合法基线。 | 不借被测 builder 生成负向输入。 |
| 10-B | 增加 Type 1..33 的确定性负向矩阵：Role、flags、zero-tail、非法 Cluster/Head ID、serial/duration/payload 域。 | 每个拒绝样本保持 raw/semantic output 不变。 |
| 10-C | 增加固定 seed、有界迭代的 raw mutation fuzz，覆盖 decode、strict dispatch 和 semantic parser。 | 无崩溃、无 sanitizer/analyzer 问题、失败无 output 写回。 |
| 10-D | 回归扫描 v4 仍未进入生产 Cluster 或 Adapter。 | encoder default-disabled，M05 继续 hold。 |

## 非目标

不修改 RFC4、v3 codec、Core Wire、pending admission 语义或任何生产 Cluster 行为。fuzz 只证明 codec 输入健壮性，不证明 v4 quorum、Config、Authority、Handover、Rekey 或实机链路已经完成。

## 已实现

- `tests/test_cluster_wire_v4_codec.c` 新增独立 raw-frame 写入 fixture；负向字节不经 `ucn_cluster_wire_v4_encode()` 生成，避免 parser/encoder 同源回环掩盖问题。
- Type `1..33` 每项均验证合法基线，并逐项注入：非法 Role、该 Type 的组合 flag、保留 flag bit、Cluster ID 为 `0`/broadcast、Term 为 `0`/越 serial 上界、Head ID 为 `0`/broadcast、必填 payload word 为零；有 zero-tail 的 Type 还验证 non-zero tail 拒绝。Type 12 额外覆盖 `SYNC_BEGIN` marker 的零尾规则。
- 未知 version（v3/5 带 40 B）、未知 Type（0/34）均走 strict dispatch 拒绝。每个拒绝断言 `ucn_cluster_wire_v4_decode()` 与 `ucn_cluster_wire_decode()` 的 caller output 均逐字节不变；直接 semantic parser 拒绝也保持 semantic output 不变。
- 固定 seed `0xC15A4E5D`，4096 次有界 mutation fuzz 覆盖 Type `1..33` 的合法基线、随机原始 40 B、1..3 处字节篡改和 39/41 B 长度。每次均检查：成功结果必须仍合法并可转 semantic；失败结果不得写回 raw/dispatch output。

## 自审

### 合同核对

1. `ucn_cluster_wire_v4_decode()` 先完成 exact length/version 与 raw structural gate，只在成功后赋值 output；`ucn_cluster_wire_decode()` 也先写入局部对象、成功后才交付；`ucn_cluster_wire_v4_semantic_from_frame()` 同样使用局部 semantic。负向矩阵与 fuzz 都对这些“不写回”合同设了 sentinel + `memcmp` 断言。
2. 本项未调用 pending helper，也没有 Cluster 状态对象；因此 fuzz 的可观察副作用面只限三个 codec 输出对象，失败时均被钉死不变。
3. 静态隔离扫描确认 `src/extended/ucn_cluster.c` 没有 `ucn_cluster_wire_v4`/`UCN_CLUSTER_WIRE_V4` 引用；`src/adapters/` 与 `include/ucn/adapters/` 没有 Cluster 依赖。`UCN_CLUSTER_WIRE_V4_ENCODER_ENABLED` 的生产默认值仍为 `0`。

### 验证证据

| 门禁 | 实际结果 |
|---|---|
| Windows GCC Full | `28/28` CTest 通过。 |
| 定向 v4 codec | `ucn_cluster_wire_v4_codec_tests` 通过（含矩阵与 4096 次固定 seed fuzz）。 |
| WSL ASan/UBSan | `18/18` CTest 通过。 |
| WSL GCC `-fanalyzer -Wall -Wextra -Werror` | `5/5` CTest 通过。 |
| 隔离扫描 | production Cluster v4 refs = none；generic Adapter Cluster refs = none。 |
| `git diff --check` | 通过；仅打印仓库既有 CRLF 转换提示。 |

### 自审结论

初版外审确认一个 P1：原矩阵只覆盖每 Type 的单一字段与 `DISABLED` Role，不能称为全字段合同。该问题已按 [`UCN_V5_Cluster_M05_10_外审P1整改计划_2026-08-22.md`](UCN_V5_Cluster_M05_10_外审P1整改计划_2026-08-22.md) 重构为独立 RFC4 fixture table：所有 Type、所有当前 Role 枚举、全部 flag byte 与每个受 codec 约束的 P 字段均有确定性测试。

外部复审已签署 **GO（受限范围）**：独立 RFC4 fixture、Type 12 marker、同簇 `HANDOVER_READY` Backup 分支与 4096 次 fuzz 均通过；外审复跑 Windows Full 2/2、WSL ASan/UBSan 25/25。`CLV2-05-10` 标为 **DONE**。

该签字只证明隔离 codec 的输入拒绝和输出无副作用。M05 整体继续 `AUDIT HOLD`，仍禁止 production v4 RX/TX/FSM 接线、资格/Authority 决策与 v4 发送。
