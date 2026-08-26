# Zephyr 接入

> 文档级别：`GUIDE`
> 实现状态：`PARTIAL（通用合同已实现，产品 BSP/SDK 需接入）`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：公共 API、Port/Adapter/Source 实现与测试
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：部分 ESP32-S3 UART/ESP-NOW；其他平台按正文标注

Zephyr 中可用单独 thread 作为 Owner，ISR/DMA callback 写 ring 后通过 semaphore/event 唤醒。时钟使用单调 uptime，临界区选择适合 ISR 的 spinlock/irq lock 合同。

设备树负责 UART、CAN、USB 和无线实例；UCN 只接收已经初始化好的设备上下文。不要在 UCN 通用 Port 中写死 devicetree alias 或 GPIO。

构建时保证 Zephyr application 与 UCN library 看到相同 Profile/配置宏。

## 推荐映射

| UCN 抽象 | Zephyr 设施 |
| --- | --- |
| Owner | `k_thread` / cooperative 或合适优先级 preemptive thread |
| 通知/等待 | `k_sem`、`k_event` 或 `k_poll` |
| ISR ring | `ring_buf` 或产品固定 DMA ring |
| 时间 | `k_uptime_get_32()` |
| 临界区 | `irq_lock/unlock` 或 `k_spin_lock/unlock`，按 SMP/ISR 选择 |
| UART/CAN | device + async API/callback |

Port shell 提供通用等待/step 函数，但 board overlay、devicetree chosen/alias 和 pinctrl 属于应用。

## 初始化顺序

1. 用 devicetree 获取并 `device_is_ready()` 检查设备；
2. 配置 UART async RX/CAN filter/USB enable；
3. 初始化 UCN storage、Port、Runtime、Source/Link；
4. 创建 Owner thread；
5. 最后启动 RX callback 和网络 Join。

回调中 SDK buffer 的生命周期通常有限，必须在返回前复制或只提交 DMA index。

## Kconfig/CMake

产品配置应通过一个受版本控制的头和 target definitions 传播。不要用 Zephyr Kconfig 设置一份 `UCN_PROFILE`，又在外部 CMake 设置另一份。构建后检查 `compile_commands.json` 和 map。

## Zephyr 特有注意

- `k_uptime_get_32()` 回绕由 UCN time helper处理；
- SMP 下 irq lock 只影响本核时，应使用 spinlock 或 single-owner无共享设计；
- system workqueue 不适合作为可能持续高负载的唯一 Owner，避免阻塞其他系统 work；
- logging deferred thread 也会消耗 CPU/RAM，性能测试要分别开关日志；
- USB CDC ACM 需要处理 DTR/枚举变化和断连半帧。

## 验收

用 native_sim/Host 做基础接口测试，再在目标板跑 UART/CAN chunk、断链、queue full、thread starvation、stack analyzer 和 power management resume。设备 suspend/resume 后必须重新确认 Link status/MTU。
