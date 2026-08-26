# FreeRTOS Port

> 文档级别：`NORMATIVE GUIDE`
> 实现状态：通用 wrapper `CURRENT`；具体 ESP-IDF/FreeRTOS 工程 glue 由产品提供
> 最近核对：`a093862`，2026-08-25

FreeRTOS Port 不包含 FreeRTOS 头。产品把：

- Task 通知映射到 `notify_protocol_task`；
- ISR 通知映射到 `vTaskNotifyGiveFromISR` 或等价静态机制；
- `wait_for_work` 映射到有界 `ulTaskNotifyTake`/semaphore wait；
- Task/ISR critical 分别映射到对应 API，并正确传递 ISR mask token。

## 推荐结构

```text
高优先级唯一 UCN Protocol Task
  wait(next protocol deadline)
  event_runtime_run / port_task_step

UART/CAN/Wi-Fi ISR或Driver callback
  push fixed Source Ring
  notify Protocol Task

业务 Tasks
  Service/Endpoint API 或自己的 Task Queue
```

业务 Task 不直接调用 Node Step/Receive，不把 Core 放进 ISR。

兼容 wrapper 适合单 Queue；多 UART/CAN 产品优先 Event Runtime。

任务栈、优先级、core affinity、watchdog 和 ESP-IDF Driver handle 都属于产品配置，当前仓库没有一个可直接烧录所有 ESP32 的通用 FreeRTOS 工程。

## 对接回调的最小语义

`notify_protocol_task(context, from_isr)` 根据上下文选择 Task 或 ISR-safe 通知；`wait_for_work(context, max_wait_ms)` 只能在 Protocol Task 中调用，并且最多等待传入时长。

示意映射：

```c
static void notify_ucn(void *ctx, bool from_isr)
{
    TaskHandle_t task = ((app_ctx_t *)ctx)->ucn_task;
    if (from_isr) {
        BaseType_t wake = pdFALSE;
        vTaskNotifyGiveFromISR(task, &wake);
        portYIELD_FROM_ISR(wake);
    } else {
        xTaskNotifyGive(task);
    }
}
```

代码仅说明语义，具体 API 随 FreeRTOS/ESP-IDF 版本和配置核对。`wait_for_work` 不得把 `portMAX_DELAY` 无条件使用，因为 Protocol Owner 还有内部 Deadline。

## Protocol Task 主循环

```text
初始化Port/Runtime完成
while running:
    task_step 或 event_runtime task_cycle
    若仍有pending：继续推进/让出
    否则 task_wait(计算后的有界时间)
    定期喂符合产品策略的 watchdog
```

Task 优先级要高于普通日志/低优先级业务，低于必须抢占的硬实时控制视产品而定。不能让业务 Task 在高优先级忙循环中永久饿死 UCN Owner。

## ESP32 双核注意事项

可将 UCN Task 固定到一个 Core，但 UART/Wi-Fi callback 可能在另一 Core 运行。Driver Ring/通知和 Task/ISR critical 必须支持跨核可见性；禁用单核中断并不自动构成跨核锁。产品优先使用 SDK 官方 queue/ringbuffer/critical API，并实测 core affinity 和 Wi-Fi 任务竞争。

## Service 与业务 Task

业务 Task 不直接调用 `ucn_node_step/receive`。可通过 Service Router 的串行化入口、产品请求 Queue 或由 Owner 安装的 Endpoint Handler。目标 Task ready=false 时清 Inbox，重启后再 ready。

## Queue 与内存选择

UCN 内部已有固定队列时，不必为每一层再套一个动态 FreeRTOS Queue。Driver→Source 可用 DMA Ring + notify；业务→Owner 请求可用静态 Queue。所有 Queue 长度、item 最大尺寸和 Task 栈进入产品 RAM 总账。

## Watchdog 与阻塞 Driver

Link send 应快速排队或返回，不能在 Protocol Task 内等待整帧 UART 发完、Wi-Fi ACK 或 CAN mailbox 无限可用。阻塞会同时拖住所有 Link 和 Deadline。Driver busy 应返回背压，由 TX complete 再通知 Owner。

## 验证清单

- [ ] Task/ISR notify 使用正确 API 和 yield；
- [ ] Task critical 与 ISR mask-token critical 独立成对；
- [ ] 双核 callback/Owner 竞态有压力测试；
- [ ] wait 不越过协议 Deadline；
- [ ] Link send 不长时间阻塞 Owner；
- [ ] UCN Task stack high-water、CPU、watchdog 和队列峰值已测；
- [ ] 多 UART/CAN/Wi-Fi 由一个 Owner/Event Runtime 公平推进。
