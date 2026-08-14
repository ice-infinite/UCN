# UCN FreeRTOS 快速使用

> 适用：FreeRTOS MCU。ESP32 测试工程（`E:\File\PlatformIO\ESP32_UCN\ESP32S3_N16R8_UCN_Test1`）已形成 v5 事件 Owner/多板 UART Bench 证据，可作为 Arduino-ESP32 静态对象、短临界区和通知模型的产品参考；它仍不是打包进 Core 的通用 FreeRTOS SDK glue，不能把 ESP32 的 `portMUX_TYPE`、引脚、任务栈或实测吞吐直接外推到其他芯片。

> 当前 API：V5-62 Port API V2 要求所有 `ucn_port_ops_t` 具名填写 `struct_size/api_version`；旧位置初始化与旧对象不兼容。Transfer 还必须配置权威时钟并使用无时间参数的 `ucn_transfer_step()`。迁移见[总览](README.md)。

构建 v5 时按[总览的 Build Profile 与源文件矩阵](README.md#先选择-build-profile)重新选择 Nano/Lite/Full 源文件，并在所有 C/C++ 编译单元保持相同的 `UCN_PROFILE` 与 `UCN_FEATURE_SERVICE`。当前 ESP32 工程的源文件选择脚本已经具备该矩阵，但应用层 `main.cpp`/UART Benchmark 仍需另行迁移掉 v4 锁定后，才能称为 v5 FreeRTOS 实机参考。

## 1. 参考实现提供了什么

| 文件 | 当前作用 |
| --- | --- |
| `include/ucn_service_freertos_port.h` | 业务 Task 的 `send()`、`inbox_take()`、`event_take()` API，Service 与 Task 绑定。 |
| `src/ucn_service_freertos_port.cpp` | Router 短临界区、静态事件 Queue、Bridge 入站通知和统计。 |
| `src/main.cpp` | Router/Bridge/Port 初始化、`xTaskCreateStatic()` 与 Arduino `loopTask` 的 Protocol Task 模型。 |
| `include/ucn_test_profile.h` | 当前测试的静态队列深度、Bridge 预算和任务栈大小。移植产品时应改为自己的产品 Profile。 |

该 Port 的事件 Queue 只存 1 字节 Endpoint token，**不存第二份 Payload**；Router Inbox 才是唯一 Payload 副本。因此无论事件 Queue 是否满，业务 Task 被唤醒后都要继续 `inbox_take()` 直到 Inbox 为空。

## 2. 按静态方式建立对象

```cpp
extern "C" {
#include "ucn/ucn_node_storage.h"
}

static ucn_node_t g_node{};
static ucn_service_router_t g_router{};
static ucn_service_protocol_bridge_t g_bridge{};
static UcnServiceFreeRtosPort g_service_port;

static constexpr ucn_service_id_t kServiceControl = 2U;
static constexpr ucn_service_id_t kServiceActuator = 3U;

static StaticTask_t g_service_task_tcb{};
static StackType_t g_service_stack[UCN_PRODUCT_SERVICE_STACK_DEPTH]{};
```

FreeRTOS 的 Task、事件 Queue、Adapter RX 缓冲和 UCN 固定表都应在静态区。不要让 `xTaskCreate()`、`xQueueCreate()`、`pvPortMalloc()` 成为协议路径的隐含依赖。ESP-IDF Xtensa 的 `StackType_t` 单位与部分 Cortex-M Port 不同，应按照目标 FreeRTOS Port 对 `xTaskCreateStatic()` 的要求传递深度，而不是照搬字节数。

## 3. 启动顺序

Node 启动顺序固定为 `ucn_node_init()` → `ucn_node_set_wire_profiles()` → 可选 `ucn_node_set_wire_profile_auto(true)` → 明文 Boot Session 或生产 Security → Link → Router/Bridge/Port → 创建任务。所有配置都在 Scheduler 启动前完成，不能让多个 Task 在运行中修改 Wire 域。明文节点的非零 Session 必须在有效重复窗口内跨重启不复用。

```cpp
static void create_ucn_service_task(void)
{
    const ucn_service_router_config_t router_config = {
        g_node.config.node_id,
        g_bindings,
        static_cast<uint8_t>(kBindingCount),
    };
    const UcnServiceFreeRtosConsumerConfig consumers[] = {
        { kServiceControl },
        { kServiceActuator },
    };

    if (ucn_service_router_init(&g_router, &router_config) != UCN_OK ||
        ucn_service_protocol_bridge_init(&g_bridge, &g_router, &g_node) != UCN_OK ||
        product_register_remote_q0_validators(&g_bridge) != UCN_OK ||
        ucn_service_protocol_bridge_install_endpoint_handlers(&g_bridge) != UCN_OK ||
        g_service_port.begin(&g_router, &g_bridge, consumers, 2U) != UCN_OK) {
        product_enter_safe_mode();
    }

    TaskHandle_t task = xTaskCreateStatic(service_task, "ucn-service",
        UCN_PRODUCT_SERVICE_STACK_DEPTH, &g_service_port,
        tskIDLE_PRIORITY + 1U, g_service_stack, &g_service_task_tcb);
    if (task == nullptr ||
        g_service_port.bind_consumer_task(kServiceControl, task) != UCN_OK ||
        g_service_port.bind_consumer_task(kServiceActuator, task) != UCN_OK) {
        product_enter_safe_mode();
    }
}
```

先初始化 Node、Link 和 Adapter，再初始化 Router/Bridge/Port。`kServiceControl`/`kServiceActuator` 是产品自定义 ID，不是 UCN 公共常量。`product_register_remote_q0_validators()` 必须为每个 `require_remote_q0_validator=true` 的 Binding 调用 `ucn_service_protocol_bridge_set_validator()`；没有此类 Binding 时产品函数可直接返回 `UCN_OK`。Router 的 Node ID 与 Node 配置不一致、Validator 缺失、Endpoint handler 被其他模块占用、Binding 超过固定上限都会返回错误，不能忽略。

## 4. 唯一 Protocol Task

Arduino/ESP-IDF 项目可以让 `loopTask` 成为 Protocol Task；普通 FreeRTOS 项目则创建一个静态 Task。无论哪个，只有它能触碰 `g_node`：

```c
static void ucn_protocol_task(void *argument)
{
    for (;;) {
        size_t pumped = 0U;
        uint8_t bridged = 0U;

        product_drain_driver_rx(); /* 完整帧进入 Adapter RX Queue。 */
        const uint32_t now_ms = product_monotonic_ms();
        (void)ucn_adapter_rx_pump(&g_rx_queue, &g_node, 4U, &pumped);
        (void)ucn_service_protocol_bridge_step_at(&g_bridge, now_ms,
                                                   2U, &bridged);
        (void)ucn_node_step(&g_node, now_ms);
        vTaskDelay(pdMS_TO_TICKS(1U));
    }
}
```

Wi-Fi、UART、CAN 的 ISR/回调不能调用本循环中的函数。回调只填充自己的有限接收缓冲并通知 Protocol Task；由它解码/入 Adapter Queue 后 Pump。

若产品明确选择“完整帧由 ISR 直接进入 Adapter Queue”，`ucn_port_ops_t` 必须同时提供任务临界区和 ISR token 临界区。FreeRTOS 的 token 应使用目标 Port 的 `taskENTER_CRITICAL_FROM_ISR()` 返回值，并由对应 `taskEXIT_CRITICAL_FROM_ISR(token)` 恢复；不能把 `taskENTER_CRITICAL()` 或普通 mutex 用于 ISR。缺失 ISR 对时 `ucn_adapter_rx_enqueue_from_isr()` 会返回 `UCN_ERR_CONFIG`。ESP-IDF 的具体宏和 `portMUX_TYPE` 仍随芯片/SDK 变化，必须在目标 BSP 验证；默认仍优先 ISR→DMA/ring→Protocol Task。

## 5. 业务 Task 收发

```cpp
/* 所在任务必须已被 bind_consumer_task() 绑定。 */
static void control_task(void *argument)
{
    for (;;) {
        ucn_endpoint_t token;
        ucn_service_message_t message{};

        (void)g_service_port.event_take(kServiceControl, &token,
                                        pdMS_TO_TICKS(20U));
        while (g_service_port.inbox_take(kServiceControl, 0x40U,
                                         &message) == UCN_OK) {
            control_use_imu(message.payload, message.payload_length);
        }
    }
}

static ucn_result_t send_servo(ucn_node_id_t destination,
                               const uint8_t payload[16])
{
    return g_service_port.send(destination, kServiceControl, 0x61U,
                               UCN_TRAFFIC_Q0_CRITICAL, payload, 16U);
}
```

若目标是本 Node，`send()` 直接进入 Router Inbox；若是远端 Node，它进入 Remote TX，下一次 Bridge Step 才交给 Core。`UCN_OK` 只代表本机 Router 接受，不是远端执行确认。

如产品需要处理 Adapter TX Queue 的短暂 `UCN_ERR_NO_SPACE`，可在初始化时调用 `ucn_service_protocol_bridge_set_q0_backpressure_policy()`，为 Bridge 启用一个固定 Pending Q0 槽。它只对 Q0、只在固定次数/间隔/超时内重试；默认关闭，Q1 仍是 Latest Value。可用 `ucn_service_protocol_bridge_set_outbound_observer()` 接收一次最终本机提交结果，但 `UCN_OK` 仍不代表远端已入 Inbox 或已执行。完整示例和语义见 [UCN 使用与调用手册](../UCN_使用与调用手册.md#75-可选-q0-本机背压重试)。

## 6. 从 ESP32 参考复制时必须改的部分

- 将 `UCN_TEST_SERVICE_*` 宏替换为产品 Profile 宏，按实测栈、事件 Queue 和 Bridge 预算配置。
- 将 ESP32 `portMUX_TYPE` 替换为目标芯片可用于短临界区的机制；不能在 ISR 使用会阻塞的 mutex。
- 保留“Router Inbox 是 Payload 唯一副本”与“Bridge 只由 Protocol Task 调用”。
- Bridge 和 Node Step 使用同一个单调 `now_ms`；若启用 Q0 背压策略，必须根据产品控制周期设置有限 Retry/Interval/Timeout，不能照抄示例后跳过实测。
- 按[统一动态 MTU 合同](README.md#共同的构建输入)实现 Link；静态和运行期 MTU 都为 0 时失败关闭，运行期 MTU 恢复后不重建 Node。
- 监控 `UcnServiceFreeRtosPortStats`、Router、Bridge、Adapter 和 Node 统计；还要用目标平台工具测任务 High Water Mark、Heap 和 CPU 占用。

先在两节点以 Q1 小包验证，再测本机/远端 Q0、事件队列满、物理断链与执行器本地超时安全。当前 ESP32 参考的具体实机结论不能自动外推到其他 FreeRTOS 板子。

## 7. S16 Protocol Task 时限

ESP32 参考端把 Arduino `loopTask` 作为唯一 Protocol Task，并冻结最低优先级 1、最大 Block 1 ms、Wi-Fi/UART Pump 各 4 帧、Bridge 2 条、`UCN_MAX_STEP_INTERVAL_MS=10`。移植时必须按目标板重新测量这些值和 Link `send()` WCET；启动日志与周期统计应包含 `max_step_gap_ms`、`step_interval_violations`。不要把业务 Task 的 50 ms Queue Wait 复制给 Protocol Task。

## 8. V5-58/59 FreeRTOS Event Runtime 与 Stream 映射

新产品将 `notify_owner(false)` 映射到 Task Notification Give，将 `notify_owner(true)` 映射到目标 Port 的 FromISR Give 并按需要请求切换；`wait_owner()` 用有界 `ulTaskNotifyTake()` 返回是否被通知，`yield_owner()` 使用任务上下文 Yield。每个 UART/CAN/USB/Wi-Fi Source 有自己的静态 Ring；中断里只写 Ring 和 Signal。现有 `ucn_freertos_port_*` 是单 Queue 兼容入口，不能与新的 Runtime 同时成为同一 Node 的两个 Owner。

UART/RS-485/USB CDC 使用公共 `ucn_stream_source_t`：RX Task/ISR 只调用 `ucn_stream_source_write[_from_isr]()`，COBS 和公共 Queue 背压只在 Protocol Task 运行。FreeRTOS 仍负责实际 UART Event/DMA、Task Notification、TX Queue 和 FromISR Yield。
