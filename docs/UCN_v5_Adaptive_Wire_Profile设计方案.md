# UCN v5 Adaptive Wire Profile 设计方案

> 状态：V5-00/V5-01 已完成；V5-02 起仍按本文执行。已实现证据见 [V5-01 官方 Wire Profile Codec 实现报告](UCN_V5_01_官方Wire_Profile_Codec实现报告.md)。
>
> 原则：MCU-first、固定资源、官方固定格式、无 Linux 也能组网；Linux/Gateway 只是可选扩展节点。
>
> 迁移：v4 保持冻结归档；v5 使用独立 Git 分支和线协议版本，不在同一 Network 中静默混用。

## 1. 目标

v5 不再让所有帧固定携带 32 B 基础头，而是由协议官方定义四种 Wire Profile。Wire Profile 同时约束字段宽度、最大传播范围、允许扩展和控制面上限；用户只能选择官方档位，不能自定义 13 bit Node ID、37 Hop 或 19 bit Cost。

这项设计解决三个问题：

1. MCU 小包中 32 B 固定头占比过高；
2. 小型局部节点不应天然拥有骨干级广播、Hop 和控制预算；
3. Linux/高性能节点应能解析所有官方档位，但不应迫使每个本地小包使用最大头部。

## 2. 四种不同的“等级”必须分开

| 维度 | 名称 | 作用 |
| --- | --- | --- |
| 编译能力 | Nano / Lite / Full | 决定源码、状态表和 RAM/Flash 裁剪。 |
| 线编码 | W0 / W1 / W2 / W3 Wire Profile | 决定单帧字段宽度、编码长度和协议传播上限。 |
| 业务优先级 | Q0 / Q1 / Q2 / Q3 Traffic Class | 决定队列、实时性和交付语义。 |
| 安全权限 | Authorized Max Wire Profile | 决定一个身份最多能声明和使用哪个 Wire Profile。 |

因此 v5 使用 `UCN_WIRE_W0_LOCAL`～`UCN_WIRE_W3_BACKBONE`，不把 Wire Profile 叫作新的 Traffic Class，也不等同于 Nano/Lite/Full。

## 3. 官方 Profile

| Profile | 定位 | Network/Node/Session Wire 宽度 | 最大 Hop | 典型节点 |
| --- | --- | ---: | ---: | --- |
| W0 LOCAL | 小型局部域 | 1 B | 4 | 传感器、执行器、小 MCU。 |
| W1 EDGE | MCU 自组网默认档 | 2 B | 16 | STM32、ESP32、无人机节点。 |
| W2 MESH | 大型 MCU/SBC Mesh | 3 B | 64 | 高性能 MCU、SBC、区域 Leader。 |
| W3 BACKBONE | 完整地址/骨干 | 4 B | 254 | Linux、RK3588、Jetson、Gateway。 |

W3 暂定 254 Hop，而不是 255：当前 Hop 与路径计数均使用 `uint8_t`，255 仍需作为溢出/非法边界处理。若未来确需 255，必须单独修改计数语义和测试。

Node ID 的 `0` 保持非法；每种宽度的全 1 Wire 值表示广播并在内部展开为 `UCN_NODE_BROADCAST`。因此单一 W0 地址域最多只有 254 个可用 Node ID。

## 4. Version/Profile 字节

Wire Profile 不新增字节：

```text
Bit 7..6  Wire Profile Code (0..3)
Bit 5..0  Protocol Version (v5 = 5)
```

Decoder 读取 Magic 和该字节后即可选择唯一官方 Descriptor。未知版本、未知 Profile、保留位、非法扩展组合、字段超范围或实际长度不等于推导长度时直接拒绝，不尝试猜测自定义格式。

## 5. v5 基础头

固定部分：

```text
Magic                         2 B
Version + Wire Profile        1 B
Message Type                  1 B
Traffic Class + Flags         1 B
Hop Limit                     1 B
Sequence                      4 B
CRC-16                        2 B
                              ----
                              12 B
```

可变宽度部分为 Network、Source、Destination、Session 和 Payload Length：

| Profile | Network | Source | Destination | Session | Length | Base Header |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| W0 | 1 | 1 | 1 | 1 | 1 | 17 B |
| W1 | 2 | 2 | 2 | 2 | 1 | 21 B |
| W2 | 3 | 3 | 3 | 3 | 2 | 26 B |
| W3 | 4 | 4 | 4 | 4 | 2 | 30 B |

Header Size 字段取消；基础头、Route Epoch 和 Path ID 的实际长度由 Wire Profile 与 Flags 唯一推导。Traffic Class 使用 2 bit，Flags 使用 6 bit；未定义 Flag 必须为 0。CRC 始终位于实际 Header 的最后 2 B，并覆盖除 CRC 本身外的 Header、Payload 和可选 Tag。

实际可用 Payload 不是 Length 字段的理论最大值，而是：

```text
min(Profile Length 上限,
    UCN_MAX_FRAME_BYTES - Header - Tag,
    整条路径最小 MTU - Header - Tag,
    产品配置的 Payload 上限)
```

压缩头部默认用于减少 Wire 字节，不自动扩大 MCU 静态 Payload 缓冲。

## 6. 扩展字段

| 字段 | W0 | W1 | W2 | W3 | 说明 |
| --- | ---: | ---: | ---: | ---: | --- |
| Route Epoch | 1 B | 2 B | 2 B | 2 B | W0 必须使用安全的串行回绕比较。 |
| Path ID | 1 B | 2 B | 3 B | 4 B | `0` 非法；命名空间仍绑定 Source + Session。 |
| Payload Length | 1 B | 1 B | 2 B | 2 B | 仍受编译缓冲和 MTU 限制。 |
| Sequence | 4 B | 4 B | 4 B | 4 B | 不压缩，继续用于乱序、去重和 Replay 边界。 |
| CRC | 2 B | 2 B | 2 B | 2 B | 不压缩。 |

Route Cost 暂不直接冻结为 W0=8 bit。v4 Cost 是统一正数可加量且真实介质尚未完成标定，8 bit 可能连一个高代价 Link 都无法表示。v5 第一阶段保持现有 16 bit Cost；W2/W3 的 24/32 bit Cost 要在长路径、Cost 单位和饱和策略测试后再启用。

Request ID 第一阶段也保持现有 32 bit 控制载荷；缩成 1/2/3/4 B 前必须先定义回绕、活动请求冲突和 RREQ Cache 生命周期。

## 7. 地址与 Session 的第一阶段边界

Core 内部继续使用 32 bit Network/Node/Session。v5 第一阶段的 Wire 短字段是“数值必须能表示”，不是自动 Alias/NAT：

```text
Value 超过当前 Wire Profile 可表示范围
        ↓
提升 Profile；超过节点授权上限则 UCN_ERR_SCOPE/CONFIG
```

这样无需在小 MCU 中引入无界映射表，也不会产生跨 Gateway 地址歧义。

W0 的 254 地址是一个地址域的真实上限。要让 10000 个 W0 传感器复用局部地址，必须在 Extended 层另行实现 Domain ID、Gateway Directory、全局身份到局部 Alias、冲突/租约/双网关一致性和失效恢复；在这些任务完成前不得宣称“一个 W0 Network 支持 10000 Node”。

Session Slot 同样延期。若未来用短 Slot 映射完整 Session/Key Epoch，必须固定映射表容量、分配/回收、重启复用、轮换、表满、Replay 和安全存储语义。

## 8. HELLO 与 RREQ

v4 HELLO 的 4 B Payload 只重复 `frame.source`。v5 删除该重复字段，HELLO 允许零 Payload，仍检查 Source、Ingress Link、Network、Wire Profile 和准入策略。

v4 RREQ Payload 重复 Origin。v5 控制载荷压缩阶段删除该字段，Origin 使用 `frame.source`。目标格式为：

```text
Target + Request ID + Route Cost + Hop Count + RREQ Flags
```

但 RREQ 的 ID/Cost 宽度只有在回绕、Cost 单位和控制预算完成专项测试后才能按 Profile 缩短。

## 9. Hop、预算与权限

Wire Profile 是上限，不强迫每个 W1 帧都跑 16 Hop。源端和每个中继都必须验证 `1 <= hop_limit <= profile.max_hops`；非法帧直接拒绝，不修改成上限后继续转发。

控制面采用两层收紧：

1. 官方 Profile Ceiling：Hop、RREQ 并发、Token、允许扩展和逻辑转发 Fanout 的协议上限，产品不能提高；
2. Adapter/产品限制：可以根据 LoRa、CAN、Wi-Fi 等介质进一步收紧，不能突破官方上限。

具体 Token/Fanout 倍率不得先写死为 1x/4x/16x/64x，必须先在规模模拟和真实介质上标定。Hop 限制也不能替代 Jitter、逐 Peer Token、重复抑制和 Domain 边界；高密度网络中的 4 Hop 仍可能覆盖大量节点。

节点需要区分：

- 本机支持解析的最大 Wire Profile；
- 产品配置允许发送的最大 Wire Profile；
- 已认证 Origin 的授权上限；
- 直接 Ingress Peer 允许转发的上限。

只保存 `neighbor.max_wire_profile` 不能证明多跳帧的远端 Source 身份。受保护业务可由 E2E Provider 约束 Origin；RREQ 等控制帧还需要 S02 的逐跳/邻居控制面认证，当前仅有 CRC 时不能把 Profile Ceiling 宣称为抗伪造安全机制。

## 10. 安全与 Gateway

同一 Wire Domain 内，Version/Profile、Message Type、Traffic、不可变 Flags、Network、Source、Destination、Sequence、Session、Payload Length 和 Path ID 必须进入 AEAD AAD；Hop Limit 和 Route Epoch 保持逐跳可变。

Gateway 有两种模式：

### 安全终止模式

```text
W0/W1 解密 → ACL/业务检查 → W2/W3 重新编码并加密
```

最容易实现，但 Gateway 可以看到业务 Payload。

### 透明 E2E 模式

不能一边把 Wire Profile 放入 E2E AAD，一边直接修改 W0 Header 为 W3 Header而保持原 Tag。必须引入双层信封：

```text
Outer Wire Envelope
  Profile / Local Address / Hop / Outer Auth
  Gateway 可重封装

Inner E2E Envelope
  Canonical Global Source/Destination/Session/Endpoint
  Ciphertext + E2E Tag
  Gateway 不可修改
```

Inner Envelope、外层逐跳认证和 Gateway Directory 属于 Extended/Gateway 任务，不进入 v5 第一阶段小 Core。

V5-04 已用 W0 A→B→C 专项测试冻结上述边界：A/C 使用测试 E2E Provider，B 只有授权/转发能力且 `open_calls=0`；B 保持 Wire Profile、规范地址、Session、Sequence、Length 和密文/Tag 原样转发。复用 W0 Tag 后把帧重编码为 W1，或修改 Destination/Length，测试 Provider 均失败；直接修改线上密文则由 CRC 或目标 Open 拒绝。这里的 Provider 明确不是生产密码实现，不能替代 S02 的身份、审计 AEAD、密钥与逐跳认证。

## 11. 每包自动选级

节点能力上限不等于每包都使用最大 Profile，但自动选级分两个阶段：

- 已有 Route：按字段数值、Payload、扩展、Route Hop/最小 Profile 和授权上限选择最小 Profile；
- 未知 Route：无法预先知道实际 Hop，使用 Domain 默认 Profile或受限的 W0→W1→W2 扩展寻路；不能无限重试。

中继不得升级/降级一个已受 E2E 保护的帧。只有安全终止 Gateway 或未来 Outer Envelope Gateway 可以重新编码。

V5-05 已实现可选 Route-aware 自动选级，默认仍关闭。固定 TX Profile 同时是产品上限和 HELLO/RREQ 的域能力证明；HELLO 将固定档记录为每条 Link 的 Peer Ceiling，未知路由用固定档 RREQ 穿越成功后，后续更小档位满足单调接收条件。自动业务帧按地址/Session/Path/Route Epoch、实际 Route Hop、MTU 和可选 Tag 选择最小档；中继只转发已经明确的原档位，不升降已受保护帧。固定模式、控制面域探测和产品策略覆盖保持不变。

## 12. v4/v5 迁移

- v4 的 `Version=4` 字节保持冻结；v5 使用低 6 bit `Version=5` 和高 2 bit Profile。
- 普通 MCU 构建只包含一个当前协议 Decoder，避免双栈代码占用；需要迁移时由独立 Gateway/Host 双栈转换。
- v4/v5 不在同一 Network ID 中静默互通。
- v4 最终提交、Git 标签和本地源码备份是 v5 开发前置门禁。

## 13. 完成定义

每项 v5 任务仍遵守：

```text
任务登记 → 实现 → 单元测试 → 模拟/负向测试 → 资源测量 → 操作记录
```

Host Codec/CTest 只证明软件边界；ESP32/STM32 的实际 Wire 字节、吞吐、CPU、栈、Heap、功耗和共享介质波动必须另做实机验收。
