# UCN 使用与调用手册

> 适用版本：UCN v5 V5-07 当前 Core。默认固定 W3；产品可在注册 Link/安装 Security 前设置固定域，或显式开启路由感知自动选档。产品工程的 Adapter、密钥、板级引脚和业务 Endpoint 仍需自行实现。
> 目标：让业务代码只关心“发给哪个 Node 的哪个 Endpoint、什么 QoS”，而不关心数据当前经过 Wi-Fi、UART、CAN、BLE 或其他 Bearer。

## 1. 先理解 UCN 在系统中的位置

UCN 是 MCU 优先的分布式通信 Core，不是 Linux 网络替代品，也不是某个 Wi-Fi Mesh 驱动。每块 MCU 是一个 **Node**；Node 内的传感器、控制、执行器等业务任务是 **Service**；每个可通信的数据项是一个静态 **Endpoint**。

```mermaid
flowchart LR
    A[业务任务 / Service] -->|本机：固定 Inbox| R[Service Router]
    R -->|远端：固定 TX Request| B[唯一 Protocol Task]
    B --> N[UCN Node / 路由 QoS 安全]
    N --> L[Link: ESP-NOW UART CAN ...]
    L --> N2[对端 UCN Node]
    N2 --> B2[对端 Protocol Task]
    B2 --> R2[对端 Service Router]
    R2 --> C[目标业务任务]
```

这张图包含两个重要边界：

- **一块 MCU 只有一个 `ucn_node_t` 和一个拥有它的 Protocol Task。** 业务任务、ISR、驱动回调都不能并发调用 `ucn_node_send_endpoint()`、`ucn_node_receive()` 或 `ucn_node_step()`。
- **Node 内的消息不绕无线。** Service Router 识别目的 Node 等于本机时，直接复制到目标 Service 的固定 Inbox；只有跨 MCU 时才交给 Bridge 和 UCN Node。

Linux、ROS 2、地面站可以通过一个 Link/Adapter 作为普通 Node 接入，但 MCU 自组网、路由和转发不依赖 Linux。

## 2. 使用前必须冻结的配置

不要把以下信息交给运行时“猜测”。它们应是产品配置、编译期配置或受控 Flash 配置。

| 项目 | 要确定什么 | 当前做法 |
| --- | --- | --- |
| `network_id` | 哪些设备属于同一 UCN 网络 | 同一网络必须一致；不同网络的帧会被拒绝。 |
| `node_id` | 每块 MCU 的稳定逻辑身份 | 可由 MAC 派生作默认值，但产品建议支持 Flash/编译期手动指定，网络内不得重复。 |
| Wire Profile | 固定发送档、最大接收档、是否自动最小档 | 默认固定 W3；`ucn_node_set_wire_profiles()` 必须在 Link/Security 前调用，自动模式需显式开启。 |
| Endpoint ABI | 数据代表什么、长度/字节序/单位/QoS/消费者 | 静态冻结；语义变化新分配 Endpoint，不复用旧 ID。 |
| Service Binding | 哪个本机任务拥有哪个 Endpoint | R1 是一 Endpoint 一消费者，固定表、无动态注册。 |
| Link/Bearer | UART、ESP-NOW、CAN 等如何发收、MTU、对端身份 | 一个 Neighbor 可有多个 Bearer；业务不直接选择物理介质。 |
| 安全策略 | 哪些 Endpoint 可明文/必须端到端保护/允许何种转发 | Core 只提供 Provider 边界，密钥、身份和 AEAD 必须由产品实现。 |
| 路由策略 | 自动、固定、主备或 Q1 均衡 | 默认 `AUTO_BEST`；固定路径和均衡仅在已安装、已验证 Path 后启用。 |
| Protocol Task 时限 | 最大 Step 间隔、最大 Block、每轮 Pump/Bridge 预算、Link `send()` WCET | `UCN_MAX_STEP_INTERVAL_MS` 默认 10 ms；产品必须用自己的节点/Bearer 上限通过维护上界门禁。 |

## 3. 模块与“该由谁调用”速查

| 模块/头文件 | 解决什么 | 调用者 | 绝不能做什么 |
| --- | --- | --- | --- |
| `ucn.h`、`ucn_types.h` | 版本、配置、错误码、帧和 QoS 基础类型 | 启动初始化、所有层 | 直接伪造线帧。 |
| `ucn_frame.h` | 帧编码、解码、CRC 与 E2E AAD 辅助 | Core 单测、Adapter/安全 Provider 的低层实现 | 业务 Task 手工编码业务帧。 |
| `ucn_endpoint.h` | 静态 Endpoint/控制消息编号合法性判断 | 产品 ABI 定义、配置检查 | 把动态 Endpoint 当作当前 R1 产品 ABI。 |
| `ucn_node.h` | 邻居、路由、Endpoint 发送/接收、控制面、调度 | **唯一 Protocol Task** | 被多个业务 Task 或 ISR 直接调用。 |
| `ucn_neighbor.h` | Neighbor/Bearer 状态和准入授权回调类型 | 产品 Join Provider、诊断读取 | 绕过 HELLO/准入直接把陌生节点当作可信 Neighbor。 |
| `ucn_link.h` | 把一种底层介质封装为 `open/send/poll_rx/status/metrics` | Adapter/驱动层 | 让业务层看到 MAC、串口号或 CAN ID。 |
| `ucn_port.h` | 时间、随机数、持久计数器与临界区的 Port 操作表 | Adapter RX Queue 和产品 Port | 把平台 API 泄漏到 C99 Core。 |
| `ucn_adapter.h` | 驱动回调到 Protocol Task 的固定 RX 队列 | Adapter 回调入队、Protocol Task Pump | 在中断/驱动回调内执行路由、解密或业务回调。 |
| `ucn_service.h` | Node 内任务通信：本机直投、远端请求、Q0/Q1 Inbox | Service/Port，短临界区保护 | 直接访问 Link 或 `ucn_node_t`。 |
| `ucn_service_bridge.h` | Router 和 Node 的唯一桥接 | **唯一 Protocol Task** | 创建 RTOS Task、动态队列或重试伪 ACK。 |
| `ucn_policy.h`、`ucn_path.h` | 指定路径、主备、Q1 流亲和均衡 | 产品的受控配置/管理层 + Protocol Task | 对 Q0 使用逐帧均衡，或未授权安装路径。 |
| `ucn_security.h` | 端到端保护/ACL/计数器 Provider 契约 | 产品安全模块 + Protocol Task 初始化 | 把 CRC 当认证，或认为 Core 内置了生产 AEAD。 |

### 3.1 公开 API 的调用层级

以下按“谁可以调用”列出当前公开函数。`ucn_path.h` 和 `ucn_policy.h` 的低层 `ucn_path_*` / `ucn_policy_*` 函数由 Node 内部维护；产品代码优先调用 `ucn_node_*` 包装接口，避免破坏 Node 内部状态一致性。

| 场景 | 推荐 API | 调用时机 |
| --- | --- | --- |
| 启动检查 | `ucn_version()`、`ucn_validate_config()`、`ucn_node_init()` | 只在启动/重建 Node 时。 |
| 安全和准入配置 | `ucn_node_set_security()`、`ucn_node_set_security_policy()`、`ucn_node_set_endpoint_security_policy()`、`ucn_node_set_join_policy()` | 注册 Link 前完成；产品需要时安装 Snapshot/Policy/Path authorizer。 |
| Link 生命周期 | Link `ops->open()`、`ucn_node_register_link()`、Adapter 的 peer 绑定/释放 | Adapter 创建/移除物理承载时；不由业务 Task 调用。 |
| 自动入网 | `ucn_node_observe_neighbor()`、`ucn_node_probe_neighbor()`、`ucn_node_broadcast_hello()` | 只由发现 Adapter/Protocol Task 在受控节拍调用。 |
| 静态/手动路由 | `ucn_node_add_route()`、`ucn_node_discover_route()`、`ucn_node_refresh_route()`、`ucn_node_route_pending()` | 产品启动静态路由，或管理/测试层；业务优先发送 Endpoint。 |
| 收/发 Endpoint | `ucn_node_set_endpoint_handler()`、`ucn_node_send_endpoint()` | 单 Task 简化产品或 Bridge；多 Task 业务应改用 Service API。 |
| 兼容原始消息 | `ucn_node_set_rx_handler()`、`ucn_node_send()`、`ucn_node_enqueue()` | 仅已有原始 message-type 应用或测试；新业务优先 Endpoint API。 |
| 手动 Path/策略 | `ucn_node_install_local_path()`、`ucn_node_send_path_install()`、`ucn_node_set_policy_path()`、`ucn_node_set_route_policy()` | 受授权管理配置；不能放在高频业务循环。 |
| 只读策略/质量 | `ucn_node_find_route_policy()`、`ucn_node_find_policy_path()`、`ucn_node_find_q1_flow()`、`ucn_node_get_link_quality()`、`ucn_node_get_policy_stats()` | 管理/日志任务经安全同步快照读取。 |
| 诊断控制面 | `ucn_node_request_path_trace()`、`ucn_node_request_node_snapshot()`、`ucn_node_request_policy_diagnostic()` | 低频、显式用户/管理触发。 |
| Protocol Task 主循环 | `ucn_adapter_rx_pump()`、`ucn_service_protocol_bridge_step()`、`ucn_node_step()`、`ucn_node_receive()` | 后三者仅由 Node owner；通常 `receive()` 由 Adapter Pump 间接调用。 |
| 任务通信 | `ucn_service_send()`、`ucn_service_inbox_take()`；ESP32 用 `UcnServiceFreeRtosPort::send()` / `inbox_take()` | 业务 Task 可调用；Port 负责短临界区，不把 Node 暴露给 Task。 |

## 4. QoS 与 Endpoint：先选对语义

| 类别 | API 常量 | 应用语义 | 典型对象 | 当前约束 |
| --- | --- | --- | --- | --- |
| Q0 | `UCN_TRAFFIC_Q0_CRITICAL` | 最重要、失败必须显式处理 | 电机/舵机安全控制 | 不因未知路由等待或自动寻路；本地必须有超时失效安全。 |
| Q1 | `UCN_TRAFFIC_Q1_REALTIME` | 只关心最新值，旧值可覆盖 | IMU、气压、温度、状态 | 可在未知多跳目的地触发受限寻路；可使用流级均衡。 |
| Q2 | `UCN_TRAFFIC_Q2_NORMAL` | 一般业务 | Core 可承载的普通数据 | 当前 Service R1 未向业务任务开放该类。 |
| Q3 | `UCN_TRAFFIC_Q3_BULK` | 低优先级批量数据 | Core 可承载的大宗数据 | 当前 Service R1 未向业务任务开放该类。 |

R1 已冻结的业务 Endpoint 如下。Payload 按文档约定采用 big-endian；不要在不同 MCU 间直接发送 C 结构体内存镜像。

| Endpoint | 生产者 → 消费者 | QoS | 最大 Payload | 用途 |
| --- | --- | --- | ---: | --- |
| `0x40 IMU0_RAW_V1` | SENSOR → CONTROL | Q1 | 24 B | IMU 原始数据。 |
| `0x42 BAROMETER0` | SENSOR → CONTROL | Q1 | 16 B | 气压数据。 |
| `0x43 TEMPERATURE0` | SENSOR → CONTROL | Q1 | 12 B | 温度数据。 |
| `0x50 POWER_STATUS` | POWER → CONTROL | Q1 | 16 B | 电源状态。 |
| `0x60 MOTOR0_COMMAND` | CONTROL → ACTUATOR | Q0 | 16 B | 电机命令。 |
| `0x61 SERVO0_TARGET` | CONTROL → ACTUATOR | Q0 | 16 B | 舵机目标。 |

完整 ABI、字段单位和 Q0 `valid_for_ms` 规则见 [T25 首版 Endpoint 与 Service 契约](UCN_T25_首版Endpoint与Service契约.md)。

## 5. 最小 Node/Link 初始化

### 5.1 先初始化 Node

所有状态对象建议是静态对象或明确生命周期的产品对象，避免堆分配。

```c
#include "ucn/ucn.h"
#include "ucn/ucn_node_storage.h"

static ucn_node_t g_node;

static void protocol_init(void)
{
    const ucn_config_t config = {
        .network_id = UINT32_C(0x55434E01),
        .node_id = UINT32_C(0x0000000A),
        .default_hop_limit = 8U,
    };

    /* 任一步返回非 UCN_OK，停止进入业务运行态并记录错误。 */
    if (ucn_node_init(&g_node, &config) != UCN_OK ||
        ucn_node_set_wire_profiles(&g_node,
            UCN_WIRE_PROFILE_W1_EDGE,
            UCN_WIRE_PROFILE_W3_BACKBONE) != UCN_OK ||
        ucn_node_set_wire_profile_auto(&g_node, true) != UCN_OK) {
        product_enter_safe_mode();
    }
}
```

固定域模式只省去自动决策，适合资源/地址范围完全冻结的产品；自动模式不是协商任意格式，而是从四个官方档位中选满足本帧与当前出口的最小档。控制帧继续使用固定发送档，HELLO 会学习 `link->peer_wire_profile`；静态链路也可在注册后用 `ucn_node_set_link_wire_profile_limit()` 配置可信上限。中继不重新选档，防止在途帧被静默改写。

只有静态分配并独占 Node 的 Protocol Task 所在 `.c/.cpp` 文件需要包含
`ucn_node_storage.h`。业务任务、Adapter 声明和只传递 `ucn_node_t *` 的模块应
只包含 `ucn_node.h`；存储头中的字段是实现布局，不是应用 ABI，禁止直接读写。

`node_id` 不是入网后临时分配的地址；它是设备稳定身份。可让首次启动写入 Flash，或在每块板的产品配置中固定。更换同一板的物理链路不应改变 Node ID。

### 5.2 为每种物理承载实现一个 Link

`ucn_link_t` 对 Core 的最小可见信息是：`link_id`、`mtu`、直连 `peer_node_id`、`context` 和 `ops`。Adapter 必须实现：

- `open()`：准备介质；
- `send()`：同步接收一帧由 Core 编好的数据，立即发送或有界提交给本 Adapter 的 TX 队列；
- `poll_rx()`：仅用于轮询型驱动；回调型驱动可以返回 `UCN_OK`；
- `get_status()`：报告 `is_up`、MTU、收发错误；
- `get_metrics()`：报告平滑后的通用 Cost，及可选 RTT、发送失败率、队列压力；
- `close()`：释放本 Adapter 的硬件状态。

```c
static ucn_link_t g_uart_link = {
    .ops = &g_uart_link_ops,       /* 产品实现的 6 个操作 */
    .context = &g_uart_context,
    .link_id = 0x70U,
    .mtu = UCN_MAX_FRAME_BYTES,
    .peer_node_id = UINT32_C(0x0000000B),
};

static void register_uart_link(void)
{
    (void)g_uart_link.ops->open(&g_uart_link);
    (void)ucn_node_register_link(&g_node, &g_uart_link);
}
```

若同一个对端同时有 UART 与 ESP-NOW，注册两条 Link 即可。它们属于同一个 `peer_node_id` 的两个 Bearer；Core 会保留一个逻辑 Neighbor，并按状态/Cost/质量管理主备。不要用“两个 Node ID”模拟同一设备的两个接口。

### 5.3 RX 回调只入队，Protocol Task 再 Pump

```c
/* 驱动回调或 ISR 后半部：不调用 Node，不运行应用回调。 */
void uart_rx_callback(const uint8_t *data, size_t length)
{
    (void)ucn_adapter_rx_enqueue(&g_uart_rx_queue, &g_uart_link, data, length);
}

/* 唯一 Protocol Task / loopTask。now_ms 来自单调时钟。 */
void protocol_task_iteration(uint32_t now_ms)
{
    size_t pumped = 0U;
    uint8_t processed = 0U;

    (void)ucn_adapter_rx_pump(&g_uart_rx_queue, &g_node, 4U, &pumped);
    (void)ucn_service_protocol_bridge_step_at(&g_service_bridge,
                                               now_ms,
                                               2U,
                                               &processed);
    (void)ucn_node_step(&g_node, now_ms);
}
```

该循环不是“尽量快即可”，而是产品时限契约。默认 Full Profile：

```text
maintenance_bound = (4 + 1) × 10 ms × 8 Neighbor × 2 Bearer = 800 ms
1 s Heartbeat + 800 ms < 3 s Suspect
```

构建会拒绝侵入 Suspect 窗口的配置。产品还必须冻结 Protocol Task 优先级、最大 Sleep/Block、每个 Adapter Pump 数量、Bridge 数量和 Link `send()` 最坏执行时间。运行时通过 `ucn_node_get_stats()` 观察 `last_step_ms`、`max_step_gap_ms`、`step_interval_violations`、`max_heartbeat_service_delay_ms`、`max_probe_service_delay_ms`；第一次 Step 不算 Gap，计时支持 `uint32_t` 回绕。出现违规时 Node 继续调度并报警，产品不得用“停止协议任务”处理已经发生的超时。

对于发现型无线 Adapter，在受控周期调用 `ucn_node_broadcast_hello(&g_node, link, now_ms)`；收到物理层新对端后，由 Adapter 映射地址到 Candidate Link，再通过 `ucn_node_observe_neighbor()` / `ucn_node_probe_neighbor()` 进入准入流程。不要每收到一个业务包就全网广播 HELLO。

## 6. 直接使用 Node API 做跨 MCU 通信

这是不使用 Service Router 时的 Core 用法，适合 Host、测试程序或只有一个业务执行上下文的极小产品。多 Task MCU 项目优先使用第 7 节的 Service API。

### 接收侧：注册一个 Endpoint handler

```c
static void imu_rx(void *context, const ucn_frame_t *frame)
{
    /* 回调在 Protocol Task 上下文运行：只做快速校验/入本机业务队列。 */
    (void)context;
    if (frame->payload_length == 24U) {
        application_copy_imu(frame->payload);
    }
}

(void)ucn_node_set_endpoint_handler(&g_node, 0x40U, imu_rx, NULL);
```

一个 Endpoint handler 只能有一个所有者；Service Bridge 安装后，不要再为相同 Endpoint 注册第二个 handler。

### 发送侧：只给 Node、Endpoint、QoS 和 Payload

```c
const uint8_t imu_payload[24] = { /* 按 ABI 编码 */ };
ucn_result_t rc = ucn_node_send_endpoint(&g_node,
                                         UINT32_C(0x0000000B),
                                         0x40U,
                                         UCN_TRAFFIC_Q1_REALTIME,
                                         imu_payload,
                                         sizeof(imu_payload));
if (rc != UCN_OK) {
    application_report_ucn_send_error(rc);
}
```

默认 `AUTO_BEST` 会以目的 Node ID 为目标使用直连 Link、Route Cache 或 Q1 的受限按需寻路。调用方不传 Wi-Fi MAC、UART 号、CAN ID 或中继 Node ID。

常见返回处理：

| 返回 | 调用方应做什么 |
| --- | --- |
| `UCN_OK` | 表示本机 Core/Link 已接受本次发送，不表示对端业务一定已经执行。 |
| `UCN_ERR_LINK_DOWN` | 当前可用路径不可发送；Q0 进入本地安全策略，Q1 等下一周期新值。 |
| `UCN_ERR_NOT_FOUND` | 没有当前 Route/Path；Q1 可由 Core 开始受限寻路，Q0 不等待。 |
| `UCN_ERR_NO_SPACE` | 固定表或队列满；默认是最终失败。只有产品显式选择 S15 的 Q0 有界背压策略时，才按固定次数、间隔和 Deadline 做本机提交重试；仍禁止无限重试。 |
| `UCN_ERR_TOO_LARGE` / `UCN_ERR_ARGUMENT` | ABI 或配置错误，修正调用方。 |
| `UCN_ERR_SECURITY` / `UCN_ERR_ACCESS` / `UCN_ERR_REPLAY` | 安全策略或认证失败；记录审计事件，不能退回明文重发。 |

## 7. MCU 内多任务通信：Service Router + Bridge

### 7.1 何时使用

一个 MCU 内有 IMU Task、控制 Task、执行器 Task 时，它们也可用同一套 `(destination Node, Endpoint, QoS)` 语义通信。差别仅在目标 Node：

- 目标等于本机 Node：Router 本机 Fast Path，直接复制到目标 Inbox，不使用 Link、不编帧、不寻路；
- 目标是远端 Node：Router 复制到固定 Remote TX 队列，Bridge 在 Protocol Task 中调用 `ucn_node_send_endpoint()`；
- 远端帧到达本机：Node 的 Endpoint handler 通过 Bridge 投递 Router，再由目标业务 Task 读取 Inbox。

Router 本身不依赖 FreeRTOS。当前 ESP32 工程在其上提供 `UcnServiceFreeRtosPort`，用于静态 Queue、事件通知和业务 Task 绑定。

### 7.2 定义固定 Binding

```c
static const ucn_service_binding_t g_bindings[] = {
    { .endpoint = 0x40U,
      .owner_service_id = UCN_SERVICE_ID_CONTROL,
      .max_payload_length = 24U,
      .allowed_traffic_mask = UCN_SERVICE_TRAFFIC_MASK(UCN_TRAFFIC_Q1_REALTIME),
      .delivery_mode = UCN_SERVICE_DELIVERY_Q1_LATEST,
      .allowed_local_source_mask = UCN_SERVICE_SOURCE_MASK(UCN_SERVICE_ID_SENSOR),
      .accept_remote = true,
      .enabled_at_boot = true,
      .require_remote_q0_validator = false },
    { .endpoint = 0x60U,
      .owner_service_id = UCN_SERVICE_ID_ACTUATOR,
      .max_payload_length = 16U,
      .allowed_traffic_mask = UCN_SERVICE_TRAFFIC_MASK(UCN_TRAFFIC_Q0_CRITICAL),
      .delivery_mode = UCN_SERVICE_DELIVERY_Q0_FIFO,
      .allowed_local_source_mask = UCN_SERVICE_SOURCE_MASK(UCN_SERVICE_ID_CONTROL),
      .accept_remote = true,
      .enabled_at_boot = true,
      .require_remote_q0_validator = true },
};

static ucn_service_router_t g_router;
static ucn_service_protocol_bridge_t g_service_bridge;
static ucn_service_bridge_replay_state_t g_command_replay;

static ucn_result_t validate_motor_command(
    void *context, const ucn_frame_t *frame,
    ucn_node_id_t source, ucn_session_id_t session,
    ucn_endpoint_t endpoint, const uint8_t *payload,
    uint16_t payload_length, uint32_t now_ms)
{
    ucn_service_bridge_replay_state_t *replay = context;
    uint32_t command_id;

    (void)frame;
    (void)now_ms; /* 没有共享时钟时，用本机 watchdog，不比较两块 MCU 的 uptime。 */
    if (session == 0U || payload_length != 16U ||
        !product_motor_fields_are_safe(payload, payload_length)) {
        return UCN_ERR_SECURITY;
    }
    command_id = product_read_command_id(payload);
    return ucn_service_bridge_replay_accept_command(
        replay, source, session, endpoint, command_id);
}

static void service_init(void)
{
    const ucn_service_router_config_t router_config = {
        .local_node_id = UINT32_C(0x0000000A),
        .bindings = g_bindings,
        .binding_count = (uint8_t)(sizeof(g_bindings) / sizeof(g_bindings[0])),
    };

    ucn_service_bridge_replay_init(&g_command_replay);
    (void)ucn_service_router_init(&g_router, &router_config);
    (void)ucn_service_protocol_bridge_init(&g_service_bridge, &g_router, &g_node);
    (void)ucn_service_protocol_bridge_set_validator(
        &g_service_bridge, 0x60U, validate_motor_command, &g_command_replay);
    (void)ucn_service_protocol_bridge_install_endpoint_handlers(&g_service_bridge);
}
```

Binding 是产品 ABI 的一部分。Router 借用该数组而不复制，因此数组必须在 Router 整个生命周期内保持有效且不可修改；应像示例一样使用 `static const`，不能把函数栈上的临时数组传入。字段含义：`owner_service_id` 是唯一消费者；`max_payload_length` 和 `allowed_traffic_mask` 是硬边界；`allowed_local_source_mask` 只限制本机 Task 发起；`accept_remote` 决定是否接受远端帧；`enabled_at_boot` 决定启动后是否就绪；`require_remote_q0_validator` 要求 Bridge 在安装 handler 前找到对应 Validator，否则整体失败关闭。该标记只能用于可接收远端的 Q0 Binding。`local_node_id=0` 和广播 ID 都会被初始化拒绝。

Validator 在 Core 完成安全/解密后、Router 入队前运行，返回非 `UCN_OK` 的帧不会进入 Inbox。固定 Replay helper 对当前 `(Source, Session, Endpoint)` 只接受递增命令 ID；认证 Session 轮换后，由产品 Security 逻辑调用 `ucn_service_bridge_replay_rotate_session()`，不能看到不同 Session 就自动切换。Replay 表满返回 `UCN_ERR_NO_SPACE`，不得动态扩容。`now_ms` 来自 Node 最近一次 Step，因此产品必须同时满足 Protocol Task 最大 Step 间隔。

Validator 不规定 Payload ABI：可使用 12 B Command Guard，也可以解析产品已有的 16 B 电机命令。它只拦截远端帧，本机 Fast Path 仍由执行 Task 二次检查模式、范围、有效期、互锁和本地 watchdog。若 Task 还未创建，使用 `ucn_service_set_ready()` 显式切换。

### 7.3 从业务 Task 发送

纯 C Router 调用如下：

```c
ucn_result_t rc = ucn_service_send(&g_router,
                                   destination_node_id,
                                   UCN_SERVICE_ID_CONTROL,
                                   0x60U,
                                   UCN_TRAFFIC_Q0_CRITICAL,
                                   servo_payload,
                                   16U);
```

ESP32 业务 Task 不应直接持有 Router/Node。使用测试工程的 `UcnServiceFreeRtosPort`：

```cpp
/* 业务 Task：本机或远端的调用形式完全相同。 */
const ucn_result_t rc = g_service_port.send(destination_node_id,
                                            UCN_SERVICE_ID_CONTROL,
                                            0x60U,
                                            UCN_TRAFFIC_Q0_CRITICAL,
                                            servo_payload,
                                            sizeof(servo_payload));
```

`send()` 成功只代表 Router 接受并复制了 Payload。需要明确区分时调用 `ucn_service_send_ex()`：它返回的 Acceptance 是 `LOCAL_DELIVERED` 或 `REMOTE_ENQUEUED`；后者仍可能在 Bridge/Core/Link 阶段失败，不是端到端 ACK。对 Q0 命令，业务层必须保留本地失效保护，不能把“入队成功”当作执行器已经动作的确认。

Task 停机/重启前调用 `ucn_service_set_ready(..., false)`：Router 会清空对应 Q0 FIFO/Q1 Latest，重启后不会恢复执行旧命令。高风险 Q0 可按需在业务 Payload 前加 12 B `ucn_service_command_guard`，但这不是强制线格式；Validator 与消费者分别检查命令 ID、有效期、结果 Endpoint 和产品安全状态。跨节点时间戳只有在产品提供共享时间域时才有意义。

### 7.4 从业务 Task 接收

```cpp
ucn_service_message_t message;
while (g_service_port.inbox_take(UCN_SERVICE_ID_CONTROL, 0x40U, &message) == UCN_OK) {
    consume_imu(message.payload, message.payload_length);
}
```

Port 的 `event_take()` 只是一字节的“可能有消息”通知：收到通知后必须循环 `inbox_take()` 读空，因为 Q1 可能在通知之间覆盖成最新值。Q0 Inbox 是 FIFO；满时显式返回/计数。Q1 Inbox 是 Latest Value；覆盖旧值是预期行为。

### 7.5 可选 Q0 本机背压重试

默认 Bridge 仍是一次提交：Router Remote TX 出队后，如果 Core/Link 返回 `UCN_ERR_NO_SPACE`，该次提交立即失败。只有产品确认“这是固定 Adapter TX Queue 的瞬时背压”时，才启用一个固定 Pending Q0 槽：

```c
static void outbound_final(void *context,
                           const ucn_service_message_t *message,
                           ucn_result_t final_result)
{
    /* 回调返回后 message 指针不再归调用方持有；需要长期保存时只复制 ID/结果。 */
    product_record_local_submit(message->destination_node_id,
                                message->endpoint,
                                final_result);
}

static ucn_result_t service_enable_bounded_q0_retry(void)
{
    const ucn_service_bridge_q0_backpressure_policy_t policy = {
        .max_retries = 3U,
        .retry_interval_ms = UINT32_C(5),
        .timeout_ms = UINT32_C(30),
    };
    ucn_result_t rc;

    rc = ucn_service_protocol_bridge_set_outbound_observer(
        &g_service_bridge, outbound_final, NULL);
    if (rc != UCN_OK) {
        return rc;
    }
    return ucn_service_protocol_bridge_set_q0_backpressure_policy(
        &g_service_bridge, &policy);
}
```

Protocol Task 应传入同一个单调时钟：

```c
const uint32_t now_ms = product_monotonic_ms();
uint8_t processed = 0U;

(void)ucn_service_protocol_bridge_step_at(&g_service_bridge,
                                           now_ms,
                                           2U,
                                           &processed);
(void)ucn_node_step(&g_node, now_ms);
```

行为边界：

- 只重试 Q0 的 `UCN_ERR_NO_SPACE`；`LINK_DOWN`、无路由、安全拒绝、参数错误等立即终结；
- Pending 存在时不再从 Router 取下一条 Q0/Q1，因此原 Q0 不会被后来消息越序；等待重试到期的调用可以返回 `processed=0`；
- Observer 只在最终接受、耗尽、终止失败或过期时调用一次，中间 `NO_SPACE` 不回调；
- Observer 的 `UCN_OK` 只表示 `LINK_QUEUE_ACCEPTED`，不是 `REMOTE_INBOXED` 或 `REMOTE_EXECUTED`；关键命令仍用 Result Endpoint/Command ID 闭环；
- 传 `NULL` 给 `set_q0_backpressure_policy()` 可恢复默认一次提交；Pending 仍被占用时禁止改 Policy；
- Q1 不使用该机制，继续保持 Latest Value。

### 7.6 区分本机提交与远端执行结果

统一阶段依次为 `LOCAL_INBOXED`、`REMOTE_ROUTER_QUEUED`、`LINK_QUEUE_ACCEPTED`、`REMOTE_INBOXED`、`REMOTE_EXECUTED`。前两项由 `ucn_service_send_ex()` 返回的 Acceptance 经 `ucn_service_acceptance_stage()` 映射；第三项由 Bridge 本机最终 Observer 确认；后两项只能由业务 Result Endpoint 确认。

需要区分立即背压拒绝、重试耗尽、过期和终止失败时，使用结构化接口：

```c
static void outbound_event(
    void *context,
    const ucn_service_message_t *message,
    const ucn_service_bridge_outbound_event_t *event)
{
    (void)context;
    product_record_local_submit(message->destination_node_id,
                                message->endpoint,
                                event->stage,
                                event->outcome,
                                event->result);
}

(void)ucn_service_protocol_bridge_set_outbound_event_observer(
    &g_service_bridge, outbound_event, NULL);
```

关键命令继续使用可选 12 B Command Guard。目标要报告远端阶段时，在 Guard 指定的 Result Endpoint 返回可选 8 B `ucn_service_result_header_t`；源端用 `ucn_service_result_header_decode()` 和 `ucn_service_result_matches_command()` 核对 Command ID 与实际 Endpoint。`REMOTE_INBOXED` 只允许 `ACCEPTED`；`REMOTE_EXECUTED` 可为 `SUCCEEDED/REJECTED/FAILED/EXPIRED`，`detail_code` 由产品 ABI 冻结。

UCN 不为此建立通用 Pending/ACK 表。产品必须用固定容量命令等待表管理目标 Node、Session 和 Deadline：目标 Validator 在入 Inbox 前拒绝时不会自动回包，源端会看到 `LINK_QUEUE_ACCEPTED` 后业务超时；执行 Task 主动拒绝时则应返回 `REMOTE_EXECUTED + REJECTED`。完整契约见 [S20 异步阶段与业务结果关联](UCN_S20_异步阶段与业务结果关联.md)。

### 7.7 直接使用 Core 的背压重试

不经过 Service、直接调用 Core 队列的单 Task/裸机产品，也必须显式选择该语义并提供绝对 Deadline：

```c
const uint32_t now_ms = product_monotonic_ms();
const ucn_send_request_t request = {
    .destination = destination_node_id,
    .message_type = UCN_MSG_DATA_Q0,
    .traffic_class = UCN_TRAFFIC_Q0_CRITICAL,
    .delivery = UCN_DELIVERY_RETRY_ON_BACKPRESSURE,
    .deadline_ms = ucn_deadline_from_now(now_ms, UINT32_C(30)),
    .payload = payload,
    .payload_length = payload_length,
};

(void)ucn_node_enqueue(&g_node, &request);
```

Core Profile 默认最多重试 `UCN_Q0_BACKPRESSURE_MAX_RETRIES=3` 次、间隔 `UCN_Q0_BACKPRESSURE_RETRY_INTERVAL_MS=5 ms`。它保留原 Q0 FIFO 槽，等待期间只允许必要 Heartbeat/Probe/Route Refresh 维护取得机会；这仍只是本机队列准入重试，不重传已经被 Link 接受的帧。

## 8. 自动发现、入网、路由与多个 Bearer

### 自动入网的运行顺序

1. Adapter 发现物理对端，并建立 Candidate Link（地址到 Link 的映射在 Adapter 内）；
2. 双方在该 Link 上交换 HELLO；
3. Node 根据 `UCN_JOIN_OPEN`、人工准入或产品授权回调决定是否接纳；
4. 已准入的直连 Neighbor 定期 Heartbeat；未知远端由 Q1 首包按需 Route Request 寻路；
5. 中继只维护自己需要的邻居、Route/Path 等固定表，不保存永久全网拓扑。

产品若不希望开放准入，调用 `ucn_node_set_join_policy()` 配置授权回调，而不是靠“写死几个 MAC 地址”替代身份策略。Node Snapshot、策略诊断和 Path 控制也各自默认拒绝远端请求，必须安装对应 authorizer。

### 多介质对同一 Node 的处理

同一对端可同时有 UART（低 Cost）、ESP-NOW（备份）和 CAN-FD（另一 Bearer）。Core 将其合并为一个 Neighbor：

- Primary 明确 Down 时，下一帧使用健康 Backup；
- 正常质量切换默认需要候选至少优 20%、连续 3 个 500 ms 采样，并通过 2 次一跳 Heartbeat ACK；
- 单一 Bearer Down 不移除 Neighbor、不清逻辑 Route/Path；全部 Bearer Down 才回收动态状态；
- 真实板级的切换时延、丢失和乱序还需按测试地图验证，不能仅根据虚拟测试宣称“无缝”。

Adapter 的 Cost 必须来自真实可解释指标。`route_cost` 越低越优；未知指标使用保守默认值。Wi-Fi 可映射 RSSI 平滑值、丢包与本 Adapter 队列压力；UART/CAN 可映射错误率、重试/Bus-Off 与队列压力。不要虚构 RTT。

## 9. 指定路径、主备与 Q1 负载均衡

### 9.1 默认自动路径

什么也不配置时使用 `UCN_ROUTE_POLICY_AUTO_BEST`。调用仍是 `ucn_node_send_endpoint()` 或 Service `send()`；Core 根据已有 Route/候选路径和健康 Bearer 选择。不需要也不应该让业务 Task 每帧寻路。

### 9.2 真正启用指定路径前的前提

`local_path_id` 只是源端固定表的本地句柄；不能只写一个 Policy 就声称路径已上线。正确的安装顺序是：

1. 产品配置安全 Provider 与 `ucn_node_set_path_control_authorizer()`；默认拒绝是故意的；
2. 源 Node 为 P1/P2 调用 `ucn_node_install_local_path()`；
3. 源 Node 依次向每个中继和终端调用 `ucn_node_send_path_install()`，让它们形成逐跳转发表；中继不需要解密端到端业务；
4. 源 Node 用 `ucn_node_set_policy_path()` 把本地句柄、已认证 `wire_path_id`、目的 Node、首跳 Link 关联，并设 `verified=true`；
5. 用 `ucn_node_set_route_policy()` 为一个精确的 `(destination, endpoint, traffic_class)` 绑定策略；
6. 业务继续调用原来的 `ucn_node_send_endpoint()` / Service `send()`，无需把路径号传给每次业务调用。

远端 `PATH_INSTALL/PATH_REVOKE` 的接收顺序固定为 Security Provider、产品 Path Authorizer、认证管理源预算、最后才修改 Path 表。默认最多跟踪 4 个活动管理源，每个来源的 INSTALL/REVOKE 各允许突发 4 次并按 1 s/Token 恢复；来源身份是 `(source Node, source Session)`，不包含 Link，所以切换 Wi-Fi/UART Bearer 不能绕过。产品可用 `UCN_PATH_CONTROL_RX_SOURCE_DEPTH`、`UCN_PATH_CONTROL_RX_TOKEN_BURST`、`UCN_PATH_CONTROL_RX_TOKEN_REFILL_MS`、`UCN_PATH_CONTROL_RX_SOURCE_IDLE_MS` 按 MCU RAM 和管理频率调整。Session 更新必须先由产品 Security 接受；旧 Session 吊销仍是 Security Provider 的责任。

调试时读取 `ucn_node_get_stats()`：`path_*_authorization_rejected` 表示 Security/产品授权拒绝，`path_*_budget_rejected` 表示对应操作 Token 耗尽，`path_control_budget_source_full` 表示活动管理源槽已满，`path_install_table_full` 才表示真正的 Path 转发表已满。以上拒绝均发生在 Path 表写入前。

```c
ucn_policy_path_config_t p1 = {
    .local_path_id = 1U,
    .wire_path_id = 0x101U,       /* 已经在逐跳控制面安装 */
    .destination = UINT32_C(0x0000000D),
    .egress_link = &g_link_to_b,
    .verified = true,
};
ucn_route_policy_config_t policy = {
    .key = { UINT32_C(0x0000000D), 0x40U,
             (uint8_t)UCN_TRAFFIC_Q1_REALTIME },
    .mode = UCN_ROUTE_POLICY_PINNED_FAILOVER,
    .primary_local_path_id = 1U,
    .backup_local_path_id = 2U,
    .allow_discovery_on_hard_failure = false,
};

(void)ucn_node_set_policy_path(&g_node, &p1);
/* 同样安装 p2，然后再设置 policy。 */
(void)ucn_node_set_route_policy(&g_node, &policy);
```

### 9.3 四种策略的选择

| 模式 | 使用场景 | 行为 |
| --- | --- | --- |
| `AUTO_BEST` | 默认传感器/状态业务 | 使用自动 Route/质量策略。 |
| `PINNED_STRICT` | 必须固定在指定 P1 的数据 | 只走 Primary；P1 不可用即失败，不偷偷改线。 |
| `PINNED_FAILOVER` | 需要固定优先路径但允许硬故障后换 P2 | P1 正常时固定走 P1；只在硬 Down/Path 不存在时切 P2。 |
| `AUTO_BALANCE` | 多条已验证路径上的独立 Q1 实时流 | 同一个 `(目的 Node, Endpoint, Q1)` 在租约内固定一条 Path；不同 Endpoint 流可分散到 P1/P2。 |

`AUTO_BALANCE` 不是逐帧轮询、不是帧复制、不是带宽聚合，也不适用于 Q0。默认流租约 2 s；持续 3 个 500 ms 窗口队列压力超过 800‰或 Path 硬 Down 才重绑。这样 IMU 的同一流不会因为每帧跳线而制造乱序。

## 10. 安全：按 Endpoint 配置，不把 Provider 当作已完成

Core 支持三层控制：

- Node 默认 `ucn_node_set_security_policy()`；
- Endpoint 覆盖 `ucn_node_set_endpoint_security_policy()`；
- 安全 Provider `ucn_node_set_security()` 提供持久序号、会话 ID、TX/RX ACL、是否保护、`seal()` 与 `open()`。

开发构建默认允许先用明文联调。产品需要失败关闭时，应在全工程定义 `UCN_SECURITY_REQUIRED_BY_DEFAULT=1`，或初始化后立即调用 `ucn_node_set_security_required(&node, true)`；只有 `ucn_node_security_ready()` 返回真后才允许进入协议循环。Required 状态要求完整 Provider、非零 Session、持久 Sequence、TX/RX 授权、`seal/open`，并要求 Node 默认策略和所有 Endpoint 覆盖都禁止明文 TX/RX/Forward。配置未完成、策略退回明文或清除 Provider 后，`step/send/receive` 均返回 `UCN_ERR_SECURITY`。Nano 不含 Security Feature，不能作为强制生产安全 Profile。

策略含义：

| 项目 | 可选模式 |
| --- | --- |
| TX | `PLAIN`、`E2E_PROTECTED`、`AUTO` |
| RX | `PLAIN_ONLY`、`ENCRYPTED_ONLY`、`BOTH` |
| 转发 | 明文与不透明 E2E 都可转发、只转发不透明 E2E、仅终端消费 |

端到端保护帧可经过不持有业务密钥的中继；中继按照转发表转发密文，目标 Node 才调用 `open()`。这不等于“任意安全配置都已经生产可用”：产品仍必须选择审计过的 AEAD、保护计数器掉电回退、配置密钥轮换/吊销与管理 ACL。当前仓库不内置生产 AEAD 或密钥系统。

## 11. 诊断接口：只在需要时调用

这些是低频控制帧，不是业务心跳，也不维护永久全网拓扑。

| 需求 | API | 前提与结果 |
| --- | --- | --- |
| 查看本机到一个目的 Node 的实际经过节点 | `ucn_node_request_path_trace()` | 按需 Trace；回调收到受长度限制的路径结果。适合调试，不应周期高频调用。 |
| 收集当前可达 Node 快照 | `ucn_node_request_node_snapshot()` | 低频受限广播；目标默认拒绝，须配置 Snapshot authorizer；源端只保存固定数量结果。 |
| 查看某个 Node 的 Policy/Path/Flow/质量槽 | `ucn_node_request_policy_diagnostic()` | 单 Node 单播，目标须配置 Policy Diagnostic authorizer；可选 Summary 或固定槽，Reply 为固定 32 B。 |
| 查看本机运行状态 | `ucn_node_get_stats()`、`ucn_service_get_stats()`、`ucn_service_protocol_bridge_get_stats()`、`ucn_adapter_rx_get_stats()` | 直接读取本机固定计数；建议按秒输出，不在 ISR 中打印。 |

诊断回调也运行在 Protocol Task。只复制结果到管理/日志任务，避免在回调内阻塞串口、文件系统或网络。

## 12. ESP32 FreeRTOS 产品对接方式

当前 ESP32-S3/WROOM 测试工程已提供一个薄的产品 Port：

`E:\File\PlatformIO\ESP32_UCN\ESP32S3_N16R8_UCN_Test1\include\ucn_service_freertos_port.h`

它提供：

| 方法 | 用途 |
| --- | --- |
| `begin(router, bridge, consumers, count)` | 绑定已初始化的 Router/Bridge 和静态消费者表。 |
| `bind_consumer_task(service_id, task_handle)` | 让某个 Service 只能由其绑定的业务 Task 读取。 |
| `send(...)` | 业务 Task 的统一发送入口；自动区分本机 Fast Path 与远端。 |
| `inbox_take(service_id, endpoint, message)` | 从本 Service 的 Inbox 取 Q0/Q1 消息。 |
| `event_take(service_id, endpoint*, ticks)` | 等待“可能有消息”的通知；之后必须 drain Inbox。 |
| `snapshot_stats(...)` | 读取 Port 的固定统计。 |

建议的任务分工：

- `loopTask`/Protocol Task：Pump Adapter、Bridge、`ucn_node_step()`、HELLO 调度；
- Sensor/Control/Actuator Task：仅用 Port `send()` / `inbox_take()`；
- Wi-Fi/UART/CAN 回调：仅提交 Adapter RX 队列；
- 日志 Task：周期读取统计，不参与通信状态机。

这套结构允许 IMU Task 本机供给控制 Task，同时把同一 Endpoint 的最新值发给远端 Node；两个动作使用相同的业务调用，但不会让本机通信白白走无线。

## 13. 固定资源与编译期裁剪

UCN 的表和队列均是编译期数组。产品根据 MCU RAM 调整上限后，必须重新构建并执行完整回归；不能运行时无限扩容。常用默认值包括：

| 配置 | 默认值 | 作用 |
| --- | ---: | --- |
| `UCN_MAX_LINKS` | 4 | 一个 Node 可注册的 Link 数。 |
| `UCN_MAX_NEIGHBORS` | 8 | Neighbor 槽数。 |
| `UCN_MAX_BEARERS_PER_NEIGHBOR` | 2 | 同一 Neighbor 的介质数量。 |
| `UCN_DUPLICATE_SOURCE_WINDOWS` | Nano/Lite/Full = 4/16/32 | 同时维护的 Source/Session 去重窗口。 |
| `UCN_DUPLICATE_WINDOW_BITS` | Nano/Lite/Full = 32/32/64 bit | 每来源可容纳的乱序/重复 Sequence 范围。 |
| `UCN_RREQ_CACHE_SIZE` | Lite/Full = 8/16 | 独立 RREQ Request ID/Best Cost 状态。 |
| `UCN_MAX_HOPS` | 16 | 可覆盖为 1～254；超过 16 尚需单独长路径验收。 |
| `UCN_SECURITY_REQUIRED_BY_DEFAULT` | 0 | 产品设为 1 时从 Node 初始化起失败关闭；Nano 不允许。 |
| `UCN_MAX_ROUTE_POLICIES` / `UCN_MAX_POLICY_PATHS` / `UCN_MAX_POLICY_FLOWS` | 8 / 8 / 8 | 策略、路径和 Q1 Flow 固定表。 |
| `UCN_SERVICE_MAX_BINDINGS` | 6 | R1 Service Binding 数。 |
| Service Remote TX Q0/Q1 | 4 / 4 | 跨 Node 的 Router 待发送请求。 |
| Service Q0 Inbox | 4 | 每个 Q0 Endpoint 的 FIFO 深度。 |
| `UCN_SERVICE_MAX_PAYLOAD_BYTES` | 32 B | 当前 Service Router 单消息最大 Payload。 |

降低这些值适合 STM32 最小板等小 RAM Profile；提高前应先量化 `sizeof(ucn_node_t)`、静态 RAM、任务栈高水位和最坏队列压力。Q0 优先级、固定资源和“不能把失败藏进无限重试”不应改变。

## 14. 推荐的产品接入顺序

1. 冻结 Network ID、每块板的稳定 Node ID、首批 Endpoint ABI 和 RAM Profile；
2. 只接入一种 Link，完成 `open/send/status/metrics` 与 RX 入队；
3. 建立唯一 Protocol Task，跑通 HELLO、单跳 Q1、Q0 本地安全失败处理；
4. 加入 Service Router/Bridge/FreeRTOS Port，让业务 Task 不再直接碰 Node；
5. 加入第二 Bearer，验证同一 Neighbor 的 Primary/Backup；
6. 用三块板验证 A→B→C 自动寻路、离网和 RERR；
7. 只有确有产品需求时，受授权安装 Path 并启用 Strict/Failover/Auto Balance；
8. 最后接入生产 Security Provider、ACL、掉电计数器和密钥流程。

每一步先运行 Core 单元/虚拟拓扑测试，再做单板、两板、三板实测。不要用“能看到 Wi-Fi Peer”替代数据路径、业务接收、Q0 失败安全或性能验收。

## 15. 当前已实现与仍需产品完成的边界

| 项目 | 当前状态 |
| --- | --- |
| MCU 无 Linux 的 Node、邻居、自动路由、转发、QoS、Service Router/Bridge | 已实现；Core 有单元与虚拟拓扑验证。 |
| 两块 ESP32-S3 的 UART Primary + ESP-NOW Backup、R1 Q0/Q1 Service 基础双向通信 | 已有实板基础验证。 |
| 固定路径、主备、Q1 流亲和均衡、Path/Policy 诊断 | Core/虚拟拓扑已验证；真实多板 Path/负载/故障性能仍待实测。 |
| 路径追踪、节点快照 | Core/虚拟拓扑已验证；真实介质覆盖率与时延待实测。 |
| 生产密码、身份、密钥管理、AES/ChaCha AEAD 选型 | 未内置；必须由产品 Security Provider 落地和审计。 |
| CAN 小 MTU 分段重组、BLE/LoRa/真实 Linux Adapter | 取决于具体产品 Adapter，未因 Core 存在而自动获得。 |

## 16. 相关文档与源码入口

- [UCN v4 协议核心说明](UCN_v4_协议核心说明.md)：协议是什么、帧和架构边界。
- [整体架构设计](UCN_整体架构设计.md)：模块关系和演进方向。
- [路由策略与负载均衡执行建议](UCN_路由策略与负载均衡执行建议.md)：Path/策略/质量阈值的设计理由。
- [T25 首版 Endpoint 与 Service 契约](UCN_T25_首版Endpoint与Service契约.md)：R1 业务 ABI 和 Payload 规则。
- [T25 节点内任务通信详细执行方案](UCN_T25_节点内任务通信详细执行方案.md)：Router/Bridge/RTOS 边界。
- [UCN 调用关系树](calltree/README.md)：按真实函数调用、回调和固定队列关系追踪运行路径。
- [快速使用手册](快速使用手册/README.md)：裸机、通用 RTOS、FreeRTOS、Zephyr、NuttX、RT-Thread 的最小接入步骤与平台边界。
- `include/ucn/`：最终以公开 API 声明和编译期配置为准；若本文与源码不一致，以源码为准并同步修订本文。
