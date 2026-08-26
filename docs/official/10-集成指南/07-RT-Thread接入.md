# RT-Thread 接入

> 文档级别：`GUIDE`
> 实现状态：`PARTIAL（通用合同已实现，产品 BSP/SDK 需接入）`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：公共 API、Port/Adapter/Source 实现与测试
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：部分 ESP32-S3 UART/ESP-NOW；其他平台按正文标注

创建一个 UCN Owner 线程和 event/semaphore。串口接收回调只读取 DMA/ring 状态并发送事件，Owner 调用 Stream Source service。CAN 接收同样先进入固定 ring。

使用 RT-Thread tick 转换为单调毫秒时要处理 tick 频率和回绕。对象、线程栈和 ring 大小写入产品配置，不依赖默认 demo 值。

## 推荐映射

| UCN | RT-Thread |
| --- | --- |
| Owner | 独立 `rt_thread` |
| 唤醒 | `rt_event`/`rt_sem` |
| Driver ring | `rt_ringbuffer` 或静态 DMA ring |
| 时间 | `rt_tick_get()` 按 `RT_TICK_PER_SECOND` 转 ms |
| UART | device rx_indicate/DMA callback |
| CAN | device RX callback + CAN Source |

通知回调只置 Runtime pending bit并 release event/semaphore。不要在 `rx_indicate` 中循环 `rt_device_read()` 到空并解析 COBS；Owner 再按预算读取。

## 初始化

1. `rt_device_find/open/control` 配置 UART/CAN；
2. 分配静态 UCN/Source/Runtime 对象；
3. 初始化产品 Port 和 Node；
4. 注册 receive callback；
5. 创建并启动 Owner thread；
6. 设置 Link up 后开始 HELLO/业务。

如果使用 FinSH/日志同一 UART，必须在物理/Carrier 层明确复用协议；不能把文本日志插进 UCN COBS 字节流。

## 临界区与 SMP

单核可用短 irq disable 或 RT-Thread spinlock API保护 ring index；SMP 产品需要真正跨核保护。应用任务通过 mailbox/mq/Service 与 Owner 通信，不共享 Node。

## 时间转换

避免整数截断长期累积误差：使用 `(uint64_t)tick * 1000 / RT_TICK_PER_SECOND` 再截成 32-bit。所有模块引用同一转换函数。wall clock/NTP 不用于 deadline。

## 验收

验证 RX callback 突发、event 合并、ring overflow、线程 starvation、tick 回绕、设备 close/reopen、stack watermark 和 heap。BSP 中没有的 CAN-FD/USB 能力必须标为未验证，不因 Port 文件存在而宣称支持。
