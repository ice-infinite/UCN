# UCN 通用 RTOS 快速使用

> 适用：任意有任务、互斥锁和有界队列的 RTOS。当前仓库没有一个通用的 `UcnServiceRtosPort`；本页给出应实现的最小适配边界，保持 C99 Core 不依赖具体 RTOS。

## 1. 任务划分

| 上下文 | 允许做的事 | 禁止做的事 |
| --- | --- | --- |
| 驱动 ISR / DMA 回调 | 把物理数据写入 ISR 安全的固定环形缓冲或消息队列，发通知。 | 调用 `ucn_node_*`、路由、解密、Service 投递。 |
| Driver RX Worker（可选） | 解载体编码，形成完整 UCN 帧，送入固定驱动队列。 | 无限等待、动态扩容、执行业务回调。 |
| **Protocol Task（唯一）** | `ucn_adapter_rx_pump()`、`ucn_service_protocol_bridge_step()`、`ucn_node_step()`、HELLO/发现、Link 生命周期。 | 被其他任务并发调用 Node。 |
| Service Task | 通过 Port 包装调用 `ucn_service_send()`/`ucn_service_inbox_take()`；处理自己的 Endpoint。 | 直接持有 `ucn_node_t`、Link 或路由表。 |

Protocol Task 只需要一个；它不是网络中心，其他 MCU 仍各自运行自己的 Protocol Task 和路由。

## 2. 静态对象与锁

```c
#include "ucn/ucn_node_storage.h"

static ucn_node_t g_node;
static ucn_service_router_t g_router;
static ucn_service_protocol_bridge_t g_bridge;
typedef struct product_service_port {
    rtos_mutex_t router_lock; /* 由具体 RTOS 静态初始化。 */
    /* 固定 Service→Task 绑定与 Endpoint 通知对象也属于此 Port。 */
} product_service_port_t;
static product_service_port_t g_service_port;
```

Router 的短临界区保护的是 `ucn_service_send()`、`ucn_service_inbox_take()`、Bridge 的远端 TX 取出和远端投递；不要把锁扩大到 Link 发送、寻路或业务计算。把同一把锁通过 Bridge Hooks 交给 Protocol Task：

```c
static void router_lock(void *context)
{
    rtos_mutex_lock(&((product_service_port_t *)context)->router_lock);
}
static void router_unlock(void *context)
{
    rtos_mutex_unlock(&((product_service_port_t *)context)->router_lock);
}
static void endpoint_ready(void *context, const ucn_frame_t *frame,
                           ucn_result_t result)
{
    if (result == UCN_OK) {
        rtos_notify_endpoint_owner(context, (ucn_endpoint_t)frame->message_type);
    }
}

static const ucn_service_bridge_inbound_hooks_t g_hooks = {
    .context = &g_service_port,
    .lock = router_lock,
    .unlock = router_unlock,
    .observer = endpoint_ready,
};
```

`endpoint_ready()` 必须在锁释放后快速执行，通知只是提示；Payload 的唯一权威副本仍在 Router Inbox。Q1 可能覆盖，因此任务被唤醒后必须循环读取 Inbox 直到空。

## 3. 初始化顺序

在创建任何 RTOS Task 前，由启动线程依次执行 `ucn_node_init()`、`ucn_node_set_wire_profiles()`，可选执行 `ucn_node_set_wire_profile_auto(true)`，然后才注册 Link、安装 Security 和初始化 Router/Bridge。Wire Profile 是 Node 初始化策略，不是每个业务 Task 自己选择的状态。

```c
void ucn_rtos_start(void)
{
    /* 已完成 Node、Link、Adapter RX Queue 的静态初始化。 */
    (void)ucn_service_router_init(&g_router, &g_service_router_config);
    (void)ucn_service_protocol_bridge_init(&g_bridge, &g_router, &g_node);
    (void)ucn_service_protocol_bridge_install_endpoint_handlers(&g_bridge);
    (void)ucn_service_protocol_bridge_set_inbound_hooks(&g_bridge, &g_hooks);

    rtos_task_create_static(protocol_task, "ucn-proto", &g_protocol_stack);
    rtos_task_create_static(sensor_task, "sensor", &g_sensor_stack);
    rtos_task_create_static(control_task, "control", &g_control_stack);
}
```

`g_service_router_config.local_node_id` 必须与 `g_node.config.node_id` 相同。每个 Endpoint 只有一个 Router Binding 所有者，且 Bridge 先占用该 Endpoint handler；不要再用 `ucn_node_set_endpoint_handler()` 为同一 Endpoint 注册第二个业务回调。

## 4. Protocol Task 模板

```c
static void protocol_task(void *argument)
{
    for (;;) {
        uint8_t sent = 0U;
        size_t pumped = 0U;

        drain_driver_rx_to_adapter_queue();
        (void)ucn_adapter_rx_pump(&g_adapter_rx, &g_node, 4U, &pumped);
        (void)ucn_service_protocol_bridge_step(&g_bridge, 2U, &sent);
        (void)ucn_node_step(&g_node, rtos_monotonic_ms());
        rtos_wait_rx_or_timeout(1U); /* 必须有周期唤醒以维护保活/超时。 */
    }
}
```

驱动若在另一任务中已有固定帧队列，`drain_driver_rx_to_adapter_queue()` 由 Protocol Task 单独执行，此时 Adapter RX Queue 可传 `NULL` Port Ops，因为没有并发访问。若必须多上下文访问 Adapter Queue，则使用适合任务上下文的短临界区；不要从 ISR 使用可能睡眠的 mutex。

## 5. Service Task 包装

```c
ucn_result_t product_service_send(ucn_node_id_t destination,
                                  ucn_service_id_t source,
                                  ucn_endpoint_t endpoint,
                                  ucn_traffic_class_t qos,
                                  const uint8_t *payload, uint16_t length)
{
    ucn_result_t rc;
    rtos_mutex_lock(&g_service_port.router_lock);
    rc = ucn_service_send(&g_router, destination, source, endpoint,
                          qos, payload, length);
    rtos_mutex_unlock(&g_service_port.router_lock);
    return rc;
}
```

接收也以同一把短锁调用 `ucn_service_inbox_take()`；产品 Port 应检查“当前 Task 是否为绑定 Service 的唯一消费者”。本机目标不会经过 Link，远端目标进入固定 Remote TX 队列，并在下次 Bridge Step 送入 Core。

## 6. 验收顺序

1. 单 Protocol Task 下以直接 Endpoint API 验证两节点 Q1。
2. 接 Router 后验证本机 Q1 Latest Value 和 Q0 FIFO 满时统计。
3. 验证远端 Q1/Q0 经 Bridge 收发；中继 Node 不应投递无关业务。
4. 强制填满驱动队列、Router Inbox、Remote TX 和事件队列，检查有界丢弃/覆盖和统计，不允许死锁。
5. 最后测栈余量、Heap、断链时 Q0 本地安全、真实时延/丢失/乱序。

## 7. S16 Protocol Task 时限

冻结该 Task 的最低优先级、`UCN_MAX_STEP_INTERVAL_MS`、最大 Queue Wait/Sleep、每轮 Adapter Pump/Bridge 预算和 Link `send()` WCET。不要用无限等待的消息队列驱动 Protocol Task；可用小于最大 Step 的超时或周期唤醒。运行时记录 `max_step_gap_ms`、`step_interval_violations`，并在压力测试中同时检查 Heartbeat/Probe 延迟。
