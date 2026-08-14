# UCN V5-59 标准 Stream Carrier 与 UART/USB Source 方案

> 日期：2026-08-14
> 状态：公共实现与 Host 软件门禁已完成；真实驱动/RTOS/实机继续 V5-61。
> 前置：V5-58 公共 `ucn_event_runtime_t` 已完成 Host 软件门禁。

## 1. 目标和边界

V5-59 把三板工程已验证的“COBS + 0 分隔符 + 有界接收”整理为主库公共、SDK 无关 Stream 模块，供 UART、RS-485、USB CDC 以及其他可靠字节流复用。模块放在独立 `src/adapters/stream/`，不塞进 FreeRTOS/Zephyr/NuttX/RT-Thread Port，也不包含 GPIO、波特率、串口 Handle、DMA 类型或 USB Endpoint 类型。

它负责：

- 调用者提供存储的固定 Byte Ring；
- Task/ISR 整块写入、满 Ring 确定性拒绝与丢字节后重新同步；
- COBS 编码和尾随 `0x00` 的 Stream Carrier；
- Owner-only Source `service()`，按字节/候选帧/错误预算 Drain；
- 拆包、粘包、Ring Wrap、坏 COBS、超长候选、长度门禁；
- 完整 UCN 帧提交 V5-58 Runtime，公共 RX Queue 满时保留已解码帧重试；
- 多实例完全独立，每个 UART/USB/RS-485 端口各占一个 Source ID。

它不负责：

- 初始化 UART、RS-485 DE/RE、USB Device、DMA 或 RTOS；
- 替产品选择引脚、波特率、流控、缓冲区大小和 Link Cost；
- 在 Carrier 层重复添加消息 CRC。UCN Frame 已有 CRC16，Transfer 完整消息另有 CRC32；生产认证仍由 Security Provider 完成；
- 在 ISR 内 COBS 解码、调用 Node、路由、解密、Transfer 或业务回调；
- 把 Stream Source 做成所有 Bearer/RTOS 的大杂烩。CAN/CAN-FD 属于独立 V5-60。

## 2. 标准线载体

```text
一个完整 UCN Frame
  -> COBS Encode（编码体中不出现 0）
  -> 追加一个 0x00 delimiter
  -> UART / RS-485 / USB CDC 字节流
```

`0x00` 是唯一 Carrier 分隔符。空分隔符仅用于重新同步，不形成 UCN 帧。COBS 解码成功只证明载体结构成立；Frame Magic、版本、长度、CRC16、安全、Network 和 Replay 仍由 `ucn_node_receive()` 检查。

最坏编码缓存采用保守上界：

```text
cobs_encoded_max(N) = N + floor(N / 254) + 2
wire_max(N)         = cobs_encoded_max(N) + 1 delimiter
```

V5-59 不改变 v5 Wire Header、消息编号或协议版本；Carrier 只存在于相邻两个物理端点之间。

## 3. 固定存储模型

`ucn_stream_source_t` 只保存索引、配置、统计和调用者存储指针。产品静态提供：

- `ring_storage[]`：驱动回调/ISR 到 Owner 的原始字节 Ring；
- `frame_storage[]`：一个候选 COBS 帧/一个待提交完整 UCN 帧；
- 公共 `ucn_adapter_rx_queue_t`：由 Event Runtime/Protocol Owner 使用。

主库提供默认 Storage 类型方便直接使用，也允许产品传入更小/更大的静态数组。不存在 `malloc`、运行时扩容或每帧分配。Ring、Frame Storage、公共 RX Queue 和 Node 的 RAM 必须分别计量，不能只报告 `sizeof(ucn_stream_source_t)`。

## 4. 并发与背压

Task/ISR 写 Ring 使用 Runtime 所绑定的 `ucn_port_ops_t` 临界区；ISR 缺成对 token 回调时返回 `UCN_ERR_CONFIG`。写入采用整块全收或全拒绝，不返回含糊的半包成功。发生 Ring 满表示字节流出现缺口。Source 记录“缺口前已经接受的字节数”：这些字节中的完整 Carrier 仍按序交付，到达真实缺口后才丢弃至下一个 `0x00` 恢复，既不把缺字节前后两个帧误拼接，也不无故丢掉缺口前已经完整排队的数据。

Source 每次 `service()` 同时受三个固定上限约束：

- Runtime 的 `max_work`：最多处理多少个 Carrier 候选/待提交帧；
- Source 的 `max_bytes_per_service`：最多从 Ring 消费多少原始字节；
- Source 的 `max_errors_per_service`：最多处理多少个坏候选后提前让出。

COBS 解码后若公共 Adapter RX Queue 满，Source 保留这一个完整帧，不继续消费后续字节；Owner 先 Pump Queue，下一 Drain Round 再提交。这样背压不会主动丢掉已经从 Driver Ring 取出的完整帧。达到 Runtime Round 上限仍由 V5-58 返回 `work_remaining`/Yield，不睡眠等待心跳。

## 5. 产品对接

初始化顺序：

```text
Node / Link / Adapter RX Queue / Event Runtime
  -> 静态 ring_storage + frame_storage
  -> ucn_stream_source_init(runtime, source_id, ingress_link, ...)
  -> 启动 UART/USB/RS-485 驱动和中断
```

驱动 RX 回调只调用：

```text
ucn_stream_source_write()           // Task/驱动线程上下文
ucn_stream_source_write_from_isr()  // 真正 ISR
```

发送侧产品 Link `send()` 先调用公共 `ucn_stream_carrier_encode()`，再把**完整编码包**一次性放入有界驱动 TX Queue；空间不足返回 `UCN_ERR_NO_SPACE`。同步等待 DMA 完成、USB Host 读取或 RS-485 空口结束不属于 Link `send()`。

## 6. 验收

Host 软件覆盖：COBS 全零/长非零/容量边界、拆包、粘包、Ring Wrap、多个 Source、Task/ISR、无 ISR 锁、Ring 满、坏 COBS、空分隔符、超长候选、长度错、错误预算、字节预算、公共 RX Queue 背压保留、Fallback、Reset 和配置裁剪。随后通过 Full/Lite/Nano、产品头、Service OFF、ASan/UBSan 与 `-fanalyzer`。

真实 UART DMA、USB CDC、RS-485 方向控制、不同 MCU Cache/DMA 一致性、流控、CPU/栈/功耗和长稳仍属于 V5-61/产品实机；Host Fake 不能替代这些证据。

## 7. 当前实现与证据

公共文件：

- `include/ucn/adapters/ucn_stream_source.h`：Storage、配置、统计、Task/ISR 写入、Reset、查询和 TX Carrier API；
- `src/adapters/stream/ucn_stream_source.c`：固定 Ring、原地 COBS Decode、缺口顺序、三重预算、Queue 背压保留；
- `tests/test_stream_source.c`：公共模块单元/模拟测试；
- `docs/calltree/stream_source.calltree.yaml`：调用与上下文边界。

默认 `UCN_MAX_FRAME_BYTES=256` 时，便利 Storage 为 `512 B Ring + 259 B Frame = 771 B`；Host x64 `sizeof(ucn_stream_source_t)=240 B`。128 B 产品头把便利 Storage 裁到 `128 + 130 = 258 B`，Event Runtime 3 Source 为 304 B。Source 对象/Storage、Runtime、公共 Adapter RX Queue 与 Node 必须分别计量，目标 MCU 结果仍需目标 ABI/链接图。

2026-08-14 验证结果：Windows Full `14/14`、Lite `11/11`、Nano `1/1`、Full Service OFF `11/11`、128 B/3 Source 产品配置 `5/5`；WSL ASan+UBSan `1/1`、GCC `-fanalyzer` `1/1`。覆盖 COBS 精确容量、拆包、粘包、Ring Wrap、多 Source、Task/ISR、无 ISR token、Reset/Fallback、坏/短/超长 Carrier、错误预算、缺口前帧保留、缺口后重同步和公共 RX Queue 背压重试。

V5-59 没有修改 v5 Wire Header、消息编号、路由或 Security AAD。下一项 V5-60 是独立 CAN/CAN-FD Frame Source/经典 CAN Carrier；不能把 COBS Stream 模块直接冒充 CAN 分段实现。
