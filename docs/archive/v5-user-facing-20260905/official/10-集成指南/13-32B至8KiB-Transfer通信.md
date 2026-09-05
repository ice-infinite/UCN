# 32 B 至 8 KiB Transfer 通信

> 文档级别：`GUIDE`
> 实现状态：`PARTIAL（通用合同已实现，产品 BSP/SDK 需接入）`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：公共 API、Port/Adapter/Source 实现与测试
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：部分 ESP32-S3 UART/ESP-NOW；其他平台按正文标注

根据最大业务消息选择最小可容纳 Class，避免所有消息都配置 T8K。为 peer/endpoint 分配 TX/RX slot，设置窗口、ACK timeout 和消息 deadline，然后持续调用 Transfer step。

Transfer 自动按当前路径 MTU 分片；每个 fragment 仍是独立 Core frame。接收端完成 CRC32 和重组后交付 handle，应用使用完 release。

大消息会占用链路更久。Q0 控制消息和 IMU 实时流应与大块日志/文件传输分流或设置策略。

## Class选择

| 实际消息长度 | 最小 Class |
| ---: | --- |
| 1～32 B | T32 |
| 33～64 B | T64 |
| 65～128 B | T128 |
| 129～256 B | T256 |
| 257～512 B | T512 |
| 513～1024 B | T1K |
| 1025～2048 B | T2K |
| 2049～4096 B | T4K |
| 4097～8192 B | T8K |

选择更大Class不会提高速度，只会放宽合法上限并可能要求更多接收能力。

## 接入步骤

1. 按最大消息设置编译期 `UCN_TRANSFER_MAX_MESSAGE_BYTES`和RX slots；
2. 初始化Transfer并提供唯一单调时钟；
3. bind Endpoint、最大Class和是否require E2E；
4. 配置每个peer的Class/window/concurrency；
5. 发送期间保持TX buffer不变；
6. Owner在Core空闲后调用Transfer step；
7. receive callback处理并release handle；
8. completion区分DELIVERED与业务执行。

## 分片与校验

T128以上按当前路径MTU减去Core/Transfer头得出fragment data length，最低16 B。每片带总长、offset、transfer ID和整消息CRC32；Core每帧还有CRC16/可选AEAD tag。CRC32能发现重组后任意片错位/损坏，但不是安全认证，攻击者场景必须使用E2E。

## Window与并发

Window=1是Stop-and-Wait，RAM/乱序压力最低；2～8可提高高RTT链路吞吐，但丢一片可能Go-Back-N重发多个片。并发Transfer需要多个TX/RX slot并与peer明确配置；单源四槽不一定比一槽快，可能因队列竞争降低4/8KiB吞吐。

## 性能估算

```text
goodput ≈ payload_bytes /
  (serialization + per-hop_store_forward + ack_wait + retries + owner_queue)
```

包越小，header/ACK比例越高；包越大，占链路时间和失败重传代价越高。应在目标跳数、MTU、窗口、Q0并发下测试T32～T8K，不从单一3M UART结果推断所有Bearer。

## 故障

- 路径MTU下降：后续fragment缩小，低于16 B则失败；
- RX slot满：远端NO_SLOT ACK，发送完成REMOTE_REJECTED；
- ACK丢失：recent completion帮助重发ACK而不重复交付；
- 中途断链/切路：相同transfer identity继续有界重试，仍需测试乱序；
- 应用忘记release：slot直到hold timeout不可复用。

## 验收

每个Class做1/2/3跳、窗口1/4/8、并发1～产品上限，记录payload goodput、p99、retries、CRC、Q0延迟和RAM。实测报告保留固件hash、接线和原始串口日志。
