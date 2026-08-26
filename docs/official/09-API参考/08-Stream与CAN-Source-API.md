# Stream 与 CAN Source API

> 文档级别：`REFERENCE`
> 实现状态：`CURRENT`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：include/ucn 公共头、链接目标与公共 API 测试
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：API 合同不等同于各平台实机驱动完成

Stream Source 为 UART、RS-485、USB CDC 等字节流提供 ISR push、ring、COBS carrier 解码、完整 frame 提交、health 和 stats。ISR push 只复制字节并置事件；解析在 Owner 上下文完成。

CAN Source 支持 CAN-FD 直接 carrier 与 Classic CAN 分段 carrier。重组槽按 CAN identity、sequence 和 timeout 管理；完成 carrier 必须先提交，不能被下一 START 静默覆盖。

两个 Source 都使用固定 buffer，配置必须声明最大 frame、ring 容量和每次 service 预算。错误 padding、超时、溢出和 malformed carrier 都有独立计数。

## Stream Source

适用于 UART、RS-485、USB CDC 等不保留消息边界的字节流。发送方先用 `ucn_stream_carrier_encode()` 对完整 UCN frame 做 COBS carrier；接收 ISR 把任意分块写入：

```c
ucn_stream_source_write_from_isr(&source, dma_chunk, chunk_len);
ucn_event_runtime_signal_source_from_isr(&runtime, source_id,
                                         UCN_EVENT_SOURCE_RX);
```

Owner 调用 Source service，从 ring 取出有限字节，寻找 carrier delimiter、COBS decode、验证 frame 长度，然后交给 Runtime。一次 IRQ 可以包含半帧、多帧或帧尾+下一帧开头，Source 必须保持状态。

Ring 满时新数据返回 `NO_SPACE`/统计 overflow；malformed COBS 只丢当前 carrier，不能清空后续合法数据。`reset()` 用于明确的 Driver 重启，不应用来掩盖持续格式错误。

## CAN-FD Source

CAN-FD carrier 可在一个物理 frame 中携带一条短 UCN frame。DLC 可能把 57 B 逻辑长度扩到 64 B，`ucn_frame_peek_encoded_size()` 得到精确 UCN 长度，剩余 padding 必须全零；非零 padding 拒绝并增加 `fd_padding_errors`。

CAN ingress resolver 把 CAN ID/总线信息映射为 ingress Link/peer。不要只用 payload 内容猜来源，安全策略也不能把 CAN 仲裁 ID 自动当已认证 Node ID。

## Classic CAN Source

8 B payload 无法直接容纳 Core frame，因此 Carrier 使用 START/CONTINUE 分段和固定重组槽。发送前：

```c
count = ucn_can_classic_carrier_segment_count(frame_len);
for (i = 0; i < count; ++i) {
    ucn_can_classic_carrier_encode_segment(..., i, &can_frame);
    driver_send(&can_frame);
}
```

接收槽至少绑定 CAN identity、sequence、expected offset、length、deadline。新 START 只有在旧槽 active 且未 complete 时才能按定义重启；已 complete 的 Carrier 必须先提交，不能被下一 START 覆盖。乱序、重复、超时和长度不符均 fail-closed。

## Bus state

`ucn_can_source_set_bus_state[_from_isr]()` 报告 active/warning/passive/bus-off 等状态。bus-off 后 Link 应不可选，重组槽按合同清理；恢复必须由 Driver 完成硬件重启，再通知 Source/Node。

## 固定存储与多实例

每个 UART/CAN 控制器建立独立 Source、storage、Link、source ID 和 Driver context。默认 storage 适合起步，产品可提供自定义 ring/reassembly 槽；所有内存必须覆盖最大 frame 与峰值中断间隔。

## 验证

Stream：逐字节、随机 chunk、多 frame、坏 COBS、ring wrap/overflow；CAN-FD：所有 DLC 边界、padding；Classic：全 segment、连续 Carrier、槽满、identity 冲突、timeout、bus-off。最终必须 `memcmp` 完整 UCN frame，而不是只检查首字节/长度。
