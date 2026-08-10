# UCN T25 节点内任务通信详细执行方案

> 状态：**T25.0～T25.3 已完成（T25.3 为 ESP32 构建验证）；T25.4～T25.5 待实施**。当前已具备纯 C Router、Protocol Task 桥接和 ESP32 静态 FreeRTOS Port；尚未得到真实板级任务通信、时延或资源测量结论。
> 适用版本：UCN v4，MCU-first，C99 Core + 产品/RTOS Port。
> 关联：[节点内任务通信建议](UCN_节点内任务通信建议.md) · [T25.0 首版 Endpoint 与 Service 契约](UCN_T25_首版Endpoint与Service契约.md) · [整体架构](UCN_整体架构设计.md) · [v4 协议核心说明](UCN_v4_协议核心说明.md) · [任务表](00-任务表.md)。

## 1. 要解决的问题

一个 MCU 上通常同时运行 IMU、控制、舵机、电源、日志等任务。它们既需要彼此通信，也可能需要把同一类业务数据发送到另一个 MCU。若让每个任务各自直接调用 `ucn_node_t`，会产生并发访问、路由表竞争、数据所有权不明和不必要的帧封装。

T25 的目标是统一业务的**寻址语义和调用方式**，同时保留两条不同的执行路径：

```text
业务任务
   │  ucn_service_send(目标 Node, Endpoint, QoS, Payload)
   ▼
Service Router（产品层，固定表/固定队列）
   ├─ 目标是本 MCU Node ──────► Local Fast Path ─────► 目标任务 Inbox
   └─ 目标是远端 Node ────────► Remote TX Request ──► 唯一协议任务
                                                       │
                                                       ▼
                                           ucn_node_send_endpoint()
                                           Link / 路由 / 安全 / 转发
```

本机任务之间不会构造 UCN 帧，也不会调用 Link、寻路、CRC 或无线加密；跨 MCU 时仍完整复用现有 v4 Core。这样“业务代码写法一致”，但不会为了形式统一而损失本机实时性。

## 2. 首版范围与明确不做的事

### 2.1 T25 首版必须实现

- 一个 MCU 只有一个 UCN Node ID；业务 Task/Service 不是 Node。
- 固定 Service 表：静态 Endpoint 映射到本机任务 Inbox。
- 统一发送 API：根据目标 Node ID 自动走本机直投或远端协议发送队列。
- Q0/Q1 独立、固定容量的 Inbox 语义。
- 远端入站 Endpoint 回调只做快速转交，不在协议任务上下文运行业务逻辑。
- 纯 C Router 单元测试和模拟测试；随后才接 FreeRTOS/ESP32 Port。
- 统计可见：本机成功、远端排队、未知 Endpoint、未就绪、长度/QoS 不匹配、队列满和 Q1 覆盖。

### 2.2 T25 首版明确不做

- 不修改 v4 的 `ucn_frame_t`、帧头、CRC、Path ID、AODV-Lite、Link 或安全 Provider。
- 不给每个任务分配 Node ID、邻居、Heartbeat、路由表或入网流程。
- 不引入动态内存、动态任务发现、动态 Endpoint 注册或无上限队列。
- 不把本机任务通信伪装成无线帧后再回环接收。
- 不把 PWM、FOC、姿态闭环等硬实时控制放到网络任务中。
- 不承诺首版即提供零拷贝、一个发布者向任意数量订阅者自动扇出、动态订阅目录或可靠大数据传输；这些留给后续阶段。

## 3. 基本术语和责任边界

| 名称 | 代表什么 | 是否入网 | T25 中的责任 |
| --- | --- | --- | --- |
| **Node** | 一块能独立接入 UCN 的 MCU/设备。 | 是 | 拥有 Node ID、邻居、路由、Link、安全和唯一协议任务。 |
| **Protocol Task** | 唯一拥有 `ucn_node_t` 的任务。 | 不适用 | 调用 `ucn_node_step()`、`ucn_node_receive()` 和最终 `ucn_node_send_endpoint()`。 |
| **Service / Task** | Node 内的业务执行单元，例如 IMU 或舵机任务。 | 否 | 读取自己的 Inbox；通过 Router 提交发送请求。 |
| **Endpoint** | 静态业务 ABI，范围使用现有 `0x40..0xBF`。 | 作为业务帧 `message_type` 传输 | 标识“本 Node 内要由哪个业务接口处理”，不是 Task ID。 |
| **Inbox** | 某 Endpoint 对应的有界本地接收槽/队列。 | 否 | 将数据从 Router 转交给目标业务任务。 |
| **Service Router** | Core 外的产品层组件。 | 否 | 解析 Endpoint、选本机/远端路径、维护 Service 状态和统计。 |

因此，业务的标准地址始终是：

```text
Destination Node ID + Endpoint + Traffic Class + Payload ABI
```

同一 MCU 内只需在 Router 中判断 `Destination Node ID == local_node_id`，即可将这一地址翻译为本机 Inbox。远端仍由 Core 按原规则路由到目标 Node，再由目标 Node 的 Router 投递到其对应 Endpoint。

## 4. 分层结构和任务上下文

```text
┌──────────────────── 产品业务层 ────────────────────┐
│ IMU Task     Control Task       Servo Task           │
│ 读/写 Inbox  读/写 Inbox        读/写 Inbox          │
└───────────────┬───────────────────┬─────────────────┘
                │ ucn_service_send() │
┌───────────────▼───────────────────▼─────────────────┐
│ Service Router（Core 外、固定 Service 表）            │
│ - Local Fast Path：Endpoint -> Inbox                 │
│ - Remote TX Request Q0/Q1：业务任务 -> Protocol Task │
│ - 入站 Adapter：Endpoint callback -> Inbox            │
└───────────────┬─────────────────────────────────────┘
                │ 仅 Protocol Task 调用 Core
┌───────────────▼─────────────────────────────────────┐
│ UCN Core：Node / QoS / 路由 / Path / 安全 / Link       │
└───────────────┬─────────────────────────────────────┘
                │
          WiFi / UART / CAN / 其他 Bearer
```

必须遵守下列并发规则：

1. **Protocol Task 独占 `ucn_node_t`。**业务任务不得直接调用 `ucn_node_receive()`、`ucn_node_step()` 或 `ucn_node_send_endpoint()`。
2. **Endpoint 回调处于 Protocol Task 上下文。**回调只能校验、复制/引用受控数据并投递 Inbox；不能做阻塞 I/O、控制计算、等待队列或再次驱动 Core。
3. **业务任务不直接访问路由/Link。**它们只能提交一个固定大小的 Remote TX Request。
4. **本机投递不能等网络。**本机 Q0/Q1 的成功与失败仅取决于 Service 表、Inbox 状态和容量，不等待 Link、邻居或路由。

## 5. T25.1 已实现的纯 C API 合约

`include/ucn/ucn_service.h` 与 `src/ucn_service.c` 已实现平台无关的 `ucn_service` 模块。它不放入 `ucn_node.h`，不包含 FreeRTOS 头文件，也不操作 `ucn_node_t` 或任一 Link。实际 API 如下：

```c
ucn_result_t ucn_service_router_init(ucn_service_router_t *router,
                                     const ucn_service_router_config_t *config);
ucn_result_t ucn_service_set_ready(ucn_service_router_t *router,
                                   ucn_endpoint_t endpoint, bool ready);
ucn_result_t ucn_service_send(ucn_service_router_t *router,
                              ucn_node_id_t destination,
                              ucn_service_id_t source_service_id,
                              ucn_endpoint_t endpoint,
                              ucn_traffic_class_t traffic_class,
                              const uint8_t *payload,
                              uint16_t payload_length);
ucn_result_t ucn_service_deliver_remote(ucn_service_router_t *router,
                                        const ucn_frame_t *frame);
ucn_result_t ucn_service_inbox_take(ucn_service_router_t *router,
                                    ucn_service_id_t owner_service_id,
                                    ucn_endpoint_t endpoint,
                                    ucn_service_message_t *message);
ucn_result_t ucn_service_remote_tx_take(ucn_service_router_t *router,
                                        ucn_service_message_t *message);
```

`ucn_service_send()` 的目标为本 Node 时只复制到绑定 Inbox；目标为远端时只复制到固定 Remote TX 队列。T25.2 已由独立的 `ucn_service_bridge` 在唯一 Protocol Task 边界上有界调用 `ucn_service_remote_tx_take()`，再调用既有 `ucn_node_send_endpoint()`；Router 本身仍不直接完成该桥接。

### 5.1 T25.2 Protocol Task Bridge

`include/ucn/ucn_service_bridge.h` 与 `src/ucn_service_bridge.c` 是唯一允许同时引用 Router 与 Node 的薄适配层；不包含 FreeRTOS，也不改变 `ucn_node_t`。Port 先初始化并安装 Endpoint handler，随后只在拥有 Node 的 Protocol Task 中调用：

```c
ucn_service_protocol_bridge_init(&g_bridge, &g_service_router, &g_node);
ucn_service_protocol_bridge_install_endpoint_handlers(&g_bridge);

/* Protocol Task 主循环：每轮上限由产品 Profile 冻结。 */
ucn_service_protocol_bridge_step(&g_bridge, APP_SERVICE_TX_BUDGET, NULL);
ucn_node_step(&g_node, now_ms);
```

Bridge 会拒绝覆盖其他组件已占用的 Endpoint handler；每次最多取出 `max_requests` 个请求，且继承 Router 的 Q0 优先。被 Core 拒绝的已出队请求不在 Bridge 隐式重试：Q0 的无路由/Link Down 是显式失败，Q1 的受限寻路与 Pending 仍完全由 Core 决定。Endpoint handler 的原有类型不返回错误，因此目标 Router 的未就绪/队列满会计入 Bridge/Router 本机统计，而不会伪造成对源端的业务 ACK；可靠确认留给后续独立能力。

T25.3 为 Bridge 增加了可选、仍保持纯 C 的 `set_inbound_hooks()`：产品 Port 可在 Router 的短复制/出队临界区前后进入/退出自己的锁，并在投递完成、已退出锁后收到观察回调。该接口不包含 FreeRTOS 类型、不创建对象、也不影响帧格式；没有安装 hook 的 C99 产品仍保持 T25.2 原行为。

产品可按 R1 ABI 提供薄封装，例如 IMU 任务调用：

```c
return ucn_service_send(&g_service_router, destination, APP_SERVICE_SENSOR,
                        0x40U /* IMU0_RAW_V1 */, UCN_TRAFFIC_Q1_REALTIME,
                        encoded_sample, 24U);
```

`source_service_id` 只用于本机审计、权限和统计；它不替代 Endpoint，也不写入现有 v4 正常业务帧。因此它不会增加远端帧长度。

### 5.2 Service 绑定表

启动时由产品配置一次固定表，不能在业务运行中任意增删：

```c
typedef enum {
    UCN_SERVICE_DELIVERY_Q0_FIFO,
    UCN_SERVICE_DELIVERY_Q1_LATEST,
} ucn_service_delivery_mode_t;

typedef struct {
    ucn_endpoint_t endpoint;
    ucn_service_id_t owner_service_id;
    uint16_t max_payload_length;
    uint8_t allowed_traffic_mask;       /* Q0/Q1；首版拒绝 Q2/Q3 */
    ucn_service_delivery_mode_t delivery_mode;
    uint32_t allowed_local_source_mask; /* 本机 Fast Path / 远端发送 ACL */
    bool accept_remote;                 /* 是否接收已由 Core 验证的远端业务帧 */
    bool enabled_at_boot;
} ucn_service_binding_t;
```

首版规则：

- 一个静态 Endpoint 只能有一个绑定，且在 R1 只能属于 Q0 或 Q1 其中一种投递语义；重复 Endpoint 是启动配置错误。
- 一个 Service 可以拥有多个 Endpoint。
- 绑定表项应当静态 const；Router 只保存 `ready`、统计和 Inbox 状态等固定运行时状态。
- `endpoint` 必须位于现有静态范围 `0x40..0xBF`；控制帧和诊断帧不能绑定给业务 Task。
- 接收数据的长度、QoS 与投递模式必须和表项匹配，不匹配立即拒绝并计数。

这样可以先保证所有权、延迟和 RAM 上界清楚。一个 Endpoint 多消费者的自动扇出会在后续“发布表”阶段独立加入，首版不含隐式复制。

## 6. 三条实际数据路径

### 6.1 本机 Local Fast Path

条件：`destination == router->local_node_id`。

```text
Task A
  -> ucn_service_send()
  -> 查 Endpoint 固定绑定
  -> 校验 ready / QoS / payload 长度
  -> Q0 FIFO 或 Q1 Latest Inbox
  -> Task B 在自己的上下文读取
```

该路径的特征：

- 没有 UCN 帧头、CRC、序号、TTL、Path ID、加密 Tag、Link 排队或无线开销。
- Q0 保持 FIFO；Q1 只保留最新值。
- 返回值仅表示“本机是否已接受到指定 Inbox”，不代表 Task B 已经执行完控制动作。
- 本机数据无需网络安全 Provider；但 Router 仍可执行本机 `source_service_id -> endpoint` 的允许表，避免不应访问控制 Endpoint 的任务越权。

### 6.2 远端 Direct / 已缓存路由路径

条件：目标不是本 Node，并且 Protocol Task 从 Remote TX Request Queue 取到请求。

```text
Task A -> Remote TX Request Queue -> Protocol Task
      -> ucn_node_send_endpoint()
      -> 现有 Q0/Q1、Security、Policy、Route Cache、Link
      -> 目标 Node Endpoint callback -> 目标 Router -> 目标 Service Inbox
```

这里“直连”或“已缓存路径”只是 Core 的路由结果；T25 不自行选 WiFi、UART 或 CAN，也不绕过已实现的 Policy/Path/Bearer 逻辑。

### 6.3 远端未知多跳路径

远端 Q1 请求继续遵守现有 Core：如产品允许，`ucn_node_send_endpoint()` 可触发受限 RREQ/待发合并；Q0 仍必须已有直连或预建路径，否则立即失败并由本地安全状态机处理。

```text
本机 Service Router 只负责“把请求交给 Protocol Task”
Protocol Task 仍负责“是否寻路、如何转发、是否加密、何时失败”
```

因此 T25 不会把本机任务通信变成新的路由层，也不会破坏已有指定路径或 Q1 流亲和均衡。

## 7. Q0/Q1 Inbox 语义与背压

| 项目 | Q0 Critical | Q1 Realtime |
| --- | --- | --- |
| 典型用途 | 解锁/停机/模式切换/舵机目标 | IMU、姿态、气压、温度、电源状态 |
| 本机容器 | 固定深度 FIFO | 每 Endpoint 一个 Latest Slot（或固定少量槽） |
| 新数据到满容器 | 立即返回 `UCN_ERR_NO_SPACE`，计数；绝不静默覆盖旧命令 | 覆盖尚未消费的旧值，计入 `q1_overwrites` |
| 顺序 | 保序 | 只保证最新样本，不保证全部历史样本 |
| 等待方式 | 发送方 API 不阻塞 | 发送方 API 不阻塞 |
| 网络未知路由 | 不等 RREQ；调用方转本地安全逻辑 | 可按已有 Core 策略受限等待/合并 |

关键区别是：**Q1 覆盖是有意的实时策略，不是“丢包被隐藏”；Q0 满队列必须是显式故障。**

首版采用“固定 Payload 副本”时，每个 Q0 队列槽与 Q1 Latest Slot 都拥有完整的固定长度数据区。所有可占用的 RAM 都能在编译期计算；调用者返回后即可复用自己的栈或临时 Buffer。

## 8. 固定资源配置方法

用户不需要现在为所有 MCU 设一个统一 RAM 数字。T25 应给出编译期 Profile，由每种目标板根据剩余 RAM 选择容量。

```c
/* 示例默认值，仅是起始 Profile；产品在开工前按 RAM 预算冻结。 */
#define UCN_SERVICE_MAX_BINDINGS          8U
#define UCN_SERVICE_REMOTE_TX_Q0_DEPTH    4U
#define UCN_SERVICE_REMOTE_TX_Q1_DEPTH    4U
#define UCN_SERVICE_DEFAULT_Q0_DEPTH      4U
#define UCN_SERVICE_Q1_LATEST_SLOTS       1U
#define UCN_SERVICE_MAX_PAYLOAD_BYTES     UCN_MAX_PAYLOAD_BYTES
```

资源核算必须按真实配置完成，不以示例数字作承诺：

```text
Service Router 静态 RAM
  = Binding 状态
  + Σ(Q0 Inbox 深度 × 每槽消息大小)
  + Σ(Q1 Latest Slot × 每槽消息大小)
  + Remote TX Q0/Q1 深度 × 每槽消息大小
  + 固定统计、Port 句柄和必要的同步对象
```

其中“每槽消息大小”至少包括 destination、endpoint、traffic class、payload length、时间/序号（如产品需要）和 `max_payload` 数据区。C99 单元测试应对每个 Profile 输出 `sizeof(ucn_service_router_t)`；FreeRTOS 实机再测 Heap、各业务 Task 栈水位和 Protocol Task 栈水位。

## 9. 数据所有权：先复制，后续才零拷贝

### 9.1 首版：固定副本（必须）

首版在 `ucn_service_send()` 或 `ucn_service_deliver_remote()` 接受成功前，复制 Payload 到 Router 所拥有的固定槽位。优点是：

- 任务可立刻复用栈、DMA 临时区或采样 Buffer。
- Q0/Q1、远端 TX 与 FreeRTOS Port 都使用同一所有权规则。
- 单元测试可以精确证明满队列和覆盖时的行为。

限制是传感器数据会有一次本机复制。对 IMU 这类很小且高频的 Payload，先用 Q1 Latest 控制队列深度，通常比过早引入复杂的引用计数更安全。

### 9.2 后续阶段：固定 Buffer Pool（可选）

只有当目标板的 RAM/CPU 实测证明复制不可接受时，才增加独立的 `ucn_service_buffer_pool`：

```text
生产者申请固定块 -> 写入 -> Router 增加引用 -> Inbox/远端 TX 持有
消费者完成 -> release
最后一个引用 release -> 块回收到固定池
```

该阶段必须满足：

- 固定块数、固定块大小、无 `malloc/free`；池耗尽返回明确错误。
- 句柄必须带 generation，防止旧句柄在块复用后误访问。
- 禁止异步保存普通裸指针；必须通过受控句柄取得数据。
- 远端发送仍需由 Protocol Task 编码为帧；若 Link 驱动不会立即复制，Adapter 也必须拥有受控生命周期。
- 单元测试必须覆盖双消费者、远端 TX 未完成、Q1 覆盖释放、任务重启和池耗尽。

零拷贝不是 T25 首版的验收条件，不能为了节省一次复制而牺牲可验证性。

## 10. 远端接收、任务重启和安全边界

### 10.1 远端入站流程

目标 Node 的 Core 完成帧校验、路由终止与安全处理后，已有的 Endpoint handler 被调用。T25 将把该 handler 绑定为极薄的 Adapter：

```text
Core Endpoint callback
  -> ucn_service_deliver_remote(router, frame)
  -> 按 frame.message_type 查 Service Binding
  -> 校验 QoS/长度/本机策略
  -> 放入目标 Inbox
  -> 立即返回
```

中继节点永远不触发目标业务 Endpoint；透明密文中继仍由 Core 按现有规则处理。只有最终目标 Node 才会进入上述 Service Router。

### 10.2 Task 重启/未就绪

Task 重启不能等价于 Node 离网：Node ID、邻居、路由和 Link 全部继续存在。Router 为每个 Binding 保存 `ready` 状态：

- 未就绪时，默认拒绝本机/远端投递并增加 `destination_not_ready`；不假装成功。
- 需要“重启期间只保留最后一个传感器状态”的 Endpoint，可在产品配置中明确选择 Q1 reset policy；首版默认清空旧值后等待新样本。
- 对 Q0 控制 Endpoint，Task 重启后必须由本机安全状态机进入安全状态，而不是从过期网络命令自动恢复。

### 10.3 安全

- 跨 Node：继续使用现有 Endpoint 安全策略、Security Provider、重放保护和透明转发规则；T25 不降低任何网络安全要求。
- 本机：不需要走网络 AEAD，但可配置最小的静态本机 ACL，例如 `Control Service` 可向 Servo Endpoint 发送、Logger 不能发送。
- 远端 Endpoint ACL 与本机 Service ACL 是两层不同的规则；都只能由产品配置冻结，默认拒绝高风险控制接口比默认允许更安全。

## 11. 业务示例：IMU、控制和舵机

| Endpoint（示例） | Producer | Consumer | QoS / Inbox | 本机执行 | 远端执行 |
| --- | --- | --- | --- | --- | --- |
| `0x40 IMU0_RAW_V1` | IMU Task | 本机 Control Task | Q1 Latest | 一次本机投递，控制只读取最新采样 | 交给 Protocol Task；目标 Node 的 Control Inbox 只留最新样本 |
| `0x60 MOTOR0_COMMAND_V1` | Control Task | Actuator Task | Q0 FIFO | 显式接受或满队列失败 | Q0 只走直连/预建路由；未知路径立即失败 |
| `0x61 SERVO0_TARGET_V1` | Control Task | Servo Task | Q0 FIFO | Servo Task 执行 PWM、限位和超时安全 | 目标 Node 收到后投递 Servo Inbox；网络不直接驱动 PWM |
| `0x50 POWER_STATUS_V1` | Power Task | Control Task | Q1 Latest | 本机状态读最新值 | 可按产品需要发到远端监控 Node |

一个节点持续获取另一个节点的 IMU 数据的方式并不会改变：源 IMU Task 按周期向目标 Node 的 `0x40` 发送 Q1，目标 Router 将每个到达样本投递到本机 Control Inbox。由于是 Q1 Latest，即使暂时积压，控制任务读到的也是最新样本而不是旧队列。具体字段与编码见 [T25.0 首版 Endpoint 与 Service 契约](UCN_T25_首版Endpoint与Service契约.md)。

如果 IMU Task 同时要给**本机**控制任务和**远端**控制节点，T25 首版采用两个显式发送：一个目标为本 Node，另一个目标为远端 Node。两条路径独立，远端异常不会阻塞本地控制。后续若这种模式很常见，再以静态发布表实现受控扇出和 Buffer Pool；不能在首版暗中复制给任意订阅者。

## 12. 推荐模块与文件落点

| 位置 | T25 动作 | 说明 |
| --- | --- | --- |
| `include/ucn/ucn_service.h` | 新增 | 纯 C Router、固定配置、统计和 API 声明；不得包含 FreeRTOS。 |
| `src/ucn_service.c` | 新增 | Service 表校验、本机直投、远端 TX 固定队列、入站投递、统计。 |
| `tests/test_service.c` | 新增 | 纯 C Router 的所有单元/虚拟测试。 |
| `tests/test_main.c`、`CMakeLists.txt` | 修改 | 注册新测试；保留现有四个构建 Profile。 |
| 产品 Port，例如 ESP 工程的 `src/ucn_service_freertos.cpp` | 新增 | 静态 FreeRTOS Queue/任务通知映射；只依赖 `ucn_service.h`。 |
| 产品 Endpoint 配置文件 | 新增或修改 | 冻结 ABI、绑定表、队列容量、本机 ACL 和 Service 启动顺序。 |
| `ucn_node.h/.c`、帧/路由/安全文件 | 首版不改或仅注册既有回调 | 不把 RTOS 或 Task 概念反向渗入 Core。 |

实现时先让 `ucn_service.c` 能在 Windows CMake 中完全测试，再由 ESP32 Port 提供真正的 Queue/Task 映射。不可先把 FreeRTOS API 混进 Core 后再补测试。

## 13. 分阶段实施顺序与验收门禁

### T25.0：评审并冻结产品契约（已完成）

先由产品明确下列配置，未冻结前不写业务绑定代码：

1. 每个 Endpoint 的编号、Payload C 结构、字节序、单位、版本和最大长度。
2. Endpoint 属于 Q0 还是 Q1；Q0 深度、Q1 是否 Latest。
3. 每个 Endpoint 的 owner Service、允许发送者、启动/重启时 `ready` 策略。
4. 每种目标板的 Service 表数、远端 TX 深度、Payload 上限和 RAM 预算。
5. 首版是否只支持单消费者（建议：是），以及 IMU 等需不需要静态双发。

**门禁：**配置表通过人工评审；不得用“任意 Task 任意消息”代替 ABI。

### T25.1：纯 C 固定 Service Router（已完成）

已在 `include/ucn/ucn_service.h`、`src/ucn_service.c` 和 `tests/test_service.c` 中实现静态表初始化、配置拒绝、Q0 FIFO、Q1 Latest、本机直投、Remote TX Request 队列、远端入站投递和统计；不接 FreeRTOS，Router 也不直接调用 Core。

**单元测试：**

- 重复/越界 Endpoint、表满、无效 QoS、超最大长度必须拒绝。
- 本机 A→B 成功时 Link Mock 的发送计数为零。
- 未绑定、未就绪、ACL 拒绝、Q0 满、Q1 覆盖均返回正确错误码/统计。
- Q0 先入先出；Q1 只读取最后一次写入值。
- Remote 目标只进入对应 TX 队列，不进入本机 Inbox。

**实际验证：**Debug、Release、64 B 和单 Bearer Profile 的 CTest 均为 `1/1` 通过；测试还以三个 Router 模拟 A→B→C，确认 B 无法把发往 C 的帧投入自身 Service Inbox。S3 Node A/B 与 WROOM 的 PlatformIO 构建也确实编译了 `ucn_service.c`；当前测试固件没有实例化 Router，故 RAM/Flash 无新增运行时结论。

### T25.2：与现有 Core 的协议任务桥接（已完成）

已新增 `ucn_service_bridge.h/.c` 与 `test_service_bridge.c`。Bridge 初始化时核对 Router/Node 的本 Node ID 一致；安装前预检 Endpoint handler 所有权和固定槽位，拒绝覆盖其他组件。Protocol Task 每轮最多调用 `ucn_service_protocol_bridge_step()` 指定次数；其只在当前 Task 中调用现有 `ucn_node_send_endpoint()`。目标 Endpoint callback 只调用 `ucn_service_deliver_remote()` 并记录本机投递结果。

**模拟测试：**

- 虚拟 A→B→C：A 发往 C 的 Endpoint，B 只转发，C 的 Router 投递 C 本机 Inbox。
- Q1 未知路由仍由 Core 的受限等待/RREQ 处理；Q0 未知路由立即失败。
- 保护帧经过中继时，B 不出现业务 Task 投递；C 才能获得 Payload。
- Protocol Task 每轮处理上限后仍能继续执行 `ucn_node_step()`，不被业务发送队列饿死。

**实际验证：**`test_service_bridge.c` 覆盖 Node ID 不一致、未安装 handler、外部 handler 冲突、无路由 Q0 显式失败和 Bridge 统计；实际虚拟 A→B→C 以每轮上限 1 先发送 Q0、再发送 Q1，B 的通用 RX 回调保持 0，C 的 Router 获得两条 Inbox 数据。另验证 C Service 未就绪时，源端 Core 发送仍成功、但 C 的 Bridge 记录本机投递拒绝，不误报为端到端 ACK。Debug、Release、64 B、单 Bearer CTest 均 `1/1` 通过。

**门禁：**Router 与 Core 的连接只发生在 Protocol Task；没有新增业务帧字段或 Task ID 上线。

### T25.3：FreeRTOS/ESP32 Port（已完成构建）

已在 `E:\File\PlatformIO\ESP32_UCN\ESP32S3_N16R8_UCN_Test1` 新增 `include\ucn_service_freertos_port.h`、`src\ucn_service_freertos_port.cpp`，并在 `src\main.cpp` 实例化 R1 的 6 个 Binding、Router、Bridge 和一个 `xTaskCreateStatic()` 创建的业务分发 Task。Arduino `loopTask` 仍是唯一 Protocol Task：先 Pump Adapter，再以固定预算 `2` 调用 Bridge，最后调用 `ucn_node_step()`。

```text
业务 Task --受短临界区保护--> Router Inbox / Remote TX
                                       │
loopTask(唯一 Node owner) --Bridge--> Core / Link
远端 Endpoint --> Bridge hook --> Router Inbox --> 1 B Endpoint 事件 Queue + 任务通知
                                                      │
                                             静态业务分发 Task 读取 Inbox
```

- Router Inbox 是**唯一的 Payload 队列**；两个 `xQueueCreateStatic()` Queue 只传递 1 B Endpoint 唤醒事件，因此不会复制业务 Payload，也不会改变 Q0 FIFO / Q1 Latest 语义。
- Port 只允许已绑定的 Task 读取属于自己的 Service Inbox；业务 Task 的 `send()` 在短临界区内调用 Router，绝不触碰 `ucn_node_t`、Link 或路由。
- R1 占用 `0x40/0x42/0x43/0x50/0x60/0x61`；原 Ping/吞吐测试已迁到静态范围内、但不属于 R1 的保留测试号 `0xB0/0xB1`。`0xC0/0xC1` 不能使用，因为现有 Endpoint API 会显式拒绝它们。
- 周期 `SERVICE` / `RESOURCE` 日志已加入 Router/Bridge/Port 计数、事件丢弃数、业务 Task 收包数和 `service_stack_hw`；这些字段等待烧录后读取，当前不能写成实测数据。

**构建验证：**四个 CMake Profile 的 CTest 均 `1/1` 通过；S3 Node A/B 均为 RAM `47,180 / 327,680 B`、Flash `594,783 / 6,553,600 B`，ESP-WROOM-32 为 RAM `49,320 / 327,680 B`、Flash `621,271 / 1,310,720 B`。相对于 T25.2 未实例化 Router/Bridge 的镜像，三目标均增加 `2,640 B` 静态 RAM；S3 Flash 增加 `3,412 B`，WROOM Flash 增加 `3,464 B`。这些是完整测试固件增量，不等于纯 Router 或某一个 Queue 的独立成本。

**未完成的实机门禁：**尚未上传新固件，未验证启动日志、Task 高水位、真实 Q0/Q1、事件 Queue 满、断链本地安全或端到端时延。它们属于 T25.4。

### T25.4：两板及多板集成

在两块 ESP32-S3 上用真实 Adapter 运行：本机 IMU→Control→Servo 与远端 IMU/Q1、远端 Control/Q0 并行。随后扩展到多 MCU，验证跨中继路径。

**实机验收：**

- 本机持续 Q1 和远端持续 Q1 同时进行；本机控制周期不得依赖远端链路。
- 断开远端 WiFi/UART 后，本机 Servo 安全逻辑仍在规定时限内执行。
- 记录本机投递时延、远端端到端 P50/P95、Q0 队列满次数、Q1 覆盖、远端 TX 背压、丢失/乱序和 Heap/Stack。
- 若启用指定 Path 或 `AUTO_BALANCE`，验证它们只影响远端路径，不影响本机 Fast Path。

**门禁：**实机日志和配置文件归档；不能仅凭编译通过就声称实时性或安全性已验收。

### T25.5：可选优化，不阻塞首版

仅在 T25.1～T25.4 证明瓶颈后，按需要增加固定 Buffer Pool、静态发布/订阅扇出、更多诊断或 Q2/Q3 支持。每项都应单独建任务和资源报告，禁止把它们混入首版。

## 14. 测试矩阵

| 类别 | 场景 | 应观察的结果 |
| --- | --- | --- |
| 配置 | 重复 Endpoint、越界 Endpoint、超限 Payload、非法 Q0/Q1 | 启动/初始化失败，错误位置明确。 |
| Local Q0 | 连续控制命令、消费者变慢、队列满 | FIFO 保序；满时显式 `NO_SPACE`；无静默覆盖。 |
| Local Q1 | 高频 IMU、消费者较慢 | 只保留最新样本；覆盖计数增长；不会无限堆积。 |
| Local 隔离 | 本机 IMU 与远端 Link 断开 | 本机 Control 继续取最新本机样本；不调用 Link。 |
| Remote TX | 多任务同时发远端 Q0/Q1 | 固定队列有界；Protocol Task 独占 Core；背压可见。 |
| 三节点 | A→B→C 的 Endpoint 发送 | B 不运行 C 的业务回调；C Inbox 获得数据。 |
| 安全 | 加密端点通过中继 | 只有目标 C 在安全处理后投递；中继不解密。 |
| 重启 | 某 Service 未就绪/重启 | Node 仍入网；投递拒绝或按配置处理；Q0 本地安全动作成立。 |
| 资源 | 不同静态 Profile | `sizeof`、RAM、Heap、各任务栈水位和队列容量均可复现。 |
| 回归 | 四个 CMake Profile + ESP 构建 | 既有 v4 路由、Path、Policy、Bearer、诊断测试无退化。 |

## 15. 成功标准和不能声称的能力

T25 通过后，可以准确声称：

- 业务 Task 使用统一的 `Node + Endpoint + QoS + Payload` 语义；同 Node 自动低开销投递，跨 Node 自动进入现有 UCN Core。
- 一个 MCU 内的任务不会互相并发操作 `ucn_node_t`；协议任务仍是唯一网络所有者。
- IMU 等实时状态可用 Q1 Latest 同时服务本机或远端消费者；远端链路故障不会阻塞本机的安全/控制路径。
- Q0/Q1 的队列语义、内存上界、拒绝和覆盖都有可测统计。

在完成 Buffer Pool、订阅目录、可靠传输和完整实机验收前，不能声称：

- 任意数量 Task/消费者的自动发布订阅或零拷贝。
- 网络能承担 PWM/FOC/硬实时闭环。
- 任意负载下无丢失、绝对实时或“无缝”切换。
- 本机 Fast Path 已绕过所有产品安全/权限设计。

## 16. 建议的开工决策

当前下一步建议按以下顺序继续：

1. 先冻结首批四个 Endpoint：IMU、Control、Servo、Power 的 Payload ABI 和 Q0/Q1 规则。
2. 首版坚持“单 Endpoint 单消费者 + 固定副本”；本机/远端双消费者先由业务显式双发。
3. T25.1～T25.3 已通过纯 C Router、Protocol Task Bridge、ESP32 静态 Port 与构建门禁；下一项是 T25.4 的 WiFi/UART 台架实测。
4. T25.4 用已存在的 WiFi/UART 双 Bearer 台架测量本机/远端并发、队列/栈和端到端时延。
5. 只有实测显示复制或双发成为瓶颈，才启动独立的 Buffer Pool/静态扇出优化任务。

这条路径让 UCN 保持“MCU 网络 Core 小、产品本地调度快”的分层：网络能力继续统一，任务调度不被错误地网络化。
