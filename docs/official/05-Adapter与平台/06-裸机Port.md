# 裸机 Port

> 文档级别：`NORMATIVE GUIDE`
> 实现状态：`CURRENT`
> 事实源：`ucn_port_bare_metal.h/.c`
> 最近核对：`a093862`，2026-08-25

裸机 Port 把 Protocol Owner 放入产品 superloop。最小流程：

```text
初始化时钟/Driver/Node/Link/Port
for (;;) {
    处理硬件事件或 ISR Ring
    ucn_bare_metal_port_poll(...)
    执行业务任务
    进入可中断低功耗等待
}
```

单 Queue 兼容 API 提供 RX enqueue 和 poll。多 Bearer 裸机产品可直接使用 Event Runtime，不设置 scheduler hooks，由同一 superloop 运行。

## 中断

ISR 只写固定 Ring/Queue。若使用 Adapter 的 `from_isr` 路径，必须提供正确的 ISR critical token callbacks。

## 时间

`now_ms` 必须单调并持续推进。superloop 最坏执行时间不能让 `ucn_node_step()` 超过最大间隔；长业务运算应拆分或放入硬件/协处理器。

## 适用范围

适合简单 STM32/AVR/RP2040 等无 RTOS 产品。是否能装下取决于选用 Profile、表大小、Transfer/Cluster 是否链接和目标 linker map，不由“裸机”本身决定。

## 推荐初始化顺序

1. 初始化系统时钟和单调 `now_ms`；
2. 初始化 UART/CAN/USB 等 Driver 和静态 Ring；
3. 初始化 Source/Adapter/Link；
4. 初始化 Node、注册 Link/Endpoint；
5. 初始化 bare-metal Port 或 Event Runtime/Owner；
6. 打开 Driver 中断并将 Link 标为可用；
7. 进入 superloop；
8. 业务任务只通过 Service/请求队列与 Owner 交互。

在 Node/Source 尚未准备时就打开 RX 中断，可能让 ISR 写入未初始化 Ring。

## Superloop 如何安排预算

```c
for (;;) {
    service_fast_hardware();
    (void)ucn_bare_metal_port_poll(&ucn_port, &pumped, &bridged);
    run_bounded_sensor_work();
    run_bounded_control_work();
    enter_sleep_until_event_or_protocol_deadline();
}
```

每个业务函数必须有界。一次 Flash 擦除、阻塞 I2C 或长浮点计算若让 loop 停 100 ms，Heartbeat、ACK 和 Route Deadline 都会一起延迟。长操作应拆成状态机、DMA 或明确在产品时延预算中处理。

## ISR 与主循环共享 Ring

单生产者 ISR/单消费者 Owner 仍需正确的内存可见性和临界区。若使用 UCN 通用 from_isr Queue，必须提供 ISR token pair；若使用产品 SPSC Ring，也要保证 index 原子性/屏障，不能因“没有 RTOS”就假设编译器不会重排。

ISR 内禁止：COBS decode、CRC、路由查找、Service Handler、阻塞发送和等待 ACK。

## 低功耗等待

进入 WFI/STOP 前要计算下一个协议 Deadline，并确保 UART/CAN/RTC 能唤醒。睡眠暂停 `now_ms` 的平台不能直接把该时钟当单调协议时间；应使用睡眠期间仍推进的 timer，或恢复时补偿经过时间。

## 单 Queue 与多 Source 选择

- 一个简单 UART：`ucn_bare_metal_port_rx_enqueue()` + `poll()` 足够；
- 多 UART/CAN/USB：每介质 Source + Event Runtime 更清晰；
- 多核 MCU：仍只选一个 Core/上下文作为 Node Owner，另一核通过固定队列提交。

## 资源和栈

裸机没有 RTOS Task 栈，但 ISR 栈/主栈仍需测量。Node、Ring、Transfer RX、Service Inbox 等最好静态放在 BSS；大对象不要作为函数局部变量放栈。

## 验证清单

- [ ] 启动时中断不会访问未初始化对象；
- [ ] 最坏 superloop 周期小于协议/业务预算；
- [ ] `now_ms` 在回绕和低功耗期间保持合同；
- [ ] Ring 竞态和 overflow 有压力测试；
- [ ] 多 Source 公平且只有一个 Owner；
- [ ] linker map、主栈/ISR 栈和静态 RAM 有目标板证据。
