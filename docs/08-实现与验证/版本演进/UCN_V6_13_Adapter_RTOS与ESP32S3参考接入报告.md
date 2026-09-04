# UCN V6-13 Adapter、RTOS 与 ESP32-S3 参考接入报告

## 1. 范围与结论

V6-13 把协议核心与物理外设、RTOS 调度彻底分开。当前完成的是可编译、固定内存、
非阻塞的软件接入层以及 Host 并发验证；没有把某个 UART 号、GPIO、CAN 控制器、USB
Endpoint 或 ESP-NOW Peer 写死在协议内。

当前状态：

- 软件接口与 Host Fake：`SELF REVIEW PASS`；
- 默认 v5 产品路径：未接入 v6 Adapter；
- ESP32-S3 固件编译、六板实机、DMA/ISR、真实吞吐和功耗：`HARDWARE HOLD`；
- 24 小时长稳：`HARDWARE HOLD`；
- V6-13 整体：`PARTIAL / SOFTWARE COMPLETE`，不能据此宣称参考产品完成。

本机检查时只枚举到 COM80/COM81 虚拟串口和蓝牙 COM7/COM9，没有 ESP32 串口，所以上述
硬件项没有被 Host 测试替代。

## 2. 分层架构

```text
ISR / SDK callback
        |
        | publish complete RX item / TX completion
        v
ucn_v6_adapter                     fixed RX/TX slots, Link generation
        |
        | post bounded Owner event
        v
ucn_v6_freertos_port               task notification + timer fallback
        |
        v
single Protocol Owner              Wire/Security/Route/QoS/Transfer owners

Bearer defaults are separate:
  adapters/ucn_v6_uart.*           UART and RS-485
  adapters/ucn_v6_wifi.*           ESP-NOW
  adapters/ucn_v6_can.*            Classic CAN and CAN-FD
  adapters/ucn_v6_usb.*            USB CDC/Bulk

Product binding is separate again:
  reference/esp32s3/*              UART port, GPIO, DMA, Wi-Fi channel/peers
```

Driver 中断只允许提交一个已经完整接收的 Link Frame 或一个物理 TX completion。它不能
解析 Wire、修改路由、调用业务 Endpoint，也不能直接推进协议 FSM。协议 Owner 通过任务
通知立即唤醒；仅当通知丢失或没有事件时，定时器事件才作为有界保底。

## 3. 固定资源与 Manifest

新增编译期配置：

| 配置 | 默认值 | 合法范围 | 含义 |
|---|---:|---:|---|
| `UCN_V6_CONFIG_ADAPTER_LINKS` | 8 | 1..255 | 独立 Link 实例槽 |
| `UCN_V6_CONFIG_ADAPTER_RX_SLOTS` | 32 | 1..255 | 完整 RX Item 槽 |
| `UCN_V6_CONFIG_ADAPTER_TX_SLOTS` | 32 | 1..255 | TX Buffer 所有权槽 |
| `UCN_V6_CONFIG_ADAPTER_FRAME_BYTES` | 512 | 64..4096 | 单个完整 Link Frame 最大字节数 |

四项都进入 Feature Manifest 与 Layout Hash。默认配置的公共静态存储预算为：

- Adapter Owner：48,128 B；
- FreeRTOS Port：2,048 B；
- Adapter 公开函数最大静态栈帧：112 B；
- FreeRTOS 初始化最大静态栈帧：128 B。

最小配置 `1 Link + 1 RX + 1 TX + 64 B Frame` 已完成 MinSizeRel 编译和配置合同测试。
不同产品应根据 Link 数和并发深度覆盖默认值，不能在运行时申请堆内存。

## 4. Link 与 Buffer 所有权

### 4.1 RX

`ucn_v6_adapter_publish_rx()` 在共享 task/ISR/SMP 锁内一次性复制：

```text
{complete frame, Link ID, Link Generation, Event Token, Timestamp}
```

Owner 使用 `peek_rx()` 读取，业务处理成功后用同一个 Event Key 调用 `retire_rx()`。输出
空间不足、代际过期、时间戳非 canonical、Link 未 READY 或槽满都不会发布半个 Item。

### 4.2 TX

TX 生命周期被冻结为：

```text
QUEUED -> SUBMITTING -> SUBMITTED -> COMPLETED -> retired
   |                         |
   +----> CANCELLED <--------+
```

- 编码后的完整 Frame 被复制进固定 TX 槽；
- `buffer_token` 在 completion/cancellation 被 Owner 退休前绝不返还；
- Driver `submit()` 必须非阻塞；允许在回调内同步发布 completion；
- 同一个 Event Key 不能二次完成或二次取消；
- Driver submit 失败时退回 `QUEUED`，允许 Owner 稍后重试；
- 取消已提交项时，物理 `cancel()` 成功后才进入 `CANCELLED`；
- 物理完成与 cancel 竞争时，以已经提交的 completion 为准。

事件键按字段比较，禁止对含 padding 的结构体直接 `memcmp`。

### 4.3 Link readiness 与 reopen

Link 注册后处于 `STARTING`，只有产品 BSP 完成外设初始化并显式提交 `READY` 后才能收发。
状态为 `OFFLINE/FAULTED` 后不能原地恢复为 READY，必须执行：

```text
quiesce driver
  -> retire all Link-owned RX/TX
  -> increment no-wrap Link Generation
  -> STARTING
  -> product reinitializes hardware
  -> READY
```

旧 Generation 的迟到 RX/completion 会被拒绝。调用方必须提供足够数组容量接收所有被回收的
TX `buffer_token`；容量不足时在调用 Driver 前零写拒绝。

## 5. RTOS 锁与通知合同

任务与 ISR 使用两种不同入口：

- `lock_task()`：任务上下文可等待取得同一共享锁；
- `try_lock_from_isr()`：ISR 只能立即成功或失败；
- `unlock_task()` / `unlock_from_isr()`：允许 RTOS 保存并恢复不同临界区 token；
- `notify_owner_task()`：产品可映射到 `vTaskNotifyGiveFromISR()` / task notification；
- `wait_for_notification()`：Owner Task 阻塞等待；
- `read_monotonic_time_us()`：所有 timer/Deadline 的唯一单调时间源。

FreeRTOS Port 不包含任何 ESP-IDF/FreeRTOS 头，也不创建任务。产品只需在自己的 BSP 文件中
填写这些回调。`wait_and_run()` 只有在 Owner 队列为空时等待；等待超时会投递一个 TIMER，
因此轮询是保底而不是主处理机制。

## 6. Bearer 与参考默认值

| 文件 | Bearer | 默认值 | 必须由产品提供 |
|---|---|---|---|
| `ucn_v6_uart.c` | UART | 3 Mbit/s、单硬件优先级 | UART 号、TX/RX GPIO、DMA、时钟误差 |
| `ucn_v6_uart.c` | RS-485 | 1 Mbit/s、半双工 | DE/RTS GPIO、turnaround、总线偏置 |
| `ucn_v6_wifi.c` | ESP-NOW | 1 Mbit/s effective、250 B carrier | interface、channel、Peer/MAC、发送回调 |
| `ucn_v6_can.c` | Classic CAN | 500 kbit/s、8 B carrier、4 priority | 控制器、ID 映射、Carrier 重组 |
| `ucn_v6_can.c` | CAN-FD | 500 kbit/s arbitration、64 B carrier | data bitrate、BRS、ID 映射、Carrier 重组 |
| `ucn_v6_usb.c` | USB | 12 Mbit/s effective、最多512 B transfer | CDC/Bulk Endpoint、completion、断连事件 |

Carrier MTU 与 Link Frame MTU 不混用。CAN/CAN-FD Driver 负责 Carrier 分片重组，然后向
Adapter 原子交付完整 Link Frame；Transfer 模块只按已经归约的实际 Payload Budget 分片。

## 7. ESP32-S3 参考产品边界

`ucn_v6_esp32s3_uart_binding_t` 要求产品显式提供 UART 0..2、不同的 TX/RX GPIO、至少
256 B 的 RX/TX DMA Buffer；RS-485 还必须提供 DE GPIO。`ucn_v6_esp32s3_esp_now_binding_t`
要求信道 1..14 和非零 Peer 容量。默认示例使用 3 Mbit/s UART，但不固定 COM 口或板间
GPIO。

这个 reference builder 只验证配置并生成通用 Link Config，不调用 `uart_driver_install()`、
`esp_now_init()` 或任何 ESP-IDF API。真实 SDK 调用必须留在产品仓库的单一 BSP 文件中。

## 8. 分项自审记录

| 子项 | 自审重点 | 结果 |
|---|---|---|
| 13-01 | RX/TX 固定槽、Event Key、Buffer Token、同步 completion、取消 | PASS |
| 13-02 | 多 Link、同 Bearer 多实例、readiness、reopen、迟到回调 | PASS |
| 13-03 | task/ISR 分锁、任务通知立即唤醒、timer fallback | PASS |
| 13-04 | ESP32-S3 UART/RS-485/ESP-NOW 参数化，无固定 GPIO | PASS（软件） |
| 13-05 | Classic CAN/CAN-FD/USB 独立文件及 Carrier 边界 | PASS（软件） |
| 13-06 | 两线程双 Link Host 并发、ASan/UBSan、Analyzer | PASS；TSan 环境不可用 |
| 13-07 | ESP32-S3 固件、ISR/DMA、六板与24 h | HOLD（当前无实板串口） |

## 9. 已执行验证

- Windows GCC Full：72/72；
- Adapter/FreeRTOS 定向：通过；
- WSL 双线程双 Link：通过；
- WSL ASan/UBSan Adapter：2/2；
- GCC `-fanalyzer -Werror` Adapter：1/1；
- MinSizeRel 最小 Adapter 配置：构建与 Config contract 通过；
- `-fstack-usage`：Adapter/Port 最大分别 112/128 B；
- `git diff --check`：无空白错误。

TSan 的 GCC 运行在 WSL 上两次都在测试代码启动前报
`ThreadSanitizer: unexpected memory mapping`；Clang 18 环境又缺少
`libclang_rt.tsan-x86_64.a`。因此本报告只声明 pthread 功能并发通过，不声明 TSan 通过。

## 10. 硬件恢复后的验收顺序

1. ESP32-S3 N16R8 编译，验证固定 RAM、任务栈和二进制尺寸；
2. 单 Link UART 3 Mbit/s：ISR RX、DMA TX、completion 与断线 reopen；
3. 六板 UART/RS-485 链：1～5 跳、Q0～Q3、32 B～8 KiB；
4. ESP-NOW 单跳/多跳及 UART+ESP-NOW 混合 Route；
5. CAN/CAN-FD/USB 各自 Carrier 边界、padding、重组和故障注入；
6. Realtime 硬件 timestamp、uncertainty/asymmetry 实测；
7. Flash 撕裂写、断电、重连、Session/Link Generation 迟到事件；
8. 24 小时长稳、CPU、栈、RAM、吞吐、P99/P999 延迟与功耗。

这些结果必须绑定同一 commit、固件 Hash、板号、接线、SDK 版本和原始日志，统一归入
V6-14 证据包。
