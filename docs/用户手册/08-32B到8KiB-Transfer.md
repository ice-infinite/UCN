# 32 B 到 8 KiB Transfer

Transfer是可选扩展，用于超过单帧能力或需要重组ACK的大消息。它不会改变Core的轻量普通消息路径。

## 1. 选择最小 Class

| 长度 | Class |
| ---: | --- |
| 1..32 B | T32 |
| 33..64 B | T64 |
| 65..128 B | T128 |
| 129..256 B | T256 |
| 257..512 B | T512 |
| 513..1024 B | T1K |
| 1025..2048 B | T2K |
| 2049..4096 B | T4K |
| 4097..8192 B | T8K |

```c
ucn_transfer_class_t transfer_class =
    ucn_transfer_smallest_class_for_length(length);
```

更大Class不会提升速度，只会放宽上限并增加可能需要的RAM能力。

## 2. 链接可选Archive

```cmake
target_link_libraries(my_firmware PRIVATE ucn_core ucn_transfer)
```

不用Transfer的产品不要链接它，这样不会承担重组buffer和重试状态。

## 3. 配置静态容量

在产品配置头中按需要覆盖：

```c
#define UCN_TRANSFER_MAX_MESSAGE_BYTES 2048U
#define UCN_TRANSFER_TX_SLOTS 1U
#define UCN_TRANSFER_RX_SLOTS 2U
#define UCN_TRANSFER_MAX_ENDPOINTS 4U
#define UCN_TRANSFER_MAX_PEERS 4U
#define UCN_TRANSFER_MAX_WINDOW 4U
```

`MAX_MESSAGE_BYTES`直接影响每个RX Slot的静态数据数组。不要为了“以后也许用到”让所有小MCU默认T8K。

## 4. 初始化

```c
static uint32_t transfer_now(void *context)
{
    return product_monotonic_ms_from_context(context);
}

ucn_transfer_config_t config = {
    .node = &g_node,
    .now_ms = transfer_now,
    .now_context = &g_clock,
    .fragment_data_limit = 0U,
    .max_retries = 3U,
    .ack_timeout_ms = 250U,
    .rx_timeout_ms = 5000U,
    .completed_hold_ms = 30000U,
    .recent_completion_ms = 5000U,
    .fallback_rx_handler = generic_rx,
    .fallback_rx_context = generic_context
};

check(ucn_transfer_init(&g_transfer, &config));
```

`now_ms`是唯一权威时间。不要同时传另一个缓存时间给Transfer。

## 5. 接收端绑定 Endpoint

```c
check(ucn_transfer_bind_endpoint(
    &g_transfer,
    PRODUCT_EP_PARAMETER_BLOCK,
    UCN_TRANSFER_CLASS_T2K,
    true,
    parameter_block_received,
    &parameter_store));
```

参数含义：

- `maximum_class`：该Endpoint接收上限；
- `require_e2e`：是否必须是受保护Transfer；
- handler：完整重组并CRC通过后调用；
- context：产品上下文。

接收回调：

```c
static void parameter_block_received(
    void *context,
    ucn_node_id_t source,
    ucn_session_id_t source_session,
    ucn_endpoint_t endpoint,
    ucn_transfer_class_t transfer_class,
    const uint8_t *data,
    uint16_t length,
    ucn_transfer_rx_handle_t handle)
{
    parameter_store_t *store = context;

    if (product_accept_parameter_copy(
            store, source, source_session, data, length)) {
        (void)ucn_transfer_release_received(&g_transfer, handle);
    }
}
```

T128..T8K完成消息占用RX Slot，应用处理完必须release。T32/T64直接回调的handle为 `UCN_TRANSFER_RX_HANDLE_DIRECT`。

## 6. 配置对端能力

```c
check(ucn_transfer_set_peer_capability(
    &g_transfer, remote_node, UCN_TRANSFER_CLASS_T2K));
check(ucn_transfer_set_tx_window_size(&g_transfer, 4U));
check(ucn_transfer_set_peer_window_capability(
    &g_transfer, remote_node, 4U));
check(ucn_transfer_set_peer_concurrency_capability(
    &g_transfer, remote_node, 1U));
```

窗口和并发是产品能力合同，不会自动协商。发送方配置不能超过接收方实际RX Slot和窗口能力。

## 7. 发送

```c
check(ucn_transfer_send(
    &g_transfer,
    remote_node,
    PRODUCT_EP_PARAMETER_BLOCK,
    UCN_TRANSFER_CLASS_T2K,
    parameter_bytes,
    parameter_length,
    transfer_completed,
    &pending_operation));
```

T128..T8K发送期间，调用者必须保持原Buffer内容不变，直到completion回调。Transfer不会为大消息再复制一份完整TX Buffer。

Completion状态：

| 状态 | 含义 |
| --- | --- |
| `SENT` | T32/T64已走单帧发送，不表示远端执行 |
| `DELIVERED` | 远端完整重组并ACK |
| `REMOTE_REJECTED` | 对端拒绝/无Slot/格式等 |
| `TIMEOUT` | 整体Deadline到期 |
| `RETRY_EXHAUSTED` | ACK重试耗尽 |
| `SEND_FAILED` | 本地发送终态失败 |

即使 `DELIVERED` 也只证明Transfer重组完成，不证明参数已应用或命令已执行。需要业务Result。

## 8. 推进顺序

为了不抢占Core Q0/Q1和维护工作：

```c
ucn_result_t core_result = product_protocol_owner_step();

if (core_result == UCN_ERR_NOT_FOUND) {
    ucn_result_t transfer_result = ucn_transfer_step(&g_transfer);
    product_record_transfer_result(transfer_result);
}
```

每次Transfer Step最多提交一个新片或重试片。高吞吐来自更频繁的Owner调度和合适窗口，不是单次循环无限drain。

## 9. Window选择

| Window | 特点 | 推荐 |
| ---: | --- | --- |
| 1 | Stop-and-Wait，RAM和乱序最小 | 初次接入、低速/低RAM |
| 2..4 | 改善高RTT链路吞吐 | 常用调优范围 |
| 8 | 更高在途量，丢片重传代价大 | 充分压测后使用 |

窗口增大不保证吞吐提升。单UART中继、Owner预算、Q0并发和Driver queue都可能成为瓶颈。

## 10. 完整性与安全

- 每个Core Frame有CRC16；
- 整条Transfer有CRC32，能检测分片错位和损坏；
- CRC不是身份认证；
- 防监听/篡改必须使用E2E Security；
- `require_e2e=true`时未保护消息应失败关闭。

## 11. 故障处理

| 情况 | 行为 |
| --- | --- |
| 路径MTU下降 | 后续片缩小，低于16 B失败 |
| RX Slot满 | 对端NO_SLOT/REMOTE_REJECTED |
| ACK丢失 | recent completion帮助重发ACK，不重复交付 |
| 中途断链 | 有界重试/超时，是否切路取决于Core策略 |
| 应用不release | Slot保持到completed hold timeout |
| Buffer被发送方修改 | CRC失败或业务内容错误，属于调用违规 |

## 12. 验收矩阵

- T32..T8K边界长度和超长拒绝；
- 1/2/3跳；
- Window 1/2/4/8；
- 丢首片、中间片、末片、ACK；
- RX Slot满、并发上限、应用延迟release；
- MTU变化、断链和切路；
- 与Q0命令和Q1传感器并发；
- Payload goodput、p99、重试、RAM和CPU。

敏感数据继续阅读 [安全 Provider](09-安全Provider与受保护通信.md)。
