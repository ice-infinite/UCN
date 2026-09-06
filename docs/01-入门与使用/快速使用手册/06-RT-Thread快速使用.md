# UCN RT-Thread 快速使用

> `ARCHIVED / NOT CURRENT`：本文仅保留 v5 RT-Thread 接入记录；v6 请从[当前用户手册](../../用户手册/README.md)开始。

> 适用：RT-Thread BSP 和设备框架。当前仓库已有独立、SDK 无关的 `ucn_port_rtthread` C99 外壳；产品仍需把 Hook 映射到静态线程、互斥量、信号量/事件和真实设备驱动，不修改 C99 Core。

> 当前 API：V5-62 Port API V2 要求所有 `ucn_port_ops_t` 具名填写 `struct_size/api_version`；旧位置初始化与旧对象不兼容。Transfer 还必须配置权威时钟并使用无时间参数的 `ucn_transfer_step()`。迁移见[总览](README.md)。

## 1. 工程配置

在产品 `Kconfig` 建立自己的开关和资源值，例如：

```text
config RT_USING_PRODUCT_UCN
    bool "Enable UCN"

config PRODUCT_UCN_PROTOCOL_STACK_SIZE
    int "UCN protocol thread stack"
    default 2048

config PRODUCT_UCN_RX_DEPTH
    int "UCN RX frame depth"
    default 2
```

在产品 `SConscript`（或 BSP 已有构建规则）按[总览源文件矩阵](README.md#共同的构建输入)选择 Nano/Lite/Full 与可选 Service 源文件，并加入 `include/`。将上述 Kconfig 值映射为 `UCN_MAX_FRAME_BYTES`、`UCN_ADAPTER_RX_QUEUE_DEPTH`、`UCN_SERVICE_*` 等编译宏；所有编译单元必须共享同一个 `UCN_PROFILE`、`UCN_FEATURE_SERVICE` 和配置头。不要直接用 Heap 扩容协议表；Core/Router 的容量要在编译期冻结。

## 2. 静态线程和对象

在 `rt_thread_startup()` 前完成 `ucn_node_init()`、`ucn_node_set_wire_profiles()`、可选自动最小档、明文 Boot Session 或生产 Security，以及 Link 注册。Wire Profile 配置属于产品初始化，不能由不同业务线程各自修改；业务线程只通过 Service Router 发送 Endpoint 数据。

```c
#include "ucn/ucn_node_storage.h"

static struct rt_thread g_protocol_thread;
static rt_uint8_t g_protocol_stack[PRODUCT_UCN_PROTOCOL_STACK_SIZE];
static struct rt_mutex g_router_lock;
static struct rt_semaphore g_rx_ready;

static ucn_node_t g_node;
static ucn_adapter_rx_queue_t g_rx_queue;
static ucn_service_router_t g_router;
static ucn_service_protocol_bridge_t g_bridge;

static void start_ucn_protocol_thread(void)
{
    rt_mutex_init(&g_router_lock, "ucnrt", RT_IPC_FLAG_PRIO);
    rt_sem_init(&g_rx_ready, "ucnrx", 0, RT_IPC_FLAG_PRIO);
    rt_thread_init(&g_protocol_thread, "ucnproto", ucn_protocol_entry, RT_NULL,
        g_protocol_stack, sizeof(g_protocol_stack), PRODUCT_UCN_PROTOCOL_PRIORITY,
        PRODUCT_UCN_PROTOCOL_TICK);
    rt_thread_startup(&g_protocol_thread);
}
```

优先用 `rt_thread_init()`、`rt_mq_init()` 等静态初始化 API，而不是 `rt_thread_create()` / `rt_mq_create()`。线程栈和 IPC 缓冲属于产品静态资源预算的一部分。

## 3. ISR、设备接收和 Protocol Thread

设备 ISR 或回调只维护 BSP 的固定 DMA/ring buffer，并按该 BSP 的 ISR 规则通知 RX Worker 或 `g_rx_ready`。不要在 ISR 中取 `rt_mutex`，不要调用 `ucn_node_receive()`。

```c
static void ucn_protocol_entry(void *parameter)
{
    (void)parameter;
    while (RT_TRUE) {
        size_t pumped = 0U;
        rt_uint8_t bridged = 0U;
        const uint32_t now_ms = (uint32_t)rt_tick_get_millisecond();

        product_drain_driver_rx_to_adapter();
        (void)ucn_adapter_rx_pump(&g_rx_queue, &g_node, 4U, &pumped);
        (void)ucn_service_protocol_bridge_step_at(&g_bridge, now_ms,
                                                   2U, &bridged);
        (void)ucn_node_step(&g_node, now_ms);
        (void)rt_sem_take(&g_rx_ready, PRODUCT_UCN_PROTOCOL_WAIT_TICKS);
    }
}
```

`PRODUCT_UCN_PROTOCOL_WAIT_TICKS` 必须有限，使 `ucn_node_step()` 在空闲时仍能处理 Heartbeat、邻居/路由超时。若接收仅在 Protocol Thread 内轮询，`ucn_adapter_rx_queue_init(..., NULL, NULL)` 可避免额外锁；任务并发入队时提供任务临界区，ISR 直入完整帧时再额外提供 token 型 ISR 临界区并调用 `ucn_adapter_rx_enqueue_from_isr()`。不能用线程锁替代 ISR token。

## 4. Router 和本机任务通信

用一张固定 Binding 表初始化 Router，再把同一短 Router 锁装入 Bridge Hooks。产品 RT-Thread Port 应提供两个封装：

```c
ucn_result_t product_ucn_send(ucn_node_id_t destination,
                              ucn_service_id_t source,
                              ucn_endpoint_t endpoint,
                              ucn_traffic_class_t qos,
                              const uint8_t *payload, uint16_t length)
{
    ucn_result_t rc;
    rt_mutex_take(&g_router_lock, RT_WAITING_FOREVER);
    rc = ucn_service_send(&g_router, destination, source, endpoint,
                          qos, payload, length);
    rt_mutex_release(&g_router_lock);
    return rc;
}
```

接收侧以相同短锁调用 `ucn_service_inbox_take()`。远端帧由 Bridge 在 Protocol Thread 投递 Router，随后 observer 通过静态 `rt_mq`、事件或 semaphore 通知正确的 Service 线程。通知仅传 Endpoint/token；Q1 的 Payload 可能已覆盖成最新值，线程必须反复读取 Inbox。

若 Binding 设置 `require_remote_q0_validator=true`，产品必须先调用 `ucn_service_protocol_bridge_set_validator()` 注册校验器，再安装 Endpoint handlers；缺少 Validator 会失败关闭。

不建议把原始 `ucn_node_t` 放在全局头文件让各线程调用；只暴露 `product_ucn_send()`、`product_ucn_inbox_take()` 与受控只读诊断快照。

## 5. Link 和 RT-Thread 设备框架

Adapter 将 `rt_device_read()`、UART DMA、CAN 接收或无线模块回调转换为完整 UCN 帧。对应 `ucn_link_ops_t`：

- `open()` 配置设备、DMA、回调和固定缓冲；
- `send()` 有界提交 Core 帧；
- `get_status()` 反映真实连接/Bus-Off/错误；
- `get_status().mtu` 可报告运行期完整 UCN 帧上限；与 `link.mtu` 同时存在时取较小值；
- `get_metrics()` 输出平滑后的通用 Cost；
- `close()` 停止该 Adapter，不改变其他 Bearer。

一个对端的 UART、CAN、无线 Link 使用同一个 `peer_node_id`，由 UCN Neighbor 聚合多 Bearer；业务线程不手选物理口。经典 CAN C1/C2 Carrier 与固定重组可复用 `ucn_can_source_t`，但 RT-Thread CAN 控制器/中断/Frame Ring/TX Queue/Bus-Off 恢复仍由产品 Adapter 实现；无线配网和生产 AEAD 同样属于产品边界。

## 6. 上板验收

1. 构建后打印 `ucn_version()`、`UCN_MAX_FRAME_BYTES`、Node ID、Link 状态和各固定 Queue 深度。
2. 以两节点 Q1 Endpoint 验证端到端收发，再分别测本机 Fast Path 和远端 Q0。
3. 人为使 DMA ring、Adapter RX、Router Q0/Q1、远端 TX 和事件通知满，确认只有有界丢弃/覆盖与统计，无线程阻塞/死锁。
4. 记录 `rt_thread` 栈余量、CPU、堆状态（即使 UCN 不申请 Heap）、Link 错误与失联时执行器本地安全行为。

## 7. S16 Protocol Thread 时限

产品配置需冻结 `UCN_MAX_STEP_INTERVAL_MS`、线程优先级、最大 IPC 等待 tick、Adapter Pump/Bridge 预算和 Link `send()` WCET。Protocol Thread 应采用小于最大 Step 的周期/超时唤醒，不能永久挂在信号量上。持续负载时读取 `max_step_gap_ms`、`step_interval_violations` 并校验 Heartbeat/Probe 仍在产品上界内；当前尚无 RT-Thread 实机数据。

## 8. V5-58/59 RT-Thread Event Runtime 与 Stream 映射

产品用 `rt_event`/`rt_sem` 实现统一 Scheduler Hook，`notify_owner(from_isr)` 内选择可在当前上下文调用的 IPC API，`wait_owner()` 必须有有限 Tick 超时。每个 `rt_device` UART/CAN/USB/无线实例绑定一个固定 Source 和自己的 Ring；回调只 Signal，Protocol Thread 执行全部 Source `service()`。现有 `ucn_rtthread_port_*` 保留单 Queue 兼容，不与 Event Runtime 重复拥有同一 Node。

RT-Thread UART/USB 的接收回调把字节交给每实例 `ucn_stream_source_write[_from_isr]()`；公共 Source 统一 COBS 与 Queue 背压，`rt_device` 打开、DMA、串口参数、TX 完成和 IPC 对象继续只在产品文件中。
