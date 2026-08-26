# Frame 与 Wire Profile API

> 文档级别：`REFERENCE`
> 实现状态：`CURRENT`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：include/ucn 公共头、链接目标与公共 API 测试
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：API 合同不等同于各平台实机驱动完成

Frame API 负责 Core Wire v5 的编码、解码、长度探测和 Profile 合法性。调用顺序是先根据固定头识别 Wire Class/总长，再确认接收缓冲区足够，最后严格解码。

编码器要求字段、payload 长度和目标 Profile 一致；解码器拒绝未知版本、非法 class、截断、超长、保留位和 CRC/安全合同错误。成功后才写入完整 frame view。

Wire Class 描述的是线上头宽与能力，不等于产品 Profile。节点应以本地编译能力和远端协商结果共同决定发送格式，不能自动降级解析 malformed 帧。

## Wire Class 与头长度

| Class | API 值 | 基础头 | Route 扩展头 | Path 扩展头 | 典型用途 |
| --- | ---: | ---: | ---: | ---: | --- |
| W0 Local | 1 | 17 B | 18 B | 19 B | 小地址/近端 MCU |
| W1 Edge | 2 | 21 B | 23 B | 25 B | 边缘网络 |
| W2 Mesh | 3 | 26 B | 28 B | 31 B | 中等 Mesh |
| W3 Backbone | 4 | 30 B | 32 B | 36 B | 完整 32-bit 域 |

E2E 保护还需要固定 16 B tag。`ucn_frame_max_payload_for_profile()` 会同时考虑 flags 和 `UCN_MAX_FRAME_BYTES`，应用不应自己硬编码“256-30”。

## 编码流程

```c
ucn_frame_t frame = {0};
uint8_t wire[UCN_MAX_FRAME_BYTES];
size_t wire_len = 0U;

frame.message_type = UCN_MSG_DATA_Q1;
frame.wire_profile = UCN_WIRE_PROFILE_W1_EDGE;
frame.traffic_class = UCN_TRAFFIC_Q1_REALTIME;
frame.hop_limit = 8U;
frame.network_id = network_id;
frame.source = local_id;
frame.destination = remote_id;
frame.sequence = sequence;
frame.session_id = session_id;
frame.payload = payload;
frame.payload_length = payload_len;

ucn_result_t rc = ucn_frame_encode(&frame, wire, sizeof(wire), &wire_len);
```

调用前必须保证 payload 指针/长度一致、地址能被所选 Class 表示、flags 与 route/path/tag 字段一致。编码成功才可使用 `wire_len`；失败时不要发送 buffer 中的旧数据。

如果希望自动选最小表示，可先调用 `ucn_frame_select_min_wire_profile()`，同时给出本地 maximum 和 Link MTU。选择器只解决“能否表示并装入”，不负责确认远端支持；Node 的自动选择还会结合 HELLO/路由能力。

## 解码流程

```c
ucn_wire_profile_t profile;
size_t exact_len;
ucn_frame_t decoded;

rc = ucn_frame_peek_wire_profile(input, available, &profile);
if (rc != UCN_OK) return rc;
rc = ucn_frame_peek_encoded_size(input, available, &exact_len);
if (rc != UCN_OK || exact_len > available) return UCN_ERR_MALFORMED;
rc = ucn_frame_decode(input, exact_len, &decoded);
```

`peek_*` 只做前缀/长度级检查，用于 CAN-FD padding 或 Stream framing；它不证明 CRC、地址、安全或完整语义有效。只有 `decode` 成功后才使用 `decoded`。对于无 padding 的 Carrier，通常要求 `exact_len == available`；CAN-FD 可额外验证剩余 padding 全零。

## AAD 与安全

E2E Provider 使用 `ucn_frame_write_e2e_aad()` 生成固定 30 B 元数据绑定。调用者不能自建另一个字段顺序；Source、Destination、Session、Sequence、Endpoint/Message、Path 等字段被改变后，目标验证必须失败。

## 常见错误

- 用 Build Profile 代替 Wire Class，认为 Nano 只能收 W0；当前设计允许小节点保留 W3 RX，但高级功能仍不会被编译出来；
- 把 `ucn_frame_peek_encoded_size()` 当完整验证；
- payload 超过单帧后直接扩大 `UCN_MAX_FRAME_BYTES`，而不是评估 MTU/Transfer；
- 接收未知/错误版本后尝试其他长度解析；
- 保护帧设置 flag 却没有 tag，或明文帧残留 auth pointer。

## 验证要求

每个 Class/flags 组合应有 golden、最小/最大地址、最大 payload、39/41 等长度负例、未知版本/保留位、CRC、全输出不写回和 Release 优化构建测试。
