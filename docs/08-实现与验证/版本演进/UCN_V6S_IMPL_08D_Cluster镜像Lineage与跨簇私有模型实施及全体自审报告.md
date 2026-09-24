# UCN V6 简化版 C IMPL-08D Cluster 镜像、Lineage 与跨簇私有模型实施及全体自审报告

> 日期：2026-09-24
> 状态：`IMPLEMENTED / SELF-REVIEW PASS / EXTERNAL REVIEW HOLD`
> 证据范围：C 简化版私有 Host Cluster 模型、真实 Persistence Foundation 集成和静态资源门禁。
> 非证据范围：生产控制 Wire、公共 Runtime、真实多节点 Merge Commit、Flash 断电、MCU、物理多跳和长稳。

## 1. 本阶段解决的问题

IMPL-08C 已经能维护单个簇内的成员、Config、Authority、Backup READY 和 Epoch Transition，
但它把 Snapshot digest 当作外部输入，且没有解释旧 Cluster ID 如何永久退休、远端 Cluster
Authority 如何进入本地目录，以及跨簇 Tunnel 如何始终绑定当前 Flow。IMPL-08D 只补这些
私有状态机缺口，不提前发明生产 Wire，也不把两个独立 Persistence Provider 的提交伪装成原子事务。

本阶段形成四条实现链：

```text
Backup Snapshot BEGIN/MEMBER/END
    -> inactive canonical buffer
    -> digest/coverage/exact sequence verified
    -> atomic active-buffer swap
    -> durable BACKUP_READY
    -> Takeover preflight rechecks exact current mirror

Current Cluster Authority
    -> checked-next Cluster ID / Config generation
    -> typed REKEY requirement
    -> exact Persistence proof
    -> publish new Epoch + durable retired-cluster high-water

Current Cluster Authority + fresh target READY
    -> typed MERGE_RETIRE requirement
    -> durable local Tombstone/Fence
    -> require a fresh target continuation proof
    -> local side may continue only as retired/fenced lineage

Authenticated remote Authority summary
    -> fixed Directory slot
    -> exact current Flow fact
    -> fixed Tunnel slot
    -> every-use local Authority + Directory + Flow preflight
```

## 2. 模块与依赖边界

`ucn_v6s_cluster` 仍是私有 `EXCLUDE_FROM_ALL` target，内部实现位于 `src/cluster/`，内部合同位于
`src/internal/ucn_cluster.h`。它只直接链接 `ucn_common`，不链接 Realtime、Group、Route、Flow、
Security、Adapter、Coordinator 或 Persistence。Directory、Flow、Authority 和 Persistence 结果
均以不可变 typed fact/requirement/proof 表达；真实 Foundation 只在集成测试可执行文件中由测试
Coordinator 连接。

Cluster 关闭时不生成详细 Cluster archive、Owner 状态或详细测试；内部头不安装，符号不进入
`UCN::simplified`。因此本阶段没有扩大用户 API，也没有让可选 Cluster 反向污染最小通信核心。

## 3. Backup Snapshot 与 mirror

Owner 内含两个固定 canonical buffer。一个是 active mirror，另一个是 building buffer；没有堆分配，
也不保留与 canonical bytes 重复的 typed 成员数组。

状态机合同如下：

1. `BEGIN` 精确绑定当前 Epoch、Stable/Joint Config、Backup assignment、protected-voter coverage、
   Snapshot sequence 和绝对 Deadline；
2. `MEMBER` 只能按 `1..N` 顺序进入，且成员 Principal、Binding、flags 与目标 Config 完全一致；
3. `END` 只在成员数、canonical length、coverage 和 SHA-256/128 digest 全部一致时交换 active index；
4. 错序、重放、错 digest、超时和中途失败只清 building buffer，旧 active mirror 继续有效；
5. assignment 和 sequence 使用 checked-next；零起点只允许首个合法镜像，不允许回绕；
6. `BACKUP_READY` 只能由 active mirror 生成，不再接受调用方任意提供的非零 digest；
7. Config、Epoch、Rekey、Merge 或新 Assignment durable 后，旧 mirror/READY/Tunnel 等派生缓存撤销。

Takeover 提交前调用 `p_snapshot_current()`，重新比较 active mirror 与当前 Epoch、Config、Assignment、
Coverage 和 digest。这样旧镜像即使仍在 RAM 中，也不能为新代际提供接管证明。

## 4. Rekey、Lineage 与 Tombstone

Rekey 不是修改一个 Cluster ID 字段，而是持久化的 lineage transition：

- 新 Cluster ID 必须为当前 Cluster ID 的 checked-next；
- 新 Epoch 固定从 Term 1 开始，并保持当前 Head Principal/Binding；
- 成员集合不变，Config ID 和 Config generation 都使用 checked-next；
- `lineage_generation` 与 `retired_cluster_high_water` 写入 Cluster Record v2；
- Persistence proof 成功前不发布新 Epoch；成功后旧 Cluster ID 永久退休；
- ID、Config 或 Lineage 任何耗尽、回退、零值或损坏都失败关闭。

删除 RAM 槽不会降低 retired high-water，也不会允许扫描历史空洞重新分配旧 ID。Record 损坏时
不能靠默认值继续创建新 lineage。

## 5. Merge 的诚实原子性边界

本阶段实现的是**本地簇的 durable retirement/Fence**，不是一个跨两个存储域的分布式原子提交。
发起本地 retirement 前必须同时满足：

- 本地当前 Authority preflight 成功；
- target Cluster ID 严格更新，不能用跨簇 Term 数值比较决定优先级；
- target Epoch 的 Head 是 target Config 的精确 voter；
- target READY 认证、完整、未过期，且绑定 target Epoch/Config；
- typed requirement 经当前 Foundation Domain/Witness durable。

proof 成功后本地永久 Fence，并记录 retired high-water。由于 Persistence I/O 期间的新鲜事实不能
自动沿用，继续进入目标簇前必须重新取得 target continuation proof。目标簇是否创建、是否接纳
成员、目标端是否 durable，属于独立的多节点协议事务；本实现不声称这三件事已由一个本地函数完成。

## 6. Directory 与 Flow-bound Tunnel

Directory 保存的是受认证的远端 Authority 摘要，而不是远端 Authority 本身。每项固定绑定：

- remote Epoch、Head Principal/Binding；
- remote Config ID/generation；
- Authority generation、origin sequence 和 authority digest；
- Source Session、Capability generation/digest；
- Route、Path、Link generation 与 ID；
- Authority、Capability、Flow 三个半开 Deadline；
- authenticated、quorum-verified、flow-active 三个前提。

相同 Authority generation 的更新不得替换 Head、Config 或 Authority digest；更高 generation
不得回退 remote Term/Config。表满时不驱逐仍有效项。读取 Directory 也要求本地当前 Authority，
避免已失权 Head 继续发布跨簇路由判断。

Tunnel slot 精确绑定 Directory origin sequence 和完整 Flow fact。每次使用前重新核对：本地
Authority、source/destination Cluster、双方 Principal/Binding/Session、Capability digest/generation、
Route/Path/Link generation、Path/Link ID 和 Deadline。任何一个依赖改变都使旧 Tunnel 失效；
revoke 只删除精确依赖项。Tunnel 不授予 Authority，也不绕过普通 Flow/Security 证明。

## 7. Persistence Record v2

Cluster Record v2 使用固定 big-endian canonical layout。144 B canonical body 依次包含：

- Magic、schema、phase、role；
- current/target Epoch；
- stable/target Config ID 和 generation；
- transition transaction/deadline/high-water；
- lineage generation、retired cluster high-water；
- Backup assignment、coverage、digest/proof；
- member counts、transition kind/phase、joint/fenced/backup-ready flags。

canonical body digest 与 Foundation 发布 Record digest 分离。typed requirement 和 exact proof
继续绑定 Domain、Foundation transaction、Record/Witness generation、Runtime/Owner、Schema、
Operation kind、Body bytes、Digest 和半开 Deadline。Rekey 与 Merge 通过真实 Fake Provider 的
marker/witness 状态机完成提交、reload 和恢复测试；失败 proof/import 不污染 live 或 pending state。

## 8. 定向测试与故障矩阵

新增/扩展测试覆盖：

- Snapshot 正常完成、错序、重复 sequence、错 digest、building timeout、旧 active 保留；
- 新 mirror 使旧 READY 失效，重新 durable 后才恢复接管资格；
- Cluster ID 跳号、回退、耗尽、连续两次 Rekey、旧缓存撤销；
- Merge 目标 Head 不在 Config、READY 错误/过期、durable 本地 Fence、错 continuation、fresh continuation；
- Directory 幂等、同代冲突、Head 替换、Term/Config 回退、过期、表满不驱逐；
- Tunnel 精确 Flow、Directory 更新后失效、输出哨兵不写回、精确 revoke；
- Record v2 偏移、损坏、Foundation Commit/reload、Witness；
- 双线程同时更新 Member/Directory，并读取 Authority/Directory；
- Feature OFF、archive 符号、依赖边界、单函数栈和静态调用链。

## 9. 验证结果

| 环境 | 结果 |
| --- | --- |
| Windows GCC Full Debug | `91/91` |
| Windows GCC Lite Debug | `91/91` |
| Windows GCC Nano Debug | `91/91` |
| Windows GCC Cluster Feature-OFF | `80/80` |
| Windows GCC Persistence ON 全量 | `101/101` |
| MSVC 19.29 Release Cluster 定向 | `7/7` |
| WSL GCC ASan/UBSan Cluster 定向 | `8/8` |
| WSL GCC `-fanalyzer -Werror` Cluster 定向 | `8/8` |
| WSL Clang 18 `-Werror` Cluster 定向 | `8/8` |
| WSL GCC TSan non-PIE | member/directory writer 2000；authority/directory reader 2000 |
| Boundary gate | `sources=50 / internal_headers=21` |

TSan 产物从 DrvFS 复制到 WSL ext4 `/tmp`，以 `setarch x86_64 -R` 启动；直接从 `/mnt/e`
运行仍会受 TSan runtime `unexpected memory mapping` 影响。关闭 ASLR不是关闭 TSan，执行中未报告
data race。

资源结果为：

| Profile | Cluster Owner | Config members | Runtime members | Record slot |
| --- | ---: | ---: | ---: | ---: |
| Nano | 2376 B | 1 | 4 | 256 B |
| Lite | 4072 B | 3 | 8 | 480 B |
| Full | 7464 B | 7 | 16 | 928 B |

Cluster 源码共扫描 124 个函数，最大单函数静态栈 `160 B`；Nano 最长静态调用链 `624 B`，
低于 `768 B` 门限。它们是 Host 编译期估算，不包含未来 Coordinator、密码 Provider、Driver、
RTOS 和中断栈，不能替代 MCU 水位实测。

## 10. 三轮全体自审

第一轮按正向数据流审查：`Member → Config → Authority → Snapshot → READY → Transition →
Persistence → publish`，以及 `Authority → Directory → Flow → Tunnel preflight`。确认所有权威副作用
都在当前时间、当前代际、当前 quorum、当前容量和 durable proof 之后发生。

第二轮按故障与恢复反向审查：错序/重复 Snapshot、错 digest、旧 mirror、重新准入、过期 Lease、
Config/Lineage 回退、损坏 Record、错 Witness、Directory 冲突、Flow 漂移、表满和 callback/锁并发。
确认失败不发布半 Snapshot、半 Epoch、伪 Authority、可复活 Tombstone 或未绑定 Tunnel。

第三轮按模块和发布边界审查：Cluster 只链接 Common；Feature OFF 不生成详细实现；内部符号不进入
公共 archive；普通通信和 Group/Realtime 不依赖 Cluster。逐项核对文档没有把 Host private model
写成生产 Wire、公共 API、MCU、Flash 原子性或跨簇业务已经可用。

## 11. 结论与下一步边界

```text
IMPL-08D                         = DONE / SELF-REVIEW PASS
Cluster private state-machine   = IMPL-08C + IMPL-08D IMPLEMENTED
External review                 = HOLD
Production Cluster integration  = NOT STARTED
```

下一阶段不能继续给私有 Owner 堆功能，而应单独设计生产接线：为控制消息冻结 Wire/Opcode/AAD，
由 Coordinator 串接 Security、Admission/Capability、Route/Flow、Persistence 与 Cluster，建立真实
多节点 target-side Merge transaction，并在 ESP32/物理 Bearer/真实 Flash 上验证掉电、分区、
接管、跨簇 Tunnel、资源水位和长稳。在这些证据齐备前，不创建生产发布结论。
