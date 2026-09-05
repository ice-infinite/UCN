# Zephyr Port

> 文档级别：`NORMATIVE GUIDE`
> 实现状态：通用 wrapper `CURRENT`；Zephyr Driver/Devicetree glue 由产品提供
> 最近核对：`a093862`，2026-08-25

Zephyr Port 暴露 notify/wait wrapper，不把 `k_sem`、`k_thread`、`device` 或 Devicetree 类型带入 Core。

产品建议：

- 建立唯一 UCN thread；
- ISR-safe `k_sem_give`、work submission 或等价方式只负责唤醒；
- thread 使用有界 `k_sem_take`，等待时间不得越过下一个协议 Deadline；
- UART/CAN Driver callback 写独立 Source Ring；
- 使用 Event Runtime 统一多 Source。

`ucn_zephyr_port_thread_step()` 在 thread 上下文推进 Owner，`rx_enqueue(..., from_isr)` 只有在 Port API V2 ISR critical 合同满足时可从 ISR 使用。

Devicetree 节点、pin control、buffer、CAN filter、thread stack 和 Kconfig 由产品工程定义。官方文档不会把 wrapper 描述成完整 Zephyr module/driver。

## 推荐工程结构

```text
app/src/ucn_platform.c     k_sem/thread/clock glue
app/src/ucn_uart_adapter.c UART async callback + Stream Source
app/src/ucn_can_adapter.c  CAN callback + CAN Source
app/src/ucn_task.c         唯一 Owner thread
boards/...overlay          pinctrl、UART/CAN enable
prj.conf                   stack、driver、heap/日志配置
```

UCN 公共源码不包含 `<zephyr/...>`；这些类型只停留在产品 glue。

## notify/wait 映射

`notify_protocol_thread()` 可对静态 `k_sem` 执行 `k_sem_give()`；ISR callback 中使用 Zephyr 允许的 ISR-safe primitive。`wait_for_work(max_wait_ms)` 在 thread 中执行有界 `k_sem_take()`。即使没有通知，也要在 max wait 后返回以推进协议 Deadline。

使用 `k_work` 时要确认 work queue 的优先级和延迟不会让 Owner 饿死；复杂产品更建议独立 thread，而不是与其他慢 work 共用 system workqueue。

## UART/CAN callback 边界

Zephyr async UART 事件可能把一段 buffer 分多次上报，也可能要求及时重新提供 RX buffer。Callback 只把字节送 Stream Ring/更新 DMA buffer并通知；COBS decode 在 UCN thread。

CAN RX callback 把 `struct can_frame` 的必要字段复制到 CAN Source Ring。Bus state/error callback 映射 Link hard state/metrics。不要从 callback 直接调用 Node。

## 时间和电源管理

`now_ms` 应映射到单调 uptime，例如经产品封装的 `k_uptime_get_32()` 语义，并验证休眠期间如何推进。thread wait 与 PM suspend 要保留下一个协议 Deadline 的唤醒源。

## SMP 与锁

Zephyr SMP 下 ISR/thread 可能跨 CPU。普通 `irq_lock` 的范围和 token 恢复要按版本合同实现；不要用一个无 token 回调同时充当 Task/ISR critical。更简单的架构是 Driver Ring 使用 Zephyr 官方并发原语，UCN Source submit 只在 Owner thread。

## 构建与验证

- 把 UCN 作为独立 library/source group链接，保持 Feature 宏与产品 config 一致；
- 用 `west build`/CMake Map 核对实际目标，而非 Host size；
- 测 thread stack、ISR latency、UART buffer starvation、CAN Bus-Off；
- 在 native_sim 可做 glue smoke，但不能替代目标板时序；
- 验证 logging 后端不会和 UCN 使用同一 UART 字节流。

## 常见错误

- 在系统 workqueue 中执行阻塞 Link send；
- 把一次 UART callback 当作一帧；
- `K_FOREVER` 等待而忽略协议 Deadline；
- overlay pin/波特率与 Driver 实际实例不一致；
- 启用多个 thread 同时调用 Node；
- 只因 wrapper 编译通过就宣称 Zephyr Driver 已完成。
