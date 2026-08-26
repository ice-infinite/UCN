# 帧语义、Payload 与开销计算

> 文档级别：`NORMATIVE REFERENCE`
> 实现状态：`CURRENT`
> 最近核对：`a093862`，2026-08-25

## 默认容量

`UCN_MAX_FRAME_BYTES` 默认 256 B。静态 Payload 缓冲默认按保守 32 B Header 计算，为 224 B。实际某帧可用 Payload 还受以下因素限制：

```text
min(
  UCN_MAX_PAYLOAD_BYTES,
  当前逻辑 Link MTU - 当前 Profile Header - 可选 16 B Tag
)
```

不能把 W0 头更小得到的全部空间自动视作静态缓冲变大；公共对象仍按编译配置分配。

## 典型开销

| 情况 | 固定开销（不含 Payload） |
| --- | ---: |
| W0 基础明文 | 17 B |
| W1 基础明文 | 21 B |
| W2 基础明文 | 26 B |
| W3 基础明文 | 30 B |
| 带 E2E 保护 | 上述再加 16 B Tag |
| 带 Route Extension | 使用 18/23/28/32 B 头 |
| 带 Path ID | 使用 19/25/31/36 B 头 |

这些头尺寸包含 CRC 所需的冻结线布局。Carrier（COBS、Classic CAN 分段、CAN-FD Padding）还会增加各自介质开销，不属于 UCN Frame Header。

## 一帧不是一个来回

一帧是一次单向编码单元。发送、接收、ACK 或业务结果是不同帧/事件：

- 普通 Q0/Q1 可以只有单向帧；
- Transfer Fragment 可由 ACK 帧确认；
- Service 命令的业务执行结果需要远端任务另发 Result 消息；
- HELLO/Heartbeat 有各自周期，不是每个业务帧的应答。

## 校验范围

CRC16 覆盖单个 UCN 线帧。T128～T8K 大消息由每个 Fragment 的 Frame CRC 加整个消息 CRC32 双层检查。即使定义 8 KiB 逻辑消息，也不是把 8 KiB 无保护地塞入一帧；它会按 MTU 分片，重组后再校验完整 CRC32。

## 1. 如何计算某条 Link 的最大 Payload

设逻辑 Link MTU 为 `M`、选择头长为 `H`、E2E Tag 为 `T`（明文 0，保护 16），则线帧可放的 Payload 为：

```text
payload_on_link = min(UCN_MAX_PAYLOAD_BYTES, M - H - T)
```

如果 `M < H + T`，该格式在这条 Link 上完全不可用。计算必须使用逻辑有效 MTU：静态 MTU 与运行时 MTU 求当前合同值，而不是只看物理控制器理论最大帧。

## 2. 开销示例

### W0 明文 32 B Payload

总长 `17 + 32 = 49 B`，协议效率约 `32/49 = 65.3%`，尚未计算 COBS 分隔或 CAN carrier。

### W3 E2E 32 B Payload

总长 `30 + 32 + 16 = 78 B`，协议效率约 `41.0%`。高档地址和安全会增加固定开销，小 Payload 更明显。

### W1 明文 200 B Payload

总长 `21 + 200 = 221 B`，效率约 `90.5%`。但只有在 Link MTU、静态 Payload buffer 和字段范围都满足时才合法。

## 3. Carrier 开销

- Stream COBS：根据内容增加少量编码字节，并有帧分隔符；
- UART：每字节通常还包含起始/停止位，115200 bit/s 不等于 115200 B/s；
- Classic CAN：一个 UCN Frame 被拆成多个 8 B 物理帧，每帧有仲裁、CRC、填充和间隔；
- CAN-FD：DLC 可能把 57 B 映射为 64 B，额外 padding 必须为零；
- Wi-Fi/ESP-NOW：还存在 MAC/PHY、重试和 SDK 队列开销。

所以“UCN 头占多少”只是吞吐的一部分。

## 4. 小消息与大消息的选择

能放进一个 Core Frame、且业务不需要消息级 ACK 时，直接 Endpoint 最省状态和时延。超过当前路径 Payload 或需要完整消息确认时，选择最小 Transfer Class。

不要把 8 KiB 定义为 Core 单帧：

- 小 MTU 介质无法承载；
- 中继需要巨大 buffer；
- 任一字节错误会重发整个巨帧；
- Q0 和控制消息会长时间等待。

## 5. Fragment 的完整性

每个 Fragment 先通过 Core CRC16，防止错误物理帧进入重组。重组完成后再对整个业务消息做 CRC32，能发现错序、错误拼接或跨 fragment 数据破坏。安全模式还需 E2E AEAD Tag；CRC 不是抗篡改机制。

## 6. “一帧完成”的准确含义

物理 Driver 报 TX complete 可能只表示字节离开 DMA/FIFO；Link send 返回成功可能只表示 SDK 接受；目标 Frame decode 成功才表示单帧到达；业务是否处理需要 Endpoint/Service 层确认。

## 7. 性能调优原则

提高 Payload 通常提升协议效率，但也增加序列化时延和阻塞时间。控制系统应将高频小状态、关键命令和大块数据分开测试，选择满足 deadline 的最小合理 Frame/Transfer 参数，而不是只追求最大单包效率。
