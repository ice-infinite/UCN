# UCN 实时模块 RT-01～RT-07 全体自审报告

> 日期：2026-09-04
> 状态：`DONE / EXTERNAL REVIEW GO（受限实验软件范围）`
> 基线：`main@69901bf` 加当前未提交实时模块工作树
> 范围：默认不链接的实验软件候选；不包含生产 RX/TX、真实 BSP、掉电与时间精度实测

## 1. 全体结论

首轮全体自审后，外部审计提出 RT-A01～A07（3 项 P0、4 项 P1）；第二轮又提出
RT-A08～A10（2 项 P0、1 项 P1）；第三轮外审又提出 RT-A08-B 与 RT-A11（1 项
P0、1 项 P1）。逐项复核确认十一项及 A08 补强均真实存在，现已完成源码整改、
确定性/并发反例和第四轮全体自审。外部复审已确认当前没有保留已知 P0/P1
软件阻断项，RT-01～RT-07 在受限实验软件范围内获得 GO；该签字不启用生产
实时能力。

当前形成七个彼此可单独选择的静态 archive：

```text
ucn_realtime             16 B Envelope Codec
ucn_realtime_policy      Endpoint Policy、Payload Builder、双新鲜度门禁
ucn_time_domain          本地 Domain Time 换算与 FSM
ucn_timed_link           Driver 时间戳扩展与原子事件队列
ucn_time_sync            四报文同步、事务与 Path 准入
ucn_time_capability      Endpoint/Session/Domain/Path 能力租约
ucn_time_authority       STATIC_MASTER generation 防回退启动门
```

这些 archive 都使用 `STATIC EXCLUDE_FROM_ALL`。默认产品只构建 `ucn_core` 时不生成
任何实时 archive，Core/Node/Route/Transfer/Service/Adapter/Port/Cluster 也没有反向
引用实时 API。

## 2. 分项自审结果

| 阶段 | 软件能力 | 本阶段自审结果 |
| --- | --- | --- |
| RT-01 | 固定 16 B Metadata v1 严格编解码、uncertainty class | Golden、完整不写回及固定分量 known-mask 保守聚合通过 |
| RT-02 | 每 Endpoint 的 NONE/LOCAL/SYNCED/DEADLINE 与收包/执行双门禁 | REQUIRED E2E+ACL、采样锁存误差、future-skew、组合 uncertainty、半开 Deadline 与 Guard 绑定通过 |
| RT-03 | 64-bit Domain Time、LOCKED/HOLDOVER/UNSYNCED/FAULT | 清旧滤波但保留同 generation sample/output high-water，锁定前发现倒退并立即 FAULT |
| RT-04 | T1～T4 独立 event key、TX/RX 原子队列、Driver callback | reservation 生命周期、caller-owned 共享 task/ISR/SMP gate、reopen 与 no-wrap 通过 |
| RT-05 | SYNC/FOLLOW_UP/DELAY_REQ/DELAY_RESP 及双向固定 Path | 完整 uncertainty、`peek→retire→ack` 义务重试、乱序/重复/Path 准入通过 |
| RT-06 | Capability Lease 与 STATIC_MASTER 持久 generation | exact identity、到期、重启、双重 reload、异步/重入/损坏/跳号及共享 Authority Gate 通过 |
| RT-07 | 10 节点、双 Domain 的确定性 Host 集成模拟 | 真实 Timed Link TX/RX/completion/retire/reopen 全路径、8/8 Member LOCKED、39 有效样本通过 |

每项的详细合同、反例和阶段资源分别记录在同目录的 RT-01～RT-07 自审报告中。

## 3. 全体交叉自审发现及整改

全体自审不是简单重复 CTest，而是从后级模块反查前级模块边界，共关闭以下问题。

### 3.1 RT-02 与 Service 裁剪耦合

RT-02 最初直接调用 Service 的 Command Guard Codec。`UCN_FEATURE_SERVICE=OFF`
时，Realtime Policy archive 会留下无法解析的 Service 符号。整改后 RT-02 使用与
冻结 12 B ABI 相同的私有无状态 Codec：

- Service ON：测试把私有输出、Service 输出和固定向量做完整 `memcmp`；
- Service OFF：RT-02 可独立链接并通过全部定向测试；
- 两份实现任一字段顺序漂移都会使 Service ON 回归失败。

### 3.2 RT-03 损坏对象继续运行

Time Domain 的公共入口现统一验证 phase、sample cursor/count、配置和 canonical
状态。损坏对象不能索引窗口、继续换算或推动状态机，失败不写输出。

### 3.3 RT-04 回调重入和跨 Link token 消耗

Driver callback Fence 在进入外部函数前建立。嵌套 init/allocate/submit/cancel/
reopen 以及从一个 Link 回调操作另一个 Link 均在状态变化前失败。事件分配在
检查全局 callback Owner 后才递增 token，失败不会产生序号空洞。TX/RX Queue
还增加初始化和 head/tail/count 结构验证，损坏队列不访问数组。

### 3.4 RT-06 租约缓存复用和测试区分度

Capability Cache 只可惰性复用已到期槽，不能淘汰 live 槽；新租约全部验证完成
前不改变缓存。Session mismatch 回归使用本身合法的租约，确保失败来自 exact
admission 而非夹具先天非法。

### 3.5 RT-00A R15 的实现闭环

缺少可信 `max_asymmetry_us` 的固定 Path 只产生
`UCN_TIME_SAMPLE_DIAGNOSTIC`。Time Domain 只累计 diagnostic 统计，不增加有效
样本、不更新滤波、不进入 LOCKED，也不能生成有效同步 Envelope。普通动态 Route
则连诊断事务也不能创建，必须零 pending、零四报文并回退 LOCAL/NONE。

### 3.6 RT-A01：REQUIRED Source ACL

REQUIRED 接收现在同时要求 E2E Protected 和 `source_acl_authorized`。ACL 不再只是
远端 HOLDOVER 的附加条件；任何未获 Endpoint ACL 授权的 Source 都在业务 Payload
暴露前拒绝，失败 view 保持不写回。

### 3.7 RT-A02：完整保守 uncertainty

新增固定分量聚合合同和 known mask，强制纳入 timer resolution、Link capture、
filter residual、integer rounding 与 Path asymmetry。Time Domain 配置显式声明
oscillator bound 是否已知；有效 Sync sample 显式声明 uncertainty 是否已知；发送
Policy 另外 checked-add 业务 `sample_capture_bound_us`。零界、unknown 或任何溢出
都不能生成有效样本或共享 Domain Envelope。

### 3.8 RT-A03/A08：UNSYNCED 清窗与同代际高水位

最大 HOLDOVER 到期不再只清连续样本数，而会清空窗口、cursor/count、offset、rate、
参考点和当前有效样本。新时间源必须从 ACQUIRING 重新积累完整样本数，旧
offset=100 不会污染新 offset=900；但同 `domain_generation` 的样本防重放和已发布
Domain Time 高水位不会被清除。若重锁后的候选时间低于已发布值，Domain 进入
FAULT，调用方 output 不写回。

### 3.9 RT-A04/A05/A09：Timed Link reservation 与共享回调 Fence

Timed Link 使用固定 6 槽 reservation 表跟踪 `RESERVED/SUBMITTED`。同一 key 只能
submit 一次；提交后普通 cancel、重复 completion/retire 均拒绝。原先的进程静态
callback owner 已删除；执行域由调用者创建一个共享 gate，其 task/ISR 回调保护同一
物理锁。Driver callback 期间，任何 Link 的任务 TX/RX 和 ISR RX 控制分配都在 token
变化前拒绝，双线程双 Link TSan 回归未报告数据竞争。

### 3.10 RT-A06/A10：可重试事件释放义务

Time Sync 不再以 `memset` 静默丢弃尚未完成的 T1/T3。超时、higher-sequence 替换
和显式 Abort 先写入固定 release obligation；队列满时事务保持不变并失败关闭。
Owner 只能先 peek 队首，Timed Link retire 成功后再以 exact key ack/pop；Driver
首次取消失败时 obligation 和 key 保留，下一轮可重试，错误 ACK 不改变对象。

### 3.11 RT-A07：真实 RT-04 集成

RT-07 删除直接伪造 Event Key 的 helper。TX 真实执行 allocate→reserve→submit→
TX-event dequeue→complete；RX 真实执行 ISR allocate→原子 Timed RX enqueue/dequeue→
complete；超时/切路执行 release→retire→reopen。最终可执行文件检出 10 个
`ucn_timed_link_*` 符号，默认产品 archive 仍为 0 个实时符号。

### 3.12 第二轮自审额外发现

GCC `-O3 -Werror` 对 ISR RX 发布 reservation 报告潜在空指针/数组边界。实现已改为
在同一临界区内一次取得并显式判空固定槽，随后才发布；Release 和 Analyzer 通过。
继续对 replacement 事务做失败原子性检查时，又发现新 deadline 溢出检查位于旧
T3 release 之后；该顺序可能在最终返回 `UCN_ERR_EXHAUSTED` 时同时保留旧 pending
和 release 副本。现已把 checked deadline 前移到任何 release/状态写入之前，并以
接近 `UINT64_MAX` 的 higher-sequence 反例逐字节证明 Member 对象不变。

### 3.13 第三轮自审额外发现

在 A08～A10 整改后又增加两个实现级门禁：第一，释放队列的错误 ACK 必须逐字节
保持 Master/Member 对象不变，避免调用者错误地确认其他 key；第二，共享 callback
gate 被明确限制为并发开始前初始化，一旦被 Link 引用便不得重新初始化或移动。
POSIX 并发测试使用不同 per-Link mutex 和独立共享 gate mutex，故能够区分旧代码中
“各 Link 自己加锁但静态 owner 仍数据竞争”的缺陷。

### 3.14 RT-A08-B/A11：锁定前单调预检与 Authority 共享 Gate

第三轮外审确认 Time Domain 虽然不会真正发布时间倒退，但重锁样本会先把 phase
写成 `LOCKED`，直到调用 `get_clock_view()` 才 FAULT。整改后，
`ingest_sample()` 在写入 `LOCKED` 前就按候选 offset/rate 计算当前 Domain Time；
低于同 generation 已发布高水位时当场返回 `UCN_ERR_STATE`、进入 FAULT，且
`lock_transitions` 不增加，因此没有可观察的错误 LOCKED 窗口。

Time Authority 原先仍使用无同步的静态 callback owner。现在新增调用方持有的
`ucn_time_authority_callback_gate_t`，同一执行域所有 Authority 共享同一个 Port
任务锁域；active owner 只在该锁下读写。POSIX 回归让 Authority A 的 Provider
回调保持活动，同时从另一线程启动 Authority B，确认 B 零 Provider I/O、完整对象
不变并返回 `UCN_ERR_STATE`；TSan 未报告数据竞争。

## 4. 最终软件矩阵

下列结果均在完成交叉整改和 Guard 一致性测试后重新构建：

| 工具链/配置 | 结果 |
| --- | --- |
| Windows GCC Full Debug | 58/58 PASS |
| Windows GCC Lite Debug | 30/30 PASS |
| Windows GCC Nano Debug | 30/30 PASS |
| Windows GCC Full Service OFF | 30/30 PASS |
| Windows GCC Full Release | 30/30 PASS |
| Windows MSVC VS2019 Full Release | 58/58 PASS |
| WSL Ubuntu 24.04 ROS GCC ASan/UBSan | 32/32 PASS |
| WSL GCC `-fanalyzer -Wall -Wextra -Wpedantic -Werror` | 32/32 PASS |
| WSL GCC TSan non-PIE + `setarch x86_64 -R`，双线程双 Link/双 Authority | 两项目标均 PASS，无 data-race 报告 |

Full Debug 的 58 项包含整个仓库当前启用的 Core、Cluster、Scale 与八项实时定向
测试；WSL 多出的第 31、32 项是 POSIX 双线程 Link/Authority 并发回归；其余 30 项是裁剪配置下可注册的
完整 CTest 集，不是只运行一个实时用例。

## 5. 静态资源和固定内存

Windows GCC Release 单对象 `size` 结果如下：

| 模块 | text | data | bss |
| --- | ---: | ---: | ---: |
| `ucn_realtime` | 1408 B | 0 B | 0 B |
| `ucn_realtime_policy` | 5132 B | 0 B | 0 B |
| `ucn_time_domain` | 5056 B | 0 B | 0 B |
| `ucn_timed_link` | 12232 B | 0 B | 0 B |
| `ucn_time_sync` | 16452 B | 0 B | 0 B |
| `ucn_time_capability` | 3376 B | 0 B | 0 B |
| `ucn_time_authority` | 5040 B | 0 B | 0 B |

七个对象的未链接 text 合计为 48696 B。它只是 Host/MinGW 对象上界，不等于
目标 MCU 最终 Flash 增量；静态链接只会拉入被引用对象，最终值必须由目标交叉
编译器和产品链接图重新测量。

Host ABI 下主要调用者对象为：

| 对象 | `sizeof` |
| --- | ---: |
| Envelope | 24 B |
| Policy Registry | 448 B |
| Time Domain | 200 B |
| Timed Link | 184 B |
| Shared Timed-Link Callback Gate | 32 B |
| TX Timestamp Queue | 184 B |
| Timed RX Queue | 648 B |
| Sync Master | 584 B |
| Sync Member | 240 B |
| Capability Cache | 304 B |
| Authority Owner | 112 B |
| Shared Authority Callback Gate | 32 B |

RT-07 的单 Domain 集成夹具为 10728 B，单 Member 集成夹具为 2152 B。所有容量均
由编译期常量和调用者存储决定，没有 `malloc/calloc/realloc/free`。

Timed Link 与 Time Authority 均不再使用进程级静态 callback Owner；两类 32 B Host
gate 分别由调用者持有，并供执行域内对应的多个 Link/Authority 共享。Authority
Owner 增加一个 8 B Host gate 指针，Authority 对象文件的静态 BSS 降为 0。
这些 Host 数值不等于目标 ABI，真实 MCU 仍须按 Port 锁实现重新测量。

## 6. 隔离、代码质量与文档门禁

- `UCN_BUILD_TESTS=OFF` 的 fresh 产品构建中，实时 artifact 数为 0；
- `libucn_core.a` 无 `ucn_realtime/ucn_time/ucn_timed` 符号；
- 生产 Core、Node、Routing、Transport、Adapter、Service、Port 与总头文件无实时引用；
- 七个公共头均进入 `test_public_headers.c`；
- 核心双语函数注释门禁：`definitions=912 missing=0`；
- 生成式函数签名索引 `--check` 通过；
- 实时源码动态分配扫描为 0，`TODO/FIXME` 为 0；
- 全仓 Markdown 505 份、相对链接 1537 条，`BadLinks=0`；
- `git diff --check` 无空白错误，仅保留既有 LF/CRLF 提示。

## 7. 仍未放行的范围

以下内容明确不属于本次软件完成：

1. 生产 Node/Service RX/TX、Endpoint 编号注册、默认产品 Feature 开关；
2. ESP32、STM32、UART、CAN/CAN-FD、USB、Wi-Fi/ESP-NOW 的真实时间戳 BSP；
3. 真实 Flash 双槽、eFuse/OTP/安全元件 witness 和断电撕裂写；
4. 逻辑分析仪/示波器测得的 p50/p95/p99/max、误差、CPU、栈、功耗；
5. 多 Master、主备 Authority、Cluster Head 绑定；
6. Hop-aware Wire/AAD v2 和逐跳 Deadline 调度。

因此当前准确结论是：**RT-A01～A11 已完成整改，并通过第四轮全体自审和受限
实验软件范围外部复审；生产 RX/TX、真实 BSP、持久化介质与实机精度仍保持 HOLD。**

## 8. 外部审计建议

建议外审按以下顺序对抗：

1. 16 B Envelope 和四类控制 Payload 的逐字节字段/长度/保留位；
2. Policy 的 REQUIRED 不降级、Guard 时钟绑定和两级 Deadline；
3. Diagnostic sample 是否存在任何推动 Domain 或生成 Envelope 的旁路；
4. Timed Link callback 重入、跨 Link、队列损坏及 token no-wrap；
5. Sync 的旧 Session/generation/Path、乱序、重复、超时与动态 Route；
6. Capability live 槽不可淘汰和 exact-deadline；
7. Authority witness-first、reload 证明、异步 PENDING、损坏、回调重入，以及两个 Authority 共享 Gate 的 TSan 并发路径；
8. 默认产品 archive/symbol/source 隔离以及 Service OFF 链接。
