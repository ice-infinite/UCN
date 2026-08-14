# UCN V5-57 事件驱动 Owner 与窗口 8 三节点报告

> 日期：2026-08-14
> 状态：Core 配置、ESP32 Bench、三板 UART 两跳实测和 Host 软件回归已完成。
> 台架：A=COM5/ESP32-S3-N16R8，B=COM34/ESP32-S3-N16R8，C=COM38/ESP32-S3-N8R8；A—B—C 为 3,000,000 baud UART 两跳，ESP-NOW 同时在线。

## 1. 结论

产品运行模型已从“主循环反复查询是否有数据”改为**外设中断/驱动事件唤醒唯一 Protocol Owner**：

```text
UART ISR / Wi-Fi 回调
  -> 驱动固定 Ring / Adapter 固定 Queue
  -> Task Notification
  -> 唯一 UCN Owner 被立即唤醒
  -> 有预算地批量 Pump / Node Step / Transfer Step
  -> 无工作时再次阻塞
```

ISR 不解析 UCN Frame，不寻路、不解密、不重组，也不调用业务 Endpoint。它只搬运有界数据并通知 Owner；这样既利用外设中断的即时性，又保持 `ucn_node_t` 的单 Owner、无并发重入契约。

轮询没有作为正常数据通路：仅保留最多 10 ms 的超时唤醒，用于无中断平台、漏通知、Heartbeat/Deadline 等协议定时器和 `UCN_MAX_STEP_INTERVAL_MS` 维护门禁。发送队列有数据或 RX 事件到达时不等待 Heartbeat。

三轮正式测试共 27/27 阶段通过，Transfer 重试 0，B 两段 UART Adapter Queue Drop 0。九档等权平均有效吞吐由 V5-56 窗口 4 的 **15.712 KiB/s** 提高到 **17.283 KiB/s（+10.0%）**；相对窗口 1 的 10.578 KiB/s 提高 63.4%。T8K 正式吞吐由 24.750 提高到 29.480 KiB/s；本地发送完成稳定为 217 ms，约 36.87 KiB/s。

## 2. 中断与 Owner 的职责边界

### 2.1 UART

Arduino-ESP32 3.3.7 的 UART ISR 先把 FIFO 数据搬进 IDF Ring；`HardwareSerial::onReceive()` 回调运行在框架 UART Event Task，而不是直接在硬中断里。回调只调用 `xTaskNotifyGive()` 唤醒 UCN Owner。Owner 随后调用 `service_rx()`，按 Adapter Queue 容量批量搬运完整 Carrier Frame。

当 Adapter Queue 已满时，`service_rx()` 不再继续排空 Driver Ring，避免把来不及提交的完整帧主动丢掉。固定容量仍然存在；如果 Driver Ring 也被持续灌满，产品必须通过更大的驱动 Ring、流控或发送整形解决，不能改成无限缓存。

### 2.2 ESP-NOW

ESP-NOW 收包成功入固定队列、以及 TX completion 到达时，只通知 Owner。回调不进入 Core。产品若使用真正 ISR 上下文，必须改用 `vTaskNotifyGiveFromISR()` 和匹配的 ISR-safe Ring/临界区；不能把 Task API 冒充 ISR API。

### 2.3 Owner 批处理与公平性

每次唤醒最多执行 24 个 drain round，单轮连续处理预算为 2,000 us。达到预算时主动让出 CPU，并在下一轮继续，不进入长时间独占。没有事件时，等待时间取以下最小值：

- 10 ms 协议维护兜底；
- 下一个 HELLO、Ping、统计或压力测试节拍；
- Transfer report/deadline/下一消息时间；
- 已经到期的任务立即执行，不睡眠。

这避免了固定 10 ms 睡眠把 T32/T64 等小消息节拍量化，同时保证没有外部事件时不会空转占用 MCU。

## 3. Transfer 窗口边界

`UCN_TRANSFER_MAX_WINDOW` 的编译默认上限由 4 提高到 8；**运行期默认窗口仍为 1**。产品和 Peer 必须分别显式配置窗口能力，实际窗口取双方上限的较小值。没有能力记录的旧 Peer 仍退化为 1，因此 Wire、ACK 载荷和协议版本均未改变。

窗口 8 仍使用累计 ACK、严格顺序 RX 和有界 Go-Back-N，不复制八份 Payload。增大的是少量在途偏移状态，不是消息缓存倍增。更大的窗口在有损无线中可能增加缺口后的重发量，不能仅凭本次短线 UART 结果设为所有产品运行默认值。

## 4. 三轮实测结果

| 等级 | 窗口 4 KiB/s | 事件 Owner/窗口 8 KiB/s | 变化 |
| --- | ---: | ---: | ---: |
| T32 | 2.48 | 2.51 | +1.2% |
| T64 | 5.08 | 5.13 | +1.0% |
| T128 | 9.95 | 12.52 | +25.8% |
| T256 | 12.71 | 13.99 | +10.1% |
| T512 | 17.36 | 13.90 | -19.9%* |
| T1K | 21.45 | 23.46 | +9.4% |
| T2K | 23.21 | 26.61 | +14.6% |
| T4K | 24.42 | 27.94 | +14.4% |
| T8K | 24.75 | 29.48 | +19.1% |

`*` T512 三轮均在约 20.36 s 启动，恰好与 A/B/C 三端 5 s 周期 `V5STAT` 大段打印同一毫秒发生；115200 baud USB 调试串口同步打印阻塞了 Owner。旧窗口 4 基线的 T512 晚约 200～300 ms，避开了打印。该项仍为 16/16 完整交付、0 重试、0 Wire 错，因此记录为 **Bench 日志相位干扰**，不能解释成 Transfer 数据损坏或窗口协议退化。正式产品不得在 Protocol Owner 上同步打印长日志。

三轮共同结果：

- 27/27 阶段通过；无 Invalid、Duplicate、CRC、COBS、Length、Overflow 或 Transfer failure；
- Transfer Fragment 重试 0，B 两个 UART RX Queue Drop 0；
- T8K 本地完成三轮均为 217 ms；
- A/B/C 最终 Route 保持 UART 两跳 Cost 68；
- 事件模式固件构建 RAM/Flash：A=`63,836/604,515 B`，B=`48,820/597,431 B`，C=`55,876/600,171 B`。

Run 3 末尾 `V5OWNER` 直接证明事件路径处于工作状态：A/B/C 分别收到
`972/2800/1874` 次事件唤醒；同时有 `3407/3004/3222` 次 10 ms 维护兜底。
后者发生在无新通知的空闲间隔，仍执行一次有界检查以满足 Core 最大 Step 间隔，
不表示已到达的数据必须等下一次兜底。

## 5. 软件门禁

- Windows：Full/Lite/Nano 各 `1/1`，低资源产品配置 `5/5`；
- WSL GCC 13.3 ASan+UBSan：`1/1`；
- GCC `-fanalyzer`：`1/1`；
- 三个事件模式 PlatformIO 目标均重新构建成功；三块板已写入并运行最终 timer-aware 事件固件。

## 6. 尚未完成

- `HardwareSerial::onReceive()` 是框架 Event Task 回调；其他 SDK 的裸 ISR、DMA half/full callback 仍要按各自 ISR-safe API 接入。
- CAN/CAN-FD、USB、BLE、以太网、Zephyr/NuttX/RT-Thread 的真实事件通知尚未实机验证。
- 尚未完成 Q0 严格延迟、多发送源、ESP-NOW-only、有损无线、Bearer 切换中 Transfer、CPU Run-Time Stats、功耗和长稳。
- 诊断同步打印会干扰实时性；产品应将日志快照投递给低优先级任务或使用 DMA/异步输出。

## 7. 证据

外部测试工程保存：

- `test_results/v5_transfer_event_timer_window8_3000000_final_run1_20260814.log`
- `test_results/v5_transfer_event_timer_window8_3000000_final_run2_20260814.log`
- `test_results/v5_transfer_event_timer_window8_3000000_final_run3_20260814.log`
- `test_results/v5_transfer_event_timer_window8_3000000_summary_20260814.csv`
- `test_results/v5_transfer_window4_3000000_summary_20260814.csv`

当前可以表述为：**UCN 的 ESP32 实机参考已使用事件唤醒 Owner 作为正常路径，轮询只作 10 ms 定时/兼容兜底；窗口 8 在短线 3M UART 两跳三轮中完整交付并进一步提高总体吞吐。** 不能把这一结果外推为所有 MCU、所有介质或硬实时认证。
