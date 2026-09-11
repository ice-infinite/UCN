# UCN v6 低开销统一 Wire：各 Contract 字段与运行机制详细设计

> 状态：CURRENT WIRE CANDIDATE / SELF-REVIEWED SEMANTICS / EXTERNAL FREEZE REQUIRED，精确 Registry/Golden 尚未冻结、尚未实现
>
> 日期：2026-09-06
>
> 上位方案：[UCN v6 低开销统一 Wire 重构多方案评估](UCN_v6_低开销统一Wire重构多方案评估.md)
>
> 用户意图、自动选型、Endpoint Policy 与高级覆盖接口：[UCN v6 面向用户意图的自动传输策略与配置接口详细设计](UCN_v6_面向用户意图的自动传输策略与配置接口详细设计.md)
>
> 模块边界、唯一 Owner、依赖与关闭行为：[UCN v6 可裁剪模块边界、依赖、资源与静态装配详细设计](UCN_v6_可裁剪模块边界依赖资源与静态装配详细设计.md)
>
> 目标：逐项说明低开销 v6 中每种 Wire Contract 包含什么、字段数量、精确基础长度、如何建立、如何发送、如何转发、如何失效，以及 Cluster、Realtime、Transfer 和 Security 如何按需启用
>
> 兼容性：UCN 尚未正式发布，本设计不兼容 v4、v5 或当前 v6 Contract 1；旧实现只保留在 Git 历史中
>
> 权威关系：本文取代最终架构 RFC 原第 6 章的 9 B 前缀候选，成为当前唯一的线上字段候选来源；最终架构 RFC 继续拥有顶层安全和 MCU-first 不变量。本文在完成 Golden Bytes、逻辑模型一致性与独立外审前仍不得驱动生产 Codec、RX 或 TX

## 1. 本文解决什么问题

本文不再只讲“可以压缩”，而是给出一套可以继续细化为 Golden Bytes 的具体协议骨架。

本文唯一负责线上字段、Contract、事务键、Context 恢复和协议交互。模块依赖以模块化设计为准，用户生命周期以 Intent/API 设计为准；跨文档条款必须同步一致，不能由实现者任选一份。

需要同时满足四个目标：

1. 最简单的节点只需要自动寻路和发送数据，不承担 Cluster、Realtime、Transfer 或 E2E Encryption 的固定头开销；
2. 高频流、稳定多跳路径能够用 Context 和 Label 消除重复字段；
3. 无线或不可信中继仍有真实的认证、防篡改和重放保护；
4. Full、Lite、Nano、CAN、UART、无线和未来 FPGA 转发站仍属于同一个 v6 协议。

本文把线上数据分为三层：

```text
┌───────────────────────────────────────────────┐
│ Carrier Frame                                │
│ 帧边界、Carrier Length、Native CRC/FCS、DLC   │
├───────────────────────────────────────────────┤
│ Optional Hop Protection Trailer              │
│ Hop Sequence + Hop Authentication Tag        │
├───────────────────────────────────────────────┤
│ UCN Core Packet                              │
│ 3 B Common Header + C0..C5 固定 Contract      │
│ + Payload + Optional Origin Tag              │
└───────────────────────────────────────────────┘
```

三层职责必须严格分开：

- Carrier 解决“这一帧从哪里开始、到哪里结束”；
- Hop Protection 解决“当前一跳是谁发来的、是否被重放或篡改”；
- Core Packet 解决“要送到哪里、是什么业务、最终接收者如何解释”。

只有 Stream Carrier 需要额外 Sync/Length；CAN、CAN-FD、Datagram、ESP-NOW 等已有边界的 Carrier 不重复携带 Core Magic 和 Core Length。

## 2. 统一协议的公共 3 B Header

所有 C0～C5 都以完全相同的 3 B 开头。

### 2.1 Byte 0：VersionContract

| Bit | 字段 | 宽度 | 规则 |
| --- | --- | ---: | --- |
| 7..4 | Protocol Version | 4 bit | v6 固定为 `6` |
| 3..0 | Header Contract | 4 bit | C0～C5；其余保留并拒绝 |

逻辑字段数：2；物理长度：1 B。

Version 与 Contract 一次读取即可完成严格分派。Decoder 不允许：

- 看到未知 Contract 后尝试按其他 Contract 解析；
- 看到旧版本后回退到 v4/v5；
- 根据 Frame 长度猜测版本；
- 忽略高位或保留值。

### 2.2 Byte 1：MessageMeta

| Bit | 字段 | 宽度 | 取值 |
| --- | --- | ---: | --- |
| 7..6 | Traffic Class | 2 bit | Q0、Q1、Q2、Q3 |
| 5..4 | Delivery Guarantee | 2 bit | Best Effort、Latest、Reliable、Reserved |
| 3..2 | Interaction Role | 2 bit | One Way、Request、Result、Error |
| 1..0 | Payload Kind | 2 bit | Data、Control、Transfer、Diagnostic |

逻辑字段数：4；物理长度：1 B。

约束：

- `Reserved Delivery` 无条件拒绝；
- `Payload Kind=Control/Diagnostic` 的 operation code 位置由 11.0 固定；除 C0 已在 Header
  携带外，其余允许的 Contract 都以 2 B code 作为 Payload 第一字段；
- 非 Transfer 的 `Request/Result/Error` Payload 必须包含 Operation Envelope；
- `Payload Kind=Transfer` 时，Operation/业务关联只在先行 `TRANSFER_SETUP` 中出现，普通
  Fragment 不重复 Operation Envelope；Fragment 的 Interaction、Service 和父上下文必须与
  Setup 精确一致；
- C3 只允许 `Best Effort + One Way`；需要 Latest、Reliable、Request 或 E2E 时使用 C4；
- Control 不能因为 Q0 就自动获得 Authority，Authority 必须由本地状态机另行证明。

### 2.3 Byte 2：OriginSecurityHop

| Bit | 字段 | 宽度 | 取值 |
| --- | --- | ---: | --- |
| 7..6 | Origin Security | 2 bit | O0 None、O1 Auth、O2 AEAD、O3 Reserved |
| 5..0 | Hop Limit | 6 bit | `1..63`；0 拒绝 |

逻辑字段数：2；物理长度：1 B。

Origin Security 含义：

| Profile | Origin Tag | Payload | 用途 |
| --- | ---: | --- | --- |
| O0 | 0 B | 明文 | 无 E2E 要求；仍可有 Hop Auth |
| O1 | 16 B | 明文 | 端到端/Group 完整性和来源认证 |
| O2 | 16 B | 密文 | 端到端/Group AEAD |
| O3 | — | — | 保留，收到即拒绝 |

Hop Limit 不进入 Origin 不可变认证域，因为中继必须递减它；它必须进入 Hop Authentication 域。

`1..63` 足以覆盖 MCU Mesh 的预期范围，并节省 1 B。若未来确有超过 63 Hop 的实际产品需求，应新增经过审计的 Contract，而不是提前让所有帧永久多 1 B。

### 2.4 公共 Header 汇总

| 项目 | 数量 |
| --- | ---: |
| 逻辑字段 | 8 |
| 物理字节 | 3 B |
| 所有普通帧固定成本 | 3 B |

这 3 B 已经表达版本、格式、QoS、交付方式、交互方式、Payload 类型、端到端安全和最大转发范围，不再另加 Flags 字节。

### 2.5 通用编码规则

- 所有多字节整数使用网络字节序；
- Core Packet 不允许隐式结构体 padding；
- Payload Length 由 `Carrier Length - 固定 Contract 头 - 固定 Tag/Trailer` 精确求出；
- 如果 Carrier 不能提供可信长度，Carrier 自己必须增加长度字段；
- O1/O2 的 Origin AAD 对 Byte 2 只认证高 2 bit Origin Security，并将可变 Hop Limit 规范化为 0；
- H1/H3 的 Hop AAD 认证完整 Byte 2，因此 Hop Limit 受到当前一跳保护；
- 任意不合法组合在写状态、分配 Sequence、调用密码 Provider 或写输出之前拒绝；
- 所有拒绝路径保持输出对象和协议状态不变。

### 2.6 Canonical Digest Suite

当前候选只允许一个不可协商的摘要算法：

```text
UCN_V6_DIGEST_SUITE_1 = first_16_octets(SHA-256(input))

AAD_DIGEST = first_16_octets(SHA-256(
    "UCN6-AAD-V1" || u32be(canonical_aad_length) || canonical_aad_bytes))

PAYLOAD_DIGEST = first_16_octets(SHA-256(
    "UCN6-PAYLOAD-V1" || u32be(payload_length) || origin_plaintext_payload_bytes))

TRANSFER_MESSAGE_DIGEST = first_16_octets(SHA-256(
    "UCN6-TRANSFER-MESSAGE-V1" || u32be(total_business_length) || business_bytes))

TRANSFER_SETUP_DIGEST = first_16_octets(SHA-256(
    "UCN6-TRANSFER-SETUP-V1" || u32be(setup_payload_length) || exact_setup_payload_bytes))
```

字符串按图示 ASCII 字节编码，不含结尾 NUL；长度是网络字节序 `u32`。输出取 SHA-256
字节串的前 16 个 octet，不把它解释成主机整数，也不随 CPU 端序倒置。Digest Suite 由协议版本
与精确 C1 Transport/Flow 父 Context 唯一确定；当前 v6 context 只能映射 Suite 1，Security OFF
也不改变该映射。未来更换算法必须
建立新 Context/Generation 或新 Wire 版本，禁止同一上下文试多个算法或静默替换。

这些 128-bit digest 是去重、事务绑定和完整消息比对的 canonical fingerprint；O0 下它们
**不是认证证明**，只有 O1/O2 或其他已审计认证域才能证明 digest 的来源和不可篡改性。

`origin_plaintext_payload_bytes` 是 Origin 保护前、语义编码完成后的精确 Payload 字节：包含
线上实际存在的 Message/Operation/Fragment Envelope 与业务字节，不包含 Wire Header、Origin
Tag、Hop Tag、Carrier padding 或 ciphertext。O1 在明文上计算；O2 接收端必须先完成认证解密，
再对恢复出的同一组明文字节计算；O0 直接对收到的 Payload 计算。Transfer 的
`TRANSFER_MESSAGE_DIGEST` 另对完整重组后的纯业务消息计算，不包含逐片 Fragment Envelope。
两种 digest 不能互换，也不能对实现私有结构体或带 padding 的内存求哈希。O0/O1/O2 Golden
必须证明相同语义明文得到相同 `PAYLOAD_DIGEST`，而 O2 ciphertext/Tag 的变化不进入该输入。

## 3. Hop Protection：每跳安全不重复完整身份

Hop Protection 不由远端 Frame 自己开启，而由本地 Link Contract 决定。

### 3.0 Profile 选择与无歧义解析

公共 3 B Header 故意不再携带 H0/H1/H2/H3 选择位，因此接收端**禁止**根据总长度、Tag 是否
“像是正确”、依次试多个 Key，或认证失败后降级来猜 Hop Profile。唯一合法流程是：

```text
receive_hop_frame(carrier_item):
    require carrier supplied exact frame boundary and length
    read only the fixed Core Header and the minimum context selector into scratch
    resolve exactly one current ingress Link/Context binding
    derive exactly one H profile, key/context fingerprint and expected trailer length
    require total length matches that single profile
    verify replay and Hop authentication before any protocol-state mutation
    only then continue Origin authentication and semantic decode
```

“minimum context selector”按 Contract 固定：C0/C1 使用当前 Ingress Link 加显式地址/控制域；C2
使用 Flow Context ID；C3 使用 `{Ingress Link Generation,Forwarding Label}`；C4 使用 Label/Flow
映射；C5 使用 Tree/Group/Sender 映射。结构读取只允许访问已验证边界内的固定字节，不得分配
事务、推进 replay、写统计成功或交付 Payload。

映射必须满足：

- C0/C1/C3/C4 只能解析为 H0 或 H1；C2 只能解析为 H0、H1 或满足 3.3 全部条件的 H2；C5
  只能解析为 H0 或 H3；
- 每个 `{local link instance generation, ingress context}` 同一时刻只有一个 active inbound Hop
  profile 和一个 exact key/context fingerprint；多条同时有效、缺失或 generation 不确定均失败关闭；
- 因 Wire 没有旧/新 Hop Key selector，H1/H3 换钥不能同时试 current/previous。Owner 必须先
  Fence 旧映射，完成有界 drain/cancel，再原子发布新映射；迟到旧帧按新映射认证失败；
- H2 的“无额外 Hop Tail”只能由已认证 C2 Direct Flow Context 证明，不能把任意 C2+O1/O2
  自动解释成 H2；
- Link reopen、Peer/Group/Flow/Label generation 变化在新映射发布前先让旧映射不可用。

发送端也从选定 egress Link/Context 读取同一精确 Profile，不由应用或 Payload 自报。任何无法在
调用密码 Provider 前得到唯一 Profile 的帧均拒绝。Golden/Negative 必须覆盖 H0/H1 同长度歧义、
H2 与 C2/H0 的零 Tail 歧义、H1/H3 错 Key Owner、换钥迟到帧和“认证失败后尝试另一 Profile”。

### 3.1 H0：Native/Trusted Link

字段数：0；额外长度：0 B。

只有以下条件全部满足时才能使用：

- Link 是物理受控的点对点链路，或 Carrier 已提供等价的 Peer Authentication；
- Carrier 已提供重放保护或产品威胁模型明确不需要；
- Link 配置是本地静态 Manifest/认证 Session 的结果；
- 收到的 Frame 不能自行声明“我是可信链路”；
- Link reopen、Peer 变化、Session 变化时清空所有旧队列和 Context。

H0 不是“完全不安全”，而是“不重复实现 Carrier 已经提供的 Hop 安全”。

### 3.2 H1：UCN Hop Auth 96

| 字段 | 长度 | 说明 |
| --- | ---: | --- |
| Hop Sequence | 4 B | 当前 Peer Session 内单调；不回绕 |
| Hop Tag | 12 B | 96-bit 认证 Tag |

字段数：2；额外长度：16 B。

Hop Key 不逐帧发送。接收端用：

```text
Ingress Link Instance
+ authenticated Peer slot
+ current Peer Session Generation
```

唯一定位当前 Hop Key。任何 Link 上同一方向只能有一个活动发送映射；换钥通过 Session/Key Generation 原子切换，旧映射立即 Fence。

Hop Tag 覆盖：

- 完整 Core Header；
- Contract 字段；
- Payload 和 Origin Tag；
- 当前可变 Hop Limit；
- 当前 Forwarding/Tree Label；
- Hop Sequence；
- 双方认证握手得到的 canonical Hop Security Context Fingerprint。

发送端与接收端的 raw `Link Instance Generation` 可以是各自本地值，不能直接假设二者数字相同并
放入 Tag。它只用于本机把 Context Handle 绑定到当前 Link；线上 Hop Tag 使用双方已经认证一致的
Context Fingerprint。任一侧 Link reopen 都必须先 Fence 本地映射并通过重认证建立新的 Fingerprint。

### 3.3 H2：Direct Combined Proof

字段数：0；额外 Hop 长度：0 B，但要求 O1/O2 的 16 B Origin Tag。

只允许 C2，并且：

- 下一跳就是最终 Destination；
- Flow Context 同时绑定 Peer Session 与 E2E Principal；
- 同一 16 B Tag 的 AAD 同时包含双方认证一致的 Direct Link/Peer Context Fingerprint 和 Origin Flow 身份；
- 没有中继会修改 Label 或 Hop Limit；
- 接收端不会把该 Tag 用于另一个 Link 或 Flow。

这样一跳加密/认证流只需要一个 Tag，不叠加两个 Tag。

### 3.4 H3：Group Hop Auth

编码仍为 `Hop Sequence 4 B + Hop Tag 12 B`，长度仍是 16 B。

与 H1 的区别只在本地 Key Owner：H3 使用固定 Group/Tree Hop Context，而不是单一 Peer Context。它只用于 C5，并且 Group Context 和 Sender Slot 必须一起进入认证域。

## 4. C0 ABSOLUTE：完整身份控制协议

### 4.1 作用

C0 用于低频但必须自描述的控制事务：

- 未绑定节点 Bootstrap；
- 已绑定节点重新认证；
- Capability/Flow/Context 建立与销毁；
- Route Discovery、Probe、Activate、Error；
- Recovery；
- Context 丢失后的重新同步。

C0 不用于周期业务数据。它可以较大，因为它只在建立或修复状态时出现。

### 4.2 字段布局

以下以 A1 Realm 的 2 B Address 为例。地址宽度由 Realm Manifest 固定，不逐帧携带。

| Offset | 字段 | 长度 | Owner/含义 |
| ---: | --- | ---: | --- |
| 0 | Common Header | 3 B | 第 2 章 |
| 3 | Realm ID | 4 B | Realm Authority 分配 |
| 7 | Source Address | 2 B | 发送方当前地址；未绑定时为 Bootstrap 保留值 |
| 9 | Destination Address | 2 B | 目标或 Link-local Authority 保留值 |
| 11 | Source Binding Generation | 4 B | Source 地址绑定代际；未绑定时为 0 |
| 15 | Destination Binding Generation | 4 B | Destination 地址绑定代际；未知/未绑定时为 0 |
| 19 | Transaction ID | 4 B | 本控制事务内唯一；不回绕 |
| 23 | Protocol Opcode | 2 B | 精确控制操作 |
| 25 | Payload | N B | Opcode 固定定义 |
| 25+N | Origin Tag | 0/16 B | O0/O1/O2 决定 |
| Tail | Hop Protection | 0/16 B | H0/H1；Bootstrap 证明位于 Payload |

Contract 专有字段数：7；加公共逻辑字段后共 15 个逻辑字段。

基础头长度：

| Realm Address | C0 基础头 |
| --- | ---: |
| A0 / 1 B | 23 B |
| A1 / 2 B | 25 B |
| A2 / 3 B | 27 B |
| A3 / 4 B | 29 B |

### 4.3 Bootstrap 保留地址

以 A1 为例：

- `0x0000`：UNBOUND Source；
- `0xFFFF`：Link-local Authority/Bootstrap Destination；
- `0x0001..0xFFFE`：普通地址。

UNBOUND C0 必须满足：

- Hop Limit 等于 1；
- 中继无条件禁止转发；
- Source/Destination Binding Generation 为 0；
- Payload 携带 Device Identity Digest 和新鲜 nonce；
- 进入昂贵密码操作前先通过无状态 Cookie、per-Link 限速和固定 pending 槽；
- ADDRESS/FINAL COMMIT 的完整 transcript 由双方认证并持久化。

### 4.4 C0 如何运作

```text
Link Up
  → C0 BOOTSTRAP_HELLO
  → C0 COOKIE_CHALLENGE
  → C0 HELLO_COOKIE
  → C0 IDENTITY_CHALLENGE / RESPONSE
  → C0 ADDRESS_OFFER
  → Device persist-before-promise
  → C0 DEVICE_COMMIT
  → Authority persist-before-promise
  → C0 FINAL_COMMIT
  → Peer Session / Address Binding 可用
```

已绑定节点移动到新 Link 时走 `PEER_REAUTH`，不能假装 UNBOUND 重新领取地址。

`PEER_REAUTH` 和已绑定节点的 Session Handshake 属于 Security Session Owner，不属于 Dynamic Admission。Dynamic Admission 只负责未绑定设备获得网络资格和地址；静态 Identity/Binding 节点即使关闭 Admission，仍可通过 C0 Security Handshake 在启动、Link reopen 或 Key rotation 后建立新鲜 Session Generation、Hop Sequence 和 Replay Window。

C0 Transaction ID 只在对应 Source Binding/Bootstrap nonce 域内有意义。收到重复、过期或冲突事务时：

- 完全一致且已有 durable result：允许幂等重发结果；
- Transaction ID 相同但字段不同：拒绝；
- 旧 Binding Generation：拒绝；
- 未持久化完成：不能发送承诺帧。

## 5. C1 STATELESS：无 Flow 的普通寻址数据

### 5.1 作用

C1 是“只想自动寻路并传输数据”的基础协议。它不要求预先建立 Flow，但要求地址绑定和必要的 Hop Session 已经有效。

“Stateless”只表示没有专用 Flow Context，不表示没有身份、安全或 Route 状态。

### 5.2 字段布局

| Offset（A1） | 字段 | 长度 | 含义 |
| ---: | --- | ---: | --- |
| 0 | Common Header | 3 B | 公共语义 |
| 3 | Source Address | 2 B | Realm 内 Source |
| 5 | Destination Address | 2 B | Realm 内 Destination |
| 7 | Service ID | 2 B | 目标服务/Endpoint；控制服务使用保留范围 |
| 9 | Origin Sequence | 4 B | Source Binding + Service 域内单调 |
| 13 | Payload | N B | 业务内容或可选 Message Envelope |
| 13+N | Origin Tag | 0/16 B | O0/O1/O2 |
| Tail | Hop Protection | 0/16 B | H0/H1 |

Contract 专有字段数：4；加公共逻辑字段后共 12 个逻辑字段。

基础头长度：

| Realm Address | C1 基础头 |
| --- | ---: |
| A0 | 11 B |
| A1 | 13 B |
| A2 | 15 B |
| A3 | 17 B |

### 5.3 C1 如何运作

发送流程：

1. Node 根据 Destination 查 Active Route；
2. 若没有 Route，发起 C0 Route Discovery；
3. Route 建立后，用 Endpoint Policy 求出 Traffic、Delivery、Interaction、Origin Security；
4. 根据出口 Link Contract 决定 H0 或 H1；
5. 由 Core Message Sequence Owner 在 `{Source Binding, Service}` 域分配 Origin Sequence；
6. O1/O2 时生成 Origin Tag；
7. H1 时分配当前 Hop Sequence 并生成 Hop Tag；
8. 发送。

中继流程：

1. 先验证 Carrier 和 H1；
2. 检查 Hop Limit，`<=1` 时不得继续转发；
3. 根据 Destination 查 Route；
4. 递减 Hop Limit；
5. 不解析业务 Payload，不修改 Origin Sequence/Tag；
6. 按下一跳重新生成 Hop Sequence/Tag；
7. 发送。

目标流程：

1. 验证最后一跳；
2. 根据 Source/Destination/Service 找到唯一 Origin Security Context；
3. O1/O2 时验证 Origin Sequence 和 Tag；
4. 根据当前 Address Binding 表恢复 Source Principal；
5. 执行 `Principal + Service + Payload Kind/Opcode` ACL；
6. Delivery/Interaction 门禁通过后才交给业务。

### 5.4 地址复用与旧帧

C1 不逐帧携带 4 B Binding Generation，但并没有取消防 ABA。规则是：

- Address Binding 改变时，所有相关 Peer Session、Route 和安全 Context 必须先 Fence；
- Link/Adapter 必须清理旧 RX/TX Queue；
- 旧 Origin Tag 在新 Binding Key 下验证失败；
- H0 只能用于能够证明 Queue/Peer 代际同步失效的受控 Carrier；
- C1/O0 无论使用 H0 还是 H1，都不能证明原始 Source/Destination Binding；因此只允许 Realm
  Manifest 明确标记为整个安全生命周期内不复用的静态地址，并要求全部中继位于业务完整性
  信任边界内；
- 动态地址、可复用地址或无法证明端到端队列清空/身份连续性的路径必须使用 C1/O1/O2，或先
  建立绑定完整代际的 C2/C4 Context；H1 只能证明相邻 Peer，不能代替 Origin anti-ABA；
- 无法证明这些条件时不得选择 C1/O0，应使用 C0 恢复/Setup 或带 Origin Auth 的路径。

C1 的 O1/O2 Key 不通过试多个 Key 猜测。接收端必须用
`{Realm, Source Address, Destination Address, Service ID}` 在当前 Binding 表中得到唯一活动 Origin Context；不存在、存在多个候选或代际不确定时都失败关闭。

C1 Origin Sequence 即使在 O0 下也存在，因此不由可选 Security 模块分配。O1/O2 使用同一个已分配值建立密码 Nonce/AAD；Security 唯一负责 Key/Nonce 绑定和密码学 Replay Window，不维护第二个会竞争递增的 C1 Sequence 副本。

### 5.5 适用与限制

适用：偶发遥测、低频命令、Flow 表满后的安全回退、首次业务数据。

不适用：高频固定流、需要最小多跳头、需要稳定 Realtime/Transfer Context 的业务。

### 5.6 C1 Reliable：不依赖 Flow 的确认路径

C1 明确允许 `Delivery=Reliable`。它使用保留的 Transport Control Service 返回 C1 ACK，不要求预先建立 Flow。

C1 Reliable 原消息的事务键为：

```text
Realm
+ 当前 Source Principal/Binding
+ 当前 Destination Principal/Binding
+ Original Service ID
+ Original Origin Sequence
+ Delivery = Reliable
```

ACK 使用反向 C1 Control 帧，Payload 固定为：

| 字段 | 长度 | 说明 |
| --- | ---: | --- |
| Transport Control Opcode | 1 B | `DELIVERY_ACK` |
| Acked Service ID | 2 B | 原消息 Service |
| Acked Origin Sequence | 4 B | 原消息 C1 Sequence |
| Status | 1 B | 接受/拒绝/资源错误等冻结枚举 |
| Receive Window/Credit | 1 B | 0 表示暂不接受更多；具体单位由 Transport RFC 冻结 |

Payload 固定 9 B。ACK 自身由反向 `{Source Binding, Transport Control Service}` 域分配新的 C1 Origin Sequence。它必须满足原 Endpoint/Transport Policy 要求的来源认证，不能只凭最后一跳身份确认远端业务端点。

重传规则：

- 原消息保持相同事务键、业务 Origin Sequence、Operation ID 和 Payload digest；
- 每一跳重新分配 Hop Sequence/Tag；
- 精确重复且已完成接收的消息不得再次交付业务，只重发缓存的确定 ACK；
- 同一事务键但 Payload、Interaction、Security 或其他不可变语义不同必须拒绝；
- Security 对重复 O1/O2 帧先完成认证，只向 Transport 返回
  `AUTHENTICATED_REPLAY_CANDIDATE + canonical AAD digest + Payload digest`；Transport
  receipt Owner 再按完整事务键和两个 digest 判定 exact duplicate 或 conflict。不得由
  Security 单独宣称“精确重复”，也不得把任意 Replay rejection 自动升级为可重发 ACK；
- receipt 表固定容量、不过早驱逐仍可能重传的活动事务；满载时新 Reliable 消息失败关闭，但普通 Best Effort 不检查该专用表。

因此 `Flow OFF + Reliable ON` 是一条真实 Wire 路径，不依赖 C4。如果实现没有完成上述 ACK、重复认证和 receipt 生命周期，则必须关闭 C1 Reliable Capability，而不是仅凭 Common Header 枚举宣称支持。

## 6. C2 DIRECT：一跳 Flow 数据

### 6.1 作用

C2 用于 Source 与最终 Destination 就是当前 Link 两端的稳定 Flow。Source、Destination、Service、Binding、Delivery、Interaction、Security 和可选 Realtime Policy 全部保存在 Flow Context，不逐帧重复。

### 6.2 字段布局

| Offset | 字段 | 长度 | 含义 |
| ---: | --- | ---: | --- |
| 0 | Common Header | 3 B | 必须与 Flow Policy 一致 |
| 3 | Flow Context ID | 2 B | Parent Session 内单调分配 |
| 5 | Origin Sequence | 4 B | Flow Context 内单调 |
| 9 | Payload | N B | 业务/可选 Envelope |
| 9+N | Origin Tag | 0/16 B | O0/O1/O2 |
| Tail | Hop Protection | 0/16 B | H0/H1；H2 可复用 Origin Tag |

Contract 专有字段数：2；加公共逻辑字段后共 10 个逻辑字段。

基础头：9 B。

典型总开销：

| 保护方式 | 总开销 |
| --- | ---: |
| H0 + O0 | 9 B |
| H1 + O0 | 25 B |
| H0 + O1/O2 | 25 B |
| H2 Combined + O1/O2 | 25 B |
| H1 + 独立 O1/O2 | 41 B |

正常一跳安全流优先选择 H2，避免两个 Tag。

### 6.3 Flow Context 包含什么

Flow Context 不在线上传输的固定记录至少包括 17 个逻辑字段：

| 字段 | 作用 |
| --- | --- |
| Parent Session Generation | 防跨 Session 复用 |
| Flow Context ID | 线上短引用 |
| Source Principal/Binding | 来源身份 |
| Destination Principal/Binding | 目标身份 |
| Source Service | 源服务 |
| Destination Service | 目标服务 |
| Traffic Class ceiling | 允许的最高优先级 |
| Delivery Guarantee | Best/Latest/Reliable |
| Interaction Role policy | 允许哪些角色 |
| Payload Kind/Opcode | 业务解释 |
| Origin Security Suite | None/Auth/AEAD |
| Key Generation | 密钥代际 |
| Replay Window | Origin Sequence 门禁 |
| Max Payload | 精确 MTU 预算 |
| Optional Realtime Policy | NONE 时不分配实时状态 |
| Optional Transfer Policy | NONE 时不分配 Transfer 状态 |
| ACL Digest/Policy Generation | 防 Policy 错绑 |

### 6.4 C2 如何建立和运作

```text
C0 FLOW_PREPARE
  → 双方验证 Identity/Binding/Policy/MTU/Security
  → Receiver 预留固定 Flow slot
  → 需要持久承诺时 persist-before-promise
C0 FLOW_ACCEPT
  → 双方安装相同 Context 指纹
C0 FLOW_COMMIT
  → Context ACTIVE
后续 C2 只携带 Flow ID + Sequence
```

任一字段变化都不能就地修改 ACTIVE Context。必须建立新的 Flow ID，切换完成后再退休旧 Flow。

## 7. C3 ROUTED_HOP：最短逐跳 Label 数据

### 7.1 作用

C3 是多跳稳态中最短的数据格式。它只提供逐跳转发和逐跳安全，不提供端到端重放/认证，因此仅允许：

```text
Delivery = BEST_EFFORT
Interaction = ONE_WAY
Origin Security = O0
```

需要 Latest、Reliable、Request/Result 或不信任中继时必须用 C4。

### 7.2 字段布局

| Offset | 字段 | 长度 | 含义 |
| ---: | --- | ---: | --- |
| 0 | Common Header | 3 B | O0 + Best Effort + One Way |
| 3 | Forwarding Label | 2 B | 当前入站 Link 本地 Label |
| 5 | Flow Context ID | 2 B | 当前 Route/Flow Context |
| 7 | Payload | N B | 业务内容 |
| Tail | Hop Protection | 0/16 B | H0/H1 |

Contract 专有字段数：2；加公共逻辑字段后共 10 个逻辑字段。

基础头：7 B。

典型总开销：

- H0：7 B；
- H1：23 B。

### 7.3 中继如何转发

Label Table 记录：

```text
Ingress Link Generation
Ingress Label
Parent Route Generation
Flow Context ID
Egress Link
Egress Label
Egress Flow Context ID
Traffic ceiling
MTU budget
State / Expiry / Fence
```

中继只执行：

1. 验证 H1 或受信 H0；
2. 用 `{Ingress Link Generation, Forwarding Label}` O(1) 查表；
3. 检查 Flow ID、Traffic ceiling、State 和 Hop Limit；
4. 递减 Hop Limit；
5. 替换为 Egress Label 和 Egress Flow Context ID；
6. 生成下一跳 Hop Sequence/Tag；
7. 发送。

中继不读取：

- Source/Destination Principal；
- Endpoint；
- Operation ID；
- Cluster Membership；
- Realtime capture time；
- 业务 Payload。

因此 C3 最适合 MCU 快速路径和 FPGA 转发站。

### 7.4 失效规则

- Link reopen：该 Link 的所有 Label 立即 Fence；
- Route Generation 改变：建立新 Label，不修改旧 Label；
- Label 不存在：返回受限 Route Error，不猜 Destination；
- Label ID 耗尽：轮换 Parent Route/Link Generation；
- 表满：不驱逐 ACTIVE Label；新建路失败，旧路径保持；
- 超时：先停止新发送，等待有界在途期后退休，不能立即复用 ID。

## 8. C4 ROUTED_ORIGIN：带端到端语义的 Label 数据

### 8.1 作用

C4 在 C3 的基础上增加 Origin Sequence，用于：

- Latest；
- Reliable；
- Request/Result/Error；
- E2E Auth；
- E2E AEAD；
- Timed/Deadline Flow；
- Transfer；
- Cluster 跨中继控制。

### 8.2 字段布局

| Offset | 字段 | 长度 | 含义 |
| ---: | --- | ---: | --- |
| 0 | Common Header | 3 B | 公共语义 |
| 3 | Forwarding Label | 2 B | 当前 Hop 本地 Label |
| 5 | Flow Context ID | 2 B | 端到端 Flow 的本地映射 |
| 7 | Origin Sequence | 4 B | Origin Flow 内单调、跨中继不变 |
| 11 | Payload | N B | 业务/Envelope |
| 11+N | Origin Tag | 0/16 B | O0/O1/O2 |
| Tail | Hop Protection | 0/16 B | H0/H1 |

Contract 专有字段数：3；加公共逻辑字段后共 11 个逻辑字段。

基础头：11 B。

典型总开销：

| 模式 | 总开销 |
| --- | ---: |
| H0 + O0 | 11 B |
| H1 + O0 | 27 B |
| H0 + O1/O2 | 27 B |
| H1 + O1/O2 | 43 B |

### 8.3 C4 如何转发

中继可以修改的只有：

- Hop Limit；
- Forwarding Label；
- Hop-local Flow Context ID；
- Hop Sequence；
- Hop Tag。

中继不能修改：

- MessageMeta；
- Origin Security；
- Flow 的端到端身份；
- Origin Sequence；
- Payload；
- Origin Tag。

Flow Context ID 是 Hop-local alias，中继可以按 Label Table 改写它；所有 Hop 的 Flow Context 必须展开为同一个 128-bit canonical Flow Fingerprint。Origin Tag 的 canonical AAD 使用该 Fingerprint 所绑定的完整 Source/Destination Binding、Service、Policy、Path class 与当前线上不可变字段，不直接认证 raw Flow Context ID。Forwarding Label、raw Flow Context ID 和 Hop Limit 进入 Hop AAD。

### 8.4 C4 Flow ACK 与重传

C4 Reliable 小消息使用同一 Flow 的 Control Payload 返回 ACK；不依赖 Flow 的 C1 Reliable 使用第 5.6 节独立 ACK：

| ACK Payload 字段 | 长度 |
| --- | ---: |
| Acked Origin Sequence | 4 B |
| Status | 1 B |
| Receive Window/Credit | 1 B |

字段数：3；Payload 6 B。

ACK 自身使用新的 Origin Sequence，不能复用被确认数据的 Sequence。重发数据保持相同业务 Origin Sequence 和 Operation ID，但每一跳使用新的 Hop Sequence。

## 9. C5 GROUP：组播与簇内发布

### 9.1 作用

C5 用于受控 Group、Cluster 内广播和多目标发布。它不把完整 Group ID、Key ID、Generation 和成员表放进每帧，而引用已认证的固定 Group Context。

### 9.2 字段布局

| Offset | 字段 | 长度 | 含义 |
| ---: | --- | ---: | --- |
| 0 | Common Header | 3 B | 公共语义 |
| 3 | Tree Label | 2 B | 当前 Group forwarding tree |
| 5 | Group Context ID | 2 B | 本地固定 Group slot |
| 7 | Sender Slot | 2 B | Group 内认证 Source 映射 |
| 9 | Origin Sequence | 4 B | `{Group Generation, Sender Slot}` 域内单调 |
| 13 | Payload | N B | Group 数据/Cluster 控制 |
| 13+N | Group Origin Tag | 0/16 B | O1/O2 通常必需 |
| Tail | Group Hop Protection | 0/16 B | H0/H3 |

Contract 专有字段数：4；加公共逻辑字段后共 12 个逻辑字段。

基础头：13 B。

典型安全总开销：

- 受信 Carrier + Group Auth：29 B；
- UCN Group Hop Auth + Group Origin Auth：45 B。

### 9.3 Group Context 包含什么

- Group ID 和 Group Policy Generation；
- Group Key ID 和 Key Generation；
- Sender Slot → Principal/Binding 映射；
- Group ACL 和允许 Opcode；
- Tree Generation 和 Egress bitmap；
- 指向 Security Owner 中 per-Sender-Slot 密码 Replay Window 的带代际 Handle；
- 可选的 Group 业务重复交付状态；
- Traffic ceiling 和 Rate limit；
- 最大 Payload；
- 是否允许 Cluster Authority Control。
- 成员快照 Generation、Completion success scope、重试/fallback 与结果容量。

密码学 Replay Window 唯一由 Security Owner 管理；Group Context 只保存 Handle，不能复制窗口。Group 业务重复交付状态用于认证成功后的业务语义去重，与密码 Replay 不是同一对象。

### 9.4 C5 如何运作

1. 当前逻辑 Realm Address Authority/quorum 通过认证 C0 控制事务安装动态 Group/Tree Context；
2. 成员持久化必要 Generation 后才 ACK；
3. Source 使用 Sender Slot 和独立 Origin Sequence 发送；
4. 中继验证 Group Hop、查 Tree Label、复制到固定 Egress bitmap；
5. 每个出口替换 Tree Label 和 Hop-local Group Context ID，并生成新的 Hop Sequence/Tag；
6. 接收者验证 Group Origin Tag、Sender replay window 和 Group ACL；
7. Group/Key/Tree Generation 变化时旧 Context 整体 Fence。

动态 Group 的唯一 Policy Owner 只能是 Manifest 指定、仍持有当前 Generation、Lease、Fence、
quorum 和认证证明的逻辑 Realm Address Authority。普通成员、Group Sender 或 Cluster Head 均
不能自报或接受委派取得该角色；Cluster 只能向 Realm Authority 提出变更请求。动态 Group 因而
硬依赖 Identity、Security/Auth Provider 和 Persistence；没有当前 Authority proof 时 C0 Group
更新零写拒绝。静态 Group 则完全来自 anti-rollback 保护的只读 Manifest，不创建运行期退休/
Generation 写入；需要运行期变化时必须改用动态 Group。

Group Origin AAD 使用 canonical Group Fingerprint，不直接认证各 Hop 的 raw Group Context ID。所有 Tree 节点安装的本地 Context 必须展开为相同 Group ID、Group Generation、Policy Generation 和 Sender mapping。

共享 Group Key 只能证明“来自有权持有 Group Key 的成员”。若 ACL 或 Authority 需要精确 Source，Group Context 必须为每个 Sender Slot 安装独立派生验证子键，或者 Payload 携带发送者签名；不能假设所有成员共享的对称 Group Tag 能证明唯一发送者。Cluster Authority、Vote、Commit 等安全关键控制默认使用 C4 的单源 E2E Context，C5 只用于非授权通知或携带可独立验证的证书。

Group Completion 必须由 Context 中冻结的成员快照和 `ANY/ALL/QUORUM/SUBSET` success scope 解释，不能把单播的“远端已接收”直接套到 Group。Tree 已交付部分成员后再进行有界 Unicast fallback 时，必须保留同一 Send/Operation ID 并在成员侧去重；固定结果表满时只允许返回有界聚合/截断信息，不能动态增长。

## 10. 六种 Contract 总表

| Contract | 专有字段数 | 基础头 A1 | 稳态用途 | 主要限制 |
| --- | ---: | ---: | --- | --- |
| C0 ABSOLUTE | 7 | 25 B | Bootstrap/Setup/Recovery/Discovery | 不用于周期数据 |
| C1 STATELESS | 4 | 13 B | 无 Flow 普通数据 | 重复地址和 Service |
| C2 DIRECT | 2 | 9 B | 高频一跳 Flow | 下一跳必须是最终目标 |
| C3 ROUTED_HOP | 2 | 7 B | 最短多跳 Best Effort | 无 E2E/可靠/Latest |
| C4 ROUTED_ORIGIN | 3 | 11 B | 安全/可靠/实时多跳 | 比 C3 多 4 B |
| C5 GROUP | 4 | 13 B | Group/Cluster 发布 | 需要预装 Group/Tree Context |

“基础头”均不含 Payload、Origin Tag、Hop Protection 和 Carrier 开销。

### 10.1 允许的保护组合

| Contract | Origin Security | Hop Protection | 备注 |
| --- | --- | --- | --- |
| C0 | O0/O1/O2 | H0/H1 | 未绑定 Bootstrap 的身份签名/证明在 Payload |
| C1 | O0/O1/O2 | H0/H1 | O0/H0 仅允许受控可信路径 |
| C2 | O0/O1/O2 | H0/H1/H2 | H2 只与 O1/O2 组合 |
| C3 | O0 only | H0/H1 | 中继被视为业务完整性信任边界 |
| C4 | O0/O1/O2 | H0/H1 | 不信任中继时必须 O1/O2 |
| C5 | O1/O2 | H0/H3 | O0 仅可用于无 Authority 的公开发现类 Group，默认禁止 |

在 C1/C4 的多跳 O0 模式中，所有中继都位于业务完整性信任边界内：H1 只能证明相邻节点，不能阻止一个已认证但恶意的中继修改 Payload。只要业务不信任任一中继，就必须使用 O1/O2。

### 10.2 允许的消息语义

| Contract | Delivery | Interaction | Payload Kind |
| --- | --- | --- | --- |
| C0 | Best Effort/Reliable | One Way/Request/Result/Error | Control/Diagnostic |
| C1 | Best Effort/Latest/Reliable | 全部 | Data/Control/Transfer/Diagnostic |
| C2 | Best Effort/Latest/Reliable | 全部 | Data/Control/Transfer/Diagnostic |
| C3 | Best Effort only | One Way only | Data/Diagnostic |
| C4 | Best Effort/Latest/Reliable | 全部 | Data/Control/Transfer/Diagnostic |
| C5 | Best Effort/Latest/Reliable | One Way/Result/Error | Data/Control/Diagnostic |

任何组合只表示编码上允许，不能取代 Endpoint、Flow、Security、Cluster Authority 或 Realtime Policy 的本地准入判断。

### 10.3 线上标识符的唯一分配者

| 线上标识 | 唯一发送分配者 | 接收状态 Owner |
| --- | --- | --- |
| C0 Transaction ID | 对应 Control FSM Owner | 对应 Control FSM |
| C1 Origin Sequence | Core Message Sequence Owner | Message/Transport；O1/O2 密码 Replay 由 Security |
| C2/C4 Origin Sequence | Flow Context Owner | Flow/Transport；密码 Replay 由 Security |
| C3 Hop-local sequence（若 Contract 后续定义） | Forwarder/Link Context Owner | Forwarder |
| H1/H3 Hop Sequence | Security Peer/Group Hop Context | Security Replay Window |
| Transfer Fragment/ACK position | Transport | Transport |
| C1 Transport Parent Generation | C1 Transport Parent Owner | 独立于 Security Session；durable checked-next，绑定双方当前 Principal/Binding |
| Transfer ID（C1） | C1 Transport Parent Owner | Transport receipt/reassembly；每片另显式绑定 Setup Parent Generation |
| Transfer ID（C2/C4） | 对应 Flow Transport Owner | Transport receipt/reassembly |
| Operation ID | Service/Operation Allocator | Service correlation / Operation Journal |
| Flow/Label/Group ID 与 Generation | 对应 Context Owner | 对应 Context Owner |

同一线上字段只能有一个分配者。Security 可以把 C1/C2/C4 Origin Sequence 纳入 Nonce/AAD 并验证 Replay，但不能再维护一份独立发送计数器去覆盖该字段。

Transfer ID 在父 C1 Transport/Flow Generation 内由 checked-next 高水位单调分配，不回绕，不复用
仍位于 Setup、重组、receipt 或 replay 窗口内的值。到顶只能轮换父代际并重新建立 Transport
Context；无法轮换时进入 Fault。删除单个 Transfer 不得回退高水位。

## 11. Payload 子协议：只在需要时出现

Header 不承载所有功能。功能特有字段放在 Payload 开头，并由 MessageMeta、Service/Flow Context 唯一决定。

### 11.0 Control、Diagnostic 与 Transfer 子型位置

为了让普通 Data 保持零额外字节，又让 ACL/Decoder 不靠 Service、长度或 Payload 内容猜操作，
非 Data 子型使用下列唯一位置：

| Payload Kind | C0 | C1/C2/C3/C4/C5 | canonical `protocol_operation_code` |
| --- | --- | --- | --- |
| Data | 不适用 | 无 code 字段 | `0` |
| Control | Header 内既有 `Protocol Opcode:u16` | 仅允许的 Contract 在 Payload offset 0 编码 `Protocol Opcode:u16`；C3 禁止 Control | 线上非零 u16 |
| Diagnostic | Header 内既有 `Protocol Opcode:u16` | Payload offset 0 编码 `Diagnostic Code:u16` | 线上非零 u16；只能观察，任何副作用必须改为 Control |
| Transfer | Setup/ACK/Abort/Receipt 使用 C0 Header Opcode | C1/C2/C4 使用 11.4/11.5 固定 `Fragment Index/Kind` 位置；C3/C5 禁止 | 由 C0 Opcode 或固定 FRAGMENT/SACK subtype 规范化 |

对 C1/C2/C4/C5 的 Control/Diagnostic，2 B code 之后才依次出现条件 Operation Envelope、功能
Envelope 和业务/控制体；C0 则从 Payload offset 0 直接进入条件 Operation Envelope 或 Opcode
body，因为 Opcode 已在 C0 Header。Parser 必须先按 Contract+Kind 得到这一固定位置，再查
registry、认证和 ACL；未知/零 code、C3 Control、C3/C5 Transfer、重复在 body 中放第二个“真实
Opcode”都在状态写入前拒绝。中继可只按允许的 Contract 转发密文，但终止节点必须把线上 code
规范化为同一个 `protocol_operation_code`，外层 Hop ACL 与目标 E2E ACL 不得得出两个 Opcode。

该前缀由 Origin Tag（存在时）和 Hop Protection 覆盖。普通 Data 不携带它，因此不会为了控制面
可审计性给每个传感器帧增加 2 B。

### 11.1 普通 One-Way Data

额外 Envelope：0 B。

Payload 全部是业务数据。最常见的传感器和状态发布不会携带 Operation、Transfer、Realtime 或 Cluster 字段。

### 11.2 Request/Result/Error Envelope

只在 Interaction 不是 One Way 且 Payload Kind 不是 Transfer 时存在；Transfer 的相同业务关联
改由 11.3 Setup 承载：

| 字段 | 长度 | 说明 |
| --- | ---: | --- |
| Operation ID | 8 B | Session/Binding 域内唯一 |
| Operation Flags | 1 B | REPEATABLE、VOLATILE_DEDUP、DURABLE_AT_MOST_ONCE、final 等固定 bit |
| Result/Error Code | 0/2 B | Request 中不存在；Result/Error 中存在 |

Request 字段数：2，额外 9 B。

Result/Error 字段数：3，额外 11 B。

普通 One-Way 数据不承担这 9～11 B。

Request/Result 关联本身不要求持久化：

- REPEATABLE 只使用有界易失 correlation slot；
- VOLATILE_DEDUP 使用当前 Runtime/Session 的固定去重窗口；
- 只有 DURABLE_AT_MOST_ONCE 使用持久化 Operation Journal，并允许掉电窗口返回 IN_DOUBT。

同一个业务请求从 C2 回退 C1、或从 C1 提升 C4 时，Operation ID 必须保持不变；每个 Plan Attempt 重新生成对应 Contract 的 Sequence、Nonce 和认证封装。不能通过重新创建 Operation ID 把重试变成第二次业务操作，也不能复制只对旧 Flow/Path 有效的 Tag。

### 11.3 Transfer Setup、确认与 Abort

每个 Transfer 在首片前必须先用 C0/Transport-Control 建立接收端有界状态。Wire Registry
冻结时必须为以下语义分配固定 Opcode、精确 offset 和 Golden Bytes：

```text
TRANSFER_SETUP
TRANSFER_SETUP_ACK
TRANSFER_ABORT
TRANSFER_TERMINAL_RECEIPT
```

#### 11.3.1 C1 Transport Parent 不是 Security Session

C1 Transfer 的父类型固定为 `C1_TRANSPORT`，不能命名为含糊的 `SESSION`。它是 Transport Owner
维护的端到端父 Context，独立于 Security 是否编译，至少绑定：

```text
Realm
+ current Source Principal/Binding Generation
+ current Destination Principal/Binding Generation
+ Transport Policy Generation
+ optional current Security Context generations
+ C1 Transport Parent Generation
```

双方通过 C0 `TRANSPORT_PARENT_PREPARE/ACCEPT/COMMIT` 建立同一 canonical fingerprint；线上
Parent Context ID 固定为 0，由 C1 Header 的双向地址和 Setup 中的 Parent Generation 定位。
Generation 由 C1 Transport Parent Owner 使用 durable checked-next 高水位分配，persist-before-use、
不回绕；Transfer ID 高水位也属于该父代际。Security OFF/O0 不会取消这一代际，只是不再绑定
Security Context generations。产品若没有 Persistence 或等价硬件单调 witness，就不能启用 C1
Transfer；普通非 Transfer C1 不受影响。重启只能 reload 同一高水位继续分配，或先建立更高父
Generation，不能把易失 boot counter、Link generation 或随机 32-bit 值冒充防 ABA 证明。

`TRANSFER_SETUP` 的候选固定 Payload（不含随 A0～A3 变化的 C0 基础头；25 B 只是一项 A1
示例，不是固定 C0 长度）为：

| Offset | 字段 | 长度 | 约束 |
| ---: | --- | ---: | --- |
| 0 | Parent Context Kind | 1 B | `C1_TRANSPORT=1` 或 `FLOW=2` |
| 1 | Parent Context ID | 2 B | C1 Transport 时为 0；Flow 时非零 |
| 3 | Parent Generation | 4 B | 非零、checked serial |
| 7 | Transfer ID | 4 B | 父代际内非零、单调、不回绕 |
| 11 | Service ID | 2 B | 非零；C1 为目标 Service，C2/C4 必须等于父 Flow 绑定 Service |
| 13 | Total Business Length | 4 B | `1..Endpoint/Composition ceiling` |
| 17 | Message Digest | 16 B | 按 2.6 的完整业务消息 canonical digest |
| 33 | Delivery | 1 B | 必须是 Endpoint 与父 Context 允许值 |
| 34 | Interaction | 1 B | One-way/Request/Result/Error |
| 35 | Fragment Count | 2 B | `1..min(UCN_V6_MAX_TRANSFER_FRAGMENTS,32767)`；受 15-bit Fragment Index 域约束 |
| 37 | Fragment Data Budget | 2 B | 与精确 Path/Contract 一致 |
| 39 | Setup Lifetime ms | 4 B | `1..UCN_V6_MAX_TRANSFER_SETUP_LIFETIME_MS` |
| 43 | Operation ID | 0/8 B | One-way 时字段不存在；其余 Interaction 必须存在且非零 |
| 51 | Operation Flags | 0/1 B | One-way 时不存在；其余必须与 11.2 的执行语义完全一致，保留位为零 |
| 52 | Result/Error Code | 0/2 B | 仅 Result/Error 存在；Request 与 One-way 中字段不存在 |

因此 One-way Setup Payload 固定为 43 B，Request 为 52 B，Result/Error 为 54 B；Decoder 先
读取固定 Offset 34 的 Interaction，再要求总长度精确等于对应值。禁止把条件缺失字段编码成
零值占位。Operation ID/Flags/Result Code 与单帧 11.2 使用同一个 canonical 语义，不能让同一
Operation 因是否分片而改变幂等或执行模式。Setup digest 按 2.6 覆盖上述实际存在的精确 Setup
Payload；C0 自身的认证/AAD 另行绑定控制事务键，二者不能用一个未定义的“综合 digest”互相替代。

接收端第一次接受 Setup 时，用本地可信单调 `now_us:u64` 建立绝对 deadline：先要求 duration
位于上述范围，再 checked-multiply `lifetime_ms * 1000`，最后 checked-add 到 `now_us`；时钟
未知、乘法/加法溢出或结果不可表示时零写拒绝。有效区间是半开区间
`now_us < setup_deadline_us`，`now_us >= setup_deadline_us` 即过期。精确重复 Setup 只能读取原
deadline 和 receipt，绝不能刷新完整寿命。

接收端在发送 Setup ACK 前必须原子预留重组、bitmap、receipt、SACK/Credit 和交付资源。
完全相同 Setup 幂等返回同一 ACK；同事务键不同字段冲突拒绝。Abort 只能释放尚未产生交付
副作用且精确匹配的 Setup；终态由 receipt 有界保留。

ACK/Abort/Receipt 均携带
`{Parent Kind,Parent ID,Parent Generation,Transfer ID,Service ID,Setup Digest}`；
ACK 再携带 accepted Fragment Budget/Credit，Abort 携带固定 Reason，Terminal Receipt 携带
固定 Terminal State/Result Code。它们的精确 offset、长度、Opcode 数值和 Golden Bytes 必须
在 Wire Registry 评审中一次冻结；在此之前这些语义只能用于离线状态机，不能进入生产 Codec。

C1 的 Setup 状态属于 C1 Transport Parent 表；C2/C4 的 Setup 状态属于该次 Transfer，不得
把总长度、Operation 或可变重组进度写回长期 Flow Context。Flow 只提供冻结的父代际、MTU、
安全和 Delivery 上限。

### 11.4 Transfer Fragment Envelope

只在 Payload Kind 为 Transfer 时存在。每片 `MessageMeta.Interaction` 必须与已接受 Setup 的
Interaction 精确相同；C1 的 Service 必须等于 Setup Service，C2/C4 则由父 Flow Service 与
Setup Service 的双重匹配证明。任何片试图改成另一 Interaction、Service 或父上下文都在写
bitmap/reassembly 前拒绝：

| 字段 | 长度 | 说明 |
| --- | ---: | --- |
| Transfer ID | 4 B | C1 Transport/Flow 父代际内唯一 |
| Parent Generation | C1 为 4 B；C2/C4 为 0 B | C1 必须等于 Setup 的 Parent Generation；C2/C4 已由不可变 Flow Context 证明，禁止重复编码 |
| Fragment Index/Kind | 2 B | Fragment 时 bit15 必须为 0，低 15 bit 是从 0 开始的 Index；`0x8000` 专用于 SACK |
| Fragment Count | 2 B | 固定 Count，范围 `1..32767`，并要求 `Index < Count` |
| Fragment Payload | N B | 实际片段 |

字段数：C1 为 4、每片额外 12 B；C2/C4 为 3、每片额外 8 B。C1 Parent Generation 在任何
bitmap/reassembly 写入前与 Setup 精确比较，因此旧 Parent 的迟到片即使复用了 Transfer ID 也
只能被拒绝；不能依赖 O1/O2 恰好拦截，更不能把 O0 可信链路解释成“不会出现延迟旧帧”。

总长度、完整消息摘要、Service、Delivery/Interaction Policy、最大 Fragment 和可选 Operation ID 在本次 Transfer
Setup 状态中保存，不在每片重复。长期 Flow Context 只保存不随单次 Transfer 改变的上限。

### 11.5 Transfer SACK/Credit

SACK/Credit 不另建第五套 C0 Setup FSM，但“反向 Contract”必须是可验证的 Transport Feedback
通道，而不是把正向业务 Service 简单倒置：

- 正向 C1 Transfer 使用反向 C1，Header Service 固定为保留的
  `UCN_V6_SERVICE_TRANSPORT_FEEDBACK`；Payload 再显式携带 Original Service ID；
- 正向 C2/C4 Transfer 必须在 Flow 建立时原子建立成对的反向反馈 Flow。该 Flow 的
  `Optional Transfer Policy` 固定绑定正向 canonical Flow Fingerprint、正向 Flow Generation、
  Original Service 和允许的反馈 Traffic/Security ceiling；
- C2/C4 Setup ACK 只有在这条反馈 Flow 已 ACTIVE 且对应资源已经预留时才可发送。没有反馈
  Flow 时必须拒绝该 C2/C4 Transfer Setup，或在发送任何 Fragment 前重新规划为 C1；不得在
  传输中途把 SACK 临时降级成身份不完整的 C1；
- Feedback 的 `MessageMeta` 固定为 `Best Effort + One Way + Transfer`，Traffic Class 必须等于
  Setup 冻结的原 Traffic Class，不得借反馈升级优先级；受保护 Transfer 的反馈必须具有满足
  Endpoint/Transport Policy 的 Origin 认证，不能只依赖最后一跳身份。

Parser 在共同前缀中的 16-bit `Fragment Index/Kind` 位置判别子型：Fragment 的 bit15 为 0，
SACK 必须精确等于 `0x8000`。不能靠 Payload 长度、方向或实现状态猜测：

| 字段 | 长度 | 约束 |
| --- | ---: | --- |
| Transfer ID | 4 B | 精确匹配活动 Setup |
| Parent Generation | C1 为 4 B；C2/C4 为 0 B | C1 精确匹配 Setup；C2/C4 由反向 Flow Context 证明 |
| Fragment Index/Kind | 2 B | 固定 `0x8000`；其余 bit 组合全部拒绝 |
| Original Service ID | C1 为 2 B；C2/C4 为 0 B | C1 精确匹配 Setup；C2/C4 由成对反馈 Flow 证明 |
| Window Base | 2 B | 不超过 Setup Fragment Count |
| Received Bitmap | 4 B | 位范围不得超过 Setup Count |
| Credit | 2 B | 不超过接收端冻结窗口上限 |

字段数：C1 为 7、Payload 20 B；C2/C4 为 5、Payload 14 B。C1 的保留反馈 Service、Original
Service 和 Parent Generation，或 C2/C4 的成对反向 Flow Context，再与 Transfer ID 共同绑定
同一个 Setup；旧父代际或错 Service 的 SACK 不能推进新 Transfer。`0x8000` 与 Fragment 的
bit15=0 Index 互斥。

需要更大窗口时发送多条固定 SACK，不使用可变无限 bitmap。

每个首次提交的 Fragment 还必须在固定 Transfer evidence 表中保存：

```text
Transfer Identity + Origin Sequence + Fragment Index
+ canonical AAD fingerprint + plaintext Payload digest + immutable outcome
```

数组位置可以隐含 Transfer Identity/Index，但不能丢 Origin Sequence、AAD fingerprint、Payload
digest 或 outcome。Security 返回 replay candidate 时只能与这份原始 evidence 精确比较；一致
才可重发 SACK/terminal receipt，不一致即 conflict。活动窗口和已完成 Transfer 的 evidence 至少
保留到对应 Security Replay Window 不会再把这些 Sequence 分类为 candidate，且 Transport 的
最大合法重试/terminal-receipt 窗口也已结束；在两者之前不得因 reassembly Buffer 已释放而压缩
或驱逐。安全 Context Generation 轮换并 Fence 旧 Replay Window 后，才可按固定预算退休旧
evidence。容量由 Build Manifest 静态生成，满载时在接受新 Setup/Fragment 前失败关闭。

### 11.6 Realtime Envelope

只有选择线上 `SYNCED_STAMP/DEADLINE` 的消息才携带；NONE 模式额外 0 B。UCN Network Time
Sync v1 必须使用 C4 Flow，但 Local Stamp 不上线且不依赖 Flow；外部 Time Provider 也可以为
明确允许 Timed Envelope 的 C1/C2/C4 Contract 提供 Domain Time。不能把“Realtime 模块存在”
等同于“所有帧都是 Realtime Flow”。

候选 Timed Envelope：

| 字段 | 长度 | 说明 |
| --- | ---: | --- |
| Domain Generation | 4 B | 防时间域 ABA |
| Capture Time | 8 B | Domain Time，微秒 |
| Uncertainty Class | 1 B | 发送端上界向上量化 |
| Time Flags | 1 B | SYNCED/HOLDOVER/DEADLINE |
| Max Age/Budget | 2 B | 量化值；只允许缩短 |

字段数：5；额外 16 B。

它位于 Payload，不进入所有 Frame Header。中继默认不解析；若 Flow 明确启用 Hop Deadline Scheduling，则 Flow Context 存初始预算，线上只允许附加一个受 Hop Auth 的 2 B `Remaining Budget`，不能携带两个 64-bit 时间值。

### 11.7 Cluster Control Envelope

普通成员数据额外 0 B。只有 Cluster Control/Directory/Tunnel Payload 携带：

| 字段 | 长度 |
| --- | ---: |
| Protocol Opcode | 2 B | Cluster registry 中的非零 code；位于 C4/C5 Control Payload offset 0 |
| Cluster Epoch/Term reference | 4 B |
| Config Generation reference | 4 B |
| Transaction ID | 4 B |
| Opcode-specific payload | N B |

公共字段数：4；固定公共部分 14 B。该 2 B Protocol Opcode 就是 11.0 的 Control 前缀，不得再
在 Opcode-specific body 中重复一个 1 B Cluster Opcode。

完整成员快照、证书、Joint Config 使用多条固定 Transfer/Cluster Payload，不扩大所有数据头。

## 12. 自动寻路协议如何运作

### 12.1 基础 C1 路径发现与 SoftRoute

当发送目标没有可用 Route 时，Discovery 共用以下过程：

1. Origin 分配 C0 Transaction ID；
2. 发 C0 `ROUTE_DISCOVER_REQUEST`；
3. 中继只在固定 Discovery cache 有容量时记录反向状态；
4. 每个中继更新累计 Cost 和最小 Payload Budget；
5. Target 返回同 Transaction ID 的 `ROUTE_DISCOVER_RESPONSE`；
6. RREP 返回时，每个 Relay 可以安装/刷新一个有绝对截止期的 `SoftRoute`；
7. Origin 可用 SoftRoute 立即发送 C1 Best Effort/Latest，或将结果收集为高级 Flow Candidate。

Route Request 的候选 Payload：

| 字段 | 长度 |
| --- | ---: |
| Accumulated Cost | 4 B |
| Minimum Payload Budget | 2 B |
| Required Capability Bits | 2 B |
| Discovery Flags | 1 B |

字段数：4；Payload 9 B。Origin、Target 和 Transaction ID 已在 C0 Header 中，不重复。

SoftRoute 只包含本地 `destination/next-hop/link-generation/local-entry-generation/cost/hop/expiry`，不发布全路径 Route Generation、Label 或 Proposal Digest。它不能被用于：

- C2/C3/C4 稳态上下文；
- Pinned Path 或硬资源预留；
- Network Time Sync 的定向 Path 证明；
- Cluster Vote/Commit/Handover Authority；
- 任何 durable route promise。

因此基础发现的正常路径是 `RREQ → RREP → SoftRoute → C1`，不运行 Stage/Commit。RREP 在部分 Relay 安装后丢失只留下可过期本地提示，不会授权，也不能被解释为已提交的 `FlowPath`。

### 12.2 Advanced Flow 的 Probe 与两阶段 Label 安装

只有当 Resolver 选择 C2/C3/C4、Pinned Path、Network Time Sync、Cluster Authority 或要求原子换路时，候选路径才进入本节。这些高级路径不能只凭 RREP 直接切换：

1. Origin 发 C0 `PATH_PROBE`；
2. 各中继为该 Candidate 预留但不激活 Label；
3. Target 返回 Probe ACK；
4. Origin 验证 RTT、MTU、Capability 和 Policy；
5. Origin 发 C0 `PATH_ACTIVATE_STAGE`；
6. 各中继和 Target 在零写入容量预检后建立有绝对截止期的 `STAGE_PENDING` 记录，不替换
   旧 Active Route；Target 完成本地预留后可进入 `STAGED` 并向上一跳返回
   `PATH_STAGE_ACK`，Relay 必须等到收到下游精确 Stage ACK 后才进入 `STAGED` 并向上游 ACK；
7. Origin 收到完整、精确的 Stage ACK 链后发送 `PATH_ACTIVATE_COMMIT`；
8. Target 收到精确 Commit 后先原子发布本地 Active 并向上一跳返回可幂等重发的
   `PATH_COMMIT_ACK`；Relay 进入 `COMMITTING`、转发 Commit，并且只有收到下游精确 Commit
   ACK 后才原子发布自己的 Active、再向上游 ACK；
9. Origin 收到精确 Commit ACK 后才把 Candidate 切为 Active；
10. 旧 Active 保留 Grace，随后有界退休；
11. Stage 超时或显式 `PATH_ACTIVATE_ABORT` 只删除同事务的 Staged 状态；Commit 已被任一
    节点接受后，调用方不得再把它描述为普通内存回滚，ACK 丢失只能靠同键重发、terminal
    receipt、租约到期或 RERR 收敛。

Label Setup Payload：

| 字段 | 长度 |
| --- | ---: |
| Candidate ID | 2 B |
| Route Generation | 4 B |
| Ingress/Reverse Label | 2 B |
| Egress/Forward Label | 2 B |
| Path Profile ID | 2 B |
| Context Digest | 4 B |

字段数：6；Payload 16 B。

Stage、Commit、Abort 和两类 ACK 共享唯一事务键：

```text
Route Domain = {
  Realm,
  Origin Address/Binding Generation/Session Generation,
  Destination Address/Binding Generation
}
Route Instance Key = {Route Domain, Route Generation}
```

全文只使用 `Route Generation`（代码名 `route_generation`）；不得再增加同义
`route_epoch`。完整激活事务键为：

```text
Route Domain
C0 Transaction ID
Candidate ID
Route Generation
Label pair
Path Profile ID
Context Digest
```

任一字段不匹配不得写 Route、删除 Candidate 或增加成功统计。精确重复 Stage/Commit 必须
返回相同 receipt，不能重复占槽或推进 Generation；相同事务键携带不同字段属于冲突并
fail-closed。以上五个符号 Opcode 是 C0 Registry 的必备语义，具体数值和 Golden Byte Offset
必须在 Wire 冻结阶段登记；登记完成前 Route 激活逻辑仍不得标记 `FROZEN FOR IMPLEMENTATION`。

逐跳发布顺序固定为：`STAGE_PENDING → STAGED → COMMITTING → ACTIVE`。Relay 不得把“本地
槽已预留”冒充“下游链已 Staged”，也不得在下游 Commit ACK 前把本地 Route 暴露为 Active。
Origin 只在收到第一跳精确 Commit ACK 后切换；Commit 已可能在远端产生副作用而 ACK 丢失
时进入 `IN_DOUBT`，通过相同事务键重发和 terminal receipt 对账，不能执行普通内存回滚。

### 12.3 稳态发送

- 偶发流继续使用 C1 + SoftRoute；
- 一跳高频流提升为 C2；
- 多跳 Best Effort 可提升为 C3；
- 多跳安全/可靠/实时流使用 C4；
- 中继只走 Label fast path，不做完整路由搜索。

SoftRoute 不会因为连续发送而在原地“变成” Flow。升级必须由 Resolver 证明收益/需求，然后创建新的不可变 Proposal 并完成全部 Stage/Commit。

## 13. Flow Context 协议如何运作

### 13.1 为什么需要 Flow

以下字段如果每帧重复会造成主要开销：

- Source/Destination Identity/Binding；
- Endpoint/Service；
- Delivery/Interaction；
- Security Suite/Key Generation；
- ACL/Policy Generation；
- Route/Path；
- Realtime/Transfer Policy。

Flow 将它们认证一次，然后普通帧只引用 2 B ID。

### 13.2 Flow 建立状态机

```text
EMPTY
  → PREPARED        Receiver 预留静态槽
  → ACCEPTED        双方 Context Digest 一致
  → ACTIVE          Commit 后允许 C2/C3/C4
  → DRAINING        不接受新业务，只处理在途
  → RETIRED/FENCED  永不在同 Parent Generation 内复用 ID
```

规则：

- PREPARE/ACCEPT/COMMIT 都使用 C0；
- 新安全或 Authority 承诺必须 persist-before-promise；
- Context Digest 覆盖全部 17 个 Flow 字段；
- Meta 超出 Flow ceiling 时拒绝，不能由 Frame 临时提权；
- 表满不驱逐 ACTIVE Flow；
- Parent Session 变化时所有子 Flow 立即 Fence；
- Flow ID 耗尽时轮换 Parent Generation，不回绕。

## 14. Security 协议如何运作

### 14.1 节点端保存，Frame 不重复发送

- Principal；
- Binding Generation；
- Suite；
- Key ID/Generation；
- ACL；
- Replay Window；
- Peer/Group/Flow Context；
- 当前和 staged key；
- rate limit。

### 14.2 每帧仍必须携带

- Origin Sequence（需要 E2E 时）；
- Hop Sequence（H1/H3）；
- Origin Tag（O1/O2）；
- Hop Tag（H1/H3）。

这些字段无法靠本地配置推导，因为它们证明“这一帧是新的且未被修改”。

Frame 中的 O0/O1/O2 只是编码选择，不是权限声明。Receiver 必须把它与 Endpoint/Flow Context 的 REQUIRED Security 精确比较：本地要求 O1/O2 而 Frame 声明更低 Profile 时，在任何业务副作用前拒绝，不能把缺失 Tag 当作可选降级。

### 14.3 接收顺序

```text
Carrier length/boundary
  → Link Contract/Hop Sequence/Tag
  → Version/Contract/固定长度
  → Context/Label lookup
  → Origin Sequence/Tag
  → ACL + Opcode/Service
  → Delivery/Interaction/Realtime/Transfer gates
  → business side effect
```

昂贵 E2E crypto 必须位于廉价长度、Hop Auth、Context 和 rate-limit 门之后。

Security Replay Window 只能证明某个 Sequence “未见/已见候选”，不能只凭 bitmap 宣称业务
字节是“精确重复”。O1/O2 验证成功时 Security 向 Transport 返回
`AUTHENTICATED_REPLAY_CANDIDATE + canonical_aad_digest + payload_digest`；Reliable/Transfer
receipt Owner 再按完整事务键和两个 digest 判定 exact duplicate 或 conflict。O0 Reliable
没有密码学 replay 证明，只能由 Transport 的固定 receipt/dedup window 在受信任链路边界内
判断重复；它不得产生 `AUTHENTICATED_*` 结论。相同 Sequence、不同 digest 必须拒绝且不得
重放旧 ACK。

### 14.4 Key rotation

1. C0 建立 staged Key Generation；
2. persist-before-promise；
3. 双方确认 Context Digest；
4. 原子切换活动 Generation；
5. 旧 Generation 只允许有界 drain，不允许新发送；
6. drain 完成后销毁旧 key；
7. ID/Generation 到顶进入 Fault，不回绕。

普通 Frame 不携带 Key ID/Generation，因为当前 Flow/Peer Context 只能唯一映射一个活动发送代际。若实现需要同时试多个 Key 才能解析，说明切换合同错误，不能把选择字段重新塞回每帧。

### 14.5 Reliable 重传的 sealed bytes

首次 E2E 保护前，Origin Sequence/Nonce 必须只分配并不可逆消费一次。保护成功后 Attempt
持有固定容量的 `ORIGIN_SEALED` artifact：不可变的 E2E Header、Payload、Origin Tag、canonical
digest 及父 Binding/Flow Generation。普通 Reliable 重传只能重发同一 sealed bytes；每一跳
可以按当前 Hop Context 重新分配 Hop Sequence/Tag，但不得再次调用 Origin protect、不得换
Origin Sequence，也不得修改事务 digest。Composition 必须同时冻结 artifact 数量与字节池：
`UCN_V6_ORIGIN_SEALED_POOL_BYTES >= sum(bucket_count[c] * max_sealed_bytes[c])`。基础 C1
Reliable 使用独立保留 bucket，Transfer/Bulk 不得借尽；必须在分配 Origin Sequence/Nonce 和
调用 protect 前原子预留精确字节、holder、最大重试窗口和 terminal receipt。任一不足时零
crypto、零 Sequence/Nonce 消耗、零发送副作用拒绝。

## 15. Cluster 如何建立在低开销 Core 上

Cluster 不是第二套基础协议，而是建立在 C4/C5 上的可选控制服务。

### 15.1 Cluster OFF

- 不编译 Cluster 状态机；
- 不分配 Membership/Config/Epoch 表；
- 普通 C1/C2/C3/C4 Golden Bytes 不变化；
- Capability 中明确不支持 Cluster；
- 收到 Cluster Service/Opcode 直接拒绝。

### 15.2 Cluster ON

- `CLUSTER_BASE` 硬依赖 Flow 和 C4；Head/Member/Backup 的基础控制使用 C4；
- C5 仅属于 `CLUSTER_GROUP_ACCELERATION`，要求额外启用 Group；Group/C5 关闭时回到已经定义的 C4 基础控制，不关闭 Cluster 安全门禁；
- Epoch、Config、Vote、Certificate 位于 Cluster Payload；
- Cluster Directory 只为跨簇寻址建立 Flow/Route Context；
- 普通成员业务仍使用 C2/C3/C4，不重复 Cluster ID；
- 万级规模靠分簇后的目录与骨干 Label 路由扩展。

### 15.3 Cluster 数据转发

```text
Member C2/C3/C4
  → Cluster Head/Gateway label
  → inter-cluster C4 secured flow
  → target Cluster label
  → target Member
```

只有跨簇 Gateway Flow 的 Context 本地保存 Source/Target Cluster 身份；普通帧只带 Label/Flow ID。

当前版本不提供 `Cluster ON + Flow OFF` 的替代 Wire 路径，该 Composition 必须在构建或初始化阶段拒绝。未来若要解除依赖，必须先定义并外审另一条完整 Cluster Control Contract、事务键、安全域和重放规则。

## 16. Realtime 如何建立在低开销 Core 上

只有 UCN Network Time Sync v1 强制绑定 C4 Flow。线上 `SYNCED_STAMP/DEADLINE` 绑定实际选择
的 C1/C2/C4 Contract：C2/C4 可从 Flow Policy 取得限制，C1 则从 Endpoint/Request Policy 取得。
Realtime local-only 的 `LOCAL_STAMP` 与产品提供的外部 Time Provider 不依赖 UCN Flow 或
Persistence。它们都不是基础 Header。

### 16.1 NONE

额外字段：0；额外 Tag：0；普通路径完全不感知时间同步。

### 16.2 LOCAL_STAMP

只在发送节点本地记录时间，不上线；用于诊断或同机业务。

### 16.3 SYNCED_STAMP/DEADLINE

- UCN Network Time Sync v1 硬依赖 Flow，并使用专用 C4 Time Sync Flow 建立 Domain；
- Timed Data Payload 携带 16 B Realtime Envelope；
- C2/C4 Flow Context 保存 Domain/Policy/uncertainty limits；C1 从冻结 Endpoint/Request Policy 取得同类限制；
- Source 和 Destination 执行端到端时间门禁；
- 普通中继不解析 Envelope。

Realtime 模块本身不整体依赖 Flow：Core Local Time、LOCAL_STAMP，以及由产品直接提供的已认证外部 Time Domain Provider 都可以在 Flow OFF 时使用。只有 UCN 自身的四报文 Network Time Sync v1 依赖 C4 Flow；缺少 Flow 时不得产生同步 pending 或同步控制帧。

### 16.4 Hop Deadline Scheduling

只有明确启用的 Flow 才使用 2 B Remaining Budget：

- 初始预算保存在 Flow Context；
- 每跳只能减小；
- 进入 Hop Tag；
- 只能在原 Traffic Class 和 per-flow 配额内排序；
- 不能提升 Class、侵占 Q0 预留或无限抢占；
- Budget 到 0 时丢弃或按 Policy 降级，不能作为普通包继续传播。

## 17. Full、Lite、Nano 如何保持同一协议

Profile 只决定容量，不决定某项能力是否编译。Composition 决定 Feature；Profile 只为已经启用的模块提供固定资源上限。

| 容量项 | Nano 倾向 | Lite 倾向 | Full 倾向 | 仅在何时分配 |
| --- | --- | --- | --- | --- |
| Direct Binding/Endpoint | 极小固定表 | 小表 | 较大表 | Kernel 始终 |
| RouteSet/Path | 极小或 0 | 小表 | 较大表 | Route Store/Forwarder ON |
| Discovery pending/candidate | 极小或 0 | 小表 | 较大表 | 对应 Discovery 角色 ON |
| Flow/Label Context | 极小或 0 | 小表 | 较大表 | Flow ON |
| Security Session/Replay | 极小表 | 小表 | 较大表 | 对应 Security 子能力 ON |
| Transfer window/reassembly | 极小或 0 | 小窗口 | 较大窗口 | Reliable/Fragment ON |
| Realtime Domain/Event | 极小或 0 | 小表 | 较大表 | 对应 Realtime 子能力 ON |
| Group/Cluster | 极小或 0 | 小表 | 较大表 | Group/Cluster ON |
| Planner Candidate | 固定规则或极小 | 小集合 | 较大集合 | Advanced Planner ON |

规则：

- 不支持的能力通过认证 Capability 明确表达；
- Sender 只能选择双方和整条 Path 都支持的 Contract；
- Nano 可以编译安全端点、Flow、Realtime 或 Cluster Member，只要其显式容量满足该模块最低合同；
- Full 也可以只编译静态直连，不因资源较大自动链接 Group、Cluster 或 Advanced Planner；
- 所有 Decoder 都必须严格识别并拒绝本地不能终止的 Contract/Opcode；中继透明转发能力按 `CAN_FORWARD` 单独声明；
- 不允许因为对端能力低就静默关闭 Endpoint REQUIRED 的安全或实时要求；
- 所有 Profile 使用相同 Version/Contract 字节和字段语义；
- 某项 Feature 的容量低于最小合法值时拒绝构建或初始化，不静默关闭，也不动态扩容；
- 本机 Layout Hash 可以不同，节点间只协商共同 Wire、角色和当前有效 Capability。

## 18. Carrier 具体如何承载

### 18.1 CAN-FD

- CAN ID 可承载 Traffic Class 和 Link-local Label 的部分位；
- DLC 提供长度；
- Native CRC 提供随机错误检测；
- Core 不重复 Magic/Length/CRC；
- 不可信总线仍需要 H1/Origin Tag；
- Padding 必须为零并纳入 Hop Tag 或由 Carrier 严格校验。

### 18.2 Classic CAN

- 8 B MTU 无法直接承载大多数安全 Frame；
- Adapter 使用固定小 Carrier Fragment Header；
- 重组完成后才交付完整 Core Packet；
- 每片不重复完整 UCN Core Header；
- Carrier ID 可保存 Label/Traffic；
- 需要评估每片 Tag 与整 Carrier Tag 的安全边界，不能默认只校验最后一片。

### 18.3 UART/USB Stream

Stream Carrier 单独增加：

| 字段 | 候选长度 |
| --- | ---: |
| Sync | 2 B |
| Carrier Length | 2 B |
| Header Check | 2 B |
| Carrier CRC32C | 0/4 B |

基础 6 B，仅 Stream 使用。若 Core 没有 Origin/Hop Tag且底层 Stream 不提供等价 FCS，则 Carrier CRC32C 为必需，总计 10 B；已有认证 Tag 或 USB 等底层完整性时可为 0 B。Core Packet 内不再携带 Magic/Length。

### 18.4 ESP-NOW/无线 Datagram

- Datagram 已提供帧边界和长度；
- 若使用 UCN Security，采用 H1/O1/O2；
- 若 Carrier Session 已提供经审计的 Peer AEAD/replay，可选择 H0；
- 不能因为底层有 Wi-Fi CRC 就认为具备身份认证。

## 19. 从开机到稳定传输的完整例子

### 19.1 两节点一跳传感器流

```text
1. Link Up
2. C0 Bootstrap/Reauth，建立 Binding 和 Peer Session
3. C0 Capability 交换
4. 首次可用 C1 直接发送
5. C0 FLOW_PREPARE/ACCEPT/COMMIT
6. 稳态改用 C2：3 B Common + 2 B Flow + 4 B Seq
7. 无线安全使用 H2：再加一个 16 B Combined Tag
8. 每帧总协议开销 25 B，而不是当前约 77～101 B
```

### 19.2 三跳普通 Best Effort

```text
1. C0 Route Discovery
2. Candidate Probe
3. Path Activate，在每跳安装 Label
4. Flow Context 激活
5. 稳态 C3：7 B Core
6. 每跳若由 UCN 认证，加 H1 16 B
7. 中继只查表、减 Hop、换 Label、重做 Hop Tag
8. 每跳线上协议开销 23 B
```

### 19.3 三跳加密可靠命令

```text
1. 安装 C4 Flow 和 Route Label
2. Request Payload 增加 9 B Operation Envelope
3. C4 Core 11 B
4. Origin AEAD Tag 16 B
5. 不可信每跳 H1 16 B
6. 固定安全头合计 43 B；Operation 字段只在请求存在
7. 中继看不到业务明文，只处理 Label/Hop
8. Destination 验证 E2E、ACL、Operation Journal 后执行
```

### 19.4 Cluster 内普通遥测

```text
Cluster 已启用，但普通遥测仍走 C2/C3/C4
不携带 Cluster ID、Epoch、Config、Vote 或 Certificate
只有 Head 选举、成员变更和跨簇目录才使用 Cluster Payload
```

## 20. 开销总账

### 20.1 Header/Trailer

| 组件 | 长度 |
| --- | ---: |
| Common Header | 3 B |
| C0 专有部分 A1 | 22 B |
| C1 专有部分 A1 | 10 B |
| C2 专有部分 | 6 B |
| C3 专有部分 | 4 B |
| C4 专有部分 | 8 B |
| C5 专有部分 | 10 B |
| Origin Tag O1/O2 | 16 B |
| Hop Protection H1/H3 | 16 B |
| Direct Combined H2 | 0 B additional |

### 20.2 可选 Payload Envelope

| 功能 | 不使用时 | 使用时 |
| --- | ---: | ---: |
| One-Way | 0 B | 0 B |
| Request | 0 B | 9 B |
| Result/Error | 0 B | 11 B |
| Transfer Fragment | 0 B | C1 为 12 B/片；C2/C4 为 8 B/片 |
| Transfer SACK | 0 B | C1 为 20 B；C2/C4 为 14 B |
| Control/Diagnostic operation code | Data 为 0 B | C0 已在 Header；其余允许 Contract 为 2 B |
| Realtime | 0 B | 16 B |
| Hop Budget | 0 B | 2 B |
| Cluster Control | 0 B | 14 B + opcode-specific payload |

### 20.3 32 B Payload 理论占比

| 路径 | 协议开销 | Payload 占比 |
| --- | ---: | ---: |
| C2 H0/O0 | 9 B | 78.0% |
| C2 H2/O1 | 25 B | 56.1% |
| C3 H0 | 7 B | 82.1% |
| C3 H1 | 23 B | 58.2% |
| C4 H0/O1 | 27 B | 54.2% |
| C4 H1/O1 | 43 B | 42.7% |
| 当前 v6 A0 Hop DATA | 77 B | 29.4% |
| 当前 v6 A0 Hop+E2E DATA | 101 B | 24.1% |

这些数字仍未计算 Carrier 分片/填充，但已经把每个组成部分列明，后续不得用模糊的“基础头”掩盖真实数据路径开销。

## 21. 不同应用场景应选择什么方案

### 21.1 计算口径

本节统一使用下面的口径，避免把不同层的数字混为一谈：

```text
UCN协议开销
  = Core Contract基础头
  + Origin Tag
  + Hop Protection
  + 业务专用Envelope

UCN逻辑帧长度
  = UCN协议开销 + 业务Payload

实际Carrier数据区占用
  = Carrier封装/分片/填充 + UCN逻辑帧长度
```

除 Carrier 专节外，表中的“协议开销”均不包含：

- 业务 Payload；
- Ethernet/Wi-Fi/ESP-NOW/CAN/UART 自身物理头；
- PHY preamble、inter-frame gap、ACK、bit stuffing；
- 无线重传和冲突退避；
- Route/Flow 建立时的一次性控制报文。

Payload 效率计算为：

```text
payload_efficiency = payload_bytes / (payload_bytes + UCN_protocol_overhead)
```

### 21.2 自动选型决策树

发送端不要求应用手工选择每个字段。Runtime 按下面的顺序自动选择：

```text
是否为Bootstrap/认证/建路/建Flow/恢复？
  是 → C0
  否 ↓
是否为Group/Cluster多目标发布？
  是 → C5
  否 ↓
是否存在ACTIVE Direct Flow，且下一跳就是最终目标？
  是 → C2
  否 ↓
是否存在ACTIVE Label Route？
  是，并且仅Best Effort + One Way + 无E2E → C3
  是，但需要Latest/Reliable/Request/E2E/Realtime/Transfer → C4
  否 → C1
```

随后选择 Origin Security：

```text
Endpoint要求保密       → O2 AEAD
Endpoint要求来源/完整性 → O1 Auth
所有中继和目标链路可信   → O0 None
```

最后选择 Hop Protection：

```text
Carrier已提供等价Peer认证与Replay → H0
一跳Flow且O1/O2可绑定Direct Link → H2 Combined
普通单播不可信链路               → H1
Group不可信链路                  → H3
```

选择结果必须同时满足 Endpoint Policy、Peer Capability、整条 Path Capability 和 Link Threat Policy。缺少任一 REQUIRED 能力都返回错误，不选择更弱方案。

### 21.3 普通数据场景

以下均以 A1/2 B Realm 地址为基准。

| 场景 | 推荐组合 | 开销计算 | 协议开销 | 32 B Payload效率 |
| --- | --- | --- | ---: | ---: |
| 偶发一跳、受控有线、无需Flow | C1+H0+O0 | 13 | 13 B | 71.1% |
| 偶发多跳、每跳可信、无需E2E | C1+H0+O0 | 13 | 13 B | 71.1% |
| 偶发无线、信任中继、Hop认证 | C1+H1+O0 | 13+16 | 29 B | 52.5% |
| 偶发多跳、不信任中继、E2E认证 | C1+H1+O1 | 13+16+16 | 45 B | 41.6% |
| 偶发多跳、不信任中继、E2E加密 | C1+H1+O2 | 13+16+16 | 45 B | 41.6% |
| 稳定一跳、Carrier已认证 | C2+H0+O0 | 9 | 9 B | 78.0% |
| 稳定一跳、UCN Hop认证 | C2+H1+O0 | 9+16 | 25 B | 56.1% |
| 稳定一跳、E2E认证 | C2+H2+O1 | 9+16 | 25 B | 56.1% |
| 稳定一跳、E2E加密 | C2+H2+O2 | 9+16 | 25 B | 56.1% |
| 稳定多跳、受控有线、Best Effort | C3+H0+O0 | 7 | 7 B | 82.1% |
| 稳定多跳、Hop认证、信任中继 | C3+H1+O0 | 7+16 | 23 B | 58.2% |
| 稳定多跳、Carrier认证、E2E认证 | C4+H0+O1 | 11+16 | 27 B | 54.2% |
| 稳定多跳、Hop认证+E2E认证 | C4+H1+O1 | 11+16+16 | 43 B | 42.7% |
| 稳定多跳、Hop认证+E2E加密 | C4+H1+O2 | 11+16+16 | 43 B | 42.7% |

选择原则：

- 数据只发一两次时优先 C1，避免为了节省几字节先交换 Flow Setup；
- 同一 Service 持续发送时使用 C2；
- 多跳 Best Effort 且信任中继时使用 C3；
- 只有已经存在 ACTIVE 多跳 Label Route，且需要端到端去重、Latest、Reliable、Request、
  Realtime、Transfer 或不信任中继时才使用 C4；无 Label Route 的偶发业务仍可用 C1，稳定一跳
  Direct Flow 仍用 C2，不能因高级语义一律升级 C4；
- O1 与 O2 的开销相同，区别是明文认证还是加密，不需要为“是否加密”再增加头字段。

### 21.4 控制命令与 Request/Result

| 场景 | 推荐组合 | 开销计算 | 协议开销 | 32 B业务Payload效率 |
| --- | --- | --- | ---: | ---: |
| 一跳安全 Request | C2+H2+O1/O2+Request | 9+16+9 | 34 B | 48.5% |
| 一跳安全 Result | C2+H2+O1/O2+Result | 9+16+11 | 36 B | 47.1% |
| 多跳安全 Request | C4+H1+O1/O2+Request | 11+16+16+9 | 52 B | 38.1% |
| 多跳安全 Result/Error | C4+H1+O1/O2+Result | 11+16+16+11 | 54 B | 37.2% |
| 一跳受控有线 Request | C2+H0+O0+Request | 9+9 | 18 B | 64.0% |
| 多跳受控有线 Request | C4+H0+O0+Request | 11+9 | 20 B | 61.5% |

Operation ID 只在 Request/Result/Error 出现：非 Transfer 位于 Operation Envelope，Transfer
则只位于先行 Setup，Fragment 不重复。周期 One-Way 控制量不携带 8 B Operation ID。

安全执行顺序：

1. Hop/Origin 认证；
2. Flow 与 ACL；
3. Operation ID correlation，以及按 Execution Semantics 选择易失去重或 durable journal；
4. Realtime/Deadline；
5. 业务执行；
6. 生成 Result。

### 21.5 大消息与 Transfer

| 场景 | 推荐组合 | 开销计算 | 每片协议开销 |
| --- | --- | --- | ---: |
| 一跳受控有线 Fragment | C2+H0+O0+Fragment | 9+8 | 17 B |
| 一跳安全 Fragment | C2+H2+O1/O2+Fragment | 9+16+8 | 33 B |
| 多跳受控有线 Fragment | C4+H0+O0+Fragment | 11+8 | 19 B |
| 多跳 Hop+E2E 安全 Fragment | C4+H1+O1/O2+Fragment | 11+16+16+8 | 51 B |
| 多跳安全 SACK/Credit | C4+H1+O1/O2+SACK | 11+16+16+14 | 57 B |

这些开销是每个 Fragment 的成本。为了避免小片低效：

- 先按 DATA Contract 计算 `data_payload_budget`；能装下完整业务 Payload 时必须直接 DATA；
- 只有 DATA 放不下时才按 C1 12 B、C2/C4 8 B Fragment Envelope 计算对应 `fragment_payload_budget`；
- Transfer 应使用 Path 实际 Payload Budget 选择尽可能大的 Fragment；
- 不能为了固定 32 B Fragment 而浪费 CAN-FD、UART 或无线的大 MTU；
- 总长度、消息摘要、Service、Delivery/Interaction Policy 保存在本次 Transfer Setup/状态中，
  不在每片重复；长期 Flow Context 只保存父代际、Service/Policy 上限、MTU、安全和 Credit
  等跨 Transfer 稳定字段，不保存单次消息摘要或重组进度；
- 极小消息不得进入 Transfer，直接使用 C1/C2/C4。

`data_payload_budget` 与 `fragment_payload_budget` 不相等，不能用后者判断是否需要分片：

```text
data_payload_budget = path_frame_mtu - DATA头 - 安全Tag - DATA可选扩展
fragment_payload_budget = path_frame_mtu - Fragment头 - 安全Tag - Fragment可选扩展
```

如果 Payload 大于 Fragment Budget 但仍小于 DATA Budget，它仍是一帧 DATA；错误使用 Fragment Budget 会无谓增加 Transfer 状态、ACK 和头开销。

例如多跳全安全场景发送 512 B 业务数据：

- 若每片业务 Payload 为 128 B，需要 4 片；
- 每片协议开销 51 B；
- 总协议开销 `4×51=204 B`；
- 不含 Carrier 时总字节 `512+204=716 B`；
- Payload 效率约 71.5%。

若错误地使用 32 B Fragment：

- 需要 16 片；
- 总协议开销 `16×51=816 B`；
- Payload 效率只约 38.6%。

所以 Transfer 的关键不只是缩短头，还要按 Path MTU 增大片段。

### 21.6 Realtime 场景

| 场景 | 推荐组合 | 开销计算 | 协议开销 | 32 B业务Payload效率 |
| --- | --- | --- | ---: | ---: |
| 普通非实时一跳安全流 | C2+H2+O1/O2 | 9+16 | 25 B | 56.1% |
| 一跳同步时间戳/Deadline | C2+H2+O1/O2+RT | 9+16+16 | 41 B | 43.8% |
| 普通非实时多跳安全流 | C4+H1+O1/O2 | 11+16+16 | 43 B | 42.7% |
| 多跳同步时间戳/Deadline | C4+H1+O1/O2+RT | 11+16+16+16 | 59 B | 35.2% |
| 多跳实时+Hop Budget | C4+H1+O1/O2+RT+Budget | 11+16+16+16+2 | 61 B | 34.4% |
| 受控有线多跳实时 | C4+H0+O0+RT | 11+16 | 27 B | 54.2% |

Realtime 开销看起来仍然明显，但它只由明确启用 Timed Endpoint 的 Frame 承担。普通数据不会因为系统中存在时间同步模块而增加 1 B。

建议：

- 只需要本地采样日志的节点使用 `LOCAL_STAMP`，线上额外 0 B；
- 目标端需要知道采样时间时使用 16 B Realtime Envelope；
- 中继只有确实需要 Deadline-aware scheduling 时才增加 2 B Budget；
- 不应把 64-bit Deadline 和多个 uncertainty 分量逐跳重复放入 Header。

### 21.7 Cluster 与大规模网络场景

| 场景 | 推荐组合 | 开销计算 | 协议开销 | 说明 |
| --- | --- | --- | ---: | --- |
| Cluster内普通一跳遥测 | C2按普通安全策略 | 9/25 B | 9～25 B | 零Cluster字段 |
| Cluster内普通多跳Best Effort | C3按Link策略 | 7/23 B | 7～23 B | 零Cluster字段 |
| Cluster内普通多跳安全业务 | C4+H1+O1/O2 | 11+16+16 | 43 B | 零Cluster字段 |
| 单播Authority/Config控制 | C4+H1+O1/O2+Cluster | 11+16+16+14 | 57 B | 再加Opcode-specific payload |
| Group成员通知，Carrier可信 | C5+H0+O1/O2 | 13+16 | 29 B | 非Authority通知 |
| Group成员通知，UCN Hop保护 | C5+H3+O1/O2 | 13+16+16 | 45 B | 非Authority通知 |
| Group Cluster控制 | C5+H3+O1/O2+Cluster | 13+16+16+14 | 59 B | 必须有独立可验认证/证书 |

这里最重要的结论是：

> 节点数量变多只增加本地目录、Cluster控制和少量建路成本，不应增加每个普通业务帧的固定头。

对几千节点：

- Realm 使用 A1/2 B Address；
- Cluster ID、Epoch、Config 保存在 Gateway/Flow Context；
- Cluster 间骨干使用 C4 Label Flow；
- 普通成员数据仍按一跳/多跳、安全需求选择 C2/C3/C4。

### 21.8 Group、广播与发现

| 场景 | 推荐组合 | 协议开销 |
| --- | --- | ---: |
| Link-local公开Bootstrap发现 | C0+H0+O0 | C0 A1 25 B + Opcode payload |
| 已认证Peer Discovery | C0+H1+O0/O1 | 41 B 或 57 B + Opcode payload |
| 受信Carrier Group发布 | C5+H0+O1/O2 | 29 B |
| 不可信Link Group发布 | C5+H3+O1/O2 | 45 B |
| 精确Principal签名Group控制 | C5安全开销+Payload签名 | 45 B + 签名/证书 |

Bootstrap 消息可能比普通数据大很多，因为它必须携带 Identity、nonce、Cookie、签名和 transcript。它们是低频控制成本，不能用来代表稳态数据开销。

### 21.9 诊断与维护

| 场景 | 推荐组合 | 开销 |
| --- | --- | ---: |
| 一跳本地诊断、可信USB | C1/C2+H0+O0 | 9～13 B |
| 远程诊断、E2E认证 | C4+H1+O1 | 43 B |
| 远程配置修改 | C4+H1+O1/O2+Request | 52 B |
| 固件块传输 | C4+H1+O2+Transfer | 51 B/片 |

读取统计可以是 Best Effort/Request；修改配置、擦写、升级必须使用 Request + E2E + durable operation policy，不能为了省 9 B Operation Envelope 放弃幂等保护。

### 21.10 不同 Payload 大小时的效率

| 方案/开销 | 8 B | 16 B | 32 B | 64 B | 128 B | 256 B |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| C3 H0 / 7 B | 53.3% | 69.6% | 82.1% | 90.1% | 94.8% | 97.3% |
| C2 H0 / 9 B | 47.1% | 64.0% | 78.0% | 87.7% | 93.4% | 96.6% |
| C1 H0 / 13 B | 38.1% | 55.2% | 71.1% | 83.1% | 90.8% | 95.2% |
| C3 H1 / 23 B | 25.8% | 41.0% | 58.2% | 73.6% | 84.8% | 91.8% |
| C2 H2 / 25 B | 24.2% | 39.0% | 56.1% | 71.9% | 83.7% | 91.1% |
| C4 H0 O1 / 27 B | 22.9% | 37.2% | 54.2% | 70.3% | 82.6% | 90.5% |
| C1 H1 / 29 B | 21.6% | 35.6% | 52.5% | 68.8% | 81.5% | 89.8% |
| C4 H1 O1 / 43 B | 15.7% | 27.1% | 42.7% | 59.8% | 74.9% | 85.6% |
| C5 H3 O1 / 45 B | 15.1% | 26.2% | 41.6% | 58.7% | 74.0% | 85.0% |
| C4安全Fragment / 51 B | 13.6% | 23.9% | 38.6% | 55.7% | 71.5% | 83.4% |
| C4安全Realtime / 59 B | 11.9% | 21.3% | 35.2% | 52.0% | 68.4% | 81.3% |

这张表说明：

- 小 Payload 对固定 Tag 最敏感；
- 高频 8 B 传感器值不应一条值发一帧，可在同一 Flow 中聚合多个采样；
- 64～256 B Payload 时，大多数低开销 Contract 已达到较好效率；
- Transfer 应优先使用大 Fragment；
- 是否需要 E2E/Realtime 必须按业务风险选择，不能只看效率。

### 21.11 Carrier 额外开销

#### 21.11.1 CAN-FD / Datagram / ESP-NOW

这些 Carrier 自带帧边界和长度，因此 UCN Carrier Header 为 0 B。CAN-FD 仍可能因 DLC 档位产生 Padding。

以 32 B Payload 为例：

| UCN组合 | UCN逻辑帧 | CAN-FD数据区 | Padding | 含Padding效率 |
| --- | ---: | ---: | ---: | ---: |
| C1 H0/O0 | 45 B | 48 B | 3 B | 66.7% |
| C2 H0/O0 | 41 B | 48 B | 7 B | 66.7% |
| C3 H0/O0 | 39 B | 48 B | 9 B | 66.7% |
| C2 H2/O1 | 57 B | 64 B | 7 B | 50.0% |
| C3 H1/O0 | 55 B | 64 B | 9 B | 50.0% |
| C4 H1/O1 | 75 B | 需要Carrier分片 | — | 由分片Contract决定 |

CAN-FD 物理帧头、CRC、bit stuffing 和总线仲裁时间不在上表中。

#### 21.11.2 UART/USB Stream

- 有 Auth Tag 或底层完整性：增加 6 B Stream Carrier；
- 原始 UART 且 H0/O0：增加 10 B，包括 CRC32C。

32 B Payload 示例：

| UCN组合 | UCN开销 | Stream开销 | 合计非业务开销 | Payload效率 |
| --- | ---: | ---: | ---: | ---: |
| C2 H0/O0 + Raw UART CRC | 9 B | 10 B | 19 B | 62.7% |
| C2 H2/O1 + Stream framing | 25 B | 6 B | 31 B | 50.8% |
| C3 H1/O0 + Stream framing | 23 B | 6 B | 29 B | 52.5% |
| C4 H1/O1 + Stream framing | 43 B | 6 B | 49 B | 39.5% |

USB 自身已经有链路 CRC 时，Stream Carrier CRC32C 可省略，但 Sync/Length/Header Check 是否全部需要仍应由最终 USB Adapter Contract 审计。

#### 21.11.3 Classic CAN 候选分片

Classic CAN 只有 8 B 数据区。若采用候选固定 Carrier：

```text
START    = control 1 B + logical length 2 B + data 5 B
CONTINUE = control 1 B + data up to 7 B
frame_count(L) = 1 + ceil(max(0, L-5) / 7)
```

其中 `L` 是完整 UCN 逻辑帧长度。以每个 CAN 数据区固定占满 8 B 计算：

| 32 B Payload组合 | L | CAN帧数 | 数据区总占用 | 含Carrier/填充效率 |
| --- | ---: | ---: | ---: | ---: |
| C3 H0/O0 | 39 B | 6 | 48 B | 66.7% |
| C2 H0/O0 | 41 B | 7 | 56 B | 57.1% |
| C3 H1/O0 | 55 B | 9 | 72 B | 44.4% |
| C2 H2/O1 | 57 B | 9 | 72 B | 44.4% |
| C4 H1/O1 | 75 B | 11 | 88 B | 36.4% |

这是 Carrier 设计候选，不是已冻结 Classic CAN 格式。最终还必须验证：

- 每片乱序、重复和丢失；
- Carrier ID 冲突；
- START 被覆盖；
- 超时和槽满；
- Tag 是覆盖完整 Carrier 还是逐片认证；
- CAN 仲裁、bit stuffing 和 ACK 的真实总线时间。

### 21.12 Context 建立成本何时值得

C2/C3/C4 会先产生 Flow/Route Setup 控制成本，因此不能对只发一帧的数据强制建 Flow。

设：

```text
setup_bytes = 一次Context/Route建立总字节
save_per_frame = C1每帧开销 - 目标Contract每帧开销
break_even_frames = ceil(setup_bytes / save_per_frame)
```

示例：

- C1 H1/O0 为 29 B，C2 H1/O0 为 25 B，每帧只省 4 B；短流未必值得建 C2；
- C1 H1/O1 为 45 B，C2 H2/O1 为 25 B，每帧省 20 B，一跳安全周期流很快回本；
- C1 H1/O0 为 29 B，C3 H1/O0 为 23 B，每帧省 6 B，但 C3 还减少中继查路和解析成本；
- C1 H1/O1 为 45 B，C4 H1/O1 为 43 B，每帧只省 2 B，C4 的主要收益是 Label fast path、冻结 Policy 和可靠语义，不只是字节。

Runtime 可以根据 Endpoint 的预计频率、已发送帧数和 Context 容量自动决定是否从 C1 提升到 C2/C3/C4，但一旦提升必须完成完整 PREPARE/COMMIT，不能中途假设 Context 已经存在。

### 21.13 推荐场景配置模板

#### 模板 A：最小有线传感网络

```ini
Realm Address = A0/A1
Default Data  = C1
Stable Flow   = C2/C3
Origin        = O0
Hop           = H0
Cluster       = OFF
Realtime      = OFF
Transfer      = optional
```

典型开销：7～13 B。

#### 模板 B：无线安全传感网络

```ini
Default Data  = C1+H1
Stable Direct = C2+H2+O1/O2
Stable Routed = C4+H1+O1/O2
Cluster       = optional
Realtime      = per Endpoint/selected Contract
```

典型开销：一跳 25 B，多跳 43 B。

#### 模板 C：高速多跳骨干

```ini
Route         = Label switched
Best Effort   = C3
Secure Flow   = C4
Hop           = H0 if bearer-authenticated, otherwise H1
FPGA FastPath = C3/C4 label lookup
```

典型开销：受信骨干 7～27 B；UCN 自行全安全 23～43 B。

#### 模板 D：万级分簇网络

```ini
Realm Address = A1 or A2
Member Data   = C2/C3/C4
Cluster Ctrl  = C4/C5 payload
InterCluster  = C4 secured label flow
Directory     = Context, not per-frame field
```

普通数据仍为 7～43 B；只有 Cluster Control 为 57～59 B 加 Opcode-specific Payload。

#### 模板 E：实时控制网络

```ini
Normal Status = C2/C3 without RT envelope
Timed Command = C2/C4 + 16 B RT envelope
Hop Budget    = only deadline-aware Flow
Security      = H2 direct or H1+O2 routed
```

典型开销：非实时 7～25 B；一跳实时安全 41 B；多跳实时全安全 59～61 B。

### 21.14 场景选型的最终原则

按优先级排序：

1. 先满足安全、可靠、实时和 Authority 的 REQUIRED 合同；
2. 在满足合同的固定 Contract 中选择最短者；
3. 短流用 C1，长流再建立 Context；
4. 一跳用 C2，多跳用 C3/C4；
5. 普通数据绝不携带 Cluster/Realtime/Transfer 字段；
6. Carrier 已提供等价能力时不重复实现；
7. 不因追求最短头而把攻击、重放或错配风险转移给无法验证的节点假设。

## 22. 哪些字段明确不再进入普通 Frame

以下字段只在 C0 Setup 或节点 Context 中存在：

- Wire Address Class；
- Realm ID（普通 Realm 内 Flow）；
- Source/Destination Binding Generation；
- Peer Session Generation；
- Key ID/Key Generation；
- Security Suite ID；
- Source/Destination Endpoint；
- Protocol Opcode（Flow 已绑定时）；
- 64-bit Operation ID（普通 One-Way 不存在；非 Transfer Request/Result/Error 位于 Operation Envelope，Transfer 时只位于 Setup；长期 Flow Context 不保存单次 Operation ID）；
- Route Generation；
- Path Generation；
- 64-bit initial/remaining budget；
- Cluster ID/Epoch/Config；
- Capability bitmap；
- Payload Length（由 Carrier 得出）。

这些语义没有消失，只是由经过认证、不可跨代复用的 Context 保存。

## 23. 失败与恢复原则

- 未知 Contract：拒绝；
- 未知 Flow/Label/Group Context：拒绝并发送有界错误，不猜测完整身份；
- Context 表满：不驱逐 ACTIVE 项；
- Route/Flow Setup 失败：旧 Active 保持；
- Link reopen：相关 Hop/Flow/Label 一次性 Fence；
- Parent Generation 改变：子 Context 全部失效；
- Sequence/ID 耗尽：轮换父代际或 Fault，不回绕；
- Security REQUIRED 无可用 Profile：失败，不退到 O0/H0；
- MTU 不足：选择等价短 Contract或 Transfer，不关闭安全要求；
- Cluster/Realtime/Transfer 不支持：只拒绝要求对应能力的 Endpoint/Request，不影响普通 C1/C2/C3；
- 持久化失败：承诺消息不得发送。

## 24. 生产 Codec 前必须独立冻结的精确 Registry

本文的 Contract 分层、软路由/Flow 分界和功能按需付费语义已完成内部自审；但精确线上 ABI 仍需独立的 Registry/Golden 冻结。以下任一项未完成时，可以审查模块逻辑和离线 typed model，但不能开始生产 Codec/RX/TX：

1. C0 每个 Opcode，以及 C1/C2/C4/C5 Control/Diagnostic 2 B code registry 的精确 Payload byte offset；
2. O1/O2/H1 的具体密码 Suite、Nonce 和 canonical AAD；
3. 12 B Hop Tag 的定量伪造概率与密钥轮换上限；
4. H2 Combined Proof 的严格 Key/AAD 隔离；
5. Flow/Label/Group Context Record 的精确字段和静态容量；
6. A0～A3 Realm Manifest 与跨 Realm Gateway；
7. C0 Bootstrap 大证明以及 43/52/54 B Transfer Setup 在小 MTU Carrier 上的安全分片；
8. Advanced Flow Route Stage/Commit 的五个 Opcode、并发、部分提交、ACK 重放、terminal receipt、掉电和 `IN_DOUBT` 边界；基础 SoftRoute 不得出现这五个状态；
9. Transfer Fragment 的最大数量和重组预算；
10. Group multicast 的 Sender 身份与 Tree Hop Key 模型；
11. Stream/Classic CAN Carrier 的精确 framing；
12. 每个 Contract 的 Golden、Negative 和 fuzz oracle。

## 25. 建议冻结顺序

```text
W0  用户确认 C0..C5 和三层结构
W1  冻结 Common 3 B 与 Realm 地址宽度
W2  冻结 Hop/Origin Security 域
W3  冻结 C0 和 Bootstrap/Context Setup
W4  冻结 C1/C2 普通数据
W5  冻结 C3/C4 Label Route
W6  冻结 C5 Group
W7  冻结 Payload Envelope
W8  Golden/Negative/Fuzz，仅 default-OFF Codec
W9  独立安全和资源外审
W10 才允许迁移生产 Runtime
```

任何阶段如果发现必须给所有普通帧增加字段，应先证明该字段不能放进：

1. Carrier；
2. C0 Setup；
3. Flow/Label/Group Context；
4. 仅相关业务的 Payload Envelope；
5. 节点本地状态。

只有五处都不能安全承载时，才允许修改公共 3 B Header。

## 26. 本设计的最终效果

这套方案没有删掉 UCN 已经建立的功能，而是重新安排它们出现的位置：

- 自动寻路：C0 建路，C1 无状态发送，C3/C4 Label 稳态转发；
- 普通数据：C1/C2/C3，不承担无关功能；
- 无线安全：H1/O1/O2 按威胁模型启用；
- 大规模网络：短地址 + Cluster Directory + Label 骨干；
- Cluster：只在控制 Payload 和 Group/跨簇 Flow 中出现；
- Realtime：Local Stamp 不上线；Timed Envelope 只在明确选择它的 Contract Payload 中出现，
  Network Time Sync v1 才强制 C4 Flow；
- Transfer：只在大消息 Fragment/SACK 中出现；
- Request/Result：只在对应消息携带 Operation ID；
- FPGA/硬件转发：C3/C4 只需 Label fast path；
- Full/Lite/Nano：同一协议，不同固定资源和 Capability。

最常用的稳定一跳 Core Header 为 9 B，最短多跳 Core Header 为 7 B；即使由 UCN 自己提供逐跳 96-bit 认证，总开销也分别为 25 B 和 23 B。Cluster、Realtime、Transfer 未使用时均为零附加字节。

这才是“功能完整但按需付费”的统一协议，而不是让每一帧为整个协议的所有潜在能力买单。
