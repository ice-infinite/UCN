# FreeRTOS 接入

> 文档级别：`GUIDE`
> 实现状态：`PARTIAL（通用合同已实现，产品 BSP/SDK 需接入）`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：公共 API、Port/Adapter/Source 实现与测试
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：部分 ESP32-S3 UART/ESP-NOW；其他平台按正文标注

- Owner 使用独立 Task；
- 驱动 ISR 用 `xTaskNotifyFromISR` 或等价机制；
- ring 临界区使用 FreeRTOS task/ISR 各自正确的 mask API；
- 应用任务通过 Queue/Task Notification 提交请求；
- 用 stack high-water 和运行统计验证预算。

FreeRTOS Port 文件只封装 OS 语义，UART/CAN/Wi-Fi 驱动和引脚仍由产品 BSP 配置。首次集成先关闭 Cluster/实验组件，完成 Core 收发后逐项开启。

## 对象与任务

静态产品建议使用 `xTaskCreateStatic()`、静态 Queue/StreamBuffer 和静态 UCN storage。Owner task 的 stack 不能照抄 demo；先用 Host/静态分析估算，再以 `uxTaskGetStackHighWaterMark()` 压测校准。

```c
static void ucn_owner_task(void *arg)
{
    product_ucn_t *u = arg;
    for (;;) {
        ucn_event_runtime_task_cycle(&u->runtime,
                                     UCN_MAX_STEP_INTERVAL_MS,
                                     &u->last_run);
        product_run_transfer_cluster(u);
    }
}
```

## 通知

`notify_owner` 在 task 上下文用 `xTaskNotifyGive()`；ISR 使用 `vTaskNotifyGiveFromISR()` 或 bit notification，并按返回标志 `portYIELD_FROM_ISR()`。多个 Source 应使用 Runtime pending bit，而不是为每个字节发一条 RTOS queue 消息。

## 临界区 v2

普通上下文可以使用 `taskENTER_CRITICAL/taskEXIT_CRITICAL`；ISR 需要 `taskENTER_CRITICAL_FROM_ISR()` 返回 token，并在 `taskEXIT_CRITICAL_FROM_ISR(token)` 恢复。不可把 task API 直接放入 `enqueue_from_isr()`。

在 ESP-IDF 双核 FreeRTOS 上，必须使用适合多核的 portMUX/spinlock 合同；仅禁用当前核中断不一定保护另一核。最简单的产品姿态是让 Driver/Owner 固定在明确核心，并使用 SDK 推荐的 ISR-safe ring。

## 时间和持久化

`xTaskGetTickCount()` 需转换为毫秒并保留 32-bit 回绕；不要使用会被校时调整的 wall clock。Security/Cluster counter/Record 的 Flash/NVS 操作可能耗时，Provider 应异步提交，完成后通知 Owner，且不能在回调内重入。

## 驱动模式

- UART DMA/event queue：ISR/driver task 将 chunk 写 Stream Source，再通知 Owner；
- TWAI/CAN：RX callback 写固定 CAN Source ring；Bus-Off 状态同步到 Source；
- ESP-NOW/Wi-Fi callback：复制完整 packet 到 Adapter Queue，不能保存 SDK 临时指针；
- USB CDC：连接状态变更更新 Link up/down。

## 检查清单

- Owner 唯一、任务优先级与 core affinity 已记录；
- ISR API 全部为 FromISR 版本；
- Queue/ring 满有统计，不 malloc；
- Watchdog 下 Owner 不做阻塞 IO/日志；
- stack high-water、heap minimum、RX overflow、p99 wake latency 已压测；
- 关闭 Wi-Fi 或 NVS 失败时能保持 fail-closed。
