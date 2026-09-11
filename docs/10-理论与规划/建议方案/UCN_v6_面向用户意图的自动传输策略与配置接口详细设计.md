# UCN v6 面向用户意图的自动传输策略与配置接口详细设计

> 状态：SELF-REVIEWED / EXTERNAL REVIEW REQUIRED，架构与 API 候选尚未冻结、尚未实现
>
> 日期：2026-09-06
>
> Wire 依据：[UCN v6 低开销统一 Wire：各 Contract 字段与运行机制详细设计](UCN_v6_低开销统一Wire各Contract字段与运行机制详细设计.md)
>
> 模块边界、Owner、依赖与关闭行为：[UCN v6 可裁剪模块边界、依赖、资源与静态装配详细设计](UCN_v6_可裁剪模块边界依赖资源与静态装配详细设计.md)
>
> 目标：用户描述“我要什么”，协议自动决定“底层怎么实现”；同时为高级用户保留受安全门禁约束的显式配置入口
>
> 兼容性：本设计面向未发布的最终 v6，不保留旧发送 API 或旧 Wire 的兼容义务

本文唯一负责用户语义、Policy 合并、Core Resolver、Runtime Setup Coordinator、Advanced Planner、请求/Attempt/Buffer 生命周期和 Completion。线上字节以 Wire 文档为准，模块依赖和唯一状态 Owner 以模块化文档为准；交叉合同必须同步一致。

实现者应配合[Capability 与 Contract Resolver 简化设计](UCN_v6_逻辑模型与伪代码/23-Capability与Contract-Resolver简化设计.md)理解硬性准入，配合[Advanced Route 与 Flow 简化设计](UCN_v6_逻辑模型与伪代码/24-Advanced-Route与Flow简化设计.md)理解 SoftRoute/Flow 分界。精确公共签名和 Driver/Owner 边界以[统一公共 API 与内部 SPI 候选](UCN_v6_逻辑模型与伪代码/25-统一公共API与内部SPI冻结候选.md)为准。

## 1. 设计出发点

普通 UCN 用户不应该理解下面这些内部概念：

- 当前路径有几跳；
- C1、C2、C3、C4 哪个头更短；
- H0、H1、H2 使用哪个 Hop 认证；
- O0、O1、O2 使用哪个 Origin Security；
- Route Generation、Path Generation、Flow ID 和 Label 是什么；
- Fragment 应该多大；
- Q0、Q1、Q2、Q3 如何调度；
- 当前 Carrier 是 CAN-FD、UART 还是无线。

用户真正关心的是：

1. 数据发给谁；
2. 是单发、群发还是请求对方返回结果；
3. 更重视低延迟、持续吞吐、功耗还是均衡；
4. 是否允许丢失、是否只保留最新值、是否必须可靠送达；
5. 是否需要时间戳或严格 Deadline；
6. 是否只需要认证，还是还要加密；
7. 是否必须经过或避开某条 Path/Bearer；
8. 需要确认到“已排队、已发出、对端收到”还是“业务执行完成”。

因此最终公共 API 应采用：

```text
User Intent
    ↓
Endpoint/Product Policy
    ↓
Core Resolution Pipeline
    ├─ Core Intent Resolver（必选、只读、确定性）
    ├─ Advanced Planner（可选、候选与评分）
    └─ Runtime Setup Coordinator（有界副作用与生命周期）
    ↓
Wire Contract + Security + Route + QoS + Transfer + Realtime
```

Contract 是协议内部执行计划，不是普通用户的必填参数。

## 2. 设计原则

### 2.1 默认自动

用户只给 Target、Endpoint/Service、Payload 就能发送。其他字段缺省时，协议使用产品安全下限和 Endpoint Policy 自动规划。

### 2.2 用户描述目标，不描述实现

用户说：

```text
低延迟
至少 500 kB/s
必须在 2 ms 内到达
只需认证
必须加密
可靠送达
只保留最新值
发送给某个 Group
优先 CAN-FD
必须走指定 Path
```

用户不需要说：

```text
使用 C4 + H1 + O2
分成 128 B Fragment
放入 Q1
使用 Path Generation 7
```

### 2.3 高级覆盖只能收紧，不能绕过

显式配置可以：

- 要求更强安全；
- 禁止自动切路；
- 固定一条已经验证的 Path；
- 禁止分片或聚合；
- 收紧最大延迟；
- 要求更高完成级别；
- 为实验强制某个兼容的 Contract。

显式配置不能：

- 把产品要求的加密降成明文；
- 把 REQUIRED Realtime 降成普通发送；
- 使用已过期 Path/Session；
- 超过 Endpoint 的 Traffic ceiling；
- 绕过 Capability、ACL、MTU 或持久化门禁；
- 因为指定格式不合适就静默换成弱安全格式。

### 2.4 自动不等于不可观察

用户不必选择底层组合，但必须能够查询协议最终选择了什么、为什么选择、开销多大、没有满足什么条件。

### 2.5 MCU-first 和固定资源

- Planner 不使用动态内存；
- Candidate 数量、Flow、Label、Pending 和诊断记录都有编译期上限；
- 一次 `step()` 只处理有界工作；
- 复杂规划可以分多轮推进，不能阻塞 Protocol Owner；
- Linux/Host 不拥有特殊决策权限；
- Nano/Lite/Full 使用同一 Intent 语义，只改变已启用模块的固定容量和 Candidate 上限；某项能力是否存在由 Composition 决定。

## 3. 用户需要理解的最小概念

### 3.1 Target

Target 只有三种：

| Target | 用户含义 | 内部行为 |
| --- | --- | --- |
| NODE | 发给一个节点的某个 Service | C1/C2/C3/C4 自动选择 |
| GROUP | 发给一个已配置 Group | C5 或受控 Group fallback |
| LOCAL_BROADCAST | 本 Link/Realm 的受限发现 | C0，默认禁止业务广播 |

用户不传 Hop Count。Target 在内部解析成当前 Binding、Session、Route 和 Flow。

### 3.2 Service/Endpoint

Service 表示目标节点提供的业务，例如：

- IMU 数据；
- 电机目标值；
- 参数读取；
- 参数写入；
- 文件块；
- 告警；
- 设备诊断。

Service Policy 是自动规划的主要依据。普通用户通常只需在产品初始化时配置一次。

### 3.3 Delivery

| 用户选项 | 含义 | 典型用途 |
| --- | --- | --- |
| AUTO | 继承 Endpoint | 默认 |
| BEST_EFFORT | 尽力发送，允许丢失/重复 | 高频原始遥测 |
| LATEST | 只关心最新值，旧值可被覆盖 | 姿态、温度、位置 |
| RELIABLE | 必须重传并确认 | 命令、配置、文件 |

“Reliable”表示传输可靠，不等于业务一定执行成功。业务执行结果由 Request/Result 与 Completion Level 表达。

`BEST_EFFORT`、`LATEST` 和 `RELIABLE` 是互斥业务语义，不是可以按枚举大小比较的强弱等级。Endpoint 必须给出默认值和允许集合；Per-send 只能从允许集合中选择。若产品需要“只可靠交付最新状态”，应定义独立的 `RELIABLE_LATEST` 合同及接收替换规则，不能由 Planner 临时把 LATEST 与 RELIABLE 拼接。首版不实现该组合，遇到此需求明确返回冲突。

### 3.4 Interaction 与执行语义

Interaction 由 API 选择：

| API/角色 | 含义 |
| --- | --- |
| PUBLISH/ONE_WAY | 只发送消息，不等待业务 Result |
| REQUEST | 发起一次请求，并关联 RESULT/ERROR |
| RESULT/ERROR | 对已有 Request 的最终或阶段响应 |

Request/Result 关联不等于必须写 Flash。Request 还需要独立的执行语义：

| 执行语义 | 状态与保证 | 是否要求持久化 Journal |
| --- | --- | --- |
| REPEATABLE | 允许业务重复执行；只保存易失请求关联 | 否 |
| VOLATILE_DEDUP | 在当前 Runtime/Session 固定窗口内去重 | 否 |
| DURABLE_AT_MOST_ONCE | 跨重启记录执行状态；可能返回 IN_DOUBT | 是 |

只有 Endpoint 明确允许 `DURABLE_AT_MOST_ONCE`、Provider Ready 且固定 Journal 有槽时，Planner 才生成 durable operation Candidate。普通读请求、遥测查询和可重复诊断不能因没有 Flash Provider 而被拒绝。

### 3.5 Performance Goal

速度必须拆成吞吐和延迟，因为二者不是同一个概念：

| 目标 | 含义 |
| --- | --- |
| AUTO/BALANCED | 协议在延迟、吞吐、资源和稳定性之间自动平衡 |
| LOW_LATENCY | 尽量减少排队、聚合和首包等待 |
| HIGH_THROUGHPUT | 允许小幅聚合延迟，优先大 Payload/Fragment 和高 Goodput Path |
| ENERGY_SAVING | 允许更长批处理与休眠窗口 |
| BACKGROUND | 只使用剩余资源，不影响前台流 |

还可提供数值 SLO：

- `max_latency_us`；
- `min_goodput_bytes_per_s`；
- `max_jitter_us`；
- `max_staleness_us`。

数值为 0 表示继承 Endpoint，而不是“必须为零”。

数值 SLO 还必须配合 `quality_requirement`：

| 模式 | 语义 |
| --- | --- |
| AUTO | 由 Endpoint Policy 决定是偏好还是硬约束 |
| PREFER | 作为评分目标；达不到时可发送满足全部硬门禁的最佳 Plan，并在 Receipt 标记降级 |
| REQUIRE | 作为硬门禁；无法满足时不发送 |

因此“想尽量快”和“5 ms 内到达，否则不要发”不会被混成同一种语义。

### 3.6 Realtime

| 选项 | 含义 | 额外线上成本 |
| --- | --- | ---: |
| AUTO | 继承 Endpoint | 由 Policy 决定 |
| NONE | 不需要跨节点时间语义 | 0 B |
| LOCAL_STAMP | 只在本地记录时间 | 0 B |
| SYNCED_STAMP | 目标知道可信采样时间 | 16 B |
| DEADLINE | 目标执行年龄/Deadline 门禁 | 16 B；中继预算可再加 2 B |

Realtime 不等于 Low Latency。Low Latency 是调度偏好；Deadline 是必须验证的时间合同。

### 3.7 Security

| 用户选项 | 含义 |
| --- | --- |
| AUTO | 继承产品和 Endpoint 安全下限 |
| AUTHENTICATED | 验证来源和完整性，Payload 可见 |
| CONFIDENTIAL | 验证来源并加密 Payload |
| TRUSTED_LINK_ALLOWED | 只允许产品已证明位于同一业务完整性信任边界内的精确 Link，或由 Route Owner 汇总的完整冻结 Path；H0 还要求每一跳具备等价原生保护；不是“关闭安全” |

安全默认值不能是“无安全”。零值必须表示 AUTO/继承。

这里的 `AUTHENTICATED` 指**端到端来源认证 O1**，不能用“最后一跳做过 Hop Auth”冒充。
`TRUSTED_LINK_ALLOWED` 也不是应用自行声明链路可信，而是 Route/Planner 根据 Product Threat
Policy 对精确 Link/Path Generation 产生的本地证明；多跳时必须证明全部转发边界都在同一
业务完整性信任域。它可以许可 O0；只有每跳 Carrier/Link 又满足 H0 合同时才可省 Hop Auth。
一旦 Endpoint ACL 依赖原始 Device Principal，或存在不可信中继，就必须使用 O1/O2。

### 3.8 Completion

| 级别 | 用户收到成功的时点 |
| --- | --- |
| LOCAL_ACCEPTED | 已被本机固定队列接受 |
| LINK_SUBMITTED | 已提交给物理 Link |
| REMOTE_REASSEMBLED | 对端完成重组 |
| REMOTE_INBOX_ACCEPTED | 对端业务 Inbox 接受 |
| APPLICATION_RESULT | 对端业务返回 Result |

用户只需选择自己真正需要的完成级别。要求越高，状态、ACK 和重试开销越大。

### 3.9 Path Preference

| 选项 | 行为 |
| --- | --- |
| AUTO | 协议选择并允许安全切路 |
| PREFER | 优先指定 Path/Bearer，不可用时允许等价 fallback |
| PINNED | 必须使用指定且仍有效的 Path；失效就报错 |
| AVOID | 排除指定 Bearer/Path 类别 |

普通用户使用 AUTO。高级用户操作的是 opaque Path Handle，不是裸 Path ID/Generation。

## 4. 四层公共使用接口

### 4.1 Level 1：零配置发送

面向大多数用户：

```c
ucn_send_handle_t send_handle = UCN_HANDLE_INVALID;

ucn_publish(
    node,
    target,
    payload,
    payload_length,
    NULL,
    &send_handle);
```

`options == NULL` 表示完全继承 Product + Endpoint Policy。

另外提供语义更清晰的包装：

```c
ucn_publish(...);        /* One Way */
ucn_publish_group(...);  /* Group */
ucn_request(...);        /* Request/Result */
```

这些包装最终都进入同一个 Core Resolution Pipeline，不各自实现独立路由或安全逻辑。Advanced Planner 可以关闭；关闭后仍由 Core Resolver 和 Runtime Setup Coordinator 使用固定规则完成合法的静态发送或必要准备。

### 4.2 Level 2：Endpoint Policy

产品开发者为每个 Service 配置一次：

```c
ucn_endpoint_policy_install(node, &policy);
```

之后每次发送不再重复指定可靠性、安全、实时或性能目标。

### 4.3 Level 3：Per-send Constraint

少数消息需要临时收紧：

```c
ucn_send_handle_t send_handle = UCN_HANDLE_INVALID;
ucn_send_options_t options = UCN_SEND_OPTIONS_INIT;
options.security = UCN_SECURITY_CONFIDENTIAL;
options.realtime = UCN_REALTIME_DEADLINE;
options.max_latency_us = 2000U;
options.quality_requirement = UCN_QUALITY_REQUIRE;

ucn_publish(..., &options, &send_handle);
```

### 4.4 Level 4：Expert Planner Control

仅用于诊断、认证测试、固定工业拓扑和协议开发：

- 预览候选 Plan；
- 指定 opaque Path Handle；
- 禁止/允许聚合、重排序、分片；
- Prefer/Force 某个兼容 Contract；
- 读取精确开销与选择原因。

即使 Expert 模式也不能越过安全和 Authority 门禁。

## 5. 配置优先级与合并规则

有效需求按以下层级合并：

```text
1. Product/Realm Hard Policy
2. Link Threat Policy
3. Endpoint/Service Policy
4. Group Policy
5. Per-send Options
6. Live Capability/Route/Resource State
```

规则不是“最后一个覆盖前面”，也不是把所有枚举统一解释成“取更严格者”。`resolve_effective_intent()` 必须执行三个独立阶段：

```text
Default Resolution
  → Constraint Intersection
  → Compatibility Validation
```

### 5.1 Default Resolution

- Per-send 为 AUTO/INHERIT 时，读取 Group/Endpoint 默认值；
- Group 没有覆盖时读取 Endpoint；
- Endpoint 没有覆盖时读取 Product/Realm 默认；
- Default 只负责补全未指定字段，不负责降低任何 Hard Constraint。

### 5.2 Hard Constraint Intersection

- 安全最小保证、ACL、Feature、目标类型和执行语义必须全部满足；
- 最大延迟、最大抖动、最大陈旧时间取更小的非零硬上限；
- 最低 Goodput 取更大的硬下限；
- 禁止项取并集，允许项取交集；
- Pinned Path 是路径硬约束，Prefer/Avoid 是偏好或候选过滤；
- Live 状态只能证明某个 Candidate 当前可用，不能降低 Product Hard Policy。

### 5.3 Compatibility Validation

Delivery、Realtime、Completion、Security trust mode 和 Execution Semantics 不能依赖枚举序号合并。每个 Endpoint 必须提供：

- 一个默认值；
- 一个允许值 bitset；
- 必要的组合约束表；
- 不兼容时返回 `UCN_ERR_POLICY_CONFLICT`。

例如 Endpoint 默认 LATEST、允许集合只有 `{BEST_EFFORT, LATEST}` 时，Per-send 请求 RELIABLE 必须冲突；只有 Endpoint 明确允许 RELIABLE 时才能切换语义。`TRUSTED_LINK_ALLOWED` 是部署信任许可，不是低一级的 AUTHENTICATED；`LOCAL_STAMP` 也不能通过“取最大值”自动变成 `SYNCED_STAMP`。

例如：

```text
Product 要求无线至少认证
Endpoint 要求配置写入必须加密
Per-send 请求 AUTHENTICATED
最终仍然是 CONFIDENTIAL
```

这个例子能按安全保证下限求交，是因为 AUTHENTICATED/CONFIDENTIAL 明确定义了包含关系；不能把同一算法推广到互斥 Delivery 或不同时间语义。

## 6. 用户 Intent 数据模型

### 6.1 Target

候选公共结构：

```c
typedef enum ucn_target_kind {
    UCN_TARGET_NODE = 1,
    UCN_TARGET_GROUP = 2,
    UCN_TARGET_LOCAL_BROADCAST = 3
} ucn_target_kind_t;

typedef struct ucn_target {
    ucn_target_kind_t kind;
    uint16_t service_id;
    union {
        ucn_node_ref_t node;
        ucn_group_handle_t group;
        ucn_broadcast_scope_t local_broadcast;
    } destination;
} ucn_target_t;
```

`ucn_node_ref_t` 是产品侧稳定别名，由 Planner 解析为当前 Principal/Binding；`ucn_group_handle_t` 和广播作用域都是绑定 Runtime/Policy Generation 的 opaque handle。公共 API 不接受裸 Group ID、Binding Generation 或网络地址，避免地址复用和句柄 ABA。

### 6.2 Send Options

```c
typedef struct ucn_send_options {
    uint16_t struct_size;
    uint16_t api_version;

    ucn_performance_goal_t performance;
    ucn_delivery_requirement_t delivery;
    ucn_realtime_requirement_t realtime;
    ucn_security_requirement_t security;
    ucn_completion_requirement_t completion;
    ucn_path_requirement_t path_requirement;
    ucn_quality_requirement_t quality_requirement;
    ucn_execution_semantics_t execution_semantics;

    uint64_t max_latency_us;
    uint64_t min_goodput_bytes_per_s;
    uint64_t max_jitter_us;
    uint64_t max_staleness_us;
    uint64_t deadline_domain_time_us;
    ucn_time_domain_handle_t deadline_domain;

    ucn_tristate_t batching;
    ucn_tristate_t reordering;
    ucn_tristate_t fragmentation;

    ucn_path_handle_t preferred_path;
    uint32_t preferred_local_egress_bearer_mask;
    uint32_t forbidden_local_egress_bearer_mask;
    ucn_path_constraint_handle_t end_to_end_path_constraint;
} ucn_send_options_t;
```

字段分组：

| 组 | 字段数 | 作用 |
| --- | ---: | --- |
| API 合同 | 2 | size/version |
| 语义枚举 | 8 | performance/delivery/realtime/security/completion/path/quality/execution |
| 数值 SLO | 5 | latency/goodput/jitter/staleness/deadline |
| 行为三态 | 3 | batching/reordering/fragmentation |
| Opaque Handle/Path | 5 | time domain/path/local bearer/end-to-end constraint |
| 总计 | 23 | 普通用户无需逐项填写 |

虽然结构有 23 个字段，但它是本地 API 参数，不进入每个 Wire Frame。绝大多数调用传 `NULL`。`execution_semantics` 只对 REQUEST 有效；普通 PUBLISH 必须保持 INHERIT/REPEATABLE 语义。

### 6.3 零初始化和版本安全

- `UCN_SEND_OPTIONS_INIT` 设置 `struct_size/api_version`；
- 所有枚举的 0 必须是 AUTO/INHERIT，不得表示 NONE 或明文；
- 所有数值的 0 表示继承，不表示无限或零延迟；
- 三态 0 表示 INHERIT，1 表示 FORBID，2 表示 ALLOW；
- 未知枚举、未知非零保留位、过小 struct_size 无条件拒绝；
- Static Validation 完成前不分配 Sequence、Buffer、Request、Flow 或 Route 状态；进入 `LOCAL_ACCEPTED` 后只允许分配已声明上限的 Request/Copy/ownership 槽，动态 Route/Context 准备仍须在实际发送前再次准入。

时间字段的时钟域必须唯一：

- `max_latency_us` 是从本机接受发送请求开始计算的相对预算；
- `deadline_domain_time_us` 是 Realtime Domain 的绝对时间，只能与 `UCN_REALTIME_DEADLINE` 一起使用；
- `deadline_domain` 是绑定 Domain ID、Generation 和 Runtime Instance 的 opaque handle；请求接受时冻结，重规划不得重新解释；
- Domain 未 LOCKED、Generation 不匹配或 uncertainty 不能证明 Deadline 时，绝对 Deadline 必须失败关闭；
- Runtime 不允许把本机 uptime、墙钟和 Domain Time 低位互相替代。

`max_staleness_us` 必须基于真实 Capture Reference，而不是发送 API 调用时刻。Capture Reference 可来自：

- 硬件采样 Buffer Token 携带的锁存时间；
- `ucn_publish_sample()` 显式传入的 `ucn_capture_ref_t`；
- 已验证的软件采样 API 产生的本地/Domain 时间引用。

Capture Reference 至少绑定 Time Domain、Domain Generation、capture time、来源类型和保守 uncertainty。没有可信 Capture Reference 时，要求采样陈旧度或 SYNCED_STAMP/DEADLINE 的请求失败关闭，Runtime 不能自行用 `now()` 伪造采样时间。

## 7. Endpoint Policy 数据模型

Endpoint Policy 是默认自动行为的核心。候选字段：

```c
typedef struct ucn_endpoint_policy {
    uint16_t struct_size;
    uint16_t api_version;
    uint32_t policy_generation;

    uint16_t endpoint_id;
    ucn_usage_profile_t usage_profile;

    ucn_delivery_requirement_t default_delivery;
    ucn_security_requirement_t minimum_security;
    ucn_realtime_requirement_t default_realtime;
    ucn_completion_requirement_t default_completion;
    ucn_performance_goal_t default_performance;
    ucn_quality_requirement_t default_quality_requirement;
    ucn_execution_semantics_t default_execution_semantics;

    uint32_t allowed_target_kinds;
    uint32_t allowed_bearer_mask;
    uint32_t required_feature_bits;
    uint32_t allowed_delivery_bits;
    uint32_t allowed_realtime_bits;
    uint32_t allowed_completion_bits;
    uint32_t allowed_execution_bits;
    uint32_t allowed_security_bits;

    uint16_t max_payload_bytes;
    uint16_t max_inflight;
    uint32_t max_rate_per_s;
    ucn_traffic_class_t traffic_class_ceiling;

    uint64_t default_max_latency_us;
    uint64_t minimum_goodput_bytes_per_s;
    uint64_t maximum_staleness_us;
    uint64_t max_batch_delay_us;

    bool allow_batching;
    bool allow_reordering;
    bool allow_fragmentation;
    bool allow_path_fallback;
} ucn_endpoint_policy_t;
```

Endpoint Policy 由产品配置，不逐帧携带。安装时必须完整校验 size/version、枚举、容量和字段组合。

不能把整个本地 Policy 原样放入 Flow Context Digest。字段分成两类：

| 类别 | 典型字段 | 更新影响 |
| --- | --- | --- |
| Wire Flow Contract | Delivery、Interaction、Execution、Security、Realtime、ACL/Opcode、Completion capability | 进入双方共同 Contract Digest；变化后旧 Context 失效 |
| Local Planner Policy | 性能权重、路径偏好、批处理上限、功耗权重 | 只绑定本地 Policy Generation；旧 Plan 仍满足硬条件时可平滑重选 |

ACL 撤销、Binding/Key 失效、安全下限提高或 Deadline 到达必须立即 Fence 旧资格；只有性能偏好等软变化才允许 Grace/Drain。

## 8. 内置业务模板

为了让用户连 23 个 Options 字段都不用理解，提供预定义 Usage Template。这里不用 `Profile` 命名，避免与 Nano/Lite/Full 资源 Profile 混淆：

| Template | Delivery | Performance | Security默认 | Realtime | Execution | Completion |
| --- | --- | --- | --- | --- | --- | --- |
| TELEMETRY_LATEST | Latest | Balanced | Product floor | None | Repeatable | Local accepted |
| TELEMETRY_RAW | Best Effort | High Throughput | Product floor | None | Repeatable | Local accepted |
| EVENT_RELIABLE | Reliable | Low Latency | Authenticated | None | Volatile dedup | Remote inbox |
| CONTROL_COMMAND | Reliable | Low Latency | Auth/Encrypted | Deadline optional | Endpoint决定 | Application result |
| BULK_TRANSFER | Reliable | High Throughput | Confidential | None | Volatile dedup | Remote reassembled |
| GROUP_STATUS | Latest | Balanced | Authenticated | None | Repeatable | Local accepted |
| DIAGNOSTIC_READ | Reliable | Balanced | Authenticated | None | Repeatable | Application result |
| CONFIG_WRITE | Reliable | Low Latency | Confidential | Optional deadline | Durable at-most-once | Application result |
| BACKGROUND_LOG | Reliable | Background | Confidential | None | Volatile dedup | Remote reassembled |

Product 可以修改模板，但普通应用只选择 Endpoint，Endpoint 已经绑定模板。

## 9. Core Resolution Pipeline 与可选 Advanced Planner

统一入口不等于完整 Planner 每包必经。Pipeline 分成三个职责：

| 部分 | 是否必选 | 是否允许副作用 | 职责 |
| --- | --- | --- | --- |
| Core Intent Resolver | 是 | 否 | 合并 Policy、检查硬约束、判断 READY/NEED_*/拒绝 |
| Advanced Planner | 否 | 否 | 生成多候选、评分、预测收益和优化建议 |
| Runtime Setup Coordinator | 是 | 是，但有界 | 分配 Request、调用模块、等待 completion、取消和退休资源 |

三者都不拥有 Route、Security、Flow、QoS、Transfer 或 Realtime 的内部 Active 状态。Resolver/Planner 只读各 Owner 的 View；Coordinator 只保存请求级 handle、dependency 和 obligation，通过受控 API 请求对应 Owner 推进状态。

```text
Send Request
  → Core Intent Resolver
  → Target/Binding Resolver
  → READY / NEED_DEPENDENCY(typed DependencyRequirement) /
    WAIT_EXISTING(exact DependencyHandle) / REJECT_*
  → Advanced Candidate/Cost Planner（只有需要时）
  → Runtime Setup Coordinator（只有 NEED_* 时产生副作用）
  → 依赖完成后重新 Resolve
  → Send Admission
  → QoS Queue
```

Core Resolver 不分配事务槽、不发送 Route/Flow/Security 控制帧，也不推进模块 FSM。Advanced Planner 关闭时，Resolver 仍产生确定性的 NEED_*；Runtime Setup Coordinator 仍可按产品固定策略调用已编译模块。

### 9.1 Resolver/Planner 输入

- Effective Endpoint Intent；
- 当前 Target Binding/Session；
- 可用 RouteSet/Path；
- Path MTU、Payload Budget 和 Capability；
- Link Threat/Carrier Contract；
- 当前 Flow/Label/Group Context；
- QoS Queue 压力、Token Bucket 和预计排队；
- Realtime Domain/uncertainty；
- Transfer slot/credit；
- Cluster Authority/Group 状态；
- 当前固定资源余量。

### 9.2 Plan/Decision 输出

```c
typedef struct ucn_send_plan_view {
    uint64_t plan_generation;
    ucn_contract_t contract;
    ucn_origin_security_t origin_security;
    ucn_hop_protection_t hop_protection;
    ucn_route_mode_t route_mode;
    ucn_traffic_class_t traffic_class;

    ucn_path_handle_t selected_path;
    uint16_t fragment_payload_bytes;
    uint16_t exact_egress_frame_protocol_overhead_bytes;
    uint16_t exact_egress_frame_carrier_overhead_bytes;

    uint64_t estimated_message_forward_overhead_bytes;
    uint64_t estimated_setup_control_bytes;
    uint64_t estimated_ack_result_bytes;
    uint64_t estimated_retry_airtime_us;

    uint64_t estimated_latency_us;
    uint64_t estimated_goodput_bytes_per_s;
    uint64_t estimated_jitter_us;
    ucn_quality_evidence_set_t quality_evidence;

    bool requires_route_setup;
    bool requires_flow_setup;
    bool requires_transfer;
    bool uses_realtime_envelope;
    bool uses_hop_budget;

    uint32_t selection_reason_bits;
} ucn_send_plan_view_t;
```

Plan View 是诊断快照，不是可长期复用的 Authority。真正发送前必须重新验证 Path、Session、Capability、Security 和 Deadline。

会产生副作用的 NEED_* 结果不等于 Plan 已可发送。Coordinator 完成准备后必须重新取得当前 View，并复核 Runtime/Owner Instance、Generation、Fence、有效期、Policy 和资源；不能把准备前的 Plan 直接提交。

### 9.3 稳定 Flow 快路径

Advanced Planner 不是每帧必经路径。默认发送先查固定容量的 Flow Execution Binding Cache：

```text
命中有效 Flow Execution Binding
  → 校验 Runtime/Owner Instance、Payload、Policy/Binding/Path/Security Generation、Fence、有效期和资源
  → 扣本次预算并直接入队

未命中、失效、硬条件变化或连续收益达到门限
  → 才进入 Advanced Candidate Planner 或固定候选规则
```

Cache key 至少绑定 Runtime Instance、Endpoint、Target Principal/Group snapshot、Delivery/Interaction/Execution、Security、Realtime 和本地 Policy Generation。快路径仍要通过具体结构证明 O(1) 地执行代际、Fence、有效期、Deadline、ACL 与资源检查，不能仅因使用 Handle 就宣称 O(1)，也不能重新遍历全部 Path、Fragment 档位、功耗模型和 Group 状态。

计划建议、执行绑定与发送状态分开：

- `Advanced Plan View` 是候选/评分的短期只读建议，不拥有生命周期；
- `Flow Execution Binding` 由 Flow/Runtime 基础执行合同拥有，可服务多次发送，不依赖 `planner_private`；
- `Send Request` 是单次消息的 Payload、起点、Deadline、Completion 和资源归还状态；
- 多个 Send Request 可以引用同一 Execution Binding，但不能共享 Completion、Attempt 或 Buffer ownership。

## 10. Candidate 生成

Planner 根据当前状态生成有限 Candidate，不做无限搜索。

### 10.1 Unicast Candidate

最多生成：

1. C1 当前最佳 Route；
2. C2 Direct Flow；
3. C3 当前 Label Path；
4. C4 当前 Label Path；
5. Active-Standby 的备用 Path；
6. Weighted Multipath 中不超过产品上限的 Path；
7. 需要 Setup 的未来 C2/C3/C4 Candidate。

### 10.2 Group Candidate

最多生成：

1. C5 Active Tree；
2. C5 Standby Tree；
3. 产品明确允许时的有界 Unicast fanout；
4. Link-local C0 discovery，仅限发现类 Endpoint。

默认不把一个 Group 静默展开成大量 Unicast，因为这可能突然放大网络负载。

### 10.3 Transfer Candidate

对每条可行 Path，只生成固定数量 Fragment Size：

- Path 最大有效 Fragment；
- 为调度/延迟收紧的一档；
- 产品指定的一档。

不遍历所有可能长度。

## 11. Hard Constraint Filter

Candidate 先过硬门禁，再参与评分。以下任一失败都直接删除 Candidate：

### 11.1 身份和安全

- Target Binding/Session 过期；
- Security 低于 Product/Endpoint floor；
- Candidate 选择 O0 Reliable，但 Endpoint 不是 `PUBLIC_UNAUTHENTICATED`、存在权限副作用，
  或 Route Owner 无法为当前精确 Link/Path Generation 给出 `TRUSTED_LINK_ALLOWED`；
- Origin/Hop Suite 不可用；
- E2E Principal 无法唯一解析；
- Group Sender/Key/Generation 不匹配；
- Replay/Sequence 资源耗尽；
- ACL 不允许该 Service/Opcode。

### 11.2 功能

- Endpoint 要求 Reliable，但 Contract 只支持 Best Effort；
- Endpoint 要求 Request/Result，但没有易失 Request correlation slot；
- Endpoint 要求 VOLATILE_DEDUP，但对端没有易失去重窗口；
- Endpoint 要求 DURABLE_AT_MOST_ONCE，但没有 Ready 的 Operation Journal、持久化 Provider、
  对账能力、预留 Operation reply receipt/persistence continuation，或其声明结果上限超过当前
  `UCN_V6_DURABLE_RESULT_INLINE_BYTES`；
- Realtime REQUIRED，但 Domain 未 LOCKED 或 uncertainty 超限；
- Completion 要求 Remote/Application ACK，但对端不支持；
- Group/Cluster/Transfer Feature 未启用；
- Candidate 需要 C1 Transfer，但当前双方没有已经 committed 的 C1 Transport Parent，或产品没有
  Persistence/等价硬件单调 witness 来保证 Parent Generation 与 Transfer-ID high-water
  persist-before-use、重启不回退和不回绕。Security Session、Link Generation、boot counter 和随机数
  都不能替代该证明；这条门禁不影响普通单帧 C1 与 C1 Reliable。

### 11.3 Path

- PINNED Path Handle 过期；
- Bearer 被禁止；
- Path Capability 不满足 required bits；
- MTU/Payload Budget 不足且禁止分片；
- Path 不允许重排序但 Candidate 会逐包切路；
- Hop Budget 经过保守扣减后不足。

### 11.4 资源

资源门禁必须由 `required_resources(candidate)` 精确导出，不能把所有可选模块的容量统一检查：

- 普通 C1 PUBLISH 只检查实际所需 Queue、Buffer、Route 和 Security 状态；
- 不需要 Transfer 的消息不检查 Transfer slot；
- REPEATABLE Request 不检查 durable Journal；
- 非实时消息不检查 Realtime pending；
- 非 Group 消息不检查 Group/Tree slot；
- Candidate 实际需要的 QoS、Flow、Route、Transfer、Operation 或 Realtime 固定槽不足；
- Buffer 生命周期无法满足 Completion；
- Setup 需要持久化但 Provider 未 Ready；
- C1 Transport Parent/Transfer-ID high-water 的 checked-next 提交、reload 证明或固定资源无法在
  首片前完成；
- Owner 当前 Faulted/I/O active/reentry gated。

硬门禁失败不能用“分数较低”表达，否则 Planner 可能在没有其他 Candidate 时仍选中不安全方案。

## 12. Cost Estimator 与评分

只有通过硬门禁的 Candidate 才评分。

### 12.1 估算指标

- 预计端到端延迟；
- 预计有效吞吐 Goodput；
- 预计抖动；
- UCN Header/Tag/Envelope 开销；
- Carrier padding/fragment 开销；
- Route/Flow Setup 一次性成本；
- Queue residence；
- 丢包率和重传成本；
- 功耗/唤醒次数；
- 当前 Path 稳定时间；
- Fixed slot 占用。

这些指标只能来自本地可审计测量、受认证的邻接/Path 能力以及固定窗口统计。远端自行声明的“低延迟/高带宽”不能直接成为提权或硬 SLO 证明；未知、样本不足和溢出必须产生 conservative/unknown 结果。

### 12.2 性能目标权重

| Performance | 延迟 | 吞吐 | 功耗 | Setup | 稳定性 |
| --- | ---: | ---: | ---: | ---: | ---: |
| BALANCED | 中 | 中 | 中 | 中 | 高 |
| LOW_LATENCY | 最高 | 低 | 低 | 倾向现有可用路径 | 高 |
| HIGH_THROUGHPUT | 中 | 最高 | 中 | 可接受一次 Setup | 中 |
| ENERGY_SAVING | 低 | 中 | 最高 | 倾向聚合 | 高 |
| BACKGROUND | 最低 | 中 | 高 | 不抢占前台 | 中 |

### 12.3 数值 SLO

质量证据分为两类：

| 证据 | 可用于 PREFER | 可用于 REQUIRE |
| --- | --- | --- |
| ESTIMATED | 是 | 否 |
| BOUNDED | 是 | 是，但只在声明的前提和有效代际内 |

`ESTIMATED` 来自历史窗口和当前状态，只能用于排序或 PREFER。`BOUNDED` 必须来自有界队列/调度、资源预留、可验证 Link/Path 上界、Realtime uncertainty 等明确证据；任一前提失效时，Candidate 立即失去 REQUIRE 资格。

`ucn_quality_evidence_set_t` 必须分别记录 latency、goodput、jitter 和 staleness 的 `UNKNOWN/ESTIMATED/BOUNDED`，不能用一个总标志让某项 BOUNDED 掩盖另一项 UNKNOWN。

如果 `quality_requirement=REQUIRE`，用户指定的非零 `max_latency_us`、`min_goodput_bytes_per_s`、`max_jitter_us` 或 `max_staleness_us` 是硬 SLO：

- 没有 Candidate 满足时返回 `UCN_ERR_QUALITY_UNAVAILABLE`；
- 不能选择“最接近”的 Candidate 并谎报成功；
- Plan View 可以报告最佳可达估计，供用户调整；
- 未知证据不算满足；不能拿 4 ms 的统计估计冒充“5 ms 内必达”；
- 只有有界调度、实时 Domain、可验证资源预留和 Path 上界才能声明 BOUNDED。

如果 `quality_requirement=PREFER`，未达到数值目标但仍满足 Security、Delivery、Completion、Realtime 和 Product Hard Policy 的 Candidate 可以发送；Receipt 必须标记 `QUALITY_DEGRADED` 并给出最佳可达估计。`AUTO` 则使用 Endpoint Policy 冻结的模式，不能由 Planner 临时猜测。

### 12.4 指标口径

| 指标 | 规范口径 |
| --- | --- |
| Latency | 从本地 Request accepted 到请求的 Completion target；不同 Completion 不共用一个数值 |
| Goodput | 固定观测窗口内成功达到指定 Completion 的业务 Payload bytes/s，不含 UCN/Carrier 字节；窗口内应用供数不足时为 UNKNOWN |
| Jitter | 同一逻辑 Flow 连续 Completion latency 的保守峰峰上界；分位数只能作为诊断，不能满足 REQUIRE |
| Staleness | 从可信 Capture Reference 到目标 Inbox/Execution gate 的年龄上界，包含双方 uncertainty |

Endpoint 必须冻结 Goodput 窗口长度、Latency Completion target 和 Jitter 窗口；这些字段进入相应质量证据代际。Checked arithmetic 失败、窗口样本不足或代际变化都产生 UNKNOWN。

### 12.5 确定性 Tie-break

分数相同时依次选择：

1. 不需要新 Setup 的 Candidate；
2. 当前已 Active 且稳定时间更长的 Path；
3. 更低线上开销；
4. 更少固定状态；
5. 更小 Path ID 的规范顺序。

不能用随机选择导致不同节点或重复测试不可复现。

## 13. 自动 Contract 选择

Planner 内部使用以下规则：

| 条件 | 自动 Contract |
| --- | --- |
| Bootstrap/Context/Route Setup | C0 |
| 无 Flow 或短流 | C1 |
| Active Direct Flow | C2 |
| Label Path + Best Effort + One Way + O0 | C3 |
| Label Path + Latest/Reliable/Request/E2E/Realtime/Transfer | C4 |
| Group/Tree | C5 |

用户不看到 Hop Count，但 Planner 可以用它估算延迟、Goodput 和开销。

### 13.1 冷启动

第一次发送通常：

```text
C1 pending
  → 自动 Route Discovery
  → 安装可过期 SoftRoute
  → 当 C1 已满足全部硬条件时先发送
  → 观察流量是否持续
  → 必要时用独立 Stage/Commit 建立 C2/C3/C4 FlowPath
```

“后台建立”不能无故阻塞首包：只要 C1 + SoftRoute 已经满足同一 Security、Delivery、Realtime 和 Completion 硬条件，首包立即走 C1；C2/C3/C4 FlowPath 通过完整 Stage/Commit 成功后，后续帧再原子切换。Pinned Path、Network Time Sync 和 Cluster Authority 不允许只消费 SoftRoute。若只有新 Flow Context 才能满足硬条件，则请求保持有界 Pending 或明确失败，不能先用弱语义发送。

### 13.2 短流

只发少量 Frame 时保持 C1，避免 Setup 成本高于节省的 Header。

### 13.3 长流

首版不把字节、时间和固定内存直接相加成一个无量纲分数。只有以下独立条件全部满足才建立新 Context：

```text
预计未来帧数 × 每帧节省字节 > Setup线上字节
并且 Setup等待时间不突破现有Send Request预算
并且 Context固定槽有余量
并且 新Plan连续达到收益门限
```

未来如果引入统一综合成本，必须先冻结归一化单位、权重范围、UNKNOWN 处理和 checked/saturating arithmetic，不能把字节、微秒和槽数量直接相加。

连续满足阈值后才建立 Flow/Label。

### 13.4 不允许逐帧抖动

Contract、Path 或聚合策略的切换必须有：

- 连续样本门限；
- 最短驻留时间；
- 最小收益阈值；
- 新 Plan PREPARE/COMMIT；
- 旧 Plan Grace/Drain；
- Plan Generation 防 ABA。

## 14. 自动 QoS

普通用户不直接选择 Q0～Q3。Planner 根据 Intent 映射：

| Intent | 默认内部队列 |
| --- | --- |
| 有效硬 Deadline、受限关键控制 | Q0 |
| Low Latency、Event、Request/Result | Q1 |
| High Throughput、Bulk Transfer | Q2 |
| Background、Log、维护 | Q3 |

表中的“维护”只指日志上传、非关键诊断和可延期整理。以下协议生命线不能笼统放入“只用剩余资源”的 Q3：

- 租约续期和 Authority Fence；
- Reliable/Transfer 必需 ACK、Credit 和取消确认；
- Key/Session 即将到期的安全控制；
- Realtime Domain 保持所需的有界同步控制；
- Driver completion、Buffer release 和资源退休。

这些控制项必须有独立的有界控制预算或对应 Traffic Class 配额，仍受速率限制，但不能因持续前台流量而永久饥饿。

门禁：

- Endpoint Policy 设置 Traffic ceiling；
- 用户不能通过 `LOW_LATENCY` 给普通数据无限提到 Q0；
- Q0 需要速率、burst 和 admission budget；
- Group/Broadcast 不能无限占用高优先级；
- Hop Budget 只能在本 Traffic Class 内排序，不能跨 Class 提权。

## 15. 自动速度优化

### 15.1 高吞吐模式

Planner 可以自动：

- 建立 C2/C3/C4 Flow；
- 使用 Path 最大安全 Fragment；
- 聚合同一 Flow 的多个小 Payload；
- 选择高 Goodput Path；
- 使用 Weighted Multipath，但只在允许重排序时；
- 降低每字节固定 Header/Tag 次数；
- 把 Bulk 放入 Q2，避免压住控制流。

### 15.2 低延迟模式

Planner 自动：

- 禁止或缩短聚合等待；
- 优先已有 Active Flow，避免临时 Setup；
- 选择低 queue residence/低 RTT Path；
- 使用 Q1；只有严格准入的 Deadline 才进入 Q0；
- 保持 Flow Pin，避免乱序；
- 小消息不强制等待大批次。

### 15.3 小 Payload 聚合

8 B 业务值在安全多跳 Frame 中效率很低。只有以下条件全部满足时才能聚合：

- Endpoint 允许；
- Delivery/Interaction 兼容；
- 同一 Target、Flow、安全和 Deadline 域；
- 没有 Request/Result 独立执行语义；
- 聚合等待不超过 `max_batch_delay_us`；
- Realtime/Latest 边界仍可验证。

Low Latency 或 Deadline Flow 默认不聚合，除非产品明确配置。

## 16. 自动 Realtime

用户只声明：

```text
不需要时间
需要可信采样时间
必须在某时限内到达/执行
```

Planner 负责：

1. 检查 Endpoint Realtime Policy；
2. 校验 Capture Reference，冻结 Time Domain Handle 和 Domain Generation；
3. 解析当前 Time Domain；
4. 检查 Path timestamp capability 和 uncertainty；
5. 选择 C2 或 C4；
6. 加入 16 B Realtime Envelope；
7. 只有中继确实支持 Deadline-aware scheduling 时加入 2 B Hop Budget；
8. 在入队前和业务副作用前执行两级 Deadline 门禁。

时间合同还必须明确终点：

- `REMOTE_INBOX_ACCEPTED` Deadline 截止于远端 Inbox 准入；
- `EXECUTION_STARTED` 截止于业务副作用开始前的最终门禁；
- `EXECUTION_COMPLETED` 只有业务执行器提供可验证的最坏执行时间和完成回报时才可选择；
- 网络 Planner 不能仅凭“消息已经到达”承诺执行完成。

Route Discovery、Context Setup、排队、重传和重规划均消耗原始预算，不能刷新 Capture Time、Request accepted time 或绝对 Deadline。

如果 REQUIRED 条件不满足：

- 不发送普通无时间 Frame；
- 不静默改成 LOCAL_STAMP；
- 返回明确原因：Domain unlocked、uncertainty too high、Path unsupported、deadline impossible。

## 17. 自动 Security

### 17.1 Product Threat Policy

产品为 Bearer 分类：

| Bearer | 示例默认 |
| --- | --- |
| 机内受控点对点 UART | H0 可允许 |
| 受控 CAN/CAN-FD | H0 或 H1，由产品风险决定 |
| ESP-NOW/Wi-Fi/LoRa/公网 | 至少 Hop Auth；敏感业务 O1/O2 |
| USB 本地维护 | 取决于物理访问威胁，配置写入仍建议 O1/O2 |

### 17.2 Planner 选择

- 只要 Endpoint 要求 Confidential，就选 O2；
- 要求 Authenticity 选 O1；
- 稳定一跳 O1/O2 优先 H2 Combined，避免双 Tag；
- 多跳不信任中继时使用 C4+O1/O2；
- Carrier 已提供等价 Hop Auth 时使用 H0，不能由远端 Frame 自称；
- 找不到满足安全下限的 Path 时失败。

“可信有线”是产品部署属性，不是协议根据 Bearer 名称自动推断。CAN、UART、USB 只有在 Product Manifest 明确界定物理边界、威胁模型和维护权限后才可能允许 H0；同一种 Bearer 在外露接口上仍可被要求 H1/O1/O2。

### 17.3 不允许的自动降级

```text
O2 → O1
O1 → O0
H1 → 未认证无线 H0
E2E → 只做 Hop Auth
```

除非用户修改 Product/Endpoint Policy 并重新建立 Context，否则 Planner 不进行这些降级。

## 18. 自动单播、群发和广播

### 18.1 NODE

协议在 C1/C2/C3/C4 中自动选择。应用不关心一跳还是多跳。

### 18.2 GROUP

协议优先：

1. 使用 Active C5 Group Tree；
2. 没有 Tree 时自动启动受控 Group Setup；
3. 用户允许且成员数量不超过静态上限时，可做 bounded unicast fallback；
4. 否则返回 Group unavailable。

不能把 1000 个成员的 Group 自动展开成 1000 条高优先级 Unicast。

Group Handle 必须引用已安装的 `Group Delivery Policy`：

| 字段 | 含义 |
| --- | --- |
| member_snapshot_generation | 本次发送冻结的成员集合与代际 |
| success_scope | LOCAL_ONLY、ANY、ALL、QUORUM 或固定 SUBSET |
| required_count/quorum_rule | 成功人数或规范 quorum 算法 |
| member_leave_rule | 成员离开算失败、忽略还是由新快照重新发起 |
| retry_fallback_rule | 是否允许 Tree 后对未确认成员做有界 Unicast |
| max_result_records | MCU 上最多保留多少成员结果 |
| sender_auth_scope | GROUP_MEMBER 或 EXACT_PRINCIPAL |

Completion 必须结合 `success_scope` 解释：Group 的 `REMOTE_INBOX_ACCEPTED` 或 `APPLICATION_RESULT` 不能沿用单播的“对端”概念。Tree 已交付部分成员后再做 Unicast fallback，必须保持同一 Send/Operation ID，使接收端去重；不能将 fallback 生成成新的业务操作。

`LOCAL_ONLY` 只证明完整 GroupSend 与 fanout/Tree 计划已在本机原子接受。`ANY/ALL/QUORUM/SUBSET`
必须由冻结成员快照中的认证、去重、绑定同一 Send/Operation ID 的逐成员终态 receipt 计算，不能
把出口 submit 数量当作远端成功。Best Effort Group 因而只能声明 `LOCAL_ONLY`；要求非本地范围
时必须选择 Reliable member receipt 和固定容量结果记录，能力不足则在首个发送副作用前拒绝，
不得静默降级。`ucn_publish_group()` 只能产生交付 receipt；业务 `APPLICATION_RESULT` 仍须使用
`ucn_request_group()`。

成员结果使用固定 bitmap/摘要与有界结果表。超过 `max_result_records` 时仍可给出聚合计数和截断标志，但不能动态分配无限成员结果。成员快照在本次 Send Request 生命周期内不变；新成员不会自动扩大成功集合，成员退出按冻结规则处理。

Group Origin Auth 还要区分认证范围：共享 Group Key 只能证明“某个获授权组成员”，不能证明唯一 Device Principal。Endpoint ACL 要求精确来源时，Planner 只有看到 Sender Slot 派生身份、独立签名或等价 `EXACT_PRINCIPAL` 证明才能放行，不能因为 C5/O1 存在就默认满足。

### 18.3 LOCAL_BROADCAST

默认只开放给 Bootstrap、Discovery 和产品明确允许的诊断。必须有：

- Hop Limit 1；
- rate limit；
- fixed pending；
- amplification limit；
- 禁止 Authority 副作用。

普通业务广播默认关闭。

## 19. 自动路径选择

### 19.1 AUTO

Planner 根据：

- Path 可用性；
- Goodput；
- 延迟/抖动；
- MTU/Fragment Budget；
- Security/Realtime Capability；
- Queue pressure；
- 能耗；
- 稳定性；
- Setup 成本。

选择 Active Path，并通过迟滞防止频繁切换。

### 19.2 PREFER

用户可以偏好 CAN-FD、UART、无线或某个 Path Handle。如果偏好不可用，Planner 只在语义和安全完全等价时 fallback。

Bearer mask 默认只约束**本机出口**，因为普通节点通常无法仅凭本地信息证明远端每一跳的物理 Bearer。若业务要求“整条路径不得经过无线/公网”等端到端约束，必须传由 Route/Policy Owner 签发的 `end_to_end_path_constraint` opaque handle；该 handle 绑定可认证 Path 属性、Topology/Route Generation 和有效期。缺少路径证据时必须拒绝，不能把本机出口满足误报为全路径满足。

### 19.3 PINNED

适用于：

- 工业固定链路；
- 实验对比；
- 特定低抖动 Path；
- 法规或物理隔离要求。

Pinned Handle 绑定完整 Route/Path/Session Generation。失效后返回错误，不按相同数字寻找“新 Path”。

### 19.4 Multipath

- Latest/Best Effort 可按 Flow pin 或受控 weighted policy；
- Reliable 默认 per-flow pin，避免乱序扩大重组窗口；
- 只有 `allow_reordering` 且 Transfer/Receiver Window 足够时才逐片多路径；
- Realtime 默认固定最可预测 Path，不以平均吞吐代替最坏延迟。

## 20. 自动分片与 Transfer

用户只传 Payload 和长度。Planner：

1. 先冻结本消息的 DATA Contract、安全和可选扩展；
2. 精确计算完整单帧预算；
3. Payload 能装下时直接 DATA；
4. 只有单帧装不下且允许分片时，才计算 Transfer Fragment 预算；
5. 自动建立 Transfer；
6. High Throughput 使用最大安全 Fragment；
7. Low Latency 可选更小但受上限约束的 Fragment；
8. 按 Credit/Window 调度；
9. 处理 SACK、重传和 Completion；
10. 最终只回调一次完整业务结果。

两个预算不能混用：

```text
data_payload_budget
  = path_frame_mtu
  - selected_data_contract_bytes
  - selected_security_bytes
  - selected_optional_envelopes

fragment_payload_budget
  = path_frame_mtu
  - selected_fragment_contract_bytes
  - selected_security_bytes
  - fragment_envelope_bytes
  - selected_optional_envelopes
```

普通 DATA 没有 Fragment Envelope，因此可能满足 `payload <= data_payload_budget` 但大于 `fragment_payload_budget`；这种消息仍必须单帧发送，不能无谓进入 Transfer。

用户通常不配置 Fragment Size。高级选项只能在 Path Budget 内收紧，不能指定不可编码长度。

开销输出也必须分口径：

- `exact_egress_frame_*` 是当前已冻结 Plan 单帧可逐字节复算的精确值；
- `estimated_message_forward_overhead_bytes` 是所有前向分片的估计合计；
- Setup、ACK、Result 独立计数，不伪装成“固定 Header”；
- Carrier 开销区分当前出口、逐跳总量和 padding；
- 重试、切路和未来空口时间只能是 ESTIMATED/BOUNDED，不能声称编码前精确已知；
- 消息/多跳/Group 总量使用 checked `uint64_t` 与 known/overflow 标志，不能塞入 `uint16_t` 静默截断。

## 21. Flow Plan 与单次发送生命周期

### 21.1 Flow Plan 状态机

```text
UNPLANNED
  → RESOLVING_TARGET
  → DISCOVERING_ROUTE       （需要时）
  → PREPARING_CONTEXT       （长流/Group/Realtime需要时）
  → ACTIVE
  → DEGRADED                （当前Plan仍安全但质量下降）
  → REPLANNING
  → ACTIVE
  → DRAINING/RETIRED

任意阶段：
  → FAILED                  （无合法Candidate）
  → FENCED                  （父代际/安全/Authority失效）
```

关键区别：

- DEGRADED 仍满足 Hard Policy，只是未满足软偏好；
- 一旦 Security、Realtime、Binding、Path 或 Authority 不再满足硬条件，立即 FENCED，不能留在 DEGRADED；
- 新 Plan ACTIVE 前旧 Plan 不被删除；
- 切换失败时，只有旧 Active 仍满足**当前全部硬条件**才允许继续使用；
- Fenced Plan 永不因相同数字重新出现而复活。

Fence 禁止新业务发送和业务副作用，但不能阻止底层完成、取消确认、密钥/Buffer 退休与资源回收；否则安全撤权会反过来制造永久资源泄漏。

### 21.2 三个不能合并的生命周期对象

| 对象 | 作用 | 何时新建 |
| --- | --- | --- |
| Send Request | 一条受跟踪应用逻辑请求、内部执行过程和最终 Completion | 需要 Handle、callback、远端 Completion 或高级生命周期时创建；执行槽退休后由独立固定 Completion Receipt 保留终态 |
| Transmission Attempt | 一次具体 Path/Contract/Frame/ACK/retry 尝试 | 首次发送或每次切路/重建 Contract |
| Buffer Obligation | 谁仍有权读取/写入应用或 Copy Buffer | Buffer 被接管、借用或向下提交时 |

切路只新建 Attempt，不新建另一个业务 Request；同一 Operation 的重传不重新分配 Operation ID。Request 达到某个 Completion 不自动结束 Buffer Obligation。

### 21.3 未跟踪最小发布与 Send Request 状态机

普通用户调用统一 `ucn_publish()` 时，协议可以自动选择不创建 Send Request 的最小本地路径，但
必须同时满足：`ONE_WAY + operation_id=0 + BEST_EFFORT + COPY + 单帧 + LOCAL_ACCEPTED`、无 callback、
`out_handle == NULL`，且不要求 Group、Latest、Reliable、Transfer、Zero-copy、Durable Operation
或任何 Operation/dedup、远端结果。该路径只占一个 TX Slot，允许在其中等待基础 SoftRoute discovery；后续发送失败只
记录诊断。若需要其他 Setup/Completion，或调用者要求 Handle，必须在返回 `UCN_OK` 前原子创建
受跟踪 Request，不能事后升级。

以下状态机只适用于受跟踪 Request：

```text
UNVALIDATED
  → STATIC_VALIDATED
  → LOCAL_ACCEPTED
  → PREPARING             （可选：寻路/Context/证明）
  → SEND_ADMITTED
  → LINK_SUBMITTED
  → WAITING_REMOTE        （按Completion要求）
  → TERMINAL_SUCCESS / TERMINAL_FAILURE / TERMINAL_UNKNOWN
  → RESOURCES_RETIRED
```

各阶段合同：

| 阶段 | 允许的动作 |
| --- | --- |
| STATIC_VALIDATED | 只验证指针、长度、枚举、Policy 兼容性；零 Sequence、零发送副作用 |
| LOCAL_ACCEPTED | 对受跟踪路径原子预留有界 Request Anchor、Request execution 与 Completion Receipt 三个独立槽，始终预留 query/retention obligation，仅在用户配置回调时预留 callback-delivery obligation；Copy Payload 或接管 Zero-copy ownership，冻结开始时刻、Target Principal、Capture/Deadline 和代际 Handle；若它正是用户要求的 Completion Level，同一 commit 还要写不可变 Receipt latch 与 retention 起点，并在配置回调时激活其已预留槽 |
| PREPARING | 有界寻路、Context Setup 与能力证明；不能重置原始超时起点 |
| SEND_ADMITTED | 对选中 Plan 再次执行安全、时间、质量、资源和代际检查 |
| LINK_SUBMITTED | 已产生物理发送副作用；取消后不能声称远端一定未执行 |
| TERMINAL | 对本请求只产生一次最终 Completion 结论 |
| RESOURCES_RETIRED | 所有 Driver/Transfer/Buffer 引用已释放，应用才可复用 Zero-copy Buffer；执行槽可释放，但终态 Receipt 按有界合同继续保留 |

`LOCAL_ACCEPTED` 不是“所有动态条件已满足”，而是 Runtime 已为后续准备承担有界责任。Request pending timeout、Route Discovery、Flow Setup、重传和重规划全部使用原始冻结起点，不得各自重新获得完整预算。

### 21.4 Transmission Attempt 状态机

```text
FREE
  → PLANNED
  → RESOURCES_RESERVED
  → LINK_SUBMITTED
  → IN_FLIGHT
  → ACKED / FAILED / CANCEL_REQUESTED / IN_DOUBT
  → ATTEMPT_RETIRED
```

Attempt 必须绑定 Request ID、Attempt ID、Runtime/Owner Instance、Plan/Path/Session/Flow Generation、Contract、原始 Deadline 起点和 Buffer Obligation。旧 Attempt 的迟到 ACK 不能完成新 Attempt，也不能改变已经切换后的 Active Path。

### 21.5 Completion 与 Buffer Obligation 分离

- Completion 表示用户要求的业务/传输里程碑是否达到；
- Buffer Release 表示协议、Driver、DMA 和重传窗口不再引用应用内存；
- Zero-copy 即使要求 `LOCAL_ACCEPTED` Completion，也必须等独立 `BUFFER_RELEASED` 事件后才能复用 Buffer；
- Completion 只发生一次，Buffer Release 也只发生一次，但二者顺序可以不同；
- Runtime 生命周期内保证 callback 恰好一次；跨掉电恢复只能由已冻结的 durable operation/recovery 合同承诺，不能沿用普通 callback 语句。

用户 Completion 与后台最终 Outcome 是两个字段、两条事件语义：例如请求的
`LOCAL_ACCEPTED` 一旦成功便不可因后续 Link 失败改写；后台诊断仍可最终记录
`DELIVERY_FAILED/OUTCOME_UNKNOWN`，但不得再触发第二个相反 Completion。要求
`APPLICATION_RESULT` 的请求则只有收到并验证对应终态时才发布用户 Completion。查询 API
必须显式返回 `completion_view` 和 `execution_outcome_view`，不能用一个会变含义的 summary
同时代表两者。

所有同步、异步、取消、超时和恢复/对账结果最终都进入同一个 Finalizer。若不可变终态已经证明
达到用户要求的 Completion Level，而 Receipt latch 尚未发布，Finalizer 必须直接发布一次
SUCCESS；若证明不可能达到则发布 FAILURE，无法判定远端结果则发布 UNKNOWN。禁止只记录成功
Outcome 后等待一个永远不会再来的 Driver/ACK 事件，也禁止在失败后保留未发布 latch。

可查询终态必须落在每个 Request 都预先保留的固定 Receipt/query/retention obligation 中；
callback-delivery 槽只在用户配置回调时预留，并且只能在终态 Completion 已发布后调度。Request 执行槽只有在内部
Outcome 终结、全部 obligation 退休且终态已经安全转入 Receipt 后才可复用；旧代际 Handle 只
能通过稳定的 Request Anchor 解析到 exact Receipt，不能因槽复用读到新 Request。Anchor 在
Request 活跃时持有 exact live-request 引用，在用户 Completion 发布后持有 exact Receipt 引用；
这两个引用分别带 slot/generation，公共 Handle 不直接冒充其中任一槽。应用即使提前 ACK
`LOCAL_ACCEPTED`，只要后台 Request/Attempt 仍未退休，Anchor 和 Receipt 也不得释放；只有
内部执行完全退休且应用已 ACK/consume，或非 durable 合同达到 API 冻结的最大查询窗口后，
才可按顺序释放 Receipt 与 Anchor。过期查询返回明确状态。没有 Anchor/Receipt 容量时必须在
分配 Sequence 或产生任何发送副作用前拒绝 Request。

Buffer Obligation 至少记录应用、Copy Pool、密码 Provider、Transport、Adapter/Driver/DMA 的持有关系。Request 失败或取消只发布 release/cancel obligation；只有最后一个持有者完成退休后才产生 `BUFFER_RELEASED`。任何模块不得通过清零 Request 槽间接释放仍在下层使用的内存。

### 21.6 取消、超时和未知结果

| 终止结果 | 含义 |
| --- | --- |
| CANCELLED_BEFORE_SUBMIT | 确定没有物理发送副作用 |
| CANCELLED_AFTER_SUBMIT_IN_DOUBT | 停止本地后续尝试，但远端可能已经收到或执行 |
| REMOTE_REJECTED | 已收到认证的业务拒绝结果 |
| REMOTE_FAILED | 已收到认证的业务失败结果 |
| REMOTE_SUCCEEDED | 已收到认证的业务成功结果 |
| TIMED_OUT_IN_DOUBT | Deadline/等待超时，远端最终状态未知 |

`APPLICATION_RESULT` 表示收到了认证业务结果，不代表该结果一定成功。

### 21.7 重规划不变量

内部必须把稳定的 `Send Request`/`Operation` 身份与可替换的 `Plan Attempt` 分开：

| 对象 | 重规划要求 |
| --- | --- |
| Target Principal | 地址/别名重新解析不得悄悄换成另一设备 |
| Send Request ID | 整个本地生命周期不变 |
| Operation ID | 同一业务操作的重试保持不变；新请求才分配新 ID |
| Transmission Attempt ID | 每次路径/Contract 尝试独立递增并绑定 Execution Binding/Plan Generation |
| Latest logical sequence | C1/C2 切换仍能比较同一逻辑流的新旧关系 |
| Transfer/Result state | 已确认分片或 Result 不因换 Plan 清零 |
| Capture/Deadline start | 永不重置或跨 Domain Generation 重新解释 |
| Completion handle | 始终绑定原始 Request identity；执行槽退休后只解析到其 exact Completion Receipt |
| Security envelope | 按新 Contract 重新封装；不得复制旧 Tag、Nonce 或 Combined Proof |

C2 请求已经可能在远端执行而 Result 丢失时，回退 C1 必须保留原 Operation ID；不能重新调用“创建请求”路径。Latest 切换 Contract 时必须通过冻结的 logical-flow generation/sequence 解释在途旧帧。

## 22. 用户 API 语义

本节说明使用语义；无歧义签名、Handle、Driver Fact API 和内部 SPI 统一收口到第 25 篇，不再把本节视为第二份 ABI 权威。

### 22.1 最简单 Copy 发送

```c
ucn_result_t ucn_send_copy(
    ucn_node_t *node,
    const ucn_target_t *target,
    const void *payload,
    size_t payload_length,
    const ucn_send_options_t *options,
    ucn_send_handle_t *out_handle);
```

规则：

- 非阻塞；
- `options == NULL` 使用默认；
- Payload 在返回前复制进固定小消息池，调用返回后应用即可复用源内存；
- 返回 OK 只表示 Payload 已被本地稳定接受；当 `out_handle != NULL` 时必须返回有效 Handle，当其为 `NULL` 且请求满足 21.3 的严格条件时允许选择未跟踪 TX-Slot 路径；
- 后续结果通过 handle/callback 查询。

统一约定 `UCN_OK` 只表示**本地提交已被验证并由固定资源接受**，不表示已上链、已到达或业务已执行。调用者要求高于 `LOCAL_ACCEPTED` 的最终完成级别只能由受跟踪路径的后续 Completion 事件满足；未跟踪路径没有 Handle、callback、取消或事后 Completion 查询。

### 22.2 语义包装 API

```c
ucn_result_t ucn_publish(...);
ucn_result_t ucn_publish_sample(..., ucn_capture_ref_t capture, ...);
```

二者都使用 Copy 语义。`ucn_publish_sample()` 要求由 Time Owner 签发的 Capture Reference，用于可信采样时间和 Staleness；普通 `ucn_publish()` 不会自行把发送时刻伪装成采样时刻。固定池满返回 NO_SPACE，不使用堆。

### 22.3 Zero-copy API

```c
ucn_result_t ucn_send_zero_copy(
    ...,
    ucn_buffer_token_t token,
    ...);
```

适合大消息和高吞吐。应用保持 Buffer 有效，直到收到 release callback。

基础公共 API 不提供所有权含糊的裸 `ucn_send()`；若为了易用保留该名字，它必须被规范定义为 `ucn_send_copy()` 的别名，绝不能根据 Payload 长度暗中切换成 Zero-copy。

### 22.4 Request API

```c
ucn_result_t ucn_request(
    ucn_node_t *node,
    const ucn_target_t *target,
    const void *request,
    size_t request_length,
    const ucn_send_options_t *options,
    ucn_send_handle_t *out_handle);
```

Planner 自动加入线上 Request/Operation ID、请求关联和 Completion，并按 Endpoint/Per-send Delivery 决定是否可靠重传；Request 本身不偷偷把 BEST_EFFORT 改成 RELIABLE。`publish/request/respond/group` 在需要跟踪时统一使用 `ucn_send_handle_t`，避免用户学习多个同构生命周期 Handle；只有满足 21.3 严格条件且调用者传 `out_handle == NULL` 的 `publish` 可以没有 Handle。Handle 的只读 View 会说明 Interaction Role。只有 `execution_semantics=DURABLE_AT_MOST_ONCE` 时，该 Request 才同时绑定 Persistence Owner 中的 durable Operation identity/Journal，普通 Request 不会因为使用同一 Send Handle 而被强制写 Flash。默认 `ucn_request()` 使用 Copy 语义，大请求另设显式 `ucn_request_zero_copy()`。

### 22.5 Group API

```c
ucn_result_t ucn_publish_group(
    ucn_node_t *node,
    ucn_group_handle_t group,
    uint16_t service_id,
    const void *payload,
    size_t payload_length,
    const ucn_send_options_t *options,
    ucn_send_handle_t *out_handle);
```

`group` Handle 已绑定成员快照来源、Group Delivery Policy 与认证范围。普通用户仍传 `options=NULL`；Per-send Completion 只能选择该 Group Policy 已允许的 success scope，不能临时把 LOCAL_ONLY Group 提升成“全成员业务结果”。需要收集成员结果时返回有界 `ucn_group_result_handle_t`，而不是把无限成员数组写入 Receipt。

`ucn_publish_group()` 是 One Way，不能请求 `APPLICATION_RESULT`。需要成员业务结果时使用独立的 `ucn_request_group()`；在 C5 Group Operation Wire 合同冻结前，该 API 保持 default-OFF，或只在成员数不超过静态上限时使用保持同一 Operation ID 的受控 C4 Unicast fanout。

### 22.6 Plan Preview

```c
ucn_result_t ucn_send_plan_preview(
    ucn_node_t *node,
    const ucn_target_t *target,
    size_t payload_length,
    const ucn_send_options_t *options,
    ucn_send_plan_view_t *view);
```

Preview：

- 不分配 Sequence；
- 不写 Route/Flow；
- 不排队；
- 不调用密码 Provider；
- 不保证下一时刻仍可用；
- 用于 UI、诊断和高级配置检查。

## 23. Send View 与诊断

```c
typedef struct ucn_send_view {
    ucn_send_handle_t send_handle;
    ucn_admission_state_t admission;
    ucn_completion_requirement_t requested_completion;
    ucn_completion_view_t completion;
    ucn_execution_outcome_view_t execution;
    ucn_buffer_state_t buffer;
    uint32_t plan_generation;

    uint16_t exact_egress_frame_protocol_overhead_bytes;
    uint16_t exact_egress_frame_carrier_overhead_bytes;
    uint16_t fragment_count;

    uint64_t estimated_message_forward_overhead_bytes;
    uint64_t estimated_control_overhead_bytes;
    uint64_t estimated_latency_us;
    uint64_t estimated_goodput_bytes_per_s;

    ucn_plan_summary_t plan;
    ucn_quality_evidence_set_t quality_evidence;
    uint32_t reason_bits;
} ucn_send_view_t;
```

普通用户只检查返回值和最终 callback。高级用户可以看到：

- 使用 Stateless/Direct/Label/Group；
- 是否认证/加密；
- 是否分片；
- 估算协议和 Carrier 开销；
- 是否建立了 Flow；
- 是否使用 Pinned/Fallback；
- 质量未满足的原因。

`ucn_send_query()` 通过 Send Handle 返回这份只读快照。`admission`、`completion`、`execution` 和 `buffer` 是四个独立维度；`plan_generation` 表明诊断对应哪次 Attempt/重规划。它不是内部固定 `Completion Receipt`，也不是可执行 Authority。任何一个字段都不能同时表示“业务已完成”和“内存已释放”。错误或 stale Handle 必须保持输出完全不写回。

诊断字符串在 Host 层生成，MCU Core 只保存稳定 reason enum/bitset。

## 24. 普通用户示例

### 24.1 默认传感器发布

```c
ucn_target_t target = ucn_target_node(42U, ENDPOINT_TEMPERATURE);
ucn_send_handle_t send_handle = UCN_HANDLE_INVALID;

ucn_publish(node, &target, &temperature, sizeof temperature,
            NULL, &send_handle);
```

这里的 `42U` 是产品配置中的稳定 Node Reference，不是可在网络中复用的短地址；构造函数会把它包装为 opaque reference 并在发送时解析当前 Principal/Binding。

用户不指定路径、QoS、安全或 Contract。协议可能：

- 首帧使用 C1；
- 发现是一跳稳定流后建立 C2；
- 可信有线为 9 B 开销；
- 无线认证为 25 B 开销。

### 24.2 低延迟但非硬实时

```c
ucn_send_handle_t send_handle = UCN_HANDLE_INVALID;
ucn_send_options_t options = UCN_SEND_OPTIONS_INIT;
options.performance = UCN_PERFORMANCE_LOW_LATENCY;
options.max_latency_us = 5000U;
options.quality_requirement = UCN_QUALITY_PREFER;

ucn_publish(node, &target, data, size, &options, &send_handle);
```

Planner 禁止长批处理并选择低排队 Path；如果没有 5 ms 的 BOUNDED 证据或统计估计未达到目标，只要其他硬合同仍成立就可发送，但 Send View 必须标记 `QUALITY_DEGRADED`。需要“不能证明 5 ms 就不发送”时，应用显式改为 `UCN_QUALITY_REQUIRE`。

### 24.3 加密命令

```c
ucn_send_handle_t send_handle = UCN_HANDLE_INVALID;
ucn_send_options_t options = UCN_SEND_OPTIONS_INIT;
options.security = UCN_SECURITY_CONFIDENTIAL;
options.delivery = UCN_DELIVERY_RELIABLE;
options.completion = UCN_COMPLETION_APPLICATION_RESULT;

ucn_request(node, &motor_target, command, command_size,
            &options, &send_handle);
```

一跳可能自动选择 C2+H2+O2；多跳选择 C4+H1+O2。用户不需要知道实际 Hop 数。

### 24.4 硬 Deadline 命令

```c
ucn_send_options_t options = UCN_SEND_OPTIONS_INIT;
options.realtime = UCN_REALTIME_DEADLINE;
options.max_latency_us = 2000U;
options.security = UCN_SECURITY_AUTHENTICATED;
options.quality_requirement = UCN_QUALITY_REQUIRE;
```

Planner 必须先证明 Time Domain、uncertainty、Path 和调度预算满足条件。`max_latency_us` 是相对发送预算；应用若给绝对执行时刻，应填写 `deadline_domain_time_us`。不能证明就不发送。

### 24.5 Group 发布

```c
ucn_send_handle_t send_handle = UCN_HANDLE_INVALID;

ucn_publish_group(node, status_group, ENDPOINT_GROUP_STATUS,
                  &status, sizeof status,
                  NULL, &send_handle);
```

Planner 使用 C5 Active Tree。没有 Tree 时自动发起 Setup；默认不展开成无限 Unicast。

安全关键执行器 Group 命令必须先在 Group Policy 中冻结成员快照、ALL/QUORUM/SUBSET 成功条件、精确 Sender Principal、Operation 去重和部分失败处理，不能从这个普通状态发布示例推断其已自动具备这些语义。

### 24.6 指定 Path

```c
ucn_send_options_t options = UCN_SEND_OPTIONS_INIT;
options.path_requirement = UCN_PATH_PINNED;
options.preferred_path = selected_path_handle;
```

Path Handle 过期立即失败，不寻找同 ID 新 Path，也不静默走其他 Bearer。

## 25. 典型场景自动选择和开销

| 用户需求 | Planner 可能选择 | 典型开销 |
| --- | --- | ---: |
| 偶发普通数据、可信有线 | C1+H0+O0 | 13 B |
| 稳定一跳可信 Flow | C2+H0+O0 | 9 B |
| 稳定一跳无线认证/加密 | C2+H2+O1/O2 | 25 B |
| 多跳 Best Effort、信任中继 | C3+H1+O0 | 23 B |
| 多跳认证/加密、不信任中继 | C4+H1+O1/O2 | 43 B |
| 一跳安全 Request | C2 安全 + Operation | 34 B |
| 多跳安全 Request | C4 安全 + Operation | 52 B |
| 一跳安全 Realtime | C2 安全 + RT | 41 B |
| 多跳安全 Realtime | C4 安全 + RT | 59～61 B |
| 多跳安全 Transfer | C4 安全 + Fragment | 51 B/片 |
| Group 受信 Carrier | C5+H0+O1/O2 | 29 B |
| Group 不可信 Link | C5+H3+O1/O2 | 45 B |
| Cluster 普通成员业务 | 按普通 C2/C3/C4 | Cluster 额外 0 B |
| Cluster Authority 控制 | C4/C5 + Cluster Envelope | 57～59 B+操作载荷 |

这些组合是 Planner 内部结果，用户只看到 Plan Summary 和开销，不负责手工拼装。

## 26. 高级用户显式控制

### 26.1 可显式控制

- Path/Bearer Prefer、Avoid、Pinned；
- 是否允许分片、聚合、重排序；
- Security 至少达到哪个等级；
- Realtime/Deadline；
- Delivery/Completion；
- 最大延迟和最低 Goodput；
- 质量目标是 PREFER 还是 REQUIRE；
- 测试构建中 Prefer/Force 某 Contract。

### 26.2 不能显式控制

- 不能直接写 Sequence；
- 不能传 Key ID/Generation；
- 不能传 Binding/Session Generation；
- 不能构造裸 Authority；
- 不能复用失效 Flow/Path Handle；
- 不能修改 Hop Count 或伪造 Capability；
- 不能直接设置 Q0 绕过 rate admission；
- 不能声明 Carrier 已可信。

### 26.3 Force Contract

仅测试/专家构建开放：

```text
AUTO
PREFER_C1..C5
FORCE_C1..C5
```

`FORCE` 只在该 Contract 能完整满足 Effective Intent 时成功；否则返回 Contract incompatible，不允许削弱需求。

## 27. Fallback 规则

### 27.1 允许的等价 fallback

- C2 失效后，用 C1 保留相同 Security/Delivery；
- C3 Label 失效后，用 C1 Best Effort；
- C4 Label 失效后，用 C1 E2E；
- Preferred Path 失效后切到等价安全和质量的备用；
- 大消息从 Direct 转 Transfer；
- Carrier Hop Auth 与 UCN H1 在证明等价时互换。

### 27.2 禁止的 fallback

- Encrypted → Auth-only/Plain；
- Authenticated → O0；
- Reliable → Best Effort；
- Deadline → 普通 Low Latency；
- Pinned → 其他 Path；
- Group Tree → 无上限 Unicast；
- Application Result → 只报告 Link Submitted；
- E2E → 只剩 Hop Auth。

### 27.3 Policy 更新处理

| 更新类型 | 旧 Plan 处理 |
| --- | --- |
| 性能权重、优选路径变化，旧 Plan 仍合法 | 可继续短暂使用并平滑重选 |
| 批处理上限收紧但在途批次仍未超限 | 停止新增，允许有界 Drain |
| ACL 撤销、Key/Session 失效、安全下限提高 | 立即 Fence 新发送与业务副作用 |
| Target Binding/Group membership snapshot 失效 | 立即 Fence，不按相同数值重新解析 |
| Deadline 到达、Realtime proof 失效 | 立即停止执行资格 |

任何 Fence 都保留必要的 completion/cancel/retire 路径；它只撤销业务资格，不销毁释放资源所需的证据。

## 28. 错误与用户反馈

建议稳定错误分类：

| 错误 | 用户含义 |
| --- | --- |
| TARGET_UNAVAILABLE | 找不到目标当前 Binding/Session |
| ROUTE_UNAVAILABLE | 没有可用路径且发现失败/仍在进行 |
| SECURITY_UNAVAILABLE | 没有满足安全下限的 Context/Path |
| QUALITY_UNAVAILABLE | 无 Path 满足延迟/Goodput/Jitter SLO |
| REALTIME_UNAVAILABLE | Time Domain/uncertainty/Deadline 不成立 |
| CAPABILITY_UNAVAILABLE | 对端或 Path 不支持所需功能 |
| NO_SPACE | 固定 Queue/Flow/Transfer/Operation 资源不足 |
| PINNED_PATH_STALE | 指定 Path Handle 已失效 |
| POLICY_CONFLICT | Per-send 与 Product/Endpoint Policy 冲突 |
| PAYLOAD_TOO_LARGE | 禁止分片或没有合法 Fragment Budget |
| PERSISTENCE_FAILURE | 需要 durable promise 但持久化失败 |
| RESULT_IN_DOUBT | 已产生发送或执行副作用，但无法证明远端/持久化终态 |

错误必须带 reason enum 和最小诊断快照，不能只返回“发送失败”。

## 29. Buffer、回调与线程模型

- `send_copy` 只复制到固定池；
- `send_zero_copy` 用 token 管理 Buffer 生命周期；
- Application callback 不得重入修改 Runtime 生命周期；
- callback 内再次 send 可以进入独立 bounded command queue，或明确返回 RETRY；
- Plan、Path、Operation Handle 都绑定 Runtime Instance Generation；
- Link reopen、Session rotate、Route invalidation 必须精确撤销依赖 Plan；
- Completion callback 恰好一次；
- 失败后的 Buffer release 也恰好一次；
- callback 不能看到内部可写 Owner 指针。

### 29.1 初始化与业务 Ready 分离

```text
验证 Config/Storage
  → 初始化对象
  → 绑定 Owner/Provider
  → 开放建立 Session/Admission/Route 所需的控制入口
  → 各能力分别达到 Ready
  → 只开放依赖已经满足的业务
```

“对象可以被引用”不等于“所有网络能力 Ready”。尤其不能要求 Peer Session 已 Ready 后才允许接收建立 Peer Session 的认证帧。

### 29.2 停止与 Storage 复用

```text
拒绝新 Request
  → 对依赖资格建立作用域 Fence
  → 继续处理取消、Driver/Provider completion 和 Buffer retirement
  → Request/Attempt/Buffer obligation 全部退休
  → Runtime QUIESCENT
  → 才允许复用 Storage
```

callback、DMA、Driver token、Transfer window 或 Persistence I/O 活动时，`reset/reopen/destroy` 必须拒绝或进入有界 drain，不能直接清零对象。

## 30. 固定资源与 Profile

Profile 只决定容量，不决定模块是否编译；Composition 才决定 Feature。正交不表示任意小容量都能支持任意 Feature，低于模块最低容量时必须拒绝构建或初始化。

### 30.1 Nano

- 使用最小 Request/Attempt/Buffer、Endpoint、Route/Flow 和 Candidate 容量；
- 可以是安全端点、Realtime 端点或 Cluster Member，只要 Composition 启用且容量满足最低合同；
- 未启用能力明确拒绝；
- API 和 Intent 语义不要求应用改写。

### 30.2 Lite

- 为已启用模块提供中等固定容量；
- 可以启用小型 Route/Flow/Transfer/Realtime/Group 或简化 Multipath；
- 没有被 Composition 选择的模块不因 Lite 自动链接。

### 30.3 Full

- 为已启用模块提供更大的表、窗口和 Candidate 上限；
- 可以启用完整 Weighted Multipath、Group、Realtime、Transfer 或 Cluster，也可以只做高吞吐静态直连；
- 与 Nano/Lite 使用相同 Intent 和错误语义；
- 不因为资源更大获得越过安全门禁的特权。

### 30.4 编译期预算

需要新增：

- Planner Request slots；
- Plan Candidate max；
- Endpoint Policy slots；
- Group Policy slots；
- Plan diagnostic ring；
- Copy Buffer bytes；
- Zero-copy inflight tokens；
- Setup concurrency。

所有容量为 0 时，相应 Feature 必须从 Storage/Layout Manifest 消失或被显式拒绝。

Layout Hash 只验证同一本机 Runtime、应用、库和 Storage 的 ABI/配置一致性。不同节点的 Profile、Feature 和 Layout Hash 可以不同；网络互通依据共同 Wire Contract、角色 Capability、MTU、安全和 Endpoint Policy，不比较本机 Layout Hash 是否相等。

### 30.5 共享资源准入

高级模块专用槽耗尽时，无关消息不得检查该专用槽；但 Copy Buffer、Adapter TX、Request、
callback/cancel/retire obligation 和 Owner work budget 是共享资源，必须采用固定配额和预留。
Driver completion 的事实保存在每 token 的内嵌 latch；共享 wakeup 队列只作可丢失提示，不能
被描述成用户 Completion Receipt 或完成真值：

- 基础小消息保留最小 Copy/Request 容量；
- ACK、取消、完成和资源退休保留推进能力；
- Bulk/Transfer 有最大公共资源占用；
- 借用保留量必须有确定归还和抢占规则；
- 高级 QoS 与 Basic Scheduler 共享一次逻辑入队和一个 Buffer Obligation，不重复排队；
- QoS admission 失败不能回退 Basic Queue 绕过门禁。

## 31. 自动策略必须避免的错误

### 31.1 “自动”掩盖失败

不能为了让 API 返回成功而降低安全、可靠或 Deadline。

### 31.2 根据瞬时指标频繁切路

必须使用连续样本、迟滞、最短驻留和 Plan Generation。

### 31.3 小包盲目聚合

吞吐优化不能突破最大延迟、Latest 或 Request 边界。

### 31.4 自动 Group fanout 放大

必须有成员数和总字节上限。

### 31.5 把预测当保证

没有资源预留和时间证明时，只能说 Estimated，不得说 Guaranteed。

### 31.6 让高级选项传裸内部字段

用户只能传 Intent、Policy 或 opaque Handle，不能传代际和安全 Selector。

## 32. 实施顺序

本设计确认后按以下阶段实施，不能先接生产发送、以后再补 Buffer 和 Completion：

| 阶段 | 内容 | 放行条件 |
| --- | --- | --- |
| UAPI-00 语义冻结 | Default/Hard/Preference、Delivery、Execution、Completion、Group success、时间与指标口径 | 文档/Ghost API/对抗用例外审 |
| UAPI-01 纯策略模型 | default-OFF target；Intent Resolver、精确资源需求、Hard Filter、确定性选择、无副作用 Preview | 不引用生产 Runtime；Full/Lite/Nano 固定容量 |
| UAPI-02 最小发送闭环 | C1、Copy、Request slot、有界 Pending、原始超时起点、取消、Completion、Buffer Release | 冷启动、失败和回收全闭环后才能首次生产接线 |
| UAPI-02P Persistence completion 基座 | 精确绑定 Owner/事务/Record 的异步 completion、reload、失败、取消、局部 Fence和调用方资格复查 | 存储成功不得自动恢复已经到期的 Authority/Session/Policy；无关通信不中断 |
| UAPI-03 稳定 Flow 快路径 | C2/C4 Cache 命中、Plan Attempt、失效重规划、Operation/Latest 不变量 | 热路径不运行完整 Planner；切换不改变逻辑请求 |
| UAPI-04 Transfer | DATA/Fragment 双预算、分片、SACK/Credit、跨 Plan 生命周期 | 单帧不误分片；Zero-copy 释放与 Completion 分离 |
| UAPI-05 可选高级能力 | Realtime、Group、Multipath、能耗优化逐项接入 | 每项 default-OFF、独立资源预算和单独外审 |
| UAPI-06 发布面收口 | Advanced Path/Contract、删除公共裸 Frame/Route 入口、内部 primitive 私有化、用户手册 | Full/Lite/Nano、实机性能与 API 兼容门禁 |

首版不要求同时完成未来流量预测、Weighted Multipath、复杂能耗模型和 Group fallback。默认 C1 好用、稳定 Flow 足够轻、错误与资源归还清楚，优先级高于“自动 Planner 功能数量”。

在低开销 Wire Contract 冻结前，Planner 只能实现纯策略模型和测试，不能接当前臃肿 Contract 1 形成新的生产依赖。

## 33. 测试与审计要求

### 33.1 默认行为

- `options=NULL` 可完成普通发送；
- Endpoint Profile 决定默认语义；
- 无 Route 自动发现；
- 短流 C1、长流自动提升；
- 用户从不提供 Hop Count 或 Contract；
- `quality_requirement=PREFER` 未达目标时可带降级诊断发送，`REQUIRE` 必须拒绝；

### 33.2 不降级

- 无加密 Path 时 Confidential 必须失败；
- Deadline 无证明时必须失败；
- Pinned 失效时不 fallback；
- Reliable 不退 Best Effort；
- Group 不做无界 fanout。

### 33.3 自动选择

- 一跳稳定 Flow 选择 C2；
- 已有多跳 Label Path 的 Best Effort 选择 C3；
- 已有多跳 Label Path 且要求 E2E/可靠/实时语义时选择 C4；没有 Label Context 的偶发业务仍用 C1；
- Group 选择 C5；
- 短流不会因 Setup 反而更慢；
- 新方案收益不连续时不切换。

### 33.4 开销

- 已冻结 Plan 的 `exact_egress_frame_*` 与实际单帧编码逐字节一致；
- 消息、Setup、ACK、Result、Carrier padding/fragment 和重试分口径记录；
- 未知动态成本带 UNKNOWN/ESTIMATED，不伪装成精确常量；
- Cluster/Realtime/Transfer OFF 时普通 Golden 不变；
- 每个场景有 Payload 8/16/32/64/128/256 B 回归。

### 33.5 对抗

- 用户低安全覆盖不能突破 floor；
- 伪造高优先级不能进 Q0；
- stale Path Handle 不发生 ABA；
- Callback/reentry 不破坏 Planner；
- Plan Preview 无状态写入；
- 指标抖动不产生 Route/Contract 抖动；
- 表满不驱逐安全或 ACTIVE 状态。

### 33.6 策略与按需资源

- Endpoint 默认 LATEST、允许集合不含 RELIABLE 时，Per-send RELIABLE 返回 POLICY_CONFLICT；
- 普通 Request 在没有持久化 Provider 时可使用 REPEATABLE/VOLATILE_DEDUP；
- DURABLE_AT_MOST_ONCE 没有 Ready Journal 时零写入拒绝；
- Transfer、Realtime、Durable Operation 槽满不阻断完全不依赖它们的普通 C1 PUBLISH；
- ACL 撤销、安全下限提高和 Binding 失效立即 Fence，软性能权重变化才允许 Drain。

### 33.7 质量与时间

- 延迟统计估计 4 ms、但没有 5 ms BOUNDED 证据时，PREFER 可降级发送，REQUIRE 必须拒绝；
- Goodput/Jitter 窗口样本不足、应用未持续供数或算术溢出产生 UNKNOWN；
- 数据采样后等待 50 ms 再发送且要求 5 ms Staleness 时，不得用 API 调用时刻替换 Capture Time；
- Route Discovery、Setup、重传和重规划不刷新 Request 起点或绝对 Deadline；
- Domain Generation 改变使旧 Capture/Deadline Handle 失效。

### 33.8 生命周期与重规划

- Zero-copy 请求在 LOCAL_ACCEPTED Completion 后仍禁止复用 Buffer，直到独立 BUFFER_RELEASED；
- Driver 完成失败、取消重试和 Fence 后最终仍恰好一次释放 Buffer；
- 已 Link Submitted 的 Request 取消返回 IN_DOUBT，不声称远端未执行；
- 远端已执行但 Result 丢失，从 C2 回退 C1 时保持同一 Operation ID；
- Latest 在 C1/C2 切换时保持可比较的 logical sequence；
- 新 Plan 失败时，旧 Plan 只有仍满足当前 Hard Policy 才能继续。

### 33.9 Group 与预算

- Group 部分成员已收后做 Unicast fallback，重复接收不重复执行；
- ANY/ALL/QUORUM/SUBSET 对同一成员快照产生确定结果，成员变化不偷偷改变目标集合；
- Group 共享 Key 不满足 EXACT_PRINCIPAL ACL；
- Payload 大于 Fragment Budget 但小于 DATA Budget 时仍走单帧；
- 整条消息和多跳开销超过 16 bit 时不截断，返回 checked 64-bit 值或 overflow/unknown。

### 33.10 快路径和进度保证

- 高速稳定 Flow 的绝大多数帧命中 O(1) 快路径，不枚举所有 Candidate；
- Policy/Path/Security Generation 变化后首个快路径检查立即失效；
- 持续前台流量下，租约、ACK/Credit、密钥控制、Driver completion 和 Buffer retirement 仍在有界时间内推进；
- Candidate/Request/Flow 表满时返回确定错误，不扫描、驱逐或动态增长。

## 34. 最终推荐

最终公共体验应该是：

```c
ucn_send_handle_t send_handle = UCN_HANDLE_INVALID;
ucn_send_options_t options = UCN_SEND_OPTIONS_INIT;

/* 普通用户：只说发给谁、发什么。 */
ucn_publish(node, &target, data, length, NULL, NULL);

/* 有要求的用户：只描述业务目标。 */
options.performance = UCN_PERFORMANCE_LOW_LATENCY;
options.security = UCN_SECURITY_CONFIDENTIAL;
options.realtime = UCN_REALTIME_DEADLINE;
options.max_latency_us = 2000U;
options.quality_requirement = UCN_QUALITY_REQUIRE;
ucn_request(node, &target, data, length, &options, &send_handle);
```

协议内部自动完成：

```text
目标身份解析
→ 安全下限合并
→ Route/Path/Capability 解析
→ C1/C2/C3/C4/C5 选择
→ H0/H1/H2/H3 与 O0/O1/O2 选择
→ QoS 和 Deadline admission
→ Flow/Label/Group Setup
→ MTU/Transfer/Fragment
→ 发送、重试、Completion
→ 诊断和自适应重规划
```

普通用户不用理解一跳还是三跳，也不用知道 Wire Contract。高级用户仍可指定 Path、Bearer、SLO 和更严格安全，但不能破坏协议硬门禁。

这套“Intent-first、Planner-driven、Policy-fenced”的方式，才能同时实现低开销、易使用和可审计，而不是把底层复杂度转嫁给每个应用开发者。
