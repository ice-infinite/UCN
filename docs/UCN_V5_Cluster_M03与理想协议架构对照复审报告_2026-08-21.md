# UCN V5 Cluster M03 与理想协议架构对照复审报告

> 审计日期：2026-08-21（Asia/Shanghai）  
> 审计对象：`E:\File\MESH\UCN`  
> 分支：`codex/v5-adaptive-wire`  
> 上次复审点：`0fecd7f`  
> 当前提交：`ee354072673e0dcd2cdb271ae29999bdfcb93062`  
> 当前快照：`HEAD` 之后仍有 17 个已跟踪文件的工作区修改；本报告评价的是 `0fecd7f..ee35407 + 当前未提交工作区`，不是一个可复现的单一 Git 提交。

## 1. 最终结论

### 1.1 代码是不是在朝理想协议架构前进

**是，而且总体方向正确。** 本轮更新不是简单堆功能，主要在完成 Cluster Extended 控制平面的内部成熟化：

- M01 把 `role + bool + deadline` 的隐式组合迁入显式 Phase/统一迁移入口；
- M02 把 6216 行级别的大文件拆成 Codec、FSM、Membership、Backup/Takeover、Recovery、Merge 等模块；
- M03 引入 Epoch 值类型、同簇/跨簇比较域、same-Term conflict 安全等待、Detach 安全历史、受检 serial 和 Cluster ID Provider；
- Cluster 仍是 `EXCLUDE_FROM_ALL` 的可选 Extended 静态库，依赖方向保持 `Cluster -> Core`；
- Cluster 源码未引入 Adapter/Driver/HAL/RTOS SDK 类型，也未引入动态内存；普通 Data Plane 没有被改成 Head 中心转发。

这些变化符合《UCN_理想协议架构与Cluster_Target_FSM定位说明.md》定义的“Extended Cluster 控制平面内部升级”，没有破坏 MCU-first、Core 独立、Host 可选、介质无关的总架构。

### 1.2 当前补全质量

**M02 结构重构质量较好；M03 语义补全尚未达到 DONE。** 建议维持 `AUDIT HOLD`，不得进入 M04 并把 M03 标记为完成。

当前质量判断：

| 维度 | 判断 | 说明 |
|---|---|---|
| 总体架构方向 | 良好 | 模块化、Epoch、Fence 前置基础均与 Target 一致。 |
| Core/Extended 边界 | 良好 | 未发现 Core 反向 include Cluster 私有头，Cluster 无驱动 SDK 依赖。 |
| MCU 资源边界 | 基本良好 | 固定表、无动态内存，`sizeof(ucn_cluster_t)=1136`；但 Member-only/Backup-capable 的细粒度裁剪没有闭环任务。 |
| M02 行为等价拆分 | 良好 | 默认回归和多工具链通过；但 `ucn_cluster.c` 当前又增长到约 1944 行，Backup/Takeover 合并文件约 1868 行，后续仍需控制模块再膨胀。 |
| M03 Epoch 语义 | 不合格 | 仍存在跨簇 Term 参与判断、ActiveEpoch 只做读投影、未真正成为状态单一值。 |
| M03 No-wrap | 部分完成 | 发送侧关键 serial 使用 checked helper，但 Backup 接收序列仍存在 `current + 1` 回绕。 |
| 测试门禁可信度 | 部分合格 | 默认 CTest 全绿，但未覆盖可复现的 group=2 失败；`m03_isolation=checked` 是参数回显，不是性质证明。 |
| 发布成熟度 | 未达到 | M04-M14 仍为 TODO；Persistence、Joint Config、Authority/Fence、原子 Snapshot、最终 Takeover、RecoveryLineage、Rekey 尚未实现。 |

### 1.3 154 项任务全部做完后，能否补全“全部理想 UCN 架构”

结论分两层：

1. **对 Cluster Target FSM v2：基本可以。** 如果 154 项严格按原始完成定义落实、不继续缩小任务语义，并逐条通过 Safety/Liveness、持久化、故障注入、模型、规模和实机门禁，能够形成较完整、健全的 Cluster 控制平面。
2. **对整套理想 UCN 协议架构：不能只靠这 154 项。** 当前任务表本质上是 Cluster FSM 迁移计划，不是 UCN Core、Routing、Security、Adapter、Federation、Host 的总架构闭环计划。即使 M14 完成，也只能证明“Cluster Target v2 已闭环”，不能自动证明“整个 UCN 理想协议所有内容已闭环”。

当前任务表中，M00-M02 共 26 个正式任务为 DONE；M03 的 9 个任务处于待审计实现状态。按 154 个正式任务计：已签字完成约 16.9%，连同 M03 待审实现约 22.7%。真正决定分区安全和跨重启安全的 M04-M13 尚未开始。

## 2. 本轮已确认修复或改善

| 项目 | 结果 | 证据 |
|---|---|---|
| MSVC `C2099` 编译阻塞 | 已修复 | 当前 MSVC Debug 可成功构建 `ucn_cluster_sim` 与 `ucn_tests`。 |
| 默认 fast impaired group=8 | 已修复 | 固定种子、默认 group=8 可完成并输出 8 Head/56 Member。 |
| M02 模块拆分 | 已落地 | 新增 `src/extended/cluster/` 下 7 个实现文件和私有头；`ucn_cluster` 继续只依赖 `ucn_core`。 |
| 跨簇 Epoch comparator | 已建立 | `ucn_cluster_epoch_compare()` 先按 `cluster_id` 截断，再比较同簇 Term。 |
| same-Term different-Head | 已改进 | 普通 Head offer 进入本地 `TERM_CONFLICT_WAIT`，不再由 score/Node ID 裁决。 |
| serial 发送侧 fail-closed | 已改进 | Term、Backup generation、membership sequence 等主要产生路径使用 `cluster_serial_next_checked()`。 |
| Cluster ID Provider | 已建立 | 支持 purpose、parent epoch、incarnation、round，并拒绝 0/broadcast/parent reuse。 |
| 架构边界 | 保持 | Cluster 源码未发现 HAL/Driver/ESP-IDF/动态内存依赖；Core 目录未反向依赖 Cluster 私有头。 |

## 3. 未关闭缺陷与审计发现

### P0-1：固定种子 group=2 受损场景仍触发状态一致性断言

位置：`src/extended/cluster/ucn_cluster_backup.c:303-304`

复现：

```powershell
& .\build-v567-cluster-gcc\ucn_cluster_sim.exe `
  --nodes 64 --scenario impaired --profile fast-fixed `
  --seed 1592594996 --group 2 --expect-m03-isolation
```

结果：

```text
Assertion failed: cluster_phase_from_legacy_state(cluster, now_ms) ==
UCN_CLUSTER_PHASE_BACKUP_SYNCING
```

这说明 `handle_backup_assign()` 的提交后字段组合在高密度两节点分组/受损时序下仍能导出非 `BACKUP_SYNCING` Phase。默认 CTest 没有传 `--group 2`，因此 12/12 全绿没有覆盖该失败。

结论：这是 M03 里程碑阻断项，也是 M01“唯一 Phase/Shadow 一致性”尚未完全闭环的直接反例。

### P0-2：跨簇 Term 仍直接参与 `HEAD_TAKEOVER` 决策

位置：`src/extended/cluster/ucn_cluster_backup.c:1080-1088`

`RECOVERY_HEAD` 分支明确允许 `message.cluster_id != local cluster_id`，随后却执行：

```c
if (message->term <= cluster->term) {
    return UCN_ERR_REPLAY;
}
```

对应现有测试 `tests/test_cluster.c:7929-7931` 也把“foreign stable cluster term 必须大于 recovery term”固化为预期。

这直接违反 M03 门禁“不同 cluster_id 的 Term 永不直接比较”。正确语义应基于已知 Backup 证明、parent lineage/Stable Authority 证明处理；Recovery 本地 Term 与 foreign Stable Cluster Term 没有数值可比性。否则合法 Stable Authority 可能仅因数字较小而无法回收 Recovery island。

### P1-1：Backup 接收序列仍可发生 32 位回绕

位置：`src/extended/cluster/ucn_cluster_backup.c:411`、`:483`

接收侧仍使用：

```c
message->membership_sequence != cluster->membership_sequence + 1U
```

当当前值为 `UINT32_MAX` 时，C 无符号加法回绕到 0；Codec 又没有拒绝超阈值/0 sequence。当前测试只验证 Head 发送侧在阈值 fail-closed（`tests/test_cluster.c:8842-8852`），没有覆盖 Backup 接收侧的阈值、MAX、0 和恶意/异常 SYNC_BEGIN。

结论：03-07 的“No-wrap”只完成了产生侧，尚未形成收发闭环。

### P1-2：`BACKUP_ASSIGN` 缺少 Epoch 绑定，并在 Transition 前污染共享记录

位置：`src/extended/cluster/ucn_cluster_backup.c:220-280`

问题有两部分：

1. Handler 校验 expected source、sync token，却没有按当前 Member/Join Pending Epoch 校验 `message.cluster_id/term`；成功后直接覆盖本地 active epoch。
2. `known_backup_node_id/generation` 在 `cluster_transition()` 前写入；Transition 返回 `UCN_ERR_STATE` 时这两个字段不回滚。

因此，同一物理 Head 的延迟/换簇 Assignment 可以绕过普通 Head offer 的 Epoch 分类；Shadow mismatch 的拒绝路径也不是全字段原子失败。这既是旧审计问题未关闭，也会干扰后续 M04 persist-before-promise 和 M09 原子镜像设计。

### P1-3：`CLV2-03-02 active_epoch` 实际只是读投影，未满足原任务的状态收拢

位置：

- `include/ucn/ucn_cluster.h:665-684`
- `src/extended/ucn_cluster.c:1707-1732`

代码仍物理保存并在多个模块分别写入 `cluster_id/term/head_node_id`；`ucn_cluster_active_epoch_get()` 只是把三个标量复制成临时值。原任务要求把散落状态收拢为 `active_epoch`，避免继续手工组合和部分提交。

当前实现提升了读路径一致性，但没有建立单一写入口，也不能阻止三字段部分更新。建议 03-02 保持 PARTIAL，至少在进入 M04 前建立 `cluster_epoch_commit()/clear()` 一类统一写入口；是否立即改变 public storage 可与 M14 opaque split 分开处理。

### P1-4：Federation Locator cache 仍允许旧记录覆盖新记录

位置：`src/extended/ucn_cluster_federation.c:933-967`

Directory 注册路径会按 `term + record_nonce` 拒绝旧记录，但 `cache_locator()` 找到已有 cache 后直接覆盖，没有比较已有与新 locator 的 Cluster identity、Term、Head、record nonce。延迟的合法 Reply 仍可把较新的 locator/next-cluster cache 回滚。

这会削弱理想架构中的 Federation/Directory/Locator 层，也不在现有 154 项中得到明确关闭；M08-09 只要求 Authority 发布入口检查 `authority_active`，没有规定客户端 cache 的单调更新。

### P1-5：M03 isolation 规模门禁没有验证它声称的性质

位置：

- `tools/ucn_cluster_sim.c:639-666`
- `CMakeLists.txt:392-398`

`m03_isolation=checked` 只是由 `--expect-m03-isolation` 参数决定后打印；程序没有逐 Cluster/逐 Group 验证 foreign Term 未影响本地 Authority。该模式还把收敛条件从 `heads == groups && members == expected` 放宽为 `heads >= groups && heads + members == alive_nodes`。

放宽“暂时允许额外隔离 Cluster”在 M03/M11 过渡期可以接受，但输出不能称为已证明 isolation。应增加实际 invariant，例如逐 Group 检查 Cluster ID/Head/Epoch 关系、禁止跨 Group authority mutation，并把 group=2/4/8 全部加入固定种子门禁。

### P2-1：Higher-authority 全局入口未覆盖 `STEPPING_DOWN`

`process_higher_authority()` 对 Head/Recovery Head、Member、Backup、Join Pending 有处理，但 `STEPPING_DOWN` 返回未处理。该窗口收到同簇更高 Term 时仍会继续加入旧 pending Head，之后再等待新的 offer 修正。安全上旧 Head已停止普通 Authority 发送，主要是确定性收敛和时延问题，但与“任意 Active Phase 统一入口”的任务措辞不完全一致。

### P2-2：当前审计对象尚未形成可复现提交

M03 主体仍是未提交工作区修改；理想架构说明文档当前也是未跟踪文件。审计结论无法绑定单一完整 Hash，CI 与后续审计也无法稳定复现。M03 修复完成后应先形成候选提交，再做最终签字。

## 4. 当前验证结果

| 门禁 | 结果 | 备注 |
|---|---|---|
| MSVC Debug build：`ucn_cluster_sim` + `ucn_tests` | PASS | 旧 `C2099` 不再出现；仍有既有 C4819 编码警告。 |
| MSVC Debug CTest | 12/12 PASS | 默认 fast impaired 使用 group=8。 |
| Windows GCC CTest | 12/12 PASS | 默认矩阵通过。 |
| Lite CTest | 12/12 PASS | 当前缓存构建通过。 |
| Nano CTest | 12/12 PASS | 当前缓存构建通过。 |
| WSL ASan/UBSan | 22/22 PASS | 未发现 sanitizer 报错。 |
| WSL `-fanalyzer` | 22/22 PASS | 当前构建/测试通过。 |
| fast impaired, group=8 | PASS | 8 Head / 56 Member。 |
| fast impaired, group=4 | PASS | 16 Head / 48 Member。 |
| fast impaired, group=2 | **FAIL** | `ucn_cluster_backup.c:303/304` Phase 断言。 |

注意：测试通过证明的是已执行配置；它不能覆盖未执行的 group=2，也不能替代跨簇 Term、原子承诺和 Persistence 的协议证明。

## 5. 与理想 UCN 架构的映射

| 理想架构要求 | 当前方向 | 当前状态 |
|---|---|---|
| Cluster 只负责 Control Plane | 符合 | 未发现 Cluster 接管普通 Routing/Data Plane。 |
| Extended -> Core，Core 不反向依赖 | 符合 | `ucn_cluster` 只链接 `ucn_core`；Core 未 include 私有 Cluster 头。 |
| Cluster 不依赖 Driver/HAL/SDK | 符合 | 静态扫描无相关依赖。 |
| Unique Phase | 朝目标推进 | 显式 Phase/Transition 已建立，但 group=2 仍出现 Shadow/Legacy 不一致。 |
| Stable Epoch | 部分符合 | comparator 已有；状态仍是三个散写标量。 |
| Head Quorum/Fence | 尚未实现 | M08 TODO，当前 conflict wait 只是保守过渡。 |
| Committed/Joint Membership | 尚未实现 | M06/M07 TODO。 |
| Backup Epoch/Atomic Snapshot | 尚未实现 | M09 TODO，现有镜像仍是单表/刷新风险模型。 |
| Majority Takeover/Persistent Vote | 尚未实现 | M04/M10 TODO。 |
| Recovery Lineage | 尚未实现 | M12 TODO，M03 只有新 ID Provider。 |
| Persistence | 尚未实现 | M04 TODO，当前历史/round 掉电丢失。 |
| Capability Negotiation | 尚未实现 | M05 TODO。 |
| MCU 细粒度裁剪 | 计划缺口 | 只有 whole-Cluster OFF；per-module trim 被 DEFER，但没有明确收口任务。 |
| Hierarchical Federation/Directory | 部分覆盖 | M08/M12 有接口任务，但无完整层次化 Federation/Domain 验收，Locator cache 仍有回滚问题。 |

## 6. 154 项任务表能补全什么、不能补全什么

### 6.1 能补全的内容

严格完成 M00-M14 后，任务表能够覆盖 Cluster Target FSM v2 的主要正确性主链：

```text
Unique Phase
-> Epoch / same-vs-foreign classification
-> Persistence
-> Wire v4 / Capability
-> Provisional Member
-> Joint Config
-> Authority / Quorum / Fence
-> Atomic Backup Snapshot
-> Majority Takeover
-> Merge / Handover
-> RecoveryLineage
-> Rekey / Tombstone / No-wrap
-> Property / Scale / Hardware release gate
```

这条链的依赖顺序是合理的，尤其是“先 Persistence/Config，再启用 Quorum/Takeover”，能够避免在错误 denominator 上建立貌似安全的多数派。

### 6.2 不能自动补全的整套 UCN 内容

以下属于《理想协议架构》中的全局能力，不应假定由 Cluster 154 项自动完成：

1. Single Protocol Owner 从 ISR、Driver callback、Timer、Application 到 Event Queue 的端到端强制与静态/运行时门禁；
2. 普通 Data Plane 永不经 Head 中心化的跨 Endpoint/Routing/Path 验收；
3. Core Neighbor 与 Cluster Member 在所有 API、事件和生命周期上的系统级契约测试；
4. Member-only、Backup-capable、Head-capable、Full Cluster 的编译期/存储期真实资源裁剪；
5. 多 Cluster -> Federation -> Domain/Gateway 的层次化目录、Locator 单调性、跨域故障与规模验收；
6. 明确的故障/威胁模型：协议目前主要按 crash/partition/replay 设计，是否容忍已认证但 Byzantine/被攻陷节点没有规范声明；
7. Core Security 的真实 Identity、Join Authentication、Control Authentication、AEAD/Key Provider 与 Cluster persistence/rekey 的产品级组合验收；
8. UART/CAN/ESP-NOW 之外的产品 Port，以及 RTOS 调度、Flash 磨损、长期 soak、时钟回绕和真实干扰环境验证。

因此，任务表完成后推荐使用准确声明：

> “UCN Extended Cluster Target FSM v2 已完成并通过定义的故障模型、资源与实机门禁。”

不应直接声明：

> “整个 UCN 理想协议架构已全部完成且在所有介质/威胁模型下健全。”

## 7. 建议补充到任务体系的架构闭环工作流

建议在 CLV2-154 项之外新增独立的 `UCN-ARCH` 收口表：

| 建议任务 | 目标 |
|---|---|
| `ARCH-01 Fault/Threat Model` | 明确 crash、partition、replay、storage fault、Byzantine 的支持/不支持边界。 |
| `ARCH-02 Protocol Owner Gate` | 验证 ISR/Callback 不能直接修改 Core/Cluster；所有写经 bounded event queue/Owner。 |
| `ARCH-03 Data Plane Independence` | 证明普通 Endpoint/Q0/Q1 数据不要求 Head 在线、不强制经 Head 中继。 |
| `ARCH-04 Capability/Resource Profiles` | 建立 Member-only/Backup/Head/Full 的编译、RAM/Flash/栈和能力协商矩阵。 |
| `ARCH-05 Federation Hierarchy` | 多 Cluster/多 Federation/Domain/Gateway 的 Locator 单调性、Authority、分区恢复和规模测试。 |
| `ARCH-06 Security Composition` | Core Security 与 Cluster Wire/Persistence/Rekey 的密钥生命周期、重启和降级策略。 |
| `ARCH-07 Product/Soak Gate` | 真实 RTOS + UART/CAN/ESP-NOW 等介质，多轮掉电、干扰、长稳和时钟回绕。 |

## 8. 建议修复顺序

在允许进入 M04 前：

1. 修复并锁定 fast impaired `group=2` 断言；把 group=2/4/8 都加入 CTest。
2. 删除 `RECOVERY_HEAD` foreign `HEAD_TAKEOVER` 的跨簇 Term 比较，改为明确的 Stable Authority/lineage 证明。
3. 让所有接收侧安全 serial 使用 checked compare，拒绝阈值外、MAX 和回绕值。
4. 为 `BACKUP_ASSIGN` 增加精确 Epoch/tx 绑定，并让 Transition 失败路径全字段无污染。
5. 明确 03-02 是“只读访问器阶段”还是“真实 ActiveEpoch 状态收拢”；若保留分阶段，任务表不得写成全部完成。
6. 让模拟器真正计算 isolation invariant，不再打印由参数决定的 `checked`。
7. 修复 Federation locator cache 单调更新，并把它加入后续 Authority/Directory 门禁。

完成以上并把工作区固化为候选提交后，再重新执行 MSVC/GCC/ASan/Analyzer、Full/Lite/Nano、group=2/4/8 fixed-seed 回归，M03 才具备签字条件。

