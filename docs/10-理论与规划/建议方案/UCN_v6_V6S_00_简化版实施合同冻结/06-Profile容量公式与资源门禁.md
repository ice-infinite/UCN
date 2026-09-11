# V6S-00-06 Profile 容量公式与资源门禁

> 状态：`DONE / SELF-REVIEW PASS / EXTERNAL REVIEW REQUIRED`
>
> 本文冻结容量档位、计算方法、失败行为和后续测量方法；表中 ceiling 是设计门禁，不是当前实现或 MCU 实测值。

## 1. Composition 与 Profile 正交

`Composition` 回答“编译哪些能力”，`Profile` 回答“已编译能力允许多少固定资源”。

```text
Composition = {CORE, ADAPTER, SECURITY, DISCOVERY, FLOW, ...}
Profile     = {NANO, LITE, FULL}
Product override = 在硬上限内替换某些 count/bytes/budget
```

因此 `NANO + SECURITY`、`FULL + STATIC_DIRECT` 都合法。打开 Cluster 不得静默打开 Realtime；
选择 Full 也不得把全部可选模块拉入。模块关闭时，其专用 count、Storage、符号、维护工作和 Wire
流量均为零。

## 2. 数值类型与全局硬上限

| 类别 | 存储类型 | 合法域 | 失败行为 |
| --- | --- | --- | --- |
| 固定槽数/队列深度 | `uint16_t` 配置，内部索引不窄于它 | 启用时 `1..255`，关闭时精确 `0` | 编译期拒绝，不截断 |
| 单帧容量 | `uint16_t` | `64..2048`，且不小于所选最大 Wire header/tag | 编译期拒绝 |
| Copy arena bytes | `size_t` 常量 | 所有乘加 checked，结果可由 `size_t` 表示 | 编译期/static assert 拒绝 |
| 时间/Deadline | `uint64_t us` | 0 为无效；加法不得溢出 | 当前事务失败关闭 |
| Owner step budget | `uint16_t` | `1..65534`，0 无效 | 入口拒绝且零推进 |
| Generation/Sequence | 由 00-02 定义 | 不回绕、不跳过持久化门 | 局部 Fault/Fence |

任何配置表达式必须先在足够宽的无符号类型中求值，再检查能否转换到内部类型。禁止出现“允许
256，但内部 `uint8_t` 回绕为 0”的合同。

## 3. Profile 基础容量

以下是简化版默认 preset。产品可以在相同硬上限内覆盖，但覆盖会改变 Manifest/Layout Hash，
必须重编库和应用；运行时不能扩容。

### 3.1 必选 Kernel 与 Adapter 容量

| 配置项 | Nano | Lite | Full | 说明 |
| --- | ---: | ---: | ---: | --- |
| `LINKS` | 2 | 4 | 8 | 已打开 Link Instance |
| `ENDPOINTS` | 8 | 16 | 32 | 本地 Endpoint |
| `BINDINGS` | 4 | 8 | 16 | 静态/已提交地址绑定 |
| `STATIC_PATHS` | 4 | 16 | 32 | 产品配置静态 Path |
| `REQUESTS` | 4 | 8 | 16 | 受跟踪用户 Request anchor |
| `RECEIPTS` | 4 | 8 | 16 | 独立 Completion receipt |
| `BUFFER_OBLIGATIONS` | 6 | 16 | 32 | Copy/claim/release 义务 |
| `ADAPTER_RX_SLOTS` | 4 | 16 | 32 | 原子 RX record |
| `ADAPTER_TX_SLOTS` | 4 | 16 | 32 | Driver reservation/token |
| `ADAPTER_FRAME_BYTES` | 256 | 256 | 512 | 单个 Adapter slot 最大帧 |
| `Q0_DEPTH` | 4 | 8 | 16 | 紧急控制；不可被借空 |
| `Q1_DEPTH` | 4 | 8 | 16 | 控制/ACK |
| `Q2_DEPTH` | 4 | 16 | 32 | 普通业务 |
| `Q3_DEPTH` | 4 | 16 | 32 | Bulk/后台 |
| `OWNER_EVENT_LATCHES` | 5 | 5 | 5 | Driver、Provider、Timer、Request、Cleanup 五类真值/提示入口 |

`OWNER_EVENT_LATCHES=5` 是事件类别，不等于只保存五个 Driver completion。每个 TX/RX/Provider
token 的终态真值必须内嵌在其固定槽；外部 wakeup queue 只允许丢提示，不能丢完成事实。

### 3.2 可选模块容量

| 模块配置项 | Nano | Lite | Full | 启用前最低值 |
| --- | ---: | ---: | ---: | ---: |
| `BOOTSTRAP_PENDING` | 2 | 4 | 8 | 1 |
| `BOOTSTRAP_LINKS` | 2 | 4 | 8 | 1 |
| `SECURITY_SESSIONS` | 2 | 4 | 8 | 1 |
| `ACL_ENTRIES` | 4 | 8 | 16 | 1 |
| `CAPABILITY_PEERS` | 4 | 8 | 16 | 1 |
| `CAPABILITY_PATHS` | 4 | 8 | 16 | 1 |
| `ROUTE_SETS` | 4 | 8 | 16 | 1 |
| `ROUTE_PATHS_PER_SET` | 2 | 3 | 4 | 1 |
| `ROUTE_CANDIDATES` | 4 | 8 | 16 | 1 |
| `FLOW_PINS` | 4 | 16 | 32 | 1 |
| `METRIC_PATHS` | 4 | 16 | 32 | 1 |
| `QOS_FLOW_SLOTS` | 4 | 16 | 32 | 1 |
| `QOS_INFLIGHT` | 4 | 16 | 32 | 1 |
| `TRANSFER_TX_SLOTS` | 1 | 2 | 4 | 1 |
| `TRANSFER_RX_SLOTS` | 1 | 2 | 4 | 1 |
| `TRANSFER_RECENT` | 2 | 4 | 8 | 1 |
| `TRANSFER_WINDOW` | 4 | 8 | 16 | 1 |
| `TRANSFER_CREDIT_LINKS` | 2 | 4 | 8 | 1 |
| `TRANSFER_CREDIT_RESERVATIONS` | 4 | 16 | 32 | 1 |
| `OPERATION_SLOTS` | 2 | 4 | 8 | 1 |
| `OPERATION_HIGH_WATERS` | 2 | 4 | 8 | 1 |
| `REALTIME_ENDPOINTS` | 2 | 4 | 8 | 1 |
| `TIME_DOMAINS` | 1 | 1 | 2 | 1 |
| `ACTIVE_GROUPS` | 1 | 4 | 8 | 1 |
| `STATIC_GROUP_SLOTS` | 1 | 4 | 8 | 1 |
| `GROUP_KEY_SLOTS` | 1 | 2 | 4 | 1 |
| `GROUP_REPLAY_SOURCES` | 4 | 8 | 16 | 1 |
| `CLUSTER_MEMBERS` | 4 | 8 | 16 | 3 |
| `CLUSTER_VOTERS` | 3 | 7 | 16 | 3 |
| `CLUSTER_TOMBSTONES` | 2 | 4 | 8 | 1 |
| `CLUSTER_DIRECTORY` | 4 | 8 | 16 | 1 |
| `CLUSTER_TUNNELS` | 2 | 4 | 8 | 1 |

Cluster 的最低三 voter 只适用于启用 quorum/Authority 的 Composition；只读 Cluster Member
子能力另行冻结后可不持有 VoterSet。模块依赖所需的联合最小值必须由配置生成器检查，不能等到
运行时才发现一个“已启用但永远无法 Ready”的模块。

### 3.3 Transfer 大小档位

`TRANSFER_MAX_CLASS` 合法值为 0～8，并映射到 `32 << class` bytes，即 32、64、128、256、
512、1024、2048、4096、8192 B。默认 Nano/Lite/Full 分别为 3/6/8。它是单个重组结果上限，
不代表每个 RX slot 必须在栈上创建该大小副本；Buffer 必须来自 Owner Storage 内的固定 arena。

如果产品不启用 Fragment，则该模块全部容量为 0，单帧消息上限由实际
`max_payload(path, contract, security)` 决定，不能借用 Transfer arena。

## 4. Storage 公式

每个模块生成以下常量：

```text
MODULE_STORAGE_BYTES = align_up(
    OWNER_BASE_BYTES
  + sum(TABLE_COUNT_i * TABLE_ENTRY_BYTES_i)
  + sum(BUFFER_COUNT_j * BUFFER_BYTES_j)
  + STAGING_BYTES
  + GATE_AND_LATCH_BYTES,
  MODULE_STORAGE_ALIGNMENT)
```

全局 Storage：

```text
UCN_STORAGE_BYTES = align_up(
    KERNEL_STORAGE_BYTES
  + sum(ENABLED_MODULE_STORAGE_BYTES)
  + COORDINATOR_DIRECTORY_BYTES,
  UCN_STORAGE_ALIGNMENT)
```

要求：

- 所有乘法、加法、`align_up` 先 checked；溢出即配置失败。
- `TABLE_ENTRY_BYTES` 来自真实 private 类型的 `sizeof`，同时由生成报告展开成数值；不能使用
  粗略“每槽最多 512 B”永久掩盖结构膨胀。
- 模块 OFF 时对应加数必须经预处理消失，不是乘以零后仍保留类型、符号或依赖。
- 共享资源与模块专用资源分账；高级模块不得吃掉 Q0、必要 ACK、Cancel/Retire 的保留。
- Storage 中的大对象只能原地 staging，公共 API/Owner step 不得复制完整 Owner/Snapshot。

## 5. 栈门禁

这是实现必须满足的 ceiling，而非当前测量值：

| 项目 | Nano | Lite | Full |
| --- | ---: | ---: | ---: |
| 单个协议 C 函数静态栈上限 | 256 B | 256 B | 256 B |
| 最深协议同步调用链上限 | 768 B | 1536 B | 3072 B |
| ISR/Driver fact 入口上限 | 192 B | 192 B | 256 B |

Port/Crypto/Flash Provider 的内部栈单独计入产品任务栈，不得被协议报告吞掉。实现阶段使用
`-fstack-usage` 生成每函数账本，调用图计算同步链上界；未知/动态/VLA 栈条目直接失败。
目标 RTOS 还要在真实最大报文、最大证书和错误注入下测任务栈高水位，至少保留产品定义安全余量。

超过 256 B 的临时数组、完整 Frame、Crypto workspace、Record 或 Owner 副本必须进入调用方静态
Storage staging，并具备 busy/reentry gate。这个规则直接防止公开 init/open/commit 在 MCU 上
产生数 KB～数十 KB 自动对象。

## 6. CPU、扫描与调度上限

每个公开 `step` 接受 `StepBudgetPlan`，每处理一个固定 work item 消耗 1 budget。任何表扫描都要
保存 persistent rotating cursor；一次调用最多检查 `min(budget, configured_count)` 个槽。

工作类别固定为：Driver/RX completion、Provider completion、Timer/expiry、Control TX、用户
Request、Cancel/Retire。每类有编译期 cap 和最低机会；借用空闲预算不能侵占其他类保证份额。
因此持续 RX 或 Q0 洪泛不能永久饿死 Timer、取消和资源退休。

密码、证书、持久化等昂贵操作有独立 budget，不能把一次“work item”解释成无界证书链或整表
GC。Host benchmark 只报告性能，不改变 MCU 的固定步数合同。

## 7. Wire 与 Payload 预算

资源报告必须对每种启用组合输出：

```text
path_frame_mtu
- carrier framing/fcs overhead
- core contract bytes
- origin tag bytes
- hop trailer bytes
= max_payload(path, contract, security)
```

`ADAPTER_FRAME_BYTES` 是本地承载槽容量，不是远端协商 MTU。若路径最小 MTU 小于所选合同的
头/Tag，计划在发送前失败或选择明确允许的 Transfer；不得截断、降级安全或事后发现。

## 8. 满载和隔离

| 资源状态 | 正确行为 |
| --- | --- |
| 专用模块槽满 | 仅拒绝依赖该模块的新意图；已有状态不覆盖 |
| 共享 Request/Buffer 满 | 按预留/配额背压，不伪成功 |
| Q3/Bulk 满 | 不占用 Q0/Q1 保留；可丢/背压由 Intent 决定 |
| 安全 pending 满 | 不驱逐认证中的别的 Peer，不降级 O0 |
| Persistence staging 满 | 不覆盖 active transaction；局部返回 NO_SPACE |
| Generation/ID 耗尽 | 对应域 Fault/Fence，禁止回绕复用 |
| Cleanup 预算紧张 | 新 admission 可停，但 completion/cancel/retire 保持最低推进 |

## 9. 五本资源账与证据等级

每个标准 Composition × Profile 必须生成：

| 资源账 | Host/静态证据 | 目标证据 |
| --- | --- | --- |
| Flash | archive/ELF/map、符号归属、Feature OFF 差值 | MCU ELF/partition 实占 |
| RAM | `STORAGE_BYTES` 展开、BSS/map、每槽大小 | 启动后静态占用/heap 保持零增长 |
| Stack | `-fstack-usage`、调用图 ceiling | RTOS 栈水位与安全余量 |
| CPU | 每 step work 数、Host benchmark | MCU 周期/占用率/最坏抖动 |
| Wire | exact layout oracle、payload 公式 | Bearer 抓包、吞吐与重传开销 |

Host 通过、公式通过和当前源码对象大小都不能写成“ESP32/STM32 已验证”。真实 Flash、最坏栈、
时间戳精度、功耗和长稳只能在对应硬件测试完成后签字。

## 10. 配置和门禁矩阵

最低持续矩阵：

```text
STATIC_DIRECT       x Nano/Lite/Full
STATIC_ROUTED       x Nano/Lite/Full
SECURE_AUTO_MESH    x Nano/Lite/Full
RELIABLE_CONTROL    x Nano/Lite/Full
Feature OFF 单项与组合
Realtime-only / Cluster-only / 两者同时 / 两者都关闭
```

每项执行 configure 失败反例、编译、CTest、安装 consumer、`nm`/map denylist、Storage 展开和
stack-usage。某 Composition 的合法最低容量高于所选 override 时，应在 configure/compile 阶段
明确失败，不能自动换 Profile 或静默开启模块。

## 11. 本项自审

- Composition 与 Profile 已完全分开；
- 所有默认槽、队列、Frame 和 Transfer 上限均给出确定值与统一宽度；
- Storage 公式可机械展开，但没有伪造尚未实现的 private entry 字节数；
- 栈 ceiling 是实现门禁，真实 stack watermark 仍明确留给目标测试；
- 专用/共享资源、满载、清理进度和 Feature OFF 均失败关闭；
- 本文没有把 Host 数据写成 MCU 结论，也没有放行代码实施。

结论：`V6S-00-06 = DONE / SELF-REVIEW PASS / EXTERNAL REVIEW REQUIRED`。
