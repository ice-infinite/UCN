# UCN V5 Cluster M10：最终 Majority Takeover 分项与全量自审报告（2026-08-23）

**状态：AUDIT HOLD / R31–R34 CODE COMPLETE / SELF-AUDIT PASS / WAIT EXTERNAL RE-REVIEW（受控实验软件范围）**

本报告是 M10 的内部自审记录，不是外部审计签字，也不构成生产或实机完成声明。

## 1. 范围和硬边界

M10 实现了 caller-owned 的冻结多数接管模型、完整 VoteId、可验证证书和持久化 proof owner。实现被物理拆到独立的 `ucn_cluster_takeover_experimental` archive，构建开关 `UCN_BUILD_CLUSTER_TAKEOVER_EXPERIMENTAL` 默认关闭。

- 默认 `libucn_cluster.a` 不包含 takeover object；
- M10 header 还要求 `UCN_CLUSTER_TAKEOVER_EXPERIMENTAL_ENABLED=1`；裸包含在编译期拒绝，实验 target 通过 `PUBLIC` compile definition 显式导出该许可；
- `src/extended/ucn_cluster.c`、Adapter 和公共 `ucn_cluster.h` 没有 M10 API 调用；
- `UCN_CLUSTER_WIRE_V4_ENCODER_ENABLED` 默认仍为 `0`；
- M10 不接入 production v4 RX/TX/FSM、Authority、Head frame 发送或实机链路；
- M08 仍为 `WAIT EXTERNAL`，M05 顶层 `AUDIT HOLD` 不解除。

## 2. 分项自审

| 任务 | 自审结论 | 实现与定向证据 |
|---|---|---|
| 10-00 Scope fence | PASS | CMake 默认 OFF；M10 在独立 archive；默认 archive 的 `ar t` 无 takeover object，生产源码扫描无调用。 |
| 10-01 Frozen transaction | PASS | 只接受 M09 committed mirror + exact Active Config；冻结 BackupEpoch、snapshot、Config、term 和 txid；staging Config mismatch 保持 output 不写回。 |
| 10-02 Full VoteId / Record v3 | PASS（R31/R33 整改后） | VoteId 绑定 cluster/old+proposed term/config/backup/generation/snapshot；v3 292 B append-only record 有 CRC，v1/v2 partial vote decode 后永不作为 M10 proof；current complete Vote 还会围栏通用 Epoch transition，历史 Vote 可为下一轮原子轮换。 |
| 10-03 Member vote gate | PASS | 远端票必须携带 committed-v4、takeover-grace、old Head lease expired 与 exact durable VoteId proof；ACTIVE、provisional、v3 均拒绝。 |
| 10-04 Stable/Joint quorum | PASS | Stable 单 quorum、Joint old/new 双 quorum；测试遍历 `1..UCN_CLUSTER_MAX_VOTERS`，并验证 Backup self vote 的集合资格。 |
| 10-05 Certificate | PASS | canonical voter order、最多两个 bitmap word、domain-bound CRC、fragment/key/range/duplicate/quorum 校验全部在 frozen Config 上完成。 |
| 10-06 Persist-before-result | PASS（R31/R32 整改后） | Vote 和 proposed Epoch 都必须 `submit → completion → load + exact record/journal`；PENDING、失败、reload mismatch 或 Provider 重入均不推进事务；成功的 `EPOCH_DURABLE` 是不可回退终态。 |
| 10-07 Old Primary fence | PASS | 只接受 exact old epoch、同 Cluster、更高 successor term 和完整 certificate；接受后永久 fence/join intent，错误/迟到 input 不写回。 |
| 10-08 Refresh overlap | PASS | 只用 committed snapshot；staging 允许存在但无法替换 VoteId/certificate 输入。 |
| 10-09 Legacy exclusion | PASS | 开票 proof 强制 committed v4/voting/grace/expired lease；legacy v3 与 provisional 永不计入 frozen bitmap。 |
| 10-10 Timeout / impossible | PASS | unreachable 与 vote bit 不可重叠；少数派立即 abort，deadline 使用 modular-clock 逻辑，超时只进入 recovery intent，不缩小 denominator。 |
| 10-11 Software fault/property | PASS（R34 整改后，软件模型） | 覆盖同步/异步 Provider、冲突/幂等 operation、record CRC、duplicate/reorder、fragment、callback reentry，以及 generic bypass、durable terminal、two-round、legacy-history counterexample；没有把这项误称为物理掉电验证。 |

## 3. 关键安全不变量复核

1. 任何 quorum 的分母只来自 transaction 创建时冻结的 canonical Config，永不从 staging 或 Runtime member 表推导。
2. local self vote 只能由 `ucn_cluster_takeover_persist_owner_apply_durable_vote()` 在 owner reload 证明 exact durable Vote 后写入；内部 transition 不在公共头文件暴露。
3. 普通 `VOTE_COMMIT` 只能表示旧 partial vote；`TAKEOVER_VOTE_COMMIT`/`TAKEOVER_EPOCH_COMMIT` 才允许完整 M10 VoteId 与新 Epoch，二者都要求 schema v3。current Active Epoch 上已存在完整 M10 VoteId 时，通用 `EPOCH_COMMIT` 必须拒绝，不能绕过 certificate-bound successor。
4. Certificate verifier 同时核验完整 transaction key、frozen Config、bit 上界、self Backup bit、quorum 和 canonical CRC，不能以 source 或计数 shortcut 代替。
5. Epoch 持久化前没有实验 Head-ready；`EPOCH_DURABLE` 后 transaction 是单向终态，deadline/迟到 vote/unreachable 不得撤回该 RAM proof；即使成功，M10 仍不产生 production Authority、ADVERTISE 或 HEAD_TAKEOVER 帧。
6. Provider 的 `load/submit/poll` 前建立 module-level reentry fence；回调内重入 init/begin/step fail-closed，且不创建第二次写入。

## 4. 验证矩阵

所有下列 M10 受控构建均显式使用 `-DUCN_BUILD_CLUSTER_TAKEOVER_EXPERIMENTAL=ON`，除“产品配置”外均包含 `ucn_cluster_takeover_tests`。

| 环境/配置 | 结果 | 说明 |
|---|---:|---|
| Windows GCC Debug Full | 38/38 | 全量 CTest。 |
| Windows GCC Debug Lite | 38/38 | Profile 裁剪回归。 |
| Windows GCC Debug Nano | 28/28 | Nano 可用集。 |
| Windows GCC Service OFF | 38/38 | Service 独立性。 |
| Windows GCC Release + RFC4 O1/O2/O3 gates | 41/41 | 含既有 Release codec gates。 |
| Windows GCC config-contract | 41/41 | 配置契约矩阵。 |
| 用户 128 B/3-Link 产品配置（M10 默认 OFF、关闭不适配的 Scale sim） | 17/17 | 验证默认产品不构建 M10；64-node tree 不是该容量 profile 的有效测试拓扑。 |
| WSL RflySim-20.04 GCC 11.4 ASan/UBSan | 38/38 | 内存与未定义行为检查。 |
| WSL RflySim-20.04 GCC 11.4 `-fanalyzer -Wall -Wextra -Wpedantic -Werror` | 38/38 | 静态分析构建及 CTest。 |

`git diff --check` 通过；输出只有既有 CRLF 转换提示。当前机器没有可用的 MSVC 实例，因此未把 MSVC 数字写成已验证结果。

## 5. 资源与隔离证据

Release object（Host GCC）观测：

| 对象 | `.text` | `.data` | `.bss` |
|---|---:|---:|---:|
| `ucn_cluster_takeover.c` | 24,820 B | 0 B | 0 B |
| `ucn_cluster_takeover_persist.c` | 4,864 B | 0 B | 16 B |

这是**独立实验 archive 的 object 输入**，不等于最终 MCU 链接后的 Flash、RAM 或任务栈。默认产品 archive 不含这两个对象；M10 也未嵌入 `ucn_cluster_t`。

## 6. 首轮外审 R31–R34 整改复核

首轮外审否决了先前的“WAIT EXTERNAL”自审基线；下列问题已确认属实，首轮覆盖结论相应撤回。本节记录整改后的内部复核，不是外部复审签字。

| 编号 | 首轮缺陷 | 整改 | 正式回归 |
|---|---|---|---|
| R31 / P0 | generic `EPOCH_COMMIT` 可绕过 current Active Epoch 的完整 M10 VoteId，持久化任意 successor Head。 | 通用 Epoch transition 在 committed current Epoch 存在 complete takeover VoteId 时拒绝；只有 certificate-bound `TAKEOVER_EPOCH_COMMIT` 可写 successor。 | 先落完整 Vote，再构造任意 Head、Term+1 的通用 request；断言 `REJECTED` 且 durable record 逐字节不变。 |
| R32 / P0 | `EPOCH_DURABLE` 后 `step()`、迟到 vote 或 deadline 可撤回 RAM result。 | terminal-first：durable `step()` 无副作用成功；迟到 vote/unreachable 返回 replay；exact durable epoch replay 仅幂等成功。 | durable 后执行未到期 `step(150)`、到期 `step(1000)`、迟到 vote、**未投票节点 1** 的 unreachable、exact replay，均逐字节断言 transaction 不变且 Head-ready 保持。 |
| R33 / P1 | 任意历史 `last_vote.valid` 均阻止后续 M10；v1/v2 历史 partial Vote 也造成永久阻塞。 | 只把 current Active Epoch 的 Vote 视为 live one-vote promise；历史 Vote 原子替换为下轮 v3 full Vote。 | 首轮 dedicated Epoch commit 后新 Snapshot 开第二轮；另覆盖 v2 current partial 仍拒绝、v2 historical partial 可开首个 full Vote。 |
| R34 / P1 | 10-11 没有锁住上述 bypass、terminal、连续接管和 legacy-history 交叉反例；首个 terminal 用例未覆盖未到期 step，且 unreachable 命中了已投票 bitmap。 | 上述三类反例均写入 `test_cluster_takeover.c`；terminal 回归增加 `step(150)`，并选择未投票节点 1，确保旧 deadline/bitmap-overlap 逻辑无法误通过；10-11 表述收窄为软件 fault/reorder 子集。 | Windows Full/Lite/Nano/Service-OFF、Release、config-contract、产品默认关闭、WSL ASan/UBSan 与 `-fanalyzer` 矩阵重跑。 |

内部复核结论：R31–R34 均为 **CODE COMPLETE / SELF-AUDIT PASS**；M10 仍为 **AUDIT HOLD / WAIT EXTERNAL RE-REVIEW**，不得据此进入下一里程碑或改动生产边界。

## 7. 已知限制与外审必查项

- Record v3 将 Provider physical slot 从 280 B 提升到 292 B。已部署 Provider 必须先支持 292 B slot 或执行明确迁移；v1/v2 仅只读。
- 本轮只做 Provider 软件故障模型；没有真实 Flash 双槽 torn write、突然断电、MCU 栈/RAM、无线/多跳或多板实测。
- CRC 是一致性/损坏检测，不是证书加密认证；真实 wire 证书的 RX owner、身份认证、Authority 及发送路径仍必须由后续已授权里程碑实现并外审。
- M08 `WAIT EXTERNAL` 与 M05 `AUDIT HOLD` 继续阻止任何生产接线或协议整体放行。

外部审计应重点核对：Record v3 decode/migration、partial legacy vote 不升级、Provider callback reentry、pending reload proof、Stable/Joint quorum、fragment/certificate canonical 输入、timeout wrap、默认 archive 隔离，以及所有 failure path 的 no-write 行为。
