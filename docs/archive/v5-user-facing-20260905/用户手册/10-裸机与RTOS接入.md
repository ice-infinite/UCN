# 裸机与 RTOS 接入

UCN把平台差异限制在Port和调度外壳中。产品只链接一个Port Target，也可以直接使用通用Event Runtime。

## 1. 可选Port Target

```cmake
target_link_libraries(my_firmware PRIVATE
    ucn_core
    ucn_port_freertos)
```

可选目标：

- `ucn_port_bare_metal`
- `ucn_port_freertos`
- `ucn_port_zephyr`
- `ucn_port_nuttx`
- `ucn_port_rtthread`
- `ucn_port_host_fake`

这些Port不会引入完整厂商工程，只提供调度/Owner包装。GPIO、UART/CAN和SDK仍在产品BSP。

## 2. 裸机

```text
IRQ/DMA callback
 → 写BSP Ring
 → signal pending

Superloop
 → drain Source
 → pump RX
 → Node/Service/Transfer step
 → WFI
```

单队列兼容Port：

```c
ucn_bare_metal_port_t g_port;

check(ucn_bare_metal_port_init(&g_port, &owner_config));

for (;;) {
    size_t pumped = 0U;
    uint8_t bridged = 0U;
    ucn_result_t result = ucn_bare_metal_port_poll(
        &g_port, &pumped, &bridged);
    product_step_transfer_if_idle(result);
    product_wait_for_interrupt_bounded();
}
```

多Bearer裸机推荐直接用Event Runtime并省略scheduler hooks。

## 3. FreeRTOS

映射：

```text
notify_protocol_task(false) → xTaskNotifyGive(owner)
notify_protocol_task(true)  → vTaskNotifyGiveFromISR(owner,...)
wait_for_work(ms)           → ulTaskNotifyTake(...)
```

初始化兼容Port：

```c
ucn_freertos_port_config_t config = {
    .owner = owner_config,
    .ops = &product_freertos_ops,
    .runtime_context = &owner_task_context
};

check(ucn_freertos_port_init(&g_freertos_port, &config));
```

Owner Task：

```c
for (;;) {
    size_t pumped = 0U;
    uint8_t bridged = 0U;

    (void)ucn_freertos_port_task_wait(
        &g_freertos_port, PRODUCT_OWNER_WAIT_MS);
    (void)ucn_freertos_port_task_step(
        &g_freertos_port, &pumped, &bridged);
    product_step_transfer();
}
```

Driver ISR只调用 `ucn_freertos_port_rx_enqueue(..., true)` 或Source ISR接口并notify。不能从ISR执行Node。

## 4. Zephyr、NuttX、RT-Thread

三者流程相同：

| 平台 | Owner等待原语示例 | ISR唤醒 |
| --- | --- | --- |
| Zephyr | `k_sem_take`/event | `k_sem_give` |
| NuttX | semaphore/message queue | irq-safe post |
| RT-Thread | event/semaphore | ISR-safe send/release |

使用对应头文件中的具名Port config/ops，映射本平台通知、等待和时间。不要把RTOS类型放进通用Link/Source实现；厂商或OS依赖留在产品Port文件。

## 5. Owner任务优先级

推荐：

```text
硬实时ISR
  > UCN Owner Task
  > 普通业务任务
  > 日志/后台任务
```

Owner不能被低优先级mutex长期阻塞。业务handler只投递自己的任务队列，避免在Owner里执行慢操作。

## 6. 多Source公平性

Event Runtime用：

- `max_drain_rounds`限制一次醒来最多轮数；
- `max_source_work_per_round`限制每个Source工作量；
- budget耗尽后yield并重新通知；
- timeout fallback扫描弥补丢通知。

持续出现 `drain_budget_hits` 表示Owner处理能力不足、Source预算不合适或Driver/日志负担过高。

## 7. 临界区

Port API v2区分Task和ISR临界区，并保存/恢复ISR mask token。要求：

- enter/exit必须成对；
- 临界区只保护index、count和短copy元数据；
- 不在锁内解码Frame或调用Link send；
- 多核平台需要正确原子、memory barrier或spinlock；
- 旧的无token六字段Port初始化不能继续照搬。

## 8. 无中断介质

确实无法中断时，可以定期poll，但仍通过同一Source→Adapter→Owner链提交。轮询是保底或平台约束，不应让已有可靠中断的高速接口退回高频全扫描。

## 9. 平台验收

- Task/ISR同时入队；
- ISR临界区token正确恢复；
- 通知发生在Owner等待前/后均不丢工作；
- Owner timeout/fallback可恢复；
- 高速Source不饿死CAN Q0；
- Task Stack high-water、p99 wake latency和CPU达标；
- Driver重启/任务重启后无旧callback；
- Tick回绕、长时间空闲后立即发送；
- Release构建和目标板压力通过。

现场排查继续阅读 [诊断与故障排查](11-诊断-状态查询与故障排查.md)。
