# RT-Thread Port

> 文档级别：`NORMATIVE GUIDE`
> 实现状态：通用 wrapper `CURRENT`；RT-Thread BSP glue 由产品提供
> 最近核对：`a093862`，2026-08-25

RT-Thread 有独立 Port，不需要伪装成 FreeRTOS/Zephyr/NuttX 模式。

产品把 notify/wait 映射到 event、mailbox、semaphore 或线程通知，保证：

- ISR 路径只发送事件/写 Ring；
- 唯一 protocol thread 调用 `thread_step()`；
- wait 最长时间不越过协议定时器；
- Task/ISR critical 使用各自合法 API；
- Source storage、Link 和回调上下文静态有效。

多 Source 产品优先 Event Runtime，single-Queue wrapper 保留简单接入。

设备查找、serial/can device open、rx_indicate、pin、DMA、thread priority/stack 由 BSP/产品实现并实测。

## 推荐线程模型

创建一个静态 UCN protocol thread，持有 Port/Event Runtime/Node。UART `rx_indicate`、CAN callback 或无线 callback 只向各自 Source Ring 写数据并通过 event/semaphore 唤醒。

```text
driver callback -> source ring -> rt_event_send/sem_release
protocol thread -> wait bounded -> source service -> node step
business thread -> Service/request queue -> protocol thread
```

mailbox 若只传事件/索引可以使用；不要为每个最大 Frame 动态 `rt_malloc` 再传指针，除非产品明确管理 pool 和所有权。

## notify/wait 映射

`notify_protocol_thread(context, from_isr)` 映射到 ISR-safe event/semaphore release；`wait_for_work(max_wait_ms)` 把毫秒安全转换为 RT-Thread tick，并确保向上取整/0 tick 不造成忙循环或越过 Deadline。

ISR 路径不得调用可能调度/阻塞的普通线程 API，具体允许列表以当前 RT-Thread 版本为准。

## 设备驱动边界

- `rt_device_find/open/control` 在初始化阶段完成；
- Serial callback 只表示“有字节”，不能把一次 indication 当完整 UCN Frame；
- Owner thread 从 Driver/Ring 读取后做 COBS；
- CAN filter/bitrate/Bus-Off recovery 由 BSP glue；
- Device suspend/reconfigure 前先标 Link Down/Fence 旧能力。

## 时间和 tick

若 `RT_TICK_PER_SECOND` 不能精确表示 1 ms，产品 `now_ms` 转换必须单调并处理回绕；不能在每次转换中因整数截断让时间停住/倒退。等待上界同样要验证 tick rounding。

## 与 FinSH/日志共口

默认 console/FinSH 常占用一个串口。不要让人类 shell 文本和 COBS UCN 共用同一无复用字节流。选择独立 UART/USB，或关闭该口 console 并把日志重定向。

## 验证清单

- [ ] thread/event/semaphore 使用静态或受控内存；
- [ ] ms↔tick 转换不越过协议 Deadline；
- [ ] rx_indicate 只通知，不进入 Core；
- [ ] console 与 UCN 通道隔离；
- [ ] 多设备 Source 独立并由一个 thread 公平处理；
- [ ] 目标 BSP 的 stack、CPU、DMA、Bus-Off/重连完成实测。
