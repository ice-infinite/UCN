# UCN V6 简化版 IMPL-01 最小静态通信实施与自审报告

> 状态：`DONE / EXTERNAL REVIEW GO（Host 软件范围）`
>
> 适用分支：`v6-simplified`
>
> 本报告记录软件实现和 Host 门禁，不构成 MCU、真实 Flash、密码 Provider 或物理 Bearer 证明。

## 1. 实施目标与边界

IMPL-01 建立简化版第一条真实、可运行的数据路径：产品提供静态 Identity、Binding、Link、
Endpoint 和 Path；应用提交一帧 Copy Payload；Core 在本地原子接纳后，由 `ucn_step()` 以
C1 O0/H0 编码，经 Adapter Token 提交 Driver；接收端先把完整 Driver Fact 原子复制进固定 RX
槽，再由 `ucn_step()` 严格解码、核对静态 Binding 与 Endpoint、执行一次业务回调并退休 RX
obligation。

本阶段只实现：

- `C1 + BestEffort + OneWay + Data + O0 + H0`；
- A0～A3 地址宽度；
- 静态 Identity、Binding、Endpoint、Link 和 Path；
- Copy Payload 单帧发送；
- Q0～Q3 固定分区队列与 `6:3:2:1` 有界轮转；
- 可选本地发送跟踪、取消、Deadline、completion callback；
- Driver RX/TX/Link 原子事实与有界 `ucn_step()`；
- `stop → step drain → QUIESCENT → deinit` 生命周期。

明确不实现：自动发现与自动路由、Security Session、Reliable、Transfer、Persistence、Group、
Realtime、Cluster、远端收件证明和远端业务执行证明。缺少静态 Path 返回
`UCN_ERR_NOT_FOUND`；结构合法但未实施的发送语义返回 `UCN_ERR_UNSUPPORTED`，不得静默降级。

旧 `src/v6/**` 仍在分支中作为迁移输入和对照测试，但不被新的 `ucn_simplified` target 链接；
本阶段不宣称已经删除全部旧实现。

## 2. 实际模块和调用链

```text
Application
    │ ucn_publish / ucn_step / ucn_stop
    ▼
Core Runtime Coordinator          src/runtime/ucn_node.c
    ├── strict C1 Codec            src/wire/ucn_wire.c
    ├── Adapter token/RX record    src/adapter/ucn_adapter.c
    └── common gate/checks         src/core/*.c
                                      │
                                      ▼
                              caller-provided Driver

Driver RX/TX/Link fact
    → Adapter fixed slot/latch
    → Core bounded step
    → Wire decode and static policy
    → Endpoint callback
    → exact retirement
```

Wire 不读取 Endpoint、Driver 或业务对象；Adapter 不解析 Wire；Driver 不查询 Route、Binding、
ACL 或 Endpoint。Core 是 IMPL-01 唯一跨模块编排者，但 Adapter Token、Wire Frame 和业务
Receipt 分别由各自 Owner 保持独立状态。

## 3. 分项实施与逐项自审

### 3.1 IMPL-01-00：范围、依赖和迁移边界

完成内容：

- 建立独立公共聚合头 `ucn/ucn_simplified.h` 和独立 archive `ucn_simplified`；
- 新路径只依赖共同检查、Owner gate、C1 Wire、Adapter 和 Core Runtime；
- 实施边界检查器拒绝动态内存、旧 `ucn_v6_*` include/symbol 和额外源码归属；
- Feature OFF 与安装 consumer 不依赖旧 v6 Runtime。

逐项自审：`V6S_IMPL_BOUNDARY_OK sources=6 internal_headers=6`；新 archive 中旧
`ucn_v6_*` 定义符号数量为 0。结论：`PASS`。

### 3.2 IMPL-01-01：Profile、Storage、DTO 和静态绑定

完成内容：

- Nano/Lite/Full 容量均为编译期常量；所有固定表有非零和 `<= UINT8_MAX` 约束；
- `UCN_STORAGE_BYTES`/`UCN_STORAGE_ALIGNMENT` 和 `UCN_DECLARE_STORAGE()` 支持 C99 静态分配；
- 每个 Profile 的 `UCN_COMPILED_MANIFEST_HASH` 都由当前实际编译清单机械推导，不再使用手写
  常量；31 个有序 `u32be` 字段覆盖 API/Layout、简化 Feature、全部容量、Storage 和关键 ABI，
  `ucn_init()` 在首次写入 Storage 前精确核对，运行中每个公开入口继续复验节点内保存值；
- `ucn_result_t` 固定为 `int32_t`；公开 DTO 使用 `struct_size + api_version`；
- 初始化核对 Realm、本地 Identity、唯一静态 Binding、Link、锁和 callback-domain gate；
- Storage、Config、Ports、Binding、Link context 与输出之间的完整/部分别名在首次写入前拒绝；
- 初始化失败保持 Storage 和输出不变，初始化中途失败会销毁已建立 Owner 并清零 Storage。

逐项自审发现并关闭：初始化输出或外部 context 指回 Storage 时可能产生自覆盖；补充双向
overlap 检查和逐字节哨兵测试。结论：`PASS`。

### 3.3 IMPL-01-02：C1 O0/H0 严格 Codec

完成内容：

- 冻结 A0～A3 的精确 Header 长度和 big-endian 字段位置；
- Encoder/Decoder 只接受 C1、O0、H0、Data、BestEffort、OneWay 的当前子集；
- 精确检查 Version、Contract、Flags、地址范围、Service、Sequence、Hop Limit 和总长度；
- 普通 encode、in-place encode 和 decode 均在失败时保持输出及长度哨兵不变；
- payload/output 的完整和部分重叠只在明确支持的 in-place 入口允许。

验证包含独立 raw/semantic fixture、Golden、字段级非法值、长度 `-1/精确/+1`、别名和固定 Seed
decode→re-encode 属性测试。结论：`PASS`。

### 3.4 IMPL-01-03：Adapter Link、RX record 和 TX Token

完成内容：

- TX 状态为
  `RESERVED → SUBMITTING → SUBMITTED/COMPLETED/NOT_SUBMITTED/IN_DOUBT/CANCELLED → RETIRED`；
- 每个 TX Token 内嵌 terminal latch，Driver 在 submit 内同步完成不会丢事件；
- 相同 terminal proof 可幂等，冲突 proof、未知 submit 结果和非法状态失败关闭；
- RX 先复制完整 Frame、Meta、Link slot/generation，再发布可 claim 的固定记录；
- Link reopen 推进 instance generation；旧 Link Handle、RX Token 和 TX Token 不得命中新实例；
- 所有 token generation 到顶均不可回绕，仍可扫描并使用其他未耗尽槽；
- Driver callback gate 为 caller-owned、task/ISR/SMP 安全门，不使用进程静态可变指针。

逐项自审发现并关闭：最初在 `ucn_publish()` 时就预留 Adapter Token，会让 Nano 的 4 个 Adapter
Token 提前吞掉 Q0～Q3 各自队列容量。现改为 Core 只在 admission 预留自己的 TX Slot；
`ucn_step()` 真正调度该帧时才按冻结 Link generation 预留 Adapter Token。门忙或 Adapter 暂满
时保留原队列项重试，未提交帧取消时零 Driver 调用。结论：`PASS`。

### 3.5 IMPL-01-04：Core 生命周期、Endpoint 和静态 Path

完成内容：

- 生命周期为 `INITIALIZED → RUNNING → STOPPING → QUIESCENT → deinitialized`；
  `STARTING` 保留给后续异步启动扩展，IMPL-01 不伪造该阶段；
- `ucn_stop()` 只关闭新 RX/TX 接纳并建立 Fence，不在未取得 Driver gate 时调用 Driver；
- 后续 `ucn_step()` 有界推进取消、completion 和退休，全部 obligation 清空后才进入 QUIESCENT；
- `ucn_deinit()` 只接受 QUIESCENT，先使 Core magic 失效，再释放 gate/Adapter 并清零 Storage；
- Endpoint、Path、Link 和 Send Handle 都精确绑定 runtime、Owner、slot、generation、kind；
- 静态 Path 保存创建时精确 Link instance generation；Link reopen 后旧 Path 不能发送；
- 被 TX 引用的 Path、正在 callback 的 Endpoint 和仍保留 Receipt 的对象不能被错误复用。

逐项自审发现并关闭：直接从 INITIALIZED deinit 会绕过 stop Fence；以及 stop 内直接调用 Driver
可能与 callback gate 冲突。现统一执行 stop→step→quiescent→deinit。结论：`PASS`。

### 3.6 IMPL-01-05：未跟踪 Best-Effort Copy 和四级队列

完成内容：

- 不请求 Handle 且无 completion callback 时，只占一个 Core TX Slot；
- payload 在 admission 时复制，`UCN_OK` 只表示本地原子接纳；
- Q0/Q1/Q2/Q3 使用独立编译期分区，某一类满载不能借用或覆盖另一类；
- 调度序列固定为 `Q0:Q1:Q2:Q3 = 6:3:2:1`，每类内部使用持久旋转游标；
- 表满、Path/Binding 不匹配、MTU 不足或已到 Deadline 时不消费 Origin Sequence；
- Origin Sequence 到顶进入不可逆 Fault，不回绕到 1。

验证同时填满各队列、持续补充热点 Q0 并观察 Q1～Q3 有限等待。结论：`PASS`。

### 3.7 IMPL-01-06：受跟踪单帧发送

完成内容：

- 请求 Handle 或 completion callback 时，原子预留 Request、Receipt、Attempt、Buffer 和 TX Slot；
- 任一容量不足时所有对象、输出和 Sequence 均不改变；
- `ucn_send_query()` 返回 admission、Origin Sequence、Link outcome、Buffer release 和 callback 状态；
- `ucn_send_cancel()` 只发布取消要求，由 step 调 Driver；Driver gate 忙时保留可重试状态；
- terminal 后 `ucn_send_forget()` 释放 Request/Receipt；无 callback 的 Handle-only 发送也可忘记；
- callback-only 发送收到真实 Handle，回调返回后仍可用该 Handle 查询并忘记；
- Driver 已接受但无法判定物理结果时公开 `UCN_ERR_IN_DOUBT`，不冒充成功或未发送；
- Deadline 在本地接纳前采用半开语义，`now == deadline` 直接拒绝；早到 terminal proof胜过
  step 调用较晚时观察到的 Deadline。

逐项自审发现并关闭：callback 状态曾可能按“是否跟踪”而不是“是否实际调用 callback”设置；
现 `callback_delivered` 仅在非空 callback 被调用时为 1。结论：`PASS`。

### 3.8 IMPL-01-07：RX 解码、重放与 Endpoint 投递

完成内容：

- Driver 只能提交完整 bytes、Meta 和精确 Link Handle；Adapter 在返回前完成复制；
- step 先复验当前 Link instance，再严格解码 C1；
- 只接受本地 Destination、已知 Source 静态 Binding 和已注册 Service Endpoint；
- 每个静态 Binding 保存易失 64 位滑动重放窗口；重复、零值和窗口外旧 Sequence 不调用业务；
- Endpoint callback 获得只读 payload view 和本次 callback-scope capability，返回 ACCEPT 或 DROP
  后由共同尾声退休 RX Token；
- callback 动态范围持有 Core gate，递归 send/stop/reopen 等控制入口失败关闭；
- RX 轮转 claim 防止热点低槽位长期饿死旧帧。

逐项自审发现并关闭：只做格式校验无法阻止可信 O0 域中的重复帧触发两次业务；增加按静态
Binding 隔离的 replay bitmap，并验证乱序窗口、重复和过旧帧。结论：`PASS`。

### 3.9 IMPL-01-08：有界 step、Link 失效与停止收敛

完成内容：

- `ucn_step()` 每次最多执行 `max_work` 个动作；零预算和单调时间倒退失败关闭；
- 四类工作按跨调用持久游标轮转：completion、timer/cancel、RX、TX；
- 持续 RX 洪泛不能饿死 completion 或取消清理；持续 TX 也不能绕过维护预算；
- Link down/fence/reopen 使精确旧 generation 失效；IN_DOUBT 必须由 Link generation Fence 收口；
- `more_work` 只表示当前可推进工作，不把等待未来 Driver completion 的 obligation 当成忙循环；
- stop 后拒绝新发送和新 RX，同时继续处理已经存在的 completion、取消和退休；
- retained Receipt 仍是本地 obligation，会阻止 QUIESCENT 和 Storage reuse，直到显式 forget。

逐项自审发现并关闭：Adapter 仅有“存在工作”判断会让 submitted token 导致永久
`more_work=1`；新增 runnable-work 判断，将等待外部 completion 与本地可推进工作分离。结论：
`PASS`。

### 3.10 外审 A01～A06 及复审残留整改与逐项自审

外审指出的六项问题和复审残留 A02-B/A04-B/A06-B 均按冻结合同整改，没有把失败路径改成
兼容性降级：

| ID | 整改 | 可区分旧错误实现的回归 | 自审 |
| --- | --- | --- | --- |
| IMPL01-A01 | C1 所有多字节整数统一改为 network big-endian。 | Codec 测试逐字节使用 V6S-00-04 权威 C1/A1 Golden，并以独立 A0/A2/A3 fixture 补齐宽度；旧 little-endian 实现无法通过。 | PASS |
| IMPL01-A02/A02-B | Config 新增必填 `storage_layout` 和 `compiled_manifest_hash`；Hash 由 31 项 canonical manifest 字段按 big-endian FNV-1a-64 编译期推导，初始化及运行中状态复验均要求与当前二进制精确一致。 | 独立运行时 oracle 与编译期值一致；31/31 单字段单值变异均改变 Hash；错 Layout/Hash/保留字段返回 `UCN_ERR_CONFIG` 且 Storage/out-node 不写回。 | PASS |
| IMPL01-A03 | Driver 明确 `NOT_SUBMITTED` 只退休本次 Adapter Token，保留 Core Request/Attempt、原 Sequence 和队列项重试。 | 连续三次背压后第四次成功；持续背压到半开 Deadline 后停止提交并以超时终结。 | PASS |
| IMPL01-A04/A04-B | Endpoint callback 动态范围只向本次消息发布 `ucn_callback_scope_t`；三种 `ucn_callback_*` API 读取冻结的 stats/send/link 快照，普通查询与全部 Mutation API 继续被 gate 拒绝。 | 回调内精确 scope 成功；伪造/过期 scope 零写拒绝；WSL 双线程中 callback 阻塞时，无 scope 的另一线程得到 `UCN_ERR_STATE`，显式持有 scope 才能读取冻结快照。 | PASS |
| IMPL01-A05 | 安装 consumer 同时编译、链接并运行 C99 与 C++17；公共 DTO 全部固定 size/alignment/offsetof。 | GCC/Clang 使用 `-fshort-enums`，MSVC 使用 `/W4 /WX /utf-8`；任一 ABI 漂移编译失败。 | PASS |
| IMPL01-A06/A06-B | Windows consumer 使用显式 `UCN_ASCII_STAGING_ROOT`；仅当规范化后的 `%TEMP%` 全路径确认为 ASCII 时才允许作为默认值。 | 中文 `%TEMP%` 且无 override 在 CMake 配置期失败并给出修复参数；同环境显式 ASCII root 后配置及 C/C++ include/link/run 通过。 | PASS |

额外交叉自审确认：callback 快照只在本次 `ucn_endpoint_message_t.callback_scope` 的半开生命周期
可见，结束 callback 后立即失效；只检查“全局 gate 当前为 step”已被删除，普通并发线程不能再
借快照绕过查询门禁。scope 是进程内 API capability，不是密码 Authority，禁止序列化或持久化。
`NOT_SUBMITTED` 重排在退休 Adapter Token 前先验证 Request/Attempt/Receipt 图，内部图损坏时
不会形成半重排。结论：`PASS`。

## 4. IMPL-01-09：全体交叉自审

### 4.1 第一轮：合同 → 代码 → 测试

按冻结合同逐项追踪到 public API、内部状态和反例：

| 合同 | 代码落点 | 关键反例 | 结果 |
| --- | --- | --- | --- |
| C99 固定存储与 Profile | `ucn_config.h`、`ucn_node.c` | 容量、对齐、别名、脏 Storage | PASS |
| C1 精确 Wire | `ucn_wire.c` | Golden、长度、保留位、in-place | PASS |
| Adapter Token/Link 代际 | `ucn_adapter.c` | 早到、冲突、reopen、耗尽 | PASS |
| Core 生命周期 | `ucn_node.c` | stop gate 忙、retained receipt、deinit | PASS |
| 四级发送 | `ucn_node.c` | 各类满载、热点 Q0、公平性 | PASS |
| 跟踪与取消 | `ucn_node.c` | 原子容量、Deadline、IN_DOUBT | PASS |
| RX 一次投递 | `ucn_node.c` | 坏帧、错 Binding、重复 Sequence | PASS |
| 有界 step | `ucn_node.c` | RX flood、completion/cancel 清理 | PASS |
| 外审整改 | Wire/Config/Adapter/Core/安装门禁 | A01～A06 原始反例 | PASS |

### 4.2 第二轮：故障/测试 → 代码 → 合同

从所有故障注入和终态测试反向检查状态归属，确认：

- 所有失败输出在首次写回前完成参数、范围、别名和状态校验；
- 早到 completion 只进入 token 内嵌 latch，不依赖可丢 wakeup；
- 相同 proof 幂等，冲突 proof、旧 generation 和错 kind 零写拒绝；
- Driver gate 失败不删除取消义务，后续 step 可重试；
- Adapter 暂满不消费新的 Core admission，也不破坏已排队消息；
- terminalize 先退休 Adapter/Attempt/Buffer，再发布 Receipt/callback；
- callback 重入和双线程不同锁域由 caller-owned gate 失败关闭；
- Fault、stop 和 deinit 不把未知物理结果伪造成成功。

本轮未发现新的未关闭 P0/P1；明确保留第 7 节所列非本阶段承诺。A01～A06 的代码和测试变化
不继承此前外审结论，仍需以当前工作树重新外审。结论：`PASS`。

## 5. 当前精确工作树验证矩阵

| 工具链/配置 | 结果 |
| --- | --- |
| Windows GCC Full Debug | 47/47 PASS |
| Windows GCC Lite Debug | 47/47 PASS |
| Windows GCC Nano MinSizeRel | 47/47 PASS |
| Windows GCC Nano Feature-OFF MinSizeRel | 41/41 PASS |
| MSVC 19.29 Full Release `/W4 /WX` | 40/40 PASS |
| Clang 22.1.8 Full Release `-Wall -Wextra -Werror` | 41/41 PASS |
| WSL GCC ASan/UBSan Full 定向矩阵 | 41/41 PASS |
| WSL GCC `-fanalyzer -Werror` Full 定向矩阵 | 41/41 PASS |
| WSL GCC TSan Owner/Coordinator/Core/Driver Gate | 4/4 PASS |
| Windows GCC 中文 `%TEMP%` + 显式 ASCII staging C/C++ consumer | 1/1 PASS |

上述 Full、Lite、Nano 和 Feature-OFF 全量均包含安装 consumer、源码/归档/实施边界、文档冻结、
单函数栈和可见调用链门禁；Clang consumer 由父构建显式传递同族 C/C++/RC 编译器，不能静默
回落到 PATH 中的 GCC。Windows staging 门禁先把 `TEMP/TMP` 指向中文路径，验证无 override 时
在配置期给出明确失败，再以显式 ASCII root 完整执行实际 C/C++ install/include/link/run。
ASan/UBSan、Analyzer 与 TSan 均已针对本次整改后的同一工作树重新运行；此前记录的
Realtime-only、Cluster-only、Adapter-only 结果不作为本次证据。

## 6. 固定资源和栈证据

| Profile | Storage | `ucn_node_t` | Adapter | TX Slot | TX Slots |
| --- | ---: | ---: | ---: | ---: | ---: |
| Nano | 16,384 B | 7,656 B | 1,512 B | 312 B | 16 |
| Lite | 49,152 B | 22,608 B | 5,400 B | 312 B | 48 |
| Full | 98,304 B | 77,536 B | 18,840 B | 568 B | 96 |

共同对象：Handle 12 B、Lock Ops 32 B、Callback Gate 56 B、Mailbox 64 B、Requirement 64 B、
Coordinator Slot 120 B、Coordinator 840 B。

相对 A02-B/A04-B 整改前，Node 固定对象分别增加 Nano 56 B、Lite 80 B、Full 128 B；增量来自
64-bit callback scope/nonce 及按当前 Link 数冻结的只读 Handle 快照，不使用动态内存，也没有
改变各 Profile 的公开 Storage 上限。

Release `-fstack-usage` 复核：本阶段 public publish 192 B、publish validation 96 B、admission
helper 240 B、RX 处理 256 B、step 192 B；单函数均不超过 256 B，静态可见调用链不超过冻结的
768 B ceiling。此前 416 B/288 B 的编译器内联栈峰值已通过拆分校验/接纳 helper 和可移植
`noinline` 边界关闭；这仍不是 MCU RTOS 实际任务栈水位。

## 7. 已知边界与后续约束

- O0 只适用于产品明确声明可信的物理域；无线、不可信中继或需要来源认证的业务必须等待
  Security Session，不能把 `trusted_o0_network=1` 当作通用默认值。
- O0 replay bitmap 和 Origin Sequence 都是易失状态。发送端单独重启而接收端不停机时，重启后
  较低 Sequence 可能被旧窗口拒绝；IMPL-01 不承诺跨重启连续性，后续必须由新鲜 Security
  Session generation 或 Persistence high-water 建立新域。
- 静态 Path 只提供本地配置的单跳/预定出口，不是自动寻路，也不提供路径存活证明。
- 本地 Driver completion 不证明远端收到；受跟踪 Best-Effort 不等于 Reliable。
- 调用方必须在 deinit/Storage reuse 前停止外部 Driver ingress 和其他 task 对该 node 的调用；
  库内 gate 能拒绝同步重入，但不能修复产品层对已释放 caller-owned Storage 的 use-after-free。
- 旧 v6 target 仍共存，仅由边界门禁证明新 `ucn_simplified` 不链接它们；物理删除属于后续迁移。
- Host 构建、Sanitizer、Analyzer 和 TSan 不替代 ESP32/MCU 的中断并发、栈水位、DMA、MTU、
  长稳、功耗和真实链路测试。

## 8. 最终自审结论

```ini
IMPL-01-00..09 = DONE / SELF-REVIEW PASS
IMPL01-A01..A06  = DONE / EXTERNAL RE-REVIEW GO
IMPL-01          = DONE / EXTERNAL REVIEW GO（Host 软件范围）
IMPL-02          = ALLOWED TO START
PRODUCTION/MCU   = NOT PROVEN
```

IMPL-01 已形成最小静态 C1 通信的软件闭环，并完成逐小节自审、两轮相反顺序的全体审查和多工具链
验证。外部复审已独立关闭 A01～A06 及 A02-B/A04-B/A06-B，确认当前完整工作树可在 Host 软件
范围签字并启动 IMPL-02。该结论不包含 MCU、真实 Flash/掉电、物理 Bearer 或生产放行；后续任何
阶段修改本阶段文件时，仍必须重跑对应回归和边界门禁。
