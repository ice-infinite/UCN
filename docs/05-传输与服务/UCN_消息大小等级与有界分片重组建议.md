# UCN 消息大小等级与有界分片/重组建议

> 状态：**Host 软件实现、默认兼容窗口 1/显式窗口 2～8 和一组 ESP32-S3 的 115200～5M UART 两跳九档门禁已完成；事件 Owner/窗口 8 在 3M 两跳三轮 27/27 完整交付，九档平均较窗口 4 再提高 10.0%；其他 Bearer/RTOS 性能与生产安全仍待实机门禁。**
> 创建日期：2026-08-13
> 实现日期：2026-08-13
> 对应任务：`EXT-01`～`EXT-06`（软件阶段）；`EXT-07`（实机阶段）。
> 关联：[UCN 整体架构设计](../02-总体架构/UCN_整体架构设计.md) · [UCN 协议分层与配置档案](../01-入门与使用/UCN_协议分层与配置档案.md) · [UCN Adapter 契约](../06-平台与适配/UCN_Adapter_契约.md)

## 1. 目的与结论

UCN 当前只能发送一帧内能够容纳的业务 Payload；`UCN_MAX_FRAME_BYTES` 默认是 `256 B`，但实际还要扣除 Wire Header、Route/Path 字段和可选的 `16 B` E2E Tag。因此它适合控制、IMU 和小遥测，但不能直接传送参数块、日志块或 OTA 数据块。

当前已增加的不是“无限大帧”，而是一个**按需链接、固定等级、固定 RAM 上界**的 `UCN-Extended Transfer` 模块：应用发送一个有等级上限的逻辑消息，模块根据实际发送结果把消息切为多个正常 UCN 帧；目标节点确认所有片段后，仅投递一次完整消息。

本方案固定九档逻辑消息上限：

```text
32 B → 64 B → 128 B → 256 B → 512 B → 1 KiB → 2 KiB → 4 KiB → 8 KiB
```

它们是**逻辑消息最大业务长度**，不是底层单帧长度，也不是必须填满的固定长度。比如 `T256` 可传 `129..256 B`；`T8K` 可传 `4097..8192 B`。应用应选择“刚好能容纳消息的最小等级”。

## 2. 三种“等级”必须分开

| 名称 | 当前/后续符号 | 作用 | 是否决定逻辑消息长度 |
| --- | --- | --- | --- |
| Wire Class | `W0`～`W3` | 地址、Payload Length、Route/Path/Cost 等字段在线上的宽度。 | 否。 |
| QoS / Traffic Class | 当前 `Q0`、`Q1`，未来可选 Q2/Q3 | 调度优先级、Deadline、队列行为。 | 否。 |
| **Transfer Class** | `T32`～`T8K` | 一个逻辑消息可占用的最大业务字节数，以及是否允许分片。 | **是。** |

因此不得把 `T8K` 写入现有 v5 基础头，也不得把它误当成 W3 或 Q1。普通单帧业务保持现有编码和现有字节数；只有实际分片的业务才会使用新的 Extended Transfer 信封。

## 3. 固定 Transfer Class 表

| Class | 建议枚举 | 最大逻辑消息长度 | 最多片段数 | 默认调度规则 | 典型用途 |
| --- | --- | ---: | ---: | --- | --- |
| T32 | `UCN_TRANSFER_CLASS_T32` | 32 B | 1 | Transfer API 默认 Q1；**禁止分片**。Q0 直接用 Node API。 | 急停、舵机目标、短控制命令。 |
| T64 | `UCN_TRANSFER_CLASS_T64` | 64 B | 1 | Transfer API 默认 Q1；**禁止分片**。Q0 直接用 Node API。 | IMU 小包、状态字、命令结果。 |
| T128 | `UCN_TRANSFER_CLASS_T128` | 128 B | 8 | Q1 Fragment；Core 空闲时推进。 | 普通遥测、参数读写。 |
| T256 | `UCN_TRANSFER_CLASS_T256` | 256 B | 16 | Q1 Fragment；Core 空闲时推进。 | 传感器批量数据、小诊断。 |
| T512 | `UCN_TRANSFER_CLASS_T512` | 512 B | 32 | Q1 Fragment；Core 空闲时推进。 | 参数块、结构化状态。 |
| T1K | `UCN_TRANSFER_CLASS_T1K` | 1,024 B | 64 | Q1 Fragment；Core 空闲时推进。 | 参数集、小日志块。 |
| T2K | `UCN_TRANSFER_CLASS_T2K` | 2,048 B | 128 | Q1 Fragment；Core 空闲时推进。 | 日志块、配置导入。 |
| T4K | `UCN_TRANSFER_CLASS_T4K` | 4,096 B | 256 | Q1 Fragment；Core 空闲时推进。 | 较大日志/配置块。 |
| T8K | `UCN_TRANSFER_CLASS_T8K` | 8,192 B | 512 | Q1 Fragment；Core 空闲时推进。 | 小型 OTA 块或文件块。 |

规则固定如下：

1. `T32`、`T64` 必须完整装入一帧；不够则立即拒绝，绝不拆成片段。
2. `T128`～`T8K` 统一使用 Transfer 信封；能够装下时只有一个 Fragment，装不下时继续切为多个 Fragment。
3. Fragment 固定使用 Q1 业务类型；**Q0 永远不分片**。Protocol Owner 必须先处理 Core Q0/Q1/维护，只有本轮 Core 空闲时才调用 `ucn_transfer_step()`。
4. `最多片段数 = 最大长度 / 16 B`。每片至少携带 `16 B` 真实业务数据；小于此值的路径不支持 Extended Transfer，防止 8 KiB 消息被拆成数千片。
5. Endpoint 的允许等级、是否允许分片、是否要求 E2E 保护，都由产品静态配置；节点不会因为收到一个更大等级的帧而自动扩容。

## 4. 单帧、分片与 MTU 的关系

`T256` 不代表一定能在任意链路上一次发送 256 B。真正的可用单帧 Payload 为：

```text
min(UCN_MAX_FRAME_BYTES, 当前 Path 的全部 Bearer 有效 MTU)
  - 当前 Wire Header（普通 / Route / Path）
  - E2E Tag（未加密为 0，加密为 16 B）
```

分片帧还要再扣除固定 `14 B` 的 Transfer 信封：

```text
单片业务数据上限 = 上述可用单帧 Payload - 14 B
```

例：ESP-NOW `250 B`、W3 Path、启用 E2E 时，单片最大业务数据约为：

```text
250 - 36（W3 Path Header）- 16（Tag）- 14（Transfer Header）= 184 B
```

所以完整 `T8K` 消息约需 `ceil(8192 / 184) = 45` 个片段。中继节点只转发这些小帧，**不保存 8 KiB 消息，也不重组**。

反例：若某条端到端路径在当前 Wire/Path/安全条件下，分片信封后可容纳不足 `16 B` 数据，则该路径拒绝 Extended Transfer。此限制尤其影响 `64 B` CAN-FD 或更小 MTU；不能假装它们一定能安全承载 `T8K`。

## 5. 已实现的分片线协议

本节是当前 `include/ucn/ucn_transfer.h` 与 `src/extended/ucn_transfer.c` 已实现的格式。它不修改 v5 基础 Header，但占用两个新的普通路由消息类型。

### 5.1 新报文类型

后续从当前 `0x20/0x21` 数据类型之后预留：

```text
UCN_MSG_TRANSFER_FRAGMENT = 0x22
UCN_MSG_TRANSFER_ACK      = 0x23
```

它们使用已有 Node 路由、Hop、Path、CRC、Security Provider 和 E2E 机制；不是新的底层 Link 协议。中继只根据现有外层 Destination/Route/Path 转发，不能解析或重组受保护的内容。

### 5.2 `TRANSFER_FRAGMENT` 的 14 B Payload 信封

| 字节 | 字段 | 规则 |
| ---: | --- | --- |
| 0 | `format` | 首版固定为 `1`。 |
| 1 | `target_endpoint` | 完整消息最终投递的静态 Endpoint。 |
| 2 | `transfer_class` | `T32`～`T8K` 的固定枚举；接收端据此做上限检查。 |
| 3 | `flags` | `START=bit0`、`END=bit1`，其它位为 0。 |
| 4..5 | `transfer_id` | 源端在当前 Session 内递增的非零 `uint16_t`。 |
| 6..7 | `total_length` | 大端 `uint16_t`，必须为 `1..8192` 且不超过声明 Class。 |
| 8..9 | `fragment_offset` | 大端 `uint16_t`，首片必须为 0。 |
| 10..13 | `message_crc32` | 完整明文消息的 CRC32；所有片段一致。 |
| 14..N | `fragment_data` | 当前片的业务字节，长度必须至少为 1；分片模式要求至少 16。 |

完整重组 Key 固定为：

```text
(source_node_id, source_session_id, target_endpoint, transfer_id)
```

接收端首片必须是 `START + offset=0`，并始终保持**严格顺序、零乱序缓存**：下一片的 `fragment_offset` 必须等于已接收长度。乱序片不缓存，直接 ACK 当前 `next_expected_offset`；重复片也只重发同一个累计 ACK。这样即使发送端显式使用窗口 2～8，接收端仍无需 Bitmap、片段链表或每窗口 Payload 副本。

### 5.3 `TRANSFER_ACK` 的 8 B Payload

| 字节 | 字段 | 规则 |
| ---: | --- | --- |
| 0 | `format` | 固定为 `1`。 |
| 1 | `target_endpoint` | 必须匹配待确认的 Transfer。 |
| 2..3 | `transfer_id` | 必须匹配。 |
| 4..5 | `next_expected_offset` | 已可靠接收的下一个字节偏移。 |
| 6 | `status` | `OK`、`NO_SLOT`、`BAD_FORMAT`、`INTEGRITY_FAIL`、`EXPIRED`、`REJECTED`。 |
| 7 | `reserved` | 必须为 0。 |

`next_expected_offset` 是累计 ACK：它之前的连续字节均已可靠接收。窗口 1 时仍保持原 Stop-and-Wait 行为；显式窗口大于 1 时，发送端可在该累计偏移之后保留最多 N 个在途 Fragment。ACK 不前进或超时会从已确认偏移开始执行一次有界 Go-Back-N；永久 NACK、消息 Deadline 或恢复轮耗尽会终止本逻辑消息并报告原因。

## 6. 调度、可靠性与实时性边界

默认仍采用窗口 1；产品只有在本机和 Peer 都显式配置能力后才使用固定窗口 2～8。它不引入乱序 Bitmap、选择重传或无限重传：

1. 正常 Q0、Heartbeat、必要路由维护始终优先于 Transfer Fragment。
2. 一个 Protocol Owner 单次 `ucn_transfer_step()` 最多提交 **1 个**新片或重传片；循环足够快时连续 Step 填充固定窗口。产品循环仍应先调用 Core Step，Core 空闲后再调用 Transfer Step。
3. 缺口/超时默认最多进行 3 次恢复轮；当前 ACK 超时固定为 250 ms，可由产品配置覆盖。动态 RTT 计算尚未接入。
4. 整条逻辑消息有绝对 Deadline：T128/T256/T512/T1K/T2K/T4K/T8K 默认分别为 `1/2/4/8/15/30/45 s`。超时不续命。
5. Endpoint 若声明“严格实时”或 Q0，只允许 T32/T64 单帧；否则配置阶段失败。

这意味着 Transfer 解决的是“有界的大消息”，不是硬实时、不保证高吞吐，也不适合视频流。OTA/文件必须按 `T8K` 块逐块完成；每块有独立 `transfer_id`，不能一次申请无限大的文件缓存。

## 7. 固定资源与产品配置

默认 Core 继续是零分片、零大缓冲：

```c
target_link_libraries(my_product PRIVATE ucn_core) /* 不链接 ucn_transfer */
```

`ucn_transfer` 是 `EXCLUDE_FROM_ALL` 的独立静态库。仅编译 `ucn_core` 时不会构建 Transfer Archive，`ucn_node_t` 也没有 Transfer 字段。需要大消息的产品才链接它、静态创建 `ucn_transfer_t`，并在产品配置头裁剪。例如一个最多接收 2 KiB 参数块的主控：

```c
#define UCN_TRANSFER_MAX_MESSAGE_BYTES       ((size_t)2048U)
#define UCN_TRANSFER_TX_SLOTS                1
#define UCN_TRANSFER_RX_SLOTS                2
#define UCN_TRANSFER_RECENT_COMPLETIONS      4
#define UCN_TRANSFER_MAX_RETRIES             3
#define UCN_TRANSFER_MAX_WINDOW              2
```

RAM 的首要预算近似为：

```text
RX Slots × UCN_TRANSFER_MAX_MESSAGE_BYTES + 固定 Slot/Endpoint/Peer 元数据
```

Host GCC 64-bit 当前实测对象大小：默认 `1 × 8192 B RX`、编译最大窗口 8 的 `sizeof(ucn_transfer_t)=8824 B`；产品测试配置 `2 × 512 B RX`、编译最大窗口 2 为 `1696 B`。ESP32-S3 目标日志中对象为 8700 B；窗口状态原本已按协议上限固定，不复制 Fragment Payload，因此把编译门限从 4 放宽到 8 没有把对象按比例放大。这仍只是当前 ABI 数值，其他目标 MCU 必须用目标编译器重新测量。

因此 `2 × T8K` 至少需要 16 KiB 接收缓冲，明显不该默认编进小 STM32、ESP32 传感器或 Nano 节点。建议配置档案：

| 节点类型 | 推荐最大等级 | RX Slots | 说明 |
| --- | --- | ---: | --- |
| 小传感器/执行器 | T32 或 T64 | 0 | 使用现有 Endpoint 单帧 API，不链接 Transfer。 |
| 普通中继 | 不绑定 Transfer Endpoint | 0 | Core 只转发 Fragment，不创建 `ucn_transfer_t`，不占大包 RAM。 |
| 参数/诊断主控 | T512 或 T2K | 1～2 | 仅为需要的 Endpoint 开启接收。 |
| 网关/OTA 主控 | T4K 或 T8K | 2 | 必须测量 RAM、栈、CPU 与空口占用。 |

`TX/RX_SLOTS` 是“独立消息并发数”，`UCN_TRANSFER_MAX_WINDOW` 是“一条消息内
未确认 Fragment 数”，两者不能混为一个参数。默认 Peer 消息并发为 1；只有产品
确认对端至少有相同数量 RX Slot 后，才调用
`ucn_transfer_set_peer_concurrency_capability()`。四槽 ESP32 实测显著改善 T128～T1K
多跳吞吐，但 4/8 KiB 绝对吞吐回退且接收端增加约 24.1 KiB RAM，因此不是统一默认，
详见[V5-66 报告](../08-实现与验证/版本演进/UCN_V5_66_有界多消息并发Transfer优化.md)。

完成重组后，RX Slot 变为“已完成、等待 Endpoint 消费”状态。Protocol Task 只向目标任务投递一个固定 Slot Index；任务通过新 API 取得只读 Buffer，处理后必须显式 `release()`。这避免把 8 KiB 再复制到当前默认 `32 B` Service Inbox，也避免在协议任务中执行耗时业务。

## 8. 准入、安全、路径与兼容性

- 未启用 Transfer、Endpoint 未授权、等级超过 Endpoint 上限、未满足 E2E 要求、没有 RX Slot、MTU 不足或 Class 不匹配时，必须失败关闭；不得降级成无约束大包。
- 每个 Fragment 和 ACK 都复用 Node 的当前全局安全策略；中继不解密。Endpoint Binding 可要求 E2E，明文会失败关闭。当前尚未把 Fragment 内的 `target_endpoint` 自动映射为原静态 Endpoint 的独立发送策略；需要分 Endpoint 安全等级的产品应在实机发布前补充策略映射。完整 `message_crc32` 只用于检测拼接错误，不能替代认证。
- 分片首片只有在现有 Provider/E2E、Endpoint ACL、长度、Class、MTU 和 Slot 容量全部通过后才分配 RX Slot；无效/乱序片不分配内存。
- 完成的 `(Key, message_crc32)` 放入固定 `Recent Completion` 表一段时间。终端重复的最后一片只重发 ACK，不会第二次执行 Endpoint。
- 普通 v5 单帧、既有 Header、既有 `DATA_Q0/Q1` 语义完全不改。Transfer 仅在产品为对端静态配置“支持的最大 Class”后才发送 `0x22/0x23`；旧节点不会被主动发送未知 Transfer 帧。
- `ucn_transfer_init()` 后本机 Fragment 窗口为 1；Peer Class 建立时对端窗口和消息并发也都默认为 1。只有分别显式调用窗口/并发 API 才会放宽，因此未知或未升级的 v5 Transfer Peer 保持原行为。
- 首版不修改 HELLO 长度做动态能力协商，避免重演控制帧兼容问题。后续若确实需要自动发现 Transfer 能力，必须单独设计版本协商/可选扩展并做旧 v5 互通测试。

## 9. 当前 API 与 Endpoint 合同

现有 `ucn_node_send_endpoint()` 保持为单帧 API，不因加入 Transfer 改变成功语义。新增独立 API：

```c
ucn_result_t ucn_transfer_send(
    ucn_transfer_t *transfer,
    ucn_node_id_t destination,
    ucn_endpoint_t target_endpoint,
    ucn_transfer_class_t transfer_class,
    const uint8_t *data,
    uint16_t length,
    ucn_transfer_completion_fn completion,
    void *completion_context);
```

该 API 的 `UCN_OK` 对 T128～T8K 只表示“TX Transfer Slot 已接受”，**不表示远端重组、投递或执行成功**；最终结果由 Completion 回调给出。T32/T64 复用原单帧 Endpoint 路径，Completion 为 `SENT`，仍不代表远端任务已执行。

初始化和发送顺序固定为：

```c
static ucn_transfer_t g_transfer;

static uint32_t transfer_now_ms(void *context)
{
    (void)context;
    return product_monotonic_ms();
}

ucn_transfer_config_t cfg = {0};
cfg.node = &g_node;
cfg.now_ms = transfer_now_ms;
cfg.now_context = NULL;
ucn_transfer_init(&g_transfer, &cfg);
ucn_transfer_bind_endpoint(&g_transfer, 0x80,
                           UCN_TRANSFER_CLASS_T2K, false,
                           on_complete_message, NULL);
ucn_transfer_set_peer_capability(&g_transfer, remote_node_id,
                                 UCN_TRANSFER_CLASS_T2K);

/* 可选性能模式：默认不调用时，本机和 Peer 都保持窗口 1。 */
ucn_transfer_set_tx_window_size(&g_transfer, 4U);
ucn_transfer_set_peer_window_capability(&g_transfer, remote_node_id, 4U);

/* 可选：只有接收端固定 RX Slot 足够时才提高；默认仍为 1。 */
ucn_transfer_set_peer_concurrency_capability(&g_transfer,
                                             remote_node_id, 2U);

/* data 在 Completion 前必须保持只读有效。 */
ucn_transfer_send(&g_transfer, remote_node_id, 0x80,
                  UCN_TRANSFER_CLASS_T512,
                  data, data_length, on_send_complete, NULL);

/* 唯一 Protocol Owner 循环：Core 优先，空闲时才推进一个 Fragment。 */
if (ucn_protocol_owner_step(&g_owner, now_ms) == UCN_ERR_NOT_FOUND) {
    (void)ucn_transfer_step(&g_transfer);
}
```

V5-62 起 `cfg.now_ms` 是强制的权威单调毫秒时钟；Send、RX、ACK/重试和 Step 都从它采样当前值，Step 不再接收调用方缓存时间。该项属于预发布源码/ABI 破坏，旧 Transfer 初始化和旧函数调用必须迁移并全量重编译，但 Fragment/ACK Wire 不变。

`ucn_transfer_init()` 会占用 Node 的通用 RX Handler 来处理 `0x22/0x23`；已有通用 Handler 必须通过 `fallback_rx_handler/context` 传入。静态 Endpoint Handler 不受影响。完成的分片消息回调拿到非零 Handle，消费后必须调用 `ucn_transfer_release_received()`；T32/T64 的 Handle 固定为 0，只在回调期间借用原帧 Payload。

## 10. 实施任务与验收顺序

| ID | 内容 | 完成门禁 |
| --- | --- | --- |
| EXT-01 | 九档/资源/兼容契约。 | **软件完成**：独立 Header/Target，不改 v5 基础 Header 和 `ucn_node_t`。 |
| EXT-02 | Codec 与 CRC32。 | **软件完成**：14 B Fragment、8 B ACK、格式/长度/保留位/CRC 向量。 |
| EXT-03 | 固定 TX/累计 ACK 窗口。 | **当前完成**：默认 Fragment 窗口 1、显式窗口 2～8；默认消息并发 1、静态 Peer 显式并发；旧 Peer 退化、丢 START/中间片 Go-Back-N、ACK 丢失、重试耗尽和最小 MTU 失败关闭；持续 Q0 压力仍待专项。 |
| EXT-04 | 固定 RX/Recent/Handle。 | **首版完成**：CRC 错、Slot 满、多 Slot、超时、代际 Handle、防重复执行；四 RX Slot 已完成 ESP32 UART 三跳 81 阶段门禁，但大消息快速路径仍待优化。 |
| EXT-05 | 安全/MTU/路径/旧节点。 | **部分完成**：静态 Peer Class/窗口 Capability、明文要求拒绝、64/128/250/256 B MTU、两跳透明中继；自动能力/Path MTU 协商、Endpoint 独立 TX 安全策略、Path 切换和旧固件实机仍待完成。 |
| EXT-06 | 软件/资源门禁。 | **当前完成**：Full/Lite/Nano、窗口 1/4 与丢首片/中间片、512 B/双 RX 槽/最大窗口 2 产品配置、Core-only、ASan/UBSan、`-fanalyzer`；更大规模并发与目标 CPU 仍待补。 |
| EXT-07 | 实机门禁。 | **部分完成**：当前三板已完成 115200～5M UART 两跳九档、3M 窗口 1/4 和事件 Owner/窗口 8。最终事件固件三轮 27/27、0 重试，九档平均 `15.712→17.283 KiB/s`（较窗口 4 +10.0%），T8K=29.480 KiB/s；Direct 语义未改变。ESP-NOW-only、单跳、切换中 Transfer、多源并发/Q0、CAN-FD、CPU/功耗/长稳仍待执行。 |

当前可以表述为“已提供可选的有界消息分片/重组模块，默认 Fragment 窗口和 Peer 消息并发均为 1，窗口可显式放宽到 2～8，并可按静态产品能力启用有界多消息并发；一组 ESP32-S3 已完成 115200～5M UART 两跳九档、3M 窗口/事件 Owner 门禁和四节点 1～3 跳并发对比”。四路并发明显改善 T128～T1K 多独立消息及多跳 T2K，但会降低单个 T4K/T8K 顺序大消息吞吐，因此默认配置不变。不得据此表述为“已验证全部 Wi-Fi/UART/CAN 大包性能”，也不得把固定窗口或多消息并发说成完整文件传输/OTA 系统。详见 [V5-52 功能报告](../08-实现与验证/版本演进/UCN_V5_52_N8R8三节点Transfer实测报告.md)、[V5-53 压力报告](../08-实现与验证/版本演进/UCN_V5_53_三节点Transfer压力测试报告.md)、[V5-54 921600 报告](../08-实现与验证/版本演进/UCN_V5_54_921600波特率三节点Transfer压力测试报告.md)、[V5-55 极限报告](../08-实现与验证/版本演进/UCN_V5_55_UART极限波特率扫描报告.md)、[V5-56 固定窗口报告](../08-实现与验证/版本演进/UCN_V5_56_固定窗口Transfer优化与三节点对比报告.md)、[V5-57 事件 Owner 报告](../08-实现与验证/版本演进/UCN_V5_57_事件驱动Owner与窗口8三节点报告.md)与 [V5-66 并发报告](../08-实现与验证/版本演进/UCN_V5_66_有界多消息并发Transfer优化.md)。

## 11. 反向核对清单

实现每一项前后都核对：

1. 九档是否仍精确为 `32/64/128/256/512/1K/2K/4K/8K`，无额外隐式等级。
2. Core/Transfer OFF 是否仍没有分片状态、8 KiB Buffer 或额外 Wire 开销。
3. T32/T64/Q0 是否永远不走分片。
4. 每个 RX Slot、TX Slot、完成记录、重试、Deadline、最大片段数是否都有编译期上限。
5. 中继是否始终零重组缓存、只转发单帧。
6. 任何大消息失败是否可观测并回收，且不会重复执行控制 Endpoint。
7. 真实端到端表现是否和“软件模拟”分栏记录，不能从 Host 测试推导实际 Wi-Fi/CAN 吞吐。
