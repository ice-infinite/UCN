# UCN v6 可裁剪模块边界、依赖、资源与静态装配详细设计

> 文档状态：`SELF-REVIEWED / EXTERNAL REVIEW REQUIRED`
> 适用范围：UCN v6 后续破坏性模块化重构
> 当前边界：本文是目标架构和实施合同，不代表当前源码已经完成全部裁剪
> 核心约束：MCU-first、C99、固定内存、无动态插件、默认单一 Protocol Owner、普通消息不为未使用功能付出 Wire 成本
>
> Nano/Lite/Full 的精确 preset、Storage/Stack ceiling 和资源证据分级候选见[V6S-00-06 Profile 容量公式与资源门禁](UCN_v6_V6S_00_简化版实施合同冻结/06-Profile容量公式与资源门禁.md)；本文继续负责模块依赖、Composition 和资源所有权。

本文只负责模块边界、依赖和资源总合同。逐入口状态机、时序图、失败路径和可实现伪代码已经拆到 [UCN v6 逻辑模型、伪代码与状态图](UCN_v6_逻辑模型与伪代码/README.md)，避免继续把所有执行细节堆入同一文件。

## 1. 本文解决什么问题

UCN v6 已经把 Identity、Wire、Security、Route、QoS、Transfer、Realtime、Cluster、Adapter 等能力分到不同源码目录和静态库中，但“文件分开”不等于“功能已经模块化”。真正的可裁剪模块化必须同时满足：

1. 基础通信不依赖高级能力也能独立运行；
2. 未启用模块不链接其实现，不占用专用 RAM，不运行后台维护，也不产生专用控制流量；
3. 普通消息不携带未使用能力的字段；
4. 模块关闭后，请求该能力必须明确失败，不能静默降级；
5. 每种协议状态只有一个权威 Owner，模块之间不复制 Active 状态；
6. 应用始终表达业务目标，不要求普通用户理解 Route、Flow、Contract、Transfer 和 Cluster 内部组合；
7. 功能组合与 Nano/Lite/Full 容量档位正交；
8. 所有组合均由编译期事实、初始化校验和运行期门禁共同约束。

本文已完成与简化逻辑主线的内部交叉自审；在独立外审前，这些条款仍是候选合同：

- 最小通信内核必须保留什么；
- 每个可选模块负责什么、拥有什么、依赖什么；
- 模块关闭后协议还能做什么；
- 模块之间允许怎样连接；
- 如何静态装配和分配 Storage；
- 用户 Intent 如何自动解析成模块组合；
- 如何验证 Flash、RAM、CPU、Wire 和行为确实被裁剪；
- 当前 v6 从现状迁移到目标架构的顺序。

### 1.1 三份设计文档的权威范围

本次模块化设计与低开销 Wire、用户 Intent/API 设计共同组成目标合同。三者的权威范围固定为：

| 文档 | 唯一权威范围 |
| --- | --- |
| 本文 | 能力边界、唯一状态 Owner、模块依赖、关闭行为、静态资源和装配规则 |
| 低开销 Wire 设计 | 线上字段、Contract、事务键、Context 恢复、确认/重传和协议交互 |
| 用户 Intent/API 设计 | 用户语义、Policy 合并、Resolver/Runtime 协调、请求/Attempt/Buffer 生命周期和 Completion |

同一条跨文档合同必须同时一致。发现冲突时不得让实现者任选一份，也不得仅声明“新文档覆盖旧文档”；必须先同步修订，再冻结或编码。

## 2. 设计目标与非目标

### 2.1 设计目标

最终目标可以概括为：

> UCN 采用“最小通信内核 + 可选能力模块 + 静态装配层”。简单通信只承担简单通信的成本；自动寻路、安全、可靠传输、分片、实时、组通信和 Cluster 等能力，仅在产品需要且依赖成立时进入固件和消息路径。

具体目标如下：

- 两块预配置 MCU 可以只用最小内核和一个 Adapter 完成有界消息收发；
- 固定多跳不依赖动态寻路；
- 动态寻路不强制建立 Flow；
- Request/Result 不强制写 Flash；
- Reliable 不自动等价为 Durable Operation；
- Realtime 不成为普通超时和租约的依赖；
- Group 不依赖 Cluster；
- Cluster 不成为普通路由和业务通信的中心；
- Planner 不成为每帧发送的高成本必经路径；
- 中继不必拥有端点的全部业务能力；
- 同一消息语义、同一安全合同和同一 Wire Contract，不因无关模块开关不同而改变 Golden Bytes。

### 2.2 非目标

本文明确不做以下事情：

- 不实现运行时动态加载插件；
- 不要求每个模块对应一个 RTOS Task；
- 不建立通用反射式组件框架；
- 不为了“解耦”引入堆分配、虚函数系统或复杂消息总线；
- 不保证任意模块开关组合都合法；
- 不让 Feature 开关绕过安全策略；
- 不把 Host、Linux、ROS2 或网关设为 MCU 网络的控制中心；
- 不以兼容 v4/v5 为目标；旧版本只保留在 Git 历史、Tag 或归档文档中。

## 3. 四个必须分开的配置维度

后续所有 API、构建文件和文档都必须区分以下四个维度。

| 维度 | 回答的问题 | 示例 | 生效时机 |
| --- | --- | --- | --- |
| Composition | 固件拥有哪些能力？ | 是否包含 Discovery、E2E、Transfer、Cluster | 编译期为主 |
| Profile | 每项能力允许占多少资源？ | Route 数、队列深度、最大消息、并发事务数 | 编译期 |
| Policy | 产品允许怎样使用这些能力？ | 哪些 Endpoint 必须认证、是否允许明文、是否允许自动寻路 | 初始化及运行期 |
| Intent | 本次业务想达到什么效果？ | 低延迟、可靠、加密、组发、指定路径 | 每次调用 |

### 3.1 Composition 不是 Profile

`NANO/LITE/FULL` 只表达固定容量和预算，不表达功能集合。

因此以下组合必须合法：

- `NANO + SECURITY_ENDPOINT`：资源很小但具备认证能力；
- `FULL + STATIC_DIRECT`：容量较大但只做高速固定直连；
- `LITE + AUTO_MESH`：小型自动组网节点；
- `FULL + CLUSTERED_REALTIME`：完整的大规模实时簇节点。

禁止把 `FULL` 定义为“所有 Feature 自动开启”，也禁止把 `NANO` 定义为“安全能力必然关闭”。

正交不等于任何容量都能承载任何能力。每个模块必须声明最低容量合同；例如 Cluster 至少需要一个本地成员槽和满足法定人数表达的 VoterSet，Reliable 至少需要一个事务槽。所选 Profile 低于最低容量时必须拒绝构建或初始化，不能静默关闭功能，也不能在运行时偷偷扩大内存。

### 3.2 Policy 不能被 Composition 静默改写

如果产品 Policy 要求某 Endpoint 必须 E2E Auth，而 Composition 没有 E2E 模块，则必须：

- 在配置验证阶段拒绝启动；或
- 在 Endpoint 注册阶段拒绝该 Policy；或
- 在发送前返回明确配置错误。

不得把“没有编译安全模块”解释成“自动允许不安全发送”。

### 3.3 Intent 不直接指定内部模块

普通用户应表达：

- 发给谁；
- 发什么；
- 是否可靠；
- 是否低延迟或带 Deadline；
- 是否需要认证或加密；
- 单播、组播还是本地广播；
- 是否允许自动寻路；
- 是否接受失败回退。

普通用户不需要填写：

- 使用 C1、C2、C3 还是 C4；
- 是否调用 Route Discovery；
- 是否建立 Flow Label；
- 是否启用 Fragment Window；
- 使用哪个内部 Security Context；
- 是否由 Cluster Directory 提供路径候选。

这些由统一 Resolver 根据 Composition、Profile、Policy、实时状态和 Intent 决定。

## 4. 总体分层

```text
应用
  │
  │ Target + Endpoint + Payload + Intent
  ▼
统一应用 API
  │
  ├─ Core Intent Resolver：必选、确定性、低成本
  └─ Advanced Planner：可选、多候选评分和自动优化
  │
  ▼
最小通信内核
  ├─ Config / Manifest / Layout
  ├─ Protocol Owner / Fence / Event Budget
  ├─ Local Monotonic Time / Timer
  ├─ Buffer / Request 生命周期
  ├─ Wire Strict Gate / Endpoint Dispatch
  ├─ Direct Binding / Basic Queue
  └─ Policy / Capability Gate
  │
  ├─ Identity / Admission / Security
  ├─ Network / Flow
  ├─ Transport / Service / Operation
  ├─ QoS / Realtime
  ├─ Group / Cluster
  └─ Diagnostics
  │
  ▼
Runtime Composition
  │
  ▼
Adapter / Port / Driver
```

### 4.1 依赖方向

允许的依赖方向为：

```text
Application
    ↓
Public API / Resolver
    ↓
Capability Modules
    ↓
Kernel Contracts
    ↓
Adapter Contract / Port Contract
```

禁止出现：

- Kernel include Cluster 私有头；
- Wire Codec 主动修改 Route、Security 或 Cluster 状态；
- Adapter 直接调用业务 Endpoint；
- Planner 直接写 Active Route；
- Discovery 保存第二份 Active Route；
- Cluster 直接修改 Security Replay Window；
- Diagnostics 因为关闭而改变正常协议语义；
- Host 拥有 MCU 节点内部的隐式路由或准入特权。

## 5. 最小通信内核

最小内核以一个明确场景定义：

> 两块 MCU 已通过产品配置确定身份、目标、Endpoint 和物理 Link，只需要发送一条有长度边界的消息。

这一场景不得要求 Dynamic Bootstrap、Route Discovery、Flow、Reliable Transfer、Realtime、Group、Cluster 或 Advanced Planner。

### 5.1 Kernel Config 与 Build Manifest

负责：

- 编译 Profile；
- 编译 Feature Bits；
- Storage Bytes 和 Alignment；
- Layout Hash；
- 配置版本和结构尺寸；
- 依赖检查；
- 运行时配置与编译事实一致性验证。

它不负责决定本次消息选择哪条路径。

### 5.2 Protocol Owner 与 Fence

负责：

- 唯一协议执行上下文；
- 固定事件队列或事件槽；
- 每轮工作预算；
- Driver callback 重入门；
- task/ISR 之间的合法状态转移；
- Authority、Link、Session、Route 等依赖失效时的立即 Fence；
- 模块的有界 step/poll 调用。

模块化不能破坏“一个节点的协议状态默认由一个 Owner 推进”的原则。

### 5.3 本地单调时间与 Timer

即使 Realtime 模块关闭，内核仍需提供：

- 本地单调时间；
- checked duration；
- 半开 Deadline；
- 队列过期；
- 重试超时；
- Link 活性计时；
- 资源退休超时。

跨节点 Domain Time、uncertainty 和同步协议属于 Realtime 模块，不进入 Core Time。

### 5.4 Wire Strict Gate

负责：

- v6 版本和 Contract 分派；
- 精确长度；
- Flags 合法域；
- 保留位；
- Extension 顺序；
- Payload 边界；
- 输出不写回；
- 不支持 Contract/Opcode 的严格拒绝。

Wire Gate 只产生规范化语义对象或错误，不拥有 Route、Session、Flow、Transfer 或 Cluster 状态。

### 5.5 未跟踪最小发布与受跟踪 Request 生命周期

内核必须定义统一的生命周期合同，但不能把三种实际对象压成同一个状态机，也不能强迫最普通的
fire-and-forget 消息承担完整事务对象成本。

仅当请求同时满足 `PUBLISH + ONE_WAY + operation_id=0 + BEST_EFFORT + COPY + 单帧 + LOCAL_ACCEPTED`、无 callback、
`out_handle==NULL`，并且不要求 Group、Latest、Reliable、Transfer、Zero-copy、Durable Operation
或任何 Operation/dedup、远端结果时，Runtime 才可选择未跟踪最小路径。该路径只创建一个 TX Slot；TX Slot 内嵌 Copy
Frame、Deadline、Route 等待状态和 exact Driver completion latch。若还需要独立 Security/Flow/
Transfer/Persistence Setup，必须在本地接受前升级为受跟踪 Request，或零副作用拒绝。

其余发送均进入下面的受跟踪生命周期：

| 对象 | 回答的问题 | 唯一身份 |
| --- | --- | --- |
| Send Request | 受跟踪应用提交最终达到什么 Completion？ | Request ID；整个重规划过程不变 |
| Transmission Attempt | 本次使用哪条 Path/Contract、发送了哪些帧、确认和重试到哪一步？ | Attempt ID；每次切路或重建 Contract 新建 |
| Buffer Obligation | Driver、DMA、Transport、密码 Provider 或应用是否仍持有这段内存？ | Buffer Token + owner/ref 状态 |
| Completion Receipt | 用户已收到的不可变 Completion 与可查询终态 | Request Anchor + Receipt slot/generation |

Send Request 的概念状态为：

```text
FREE
  → VALIDATED
  → LOCAL_ACCEPTED
  → PREPARING
  → SEND_ADMITTED
  → WAITING_COMPLETION
  → TERMINAL_SUCCESS / TERMINAL_FAILURE / TERMINAL_UNKNOWN
  → REQUEST_RETIRED
```

Transmission Attempt 的概念状态为：

```text
FREE
  → PLANNED
  → RESOURCES_RESERVED
  → LINK_SUBMITTED
  → IN_FLIGHT
  → ACKED / FAILED / CANCEL_REQUESTED / IN_DOUBT
  → ATTEMPT_RETIRED
```

`IN_DOUBT` 保留逻辑 `OUTCOME_UNKNOWN`，但物理资源不能无说明地永久泄漏。Owner 必须执行
编译期有界的 query/cancel；仍无证明时 Fence exact Adapter Instance，并经 close/reopen 或
等价硬件 quiescence 证明旧 DMA/token 已不可能访问 Buffer，随后只退休物理 obligation，
不把逻辑 Outcome 改成成功或失败。若 Driver 永远无法给出 quiescence 证明，则该 Adapter
Instance 进入 Fault 且相关资源永久隔离，不能复用给新工作。

Buffer Obligation 独立为：

```text
FREE
  → OWNED_BY_APPLICATION / OWNED_BY_COPY_POOL
  → HELD_BY_RUNTIME
  → HELD_BY_DRIVER_OR_TRANSPORT
  → RELEASE_PENDING
  → BUFFER_RELEASED
```

必须区分：

- Buffer 已复制；
- Runtime 已接受 Request；
- Driver 已接受某个 Attempt；
- 帧已发出；
- 对端已收到；
- 业务已执行；
- Result 已持久化；
- 所有 Buffer 使用者是否已经退休。

切路可以创建新的 Attempt，但不能创建新的业务 Request。Request 达到 `LOCAL_ACCEPTED` 或其他 Completion，不代表 Zero-copy Buffer 已经可以复用；只有独立的 `BUFFER_RELEASED` 才解除内存义务。Request 失败可以发起取消，但不得直接释放仍由 DMA、Driver 或重传窗口持有的 Buffer。模块不得把某一层成功冒充更高层成功。未跟踪最小路径不暴露 Completion 查询或取消；它只能在接受时报告 `LOCAL_ACCEPTED`，后续 Link 结果进入诊断。

所有受跟踪路径共享唯一 Request Finalizer；未跟踪 TX-Slot 路径没有 Request，也不调用 Finalizer。Finalizer 必须能从已经不可变的 terminal outcome 推导并首次发布
SUCCESS、FAILURE 或 UNKNOWN，而不是只处理“目标已不可能达到”的失败分支。同步完成、无 token
本地完成或重启对账后的成功可以直接进入 Finalizer；在它确认 Receipt latch 已进入唯一终态前，
不得退休 Request execution。冲突终态进入局部 Fault/IN_DOUBT，不能覆盖已发布 Completion。

公共 Request Handle 解析稳定 Anchor，而不是直接把 Request 执行槽或 Receipt 槽当作 Handle。
Anchor 分别保存 exact live-request 与 exact receipt 的 slot/generation 引用；执行槽可在义务退休
后复用，Receipt 仍保留用户不可变 Completion。后台最终 Outcome 使用独立诊断字段，不能
改写已经发布的 `LOCAL_ACCEPTED`。Anchor/Receipt 均须在受跟踪 Request 创建时预留，且在后台执行
退休、应用 ACK/consume 或冻结的非 durable 过期合同满足前不得释放。

### 5.6 Endpoint 与基础消息分派

负责：

- Endpoint 注册；
- 发送与接收的长度边界；
- Endpoint Policy 查找；
- 接收侧业务准入；
- 业务 Inbox 或 callback 提交；
- 一次性、明确的错误返回。

耗时业务不得在 Protocol Owner 中直接执行。

### 5.7 Direct Binding

最小内核可保存产品静态配置的：

- Local Identity 引用；
- Target Address/Binding；
- 出口 Link；
- Endpoint；
- 允许的基础 Wire Contract；
- 基础安全策略。

Direct Binding 不是动态 Route Store，也不能接受 Discovery 提交的候选。

### 5.8 Basic Queue 与最低调度门禁

QoS 高级模块关闭时，内核仍保留：

- 固定深度队列；
- Control 与业务的最低资源隔离；
- 每轮发送预算；
- 队列满明确失败；
- 取消和最终回收；
- 不允许单一来源无限占用全部固定槽。

高级 Q0～Q3、Latest、Flow Fairness、EDF、聚合和详细统计属于 QoS 模块。

### 5.9 Policy 与 Capability Gate

这是必选安全语义，不属于可关闭的密码实现。

负责检查：

- 固件是否编译了所需功能；
- Peer、Path、Endpoint 是否声明并证明所需能力；
- 当前状态是否仍满足 Policy；
- 资源是否可用；
- 是否存在非法降级；
- 当前 Intent 是否能形成至少一个合法执行计划。

### 5.10 不属于最小内核的能力

以下能力不得成为最小直连发送的依赖：

- 动态地址分配；
- 自动寻路；
- Flow 建立；
- Peer Session；
- E2E 加密；
- 可靠重传；
- 分片重组；
- Request/Result；
- Durable Operation；
- 跨节点时间同步；
- Group；
- Cluster；
- Advanced Planner；
- 详细诊断和 Host 输出。

## 6. 模块统一合同

每个可选模块必须拥有一份静态描述，不要求形成运行时反射对象，但必须能生成编译事实和测试表。

建议描述字段：

```c
typedef struct {
    uint32_t module_bit;
    uint32_t required_module_bits;
    uint32_t optional_module_bits;
    size_t storage_bytes;
    size_t storage_alignment;
    uint32_t max_step_work;
    uint32_t wire_contract_bits;
} ucn_v6_module_manifest_entry_t;
```

该结构只是说明形式。最终可以由生成头、常量表和静态断言实现，不要求在 MCU 中保留完整字符串或动态注册表。

每个模块都必须回答：

1. 它增加什么能力；
2. 它唯一拥有何种状态；
3. 它依赖哪些模块；
4. 它能从哪些模块获取只读 View；
5. 它接受什么事件；
6. 它返回什么句柄或结果；
7. 它关闭后哪些路径仍必须正常；
8. 它如何被 Fence；
9. 它占多少 Flash、RAM、栈、CPU 和 Wire；
10. 它如何证明完全关闭。

## 7. Identity 与 Admission

### 7.1 Static Identity/Binding

建议把基础 Identity 类型和静态 Binding 保留为轻量基础能力。

它拥有：

- 不可变 Device Principal 引用；
- Realm；
- 静态 Address Binding；
- Binding Generation；
- 本地持久化高水位引用。

它不拥有：

- 动态 Join pending；
- Bootstrap cookie；
- Address Authority 租约；
- Peer Session 密钥；
- Endpoint ACL。

关闭动态 Admission 后，产品仍可使用制造时或部署时写入的静态身份和 Binding。

### 7.2 Dynamic Admission

Dynamic Admission 包含：

- Bootstrap Discovery；
- Challenge/Response；
- Address Offer/Commit；
- 未绑定设备的 Join/资格授予；
- pending slot、cookie、限流；
- Address Authority 证明；
- persist-before-use。

依赖：

- Static Identity；
- Security Authentication Provider；
- Persistence；
- Core Time；
- Link/Peer Context。

Dynamic Admission 可以调用 Security Session Handshake 证明设备身份，但不拥有 Peer Session，也不负责已绑定节点的普通重连、会话轮换或重新认证。

关闭行为：

- 不接受陌生设备；
- 不发送 Bootstrap 控制帧；
- 静态 Binding 仍可工作；
- 已要求动态入网的产品配置必须初始化失败。

### 7.3 Capability 的两层边界

Capability 不能作为一个整体被简单删除，应拆成：

**内核静态能力事实：**

- 当前 Build Manifest；
- 本地支持的 Wire Contract 和 Opcode；
- `CAN_SEND/CAN_FORWARD/CAN_TERMINATE/CAN_AUTHORIZE` 角色能力；
- 严格拒绝未实现能力。

这些属于 Policy/Capability Gate，必须存在。

**可选动态能力交换：**

- Peer Capability Record；
- Path MTU 和 Payload Budget；
- Capability Lease；
- Group Discovery Hint；
- 远端能力 Generation 和过期状态。

动态能力交换关闭后，静态配置的 Peer/Path 能力仍可使用；未知远端不得被乐观假设为支持高级能力。

Capability 事实如何与 Policy、Authority、资源 View 求交，以[Capability 与 Contract Resolver 简化设计](UCN_v6_逻辑模型与伪代码/23-Capability与Contract-Resolver简化设计.md)为准。Capability 从不单独授予执行权限。

## 8. Security 模块

Security 建议分为子能力，而不是单一总开关。

| 子能力 | 作用 | 典型依赖 | 关闭行为 |
| --- | --- | --- | --- |
| Authentication Provider | 验证设备或 Authority 证明 | Identity、产品密钥 | 不能动态认证 |
| Peer Session | 已绑定 Peer 的握手、重认证、轮换和失效 | Static Identity、Authentication Provider、Persistence Foundation/等价单调 Provider | 不具备 Peer 安全 |
| Hop Protection | 每跳认证、Replay | Peer Session、Wire | 要求 Hop Auth 的 Path 不可用 |
| E2E Auth | 端到端来源真实性 | Endpoint Policy、E2E Context | 要求原始 Principal 的消息拒绝 |
| E2E Encrypt | 端到端机密性 | E2E Auth/AEAD Provider | 要求加密的消息拒绝 |
| Group Security | Group Key、密码学 Group Replay Window | Group、Persistence | 安全 Group 不可用 |

Security Policy Gate 始终存在。密码 Provider 和状态表才是可选模块。

“静态会话”不表示把一个运行时 Session 永久写死。静态配置只能提供 Peer Identity、信任锚和预共享/派生密钥材料；每次启动、Link reopen 或 Session Generation 变化后，仍必须由 Security Session Handshake 建立新鲜发送序号、Replay Window 和会话代际。Dynamic Admission 只在陌生或未绑定设备获得资格时调用同一认证能力。

因此以下组合必须完整可用：

```text
Static Identity/Binding
+ Security Session Handshake
+ Hop Protection
+ Persistence Foundation/等价单调 Provider
+ Dynamic Admission OFF
= 预配置安全节点重启或换 Link 后重新建立 Peer Session
```

这里的 Persistence 依赖只属于会发布受保护 `ACTIVE` Session 的代际、Sequence/Replay 父域；静态 Identity 本身和产品明确允许的 O0/H0 明文路径不因此依赖 Persistence。Security Owner 必须先把完整 Session Generation 候选交给唯一 Persistence Owner，收到 committed-record reload 精确证明后才发布 Session；不得在 Security 内保留第二套 Provider vtable 作为最终架构。

Security 的状态唯一由 Security Owner 拥有：

- Key/Suite Selector；
- Hop Sequence、密码 Nonce 和 Key Generation；
- Replay Window；
- Peer Session；
- E2E Context；
- Group Key Slot。

其他模块只能持有带 Generation 的 Security Context Handle，不能缓存密钥或复制 Replay Window。
需要把 Security replay mutation 与 Group/Transport 资源原子提交时，Security Owner 另提供固定
`UCN_V6_MAX_REPLAY_RESERVATIONS` 的 exact reservation handle：认证后先 `RESERVED`，调用方
全部下游资源预留成功后才 `CONSUMED`，否则 `ABORTED`。Handle 必须绑定 Runtime/Security/
Context/slot generation、Sequence、digest 和 deadline；错误输入不得驱逐或 lazy-expire 其他槽。

普通业务 Origin Sequence 不由 Security 泛化拥有：C1 由 Core Message Sequence Owner 分配，C2/C4 由 Flow Context Owner 分配，Transport 拥有 ACK/Fragment 位置，Operation ID 由 Service/Operation Allocator 分配。Security 在 O1/O2/H1/H3 下验证对应线上序号与当前 Key/Session 的唯一绑定，并唯一拥有密码学 Replay Window。

Hop Profile 也不是 Wire 自报能力。每个 `{link instance generation,ingress/egress context}` 只能
由 Link/Security Owner 发布一个 H0/H1/H2/H3 选择和一个 exact fingerprint；Parser 只读最小
context selector 后必须唯一解析，禁止按长度、Tag 或试 Key 猜测。H2 仅属于已认证 C2 Direct
Flow；H3 仅属于 C5 Group/Tree。没有线上 old/new Key selector 时，换钥必须 Fence+drain 后原子
替换，不能保留两个可试的 inbound mapping。关闭 Hop Protection 时，需要 H1/H2/H3 的 Path 在
创建 Request/Attempt 前拒绝，不能由 H0 静默接管。

## 9. Network 模块

Network 必须拆成 Route Store、Forwarder 和 Discovery。

### 9.1 Route Store

唯一拥有：

- Direct/Static/SoftRoute 的本地 Forward Entry；
- Advanced FlowPath 的 Active RouteSet/Path；
- Route/Path Generation；
- 静态与动态来源标记；
- 有效期、Fence 和失效原因。

Route Store 接受两种明确输入：基础 RREP 只可安装可过期 `SoftRoute`；Advanced Flow Owner 只在完整 Stage/Commit 后提交 `FlowPath ACTIVE`。两者不共用可互相冒充的权限位。

### 9.2 Forwarder

负责：

- 根据 Route/Label 查找出口；
- 检查 Hop Limit；
- 检查 Link Instance Generation；
- 执行允许的 Hop Security；
- 转发不可变 E2E Payload；
- 更新逐跳可变字段；
- 有界入队。

Forwarder 不负责：

- 发现路径；
- 选择业务语义；
- 终止 E2E Transfer；
- 执行业务 Endpoint；
- 保存第二份 Active Route。

### 9.3 Route Discovery

负责：

- 未知目标的发现；
- RREQ/RREP 反向临时状态；
- 从 RREP 安装/刷新本地 SoftRoute；
- RERR、租约和有界清理；
- 为 Advanced Flow 提供可冻结的发现事实，但不自己完成 Flow 授权。

Discovery 在同一模块内区分角色能力：

| 角色能力 | 作用 | 最低依赖 |
| --- | --- | --- |
| DISCOVERY_ORIGINATE | 为本节点目标发起发现并接收结果 | Route Store、C0 Control、必要 Security |
| DISCOVERY_RESPOND | 作为目标或已有合法路径持有者应答 | Route Store、Identity/Binding、必要 Security |
| DISCOVERY_RELAY | 转发发现控制、维护反向临时状态 | Forwarder、Route Store、必要 Hop Security |
| DISCOVERY_AUTHORIZE | 对候选或地址/目录结果给出 Authority 证明 | 对应 Authority Owner，不由普通叶子隐式获得 |

只发起寻路而不承担中继的叶子节点不因启用 `DISCOVERY_ORIGINATE` 被迫编译完整 Transit Forwarder；只有 `DISCOVERY_RELAY` 强依赖 Forwarder。Capability 的 `CAN_SEND/CAN_FORWARD/CAN_TERMINATE/CAN_AUTHORIZE` 必须进入构建依赖和远端准入，不能只作为诊断位。

关闭 Discovery 后：

- Direct Binding 正常；
- 静态 Route 正常；
- 固定多跳正常；
- 未知目标返回 `UNREACHABLE`；
- 不产生发现控制流量。

### 9.4 三种网络组合

```text
静态直连：Direct Binding

固定多跳：Route Store + Forwarder + Static Route Provider

自动组网：Route Store + Forwarder + Route Discovery
```

## 10. Flow 模块

Flow 的作用是把 Discovery 事实通过可对账 Stage/Commit 转成稳定 FlowPath，并减少稳态报文重复字段和规划成本。它不维护第二张基础 Forward Table：本地 Label/Path 发布仍通过 Route Store/Forwarder 唯一所有链。

Flow Owner 唯一拥有：

- 端到端业务 Flow 定义；
- Endpoint、Interaction、Delivery、Security、Realtime 合同摘要；
- Flow Context ID/Generation；
- 建立、更新、过期和 Fence；
- 稳态 Fast Path Execution Binding。
- 不可变 Flow Proposal、Stage/Commit/IN_DOUBT 与 terminal receipt。

Network/Forwarder 可拥有本地 Label 转发表，但不拥有完整业务语义。

Fast Path Execution Binding 属于 Flow/Runtime 基础执行合同，只引用已提交 Route、Security、Policy 和时间 View。Advanced Planner 可以提出候选和优化建议，但关闭 Planner 后，Runtime 仍能建立、验证和使用固定规则生成的执行绑定；Flow 不得 include 或依赖 `planner_private` 状态。

Flow 关闭后：

- 使用 C1 或允许的显式寻址 Contract；
- 自动寻路仍可工作；
- Request/Result、Reliable 和 Security 仍可工作；
- 稳态消息不能使用需要 Flow Context 的短格式；
- 普通消息不产生 Flow Setup 流量。

Flow 的关键依赖不是 Discovery，而是：

- Wire Contract；
- Endpoint；
- Route/Direct Path View；
- 需要时的 Security Context；
- Context Generation/Fence。

若候选来自自动 Discovery，Flow 只消费已冻结的发现事实；若来自静态 Path Provider，则不需要 Discovery 模块。完整事务见[Advanced Route 与 Flow 简化设计](UCN_v6_逻辑模型与伪代码/24-Advanced-Route与Flow简化设计.md)。

## 11. Transport 模块

Transport 回答“消息如何完整到达”，不回答“业务副作用是否只执行一次”。

### 11.1 Reliable Delivery

拥有：

- 发送事务；
- ACK/SACK；
- retry；
- delivery deadline；
- per-peer/path credit；
- 发送完成层级；
- 首次 Origin protect 后不可变的 `ORIGIN_SEALED` artifact、holder 与 terminal receipt。

Reliable 必须至少有两条完整 Wire 路径：

| 场景 | Wire 路径 | 是否依赖 Flow |
| --- | --- | --- |
| 偶发、未建立 Context 的小消息 | C1 Reliable + 显式 Delivery ACK | 否 |
| 已建立稳定 Context 的消息 | C4 Reliable + Flow 内 ACK/Credit | 是 |

C1 Reliable 的事务键、ACK Payload、重复报文处理和安全绑定由低开销 Wire 文档精确定义。模块只在真实存在完整 C1 确认路径时才允许声明“Flow OFF + Reliable ON”；否则该组合必须在构建或初始化时拒绝。

Reliable 重传不能再次消费 Origin Sequence/Nonce。首次保护后，Transport 只重发同一 sealed
E2E bytes；每跳 Hop protection 可按当前下一跳重新生成。Security Replay 只给出 replay
candidate 与 canonical digest，Transport receipt Owner 负责 exact/conflict 裁决；Security OFF
的 O0 Reliable 只有在 Endpoint 为 `PUBLIC_UNAUTHENTICATED`、无权限副作用，且当前 Planner/
Route Owner 持有绑定精确 Link/Path Generation 的 `TRUSTED_LINK_ALLOWED` 时，才能使用
Transport 去重，不得宣称已认证重复。

### 11.2 Fragment/Reassembly

拥有：

- Fragment Window；
- Reassembly Buffer；
- Fragment bitmap；
- message length 与 Wire 2.6 `UCN_V6_DIGEST_SUITE_1` canonical digest；
- timeout 和固定资源上限；
- 首片前经唯一 C0 Transport-Control 承载的 `TRANSFER_SETUP/TRANSFER_SETUP_ACK/`
  `TRANSFER_ABORT/TRANSFER_TERMINAL_RECEIPT`；
- 独立于 Security Session、由 durable checked-next 高水位拥有的 C1 Transport Parent，及
  C1 Transport Parent/C2/C4 Flow 父代际内单调、不回绕的 Transfer ID 高水位；
- C1 Fragment 每片显式携带并验证 4 B Setup Parent Generation；C2/C4 由不可变 Flow Context
  证明父代际，不重复该字段；
- Fragment 的 16-bit Index/Kind 使用 bit15=0，反向 SACK/Credit 在同一位置使用固定值
  `0x8000`，不能靠长度或方向猜消息子型；C1 SACK 使用保留 Transport Feedback Service 并
  显式携带 Original Service 与 Parent Generation，C2/C4 SACK 只能使用 Flow 建立时原子创建、
  绑定正向 Flow Fingerprint 的成对反向反馈 Flow。

One-way Transfer 没有 Operation ID，但完整身份仍绑定 principals/bindings、Service、安全域、
父 Context、Transfer ID 与 Setup digest；Request/Result/Error Transfer 必须再绑定非零
Operation ID。Service、总长度、摘要、Delivery/Interaction 和重组进度属于单次 Transfer 状态，
不写回长期 Flow Context。Digest primitive 属于 Transfer/Wire 的固定 Suite 1，即使 Security OFF
也必须存在；O0 digest 只作比对，不升级为认证证明。

Request/Result/Error Transfer Setup 必须携带与普通 Operation Envelope 相同的 Operation Flags，
Result/Error 还携带相同 Result Code；是否分片不能改变幂等/持久执行语义。Transport 必须为活动
窗口及合法 replay/terminal-retention 期静态保留逐片 `{Origin Sequence,AAD digest,Payload
digest,outcome}` evidence；Buffer 退休不能提前删除 exact-replay 证明。

### 11.3 共享机制但不混淆语义

Reliable 小消息和可靠大消息可以共享：

- Sequence；
- ACK timer；
- retry budget；
- congestion/credit；
- completion dispatcher。

但 Fragment 关闭时，单帧 Reliable 仍应工作。

### 11.4 Carrier 分段不是 Transport Fragment

Classic CAN 为承载一个完整 UCN Frame 所做的 Carrier 分段属于 Adapter。它不得依赖端到端 Fragment 模块。

Transport 关闭后：

- 单帧 Best Effort 正常；
- 超过单帧 Payload Budget 的发送明确失败；
- 要求 Reliable 的发送明确失败；
- 不占用 Transfer Slot 和 Reassembly Buffer。

## 12. Service 与 Operation

### 12.1 Service/RPC

负责：

- Request/Result/Error；
- Operation ID 关联；
- 易失 pending；
- 业务超时；
- 重复 Result 的有界处理；
- Completion 交付。

Request/Result 不等于 Reliable，也不等于持久化。

### 12.2 Durable Operation

负责：

- 持久化去重；
- `PREPARED/EXECUTING/COMMITTED_RESULT/IN_DOUBT/TOMBSTONED`；
- persist-before-execute；
- 结果重放；
- 掉电后的对账；
- 固定 Journal 槽和 GC；
- anti-rollback Witness。

在 `PREPARED` 和任何执行器可见副作用之前，Operation Owner 必须一次预留 Journal、Persistence
continuation、独立 Operation reply receipt、必有 query/retention/reconciliation obligation、
仅在 Endpoint 配置回调时存在的 callback-delivery obligation，以及
Endpoint 声明上限内的固定 inline result bytes。执行后不得再申请终态资源；当前基础合同只支持
`max_durable_result_bytes <= UCN_V6_DURABLE_RESULT_INLINE_BYTES`。超出上限且副作用可能已发生
时使用预留资源进入 `IN_DOUBT` 并 Fence，不能重试执行器。大结果未来必须另行冻结原子 Blob
Provider、ref+digest 和恢复顺序。

依赖：

- Service/RPC；
- Security Principal；
- Persistence Provider；
- Operation ID；
- Core Owner Fence。

关闭 Durable Operation 后：

- 普通 One-Way 正常；
- Request/Result 正常；
- Reliable 正常；
- 不承诺跨重启去重；
- 请求 `DURABLE_AT_MOST_ONCE` 时明确拒绝。

### 12.3 Persistence 是独立基础设施模块

Persistence 不应隐藏在 Cluster、Admission 或 Operation 内部。它提供：

- 固定 Record/Blob 槽；
- schema/version/layout 校验；
- submit/poll/load；
- completion journal；
- anti-rollback Witness；
- torn-write 恢复；
- 指定 Record 已 durable、reload 一致且满足存储单调合同的证明。

Persistence Owner 唯一拥有 Provider I/O 状态和提交序列。调用模块提交规范化的 operation 和 next snapshot，但不能直接操作 Flash。

`persist-before-promise` 的最终责任分为两层：

| 层次 | 能证明什么 | 不能证明什么 |
| --- | --- | --- |
| Persistence Owner | 指定调用者/事务/operation/record 已按合同提交、读回一致、版本和单调关系成立 | 当前业务 Authority、租约、Path、Config 或 Policy 仍有效 |
| 调用模块 Owner | persistence I/O completion/durability proof 精确属于当前事务，并且完成时刻的身份、租约、Config、Authority、Policy 仍允许发出承诺 | Flash 是否真的按存储合同完成 |

Persistence 的 durable journal 只绑定可跨重启恢复的 Realm/Owner domain、transaction ID、
operation type/ID、record fingerprint 和预期前态/后态；Runtime/Owner/Persistence Instance 与
pending token 只属于本次启动的易失 completion routing，不得写进 durable fingerprint。
同一次启动中的 completion proof 必须同时匹配 durable identity 和易失 routing identity；重启后
拒绝所有旧易失 completion，只能从 durable record 创建新的 recovery continuation。调用模块在
收到证明后必须重新执行当前使用点门禁；等待期间 Authority 到期、Config 改变、Session 轮换或
事务被 Fence 时，即使写入成功也不得发送旧承诺。

关闭 Persistence 后：

- 普通易失通信正常；
- 静态、编译期不可变配置仍可工作；
- 需要动态 Binding 持久化、Durable Operation、生产 Cluster Authority 或持久 Key Generation 的配置必须拒绝；
- 不得用 `VOLATILE_TEST` 冒充掉电安全证明。

## 13. QoS 模块

QoS 模块在 Basic Queue 之上增加：

- Q0～Q3；
- Weighted Scheduling；
- per-source/per-flow quota；
- Latest；
- 聚合；
- Deadline-aware ordering；
- inflight budget；
- 分类统计。

QoS 不允许：

- 把低级 Traffic Class 升级为高级；
- 让 Deadline 抢占其他 Class 的保留资源；
- 用极小 remaining budget 注入无限优先级；
- 依赖完整业务 Payload 才能转发；
- 在队列后才发现必需模块不存在。

QoS 关闭后仍保留 Core Basic Queue，但不得宣称具备 Q0～Q3 比例、Latest、公平性或 Deadline 调度保证。

## 14. Realtime 模块

Realtime 建议继续分为：

- Time Domain；
- Sync Provider；
- Timed Link/Event Key；
- Realtime Envelope；
- Deadline Gate；
- 可选 Hop Scheduling Budget。

Realtime 关闭后：

- Core Local Time 正常；
- retry、lease、queue timeout 正常；
- 普通消息不携带 Realtime Envelope；
- `SYNCED_STAMP/DEADLINE` Intent 明确拒绝；
- `LOCAL_STAMP` 是否可用由单独轻量 Feature 决定；
- Cluster 不得因为使用本地租约而依赖 Time Domain。

Realtime 的子能力依赖必须分开：

| 子能力 | 是否要求 Flow | 说明 |
| --- | --- | --- |
| Core Local Time | 否 | 最小内核能力 |
| LOCAL_STAMP | 否 | 本地记录，不上线或使用轻量本地元数据 |
| 外部已认证 Time Domain Provider | 否 | 由产品 Provider 直接更新 Domain |
| UCN Network Time Sync v1 | 是 | 当前 Wire 明确使用专用 C4 Time Sync Flow |
| SYNCED_STAMP/DEADLINE | 取决于所选 Wire Contract | 必须具有可表达 Domain/Envelope 的完整路径 |

Realtime 必须通过只读 Route/Path View 使用冻结路径，不能拥有 Route。

## 15. Group 模块

Group 负责：

- Group ID/Generation；
- 固定成员或动态成员集合；
- Group Context；
- Tree 或多出口发送；
- Group Key 引用；
- 组业务重复交付状态；
- 有界成员和转发状态。

Group 不负责选举管理 Authority。固定 Group 可以由产品 Manifest 创建，不依赖 Cluster。

Group 的依赖必须按创建方式拆开：

```text
GROUP_STATIC
  requires GROUP + Manifest fixed slot
  does not require runtime Persistence

GROUP_DYNAMIC
  requires GROUP + IDENTITY + SECURITY/AUTH_PROVIDER + PERSISTENCE
           + valid GROUP_AUTHORITY

GROUP_SECURE
  requires GROUP + GROUP_SECURITY + PERSISTENCE
```

`GROUP_AUTHORITY` 不是一个可以由任意 Group 成员自报的 Feature bit，而是动态 Group 的唯一
控制权证明。每个 Realm/Group Scope 必须在 Manifest 中选择且只选择一种 Owner 模式：只读
签名静态 Manifest，或持有当前 Generation、Lease、Fence、quorum 和认证证明的唯一逻辑 Realm
Address Authority。Cluster Head 不能接受委派成为第二个 Group Authority；它只能向 Realm
Authority 提出变更请求。`GROUP_AUTHORITY` 的线上更新必须经 Identity 解析、Security
认证/ACL、Persistence persist-before-use 后，Group Owner 才能消费。因此构建系统不得接受
只有 `GROUP + PERSISTENCE` 而没有 Identity、认证 Provider/Security 的动态 Group 组合。

静态槽的 `NEVER_ACTIVATED/ACTIVE/RETIRED` 由平台 anti-rollback 保护的签名 Manifest 决定，
运行期只读且不使用 UCN Persistence；要运行期更新/退休必须选动态模式。动态 Group ID 只能从
持久化单调高水位分配，删除 Group 不得回退高水位或复用 `RETIRED` 槽。

Group Key 和密码学 Replay Window 始终由 Security Owner 管理；Group 只通过 Group Security Handle 请求认证/解密。若产品还需要抑制已经通过安全验证、但业务语义重复的组消息，Group 可以拥有独立的“组业务重复交付状态”，但不得把它命名或实现为第二份密码学 Replay Window。

关闭 Group 后：

- 单播正常；
- 对多个目标可由应用分别单播，但协议不承诺原子组交付；
- 不发送 Group HELLO；
- 不保留 Group/Key 表；
- GROUP Intent 明确拒绝。

## 16. Cluster 模块

Cluster 负责：

- Member；
- Config；
- VoterSet；
- Epoch；
- Head/Backup；
- Takeover/Recovery；
- Handover；
- Federation/Directory；
- Cluster Persistence。

Cluster 可使用：

- Identity/Security；
- Persistence；
- Network；
- Transport；
- 可选 Group。

当前低开销 Wire 的 Cluster 控制建立在 C4/C5 上，因此目标依赖冻结为：

```text
CLUSTER_BASE
  requires FLOW + C4 + IDENTITY + SECURITY + PERSISTENCE + ROUTE_STORE

CLUSTER_GROUP_ACCELERATION
  requires CLUSTER_BASE + GROUP + C5
```

Group 是 Cluster 的可选组播/树加速能力，但 Flow/C4 在当前 Wire 方案中是 Cluster 基础控制的硬依赖。若未来希望支持 `Cluster ON + Flow OFF`，必须先定义、审计并测试另一条完整 Cluster Control Contract，不能仅删除依赖位。

但普通通信不得依赖 Cluster。

Cluster 关闭后：

- 静态直连、固定多跳、自动寻路和安全通信仍可工作；
- 不保留 Cluster Member/Config/Vote/Tombstone/Directory/Tunnel 状态；
- 不发送 Cluster 控制帧；
- 不存在隐式 Head Authority。

## 17. Planner 与 Resolver

本节的简化权威和完整准入顺序见[Capability 与 Contract Resolver 简化设计](UCN_v6_逻辑模型与伪代码/23-Capability与Contract-Resolver简化设计.md)。

### 17.1 Core Intent Resolver

Resolver 是统一 API 的必选部分，但必须保持小、确定性和有界。

处理顺序：

```text
合并 Product Default
  → Endpoint Policy
  → Per-send Intent
  → Expert Override
  → Hard Constraint Intersection
  → 编译能力检查
  → 当前状态与资源检查
  → 选择已存在的最低成本合法路径
```

Resolver 不执行复杂搜索，也不直接修改模块状态。

Resolver 的输出是纯决策，例如：

```text
READY(plan_inputs)
NEED_DEPENDENCY(typed DependencyRequirement)
WAIT_EXISTING(exact DependencyHandle)
REJECT_UNSUPPORTED
REJECT_POLICY
REJECT_RESOURCE
```

这些是语义类别，最终枚举名可以在 API RFC 中冻结。Resolver 返回所需动作及其不可变输入，不自己分配事务槽、发送控制帧或推进模块 FSM。

### 17.2 Runtime Setup Coordinator

Runtime 中的 Setup Coordinator 负责执行 Resolver 返回的动作：

- 分配一个有界 Send Request；
- 申请 Discovery/Security/Flow/Transport 资源；
- 管理准备阶段 Pending；
- 保留原始 Timeout/Deadline 起点；
- 接收模块 Completion；
- 重新调用 Resolver；
- 最终 Send Admission；
- 失败、取消和资源退休。

Setup Coordinator 只协调，不拥有 Route、Session、Flow 或 Transfer 的内部状态。模块返回 opaque handle 和 completion；Coordinator 保存请求级依赖与 obligation。

### 17.3 Advanced Planner

可选能力包括：

- 多候选评分；
- 自动 Route Discovery；
- 自动 Flow 提升；
- 多 Path；
- 吞吐、延迟、能耗权衡；
- 聚合收益预测；
- 自动恢复和降级建议。

Advanced Planner 只产生候选、评分和优化建议，不分配资源，也不直接请求模块改变状态。Runtime Setup Coordinator 根据最终 Resolver 决策，通过各模块受控 API 建立资源。

### 17.4 Advanced Planner 关闭后的行为

- 静态和默认策略仍可发送；
- 已有合法 Path 可直接使用；
- Resolver 仍可返回 `NEED_DEPENDENCY(typed DependencyRequirement)`；SoftRoute、Security Session、Flow 和 Transfer 只是同一 tagged union 的不同 kind；
- Runtime Setup Coordinator 仍可按固定策略调用已编译模块；
- 不进行多候选评分；
- 不改变 API 语义。

## 18. Diagnostics 模块

Core 必须保留：

- 最终错误码；
- 最低限度的 queue/resource/fault 计数；
- 当前 Feature Manifest；
- Build/Profile/Layout 信息。

Diagnostics 模块增加：

- Trace；
- Route/Flow/Transfer 快照；
- 详细分桶；
- 历史事件；
- Host 导出；
- 调试控制 Endpoint。

Diagnostics 关闭后，正常协议行为和 Golden Bytes 不得变化。

## 19. Adapter、Port、Runtime 必须分离

### 19.1 Runtime

Runtime 负责：

- 装配所选模块；
- 接受应用命令；
- 接受 Adapter 事件；
- 调用 Owner；
- 按预算推进模块；
- 连接失效通知；
- 统一 Completion。

Runtime 不应由“是否编译参考 Adapter”决定是否存在。

### 19.2 Adapter

每种 Adapter 单独选择：

- UART；
- CAN；
- CAN-FD；
- USB；
- Wi-Fi/ESP-NOW；
- UDP；
- 产品自定义 Link。

Adapter 负责 Carrier 封装、RX/TX item、Driver token 和 Link Instance，不拥有 Route、Security 或业务状态。每个允许提交的 TX token 必须在调用 Driver 前同时拥有一个不可失败的内嵌
completion latch；Driver/ISR 把 exact `{link instance,token,generation,result}` 原子锁存到 token，
外层事件队列只负责可合并、可丢失的唤醒。队列满不能丢失完成证明。submit 返回值和早到
completion 必须在同一 gate 下合并，终态只推进一次。

所有 `submit/open/close/reopen/reconcile` Driver 路径统一调用唯一的
`call_driver_with_gate_held()` 模板：先 `try_acquire` 共享 callback-domain gate，再在 Adapter
event gate 下重校验并一次发布完整状态/continuation bundle，设置 callback-active，调用 Driver，
在同一 event gate 合并早到 latch，最后由单一 epilogue 清除 callback-active 并释放 gate。任何
生命周期路径都不得直接调用 Driver，`stop()` 只能先发布不依赖 Driver 的工作标记；旧 Link
重开必须先进入 `CLOSING_OLD`，旧 obligation 清零后才能 `OPENING_NEW`。

### 19.3 Port

Port 只提供：

- 临界区；
- 原子操作；
- 单调时钟；
- 通知/唤醒；
- 可选持久化和熵 Provider 接口。

Core 不依赖 FreeRTOS、Zephyr 或 Linux。不同平台只能实现 Port，不改变协议语义。

## 20. 唯一状态所有权表

| 状态或资源 | 唯一 Owner | 其他模块如何使用 |
| --- | --- | --- |
| Build Manifest/Layout | Config | 只读常量或 View |
| Protocol phase/reentry/fence | Protocol Owner | 受控入口 |
| Driver token/Link instance | Adapter | 带 generation 的 handle |
| Device Principal/Binding | Identity | 只读 identity view |
| C1 Origin Sequence | Core Message Sequence Owner | 分配结果；Security 只验证密码绑定/Replay |
| Admission pending | Admission | transaction handle |
| Hop Sequence/Key/Nonce/密码 Replay | Security | security context handle |
| Active RouteSet/Path | Route Store | route/path handle |
| Route Candidate/Probe | Discovery | 冻结候选提交 |
| Endpoint Flow/Context | Flow | flow handle |
| C2/C4 Origin Sequence | Flow | 当前 Flow 的发送序号结果 |
| Forwarding Label | Forwarder | local label lookup |
| Reliable ACK/Fragment window | Transport | transfer handle |
| Request correlation/Operation ID allocation | Service | request/operation handle |
| Durable journal | Operation | operation handle |
| Provider I/O/Record/Witness | Persistence | durability proof/loaded snapshot |
| Q0～Q3/flow quota | QoS | enqueue admission |
| Time domain/uncertainty | Realtime | clock view |
| Group membership/context/业务重复交付 | Group | group handle |
| Cluster Config/Epoch/Authority | Cluster | cluster authority view |
| Request/Attempt/Buffer obligation | Runtime Setup Coordinator | request/attempt/buffer handle |
| Plan candidate/score | Advanced Planner | 短期只读建议 |
| Trace/history | Diagnostics | 只读导出 |

任何模块都不得保存另一个 Owner 状态的无代际永久副本。同一个线上 Sequence 字段也只能有一个发送分配者；Security、Transport 或 Flow 如果需要使用该值，只能消费 Owner 已分配的结果，不能各自维护会产生冲突的副本。

Direct Binding 中的 Target Binding 是 Identity Owner 的带代际引用或不可变静态 Manifest 项，不是第二个可以独立更新的地址权威。

## 21. Dependency 与失效传播

### 21.1 依赖必须有 Generation

跨模块长期引用至少包含：

- 具体 Runtime/Owner Instance 标识；
- 对象 ID；
- Generation；
- 必要的 policy/config digest；
- 失效原因或 Fence 状态。

不能只保存数组下标或裸指针，并假设它永久有效。Owner 标识必须区分两个不同 Runtime 实例；即使二者的对象 ID 和 Generation 数值相同，句柄也不得串用。

每次使用长期句柄时，完整有效性条件是：

```text
Runtime/Owner Instance 匹配
+ Object ID 与 Generation 匹配
+ 未被 Fence
+ trusted now 仍在有效期内
+ 当前 Policy/Config 仍允许
+ 本次资源准入仍成立
```

Generation 相同只是必要条件，不是充分条件。后台清扫尚未运行时，已经过期的 Path、Session、Capability Lease 或 Authority 仍必须在使用点立即拒绝。

### 21.2 失效顺序

统一顺序为：

```text
检测到依赖失效
  → 立即撤销使用资格/Fence
  → 拒绝依赖该资格的新发送、状态提交和授权副作用
  → 发布有界清理 obligation
  → 退休 Driver token、Buffer、pending、timer
  → 回收固定槽
```

资格撤销不能依赖一个可能满载的事件队列。事件用于清理，Fence 用于立即保证安全。Fence 必须有作用域：Cluster Authority Fence 只阻断依赖该 Authority 的 Cluster 行为，不冻结普通业务；某个 Operation/Persistence 事务故障也不能无条件冻结整个节点。清理、取消确认、Driver completion 和资源退休始终允许继续推进。

### 21.3 典型传播

Link reopen：

```text
Link generation 改变
  → 旧 Path 立即不可用
  → 旧 Flow Fast Plan 立即不可用
  → 相关 Transfer 暂停或失败
  → Driver token 有界退休
```

Security revoke：

```text
Security context generation 改变
  → 要求认证的 Route/Flow/Request 立即不可提交
  → 已排队但未发出的旧上下文帧撤销
  → Replay/Nonce 状态按 Owner 合同退休
```

Cluster Authority 丢失：

```text
Authority Fence
  → Cluster 控制发送停止
  → 普通业务通信继续按自身 Route/Security Policy 运行
```

## 22. 静态装配与构建配置

### 22.1 推荐的配置入口

建议由产品选择 Composition：

```cmake
-DUCN_COMPOSITION=STATIC_DIRECT
-DUCN_PROFILE=NANO
```

高级产品可覆盖独立 Feature：

```cmake
-DUCN_MODULE_ROUTE_DISCOVERY=ON
-DUCN_MODULE_FLOW=ON
-DUCN_MODULE_E2E_SECURITY=ON
-DUCN_MODULE_TRANSFER_FRAGMENT=OFF
```

Composition 只是经过验证的 Feature 集合，不是另一套协议实现。

### 22.2 生成统一 Build Config

CMake 或外部产品工具生成：

```text
ucn_v6_build_config.h
  ├─ UCN_V6_MODULE_*_ENABLED
  ├─ UCN_V6_PROFILE
  ├─ UCN_V6_COMPILED_FEATURE_BITS
  ├─ UCN_V6_COMPILED_LAYOUT_HASH
  ├─ UCN_V6_*_STORAGE_BYTES
  └─ UCN_V6_*_STORAGE_ALIGNMENT
```

手工集成源码时也必须使用同一个生成配置，不能由不同 `.c` 文件看到不同 Feature 值。

### 22.3 依赖检查

示例：

```text
DISCOVERY_ORIGINATE requires ROUTE_STORE + C0_CONTROL
DISCOVERY_RESPOND   requires ROUTE_STORE + IDENTITY + C0_CONTROL
DISCOVERY_RELAY     requires ROUTE_STORE + FORWARDER + C0_CONTROL
FLOW_ROUTED         requires FLOW + ROUTE_STORE + FORWARDER
PEER_SESSION        requires IDENTITY + AUTH_PROVIDER
E2E_ENCRYPT         requires E2E_AUTH + AEAD_PROVIDER
DYNAMIC_ADMISSION   requires IDENTITY + AUTH_PROVIDER + PERSISTENCE
TRANSFER_FRAGMENT   requires DIGEST_SUITE_1 + (TRANSFER_RELIABLE or explicit best-effort policy)
TRANSFER_C1         requires TRANSFER_FRAGMENT + C1_TRANSPORT_PARENT
C1_TRANSPORT_PARENT requires PERSISTENCE or AUDITED_MONOTONIC_WITNESS
RPC_DURABLE         requires RPC + OPERATION_JOURNAL + PERSISTENCE
REALTIME_NETWORK_SYNC_V1 requires TIME_DOMAIN + TIMED_LINK + FLOW + C4
GROUP_STATIC        requires GROUP + MANIFEST_FIXED_GROUP_SLOTS
GROUP_AUTHORITY     requires IDENTITY + SECURITY/AUTH_PROVIDER + PERSISTENCE
GROUP_DYNAMIC       requires GROUP + GROUP_AUTHORITY
GROUP_SECURE        requires GROUP + GROUP_SECURITY + PERSISTENCE
CLUSTER_BASE        requires FLOW + C4 + IDENTITY + SECURITY + PERSISTENCE + ROUTE_STORE
CLUSTER_GROUP_ACCEL requires CLUSTER_BASE + GROUP + C5
```

依赖不成立时优先在 CMake configure 或编译期失败；涉及 Provider 可用性的条件在 init 时失败。

### 22.3.1 能力—Contract—角色矩阵

每个 Feature 还必须列出真实 Wire 路径，不能只列源码依赖：

| 能力 | 可用 Contract | Sender | Relay | Terminator/Authority | Flow OFF 结果 |
| --- | --- | --- | --- | --- | --- |
| 普通显式数据 | C1 + Direct/Static/SoftRoute | C1 send | C1 forward | Endpoint terminate | 完整可用，不运行 Stage/Commit |
| 一跳稳定 Flow | C2 | Flow send | 不适用 | Flow terminate | 不可用，回到合法 C1 |
| 最短 Label 转发 | C3 + committed FlowPath | C3 send | Label forward | C3 terminate | 不可用 |
| 可靠/安全 Routed Flow | C4 + committed FlowPath | Flow/Transport send | C4 forward | Flow/Transport terminate | 使用已定义 C1 Reliable 或拒绝 |
| C1 大消息 Transfer | C1 + C0 Transport-Control | Transport Parent/Transfer send | C1 forward | Transfer terminate | Flow 本身不要求；但没有 Persistence/等价单调 witness 时拒绝 C1 Transfer |
| Group | C5 | Group sender | Tree forward | Group terminate | 与 Flow 无直接等价替代 |
| UCN Network Time Sync v1 | C4 Time Sync Flow | Time source/member | C4 forward | Time Sync terminate | 不可用；外部 Time Provider 不受影响 |
| Cluster Base Control | C4 | Cluster role | C4 forward | Cluster Authority/Member | 当前方案拒绝 |
| Cluster Group Acceleration | C5 | Cluster/Group role | Tree forward | Cluster role | 回到 C4 Cluster Base |

最终冻结前，这张矩阵必须扩展到每个 Control/Transfer Opcode，并逐项给出所需 `CAN_SEND/CAN_FORWARD/CAN_TERMINATE/CAN_AUTHORIZE`、安全合同、事务键和缺失依赖行为。

### 22.4 不使用强制隐式开启

禁止这样的行为：

```text
打开 Cluster
  → CMake 静默打开 Realtime、Group、所有 Adapter 和 Diagnostics
```

正确行为是打印缺失依赖并拒绝配置，让产品明确选择。

### 22.5 公共头和安装包也必须反映裁剪结果

安装面建议分为：

- 永远安装的 Kernel contract/API 头；
- 仅对应模块启用时安装或导出的模块头；
- 生成的 `ucn_v6_build_config.h`；
- 只包含实际目标的 `UCNTargets.cmake`；
- 可供消费者检查的 Feature Manifest。

统一应用 API 可以始终存在，但不得通过公共 umbrella header 无条件暴露所有模块私有 API。Feature-off 消费者包含对应模块头时，应在编译期得到明确诊断，而不是直到链接时才发现缺少 archive。

### 22.6 Layout Hash 只属于本机 ABI

`UCN_V6_COMPILED_LAYOUT_HASH` 用于证明同一设备上的应用、静态库、Storage 和产品配置采用相同布局。它不得进入节点间兼容判断，也不要求 Nano 端点和 Full 网关相等。

节点间互通只比较：

- Protocol Version；
- Wire Contract/Opcode；
- 发送、转发、终止和授权角色能力；
- Address/MTU/Payload Budget；
- Security、Realtime、Transfer 等当前有效 Capability；
- 对应 Policy 和 Generation。

## 23. Storage 与固定资源

### 23.1 每个模块独立 Storage

推荐由调用方静态提供：

```c
UCN_V6_ALIGNED_STORAGE(kernel_storage,
                       UCN_V6_KERNEL_STORAGE_BYTES,
                       UCN_V6_KERNEL_STORAGE_ALIGNMENT);

#if UCN_V6_MODULE_ROUTE_DISCOVERY_ENABLED
UCN_V6_ALIGNED_STORAGE(discovery_storage,
                       UCN_V6_DISCOVERY_STORAGE_BYTES,
                       UCN_V6_DISCOVERY_STORAGE_ALIGNMENT);
#endif
```

禁止让一个公共 Runtime Storage 无条件预留所有可选模块的最大数组。

C1 Transfer 启用时还必须由产品配置显式给出：

```text
UCN_V6_C1_TRANSPORT_PARENT_SLOTS
UCN_V6_C1_TRANSFER_ACTIVE_SLOTS
UCN_V6_C1_TRANSFER_RECEIPT_SLOTS
UCN_V6_C1_FRAGMENT_EVIDENCE_SLOTS
```

Parent 槽保存 canonical 双方 Principal/Binding、Transport Policy、可选 Security generations、
Parent Generation 与 Transfer-ID high-water；receipt/evidence 在重组 Buffer 释放后仍按冻结窗口保留。
所有容量为 0 时 C1 Transfer 符号和状态必须消失，不能偷偷借用 Flow、Security Session 或动态堆。

### 23.2 初始化前检查

每个 `init_in_place()` 必须检查：

- 指针非空；
- 容量足够；
- 对齐正确；
- Config/API version；
- Feature Manifest；
- Layout Hash；
- Provider 契约；
- 依赖 Owner Ready；
- Storage 尚未由其他实例占用。

### 23.3 资源耗尽

所有表有编译期容量。满载时：

- 不动态扩容；
- 不覆盖 Active 状态；
- 不驱逐未决安全事务；
- 不把高级模块耗尽传播成无关基础消息失败；
- 返回确定错误并更新对应统计。

例如 Transfer 槽耗尽不能阻止一个不需要 Transfer 的单帧 Best Effort 消息。

### 23.4 专用资源与共享资源必须分账

“某模块专用资源耗尽不阻断无关功能”只对专用槽成立。高级业务仍可能消耗公共 Copy Buffer、Adapter TX、Request、callback/cancel/retire obligation 和 Owner work budget，因此必须冻结共享资源配额。Driver 完成事实保存在每 token 内嵌终态 latch 中，不消耗共享“Completion 事件真值槽”；共享队列只承载可丢失 wakeup hint：

| 共享资源 | 必须定义的规则 |
| --- | --- |
| 小消息 Copy Buffer | 基础消息保留量、Bulk 最大占用、是否允许借用及归还条件 |
| Adapter TX slot | 必要控制/ACK/取消/普通数据/Bulk 的保留与上限 |
| Kernel Request slot | 每来源、每 Endpoint、每高级能力的最大占用 |
| Callback/Cancel/Retire obligation 与 wakeup hint | correctness obligation 在 Request/token 创建前预留；Driver 终态存在 token latch，wakeup 队列满可丢提示但不能丢真值；Receipt callback 由保留预算扫描 `READY→INVOKING→DELIVERED`，RUNNING/STOPPING 均推进；取消/退休拥有最低推进容量 |
| Owner work budget | 维护、Fence、取消和资源退休必须获得最低推进份额 |
| Crypto/Persistence staging | 不能由一个模块长期占满并阻断无关安全/持久化事务 |

统一规则为：

> 专用资源耗尽不直接阻断无关功能；共享资源耗尽按已声明的配额、预留、借用和背压合同处理。

### 23.5 Basic Queue 与高级 QoS 只能有一个物理所有权链

“高级 QoS 建立在 Basic Queue 之上”表示共享同一套 Buffer/Request ownership 和 Adapter admission，并替换或扩展调度策略；不得实现成 Basic Queue、QoS Queue、Adapter Queue 对同一消息无条件复制三次。

推荐形式：

```text
一个逻辑 enqueue admission
  → 一个 Buffer obligation
  → Basic Scheduler 或 Advanced QoS Scheduler 二选一
  → Adapter reservation
```

QoS 准入失败不能回退到 Basic Scheduler 绕过 Traffic、quota 或安全门禁。Adapter 内部允许拥有 Driver ring，但它持有的是同一 obligation 的下一级引用，不创建第二个逻辑 Send Request。

### 23.6 最小内核必须有数值预算

目录名为 `kernel` 不能证明实现足够小。MOD-01 必须为每个 ABI/Profile 填写下表，未经测量不得填写成产品保证：

| 最小内核项目 | 必须记录的数值 |
| --- | --- |
| Kernel/Runtime 固定对象 | Storage bytes、alignment、BSS |
| Direct Binding/Endpoint | 槽数、每槽字节、查找上界 |
| Send Request/Attempt | 槽数、每槽字节、最大并发 |
| Copy Buffer | 总字节、单消息上限、保留量 |
| Buffer Obligation/Completion | 最大数量、溢出和退休策略 |
| Basic Queue | 深度、控制保留、单次出队上界 |
| 普通发送路径 | 最大步骤、最大查表次数、最大栈帧 |
| 空闲维护 | 周期、每轮最大工作、零流量 CPU 占比 |
| 基础 Wire | 每种最小 Contract 固定字节 |

初稿阶段可以记录 `MEASURE BEFORE FREEZE`，但进入实现候选冻结前必须给出 Host 和至少一个目标 MCU 的实际值及测量方法。

## 24. 执行模型

### 24.1 默认单一 Protocol Owner

```text
应用任务 ── command ──┐
Driver/ISR ─ event ───┼─→ 固定事件槽/队列 → Protocol Owner
Timer ───── notification┘                         │
                                                 ├─ Kernel
                                                 ├─ 所需模块 step
                                                 └─ Adapter submit
```

模块不是任务。每个模块提供有界状态机接口，例如：

```c
ucn_v6_result_t module_step(module_t *module,
                            uint64_t now_us,
                            uint32_t work_budget,
                            uint32_t *work_done);
```

### 24.2 模块不得无界扫描

每次 `step()` 必须：

- 接受明确预算；
- 最多处理固定数量槽；
- 保存 cursor；
- 不阻塞等待 I/O；
- 不在锁内执行应用 callback；
- 不递归进入其他控制 API。

### 24.3 通知与轮询

Driver/ISR 通过事件或任务通知唤醒 Owner。Owner 收到通知后立即推进，但协议正确性不能依赖通知永不丢失；低频维护 Timer 可补充检查到期状态。

### 24.4 初始化阶段不能把“对象存在”与“网络已就绪”混为一谈

统一启动顺序为：

```text
1. 验证 Build Manifest、产品配置和 Storage
2. 初始化对象内存、magic、schema、局部 callback gate
3. 绑定依赖 Owner 和 Provider
4. 开放建立依赖所需的最小控制 RX/TX
5. 推进 Link/Session/Admission/Route/Flow 等状态机
6. 某项业务所需依赖全部 Ready 后，只开放该项业务
```

“Owner 已初始化可引用”与“Session/Route/Authority 已 Ready”是两个不同状态。不能要求所有安全会话 Ready 后才开放建立安全会话所需的认证控制入口。

### 24.5 停止和 Storage 复用

统一停止顺序为：

```text
拒绝新业务 Request
  → 对相关资格建立 Fence
  → 发起取消并继续接收 Driver/Provider completion
  → 退休 Attempt、Buffer、timer、pending 和 callback obligation
  → 所有模块达到 QUIESCENT
  → 才允许清零或复用 Storage
```

关闭、reset、reconfigure 和 runtime destroy 都必须服从该顺序。活动 callback、DMA、Driver token、持久化 I/O 或远端等待义务存在时，不得通过一次 `memset` 使对象看似 FREE。

## 25. Wire 裁剪合同

### 25.1 模块关闭不改变无关帧

对相同：

- Message semantic；
- Security contract；
- Address/Flow context；
- Payload；
- Sequence；

启用或关闭无关模块后，编码字节必须完全一致。

例如 Cluster OFF 不得改变普通 C1 Data；Realtime OFF 不得让普通消息多一个“Realtime Disabled”字段。

### 25.2 不支持能力必须严格拒绝

节点收到要求本地终止处理的未知模块 Opcode 时必须拒绝。中继是否允许透明转发由 Contract 和 Capability 明确决定，不能把“能转发”解释成“能终止”。

Capability 建议区分：

- `CAN_SEND`；
- `CAN_FORWARD`；
- `CAN_TERMINATE`；
- `CAN_AUTHORIZE`。

### 25.3 普通消息零专用成本

模块的专用字段只能出现在：

- 使用该模块的 Contract；
- 对应 Payload Envelope；
- 建立 Context 的控制帧；
- 明确的安全 Tag。

不得在统一基础 Header 中为所有消息永久预留 Cluster、Realtime、Transfer、Group 或 Diagnostics 字段。

## 26. 用户 Intent 到执行计划

### 26.1 公共调用模型

概念上保持：

```c
ucn_publish(node,
         target,
         endpoint,
         payload,
         payload_size,
         options,
         optional_out_handle);
```

`options` 表达业务约束，不直接暴露模块内部状态。

### 26.2 解析流程

```text
1. 合并 Default/Endpoint/Per-send/Expert 约束
2. 检查 Target 与 Endpoint
3. 检查要求的 Delivery/Interaction/Execution
4. 检查 Security 最低要求
5. 检查 Realtime/Performance 约束
6. 检查 Path 偏好和当前已有路径
7. 检查编译能力与实时资源
8. 生成最低成本合法 Candidate
9. Resolver 返回 READY、NEED_* 或拒绝原因，不产生副作用
10. Runtime Setup Coordinator 按固定策略启动允许的 Discovery/Security/Flow/Transfer
11. 模块完成后重新解析，不继承过期资格
12. 最终提交前检查 Runtime/Owner Instance、Generation、Fence、有效期、Policy 和资源
```

### 26.3 自动选择原则

Resolver 应优先选择：

1. 已存在且仍合法的最低开销计划；
2. 不新增控制事务的计划；
3. 不使用超出 Intent 的安全或可靠性成本；
4. 满足硬约束后，才比较延迟、吞吐、能耗；
5. 稳定流达到收益阈值后才升级 Flow；
6. 低于收益阈值或依赖变化时回到无状态路径；
7. 不允许为了发送成功自动降低认证、加密、Deadline 或 Pinned Path 要求。

## 27. 推荐 Composition

| Composition | 主要用途 | 必选模块 | 常见可选项 |
| --- | --- | --- | --- |
| STATIC_DIRECT | 两板 UART/CAN/USB 固定通信 | Kernel、Static Identity、Direct Binding、一个 Adapter | Hop/E2E Security、Flow |
| STATIC_ROUTED | 固定拓扑多跳 | STATIC_DIRECT、Route Store、Forwarder | Hop Security、Flow |
| AUTO_MESH | MCU 自动组网 | Kernel、Identity、Admission、Security、Route Store、Forwarder、Discovery | Flow、QoS、Reliable |
| SECURE_AUTO_MESH | 不可信无线网络 | AUTO_MESH、Hop Security、E2E Auth | E2E Encrypt、Group |
| RELIABLE_CONTROL | 参数查询与控制 | STATIC/AUTO Network、Security、Reliable、RPC | Durable Operation、Realtime |
| BULK_TRANSFER | 文件、日志、大消息 | Network、Reliable、Fragment、Credit、QoS | E2E Encrypt |
| CLUSTERED_NETWORK | 大规模多簇网络 | SECURE_AUTO_MESH、Persistence、Cluster | Group、Transfer、Realtime |
| REALTIME_CONTROL | 同步采样和 Deadline 控制 | Network、Security、Realtime、Timed Link | Reliable、Cluster |

这些是构建组合，不是新 Wire 版本。所有组合仍属于同一个 v6 协议。

## 28. 场景运行示例

### 28.1 固定两板 UART 状态上报

启用：

```text
Kernel + Static Identity + Direct Binding + UART Adapter
```

流程：

```text
应用提交 Target/Endpoint/Payload
  → Resolver 读取静态 Binding
  → Policy 允许受控明文或静态安全上下文
  → Wire 编码
  → Basic Queue
  → UART Adapter
```

不会运行 Discovery、Transfer、Realtime、Group、Cluster 或 Advanced Planner。

### 28.2 固定三跳可靠命令

启用：

```text
Kernel + Route Store + Forwarder + Static Routes
+ Security + Reliable + RPC
```

流程：

```text
Request Intent
  → Resolver 选择已有静态 Route
  → 建立易失 Request correlation
  → Reliable 分配事务和 retry
  → 中继只转发，不执行 RPC
  → 目标验证 E2E/ACL 后执行
  → Result 沿合法返回路径发送
```

如果没有要求 `DURABLE_AT_MOST_ONCE`，不使用 Operation Journal。

### 28.3 无线自动组网遥测

启用：

```text
Kernel + Admission + Peer/Hop Security
+ Route Store + Forwarder + Discovery
+ 可选 Flow/QoS
```

首次无路线时进入有界 Pending 和 Discovery。Route 安装后发送。稳定高频流达到收益阈值才建立 Flow；普通偶发遥测继续用显式 Contract。

### 28.4 大规模 Cluster 网络

叶子节点可以只启用：

```text
Secure Mesh + Cluster Member 子能力
```

Head/Backup 才按角色启用或分配：

- Cluster Config/Voter；
- Takeover/Recovery；
- Federation/Directory；
- 更高的 Route/Transfer/Diagnostics 容量。

是否成为 Head 是运行时角色，是否具备 Head 能力是编译 Composition。没有 Head 能力的叶子不得被选为 Head。

## 29. 当前 v6 与目标架构的差距

截至本文创建时，当前工程已有良好基础：

- C99；
- 多个 v6 静态库；
- Opaque Storage；
- Feature Bits；
- Layout Hash；
- Realtime/Cluster/Adapter 条件构建；
- Protocol Owner 和固定资源设计。

但仍存在以下**已经按当前 CMake、头文件和静态库依赖核对过的物理差距**。这些行不是未来可能性，也不能被 Feature bit 的逻辑拒绝替代：

| 当前物理状态 | 目标状态 | 冻结前证据 |
| --- | --- | --- |
| CMake 只有 `UCN_FEATURE_REALTIME`、`UCN_FEATURE_CLUSTER`、`UCN_FEATURE_ADAPTER` 三个主要模块开关 | Security、Admission、Capability、Discovery、Flow、Reliable、Fragment、RPC、Operation、QoS 等可独立选择 | configure matrix + 编译命令 + target 列表 |
| Identity 与 Bootstrap 同库；Security、Capability、Route、QoS、Transfer 仍进入常用目标 | Static Identity、Dynamic Admission 和各专用模块按 Composition 独立链接 | `nm`/map 中 Feature-OFF 专用符号为零 |
| 静态库之间仍有较长 `PUBLIC` 链，关闭末端模块不能证明没有传递拉入 | 依赖只沿 contract/SPI 单向进入，消费者只得到所选模块 | installed consumer + link map |
| Adapter 开关同时决定标准 Runtime 是否构建，Runtime config 又携带大多数 Owner 指针 | Runtime Coordinator、Adapter、Port、Reference Board 分离；Runtime 只含所选 Owner | Adapter OFF + Runtime ON 构建/运行；Storage 差值 |
| Realtime local-only 的现有目标仍可能通过 Runtime/Route/Store 依赖链拉入不需要的网络能力 | Local Stamp 只依赖 Kernel Local Time + Timed Adapter；Network Sync 才依赖 Flow/Persistence | local-only archive 的符号、RAM、Wire 状态均为零差异 |
| Identity、Security、Message/Operation、Realtime、Cluster 各有自己的 Store/Provider vtable | 一个 Persistence Foundation、一个 Provider I/O Owner、多个隔离 durable domain | Provider 回调入口唯一；旧 Store denylist 为零 |
| 当前没有独立 Group、Flow、Persistence 基础设施模块目标 | 三者各有 Owner、Storage、Feature、测试和关闭行为 | 独立 target + Feature-OFF consumer |
| Capability 整体常驻 | 编译 Manifest/严格拒绝必选，动态 Peer Capability 可选 | Capability OFF 不留 Peer 表/刷新 FSM |
| QoS 整体常驻 | Basic Queue 必选，高级 QoS 可选 | Q0～Q3/Latest/EDF 状态只在 QoS ON 出现 |
| Transfer 作为整体常驻 | Reliable、Fragment/Credit 按合同拆分 | 单帧 Reliable 不拉入 reassembly/credit 表 |
| 没有明确独立 Flow Owner | 建立 Flow Context/Generation/Fence 模块 | SoftRoute-only 构建无 Flow opcode/state |
| 一个发布目标自动拉入大量库，Storage 宏为多模块始终存在 | Composition 只链接并分配实际模块 | Full/Lite/Nano 的 archive/map/Storage 资源账 |

这些差距是模块化重构任务，不是对当前源码既有安全修复的否定；但它们明确阻断“模块已解耦”“Feature OFF 零物理成本”或“简化架构已实现”的声明。每完成一行都必须先保留原行为回归，再用独立符号、依赖和资源证据销账。

## 30. 推荐目录结构

```text
include/ucn/v6/
    api/                    # 用户统一 API
    contracts/              # 跨模块稳定合同
    modules/                # 启用模块的公共 API

src/v6/
    kernel/
        config/
        owner/
        time/
        buffer/
        wire/
        endpoint/
        resolver/
        direct/
        queue/

    modules/
        identity/
        admission/
        security/
        network/
            route_store/
            forwarder/
            discovery/
        flow/
        transport/
            reliable/
            fragment/
        service/
        operation/
        persistence/
        qos/
        realtime/
        group/
        cluster/
        planner/
        diagnostics/

    runtime/
    composition/
    adapters/
    ports/
    reference/
```

目录结构只表达依赖边界。不是每个目录都必须立即成为独立静态库。

## 31. 分阶段重构顺序

### MOD-00：冻结模块合同

- 审核本文；
- 给每个模块补齐 Owner、依赖、关闭行为和资源账；
- 冻结最小内核；
- 冻结 Composition/Profile/Policy/Intent 四维模型。

不得提前大规模移动源码。

### MOD-01：最小发送和生命周期基线

- 静态直连；
- C1/O0 Copy 小消息；
- 有界 Send Request 与 Transmission Attempt；
- Buffer Obligation；
- submit/failure/cancel/Driver Completion；
- Completion 与 Buffer Release 分离；
- init/start/stop/quiescent/Storage reuse；
- 形成后续每个模块都必须复跑的最小通信回归。

### MOD-02：建立构建 Manifest、依赖图和最小预算

- 增加独立 `UCN_V6_MODULE_*`；
- 生成统一 build config；
- 增加 configure/compile/init 三层检查；
- 保证 Feature Bits 和 Layout Hash 只包含实际模块；
- 增加标准 Composition preset；
- 填写每个 Profile 的最小 Kernel RAM/Flash/Stack/CPU/Wire 数值预算；
- 为每个开关增加 `target graph + nm/map + installed consumer + Storage bytes` 四类物理门禁，逻辑返回 `UNSUPPORTED` 不能替代链接裁剪；
- 先证明 `Adapter OFF + Runtime ON`、`Realtime local-only + Route/Flow/Persistence OFF` 和 `Static Identity + Admission OFF` 三个最小组合。

### MOD-03：拆开 Runtime、Adapter、Port、Reference

- Runtime 不再依赖参考 Adapter 开关；
- UART/CAN/Wi-Fi/USB 独立选择；
- FreeRTOS/Zephyr/裸机 Port 独立；
- ESP32S3 参考实现不进入通用 Adapter；
- Runtime Setup Coordinator 成为唯一有副作用的跨模块准备协调者；
- Runtime config 只持有所选 Owner 的 contract handle，不再包含一个固定“大而全 Owner 指针集合”；
- 先用第 26 篇的 typed requirement/handle/event 连接现有模块，再删除跨 Owner 直接调用，避免目录移动后仍保留逻辑耦合。

### MOD-04：统一 Persistence 基础设施

- 先冻结每个 durable domain 的小型 Record/Operation DTO；禁止把全部模块对象复制成一个巨型 Snapshot；
- 建立单一 Provider I/O Owner，但为 Identity、Security、Operation、Realtime、Group、Cluster 等保留独立 slot/witness/pending/Fault/Fence；
- 统一异步 submit/poll/load、exact volatile continuation 与 durable identity，重启后不得恢复旧 callback/slot；
- 固定 `write inactive → verify → atomic commit marker → advance witness → reload` 顺序；调用方拿到 proof 后还要复核当前资格；
- 逐 domain 迁移：旧 Store 只能通过隔离测试 Composition 的 adapter 暂时接入，新旧实现不得生产双写；
- 每迁移一个 domain，先完成同步/异步、双槽、撕裂写、重入、reload 和旧记录拒绝测试，再删除对应旧 Store vtable；
- 最终用 `rg/nm/CMake` denylist 强制旧 Identity/Security/Message/Realtime/Cluster Store 符号归零；
- 任一 domain 失败只建立局部 Fence，不冻结无关易失通信；
- 在所有旧 Store writer 删除且外审通过前，不允许消费者切到“统一 Persistence 已完成”的发布面。

### MOD-05：拆开 Identity、Security Session 与 Dynamic Admission

- Static Identity/Binding；
- Security Session Handshake、重认证和轮换；
- Dynamic Bootstrap/Join/资格授予；
- Authentication、Peer、Hop、E2E、Group Security；
- 保留内核 Policy Gate；
- 验证 `Admission OFF + Peer/Hop Security ON` 的重启重连；
- Dynamic Admission 只能在 MOD-04 Persistence 可用后进入 `ADDRESS_BOUND/ADMITTED`；
  在此之前只允许隔离的 transcript/cookie/数据模型测试，不得发出持久地址承诺。

### MOD-06：拆开 Network 与 Flow

- Route Store；
- Forwarder；
- Discovery Originator/Responder/Relay 角色；
- Flow Context；
- 每种 Active 状态只有一个 Owner；
- 固定多跳不依赖 Discovery；
- Flow OFF 的 C1 Reliable 真实闭环。

### MOD-07：拆开 Messaging 与调度能力

- Best Effort；
- Reliable；
- Fragment；
- RPC；
- Durable Operation；
- 共享 retry/ACK 基础但不混淆语义；
- Basic Queue 与 Advanced QoS 共享一次物理 ownership；
- 公共资源配额和必要完成/取消预留。

### MOD-08：Optional Domain 装配

- Realtime；
- Group；
- Cluster；
- Diagnostics；
- 验证 C4/C5、Flow、Group 的真实 Wire 依赖；
- 验证相互之间只有声明依赖，没有隐式链接。

### MOD-09：统一 API、Resolver 与可选 Advanced Planner

- 普通用户只提交 Intent；
- Core Resolver 保持只读；
- Runtime Setup Coordinator 执行动作；
- Expert Override 有限且不能绕过 Policy；
- Advanced Planner OFF 仍可使用固定决策表；
- 稳定 Flow 使用经过 O(1) 结构证明的 Fast Path。

### MOD-10：全组合门禁与实机资源测量

- 标准 Composition × Nano/Lite/Full；
- Host、MSVC、GCC、Sanitizer、Analyzer；
- ESP32/STM32 ELF、Map、Stack、CPU、功耗；
- Wire Golden；
- 多 Bearer、多跳和故障注入。

## 32. 测试与验收矩阵

### 32.1 最小构建

必须证明：

- 高级模块全部关闭时可编译；
- 静态直连消息可收发；
- 没有高级模块符号；
- 没有高级模块 Storage；
- 没有高级控制帧；
- 请求高级能力明确失败。

### 32.2 模块关闭回归

| 关闭模块 | 必须仍通过 | 必须拒绝/消失 |
| --- | --- | --- |
| Discovery | 静态直连、静态多跳 | 自动找路、发现控制流量 |
| Security Crypto | 产品允许的受控非认证通信 | 认证/加密 Endpoint |
| Flow | 显式 Contract 数据 | Flow 短格式和 Setup |
| Reliable | 单帧 Best Effort | Reliable Intent |
| Fragment | 单帧消息 | 超单帧消息 |
| RPC | One-Way | Request/Result |
| Operation | 普通 RPC | Durable At-Most-Once |
| Persistence | 易失通信、静态配置、单帧 C1、C1 Reliable | 动态持久状态、Durable Operation、持久 Authority，以及没有等价硬件单调 witness 的 C1 Transfer |
| Advanced QoS | Basic Queue | Q0～Q3/Latest 保证 |
| Realtime | 普通发送、本地超时 | Synced Stamp/Deadline |
| Group | 单播 | Group Intent |
| Cluster | 普通网络 | Cluster Authority 控制 |
| Planner | 固定策略发送 | 高级多候选优化 |
| Diagnostics | 正常协议 | Trace/详细快照 |

### 32.3 五本资源账

每个模块 ON/OFF 都要记录：

| 账目 | 证据 |
| --- | --- |
| Flash | archive/ELF/Map、符号 denylist |
| RAM | Storage Bytes、BSS、固定表项 |
| Stack | `-fstack-usage`、目标任务栈高水位 |
| CPU/后台流量 | step 工作量、控制帧计数、空闲占比 |
| Wire | Golden Bytes、Header/Trailer/Envelope 开销 |

### 32.4 组合测试策略

不穷举所有布尔组合，而采用：

- 标准 Composition 全测；
- 每个模块单独 OFF；
- 每条依赖缺失负向；
- 高风险模块两两组合；
- Nano/Lite/Full 三档；
- Feature Manifest 与链接符号核验；
- Golden Bytes 跨组合一致性；
- 固定 seed 状态机/Fuzz；
- 真实 MCU 资源和故障测试。

### 32.5 必须进入门禁的交界反例

| 组合或故障 | 必须证明 |
| --- | --- |
| Security/Flow/Reliable OFF，C1/O0 | Origin Sequence、长度、Basic Queue 和错误路径仍有唯一 Owner |
| Admission OFF，Peer/Hop Security ON，重启或 Link reopen | 不依赖动态地址分配即可建立新鲜 Session/Replay 状态 |
| Flow OFF，Reliable ON | C1 ACK、重传、认证重复和终态真实闭环 |
| Cluster ON，Flow OFF | 当前设计在零状态/零发送前明确拒绝 |
| Cluster ON，Group OFF | C4 基础控制可用，不调用 C5 |
| 仅 Forwarder，RPC/Fragment terminate OFF | 可转发允许 Contract，不进入 Endpoint，不分配完整重组 Buffer |
| Bulk 占满允许的公共资源 | 基础小消息、ACK、取消和 completion 按预留/背压合同推进 |
| Request 已完成，Driver 仍持有 Buffer | 不提前产生 BUFFER_RELEASED，不复用内存 |
| Persistence 等待期间 Authority 到期 | durable success 不重新获得旧提交/发送资格 |
| Path 已过期但 Generation 未变化 | Fast Path 在使用点拒绝 |
| 两个 Runtime 的 ID/Generation 数字相同 | 句柄因 Runtime/Owner Instance 不同而拒绝串用 |
| Advanced QoS ON/OFF | 不产生重复逻辑排队，不存在绕过 admission 的 Basic Queue 后门 |
| Nano 安全端点与 Full 网关 | 本地 Layout Hash 不同但共同 Wire/Capability 可互通 |

## 33. 发布门禁

模块化重构只有满足以下条件才能宣称完成：

1. 最小 Composition 可单独运行；
2. 所有模块有唯一 Owner；
3. 所有依赖由编译期和 init 两层证明；
4. 模块 OFF 不链接实现；
5. 模块 OFF 不占专用 RAM；
6. 模块 OFF 不运行后台维护；
7. 模块 OFF 不产生专用 Wire；
8. 请求缺失能力明确失败；
9. 不存在静默安全降级；
10. 无关 Feature 不改变普通 Golden Bytes；
11. Composition 与 Profile 完全正交；
12. Runtime 不依赖参考 Adapter；
13. Application API 不暴露内部模块组合负担；
14. Host 不拥有 MCU 网络的隐式控制特权；
15. ESP32/STM32 已验证 Flash、RAM、任务栈、CPU 和功耗。

## 34. 最终决策摘要

### 34.1 必须常驻的语义内核

- Config/Manifest/Layout；
- Protocol Owner/Fence；
- Core Local Time；
- Buffer/Request 生命周期；
- Strict Wire Gate；
- Endpoint；
- Direct Binding；
- Basic Queue；
- Intent Resolver；
- Policy/Capability Gate。

### 34.2 应当可裁剪的能力

- Dynamic Admission/Join；
- Dynamic Capability Exchange；
- Peer Session/Hop/E2E/Group Security 实现；
- Route Discovery；
- Forwarder 和固定多跳；
- Flow；
- Reliable；
- Fragment；
- RPC；
- Durable Operation；
- Persistence Provider/Journal Infrastructure；
- Advanced QoS；
- Realtime；
- Group；
- Cluster；
- Advanced Planner；
- Diagnostics；
- 各 Adapter/Port/Reference Platform。

### 34.3 最重要的四条拆分线

1. 自动寻路与正式 Route/Forwarding 分开；
2. Reliable、Fragment、RPC、Durable Operation 分开；
3. Flow Context 与 Route Store 分开；
4. 统一发送入口/Core Resolver 与 Advanced Planner 分开。

### 34.4 最终用户体验

普通用户只需要说明：

```text
给谁发送什么；
是否可靠；
是否低延迟或实时；
是否需要认证/加密；
单发还是群发；
是否指定路径。
```

协议根据已编译能力、产品 Policy、当前网络状态和资源自动选择最低成本合法方案。高级用户可以显式限制方案，但不能绕过安全和资源门禁。

这才是 UCN 模块化的最终目的：不是让用户面对更多开关，而是让产品只携带需要的能力，让每条消息只支付它真正使用的成本。
