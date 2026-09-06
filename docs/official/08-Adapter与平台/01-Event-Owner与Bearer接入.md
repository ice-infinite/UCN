# Event Owner 与 Bearer 接入

## 1. 统一边界

UART、Wi-Fi/ESP-NOW、CAN/CAN-FD、USB 的共同模型是：驱动/ISR 产生事件，固定 Ring 保存原子
item，通知 Protocol Owner，Owner 在任务上下文完成解析、路由和业务分派。轮询只用于没有
中断的平台或保底 timer，不是有数据后等待心跳发送。

一个节点可以注册多条 Link，同种 Bearer 也可注册多实例；每个实例拥有独立 Link ID、
Instance Generation、MTU、Metric、队列和 Driver context。重开 Link 必须推进 Generation，
旧 RX/TX completion、timestamp key 和 reservation 随即失效。

Link ID 的合法域为 `1..65534`，`0/65535` 均不得注册。多 Link TX 调度只在当前 READY 的
Link 中选择最早排队项；一个 Offline/Faulted Link 上保留的待取消项不能阻塞其他健康 Link。
若队列内部引用不存在的 Link 或错误 Generation，则属于对象不变量损坏并进入 fail-closed，
不能把这种损坏与正常断链混为一类跳过。

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

## 3. 标准 Runtime Owner

启用 Adapter 的产品必须以 `ucn_v6_runtime_owner_t` 作为协议编排入口，而不是自行拼接一套
Stack Owner hooks。典型初始化顺序为：

1. 分别初始化 Bootstrap、Security、Capability、Route、Metric、QoS、Transfer 及可选
   Realtime/Cluster Owner；
2. 初始化并注册 Adapter Link；
3. 用上述 Owner 和两个必要应用回调构造 `ucn_v6_runtime_config_t`；
4. 调用 `ucn_v6_runtime_init_in_place()`；
5. 调用 `ucn_v6_runtime_make_stack_hooks()`，把返回的唯一 hooks 交给 Stack Owner；
6. Driver 事件通知到达后立即运行 Stack Owner；Timer 只补 Deadline 和漏通知。

Runtime 保证 RX 只有在应用明确返回 CONSUMED/DROP 后才退休；RETRY 保留同一原子 RX 项。
TX completion 先从 Adapter 精确退休，再进入固定 release retry 槽；即使应用暂时不能接收
Buffer token，也不会让 Driver reservation 与业务所有权混淆。依赖失效按固定顺序扇出到
Security、Capability、Route、QoS、Transfer、Realtime、Cluster 和最终 Endpoint 回调。

Realtime 同步由 Runtime 持有完整事务。Master 以 `ucn_v6_runtime_time_start_sync()` 构造、
保护和排队 `TIME_SYNC`；Member 必须在精确 `TIME_SYNC` RX 的 `handle_ingress()` 动态范围内
调用 `ucn_v6_runtime_time_observe_sync()`，取得仅对当前 Runtime 实例有效的 opaque handle，
再交给 `ucn_v6_runtime_time_send_delay_request()`。Master 对当前认证请求调用
`ucn_v6_runtime_time_respond_delay_request()`，Member 对当前认证响应调用
`ucn_v6_runtime_time_complete()`。T2/T4 来自对应的原子 RX item，T1/T3 只接受 Runtime 自己
创建的 Adapter reservation 的物理 TX completion；应用不能传入任意 event key。正向和反向
Path 分别冻结并可使用不同 ID，不能用一方的 ACK/时间戳证明另一方向。

Runtime 自身同样使用 caller-owned 固定 Storage；表满、重叠输入、错误 Link Generation、
丢失退休义务或零进度 backlog 都失败关闭。它不替应用决定 Endpoint 业务语义，也不替代
密码 Provider、Flash Provider 或真实 Bearer Driver。

## 4. Bearer 配置

`ucn_v6_uart_link_config_init()`、`ucn_v6_esp_now_link_config_init()`、
`ucn_v6_can_link_config_init()` 和 `ucn_v6_usb_link_config_init()` 生成协议侧配置，不负责调用
芯片 SDK。产品仍需提供真正的 open/close/submit/cancel、ISR/DMA、缓存一致性和时钟。

Classic CAN 需要 Carrier 分片，CAN-FD 要验证 padding；Stream/UART 需要 framing、转义或长度
边界；USB 和 Wi-Fi 仍要处理 completion、断链和重连。Carrier MTU 与 UCN Frame MTU 必须
分别配置。

## 5. FreeRTOS 与 ESP32-S3

FreeRTOS Port 封装 Owner 通知、等待和运行预算。ISR 路径只能使用 ISR-safe API 与非阻塞
try-lock，任务路径可以使用任务锁；二者共享的 gate 必须由产品正确实现。ESP32-S3 参考层
能构造 UART/ESP-NOW binding，但引脚、UART 端口、波特率、Wi-Fi channel、队列长度和 task
priority 仍是产品 Manifest/SDK 配置。

## 6. 其他 RTOS/MCU

Zephyr、NuttX、RT-Thread 或裸机移植不应修改 Core。新平台实现同一 Runtime/Owner/Adapter
合同：原子事件、通知、单 Owner、单调时钟、callback gate、固定 Storage 和明确错误返回。
不能把平台全局变量塞回通用协议，也不能从中断直接调用完整协议状态机。

## 7. 实机验收

每个 Bearer 至少验证长度边界、队列满、同步/异步 completion、取消失败重试、断链/reopen、
迟到事件、并发任务/ISR、1～多跳吞吐、P99/P999 延迟、CPU/栈/RAM 与长稳。Host Fake
Adapter 通过只代表接口状态机，不代表真实外设完成。
