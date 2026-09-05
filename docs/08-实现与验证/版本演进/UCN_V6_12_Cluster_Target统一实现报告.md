# UCN V6-12 Cluster Target 统一实现报告

## 1. 范围与架构边界

V6-12 新增独立 `ucn_v6_cluster` archive，把 Cluster 权威状态建立在 v6 Identity、Wire、
Security、Capability、Route 与 Transfer 基座之上。它不链接 Realtime，也不复用 v5 的
Cluster v3/v4 双栈、Mixed Version、Legacy bridge 或 Record。默认产品未启用
`UCN_BUILD_V6_EXPERIMENTAL` 时，`ucn_core` 仍可独立构建且没有任何 v6 符号。

本小节完成的是统一 v6 软件模型、持久化合同和 Host 对抗门禁；真实 Flash 掉电、网络分区、
多 MCU 和跨簇物理传输仍由 V6-13/14 验证，不能用 Host Fake 冒充。

## 2. 唯一 Record 与持久化顺序

Cluster Record v1 固定为 4096 B，所有字段显式按网络序编码，末尾使用 CRC32C。Record 只表达
最终 v6 状态：Record/transaction high-water、boot incarnation、Active/Max Epoch、Stable/
Joint Config、Backup、Last Vote、Transition Proof、Tombstone 和 Authority Fence。旧 Magic、
旧 Schema、非规范空字段、非法 serial、脏 padding 或坏 CRC 一律拒绝，没有迁移和降级解析。

每次持久化严格执行：

```text
validate next state
  -> encode into Owner record_work
  -> enter shared callback gate
  -> provider.submit()
  -> provider.load()
  -> leave gate
  -> exact raw-byte comparison
  -> install durable snapshot
```

Provider 假成功、回读不同、回调重入和 generation 耗尽都会失败关闭并撤销 Authority。启动时
先加载唯一 v6 Record，再持久推进 boot incarnation，完成回读证明后才向调用方返回 Owner。

## 3. 成员、Quorum 与 Authority

成员记录固定容量，且只接收 V6-07 已完成 Hop 认证、E2E 认证和 Endpoint ACL 的结果。认证
Principal、Ingress Peer Session、Binding 与 V6-06 Capability Cache 必须完全一致，Capability
还必须声明 Cluster Feature。租约使用绝对半开 Deadline，过期成员不再贡献 quorum。

Authority 的必要条件同时包括：

- 持久角色为 Head，且 Active Epoch 的 Head Identity 就是本机；
- Authority Fence 未置位，持久化 Provider 未故障；
- Stable Config 的活跃 voter 达到多数；
- Joint 阶段同时满足旧 Config 与新 Config 的两个多数。

任何成员租约过期、配置切换或持久化故障都在继续产生权威副作用前重新计算，不能依赖陈旧
`authority_active` 缓存。

## 4. Joint Config 与 Backup

新配置必须严格使用下一个 Config ID 与 Generation，Voter 顺序规范化且无重复。Prepare 先把
完整 C_new、txid 和阶段写入 Record；若配置要求 Backup，则 Commit 前必须收到当前指定 Backup
对同一 `txid + config_id + config_generation` 的认证 ACK。Commit 同时要求旧、新 voter set 的
实时 quorum，随后原子替换 Stable Config。Abort 也绑定完整 C_new，错误或重放事务零写拒绝。

Backup Assignment 精确绑定 Principal、Binding 和单调 generation。Voter 或 Capability 不满足
要求时不能成为 Backup；新 Config 移除 Backup 时 Commit 会一并清除其持久状态。

## 5. Takeover、Handover、Recovery 与 Rekey

所有权威转换都保存完整 `old_epoch + target_epoch + target_config + txid`，而不是只存几个标签：

- Takeover：仅已持久指定的本机 Backup、且旧 Head 租约已过期时启动；Vote 精确绑定完整事务，
  达到目标多数后才持久化目标 Epoch。
- Handover：旧 Head 必须持有当前 Authority；同簇目标 Term 必须精确加一，跨簇目标从 Term 1
  开始；目标 Head 的 READY 必须认证并匹配完整事务，Commit 后旧 Head永久 Fence。
- Recovery：只有旧 Authority 不存在且旧 Head 租约已过期时启动；新 Cluster ID/Term 1、完整
  Vote proof 与 quorum 持久化后才恢复 Head。
- Rekey：当前 Head 必须仍有实时 quorum；新 Cluster ID 不得命中任何退休 lineage，Commit
  原子写入 Tombstone，并把新 Cluster 的 Term/Config Generation 从 1 开始。

跨 Cluster Handover、Recovery 和 Rekey 均保留 `retired -> replacement` Tombstone；删除当前
状态不会释放历史 ID，表满时失败关闭。

## 6. Directory、Tunnel 与跨簇 Transfer

Directory 条目只接受认证远端 Head，并要求 Epoch、Route/Path Generation 与 Deadline 合法；
同远端 Cluster 的 Term 只能单调前进。Tunnel 只能由当前有 Authority 的 Head 安装，精确保存
Route Domain、Path Capability、远端 Cluster 和 Deadline。复制 Tunnel 时会重新检查过期和
代际，然后导出 V6-10 Transfer Path，因此跨簇传输不能绕开 Session、Capability 和 MTU 预算。

Directory/Tunnel 均使用编译期固定槽，表满不驱逐已有安全状态；`step(now)` 负责半开边界到期
清理并重新计算 Authority。

## 7. MCU 资源与栈自审

默认配置提供 16 Member、16 Voter、8 Tombstone、16 Directory 和 8 Tunnel，Cluster Owner
编译期预算为 22272 B。两份 4096 B Record buffer 与 durable/staging Snapshot 都在调用方提供
的 Owner Storage 中，不在任务栈上临时分配。

事务入口原先会在栈上复制约 2 KB Snapshot；自审已统一改为 Owner staging。Record decoder
要求调用方提供独立 scratch，从而同时维持“失败不改 output”和小栈合同。GCC
`-fstack-usage` 观测的最大公共函数静态栈帧为 208 B，最大内部 helper 为 624 B。

## 8. 分项自审

| 小节 | 自审结论 |
|---|---|
| 12-01 Record/Codec | 唯一 Schema、CRC、canonical、旧 Schema 拒绝、scratch 与输出不写回已覆盖 |
| 12-02 Persistence | submit/load/原始字节回读、boot incarnation、callback gate 与故障 Fence 已覆盖 |
| 12-03 Membership/Authority | Security+Capability 绑定、租约、Stable/Joint quorum 和陈旧 Authority 预检已覆盖 |
| 12-04 Joint/Backup | next serial、完整 C_new、精确 Backup ACK、双 quorum Commit 与 Abort 已覆盖 |
| 12-05 Takeover | Head 租约、指定 Backup、完整 Vote、历史重放和 durable successor 已覆盖 |
| 12-06 Handover | 同簇/跨簇规则、READY 身份、完整目标 Config、旧 Head Fence 和 Tombstone 已覆盖 |
| 12-07 Recovery/Rekey | 旧 Head 失效、quorum、退休 ID 拒绝、transaction high-water 与 lineage 已覆盖 |
| 12-08 Directory/Tunnel | 认证 Head、代际单调、固定槽、过期和 Transfer Path 导出已覆盖 |
| 12-09 资源/隔离 | 32 voter 上限、Owner staging、小栈、无 Realtime 链接和 default-OFF Core 已覆盖 |

V6-15 第三轮交叉自审补充了两类 Record/API 边界：

- Tunnel 重放改为逐字段语义相等，不以包含 padding 的整个 C 结构体 `memcmp` 判定；相同语义、
  不同 padding 的输入保持幂等，真实字段冲突仍按 replay 拒绝；
- Record Generation、transaction high-water、所有 Cluster txid 和公开 Role/Phase/Control Kind
  都执行完整 no-wrap 或无符号枚举合法域检查；到达阈值后持久化与启动推进失败关闭。

## 9. 验证结果

| 门禁 | 结果 |
|---|---|
| Windows GCC Full | 71/71 |
| 最大 32 Member/32 Voter 配置 | Cluster 1/1 |
| WSL ASan/UBSan | Config + Cluster 2/2 |
| WSL `-fanalyzer -Werror` | Cluster 1/1 |
| GCC `-fstack-usage` | 最大公共 208 B；最大内部 624 B |
| default-OFF `ucn_core` v6 symbol | 0 |
| Cluster archive Realtime unresolved symbol | 0 |
| `git diff --check` | 无空白错误，仅行尾转换提示 |

当前本机没有可用 MSVC，因此本报告不复制旧结果冒充本轮证明；MSVC、Clang、全部 Profile/
Feature 矩阵统一归 V6-14 重跑。

当前状态：`V6-12 SOFTWARE IMPLEMENTED / SELF-REVIEW PASS / FINAL EXTERNAL REVIEW DEFERRED`。

## 10. 尚未获得的证明

- 真实 Flash 双槽、撕裂写与断电重启；
- 多 MCU 分区、恢复、Joint、Takeover 与 Handover；
- 真实跨簇 Tunnel/Transfer、链路断开和重连；
- 参考产品 RAM、任务栈、CPU 与功耗；
- 独立外部审计。

这些项目必须由 V6-13/14 的对应原始证据关闭，不能由当前 Host 测试推导。

## 11. V6X-A05、A06、A08 外审整改补充

Cluster ACK、Vote、Ready、Directory/Tunnel 等状态推进不再接受与 Wire Payload 分离的调用方
结构。Control 和 Directory 都有固定 canonical codec；入口从 V6-07 Security Open Result 的
已认证 Payload 解码，再匹配 Type、Opcode、Epoch、Config、Principal、Binding、Session 和
Capability。修改 Payload 任一语义字节都会使认证语义入口失败，不能用未认证旁路替换。

Cluster Store 增加独立 rollback witness 和单调 `record_generation`。所有状态提交执行
`witness-first → record submit → record reload → canonical exact compare`；启动时 witness 与
Record 任一方向不一致均进入 Fault，不静默采用旧槽。Host Fake 只证明软件调用顺序，真实双槽
介质与断电仍属于硬件 HOLD。

成员、Authority、Directory 与 Tunnel/Path 导出在使用 Capability 前均重新检查 Discovery 和
Capability 半开 Deadline。过期状态不会等待后台清扫才能失权，`now == deadline` 已有定向反例。
