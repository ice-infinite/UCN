# UCN V6 简化版 IMPL-00 共同基础实施与自审报告

> 状态：`DONE / IMPL-00-00..06 SELF-REVIEW PASS / EXTERNAL REVIEW DEFERRED`
>
> 分支：`v6-simplified`
>
> 合同签字基线：`0c8e55107d0a9f5c77c744d33e87d466da7f3088`；候选清单 SHA256
> `AC97DF798655B1F986D9921A0FC180EBDE71116715F08582D240F26A60C85EC5`。

## 1. 任务边界

IMPL-00 只建立后续模块共同使用、且不能由任一业务模块私自复制的基础：固定宽度结果与
Handle、checked 算术/时间/代际、caller-owned callback gate、Owner mailbox、Coordinator 的
typed requirement/event 路由，以及 CMake/安装/Feature-OFF/资源门禁。

本阶段不实现 C0～C5 Codec，不开放新的生产 RX/TX，不建立 Session、Route、Transfer、Group、
Realtime 或 Cluster 状态，也不宣称真实 MCU、Flash、密码 Provider 或物理 Bearer已经验证。

## 2. IMPL-00-00 当前基线清点

基线清点发生在外审 GO 台账提交 `9bd7176` 之后。当前完整 V6 作为迁移输入，不作为简化实现
的反向规范：

| 项目 | 当前事实 | IMPL-00 目标 |
| --- | --- | --- |
| 生产 C 源文件 | `src/v6/**` 共 23 份 | 新共同基础独立 target；旧模块只在后续逐域迁移期间保留 |
| V6 公共头 | `include/ucn/v6/**` 共 22 份 | 最终用户公共面收敛到 `include/ucn/ucn*.h`；内部 SPI 不安装 |
| V6 测试源 | `tests/v6/**` 共 17 份 | 新旧实现测试分 target，不能让旧测试替新合同背书 |
| Owner | `ucn_v6_owner` 同时含 mailbox、13 阶段调度和跨模块 invalidation | mailbox 基元与 Coordinator 分离；业务 Owner 只经 Coordinator 对接 |
| 共同结果码 | `ucn_v6_result_t` 是具名 enum，只有 0 至 -12 | 最终 `ucn_result_t` 固定为 `int32_t`，常量补齐至 `IN_DOUBT=-15` |
| CMake 依赖 | 多个业务 archive 使用较长 `PUBLIC` 依赖链 | 共同基础独立；业务依赖默认 PRIVATE，只有公共 ABI 所需部分传播 |
| 发布面 | `ucn` INTERFACE 当前聚合所有已启用完整 V6 模块 | 在 IMPL-01 切换最小通信闭环前不冒充简化版发布面 |
| 签字证据 | 61 项 Windows 工作树字节清单 | 从不可变 Git 提交重建并复算；当前实现变化不继承签字 |

当前已知迁移风险也被固定：

1. 不能把目录改名当作物理解耦；必须同时看 CMake、符号、安装头、Storage 和任务。
2. 不能直接把 `ucn_v6_stack_owner` 改名成 Coordinator；需先分离 mailbox 与模块连接职责。
3. 不能在共同基础里引入 Identity、Security、Route 或 Persistence 的具体业务结构。
4. 不能用 enum、`bool`、位域、裸指针或编译器 padding 形成稳定公共 ABI。
5. 旧实现只可作为迁移输入；最终发布前必须由 denylist 证明旧 target/header/symbol 为零。

## 3. 小节点与自审状态

| 小节点 | 当前状态 | 独立证据 |
| --- | --- | --- |
| IMPL-00-00 基线、范围与 denylist 输入 | `DONE / SELF-REVIEW PASS` | 签字工件复算、源码/头/测试计数、CMake target 与旧 Owner 职责清点 |
| IMPL-00-01 共同类型与 checked 基元 | `DONE / SELF-REVIEW PASS` | 固定 32-bit 结果 ABI、12 B Handle、checked no-wrap/time/size、失败输出不写回、`-fshort-enums` |
| IMPL-00-02 Gate 与 Owner mailbox | `DONE / SELF-REVIEW PASS` | caller-owned lock、跨 Owner 重入、精确 leave、可合并 mailbox、公平游标、pthread/TSan 定向 |
| IMPL-00-03 Coordinator typed 路由 | `DONE / SELF-REVIEW PASS` | kind→Owner 固定映射、canonical exact bytes、digest collision、过期/错 Handle/重入/满载 |
| IMPL-00-04 CMake/头文件物理解耦 | `DONE / SELF-REVIEW PASS` | 新 target 最小依赖、内部 SPI 不安装、Feature-OFF 与发布面反向检查 |
| IMPL-00-05 持续资源与工具链门禁 | `DONE / SELF-REVIEW PASS` | 八组 GCC Profile/Feature、MSVC、Clang、ASan/UBSan、Analyzer、TSan、Stack/Call-chain、Archive/安装 |
| IMPL-00-06 两轮全体交叉自审 | `DONE / SELF-REVIEW PASS` | 合同→代码→测试与测试/故障→代码→合同两轮反向审计；所有发现整改后重跑 |

## 4. IMPL-00-00 自审

- 签字提交是当前 HEAD 的祖先，清单仍为 61 项且文件哈希未改变。
- 冻结检查器能够从签字 Git tree 重建两份被 Git 规范化 EOL 的 Windows 原始字节。
- 当前耦合被明确登记为“迁移输入”，没有误写成已经解耦。
- 下一节点只实现共同类型与 checked 基元，不提前碰 Wire 或业务状态机。

结论：`IMPL-00-00 = DONE / SELF-REVIEW PASS`；`IMPL-00-01 = IN PROGRESS`。

## 5. IMPL-00-01 实现与自审

新增始终安装的 `ucn/ucn_types.h`，公共 `ucn_result_t` 固定为 `int32_t`，错误常量不再把
C99 `enum` 的编译器宽度带入 ABI。最终 Handle 固定为 12 B，并绑定 Runtime Instance、Owner
Instance、slot、Generation 与 object kind；全零、错实例、错 Owner、越界 slot、零 Generation、
错 kind 和非零保留字段均拒绝。

内部 `ucn_checked` 只提供无业务状态的共同基元：32/64-bit allocate-first 与 checked-next、
微秒 Deadline 创建/半开过期判断、`size_t` 加法和乘法。所有可能失败的输出都延迟到全部检查
通过后写入；零值、终值、溢出和 NULL 分支均有确定性回归。

物理解耦证据：共同代码位于独立 `ucn_common` target，只依赖公共标量头；内部头保留在
`src/internal` 且不安装。archive 门禁只对该精确 archive 允许 `ucn_i_*`，没有放宽旧 v3/v4/v5
名称或符号。安装 consumer 同时检查 32-bit result；GCC 另用 `-fshort-enums` 重新编译同一测试。

本节点验证：

- Windows GCC Full Debug：全量 `30/30`，随后加入 short-enum 门禁后定向 `5/5`；
- `ucn_common_tests` 与 `ucn_common_short_enum_tests` 均通过；
- 安装 consumer、source boundary、archive symbol gate 均通过；
- 冻结签字仍从 `0c8e551` 复算为 `61` 项，未把当前实现冒充原外审候选；
- 文档门禁 `100 documents / 277 links`，`git diff --check` 无空白错误，仅 EOL 提示。

自审未发现 P0/P1：没有动态内存、业务模块依赖、Wire 字段或跨重启 Handle 承诺。当前
`ucn_common` 只是共同基础，不表示旧完整 V6 已迁移或简化版最小通信已经可用。

结论：`IMPL-00-01 = DONE / SELF-REVIEW PASS`；`IMPL-00-02 = IN PROGRESS`。

## 6. IMPL-00-02 实现与自审

callback gate 与 mailbox 均由 Composition 提供的 `ucn_i_lock_ops_t` 保护；共同层没有进程级
静态可变 Owner，也没有把 `volatile` 当成同步。Gate 的 claim 精确绑定 Owner Instance、
operation ID、operation generation 和 kind：持有期间任何同域第二次 enter、错误 leave、destroy
或底层临界区失败都在状态写入前拒绝。初始化/销毁只允许由生命周期 Owner 在独占静止期执行，
普通 enter/leave/view 才是并发入口。

Owner mailbox 只保存五类可合并的 wakeup hint。真实 completion、Fence 和 obligation 必须继续
保存在各自对象内嵌 latch 中；即使 hint 合并或饱和也不丢正确性真值。`take()` 从持久 cursor
循环选择，热点 Completion 不能饿死已等待的 Invalidation/Timer；occurrence 只作饱和诊断。

本节点对抗验证包括：

- 同一 caller-owned gate 上两个 Owner 竞争只有一个成功；错误 claim 不能释放另一个 Owner；
- active gate 不能 destroy/re-init，失败时完整对象不变；底层 lock 失败零写；
- 重复 hint 合并、`UINT32_MAX` 饱和、空 mailbox 输出哨兵不变；
- 热点 class 0 持续补充时，class 1 下一轮仍被选中；pending mailbox 不能 destroy；
- WSL pthread 双线程共享 gate 与双类各 10,000 次 publish 通过；GCC TSan 定向可执行文件运行
  通过且未报告 data race；
- WSL `-fanalyzer` 共同层 4/4、Windows MSVC 19.29 Release `/W4 /WX` 定向 2/2 通过；
- Windows GCC Full 全量 32/32 通过。

自审发现并在签字前修正了一个并发问题：最初入口在取得 caller lock 前读取 mutable claim/
cursor；现已拆为只读 immutable header 的 preflight，以及加锁后的完整 invariant 校验。普通并发
路径不再在锁外读取 mutable gate/mailbox 字段。未发现剩余 P0/P1。

结论：`IMPL-00-02 = DONE / SELF-REVIEW PASS`；`IMPL-00-03 = IN PROGRESS`。

## 7. IMPL-00-03 实现与自审

新增独立 `ucn_coordinator` target。Coordinator 只理解九种 dependency kind、固定 Owner 映射、
公共 Header、32 B 有界 canonical body 和 exact Handle；它不 include 或解析 Identity、Security、
Route、Persistence 等业务结构。kind-specific body 必须先由目标模块 typed builder 规范化；
Coordinator 仅保存、哈希和逐字节比较。

`requirement_digest` 使用显式 little-endian canonical bytes 计算，只作固定表预筛选。命中后仍
比较 policy、deadline、Runtime/Requester、kind、长度和完整 32 B exact body。测试注入固定 digest，
证明相同 digest、不同 exact body 在 Owner callback 和状态写入前拒绝，原 slot 与输出哨兵不变；
exact duplicate 才复用原 Handle 且不重复调用 Owner。

Coordinator 的四个 pending 槽只保存路由 continuation，不复制业务 Active 状态。route/event
调用使用同一个 caller-owned lock 与 `route_active` 动态门；Owner ensure 或 Request event sink
同步重入均失败关闭。Owner 返回的 Handle 必须精确匹配 Runtime、Owner Instance、slot limit、
Generation 和 kind，否则 Coordinator 进入本地 Fault，不发布输出。同一 Handle 也禁止被 Owner
用于两个不同 canonical requirement，否则事件路由会产生歧义，Coordinator 在写入第二个槽前
失败关闭。事件必须精确匹配原 Handle、kind、digest 与 Owner；迟到 `READY` 不会发布成功，而是
规范化成一次确定的 `FAILED/TIMEOUT` 终态。

逻辑模型中的 `DependencyHandle` 由两部分共同实现：Owner 签发的 12 B `ucn_handle_t` 标识具体
对象，Coordinator 槽保存与它一一绑定的完整 canonical requirement、digest、policy 与 deadline。
12 B Handle 单独不构成依赖证明，任何复用、事件匹配或退休都必须先通过 Coordinator 对保留
requirement 的精确比较；因此没有把可碰撞 digest 或调用方另传字段提升成身份依据。

终态不是在 event sink 返回成功后立即遗忘：Coordinator 先把终态交付一次，再要求目标 Owner
退休精确 dependency Handle。退休失败时保留 `DELIVERED` terminal 与 slot，精确重试只重试退休，
不会重复调用 sink；错终态重试零写拒绝。只有 sink 和 retire 都完成后才清零整个 slot。

定向回归覆盖：exact duplicate、强制 digest 碰撞、非 canonical tail、未绑定 kind、半开 Deadline、
满表零 Owner 调用、stale Handle、sink 失败重试、sink 重入、retire 失败精确重试、迟到 READY
规范化、Owner 错 Handle及重复 Handle 导致 Fault。真实 pthread 测试还让一个线程停在 Owner
callback 中，确认另一线程不能进入 Coordinator 写路径，且终态最终交付并退休。

自审未发现 P0/P1。当前 route slot 是易失 continuation，不是 durable proof，也不表示任何业务
Owner 已迁移；固定 digest 注入点只服务内部确定性/碰撞验证，digest 从不承担安全或相等性判断。

结论：`IMPL-00-03 = DONE / SELF-REVIEW PASS`；`IMPL-00-04 = IN PROGRESS`。

## 8. IMPL-00-04 实现与自审

共同基础被拆为两个生产 archive：`ucn_common` 只拥有 checked 基元和 Owner gate/mailbox，
`ucn_coordinator` 只拥有 typed dependency 路由，并且仅以 `PRIVATE` 方式依赖 `ucn_common`。
两者都不加入旧 `UCN_EXPORT_TARGETS`，不进入旧 `UCN::ucn` 聚合发布面；这样后续模块可逐个
迁移，但当前完整 V6 不会因链接简化版基础而被误报为已经完成切换。

公共安装面只新增稳定标量 `ucn/ucn_types.h`。`src/internal/ucn_checked.h`、`ucn_owner.h` 与
`ucn_coordinator.h` 不安装，两个内部 archive 和 CMake target 也不安装。安装 consumer 在
真实安装后反向扫描头、archive 与 `UCNTargets*.cmake`，任一内部对象泄漏都会失败。

新增 `check_v6s_impl_boundaries.py`，固定三个共同源码、三个内部头、两个 target 的精确源码
归属及 Coordinator 的唯一依赖；共同源码出现旧 `ucn/v6`/`ucn_v6_*`、动态内存或业务依赖时
失败。Nano 且 Realtime/Cluster/Adapter 全 OFF 的 MinSizeRel 全量 `28/28` 通过；共同层测试和
门禁仍存在，三个可选模块的头、archive 与任务继续由既有安装门禁验证归零。

本节点自审还发现 `ucn_i_coordinator_init()` 曾在栈上复制完整 Coordinator，GCC 静态栈为
656 B。完成全部可失败前置校验后已改为直接初始化 caller-owned Storage；专用 256 B ceiling
门禁当前扫描 `src/core` 50 个函数，最大为 160 B。`nm -u` 证明 `ucn_common` 只引用 `memset`
及自身 checked 符号，`ucn_coordinator` 只引用 libc 和 `ucn_i_*` 共同基元。

Windows GCC Full 定向物理解耦/安装/栈/Coordinator `5/5` 通过；Nano Feature-OFF 全量
`28/28` 通过。当前结论只证明新基础的物理边界成立；旧 `src/v6` 业务 target 仍是后续迁移
输入，不能据此宣称简化版发布面或任一业务协议已经完成。

结论：`IMPL-00-04 = DONE / SELF-REVIEW PASS`；`IMPL-00-05 = IN PROGRESS`。

## 9. IMPL-00-05 资源、工具链与持续门禁自审

共同基础资源合同不只检查单函数栈。GCC 生产 target 同时生成 `.su` 与
`-fcallgraph-info=su`：单函数静态栈 ceiling 为 256 B，当前 Release 扫描 31 个 `src/core`
函数，最大值为 256 B；静态可见最长同步调用链为 520 B，低于 768 B ceiling。外部
Provider/Driver callback 的内部栈不伪装成协议栈证据，必须由产品另行预算。

固定存储报告为：Handle 12 B、Lock Ops 32 B、Callback Gate 56 B、Mailbox 64 B、Requirement
64 B、Coordinator Slot 120 B、Coordinator 840 B。共同层无动态分配；Coordinator 固定四个
continuation slot，表满不驱逐。`nm`/`dumpbin` 同时扫描定义和未定义符号；新 archive 只允许
`ucn_i_*` 内部符号与 libc，不得反向依赖旧 `ucn_v6_*`，旧业务 archive 也不得提前依赖新基础。

最终工具链矩阵：

| 配置 | 结果 |
| --- | --- |
| Windows GCC Full Debug / Full Release / Lite Debug / Nano Debug | `37/37` / `37/37` / `37/37` / `37/37` |
| Windows GCC Nano Feature 全 OFF / 仅 Realtime / 仅 Cluster / 仅 Adapter | `31/31` / `32/32` / `34/34` / `33/33` |
| MSVC 19.51 Full Release `/W4 /WX` | `33/33` |
| Clang 22.1.8 Full Release | `34/34` |
| WSL GCC ASan/UBSan / `-fanalyzer -Werror` | `38/38` / `38/38` |
| WSL GCC TSan，真实双线程 Owner 与 Coordinator | `2/2` |

自审发现并修正三类门禁问题：旧 Security 的 Release-only maybe-uninitialized 警告；MSVC 对
Coordinator 局部变量遮蔽的 C4456；以及 ASan 插桩栈不能充当静态目标栈证据。Sanitizer/Analyzer
矩阵现明确排除两项静态栈测试；静态栈只由未插桩 GCC Profile 矩阵证明。WSL TSan 在 ASLR 下
会因运行时 `unexpected memory mapping` 自身退出，因此门禁以 `setarch -R` 启动真实测试；这不
关闭或忽略 TSan 报告。

结论：`IMPL-00-05 = DONE / SELF-REVIEW PASS`；没有开放的资源或工具链 P0/P1。

## 10. IMPL-00-06 两轮全体交叉自审

第一轮按“冻结合同 → 数据结构/API → 状态提交点 → 测试”的顺序检查。除复核结果码 ABI、
checked 算术、Owner 锁域、Coordinator 唯一路由与 Feature-OFF 外，还发现并关闭三项实现缺口：

1. `object_kind` 原先只检查非零，可能让保留值作为内部 Handle kind；现统一限制为已定义的
   `SEND..TIME_DOMAIN`，公共 helper 和 Owner binding 两层均检查。
2. 不同 canonical requirement 若被错误 Owner 返回同一个 Handle，原事件查找会产生歧义；现
   在第二个 slot 写入前进入 Fault，保留旧 slot 且输出不写回。
3. Lock、event sink 或 Owner context 若指回待初始化/绑定的 Coordinator/Gate/Mailbox 本体，
   初始化清零或后续 slot 写入会破坏 callback 状态；现于首次写入前拒绝这种自重叠配置。

第二轮按“测试反例 → 失败原子性 → 工具门禁 → 代码与合同”的相反顺序检查。逐个公共/内部
入口确认 NULL、保留枚举、终值/no-wrap、别名、锁失败、重入、满载、过期、digest 碰撞、错
Handle、终态重试和销毁边界；再从安装 consumer、archive、栈/调用链和 Feature-OFF 反向确认
物理解耦。该轮发现并关闭终态交付后未退休 Owner 资源、迟到 READY 槽泄漏，以及 retire 失败
可能重复业务 sink 的生命周期问题，最终形成 `stage → deliver once → retire → clear` 单向序列。

所有修复完成后重新执行第 9 节矩阵，未发现开放 P0/P1。以下限制保持：

- 这只是内部共同基础，还没有实现 C0～C5 Wire、静态通信闭环或简化版生产发布面；
- TSan 是 Host 并发证据，不替代 MCU ISR/SMP 临界区实现审计；
- 资源数字是 Host/GCC 静态证据，不替代目标 RTOS 栈水位、Flash/RAM map 和长稳；
- 当前完整 V6 target 仍是迁移输入，最终必须在后续节点由 denylist 删除，而不是长期双栈。

结论：`IMPL-00 = DONE / SELF-REVIEW PASS / EXTERNAL REVIEW DEFERRED`。按用户要求不在每个
模块后发起外审；下一步进入 `IMPL-01`，最终在全部实施节点完成并经过多轮全体自审后统一送审。
