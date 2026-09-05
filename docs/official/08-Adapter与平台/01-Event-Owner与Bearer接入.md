# Event Owner 与 Bearer 接入

## 1. 统一边界

UART、Wi-Fi/ESP-NOW、CAN/CAN-FD、USB 的共同模型是：驱动/ISR 产生事件，固定 Ring 保存原子
item，通知 Protocol Owner，Owner 在任务上下文完成解析、路由和业务分派。轮询只用于没有
中断的平台或保底 timer，不是有数据后等待心跳发送。

一个节点可以注册多条 Link，同种 Bearer 也可注册多实例；每个实例拥有独立 Link ID、
Instance Generation、MTU、Metric、队列和 Driver context。重开 Link 必须推进 Generation，
旧 RX/TX completion、timestamp key 和 reservation 随即失效。

## 2. Adapter 生命周期

1. `ucn_v6_adapter_init_in_place()` 核对 Storage、Manifest、Runtime callbacks；
2. `ucn_v6_adapter_register_link()` 注册每条实例；
3. ISR/driver 用 `ucn_v6_adapter_publish_rx()` 发布完整 frame item；
4. Owner 通过 `peek_rx()` 读取，处理成功后 `retire_rx()`；
5. 发送方 `enqueue_tx()`，Owner/driver 调用 `service_tx()`；
6. Driver 用 `publish_tx_completion()` 返回结果；Owner peek/retire completion；
7. 取消、断链或 reopen 都必须保留 token 所有权，直到 Driver 确认退休。

队列是有界的。满载必须返回 backpressure，并通过统计暴露；不得覆盖尚未退休 item。同步
Driver 回调也要经过 shared gate，防止 submit 尚未返回时递归推进第二个对象。

## 3. Bearer 配置

`ucn_v6_uart_link_config_init()`、`ucn_v6_esp_now_link_config_init()`、
`ucn_v6_can_link_config_init()` 和 `ucn_v6_usb_link_config_init()` 生成协议侧配置，不负责调用
芯片 SDK。产品仍需提供真正的 open/close/submit/cancel、ISR/DMA、缓存一致性和时钟。

Classic CAN 需要 Carrier 分片，CAN-FD 要验证 padding；Stream/UART 需要 framing、转义或长度
边界；USB 和 Wi-Fi 仍要处理 completion、断链和重连。Carrier MTU 与 UCN Frame MTU 必须
分别配置。

## 4. FreeRTOS 与 ESP32-S3

FreeRTOS Port 封装 Owner 通知、等待和运行预算。ISR 路径只能使用 ISR-safe API 与非阻塞
try-lock，任务路径可以使用任务锁；二者共享的 gate 必须由产品正确实现。ESP32-S3 参考层
能构造 UART/ESP-NOW binding，但引脚、UART 端口、波特率、Wi-Fi channel、队列长度和 task
priority 仍是产品 Manifest/SDK 配置。

## 5. 其他 RTOS/MCU

Zephyr、NuttX、RT-Thread 或裸机移植不应修改 Core。新平台实现同一 Runtime/Owner/Adapter
合同：原子事件、通知、单 Owner、单调时钟、callback gate、固定 Storage 和明确错误返回。
不能把平台全局变量塞回通用协议，也不能从中断直接调用完整协议状态机。

## 6. 实机验收

每个 Bearer 至少验证长度边界、队列满、同步/异步 completion、取消失败重试、断链/reopen、
迟到事件、并发任务/ISR、1～多跳吞吐、P99/P999 延迟、CPU/栈/RAM 与长稳。Host Fake
Adapter 通过只代表接口状态机，不代表真实外设完成。
