# UCN V6S IMPL-08C Cluster 本地控制核心实施及全体自审报告

> 日期：2026-09-24
> 分支：`v6-simplified-rust`
> 结论：`LOCAL CLUSTER CONTROL CORE IMPLEMENTED / SELF-REVIEW PASS / AUDIT HOLD`

## 1. 目标、范围与未完成边界

本阶段把 Cluster 的本地控制核心从空骨架推进为可执行的私有 C 模型。目标是在固定内存中
证明：成员事实不会跨 Binding/Session/Capability 代际复用；Authority 每次使用前都会以当前
时间重新判断；Stable/Joint Config、Backup READY、Takeover、Recovery 与 Planned Handover
都必须先得到精确耐久证明，不能靠历史 bitmap 或 RAM 标志直接获得写权限。

本阶段没有实现跨簇 Directory/Tunnel、跨簇 Merge、Cluster Rekey 与 durable Tombstone。这些
能力不能靠两个普通 Persistence Domain 的顺序提交伪装成原子操作，后续必须在 IMPL-08D
单独冻结 lineage、故障恢复和多 Domain 原子性合同。因此本报告只签署“本地簇控制核心”，
不签署完整 Cluster 管理面。

## 2. 模块边界与数据流

```text
Identity / Security / Capability / Flow immutable facts
                         │
                         ▼
                    Coordinator
                         │ typed facts / requirement / proof
                         ▼
                Cluster Owner（单写）
              ┌──────────┼───────────┐
              │          │           │
        Member/Config  Authority   Transition
              │          │           │
              └──── canonical pending state ────┐
                                                 │
                         Persistence requirement │
                    Coordinator / Foundation ◄───┘
                                  │ exact reload proof
                                  └──────────────► publish
```

`ucn_v6s_cluster` 只链接 `ucn_common`。它不持有其他业务 Owner 指针，不直接调用
Persistence Provider，不链接 Realtime、Group、Route、Security、Adapter 或 Coordinator。
所有外部输入都是调用方复制的不可变事实，所有持久化交互都通过 typed Requirement/Proof。

## 3. IMPL-08C-00：固定容量、Feature 与物理解耦

| Profile | Config members | Runtime members | Owner bytes | Record bytes |
| --- | ---: | ---: | ---: | ---: |
| Nano | 1 | 4 | 1392 | 256 |
| Lite | 3 | 8 | 2352 | 480 |
| Full | 7 | 16 | 4272 | 928 |

Owner、成员槽、Config、Vote evidence、Record staging 与摘要 workspace 都由调用方静态提供，
模块不使用动态内存。`UCN_FEATURE_CLUSTER=OFF` 时不生成详细 Cluster archive、详细测试、
Persistence 集成或栈门禁；统一 scaffold 仍只是零业务状态的架构占位。

分项自审：通过。

## 4. IMPL-08C-01：成员事实与 Authority preflight

成员事实精确绑定 Principal、Address Binding、Peer Session、Capability Generation、Link
Generation、角色、租约和认证状态。重新准入、Session/Binding/Capability/Link 变化或租约失效
都会使相应 Vote evidence 失效，防止成员恢复后历史票重新复活。

Authority 不是缓存布尔值。每次调用都用当前 `now_us` 刷新成员 Lease，并按当前 Stable Config
或 Joint 的 `C_old + C_new` 双 quorum 重新计算。候选新 Config 在 durable JOINT/Commit 前不获得
Authority；旧 Head 在 PREPARED 只按旧 Config preflight，在 JOINT 按新旧双 quorum preflight。
过期、缺 quorum、Head 不属于当前 voter 集或阶段不允许写入时，preflight 直接失败。

分项自审：通过。

## 5. IMPL-08C-02：Stable/Joint Config 生命周期

Config 状态按以下单向流程推进：

```text
STABLE
  → CONFIG_PREPARED durable
  → JOINT durable
  → new STABLE durable

CONFIG_PREPARED / JOINT
  → ABORT durable
  → original STABLE
```

Config member 使用 canonical Principal 排序并拒绝重复、非法 Head、空 voter 集和越容量。Prepare、
进入 Joint、Commit 与 Abort 都只写 `pending_state` 并输出 Persistence Requirement；只有精确
Foundation Proof 被接受后才替换 live state。不存在“PREPARED + RAM quorum 直接 Commit”的旁路。

分项自审：通过。

## 6. IMPL-08C-03：Backup coverage 与 durable READY

Backup READY 精确绑定 assignment generation、Config、完整 protected-voter bitmap 和非零
snapshot digest。READY 不是调用成功后立即发布的 RAM 标志，而是 canonical Cluster Record
中的耐久状态；缺少任一受保护 voter、digest 为空或上下文不一致都会在写 Record 前拒绝。

本阶段不包含 Snapshot 字节流、增量同步或 Backup mirror 状态机。snapshot digest 只绑定由上层
Coordinator 已验证的不可变 Snapshot 事实，不能单独证明实际镜像已经完成。

Takeover 发布新 Epoch 后会清除旧 Backup assignment、coverage 和 READY，防止旧簇快照跨 Epoch
继承。任何 READY、Config 或 Epoch 不一致都会被 Record/state validator 拒绝。

分项自审：通过。自审期间发现并关闭了“Backup READY 只存在于 RAM”的初稿缺陷。

## 7. IMPL-08C-04：统一 Transition

Takeover、Recovery 与 Planned Handover 共用一个 Transition 容器和一套 Vote evidence。每票保存
voter Principal、Binding Generation、Session Generation、Capability Generation、Vote ID 和
canonical digest；Commit 时按当前成员事实逐票复核，不接受 bitmap-only 证明。

`EPOCH_DURABLE` 是单向终态。迟到 vote、unreachable 或 step 不得把它改回 quorum/aborted。
Takeover/Recovery 只能提交本地目标 Head；Planned Handover 在目标 READY 后先持久化本地 Fence，
Proof 接受后仍必须取得同 digest 的新鲜目标证明，才能继续对外动作。这样不会把持久化前的
freshness 跨 I/O 时间窗口复用。

分项自审：通过。自审期间发现并关闭了 absent vote 被零值误判、Takeover 保留旧 Backup READY、
以及 Handover 复用 pre-persist freshness 三类问题。

## 8. IMPL-08C-05：Record 与 Persistence Foundation

Cluster Record 为固定 big-endian 布局：

```text
144 B header
+ 2 × Config-member-count × 24 B
+ 2 × Config-member-count × 32 B vote evidence
```

Record 包含当前/候选 Config、Epoch、阶段、Backup assignment/coverage/snapshot digest、Transition、Vote evidence、Transaction
high-water 和必要 fence。业务 canonical body digest 与 Foundation published record digest
分开保存。Proof 精确核对 Persistence Handle/Owner、Domain、Transaction、Record/Witness
Generation、Runtime/Caller、Schema、Operation、正文长度、published digest 和半开 Deadline。

真实 Foundation 集成测试完成 `prepare → submit → marker/witness → reload proof → publish`，并覆盖
重启 reload。损坏 magic、非法 Backup READY、非法 Transition、错误 digest 和错误 proof 都在
发布前失败，且 import 的 live/pending state 与输出哨兵保持不变。

分项自审：通过。自审期间发现并关闭了 malformed import 污染 pending state 的问题。

## 9. IMPL-08C-06：资源与验证矩阵

| 环境 | 结果 |
| --- | --- |
| Windows GCC Full/Lite/Nano 全量 | 各 88/88 |
| Windows GCC Cluster Feature-OFF | 80/80 |
| Windows GCC Persistence 集成全量 | 98/98 |
| MSVC 19.29 Release Cluster 定向 | 4/4 |
| WSL GCC ASan/UBSan Cluster 定向 | 5/5 |
| WSL GCC `-fanalyzer -Werror` Cluster 定向 | 5/5 |
| WSL Clang 18 Release Cluster 定向 | 5/5 |
| WSL GCC TSan non-PIE 双线程 | writer 2000 / reader 2000 |

TSan 可执行文件在 DrvFS `/mnt/e` 直接启动会触发运行时自身的 `unexpected memory mapping`；复制到
WSL ext4 `/tmp` 后通过，并确认 ELF 类型为 non-PIE `EXEC`。因此报告同时保留运行环境限制，
不把 DrvFS 启动失败写成测试通过。

GCC 扫描 84 个 Cluster 函数，最大单函数栈为 `160 B`；Nano 最长静态调用链为 `560 B`，低于
`768 B` 门限。该数字不包含未来 Coordinator、密码 Provider、Driver 与 RTOS 调用链，目标任务
栈仍需实机水位。

## 10. 三轮全体自审

第一轮按 `Member → Config → Authority → Backup → Transition → Persistence → publish` 正向追踪，
核对每个副作用是否在当前 Lease/quorum、容量、代际和 durable proof 之后发生。

第二轮从错误输入逆向追踪：过期成员、重新准入、错 Binding/Session/Capability、Stable/Joint
quorum 变化、错 Config、错 Backup coverage/snapshot digest、迟到 vote、错 Handover digest、损坏 Record、错
Foundation Proof、Deadline、表满和句柄重放。确认失败不发布半 Config、半 Epoch 或伪 Authority。

第三轮核对架构和资源边界：Cluster 只链接 Common；Feature OFF 无详细 archive/测试；公共 archive
无私有 Cluster 符号；Owner 和 Record 尺寸固定；无动态内存；边界、archive、栈、调用链、并发、
多 Profile 和多工具链门禁通过。

当前未发现本阶段范围内已知软件 P0/P1；结论仍是自审，不替代独立外审。

## 11. 未放行边界与下一步

- 跨簇 Directory、Directory Tunnel 和跨簇 Merge；
- Cluster Rekey、lineage 与 durable Tombstone；
- Backup Snapshot 传输、增量同步和 mirror 状态机；
- 多 Persistence Domain 原子提交与断电恢复合同；
- 生产 Cluster 控制 Wire、公开 API、Coordinator/Runtime 接线；
- 真实 Security/Capability/Flow Provider、真实 Flash/Witness 与物理掉电；
- MCU RAM/栈/ISR/SMP、物理 Bearer、长稳、性能、功耗和故障注入；
- C 与 Rust 的 Cluster 差分互操作。

因此本阶段只可标记 `LOCAL CLUSTER CONTROL CORE IMPLEMENTED / SELF-REVIEW PASS /
AUDIT HOLD`。下一项应先完成 IMPL-08D 的跨簇与 lineage 合同冻结，而不是直接把本模块接入
公共 Runtime 或宣称 C 简化版 Cluster 已全部完成。
