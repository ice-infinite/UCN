# Port、Event Runtime、Owner API

> 文档级别：`REFERENCE`
> 实现状态：`CURRENT`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：include/ucn 公共头、链接目标与公共 API 测试
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：API 合同不等同于各平台实机驱动完成

Port API v2 把临界区、时间、通知和任务/ISR 差异拆开。ISR 专用入口不能复用需要 task token 的锁；推荐 ISR 写 BSP ring，再通知唯一 Owner。

Event Runtime 汇总多个 Source 的待处理事件，Owner 被通知后按预算 drain Source、提交 Adapter Queue、推进 Node/Transfer/Service。轮询只用于裸机主循环或中断丢失保底。

FreeRTOS、Zephyr、NuttX、RT-Thread 文件分别提供平台壳层，不改变通用 Runtime 合同。

## Port API v2

`ucn_port_ops_t` 提供同一平台的基础能力：

- `now_ms`：单调 32-bit 毫秒时钟；
- `random_bytes`：安全/挑战所需随机源；
- `load_counter/store_counter`：持久化序列或 session counter；
- task `enter/exit_critical`：普通上下文临界区；
- ISR `enter/exit_critical_from_isr`：返回/恢复平台 token。

API v2 是破坏性升级，调用者应使用具名初始化并设置版本/结构尺寸合同，不能继续依赖旧位置初始化。`ucn_port_ops_is_compatible()` 只能证明结构/API 兼容，不能证明随机数或 Flash 实现安全。

## Protocol Owner

Owner 统一串行化 Node RX、step、Service Bridge 等协议动作。初始化时绑定 Node、Adapter Queue、可选 Service Bridge、Port 和工作预算。Packet Driver 可调用：

```c
ucn_protocol_owner_rx_enqueue_from_isr(&owner, link, data, length);
```

Owner task 再调用 `ucn_protocol_owner_step()`。不要从应用任务直接调用 `ucn_node_step()`，同时又让 Owner 调用同一 Node。

## Event Runtime

Event Runtime 在 Owner 外增加多个 Source 与调度器适配：

```text
UART RX IRQ ─┐
CAN RX IRQ  ─┼→ signal_source_from_isr()
USB/Wi-Fi   ─┘        ↓
                  notify_owner()
                       ↓
                task_cycle()/run()
                  ├─有界 service Source
                  ├─提交完整 frame
                  ├─pump RX Queue
                  └─推进 Node/Bridge
```

Source 的 `service(context,events,max_work,result)` 只在 Owner 中运行，返回实际工作量和仍 pending 的事件。Runtime 通过 `max_drain_rounds × max_source_work_per_round` 限制单轮工作，预算耗尽时 yield 并保留 pending。

Packet-style Driver 已有完整 frame 时可直接 `submit_frame[_from_isr]()`；Stream/Classic CAN 通常只 signal Source，让 service 解码/重组。

## RTOS scheduler hooks

不同系统只实现三件事：通知 Owner、带超时等待、可选 yield。FreeRTOS 常用 task notification，Zephyr/NuttX/RT-Thread 可用 semaphore/event。`task_cycle()` 在已有 pending 时不等待；没有事件时最多等待 `min(requested, UCN_MAX_STEP_INTERVAL_MS)`，超时执行一次 fallback scan。

## 裸机

裸机主循环调用 `has_pending()`/`run()`，无任务时可由产品执行 WFI。轮询是中断丢失或无 IRQ 平台的保底，不能以高频扫描代替正常中断通知。

## 并发禁令与诊断

- ISR 不解析 Frame、不调用 Endpoint handler、不做路由；
- Source service 不无限 drain；
- Provider/应用 callback 不递归进入 Owner；
- task 与 ISR 临界区不得错误共用无 token 锁；
- 观察 `drain_budget_hits`、fallback scans、frames rejected、wait timeouts，持续异常说明预算或驱动有问题。
