# UCN V6 简化版 IMPL-02 统一持久化基础实施与自审报告

> 状态：`DONE / EXTERNAL REVIEW GO（受限 Host 软件范围）`
>
> 适用分支：`v6-simplified`
>
> 本报告只证明 Host 软件中的 Persistence Foundation、Fake Provider 和故障模型；不构成真实
> Flash 原子性、掉电耐久性、硬件单调 Witness、MCU 栈水位或任何业务 Authority 的证明。

## 1. 实施目标与明确边界

IMPL-02 提供所有未来耐久业务模块共用的一套持久化基础。业务模块不再各自调用 Flash，也不再
各自实现双槽、代际、重载和 Provider callback 重入保护。统一基础负责：

- Durable Domain 与编译期 Manifest；
- 96 B canonical Record Envelope 与 16 B Commit Marker；
- CRC32C、BLAKE2s-128 Body Digest 与 Durable Manifest Digest；
- inactive-slot 写入、逐字节 readback、Marker 发布、独立 Witness 推进和最终 reload；
- 启动双槽恢复、唯一最新记录选择、允许的 witness 补推进和 anti-rollback 失败关闭；
- 同步/异步 Provider 共用的 exact token/phase continuation；
- 多 Domain 固定容量、公平推进、Deadline、取消、终态退休和 immutable Durability Proof；
- Coordinator typed requirement/event 适配。

本阶段不实现任何业务 Record Body，也不把 Persistence 接入 Dynamic Admission、Security
Session、Reliable/Transfer、Durable Operation、Group、Realtime 或 Cluster。Persistence Proof
只证明 Record 中列出的耐久字节，不能单独授予地址、Session、执行结果、时间或簇 Authority。

## 2. 实际模块和依赖

```text
Business Owner（未来阶段）
    │ immutable typed requirement
    ▼
Runtime Coordinator
    │ exact persistence request
    ▼
Persistence Coordinator Adapter     src/persistence/ucn_persistence_coordinator.c
    │
    ▼
Persistence Owner                   src/persistence/ucn_persistence.c
    ├── Record/Marker/Manifest Codec src/persistence/ucn_persistence_codec.c
    ├── caller-owned shared gate
    └── fixed storage and continuation
             │
             ▼
      caller-provided Storage Provider
```

物理依赖保持单向：`ucn_persistence` 只依赖公共产品类型和内部 checked 基元；
`ucn_persistence_coordinator` 才依赖共同 Coordinator。Provider 不解析 Body，业务 Owner 不持有
Provider 指针；同步完成也必须经过 Persistence Owner 状态机和 Coordinator event，不能直接变成
业务 promise。

公共 `ucn/ucn_persistence.h` 只暴露固定存储、Manifest、Provider、Gate、init/deinit 和摘要函数。
submit/step/query/proof/retire 都位于 `src/internal/ucn_persistence.h`，只允许 Runtime Coordinator
和内部 Owner 使用；`ucn/ucn_simplified.h` 不自动包含 Persistence 头。

## 3. 分项实施与逐项自审

### 3.1 IMPL-02-00：模块边界、Feature 和容量形状

完成内容：

- 新增独立 `ucn_persistence` archive 和内部 `ucn_persistence_coordinator` adapter；
- `UCN_FEATURE_PERSISTENCE=OFF` 时不编译、不安装 Persistence 头或 archive，也不创建其测试；
- Nano/Lite/Full 分别固定 2/4/8 个 Domain 和 256/512/1024 B 最大 Body；
- Owner、Gate 和 Manifest digest workspace 全部 caller-owned、编译期定容并强制对齐；
- 实施边界检查器精确登记 3 个 Persistence 源和 2 个内部头：Foundation 私有头不得包含
  Coordinator 类型，Coordinator Adapter 的私有状态单独存放；检查器同时拒绝旧 `ucn_v6_*`
  依赖、动态内存与越界 target 归属。

逐项自审：Persistence OFF 的简化通信不含 Provider、Storage 或 Persistence 符号；公共聚合头不
泄漏内部事务 API。结论：`PASS`。

### 3.2 IMPL-02-01：公共 ABI、Manifest、Provider 和共享 Gate

完成内容：

- 全部 DTO 使用 `struct_size + api_version`；结果码继续为固定 `int32_t`；
- Domain Key、Manifest Entry、Domain Binding、Provider Vtable、Completion 和 Witness View 均为
  固定 C99 数据合同；
- `ucn_persistence_manifest_digest()` 对排序后的完整 Manifest 计算 canonical BLAKE2s-128；
- init 在首次写 Owner Storage 前验证 Profile、Feature、Layout、Domain 唯一性、Schema、容量、
  Provider 能力、锁域、Gate、Manifest digest、全部范围和部分别名；
- Provider slot 必须不超过 `maximum_slot_bytes`，且是最小写/擦对齐的整数倍；
- `poll` 对严格同步 Provider 可为空；只要任一 begin 返回 PENDING 而 poll 为空，当前 Domain 立即
  Fault，不尝试猜测完成；
- Provider callback gate 由调用方持有并由所有进入同一 Provider 域的 Owner 共享，状态锁和 Gate
  锁必须不同；Gate 以有界引用计数绑定所有存活 Owner，引用未归零时不得 deinit/reinit；同一
  Gate 还统一分配域内全局唯一、不回绕的 64-bit I/O token，杜绝共享 Provider 的跨 Owner 碰撞。

逐项自审发现并关闭：Manifest digest workspace 曾可能与 Owner Storage 重叠；现在 init 前双向
拒绝，并以 Storage/out-owner/Provider 调用计数哨兵证明零写。另补写/擦 geometry 错位和同步
no-poll/异步 no-poll 三组可区分回归。结论：`PASS`。

### 3.3 IMPL-02-02：Record、Marker 和摘要 Codec

完成内容：

- Record Envelope 精确 96 B，所有多字节字段 big-endian；
- Body 紧随 Header，槽尾固定 16 B Commit Marker，未使用区域必须为 Provider erased value；
- Body CRC32C 用于随机损坏快检；BLAKE2s-128 绑定 Domain、Schema、Generation、Transaction、
  Operation、Body length 和完整 canonical Body；
- Header CRC32C 在自身字段清零后覆盖完整 Header；
- Manifest digest 绑定 protocol/layout/feature/profile 和排序后的全部 Domain Entry；
- Encoder/Decoder/Marker/Manifest hash 均使用 caller-owned workspace，避免大对象进入 MCU 栈；
- 所有输出在完整检查成功后一次写回，NULL workspace、workspace/output/payload 部分别名均拒绝。

验证逐字段破坏 Magic、Version、Header bytes、Domain、Schema、Flags、Digest Suite、长度、CRC、
Digest、Reserved、Marker 和 erased tail；raw fixture 与 semantic fixture 分离，避免编解码同错。
结论：`PASS`。

### 3.4 IMPL-02-03：Owner、exact I/O continuation 和统一 Provider 状态机

完成内容：

- 每次 I/O 由 shared gate 在调用 Provider 前预留回调域全局唯一、非零、不回绕的 64-bit token，并固定
  `{domain,phase,slot,exact_bytes}`；
- Owner 先发布 `call_active` 与 shared gate，再释放状态锁调用 Provider；
- 同步 completion 只写预留 latch，返回 Owner 后与异步 poll completion 走同一验证函数；
- completion 必须精确匹配 API、token、phase、slot、bytes、result、blob state 和 reserved；
- PENDING 可连续多次返回，Owner 保留同一 token/phase，不重启 begin、不分配新 token；
- callback 内递归 recovery/submit/step/query/deinit，以及共享 Gate 下另一个 Owner 的 Provider I/O
  均失败关闭；双线程由真实 mutex/try-gate 验证，不依赖普通静态指针或 volatile。
- Provider 返回后若状态锁重新获取失败，调用栈只使用解锁前冻结的 Gate 指针、Owner 身份、token
  和 phase 清理独立 callback gate，绝不在无锁状态下再次读取、修改或解锁 Owner；`call_active`
  保持为永久易失 Fence，结果输出不写回，后续只能由产品故障策略重启 Runtime。

逐项自审新增“连续两次 poll 仍为 PENDING，第三次完成”的反例，并验证 token/phase 在整个等待
期间逐字节不变。结论：`PASS`。

### 3.5 IMPL-02-04：单向提交、reload 和 Durability Proof

完成内容：

```text
READY
  → write inactive full slot
  → read back and compare every byte
  → publish exact commit marker
  → advance independent witness
  → reload witness + slot A + slot B
  → select unique current record
  → verify exact durable request identity
  → publish domain snapshot + immutable proof
```

- schema/domain/capacity/expected generation/expected digest/duplicate/conflict/deadline 全部在首次 I/O
  前验证；
- 相同 pending request 返回原 Handle；同 txid 不同有界 request 冲突且零 Provider I/O；
- 当前已持久化 transaction 的相同 durable identity 可无写入返回新 proof；
- Proof 只包含实际落盘或由当前 Owner/Witness 可证明的字段；已删除未落盘的
  `business_transition_digest` 与 `volatile_continuation`，避免把当前调用关联伪装成耐久事实；
- 当前重放的业务 transition reference、Deadline 和 continuation 由 Coordinator 重新绑定，业务
  Owner 在消费 Proof 前仍须重新验证 Lease/Authority/Policy/Config/Capability。

逐项自审以“相同耐久元组、改变本次业务摘要和 continuation”重放，确认 Provider write 为零且
返回 Proof 与原 Proof 逐字节一致。结论：`PASS`。

### 3.6 IMPL-02-05：双槽恢复、Witness 与域级 Fault

完成内容：

- 启动按 Manifest 顺序加载 witness、slot A、slot B，再进行唯一选择；
- generation 0 只在 Provider 返回完整有效的 generation-zero provisioning witness 时代表空域；
- `LOAD_SLOT` 必须返回固定槽的完整原始字节并标记 `BLOB_PRESENT`；Foundation 只根据原始 Marker
  判断槽是否已发布。全擦除 Marker 代表未提交槽并被忽略；非完整有效 Marker 代表撕裂并 Fault；
  Fake Provider 不再保存或依赖任何易失 `slot_state` sidecar；
- generation 0 的首次 committed record 为 1 时允许无前驱补推进；witness 非零后，只有同时存在
  `generation=witness` 前驱与 `generation=witness+1` 后继、且 Transaction ID 严格递增时才允许
  补推进；缺前驱、相等或回退全部在 Witness I/O 前失败关闭；
- 两个槽都包含有效记录时，generation 必须严格相邻，且较新记录的 transaction ID 必须严格
  大于较旧记录；同代、跳代、transaction 相等或回退均失败关闭；
- witness 指向的最新记录损坏、两槽同代不同内容、无证明跳号、坏/缺 witness、非法版本或旧
  Manifest 均 Fault，绝不回退旧槽；
- required Domain Fault 使 Persistence Owner Fault；optional Domain Fault 只隔离自身，后续 Domain
  仍能恢复；
- Witness 和 Record generation 到顶均不回绕。

Fake Provider 的 power-cut 测试只丢弃易失 Owner/Gate，保留每个阶段已经写入的模拟介质；重启后
逐阶段验证旧状态、补恢复或新状态三种唯一结果。结论：`PASS`。

### 3.7 IMPL-02-06：多 Domain、公平性、Deadline 和 Coordinator

完成内容：

- 每 Domain 最多一个 pending；Owner 用跨调用持久旋转游标选择可运行 Domain；
- 每次 step 接受 `1..32` 的固定 operation budget，不把等待 Provider 的 continuation 忙轮询；
- 未开始请求在 `now == absolute_deadline` 以半开语义终止，不能借超时清除其他 Domain；
- 已进入不可判定物理提交阶段的失败返回 `IN_DOUBT`，不得冒充未写或成功；
- cancel 只接受尚未开始的请求；proof/failed request 必须显式 retire 后才释放槽；
- Coordinator requirement 以 digest 预筛选、32 B canonical exact body 二次比较；digest、Handle 和
  route staging 只保存在独立 Coordinator Adapter，不进入 Foundation Owner；event sink 失败时
  终态仍保留并可重试，成功后才退休；
- Foundation Owner 只保存通用 `consumer_references`。Adapter 存活时 Owner deinit 失败关闭；绑定
  Coordinator 未销毁、仍有 route/binding 或传入非精确销毁对象时 Adapter deinit 逐字节不写。

逐项自审另验证：`domain_copy_body()` 在同一 Owner 锁内同时要求 Owner 与 Domain 都为 READY，
恢复前、恢复中和实际 Fault 后均拒绝且输出不写；每个 Deadline 终态转换独立消耗一次 step
operation budget，并准确设置 `made_progress/operations_performed`。结论：`PASS`。

### 3.8 IMPL-02-07：Fake Provider 与故障矩阵

覆盖范围：

- 6 类 Provider I/O 的同步完成、单次 PENDING、多次 PENDING、FAILED；
- write、readback、marker、witness、reload 的每个掉电窗口；
- readback 内容破坏、completion blob state 错误、poll token/phase 错配；
- Provider callback 递归 init/recovery/submit/step/query/deinit；
- 相同请求、同 txid 不同 Body、Deadline、取消、retire 重试；
- factory empty、最新槽损坏、双槽同代冲突、witness 回退/跳号/损坏；
- 双槽 transaction 相等/回退、相邻与非相邻 generation、完整但未提交 Body、随机未提交 Body、
  torn Marker，且重启恢复完全不依赖易失 sidecar；
- required/optional 双 Domain 隔离和热点 Domain 公平性；
- 多 Domain 同时到期且 `operation_budget=1` 时一次只终结一个请求；恢复前/中/Fault 后正文复制
  失败且输出哨兵不变；
- Owner/Gate/Workspace/输入/输出完整与部分别名；
- 双 Owner、双线程、共享 callback gate 的 TSan 并发路径。
- 跨 Owner token 唯一性、共享分配器耗尽不回绕和零 Provider I/O。

所有失败测试同时检查返回值、Provider 调用次数、旧 durable snapshot、Owner 状态和输出哨兵；测试
没有通过放宽 `-Werror`、扩大动态资源或把错误路径降级为易失成功来获得绿灯。结论：`PASS`。

### 3.9 IMPL-02-08：构建、ABI、资源和工具链门禁

- Windows GCC：Full Debug/Release、Lite Debug、Nano MinSizeRel 全功能矩阵；
- Persistence OFF：Nano MinSizeRel，无 Persistence target/header/test/symbol；
- MSVC 19.29 Full Release：`/W4 /WX`；
- WSL Clang Full Debug：`-Wall -Wextra -Werror`；
- WSL GCC ASan/UBSan：排除不兼容的静态栈预算和未链接 Sanitizer runtime 的安装 consumer；
- WSL GCC `-fanalyzer -Werror`：分析器测试与 Release 静态调用栈门禁分开；
- WSL GCC TSan：关闭 ASLR 后运行 Owner/Coordinator/Core/Driver/Persistence 五个真实双线程用例；
- 安装 consumer：C99/C++17 include/link/run，Persistence ON/OFF 导出面一致；
- API 索引生成器已扩展到简化版顶层公共头，Persistence API 不再遗漏。
- Nano Debug 与 Nano MinSizeRel 都执行相同 768 B 可见调用链门禁；双槽 decode/select 被拆为
  独立 step operation 后，两种配置分别为 704 B 和 760 B，未通过调整上限掩盖失败。

静态 Release 栈门禁扫描 `src/core|wire|adapter|runtime|persistence`，单函数上限 256 B；可见调用链
上限按 Nano/Lite/Full 分别为 768/1536/3072 B。插桩构建只用于内存/并发错误，不能覆盖或冒充
Release 栈证据。结论：`PASS`。

## 4. 两轮全体交叉自审

### 4.1 第一轮：合同 → 代码 → 测试

| 合同 | 代码落点 | 可区分反例 | 结果 |
| --- | --- | --- | --- |
| 固定容量与 Feature OFF | CMake、公共头、内部对象 | Nano 满表、OFF 符号/安装 | PASS |
| Manifest/Provider ABI | public header、init | digest、geometry、别名、no-poll | PASS |
| Record/Marker canonical | codec | Golden、逐字段破坏、输出不写 | PASS |
| exact continuation | Owner I/O state | 同步早到、多次 PENDING、错 token | PASS |
| persist-before-proof | submit state machine | 每阶段失败、readback 破坏 | PASS |
| anti-rollback recovery | slot selector | 最新槽坏、同代冲突、witness 跳号 | PASS |
| 局部 Fault 与公平性 | step cursor | required/optional、热点双域 | PASS |
| Coordinator 唯一路径 | coordinator adapter | digest collision、sink retry、reentry | PASS |
| Proof 边界 | proof DTO | 改易失摘要/continuation 后 Proof 不变 | PASS |

本轮主动关闭 Provider geometry、可选 poll、重复 PENDING、Proof 非耐久字段、API 索引和
Provider 返回后状态锁重获失败、共享 Gate 生命周期、跨 Owner token 碰撞八个遗漏，未发现仍开放
的软件 P0/P1。
结论：`PASS`。

### 4.3 外审 IMPL02-X01～X06 整改复核

首轮外审发现的 1 项 P0、3 项 P1、2 项 P2 均按反例本体整改，而不是修改测试期望：

| ID | 整改 | 可区分反例结果 |
| --- | --- | --- |
| X01 | 双有效槽增加相邻 generation 与 transaction 严格递增校验 | tx 相等/回退、同代、generation gap 全部恢复 Fault |
| X02 | `domain_copy_body()` 在同一锁内验证 Owner/Domain READY | 恢复前、恢复中、真实 Fault 后拒绝且输出不写 |
| X03 | 超时终态从选择器移入单次 step operation | 双 Domain、budget=1 时分两次推进，均报告 1 次操作 |
| X04 | Marker 成为唯一发布事实，Fake 删除 `slot_state` | 完整未提交槽被忽略；torn Marker 失败关闭 |
| X05 | Foundation 私有状态与 Coordinator Adapter 物理解耦 | Foundation 无 Coordinator include/type/staging，OFF 不承担其 RAM |
| X06 | 把双槽 decode/select 拆成独立有界 step phase，移除大调用链叠加 | Nano Debug 最大 704 B；Nano MinSizeRel 最大 760 B，均低于 768 B ceiling |
| X01-B | witness 非零的补推进要求完整前驱/后继历史 | 缺前驱、tx 相等/回退均 Fault 且 Witness 不变；合法相邻历史和 factory `0→1` 通过 |

将外部独立探针适配到新私有头后重跑，关键结果为：tx 回退和 generation gap 均
`recovery=-4`；超时 `operations=1/progress=1`；Fault 正文复制 `copy_result=-8`；完整未提交槽
恢复 READY 且保留旧 generation；X01-B 的单独 generation 2、witness 1 反例改为
`recovery=-4`，Witness 保持 1。结论：`PASS`，等待外部重新独立复审。

### 4.2 第二轮：故障/测试 → 代码 → 合同

从所有异常终态反向追踪状态归属：

- 任何 Provider call 前都有 exact continuation 和 shared gate；
- 任何 Proof 前都有 marker、witness 和双槽重新加载；
- Provider 返回未知值、错误 completion 或 readback 差异时不更新业务 snapshot；
- 已可能发布 marker/witness 的失败只返回 `IN_DOUBT`/Fault，不执行破坏性回滚；
- 恢复只接受 witness 能唯一证明的记录，不以“较新 CRC 正确”代替高水位证明；
- digest 不是权限，Proof 不是 Authority，Host Fake 不是物理掉电证明；
- Coordinator sink 未确认前终态 obligation 不丢失，重试不重复 Provider 写；
- callback gate、Owner lock、I/O token、Domain generation 和 Handle generation 的作用域互不混用。
- Provider 返回后若状态锁重获失败，不会在未持锁状态继续 fault、summary 或 unlock；对象以
  `call_active` 永久 Fence 停止，shared gate 可供同域其他 Owner 继续做独立故障处置。
- 同一 Provider 回调域的 I/O token 只由 shared gate 分配；双 Owner 并发、轮询和耗尽均不能产生
  相同 token、回绕 token 或未经 Provider 观察的伪完成。

本轮没有发现新的未关闭 P0/P1。结论：`PASS`。

## 5. 当前候选验证矩阵

| 工具链/配置 | 结果 |
| --- | --- |
| Windows GCC Full Debug | 52/52 PASS |
| Windows GCC Full Release | 52/52 PASS |
| Windows GCC Lite Debug | 52/52 PASS |
| Windows GCC Nano Debug，Persistence ON | 52/52 PASS |
| Windows GCC Nano MinSizeRel，Persistence ON | 52/52 PASS |
| Windows GCC Nano MinSizeRel，Persistence OFF | 47/47 PASS |
| MSVC 19.29 Full Release `/W4 /WX` | 45/45 PASS |
| WSL Clang Full Debug | 52/52 PASS |
| WSL GCC ASan/UBSan，兼容门禁集合 | 47/47 PASS |
| WSL GCC `-fanalyzer -Werror`，兼容门禁集合 | 47/47 PASS |
| WSL GCC TSan，关闭 ASLR 的并发定向 | 5/5 PASS |

注：最终数值以同一候选清单再次复跑所得结果为准。静态栈/调用栈只采信非插桩 GNU Release
构建；ASan/Analyzer 下对应 `.su/.ci` 信息会被插桩改变，故在其专属矩阵中排除。

## 6. 固定资源证据

| Profile | Domains | Body 上限 | 单 Slot | Owner 实际/声明 | Gate 实际/声明 |
| --- | ---: | ---: | ---: | ---: | ---: |
| Nano | 2 | 256 B | 368 B | 3800/6144 B | 64/64 B |
| Lite | 4 | 512 B | 624 B | 8456/12288 B | 64/64 B |
| Full | 8 | 1024 B | 1136 B | 23912/32768 B | 64/64 B |

这些数值是 Host ABI 下的静态对象尺寸，不是 ESP32 实测 RAM 峰值。真实产品还需加入 Provider
context、锁、Flash driver、RTOS task stack 和硬件 DMA/缓存，并在目标编译器下重新测量。

## 7. 外审候选和签字边界

候选由 `tools/v6/generate_v6s_impl02_manifest.py` 对 IMPL-00～02 的共同基础、简化通信、
Persistence 实现、测试、工具、冻结合同、台账和报告逐文件计算 SHA256/字节数。任一候选成员
改变一个字节都会使 `v6s_impl02_candidate_gate` 失败；它不修改、替代或继承 V6S-00 的 61 项
外审清单。

当前结论：

```ini
IMPL02-X01..X06 = EXTERNAL REVIEW GO
IMPL02-X01-B    = EXTERNAL REVIEW GO
IMPL-02-00..08  = DONE / RE-SELF-REVIEW PASS
IMPL-02-09      = DONE / EXTERNAL REVIEW GO
Open P0/P1     = 0 found in Host software scope
IMPL-03        = ALLOWED TO START
```

外审签字绑定候选提交 `ffe39f4` 及候选摘要
`B6C2D917A2BBB16FBF896E2158C34246DFE8B7E9192924C760D273ADDC2787C5`。其后的状态同步只允许
修改台账与本报告；任何生产源码、测试、工具或冻结合同变化均不继承该签字。

未完成并继续阻断生产声明：

- 真实 Flash 的 program/erase granularity、16 B Marker 等价原子性和掉电撕裂；
- 独立 anti-rollback Witness 的硬件/平台实现与损坏注入；
- ESP32-S3 编译、任务栈水位、性能、功耗和 24 h 长稳；
- 生产密码存储完整性、设备根密钥和安全启动；
- 任一业务 Record Body、业务 transition validator 和业务 Authority 接线。
