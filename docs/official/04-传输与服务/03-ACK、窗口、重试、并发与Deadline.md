# ACK、窗口、重试、并发与 Deadline

> 文档级别：`NORMATIVE`
> 实现状态：`CURRENT`
> 最近核对：`a093862`，2026-08-25

## 累计 ACK

ACK 包含目标 Endpoint、Transfer ID、`next_expected_offset` 和状态。接收端确认从 0 到该 Offset 前的连续数据，不为每个片保存无界选择确认表。

状态包括 OK、NO_SLOT、BAD_FORMAT、INTEGRITY_FAIL、EXPIRED、REJECTED。

## Window

Window 允许 1～8 个 Fragment 同时在途。默认保持 1 以降低 MCU RAM/队列压力；显式配置更大窗口可提高高带宽/高 RTT 路径吞吐。

Go-Back-N 在超时或 ACK 进度不足时从累计确认点重发有界区间。

## Deadline

- ACK timeout：本批在途片等待确认的时间；
- RX timeout：重组 Slot 最久无进度时间；
- completed hold/recent completion：用于幂等响应迟到重复片；
- TX completion：重试耗尽或整体 Deadline 后明确结束。

Transfer 统一从 `config.now_ms` 采样权威时钟，避免初始化后发送使用陈旧 `now_ms`。

## 并发

编译期 TX/RX Slot 控制本地并发；Peer capability 限制每个对端可同时处理多少消息。默认消息并发为 1，产品可显式增加。

更多并发不一定更快：它会占用更多 Node Queue、Link Queue、重组 RAM 和 ACK 流量。4/8 KiB 在某些 MCU Profile 上可能因四槽压力下降吞吐。

## 完成状态

`SENT` 用于直接单帧类；`DELIVERED` 表示远端完成重组并 ACK；其余状态明确区分远端拒绝、超时、重试耗尽和本地发送失败。仍不表示远端业务逻辑执行成功。

## 累计 ACK 如何减少接收状态

若接收端已连续收到 `[0, 160)`，即使后面的 `[192,224)` 先到，ACK 仍返回 `next_expected_offset=160`。等 `[160,192)` 到达后，连续前缀可一次跳到 224。

这让 ACK 固定为 8 B，不需要为每个 Fragment 携带可变长度 bitmap；代价是丢失中间片时可能 Go-Back-N 重传已经到达但尚未被累计确认的后续片。

## Window=1 与 Window>1

Window=1 的流程最简单：发一片、等 ACK、再发下一片，RAM 和突发队列压力小，但在高 RTT 链路上利用率低。Window=4 时可连续发四片再等累计 ACK，吞吐更高，但会占用更多 Node/Link Queue，并在丢一片时重传更大区间。

理论在途业务字节近似为：

```text
in_flight ≈ window × fragment_data_bytes
```

实际还包括每片 Frame/Transfer Header、Carrier 开销和队列对象。

## ACK 状态的处理

| ACK | 发送端含义 | 推荐动作 |
| --- | --- | --- |
| OK | 连续前缀推进或事务完成 | 更新 offset/完成 |
| NO_SLOT | 远端当前无 RX Slot | 有界退避，受总 Deadline 限制 |
| BAD_FORMAT | 本事务格式不被接受 | 终止，不盲目重试同字节 |
| INTEGRITY_FAIL | 整包 CRC 不一致 | 终止/按产品策略重新发起新事务 |
| EXPIRED | 远端已释放事务 | 终止或建立新 Transfer ID |
| REJECTED | Endpoint/Policy/能力拒绝 | 终止并上报业务 |

所有状态都不能变成无限 retry loop。

## Deadline 如何建立

Transfer 使用配置提供的权威 `now_ms` 回调/时间源。初始化后立即 send、系统运行很久后再 send，都必须以当下时间建立 Deadline，不能使用上次 step 缓存的旧 `now_ms`。

相对时长先通过统一 duration helper 验证。ACK/RX/completed hold/recent completion 各有不同 Deadline，不能用一次 ACK 活动无限延长整体业务 Deadline。

## 重试状态机

```text
发送窗口
  ↓ 等累计ACK
ACK推进 ─────────→ 发送下一窗口/完成
  │
  ├─ 超时且 retries < max → 从 next_expected_offset 重发
  ├─ retries耗尽 → RETRY_EXHAUSTED
  ├─ 总Deadline到期 → TIMEOUT
  └─ Node/Link永久失败 → SEND_FAILED
```

临时 `NO_SPACE` 和 `LINK_DOWN` 需要与持久化/格式错误区分；是否等待 Route 恢复仍受 Deadline 和重试上限控制。

## 并发的两层限制

- 本地全局 TX/RX Slots：限制此 MCU 同时持有多少事务；
- Peer capability：限制面向某个对端的 Class、Window 和并发。

即便本机有 4 个 TX Slot，对端只声明并发 1，也不能同时发 4 条 T8K。否则会稳定制造 NO_SLOT 和网络重传。

## 完成回调和缓冲生命周期

发送完成回调会给出 Destination、Endpoint、Transfer ID 和完成状态。应用应在收到终态后释放或复用发送缓冲；在此之前必须遵守 API 的借用合同。

`DELIVERED` 只说明远端完整重组并对协议 ACK。如果接收业务在之后校验参数失败，应通过业务 Result Endpoint 返回 `REJECTED/FAILED`，不能修改已经成立的 Transfer 交付事实。

## 验证清单

- [ ] ACK 的 next_expected_offset 只前进不回退；
- [ ] 乱序片不会错误扩大连续前缀；
- [ ] Window 1～8 的边界和 Peer clamp 正确；
- [ ] ACK timeout、总 Deadline 和重试耗尽分别产生正确终态；
- [ ] 高 uptime 初始化后首次 send 不会立即超时；
- [ ] NO_SLOT/BAD_FORMAT/INTEGRITY_FAIL 等状态不会混淆；
- [ ] 完成回调恰好一次，迟到 ACK/Fragment 幂等。
