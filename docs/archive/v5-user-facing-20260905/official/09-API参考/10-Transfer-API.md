# Transfer API

> 文档级别：`REFERENCE`
> 实现状态：`CURRENT`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：include/ucn 公共头、链接目标与公共 API 测试
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：API 合同不等同于各平台实机驱动完成

Transfer 提供 T32、T64、T128、T256、T512、T1K、T2K、T4K、T8K 九档有界消息，内部按实际路径 MTU 分片、CRC32 校验、累计 ACK 和有界窗口重传。

调用者注册 Peer/Endpoint 与静态 TX/RX slot，调用 send 后持续在 Owner 上执行 step。接收完成返回只读/有界 handle，应用处理后必须 release，释放前 slot 不能复用。

deadline 在真实首次推进时建立，避免高 uptime 初始化或长时间空闲后的陈旧时间基准。消息并发和窗口增大可能提高吞吐，也会增加 RAM 与排队。

## 选择消息 Class

`ucn_transfer_smallest_class_for_length()` 可从实际长度选择最小 T32～T8K；调用者也可以显式限制最大 Class。T32/T64 走现有单帧 Endpoint 路径，completion 为 `SENT`，不代表远端执行；T128～T8K 才进入 Fragment/ACK/重组，成功 completion 为 `DELIVERED`。

Class 是内存/协议上限，不要求消息恰好填满。例如 180 B 可用 T256。长度超过 Class 或编译期 `UCN_TRANSFER_MAX_MESSAGE_BYTES` 返回 `TOO_LARGE`。

## 初始化与绑定

```c
ucn_transfer_config_t cfg = {
    .node = &node,
    .now_ms = product_now_ms,
    .now_context = &clock,
    .fragment_data_limit = 0U,
    .max_retries = UCN_TRANSFER_MAX_RETRIES,
    .ack_timeout_ms = UCN_TRANSFER_ACK_TIMEOUT_MS,
    .rx_timeout_ms = UCN_TRANSFER_RX_TIMEOUT_MS,
    .completed_hold_ms = UCN_TRANSFER_COMPLETED_HOLD_MS,
    .recent_completion_ms = UCN_TRANSFER_RECENT_COMPLETION_MS,
};
ucn_transfer_init(&transfer, &cfg);
ucn_transfer_bind_endpoint(&transfer, FILE_ENDPOINT,
                           UCN_TRANSFER_CLASS_T4K, true,
                           on_received, &app);
```

`now_ms` 是强制单一时间源，send/RX/ACK/step 都从它取当前值。Transfer 会占用 Node generic RX handler 来解释 Type 0x22/0x23；非 Transfer 和静态 Endpoint 通过 fallback handler 继续分发。

## Peer capability

发送前配置对端 maximum Class；window 和并发是独立 capability，默认都为 1，保持 Stop-and-Wait/单重组槽兼容。只有双方都配置足够 RX slots，才提高 concurrent transfers。

窗口改变只能在没有活跃 TX 时执行。实际窗口取本地期望、远端 maximum 和静态上限的最小值。

## 发送与 buffer 所有权

```c
rc = ucn_transfer_send(&transfer, peer, FILE_ENDPOINT,
                       UCN_TRANSFER_CLASS_T4K,
                       data, length, on_complete, &ctx);
```

T128～T8K 成功接受后，Transfer 只借用 TX buffer，调用者必须保持内容不变直到 completion。没有可用 TX slot 返回 `NO_SPACE`；未知/不足 peer capability 返回配置/大小类错误。

每个 Fragment 头包含 endpoint、class、transfer ID、总长、offset 和整消息 CRC32。接收端只接受期望 offset；累计 ACK 返回 `next_expected_offset`，丢失后做有界 Go-Back-N。

## Owner 调度

先推进 Core/Protocol Owner，再推进 Transfer：

```c
rc = ucn_protocol_owner_step(&owner);
if (rc == UCN_ERR_NOT_FOUND) {
    rc = ucn_transfer_step(&transfer);
}
```

每次 step 至多提交一条新/重传 Fragment，避免 Bulk 饿死 Q0/Q1 和维护流量。事件通知可让 step 立即运行，不需等周期 timer。

## 接收与 release

完整重组且 CRC32 成功后调用 receive handler，并给出 handle。应用应快速复制/处理，之后：

```c
ucn_transfer_release_received(&transfer, handle);
```

释放前 RX slot 不能复用；超过 completed hold 会自动释放并统计。T32/T64 的 direct handle 不占重组槽。

Recent-completion 表用于识别 ACK 丢失后的重复尾片，避免重复交付业务。

## 完成状态

- `SENT`：小消息已交 Node；
- `DELIVERED`：远端完成重组并 ACK；
- `REMOTE_REJECTED`：无 slot/格式/完整性/策略等；
- `TIMEOUT`：deadline 到期；
- `RETRY_EXHAUSTED`：ACK 重试用尽；
- `SEND_FAILED`：本地 Node/Link 提交失败。

即使 `DELIVERED` 也只到远端 Transfer 层；执行舵机/写 Flash 等业务完成仍需要 Service Result。

## 资源与错误

每个 RX slot 包含最大消息 buffer，扩大 `MAX_MESSAGE_BYTES × RX_SLOTS` 会直接增加 RAM。窗口主要增加在途/重传状态，不会突破固定 slot，但过大可能加剧中继排队。路径 MTU 下降时 Fragment 自动缩小到固定 16 B 下限；无法容纳最小 Fragment 时 fail-closed。
