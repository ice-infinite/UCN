# 通用 RTOS 对接流程

> 文档级别：`GUIDE`
> 实现状态：`PARTIAL（通用合同已实现，产品 BSP/SDK 需接入）`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：公共 API、Port/Adapter/Source 实现与测试
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：部分 ESP32-S3 UART/ESP-NOW；其他平台按正文标注

创建唯一 UCN Owner Task，优先级足以满足 `UCN_MAX_STEP_INTERVAL_MS`。各驱动 ISR/DMA completion 将数据写入专用 ring，使用 ISR-safe notify 唤醒 Owner。

Owner 每次醒来按预算处理多个 Source，然后推进 Node、Transfer、Service 和可选 Cluster；剩余工作再次自通知，避免单次独占 CPU。应用任务通过队列/Service 与 Owner 交换命令和结果。

不要让多个任务同时调用 Node，也不要用一个无 token 的锁同时覆盖 task 与 ISR 上下文。

## 推荐任务结构

```text
高优先级 Driver ISR/DMA callback
      │ push/signal only
      ▼
UCN Protocol Owner Task
      ├─Source service
      ├─Adapter RX pump
      ├─Node step/RX
      ├─Service Bridge
      ├─Transfer step
      └─可选 Cluster/Federation
      │
      ├─Service inbox → Sensor/Control Task
      └─Command queue ← Application Tasks
```

Owner 优先级应高于普通业务/日志，低于真正硬实时 ISR；任务不能被低优先级 mutex 长期反转。业务任务不直接持 Node mutex，而是把请求放入固定队列并通知 Owner。

## Scheduler glue

实现 `notify_owner(context,from_isr)`、`wait_owner(context,max_wait_ms)` 和可选 `yield_owner()`。ISR 版本必须使用 RTOS 的 `FromISR`/irq-safe API，并在需要时触发 context switch。

Owner 主循环可直接使用 `ucn_event_runtime_task_cycle()`：已有 pending 不等待；无 pending 时带超时等待；超时做 fallback scan 并推进维护。

## Source 与预算

每个 UART/CAN/Wi-Fi 实例分配 source ID。Source service 一次最多处理 `max_work`，返回 `pending_events` 让 Runtime 继续下一轮。高速 Source 不能一口气 drain 全部数据，否则慢速 CAN 的 Q0 可能长期得不到处理。

预算耗尽时 Runtime 可 yield，再立即自通知。观察 `drain_budget_hits`，若长期触发，需要提高 Owner CPU/优先级、减少日志、增加 DMA/ring 或调整 per-source budget。

## 应用通信

- 周期传感器：Service Q1 Latest，任务只取最新样本；
- 关键命令：Service Q0 FIFO + Guard + Result；
- 大块日志/参数：Transfer，低优先级策略；
- 状态查询：只读 snapshot，避免应用直接访问 Owner 对象。

## 停机/重启

先停止应用新请求，然后禁用 Driver RX/IRQ，唤醒 Owner drain 或 Abort 有界事务，标记 binding not-ready，关闭 Link，最后删除任务/对象。任务被直接 kill 可能留下 DMA callback 指向旧 storage。

## 验证

覆盖 task/ISR 同时入队、通知丢失后的 fallback、优先级反转、Owner 被阻塞、Source 风暴、队列满、任务重启、tick 回绕。用 trace 记录 p99 Owner wake latency和 task stack high-water。
