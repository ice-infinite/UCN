# UCN 使用与调用手册

> 适用版本：`codex/v5-adaptive-wire` 当前工作树的 UCN Core 5.0.0 / 线协议 v5，已包含 V5-35 静态标准 Preset Resolver。Nano/Lite/Full 都解析 W0～W3；默认固定 W3，产品可在注册 Link/安装 Security 前使用“最低够用 TX/W3 RX”，或显式开启路由感知自动选档。产品工程的 Adapter、密钥、板级引脚和业务 Endpoint 仍需自行实现。
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
| Wire Profile | 固定发送档、最大接收档、是否自动最小档 | 推荐最低够用 TX/W3 RX；`ucn_node_set_wire_profiles()` 必须在 Link/Security 前调用。极小 MTU/安全域可主动收窄 RX，自动模式需显式开启。 |
| 全局编译配置 | Profile、MTU、表深、队列、超时和预算 | 默认集中在 `ucn_config.h`；产品用 `UCN_USER_CONFIG_HEADER` 覆盖，所有相关 Translation Unit 必须一致。 |
| Endpoint ABI | 数据代表什么、长度/字节序/单位/QoS/消费者 | 静态冻结；语义变化新分配 Endpoint，不复用旧 ID。 |
| Service Binding | 哪个本机任务拥有哪个 Endpoint | R1 是一 Endpoint 一消费者，固定表、无动态注册。 |
| Link/Bearer | UART、ESP-NOW、CAN 等如何发收、MTU、对端身份 | 一个 Neighbor 可有多个 Bearer；业务不直接选择物理介质。 |
| 安全策略 | 哪些 Endpoint 可明文/必须端到端保护/允许何种转发 | Core 只提供 Provider 边界，密钥、身份和 AEAD 必须由产品实现。 |
| 路由策略 | 自动、固定、主备或 Q1 均衡 | 默认 `AUTO_BEST`；固定路径和均衡仅在已安装、已验证 Path 后启用。 |
| Protocol Task 时限 | 最大 Step 间隔、最大 Block、每轮 Pump/Bridge 预算、Link `send()` WCET | `UCN_MAX_STEP_INTERVAL_MS` 默认 10 ms；产品必须用自己的节点/Bearer 上限通过维护上界门禁。 |

## 3. 模块与“该由谁调用”速查

| 模块/头文件 | 解决什么 | 调用者 | 绝不能做什么 |
| --- | --- | --- | --- |
| `ucn_config.h` | 全部公开编译期默认值与产品覆盖入口 | 构建系统、产品配置头 | 写入 Node ID、密钥、引脚等逐设备运行配置。 |
| `ucn.h`、`ucn_types.h` | 版本、运行配置、错误码、帧和 QoS 基础类型 | 启动初始化、所有层 | 直接伪造线帧。 |
| `ucn_frame.h` | 帧编码、解码、CRC 与 E2E AAD 辅助 | Core 单测、Adapter/安全 Provider 的低层实现 | 业务 Task 手工编码业务帧。 |
| `ucn_endpoint.h` | 静态 Endpoint/控制消息编号合法性判断 | 产品 ABI 定义、配置检查 | 把动态 Endpoint 当作当前 R1 产品 ABI。 |
| `ucn_node.h` | 邻居、路由、Endpoint 发送/接收、控制面、调度 | **唯一 Protocol Task** | 被多个业务 Task 或 ISR 直接调用。 |
| `ucn_neighbor.h` | Neighbor/Bearer 状态和准入授权回调类型 | 产品 Join Provider、诊断读取 | 绕过 HELLO/准入直接把陌生节点当作可信 Neighbor。 |
| `ucn_link.h` | 把一种底层介质封装为 `open/send/poll_rx/status/metrics` | Adapter/驱动层 | 让业务层看到 MAC、串口号或 CAN ID。 |
| `ucn_port.h` | 时间、随机数、持久计数器、任务临界区和 ISR token 临界区的 Port 操作表 | Adapter RX Queue 和产品 Port | 把平台 API 泄漏到 C99 Core，或用任务锁替代 ISR token 锁。 |
| `ucn_adapter.h` | 驱动回调到 Protocol Task 的固定 RX 队列 | 任务使用 `ucn_adapter_rx_enqueue()`；完整帧 ISR 仅用 `ucn_adapter_rx_enqueue_from_isr()` | 在中断/驱动回调内执行路由、解密或业务回调；缺 ISR token 锁仍强行入队。 |
| `ucn_standard_adapter.h` | SDK 无关的 Bearer/Preset、静态产品 Link 配置和 Cost/MTU/RTT Resolver | Adapter 初始化前统一取得默认事实、校验产品覆盖 | 以为它会初始化 UART/CAN/Wi-Fi/USB、注册 `ucn_link_t` 或执行动态选路。 |
| `ucn/ports/ucn_event_runtime.h` | 固定多 Source 注册、Task/ISR 事件合并、有界 Drain、公共 Owner 运行与超时兜底 | 新多 Bearer 产品的唯一 Protocol Task/裸机主循环；ISR 只调用 signal | 在 Source/ISR 中执行业务、把通知次数当数据队列、以为它自带 Carrier/驱动。 |
| `ucn/ports/ucn_protocol_owner.h` + `ucn/ports/ucn_port_<platform>.h` | 公共唯一 Owner 与独立平台 RX 通知、等待、固定预算、统一时钟 | 只包含当前产品所选的裸机/FreeRTOS/Zephyr/NuttX/RT-Thread Port | 以为包含一个头就会创建真实 Task/Queue/Semaphore 或实现介质驱动。 |
| `ucn_service.h` | Node 内任务通信：本机直投、远端请求、Q0/Q1 Inbox | Service/Port，短临界区保护 | 直接访问 Link 或 `ucn_node_t`。 |
| `ucn_service_bridge.h` | Router 和 Node 的唯一桥接 | **唯一 Protocol Task** | 创建 RTOS Task、动态队列或重试伪 ACK。 |
| `ucn_cluster.h` | 可选单层簇选举、成员租约和只读 Cluster View | **唯一 Protocol Task** | 由业务/ISR 修改 Head、Member 或 Term。 |
| `ucn_cluster_federation.h` | 可选 C06 Locator 发布/撤销、固定 Directory、有限 Query Cache、Cluster→Head 锚点和显式单帧 Tunnel | **唯一 Protocol Task** + 产品 Endpoint `0xA1` 绑定 | 以为普通 `ucn_node_send*()` 会自动跨簇，或以为它已支持跨簇 Transfer/自动 Gateway。 |
| `ucn_policy.h`、`ucn_path.h` | 指定路径、主备、Q1 流亲和均衡 | 产品的受控配置/管理层 + Protocol Task | 对 Q0 使用逐帧均衡，或未授权安装路径。 |
| `ucn_security.h` | 端到端保护/ACL/计数器 Provider 契约 | 产品安全模块 + Protocol Task 初始化 | 把 CRC 当认证，或认为 Core 内置了生产 AEAD。 |

### 3.1 公开 API 的调用层级

以下按“谁可以调用”列出当前公开函数。`ucn_path.h` 和 `ucn_policy.h` 的低层 `ucn_path_*` / `ucn_policy_*` 函数由 Node 内部维护；产品代码优先调用 `ucn_node_*` 包装接口，避免破坏 Node 内部状态一致性。

| 场景 | 推荐 API | 调用时机 |
| --- | --- | --- |
| 启动检查 | `ucn_version()`、`ucn_validate_config()`、`ucn_node_init()` | 只在启动/重建 Node 时。 |
| 安全和准入配置 | `ucn_node_set_security()`、`ucn_node_set_security_policy()`、`ucn_node_set_endpoint_security_policy()`、`ucn_node_set_join_policy()` | 注册 Link 前完成；产品需要时安装 Snapshot/Policy/Path authorizer。 |
| Link 生命周期 | `ucn_node_register_link()`（内部调用可选 `ops->open()`）、Adapter 的 peer 绑定/释放 | Adapter 创建/移除物理承载时；不由业务 Task 调用。 |
| 自动入网 | `ucn_node_observe_neighbor()`、`ucn_node_probe_neighbor()`、`ucn_node_broadcast_hello()` | 只由发现 Adapter/Protocol Task 在受控节拍调用。 |
| 静态/手动路由 | `ucn_node_add_route()`、`ucn_node_discover_route()`、`ucn_node_refresh_route()`、`ucn_node_route_pending()` | 产品启动静态路由，或管理/测试层；业务优先发送 Endpoint。 |
| 收/发 Endpoint | `ucn_node_set_endpoint_handler()`、`ucn_node_send_endpoint()` | 单 Task 简化产品或 Bridge；多 Task 业务应改用 Service API。 |
| 兼容原始消息 | `ucn_node_set_rx_handler()`、`ucn_node_send()`、`ucn_node_enqueue()` | 仅已有原始 message-type 应用或测试；新业务优先 Endpoint API。 |
| 手动 Path/策略 | `ucn_node_install_local_path()`、`ucn_node_send_path_install()`、`ucn_node_set_policy_path()`、`ucn_node_set_route_policy()` | 受授权管理配置；不能放在高频业务循环。 |
| 只读策略/质量 | `ucn_node_find_route_policy()`、`ucn_node_find_policy_path()`、`ucn_node_find_q1_flow()`、`ucn_node_get_link_quality()`、`ucn_node_get_policy_stats()` | 管理/日志任务经安全同步快照读取。 |
| 诊断控制面 | `ucn_node_request_path_trace()`、`ucn_node_request_node_snapshot()`、`ucn_node_request_policy_diagnostic()` | 低频、显式用户/管理触发。 |
| Protocol Task 主循环 | 选择一个独立 Port 的 `ucn_<platform>_port_rx_enqueue()`、`ucn_<platform>_port_*_step()`；底层为公共 Owner、`ucn_adapter_rx_pump()`、可选 Bridge、`ucn_node_step()` | Owner 在同一个 `now_ms` 下按预算执行；不包含其它 RTOS Port。 |
| 任务通信 | `ucn_service_send()`、`ucn_service_inbox_take()`；ESP32 用 `UcnServiceFreeRtosPort::send()` / `inbox_take()` | 业务 Task 可调用；Port 负责短临界区，不把 Node 暴露给 Task。 |
| C06.2 目录解析 | `ucn_cluster_federation_init()`、Endpoint `0xA1`→`receive()`、Owner 中 `step()`、Head 的 `query_locator()` 与 `find_locator()` | 仅当产品显式链接 Federation 且由同一 Owner 串行调用；`query_locator()==UCN_OK` 后仍需轮询/下一轮读取结果。 |

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
    /* 裸机产品可把掉电保留计数与随机启动盐组合成非零 Session。 */
    const ucn_session_id_t boot_session = product_next_boot_session_id();
    const ucn_config_t config = {
        .network_id = UINT32_C(0x00004E01), /* W1 可表达。 */
        .node_id = UINT32_C(0x0000000A),
        .default_hop_limit = 8U,
    };

    /* 任一步返回非 UCN_OK，停止进入业务运行态并记录错误。 */
    if (ucn_node_init(&g_node, &config) != UCN_OK ||
        ucn_node_set_wire_profiles(&g_node,
            UCN_WIRE_PROFILE_W1_EDGE,
            UCN_WIRE_PROFILE_W3_BACKBONE) != UCN_OK ||
        ucn_node_set_wire_profile_auto(&g_node, true) != UCN_OK ||
        ucn_node_set_plain_session_id(&g_node, boot_session) != UCN_OK) {
        product_enter_safe_mode();
    }
}
```

上例是未启用 Security 的明文启动会话：每次启动必须提供新的非零
`boot_session`，不能把常量或 Node ID 当成 Session。生产环境若要求认证、
防重放或加密，应安装产品 Security Provider，由认证会话替代明文会话；
安全失败时不能自动降级为明文重发。

固定域模式只省去自动决策，适合资源/地址范围完全冻结的产品；自动模式不是协商任意格式，而是从四个官方档位中选满足本帧与当前出口的最小档。控制帧继续使用固定发送档，HELLO 会学习 `link->peer_wire_profile`；静态链路也可在注册后用 `ucn_node_set_link_wire_profile_limit()` 配置可信上限。中继不重新选档，防止在途帧被静默改写。

只有静态分配并独占 Node 的 Protocol Task 所在 `.c/.cpp` 文件需要包含
`ucn_node_storage.h`。业务任务、Adapter 声明和只传递 `ucn_node_t *` 的模块应
只包含 `ucn_node.h`；存储头中的字段是实现布局，不是应用 ABI，禁止直接读写。

### 5.1.1 可选：先解析标准 Preset，再初始化产品 Adapter

若产品采用官方初始 Cost，不要在多处手写 UART/Wi-Fi/CAN/USB 数字；在 BSP/HAL 初始化之前调用 Resolver：

```c
#include <string.h>
#include "ucn/ucn_standard_adapter.h"

ucn_standard_link_config_t link_config;
ucn_standard_resolved_link_config_t link_defaults;

(void)memset(&link_config, 0, sizeof(link_config));
link_config.local_link_id = 0x70U;
link_config.peer_node_id = UINT32_C(0x0000000B);
link_config.preset = UCN_STANDARD_PRESET_UART_115200_8N1;
link_config.required_logical_mtu = 128U;

if (ucn_standard_link_config_resolve(&link_config, &link_defaults) != UCN_OK) {
    product_enter_safe_state();
}
```

随后由**产品自己的 Adapter**把 `link_defaults.logical_mtu`、`base_cost` 和其他事实绑定到 `ucn_link_t`、`get_status()`/`get_metrics()` 与 BSP/HAL。Resolver 不调用 `open()`、不持有 Node 或驱动对象，也没有 GPIO/串口号/CAN Filter 参数。`required_logical_mtu=0` 会使用当前 `UCN_MAX_FRAME_BYTES`，不能超过 Preset 上限；CAN-FD 应显式写 `<=64`，ESP-NOW 默认 Preset 应显式写 `<=250`。经典 CAN 必须同时启用已实现的 Carrier（`carrier_enabled=true`），否则失败关闭。

`node_id` 不是入网后临时分配的地址；它是设备稳定身份。可让首次启动写入 Flash，或在每块板的产品配置中固定。更换同一板的物理链路不应改变 Node ID。

### 5.2 为每种物理承载实现一个 Link

`ucn_link_t` 对 Core 的最小可见信息是：`link_id`、`mtu`、直连 `peer_node_id`、`context` 和 `ops`。Adapter 必须实现：

- `open()`：准备介质；
- `send()`：同步接收一帧由 Core 编好的数据，立即发送或有界提交给本 Adapter 的 TX 队列；
- `poll_rx()`：仅用于轮询型驱动；回调型驱动可以返回 `UCN_OK`；
- `get_status()`：报告 `is_up`、MTU、收发错误；
- `get_metrics()`：报告平滑后的通用 Cost，及可选 RTT、发送失败率、队列压力；
- `close()`：释放本 Adapter 的硬件状态。

现代 MCU/RTOS 的正常 RX 路径应使用中断、DMA completion 或驱动事件通知，不应让
Protocol Task 高频空转查询。`poll_rx()` 保留给没有事件设施的裸机、特殊外设和保底
恢复；即使使用事件，Owner 仍需按协议下一个定时器设置有限等待，不能无限阻塞。

MTU 有统一的动态语义：`.mtu != 0` 是静态硬上限；`get_status().mtu != 0` 是当前运行时上限；两者都存在取较小值。`.mtu = 0` 允许注册，表示只依赖运行时 MTU；若两者都为 0，发送暂时返回 `UCN_ERR_LINK_DOWN`，运行时 MTU 恢复后无需重新注册。这里的 MTU 必须是 Adapter 分段/重组后的 UCN 逻辑帧上限。

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
    /* register_link() 内部会调用可选 open()，产品不要提前重复打开。 */
    if (ucn_node_register_link(&g_node, &g_uart_link) != UCN_OK) {
        product_enter_safe_mode();
    }
}
```

若同一个对端同时有 UART 与 ESP-NOW，注册两条 Link 即可。它们属于同一个 `peer_node_id` 的两个 Bearer；Core 会保留一个逻辑 Neighbor，并按状态/Cost/质量管理主备。不要用“两个 Node ID”模拟同一设备的两个接口。

### 5.3 RX 回调只入队，选择一个独立 Platform Port 再 Pump

V5-46 起，产品不能再包含集中式 Port 头。先按运行环境选择一个文件：裸机使用 `ucn/ports/ucn_port_bare_metal.h`，FreeRTOS 使用 `ucn/ports/ucn_port_freertos.h`，Zephyr/NuttX/RT-Thread 分别使用自己的独立头。CMake 产品只链接对应的 `ucn_port_<platform>` 目标；所有 Port 在内部调用同一个 SDK 无关公共 Owner。Owner 不创建平台对象，产品仍持有静态 Node、RX Queue、`ucn_port_ops_t` 和可选 Bridge。

V5-48 将 `from_isr` 继续传入 Queue：产品若允许 ISR 直接提交完整帧，必须为 `ucn_port_ops_t` 同时提供任务临界区和可返回/恢复 mask 的 ISR token 临界区。ISR 回调缺这一对时会得到 `UCN_ERR_CONFIG`；推荐路径仍是 ISR 写 BSP ring，并用 ISR-safe Notification 唤醒 Protocol Task，由 Task 普通入队。无论哪条路径，通知成功后 Owner 都应立即运行，不等待 Heartbeat。

V5-62 已在预发布阶段将 Port 破坏性升级为 API V2。每个 `ucn_port_ops_t` 都必须使用 C99 具名初始化，并首先填写 `.struct_size=(uint16_t)sizeof(ucn_port_ops_t)` 与 `.api_version=UCN_PORT_OPS_API_VERSION`；Owner、Runtime 和 Source 会在读取回调前失败关闭。旧六/八字段位置初始化和旧编译对象不再兼容，升级后必须清空构建目录并全量重编译 Core、Port、Adapter 和产品固件。

```c
static const ucn_port_ops_t g_port_ops = {
    .struct_size = (uint16_t)sizeof(ucn_port_ops_t),
    .api_version = UCN_PORT_OPS_API_VERSION,
    .now_ms = product_now_ms,
    .enter_critical = product_enter_task,
    .exit_critical = product_exit_task,
    .enter_critical_from_isr = product_enter_isr,
    .exit_critical_from_isr = product_exit_isr,
};
```

#### 5.3.1 新产品：统一多 Source Event Runtime

V5-58 起，新产品优先直接包含 `ucn/ports/ucn_event_runtime.h`。一个 UART、一个 CAN 控制器、一个 USB Endpoint 或一个 Wi-Fi Adapter 各占一个固定 Source ID；Source 之间不共享 Driver Ring。RTOS 只实现下面三个 Scheduler Hook，Carrier/Bearer 不复制 RTOS 状态机：

```c
enum {
    PRODUCT_SOURCE_UART0 = 0,
    PRODUCT_SOURCE_CAN0 = 1,
    PRODUCT_SOURCE_USB0 = 2,
    PRODUCT_SOURCE_WIFI0 = 3
};

static ucn_event_runtime_t g_runtime;

static const ucn_event_runtime_scheduler_ops_t g_scheduler_ops = {
    product_notify_owner, /* 内部按 from_isr 选普通/FromISR API */
    product_wait_owner,   /* true=收到通知，false=超时 */
    product_yield_owner   /* Drain Round 达上限时让出 CPU，可为 NULL */
};

/* 仅用于尚无公共模板的自定义 Packet/Frame Source；UART/RS-485/
 * USB CDC 直接使用下一节的 ucn_stream_source_t。 */
static ucn_result_t custom_source_service(
    void *context, ucn_event_source_events_t events, size_t max_work,
    ucn_event_source_service_result_t *out)
{
    size_t completed = 0U;
    (void)context;
    (void)events;

    /* product_source_drain() 只在 Owner Task 运行；它从本 Source
     * 固定帧队列取出最多 max_work 个完整帧，并逐帧调用
     * ucn_event_runtime_submit_frame(&g_runtime, &g_uart_link, data, len)。
     * Queue 满时必须保留未提交帧，下一 Round 再试。 */
    ucn_result_t result = product_source_drain(max_work, &completed);
    out->work_done = completed;
    out->pending_events = product_source_has_data() ?
        UCN_EVENT_SOURCE_RX_READY : 0U;
    return result;
}
```

初始化顺序为 Node/Link/RX Queue/可选 Bridge → `ucn_event_runtime_init()` → 对每个实例调用 `ucn_event_runtime_bind_source()` → 启动调度器和外设中断。`ucn_event_runtime_config_t.owner` 仍显式给出 RX/Bridge 每步预算；Runtime 的 Source/Round 默认预算可由产品配置头覆盖。

```c
/* UART ISR：只搬入 UART0 自己的固定 Ring，然后置位并通知。 */
void uart0_isr(void)
{
    product_uart0_ring_write_from_isr();
    (void)ucn_event_runtime_signal_source_from_isr(
        &g_runtime, PRODUCT_SOURCE_UART0, UCN_EVENT_SOURCE_RX_READY);
}

/* 唯一 Owner Task；有数据立即醒，10 ms 只是协议维护/漏通知上限。 */
void protocol_task(void *argument)
{
    ucn_event_runtime_run_result_t run;
    (void)argument;
    for (;;) {
        (void)ucn_event_runtime_task_cycle(
            &g_runtime, UCN_MAX_STEP_INTERVAL_MS, &run);
    }
}
```

多个 ISR 通知允许合并，因为通知只表示“某 Source 可能有工作”，真实字节/帧仍在 Ring。`service()` 返回 `pending_events` 时 Runtime 在固定 Round 预算内继续；达到上限返回 `work_remaining=true` 并调用可选 Yield，下一次 Task Cycle 不会先睡眠。等待超时才对所有已绑定 Source 做一次 `FALLBACK_SCAN`。裸机不提供 Scheduler Hook，ISR 置位后由主循环调用 `ucn_event_runtime_run()`；无中断裸机定期传 `fallback_scan=true`。

`ucn_event_runtime_submit_frame_from_isr()` 只适合驱动回调已经持有小型完整帧、且产品确认 ISR 拷贝上界可接受的 Packet Bearer；UART/CAN/USB 的首选路径仍是 ISR/DMA → 各自 Ring → Owner Source。Source 不得从 ISR 调用，不能在其中打印、解密、寻路、Transfer 重组或执行业务回调。

#### 5.3.2 UART/RS-485/USB CDC：公共 Stream Source

V5-59 起，字节流不再要求产品自己复制一份 COBS 状态机。每个端口静态创建一套 Source 与存储：

```c
#include "ucn/adapters/ucn_stream_source.h"

static ucn_stream_source_t g_uart_source;
static ucn_stream_source_default_storage_t g_uart_storage;

static ucn_result_t init_uart_source(void)
{
    const ucn_stream_source_config_t config = {
        .runtime = &g_runtime,
        .source_id = PRODUCT_SOURCE_UART0,
        .ingress_link = &g_uart_link,
        .ring_storage = g_uart_storage.ring,
        .ring_capacity = sizeof(g_uart_storage.ring),
        .frame_storage = g_uart_storage.frame,
        .frame_capacity = sizeof(g_uart_storage.frame),
        /* 其余 0 值选择 UCN_MAX_FRAME_BYTES 和全局默认预算。 */
    };

    return ucn_stream_source_init(&g_uart_source, &config);
}

void uart_rx_task_callback(const uint8_t *bytes, size_t length)
{
    (void)ucn_stream_source_write(&g_uart_source, bytes, length);
}

void uart_rx_isr(const uint8_t *bytes, size_t length)
{
    (void)ucn_stream_source_write_from_isr(
        &g_uart_source, bytes, length);
}
```

写入是“整块全收或全拒绝”；驱动要检查 `UCN_ERR_NO_SPACE` 并记过载。Source 会保留缺口前的完整 Carrier，从真实缺口开始丢弃到下一个 `0x00`，避免拼接错误。Owner 被立即唤醒并自动 COBS 解码、提交公共 RX Queue；Queue 背压时保留一个已解码帧，下一 Drain Round 重试。

发送侧的 `ucn_link_t::send()` 调用 `ucn_stream_carrier_encode()`，然后把返回的完整 Carrier 有界复制到产品 TX Queue。该函数不启动 DMA，也不等待串口发送完成：

```c
uint8_t carrier[UCN_STREAM_CARRIER_MAX_WIRE_BYTES(UCN_MAX_FRAME_BYTES)];
size_t carrier_length = 0U;

result = ucn_stream_carrier_encode(
    frame, frame_length, carrier, sizeof(carrier), &carrier_length);
if (result == UCN_OK) {
    result = product_uart_tx_enqueue(carrier, carrier_length);
}
```

默认每端口调用者存储为 512 B Ring + 259 B Frame Storage（默认 256 B UCN Frame 上限）；Host x64 `ucn_stream_source_t` 为 240 B。产品可在全局头覆盖 Ring、字节预算、错误预算和读取 Chunk，也可直接传入自定义静态数组。以上数值不是目标 MCU ABI 结果。

#### 5.3.3 CAN/CAN-FD：公共 Frame Source

V5-60 把帧流与 Stream 分开。产品先配置控制器、引脚、位时序和硬件 Acceptance Filter，再为每个控制器静态创建一个 Source：

```c
#include "ucn/adapters/ucn_can_source.h"

static ucn_can_source_t g_can_source;
static ucn_can_source_default_storage_t g_can_storage;

static ucn_result_t resolve_can_id(void *context,
                                   uint32_t identifier,
                                   bool extended,
                                   ucn_link_t **link)
{
    (void)context;
    if (!extended && identifier == 0x201U) {
        *link = &g_can_peer_link;
        return UCN_OK;
    }
    return UCN_ERR_NOT_FOUND;
}

static ucn_result_t init_can_source(void)
{
    const ucn_can_source_config_t config = {
        .runtime = &g_runtime,
        .source_id = PRODUCT_SOURCE_CAN0,
        .mode = UCN_CAN_SOURCE_MIXED,
        .ring_storage = g_can_storage.ring,
        .ring_capacity = UCN_CAN_SOURCE_DEFAULT_RING_FRAMES,
        .reassembly_slots = g_can_storage.slots,
        .reassembly_slot_count = UCN_CAN_SOURCE_DEFAULT_REASSEMBLY_SLOTS,
        .reassembly_storage = &g_can_storage.reassembly[0][0],
        .reassembly_storage_capacity = sizeof(g_can_storage.reassembly),
        .resolve_ingress = resolve_can_id,
    };
    return ucn_can_source_init(&g_can_source, &config);
}
```

驱动把 SDK 帧归一成 `ucn_can_frame_t`，其中 `length` 是 DLC 解码后的真实字节数。Task/ISR 分别调用 `ucn_can_source_write()` / `ucn_can_source_write_from_isr()`；不要在 ISR 解码、重组或调用 Node。CAN-FD TX 调用 `ucn_can_fd_carrier_encode()`，经典 CAN TX 先取 `ucn_can_classic_carrier_segment_count()`，再逐个调用 `ucn_can_classic_carrier_encode_segment()` 并写入产品固定 TX Queue。

CAN-FD Carrier 不修改 v5 Wire：只把完整 UCN 帧补到合法 DLC，尾部固定为零；Source 从 Header 取回真实长度并拒绝非零 Padding。经典 CAN 每段最多 8 B，START/CONT 使用严格递增 Segment Index；乱序、重复、丢段超时和槽满都不会拼接成业务帧。`Link.send()` 仍只负责有界入 TX Queue，不能等待全部经典 CAN 段真正发完。

经典 CAN 的产品 TX Queue 应保存完整 UCN 帧或等价的固定 Carrier 状态，由单一 Worker 对同一个 CAN ID 串行发完所有段。两个 Carrier 不得交错；Transfer ID 由该 Worker 递增分配，用来拒绝迟到段，不代表同一 CAN ID 可以并发重组。Preset Resolver 只有在产品确实安装了这条 TX/RX Carrier 后才设置 `carrier_enabled=true`。

控制器中断发现 Bus-Off 后调用 `ucn_can_source_set_bus_state_from_isr(..., UCN_CAN_BUS_OFF)`，同时让该 Link 的 `get_status().is_up=false`。驱动完成硬件恢复期间报告 `RECOVERING`，只有控制器和收发器确认可用后才报告 `ACTIVE`。Source 不自动清 TEC/REC 或重启硬件。可用 `ucn_can_source_get_health()` 把 Ring 千分比压力和失败累计映射进产品 `get_metrics()`；真实总线利用率、仲裁等待、TEC/REC 仍必须来自控制器驱动。

默认每控制器存储为 8 个规范化物理帧、2 个重组描述符和 `2 × UCN_MAX_FRAME_BYTES` 重组区。产品可在全局头覆盖默认，或为每个实例传自定义静态数组。经典 CAN 最大可表达 Carrier 为 1278 B，但这不是推荐 MTU：帧越大，占用的仲裁帧和最坏延迟越高。

#### 5.3.4 兼容入口：现有独立 Platform Port

```c
/* Task 上下文的驱动 Event 回调：入队成功后 Port 会通知 Owner。 */
void uart_rx_event_callback(const uint8_t *data, size_t length)
{
    (void)ucn_freertos_port_rx_enqueue(&g_ucn_freertos_port,
                                        &g_uart_link, data, length, false);
}

/* 真正 ISR 若直接提交完整帧，必须使用 ISR token 临界区和 FromISR 通知。 */
void uart_rx_isr_complete(const uint8_t *data, size_t length)
{
    (void)ucn_freertos_port_rx_enqueue(&g_ucn_freertos_port,
                                        &g_uart_link, data, length, true);
}

/* 唯一 Protocol Task：事件到达立即醒；10 ms 只是维护/漏通知上限。 */
void protocol_task(void *argument)
{
    (void)argument;
    for (;;) {
        size_t pumped = 0U;
        uint8_t bridged = 0U;

        (void)ucn_freertos_port_task_step(&g_ucn_freertos_port,
                                           &pumped, &bridged);
        if (pumped == 0U && bridged == 0U) {
            (void)ucn_freertos_port_task_wait(&g_ucn_freertos_port,
                                               UCN_MAX_STEP_INTERVAL_MS);
        }
    }
}
```

启动时先用 `ucn_protocol_owner_config_t` 绑定 `node`、`rx_queue`、`port_ops`、`port_context`、每轮 RX 预算和可选 Bridge/Bridge 预算；再用选择的平台 Config 绑定其专属 Runtime Hook 并初始化 Port。裸机没有 wait API；RTOS Port 的 `wait_for_work` 应映射为 Task Notification/Event/Semaphore，RX/TX completion 通过 `notify_protocol_task` 立即解除等待。等待 API 会裁剪到 `UCN_MAX_STEP_INTERVAL_MS`，该超时只承担协议定时器、无中断平台和漏通知兜底。`ucn_node_step()` 的空闲 `UCN_ERR_NOT_FOUND` 已在公共 Owner 内归一为 `UCN_OK`，原始值仍可从 Owner 统计读取。

该循环不是“尽量快即可”，而是产品时限契约。默认 Full Profile：

```text
maintenance_bound = (4 + 1) × 10 ms × 8 Neighbor × 2 Bearer = 800 ms
1 s Heartbeat + 800 ms < 3 s Suspect
```

对低时延有线 Link 可在注册前选择 FAST 存活档：

```c
ucn_link_t uart_link = {0};

/* 先填写 ops/context/link_id/mtu 等 Adapter 字段。 */
uart_link.liveness_profile = (uint8_t)UCN_LINK_LIVENESS_FAST;
result = ucn_node_register_link(&node, &uart_link);
```

FAST 使用 `250 ms Heartbeat / 1250 ms SUSPECT / 2000 ms DOWN`；零初始化 Link 保持默认 `1000/3000/4000 ms`。周期 Heartbeat 不消耗通用 RREQ/Probe/Activate Token，但仍受固定 Link/Bearer 数、维护调度上界和入站每 Peer 预算约束。FAST 只缩短静默判断，不提供端到端 ACK、重传或零丢帧保证。

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

Q1 首次发送若尚无满足 Hop/Cost/RTT 约束的路线，会在固定 Pending 槽保存该 `(destination, Endpoint)` 的最新值并启动有界发现。Deadline 是绝对值：Protocol Task 内部重试不会重新排队或延长期限；只有应用再次提交同一键的新值时才覆盖 Payload 并刷新 Deadline。发送成功、到期或永久错误后才清槽。Q0 不进入该等待机制。

常见返回处理：

| 返回 | 调用方应做什么 |
| --- | --- |
| `UCN_OK` | 表示本机 Core/Link 已接受本次发送，不表示对端业务一定已经执行。 |
| `UCN_ERR_LINK_DOWN` | 当前可用路径不可发送；Q0 进入本地安全策略，Q1 等下一周期新值。 |
| `UCN_ERR_NOT_FOUND` | 没有当前 Route/Path；Q1 可由 Core 开始受限寻路，Q0 不等待。 |
| `UCN_ERR_NO_SPACE` | 固定表或队列满；默认是最终失败。只有产品显式选择 S15 的 Q0 有界背压策略时，才按固定次数、间隔和 Deadline 做本机提交重试；仍禁止无限重试。 |
| `UCN_ERR_TOO_LARGE` / `UCN_ERR_ARGUMENT` | ABI 或配置错误，修正调用方。 |
| `UCN_ERR_SECURITY` / `UCN_ERR_ACCESS` / `UCN_ERR_REPLAY` | 安全策略或认证失败；记录审计事件，不能退回明文重发。 |

### 6.1 按需选择 32 B～8 KiB 逻辑消息

普通 `ucn_node_send_endpoint()` 仍只发送一个 UCN 帧。需要有界大消息时，产品额外链接 `ucn_transfer` 并包含 `ucn/ucn_transfer.h`。九档上限固定为：

```text
T32 / T64 / T128 / T256 / T512 / T1K / T2K / T4K / T8K
```

T32/T64 永不分片；T128～T8K 由 Transfer 根据实际 MTU 使用一个或多个 Fragment。选择的 Class 是“本条消息允许的最大长度”，实际 `length` 可以更小，但不能超过 Class 或本产品的 `UCN_TRANSFER_MAX_MESSAGE_BYTES`。

```c
static ucn_transfer_t g_transfer;

static uint32_t transfer_now_ms(void *ctx)
{
    (void)ctx;
    return product_monotonic_ms();
}

static void transfer_rx(void *ctx,
                        ucn_node_id_t source,
                        ucn_session_id_t session,
                        ucn_endpoint_t endpoint,
                        ucn_transfer_class_t transfer_class,
                        const uint8_t *data,
                        uint16_t length,
                        ucn_transfer_rx_handle_t handle)
{
    consume_message(source, endpoint, data, length);
    if (handle != UCN_TRANSFER_RX_HANDLE_DIRECT) {
        (void)ucn_transfer_release_received(&g_transfer, handle);
    }
}

ucn_transfer_config_t transfer_cfg = {0};
transfer_cfg.node = &g_node;
transfer_cfg.now_ms = transfer_now_ms;
transfer_cfg.now_context = NULL;
ucn_transfer_init(&g_transfer, &transfer_cfg);
ucn_transfer_bind_endpoint(&g_transfer, 0x80U,
                           UCN_TRANSFER_CLASS_T2K, false,
                           transfer_rx, NULL);
ucn_transfer_set_peer_capability(&g_transfer, remote_node_id,
                                 UCN_TRANSFER_CLASS_T2K);

/* 可选：默认双方窗口都是 1。只有已知对端支持时才显式打开流水。 */
ucn_transfer_set_tx_window_size(&g_transfer, 4U);
ucn_transfer_set_peer_window_capability(&g_transfer, remote_node_id, 4U);

/* 可选：多个独立分片消息并发。默认是 1；只有确认目标节点配置了
 * 至少 2 个 RX Slot 后才允许提高。它与上面的单消息 Fragment 窗口正交。 */
ucn_transfer_set_peer_concurrency_capability(&g_transfer,
                                             remote_node_id, 2U);

/* payload 在完成回调之前必须保持有效且不可修改。 */
ucn_transfer_send(&g_transfer, remote_node_id, 0x80U,
                  UCN_TRANSFER_CLASS_T512,
                  payload, payload_length,
                  transfer_send_complete, NULL);
```

必须在唯一 Protocol Owner 上下文调用 Send/Step。为保证 Core Q0、普通 Q1 和维护优先，先运行所选 Port/Owner Step，只有其返回 `UCN_ERR_NOT_FOUND` 时再调用一次 `ucn_transfer_step(&g_transfer)`。Transfer 的 Send、RX 与 Step 都从 `transfer_cfg.now_ms` 采样同一权威单调时钟；不得再向 Step 传缓存时间。每次最多推进一个新片或重传片；连续空闲 Step 可填满显式固定窗口。

注意五条规则：发送端 Buffer 在 Completion 前归应用所有但必须保持只读；分片接收 Buffer 在 `release_received()` 前占用固定 RX Slot；`ucn_transfer_init()` 会占用 Node 通用 RX Handler，原有通用 Handler 要放入 `fallback_rx_handler/context`；未显式配置 Peer 窗口时有效 Fragment 窗口始终为 1；未显式配置 Peer 消息并发时，同一目的节点最多只有 1 条分片消息在途。不能按本机能力猜测远端。窗口使用累计 ACK 和有界 Go-Back-N，多消息并发使用彼此独立的 Transfer ID/CRC/Deadline；两者都不改变 v5 Wire。四槽实测对 T128～T1K 有利、对 4/8 KiB 不利，见[V5-66 报告](UCN_V5_66_有界多消息并发Transfer优化.md)。

## 7. MCU 内多任务通信：Service Router + Bridge

### 7.1 何时使用

一个 MCU 内有 IMU Task、控制 Task、执行器 Task 时，它们也可用同一套 `(destination Node, Endpoint, QoS)` 语义通信。差别仅在目标 Node：

- 目标等于本机 Node：Router 本机 Fast Path，直接复制到目标 Inbox，不使用 Link、不编帧、不寻路；
- 目标是远端 Node：Router 复制到固定 Remote TX 队列，Bridge 在 Protocol Task 中调用 `ucn_node_send_endpoint()`；
- 远端帧到达本机：Node 的 Endpoint handler 通过 Bridge 投递 Router，再由目标业务 Task 读取 Inbox。

Router 本身不依赖 FreeRTOS。仓库外 ESP32 工程提供的 `UcnServiceFreeRtosPort` 可参考静态 Queue、事件通知和业务 Task 绑定方式，但该应用仍锁定线协议 v4；迁移其初始化和源文件选择前，不能把它当成当前 v5 编译/实机证据。

### 7.2 定义固定 Binding

```c
enum {
    kServiceSensor = 1U,
    kServiceControl = 2U,
    kServiceActuator = 3U,
};

static const ucn_service_binding_t g_bindings[] = {
    { .endpoint = 0x40U,
      .owner_service_id = kServiceControl,
      .max_payload_length = 24U,
      .allowed_traffic_mask = UCN_SERVICE_TRAFFIC_MASK(UCN_TRAFFIC_Q1_REALTIME),
      .delivery_mode = UCN_SERVICE_DELIVERY_Q1_LATEST,
      .allowed_local_source_mask = UCN_SERVICE_SOURCE_MASK(kServiceSensor),
      .accept_remote = true,
      .enabled_at_boot = true,
      .require_remote_q0_validator = false },
    { .endpoint = 0x60U,
      .owner_service_id = kServiceActuator,
      .max_payload_length = 16U,
      .allowed_traffic_mask = UCN_SERVICE_TRAFFIC_MASK(UCN_TRAFFIC_Q0_CRITICAL),
      .delivery_mode = UCN_SERVICE_DELIVERY_Q0_FIFO,
      .allowed_local_source_mask = UCN_SERVICE_SOURCE_MASK(kServiceControl),
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

Validator 在 Core 完成安全/解密后、Router 入队前运行，返回非 `UCN_OK` 的帧不会进入 Inbox。固定 Replay helper 对当前 `(Source, Session, Endpoint)` 只接受递增命令 ID；认证 Session 轮换后，由产品 Security 逻辑调用 `ucn_service_bridge_replay_rotate_session()`，不能看到不同 Session 就自动切换。Replay 表满返回 `UCN_ERR_NO_SPACE`，不得动态扩容。Bridge 应通过 `ucn_service_protocol_bridge_step_at()` 接收本轮显式 `now_ms`，并与随后 `ucn_node_step()` 使用同一时刻；产品仍必须满足 Protocol Task 最大 Step 间隔。

Validator 不规定 Payload ABI：可使用 12 B Command Guard，也可以解析产品已有的 16 B 电机命令。它只拦截远端帧，本机 Fast Path 仍由执行 Task 二次检查模式、范围、有效期、互锁和本地 watchdog。若 Task 还未创建，使用 `ucn_service_set_ready()` 显式切换。

### 7.3 从业务 Task 发送

纯 C Router 调用如下：

```c
ucn_result_t rc = ucn_service_send(&g_router,
                                   destination_node_id,
                                   kServiceControl,
                                   0x60U,
                                   UCN_TRAFFIC_Q0_CRITICAL,
                                   servo_payload,
                                   16U);
```

FreeRTOS 业务 Task 不应直接持有 Router/Node。迁移到 v5 后，可沿用历史 ESP32 `UcnServiceFreeRtosPort` 的调用形态：

```cpp
/* 业务 Task：本机或远端的调用形式完全相同。 */
const ucn_result_t rc = g_service_port.send(destination_node_id,
                                            kServiceControl,
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
while (g_service_port.inbox_take(kServiceControl, 0x40U, &message) == UCN_OK) {
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

Adapter 的 Cost 必须来自真实可解释指标。产品可以用 `ucn_standard_link_config_resolve()` 的 `base_cost` 作为官方静态初值，再由 Adapter 报告为 `route_cost`；Resolver 不会自动写入 Core。`route_cost` 越低越优；未知指标使用保守默认值。Wi-Fi 可映射 RSSI 平滑值、丢包与本 Adapter 队列压力；UART/CAN 可映射错误率、重试/Bus-Off 与队列压力。不要虚构 RTT，也不要把 LC-1 的动态评分当作已经运行的功能。

### 路由 Hop、Cost 与 RTT 门禁

Wire Profile 的最大 Hop 只表示“字段能否编码”，不代表业务一定应该走那么远。Full/Lite 可设置 Node 默认约束，并在单条 Policy 中用非零字段覆盖：

```c
ucn_route_constraints_t defaults = {
    .max_hops = 16U,
    .max_route_cost = UINT32_C(120000),
    .max_verified_rtt_ms = 80U,
    .require_verified_rtt = false,
};
ucn_route_quality_t quality;

(void)ucn_node_set_default_route_constraints(&g_node, &defaults);
if (ucn_node_get_route_quality(&g_node, remote_node, &quality) == UCN_OK &&
    quality.available) {
    /* 只用于诊断/管理展示；业务发送仍由 Core 执行约束。 */
}
```

`max_hops==0`、`max_route_cost==0` 和 `max_verified_rtt_ms==0` 在 Policy 中表示继承 Node 默认。有限 Cost 门禁遇到未知 Cost 时失败关闭；`require_verified_rtt=true` 时，没有已验证端到端 RTT 的路线不能直接承载该 Policy。直连 Link 只有在 Adapter 的 `get_metrics()` 明确给出有效 RTT 时才作为已验证单跳 RTT；动态多跳 Route 使用 Candidate Path Probe/ACK 得到的 RTT EWMA。Nano 保留统一 API，但因不编译动态路由/策略能力而返回 `UCN_ERR_CONFIG`。

自动发现采用有界 Expanding Ring：默认按 2→4→8→16 Hop 扩展，单轮 250 ms、总预算 1000 ms。业务发送不会每帧寻路，也不会在 Pending 期间反复重启当前 Ring；已有可用缓存路线时直接发送。

### 8.1 可选：启用单层自动分簇

需要 MCU 在一跳邻居中自动选 Head 时，产品额外链接 `ucn_cluster`、包含 `ucn/ucn_cluster.h`，并创建一个静态 `ucn_cluster_t`。配置 `local_node_id/head_capable/head_score/member_capacity`、权威单调 `now_ms` 和 Send 回调；Send 回调应继续调用正常的 `ucn_node_send_endpoint()`，不能绕开 Core。

产品把静态 Endpoint `UCN_CLUSTER_CONTROL_ENDPOINT`（`0xA0`）绑定到一个 Handler：Handler 将来源 Node ID、保护状态和 Payload 交给 `ucn_cluster_receive()`。产品重新计算并滤波 Head 能力后，可在唯一 Owner 调用 `ucn_cluster_set_head_score()`；它只更新评分和后续广告，不会因一次瞬时变化强迫现任 Head 退位。唯一 Protocol Owner 周期调用：

```c
ucn_cluster_sync_node_neighbors(&cluster, &node);
ucn_cluster_step(&cluster);
```

生产部署应给 `0xA0` 配 Endpoint Security Policy，并设置 `require_protected_control=true`。普通任务和 ISR 不得直接推进 Cluster 对象。默认时序为稳定优先；固定低丢包有线介质若需要缩短 Head 故障恢复，可在 `ucn_cluster_init()` 前调用 `ucn_cluster_config_apply_timing_profile(&config, UCN_CLUSTER_TIMING_PROFILE_FAST_FIXED)`。当前 C07 已在 Cluster Control v3 上提供有界 Backup 指派、快照、主备多数接管与 `RECOVERY_HEAD`：成员只接受已由 Head 宣布的 Backup ID/Generation，不能自行相信来源节点。它仍是单层、一跳控制域；若产品需要远端成员定位或 Directory Handover，必须另行接入下面的 C06 Federation，并在受保护模式提供 Handover proof Builder 和 Authorizer。完整边界见 [C07 主备簇头方案](UCN_V5_C07_主备簇头与快速代理恢复整体方案.md)。

### 8.2 可选：接入 C06.3 Locator Directory 与单帧跨簇 Tunnel

产品显式链接 `ucn_cluster_federation` 后，创建一个静态 `ucn_cluster_federation_t`，把它绑定到 `0xA1`。它和 `ucn_cluster_t` 必须由**同一个** Protocol Owner 串行推进。发送回调只负责把已编码的 Locator 控制载荷交回正常 Core Endpoint；目录副本、Head 授权和安全策略由产品提供：

```c
static const ucn_node_id_t g_directory_authorities[] = {
    UINT32_C(0x00000011),
    UINT32_C(0x00000012),
};

static ucn_result_t federation_send(void *context,
                                    ucn_node_id_t destination,
                                    ucn_endpoint_t endpoint,
                                    ucn_traffic_class_t traffic_class,
                                    const uint8_t *payload,
                                    uint16_t length)
{
    ucn_node_t *node = (ucn_node_t *)context;
    return ucn_node_send_endpoint(node, destination, endpoint,
                                  traffic_class, payload, length);
}

static bool federation_authorize_head(void *context, ucn_node_id_t source)
{
    (void)context;
    return product_head_is_authorized(source); /* 产品 ACL/身份策略 */
}

static void federation_rx(void *context, const ucn_frame_t *frame)
{
    ucn_cluster_federation_t *federation = context;
    bool protected_outer =
        (frame->flags & UCN_FRAME_FLAG_E2E_PROTECTED) != 0U;

    (void)ucn_cluster_federation_receive(federation, frame->source,
                                         protected_outer, frame->payload,
                                         frame->payload_length);
}
```

初始化时填入 `local_node_id`、`cluster=&g_cluster`、权威 `now_ms`、上述 Send 回调和固定 `directory_authorities`。充当 Directory Authority 的节点还必须设置 `directory_authority=true`，提供 `authorize_head`，并让自己的 Node ID 出现在副本数组中；否则初始化失败关闭。每轮 Owner 顺序是：

```c
ucn_cluster_sync_node_neighbors(&g_cluster, &g_node);
ucn_cluster_step(&g_cluster);
ucn_cluster_federation_step(&g_federation);
ucn_node_step(&g_node, now_ms);
```

默认 `enable_tunnel=false` 时，行为仍是 C06.2：只有当前 Head 可以调用 `ucn_cluster_federation_query_locator(&g_federation, target_node)`，`UCN_OK` 仅表示 Cache 命中或 Query 已发出，随后通过 `find_locator()` / `find_next_cluster()` 读取异步结果。

需要单帧跨簇业务时，产品显式设置 `enable_tunnel=true`。默认模式要求 `seal_inner`、`open_inner` 和 `deliver` 三个回调；缺任一个 `ucn_cluster_federation_init()` 都返回 `UCN_ERR_CONFIG`。台架诊断若只能依赖外层保护，必须显式设为 `UCN_CLUSTER_FED_INNER_SECURITY_PROTECTED_OUTER_ONLY`，不得标为端到端安全。业务发起方只调用新 API：

```c
ucn_result_t result = ucn_cluster_federation_send(
    &g_federation, remote_member, 0x50U,
    UCN_TRAFFIC_Q1_REALTIME, sensor_payload, sensor_length);
```

它只走 `A -> 本簇 Head H1 -> 目标 Head H2 -> C`；中继只学习 H2，不学习 C。Q0/Q1 与 Endpoint 原样传递。若 H1 暂无 C 的 Locator，它会发起有限 Query 并通过 `on_error(DIRECTORY_NOT_FOUND)` 通知 A；应用等待 `find_locator()` 出现结果后使用**新的调用/Transaction**重试，Federation 不会在 Head 中缓存用户 Payload。最终 C 的 `open_inner()` 成功后才会执行 `deliver()`。`T32～T8K Transfer`、大包分片和自动 Gateway 尚未穿过 Tunnel，仍待 C06.4。Endpoint `0xA1` 应配置为必须端到端保护，`require_protected_control=true` 时任何明文 Federation 帧被拒绝。详见 [C06 详细设计](UCN_V5_C06_簇间寻址目录与隧道详细设计.md)。

## 9. 指定路径、主备与 Q1 负载均衡

### 9.1 默认自动路径

什么也不配置时使用 `UCN_ROUTE_POLICY_AUTO_BEST`。调用仍是 `ucn_node_send_endpoint()` 或 Service `send()`；Core 根据已有 Route/候选路径和健康 Bearer 选择。不需要也不应该让业务 Task 每帧寻路。

### 9.2 真正启用指定路径前的前提

`local_path_id` 只是源端固定表的本地句柄；不能只写一个 Policy 就声称路径已上线。正确的安装顺序是：

1. 产品配置安全 Provider 与 `ucn_node_set_path_control_authorizer()`；默认拒绝是故意的；
2. 控制器统计整条 Path 的共同能力 `{maximum_wire_profile, minimum_mtu}`；源 Node 为 P1/P2 调用 `ucn_node_install_local_path_capable()`，显式填写从源端开始的 `remaining_hops` 和共同能力；
3. 源 Node 依次向每个中继和终端调用 `ucn_node_send_path_install_capable()`，每一跳的 `remaining_hops` 递减，非终端使用同一端到端瓶颈，终端必须为 `next_hop=0, remaining_hops=0`；中继不需要解密端到端业务；
4. 源 Node 用 `ucn_node_set_policy_path()` 把本地句柄、已认证 `wire_path_id`、目的 Node、首跳 Link 关联，并设 `verified=true`；
5. 用 `ucn_node_set_route_policy()` 为一个精确的 `(destination, endpoint, traffic_class)` 绑定策略；
6. 业务继续调用原来的 `ucn_node_send_endpoint()` / Service `send()`，无需把路径号传给每次业务调用。

远端 `PATH_INSTALL/PATH_REVOKE` 的接收顺序固定为 Security Provider、产品 Path Authorizer、认证管理源预算、最后才修改 Path 表。PATH_INSTALL 有两种 v5 精确格式：旧 `ucn_node_send_path_install()` 发送含 `RemainingHops` 的基础 `8/11/14/17 B`，`ucn_node_send_path_install_capable()` 才发送再含 `MaximumWireProfile + MinimumMTU` 的扩展 `11/14/17/20 B`。新接收端两种都接受；基础格式派生本跳能力，扩展能力与本跳逻辑 Neighbor 的 Bearer 交集求更窄值。旧 v5 节点不理解扩展长度，所以未确认目标支持时应继续使用旧 API。默认最多跟踪 4 个活动管理源，每个来源的 INSTALL/REVOKE 各允许突发 4 次并按 1 s/Token 恢复；来源身份是 `(source Node, source Session)`，不包含 Link，所以切换 Wi-Fi/UART Bearer 不能绕过。产品可用 `UCN_PATH_CONTROL_RX_SOURCE_DEPTH`、`UCN_PATH_CONTROL_RX_TOKEN_BURST`、`UCN_PATH_CONTROL_RX_TOKEN_REFILL_MS`、`UCN_PATH_CONTROL_RX_SOURCE_IDLE_MS` 按 MCU RAM 和管理频率调整。Session 更新必须先由产品 Security 接受；旧 Session 吊销仍是 Security Provider 的责任。

调试时读取 `ucn_node_get_stats()`：`path_*_authorization_rejected` 表示 Security/产品授权拒绝，`path_*_budget_rejected` 表示对应操作 Token 耗尽，`path_control_budget_source_full` 表示活动管理源槽已满，`path_install_table_full` 才表示真正的 Path 转发表已满。以上拒绝均发生在 Path 表写入前。

```c
ucn_policy_path_config_t p1 = {
    .local_path_id = 1U,
    .wire_path_id = 0x101U,       /* 已经在逐跳控制面安装 */
    .destination = UINT32_C(0x0000000D),
    .egress_link = &g_link_to_b,
    .verified = true,
    .route_cost_valid = true,
    .route_cost = UINT32_C(2500),
    .verified_rtt_valid = true,
    .verified_rtt_ms = 12U,
};
ucn_route_policy_config_t policy = {
    .key = { UINT32_C(0x0000000D), 0x40U,
             (uint8_t)UCN_TRAFFIC_Q1_REALTIME },
    .mode = UCN_ROUTE_POLICY_PINNED_FAILOVER,
    .primary_local_path_id = 1U,
    .backup_local_path_id = 2U,
    .allow_discovery_on_hard_failure = false,
    .constraints = {
        .max_hops = 4U,
        .max_route_cost = UINT32_C(10000),
        .max_verified_rtt_ms = 50U,
        .require_verified_rtt = true,
    },
};

const ucn_path_capability_t p1_capability = {
    .maximum_wire_profile = UCN_WIRE_PROFILE_W1_EDGE,
    .minimum_mtu = 64U,
};
(void)ucn_node_install_local_path_capable(
    &g_node, 0x101U, UINT32_C(0x0000000D),
    UINT32_C(0x0000000B), 2U, 30000U, &p1_capability);
/* 给中继 B 安装 next=D, remaining=1；给终端 D 安装 next=0,
 * remaining=0。非终端传入同一个 p1_capability；每次远端安装都要
 * 通过 Security 与 Authorizer。 */
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

`AUTO_BALANCE` 不是逐帧轮询、不是帧复制、不是带宽聚合，也不适用于 Q0。Full 在新建/到期 Flow 时按出口 Link 的 LC-1 `effective_select_cost ×（活动 Flow 数 + 1）` 评分；默认流租约 2 s，租约内只在持续 3 个 500 ms 窗口队列压力达到 800‰或 Path 硬 Down 时重绑。这样 IMU 的同一流不会因为每帧跳线而制造乱序。

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
| 单层自动分簇与 C06 Directory | Cluster 已有选举、成员租约、Backup 同步、多数接管、`RECOVERY_HEAD` 与稳定回切；C06 已有固定 Directory、Query/Cache、Cluster→Head Lease 和受 proof 保护的 Handover。Host 已覆盖至 1000 节点受损模拟；独立 Bearer、小时级长稳、功耗、生产密钥/ACL、跨簇 Transfer 与多级簇仍待。 |
| 生产密码、身份、密钥管理、AES/ChaCha AEAD 选型 | 未内置；必须由产品 Security Provider 落地和审计。 |
| CAN 小 MTU 分段重组、BLE/LoRa/真实 Linux Adapter | 取决于具体产品 Adapter，未因 Core 存在而自动获得。 |

## 16. 相关文档与源码入口

- [UCN v4 协议核心说明](UCN_v4_协议核心说明.md)：协议是什么、帧和架构边界。
- [整体架构设计](UCN_整体架构设计.md)：模块关系和演进方向。
- [路由策略与负载均衡执行建议](UCN_路由策略与负载均衡执行建议.md)：Path/策略/质量阈值的设计理由。
- [T25 首版 Endpoint 与 Service 契约](UCN_T25_首版Endpoint与Service契约.md)：R1 业务 ABI 和 Payload 规则。
- [T25 节点内任务通信详细执行方案](UCN_T25_节点内任务通信详细执行方案.md)：Router/Bridge/RTOS 边界。
- [UCN 调用关系树](calltree/README.md)：按真实函数调用、回调和固定队列关系追踪运行路径。
- [V5 自动分簇详细设计](UCN_V5_67_自动分簇详细设计与首阶段实现.md)：Cluster 接口、固定资源、控制 Schema、Owner 接入与未完成边界。
- [C05.1 快速簇恢复档](UCN_V5_C05_1_快速簇恢复档设计与验证.md)：默认/快速时序、API、租约安全关系与验证边界。
- [C06 簇间寻址、目录与隧道详细设计](UCN_V5_C06_簇间寻址目录与隧道详细设计.md)：当前 Locator Directory 与 C06.3 单帧 Tunnel，以及 C06.4 Transfer/自动 Gateway 的严格未完成边界。
- [快速使用手册](快速使用手册/README.md)：裸机、通用 RTOS、FreeRTOS、Zephyr、NuttX、RT-Thread 的最小接入步骤与平台边界。
- `include/ucn/`：最终以公开 API 声明和编译期配置为准；若本文与源码不一致，以源码为准并同步修订本文。
