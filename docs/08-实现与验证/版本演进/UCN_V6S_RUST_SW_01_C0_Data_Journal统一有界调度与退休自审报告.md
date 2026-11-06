# Rust RUST-SW-01 第七段：C0 / Data / Journal 统一有界调度与退休

> 2026-10-14；`v6-simplified-rust`，HEAD `03eb7eb` 加完整工作树。
> 用户授权软件实现、每个小项自审和最终整体自审；不操作实机、不提交或推送。
> 本段范围是静态单 Peer、单 Journal 服务端，不能据此关闭整个 RUST-SW-01。
> 逐项及两轮整体自审、最终软件矩阵通过；外审和生产 HOLD。

## 1. 本段解决什么问题

前六段已有真实认证 C0、耐久 Data RPC、Foundation 回读、终态恢复和 ACK/GC，但是
应用仍须自己决定先推进哪条管线、何时退休 Proof/Token、怎样处理同时到达的取消。
仅有这些独立接口，不等于存在一个有界、可收口的组合运行时。

本段新增可裁剪 `journal-runtime` 和 `JournalRuntime<C, F, A, R>`。一次 `step()`
编排原始 RX、实际 Security、C0 命令、耐久 Data Continuation、Foundation、回复发送
和退休，不接受调用方传入“已经认证/已经持久化/已经完成”的替代布尔值。
Coordinator 私有地保留业务与物理义务，外部不能用任意 Handle 推进其中一条状态机。

实际支持范围：

- C1 / H0 直达、受保护、BestEffort / Q2 的耐久 Data Request 与服务端 Result/Error。
- C0 / H0 的 OperationQuery、OperationCancel、OperationResume、OperationResultAck
  入站，以及对应 Receipt/Result 回复。认证、Endpoint ACL 和 Replay 仍由实际 Security 执行。
- 一个固定 Service、一个当前 Peer、一个 Journal，Foundation 可以含其他 Domain，但
  本 Journal 的写入、确认和 GC 必须独占通过这一 Coordinator。
- 专用 `AdapterOwner<1, T, RX, F>`：此实例不能同时交给另一 RX Owner/Core 消费。
  多管线共享 Adapter 的统一分类器不是本段实现范围。

不支持的组合明确拒绝，不改成易失、明文或“看似成功”。现有单独的易失 RPC、Transfer
及其他 Bridge 保留，但尚未全部并入本段 facade；C1 客户端 Result 入站也不由此服务端处理。

## 2. 装配与调用顺序

产品先用真实 Foundation 完成恢复；Factory Empty 对应空 OperationOwner，已存在 Record
则经实际 DurableBase 恢复。先建立实际 Security Session、配置精确 ACL/RpcEndpoint，再
把确认后的 OperationOwner 移入 `JournalRuntime::new()`；待确认的 Owner 不得直接装配。

```text
Driver 原子 RX 发布完整帧 + Link/Token
  → JournalRuntime.step / Ingress
  → C0 或 C1 严格分类 → Security 认证、ACL、Replay → 私有固定命令/请求
  → Coordinator 串行推进 Journal → Foundation 写入、回读、精确 Proof
  → 当前授权复验 → 共享 Gate 内一次 Executor 交接
  → 终态再次回读 → Data 或 C0 受保护回复 → Adapter 精确物理退休
  → 认证 ACK + 不续期保留 → 墓碑回读 → Proof 退休
```

跨 Owner 的操作都在 Runtime Coordinator；Service 不因此链接 Security、Adapter 或
Foundation Provider。Driver 只提供未认证字节与物理元数据，RX 打点不授予执行权限。

| 接口 | 输入及用途 | 输出和边界 |
| --- | --- | --- |
| `JournalRuntime::new(config, endpoint, operation)` | 固定策略、唯一 Service/Peer 权限、确认或空 Journal | 固定容量对象；不调用 Provider/Driver，不建立 Session |
| `JournalStepBudget::new(total, caps)` | 总访问数和七类访问上限 | 校验后不可任意改写的预算计划 |
| `step(budget, peer, security, crypto, persistence, provider, adapter, driver, sequence, transaction, executor)` | 实际 Owner/Provider、当前可信本地 facts、精确 Session/Fingerprint/Link、唯一计数器 | 访问数、推进数、首个错误及停止后静止状态；不返回内部可变 Handle |
| `operation()` / `stats()` / `continuation_phase()` | 只读观察 | 不导出 Journal 写权限或 Continuation 变更能力 |
| `stop()` | 围栏新工作 | 不清除已接纳 I/O、物理 Holder 或已发生副作用 |

`config` 包含 Owner、请求/C0 回复/Journal I/O 超时、重试间隔、最大尝试数和 ACK 保留期。
时间配置必须正值且不超过 `i64::MAX`；所有截止由 checked-add 构造，不因重试续期。
容量 `C=1..16`、`F=96..4096`、`0<R<=146`、`R<=A<=F`，Endpoint 容量也须与之匹配。
实际需要的最大回复大于 Holder 时仍在发布前拒绝，不能从上述下限推断任意结果都可装下。

## 3. 有界调度与公平性

这七类是 Coordinator 工作类别，不是 Wire 上的 Q0～Q3。原 Core 的四类数据队列和
`6:3:2:1` 表未被替换；本段也没有重新定义全节点 QoS。

| 类别 | 每次访问最多推进的事项 |
| --- | --- |
| Ingress | 一个原始 RX；严格分类/认证/固定预留后，在共同尾部退休该 RX |
| Foundation | `PersistenceOwner.step(provider, now, 1)`，一个实际 Foundation 工作单元 |
| Control | 一个已认证 C0 命令的检查/分派；等待 Journal 时保留精确命令 |
| Request | 一个耐久状态边，或一次有界 Executor 交接 |
| Timer | 在 Service 和 Security Replay 维护间轮转，单次各自预算为 1 |
| Transmit | 在 C 个 C0 回复槽和一个 Data 回复槽间轮转，一次 seal 或 submit |
| Retire | 在保留 RX、Data Token、C0 Token/缓存、Journal 义务四支间轮转 |

总访问数为 `1..64`，每类上限为 `1..16`，总数不能大于各上限之和。空类别和 PENDING
检查也消耗访问预算；不能靠“没推进”无限循环借预算。全局游标和各内部游标跨 `step()`
保留，预算为 1 时也不总从 Ingress 开始。每类均有非零配额，没有固定严格优先级旁路。

公平回归以 140 次单访问 step 持续提供无效 RX，最终七类各获 20 次机会；这是调用机会
的有限等待，不是链路吞吐或 WCET 证明。Provider、Crypto、Executor、Driver 均仍须满足
产品的有界非阻塞回调合同；一个访问能调用不同长度的 Codec，不能把访问数换算成微秒。

`JournalStepReport.error` 保留第一个类内错误，剩余类别继续获得预算；入口上的错时间、
错误装配或 Gate 忙直接返回错误且不推进。`progress` 只统计成功状态边，不统计所有失败
清理动作，不等于发送帧数，也不能单凭 `progress=0` 判断无待办。当前没有统一的 next-deadline
或唤醒事件 API；应用须持续提供有界 step 机会。

## 4. 取消、停止与物理退休

合法取消在 Ingress 完成实际认证并精确核对原 Operation 后，立即闩锁取消意图；即使
本轮 Control 配额已用完，后续 Request 类也不得再交接 Executor。伪造、错误摘要/身份或
不具当前权限的命令不能设置该闩锁。闩锁属于 Coordinator 本地事实，不是提前持久化成功。

Data 正在 PREPARED/EXECUTING I/O 时，C0 不覆盖 pending Journal。先确认已接纳写入并
退休精确 Proof，再由原 Continuation 写入 NoEffect；之后才处理独立 C0 Receipt。
若 Executor 已交接，则不能把停止/取消伪装成“从未发生”：已观察的结果继续持久化。

`stop()` 禁止新 seal/submit 和执行交接，但继续推进已接纳写入、失败围栏、物理取消与退休。
完成的回复缓存按原绝对截止退休，不刷新 TTL；因此停止不承诺立即静止。只在私有命令、
Continuation、I/O、RX、所有回复/Token 均已收口后报告 `quiescent=true`，已确认的 Journal
历史仍保留；这不表示共享 Foundation 的其他 Domain 或整个产品已静止。

Driver `NOT_SUBMITTED` 只退休本次 Token，保留不可变字节、原 Sequence/Tag、截止和
尝试水位，有界间隔重试。UNKNOWN 或尚未确认的取消保留 Holder，逻辑超时不回收物理资源。
Runtime 不自行 reopen 实机；测试只通过 Fake Adapter 的明确新 generation 与精确迟到完成
收口旧义务。Journal I/O 失败先按精确 Handle 退休失败请求并 Fence，不能发布成功结果。

## 5. 同代际错绑与回调门

整体自审发现，只比较 Record Generation 仍不足以防止正常 API 装配“同代际、不同正文”
的恢复对象。本段同时绑定实际 Foundation 的 Record Digest：

- DurableBase 导出确认 Record Digest；OperationOwner 在恢复/确认后保存该 digest。
- Durable Continuation 保存 generation + digest，arm/execute/publish 都核对实际当前记录。
- ACK/Resume/GC 的当前基线也核对 digest；Runtime 入口检查实际 runtime、Foundation Owner、
  consumer、Domain、Schema、Version，而不是只信任调用方结构。
- 无本 Coordinator 的私有 pending I/O 时，媒体 generation/digest 必须与 OperationOwner
  一致；有精确自有 I/O 时允许媒体先回读成功、RAM 下一状态边再确认，不误拒绝合法过渡。

新增反例把一个合法 Prepared 恢复对象装配到同代际、实际为 CommittedResult 的媒体上；
step 在推进前拒绝，统计、Provider、Driver 和 Executor 均不变化。这是正常接口错装配防护，
不是防任意内存破坏；真实 Flash 碰撞/损坏和产品持久化安全仍有独立门禁。

共享 CallbackGate 从实际 Security 配置取得，不让调用方给 Runtime 另配一个旁路 Gate。
Crypto 使用 Security 原门；Foundation Provider、Driver submit/cancel 和 Executor 都在同一
共享门内。Gate 忙时入口零推进，跨 Owner 递归进入失败；仅 Driver 原子完成 ingress 例外。
单次借用与原子 Gate 不等于整个应用自动获得任意 SMP 并发安全；装配方必须使用同一个 Gate。

## 6. 每个小项自审与整体自审

| 小项 | 自审内容与结果 |
| --- | --- |
| 固定装配及 Feature | 私有 Owner/固定 scratch、唯一 Journal、专用 Adapter；关闭 feature 仍能独立编译/测试；PASS |
| raw C0/Data 分类 | 不接受认证布尔值，实际 ACL/Replay 和请求摘要；成功 RX 在共同尾部退休，不逐帧增加人工轮转延迟；PASS |
| 七类预算 | 总数/配额边界、预算 1 的长期轮转、RX 洪泛不侵占其他类；PASS |
| Journal 与取消串行 | 异步 Provider poll、Prepared/Executing pending 取消或停止；零 Executor、精确 NoEffect 回读；PASS |
| 发送与退休 | 三次背压后第四次成功、字节/Sequence 不变；UNKNOWN 保留、精确完成退休；PASS |
| 结果 ACK / GC | 实际 Client 认证恢复回复后生成 ACK，不由 Server 手工声明“已收到”；实际墓碑回读；PASS |
| 故障/装配门 | 原始槽回读损坏、失败围栏退休、错时间/Gate、同代际错正文拒绝；PASS |

两轮整体回读分别覆盖成功调用链和取消/停止/失败义务链。除 digest 绑定外，修正了
quiescence 漏计命令队列、准备阶段不应清除取消闩锁、取消不应误吞 NoEffect 回复，以及
Timer 每次只选一种维护而非同时执行两项。最后再次核对接口、预算、状态和逐字节证据。

七组新集成/对抗回归见 `rust/crates/ucn-runtime/tests/journal_runtime.rs`。原 durable RPC、
Operation、Service 和所有旧 Bridge 回归继续保留，不以新成功链替代原失败回归。

## 7. 最终验证与原始证据

最终矩阵 11/11 通过，运行前后 380 个源码/合同依赖完全一致。独立按原始 JSON
逐项重算当前文件 SHA256，差异 0；不是只把旧第六段报告改成新计数。

| 门禁 | 本轮结果 |
| --- | --- |
| Host Debug / Release / MSRV 1.85 | 各 232/232，包含七组新增集成/对抗回归 |
| Clippy / fmt | 全 workspace/all-targets，`-D warnings` 和格式检查通过 |
| 裁剪与依赖 | 17 个组合；三可选 Owner 隔离，普通 Core 不引入高级模块 |
| 三方字节 Oracle | native C / Rust / Python，33/33；未增加新 Wire 向量 |
| Cortex | thumbv7em-none-eabi、thumbv7em-none-eabihf 编译通过 |
| ESP32-S3 | `+esp check -Zbuild-std=core` compile-only 通过，未烧录/运行 |
| 源稳定 | 380 项，`source_stable=true`，最终独立哈希复算差异 0 |

不可变原始副本：[本轮矩阵 JSON](../软件证据/Rust/2026-10-14/software-matrix-sw01-scheduler.json)。
其 SHA256：`F35C7186AF50CF3C79B26ABCECF8295F5BBBBE8DFE8CD912C6AA3E282617FF62`。
包含每条实际命令、退出码、测试计数和每个依赖的 SHA256，旧第六段副本保留作历史证据。

Host 装配示例（不是强制 Profile/目标 RAM 合同）：

| 示例类型 | bytes / alignment |
| --- | --- |
| `JournalRuntime<2,128,16,16>` | 6736 / 8 |
| `JournalRuntime<4,256,64,64>` | 10464 / 8 |
| `JournalRuntime<8,512,128,128>` | 19264 / 8 |

Durable Continuation 三示例由上一段的 272/368/496 B 变为 288/384/512 B，新增的是
本地 Record Digest，不是网络头。上表不含外置 Owner、原子 ingress、Provider 或任务栈。

最终文档门禁 `138 documents / 336 links` 通过；`git diff --check` 无空白错误。
另对本轮 19 项源码/配置/新文档检查尾随空白，结果 0；这包含 Git 尚未跟踪的新文件，
不把单纯 diff-check 当作新文件已检查。真实暂存区为空，已有其他工作树改动原样保留。

复跑命令（仓库根目录，不访问串口）：

```powershell
python tools/v6/run_rust_software_matrix.py --cargo C:/Users/y2389_4rq4ld9/.cargo/bin/cargo.exe
python tools/v6/check_v6_current_docs.py --root .
git diff --check
```

## 8. 开销与仍未闭环的内容

没有新增 Wire 字段、Opcode、OPS1 Body/Schema 或 C Storage Layout；普通 Header、Tag、
OneWay 和原 Data Envelope 不变。变化发生在本地 Coordinator 装配及 RAM：增加固定
队列/工作区、游标和 Record Digest，不能把“报文零增量”表述为“内存零增量”。

Host sizeof 仅描述示例 Rust 类型，外部 Security、Foundation、Adapter/Ingress、Provider、
Executor 和应用存储仍另计。`new()` 返回固定对象不证明 MCU 最坏栈；本轮未测目标链接
Map、优化后调用栈水位或实板运行，不能宣称 Nano/Profile 的整机资源验收通过。

仍需软件实现：源端统一 facade、多个 Peer/Service/Journal 的公平编排、全节点 Adapter
分类与原 Core 队列整合、易失/可靠/Transfer RPC、Parent/Time、动态 Admission/Route/Flow、
Group/Cluster 完整 raw 管线、槽安全复用以及未完成执行/Binding 迁移续接。
本段不改变其任务状态，不声称 C↔Rust 完整 Runtime 互通。

本轮没有新跑 C 全配置矩阵、TSan/Miri/ASan，也没有新增 O2 生产密码端到端证明；保留
原已测接口边界。真实 Flash 原子性/Witness/掉电、硬件密码/时间戳、MCU 栈、物理 Bearer、
吞吐延时与长稳仍须独立实测。只写软件自审结论，外审和生产保持 HOLD。
