# V6S-00-03 Core Wire 精确布局与安全覆盖

> 状态：`DONE / SELF-REVIEW PASS / FINAL EXTERNAL REVIEW PENDING`
>
> 本文冻结简化版 C0～C5 的 Core Packet ABI。它不实现 Encoder/Decoder；旧 V6 Contract 1
> 的 9 B 前缀、Magic、固定 CRC 和 41～47 B 基础头不得混入本格式。

## 1. Core Packet 分层

```text
Carrier framing
  └─ Core Packet
       ├─ Common Header                 3 B
       ├─ Contract fields              fixed by C0..C5
       ├─ Payload                      exact remaining length
       ├─ Origin Tag                   O0=0 B, O1/O2=16 B
       └─ Hop trailer                  H0/H2=0 B, H1/H3=16 B
```

Core Packet 不携带 Magic、总 Length 或通用 CRC。Datagram/CAN/CAN-FD 必须由 Carrier 提供
精确边界和随机错误检测；Stream 使用独立 Carrier framing。若 H0/O0 且 Carrier 没有等价
FCS，Carrier 必须附加 CRC32C，不能偷偷塞回 Core Header。

## 2. 共同 3 B Header

| Offset | Bit | 字段 | 合法值 |
| ---: | --- | --- | --- |
| 0 | 7..4 | Protocol Version | 固定 6 |
| 0 | 3..0 | Header Contract | C0=0、C1=1、C2=2、C3=3、C4=4、C5=5 |
| 1 | 7..6 | Traffic Class | Q0～Q3 |
| 1 | 5..4 | Delivery Guarantee | BestEffort=0、Latest=1、Reliable=2；3 拒绝 |
| 1 | 3..2 | Interaction Role | OneWay=0、Request=1、Result=2、Error=3 |
| 1 | 1..0 | Payload Kind | Data=0、Control=1、Transfer=2、Diagnostic=3 |
| 2 | 7..6 | Origin Security | O0=0、O1=1、O2=2；3 拒绝 |
| 2 | 5..0 | Hop Limit | `1..63` |

```text
byte0 = 0x60 | contract
byte1 = traffic<<6 | delivery<<4 | interaction<<2 | payload_kind
byte2 = origin_security<<6 | hop_limit
```

全部保留组合在任何状态写入前拒绝。结构合法性必须先满足 2.1 的完整矩阵；矩阵允许只表示
该组合可被唯一解析，最终仍须通过 Endpoint/Context/Authority Policy。

### 2.1 Contract 结构组合矩阵

| Contract | Delivery | Interaction | Payload Kind | Origin Security | Hop Profile |
| --- | --- | --- | --- | --- | --- |
| C0 | BestEffort/Reliable | OneWay/Request/Result/Error | Control/Diagnostic | O0/O1/O2 | H0/H1 |
| C1 | BestEffort/Latest/Reliable | OneWay/Request/Result/Error | Data/Control/Transfer/Diagnostic | O0/O1/O2 | H0/H1 |
| C2 | BestEffort/Latest/Reliable | OneWay/Request/Result/Error | Data/Control/Transfer/Diagnostic | O0/O1/O2 | H0/H1/H2 |
| C3 | BestEffort | OneWay | Data/Diagnostic | O0 | H0/H1 |
| C4 | BestEffort/Latest/Reliable | OneWay/Request/Result/Error | Data/Control/Transfer/Diagnostic | O0/O1/O2 | H0/H1 |
| C5 | BestEffort/Latest/Reliable | OneWay/Result/Error | Data/Control/Diagnostic | O1/O2；Manifest 明确的无 Authority public discovery 才允许 O0 | H0/H3 |

附加结构规则：

- H2 只允许 `C2 + O1/O2`，且唯一 Direct Flow Context 必须明确选择 Combined Proof；
- C0 的未绑定 Bootstrap 首帧固定为 `O0/H0/HopLimit=1`。后续只有已建立唯一 provisional
  Link Context 才可使用 H1；已绑定控制事务才可使用 O1/O2；
- C3 不允许 Control/Transfer、Latest/Reliable、Request/Result/Error 或 O1/O2；
- C5 不允许 Request 或 Transfer；需要逐目标请求时由 Planner 建立有界单播事务，不能把一次
  Group 发布解释为每个成员都执行了 Request；
- H Profile 不在线上自报，必须由最小 Context selector 在密码调用前唯一解析；矩阵中存在该
  Profile 不代表远端可自行选择或降级；
- Interaction 非 OneWay 且 Kind 非 Transfer 时必须携带 Operation Envelope；Transfer 的业务
  关联只来自已认证 Setup，不在每个 Fragment 重复。

### 2.2 Protocol Opcode/Subtype 的唯一位置

| Contract/Kind | 线上位置 | 规则 |
| --- | --- | --- |
| C0 Control/Diagnostic | C0 Header 的 `Protocol Opcode:u16` | Payload offset 0 直接进入 Operation Envelope 或 Opcode Body，不重复编码 Opcode |
| C1/C2/C4/C5 Control | Payload offset 0 的 `Protocol Opcode:u16` | 非零且必须位于对应模块 Registry；其后才是条件 Envelope/Body；C5 仍受 OneWay/Result/Error 和 Group Context 约束 |
| C1/C2/C3/C4/C5 Diagnostic | Payload offset 0 的 `Diagnostic Code:u16` | 只能观察；有副作用的操作必须改用 Control |
| C1/C2/C4 Transfer | Payload offset 0 的固定 `Fragment/SACK subtype` | 由先行 C0 Setup 精确绑定；不是通用 Protocol Opcode |
| Data | 不存在 | Payload byte 0 就是业务数据，canonical protocol opcode 为 0 |

因此 C2/C4/C5 不能从 Service、总长度或 Payload 内容猜 Opcode。未知/零 code、重复在 Body
中放第二个“真实 Opcode”、或 Kind 与上述位置不一致，均在 Replay/状态写入前拒绝。

## 3. 地址宽度

Realm Manifest 为每个 Realm 固定一个地址宽度 `W=1/2/3/4`。C0 先读固定位置的 32-bit
Realm ID，再取得唯一 W；C1 从已经绑定该 Link 的 Realm Context 取得唯一 W。缺失、存在多个
候选或 Manifest 代际不确定时拒绝，不按总长度试猜。

普通地址范围是 `1..(2^(8W)-2)`；全 0 和全 1 保留。未绑定 C0 的 Source 使用全 0，
Link-local Authority Destination 使用全 1，且必须 `HopLimit=1`、禁止转发。

## 4. C0 ABSOLUTE

| Offset | 长度 | 字段 |
| ---: | ---: | --- |
| 0 | 3 | Common Header |
| 3 | 4 | Realm ID |
| 7 | W | Source Address |
| `7+W` | W | Destination Address |
| `7+2W` | 4 | Source Binding Generation |
| `11+2W` | 4 | Destination Binding Generation |
| `15+2W` | 8 | C0 Transaction ID |
| `23+2W` | 2 | Protocol Opcode |
| `25+2W` | N | Opcode Payload |

基础头为 `25+2W`：A0/A1/A2/A3 分别 27/29/31/33 B。Bootstrap 的 Binding Generation
均为 0；已绑定控制事务必须均非零并与当前 Context 精确匹配。

## 5. C1 STATELESS

| Offset | 长度 | 字段 |
| ---: | ---: | --- |
| 0 | 3 | Common Header |
| 3 | W | Source Address |
| `3+W` | W | Destination Address |
| `3+2W` | 2 | Service ID |
| `5+2W` | 4 | Origin Sequence |
| `9+2W` | N | Payload |

基础头为 `9+2W`：A0/A1/A2/A3 分别 11/13/15/17 B。Control/Diagnostic Payload 的
前 2 B 是 Protocol Opcode；Data 不携带 Opcode。C1 地址防 ABA 依赖当前绑定、安全 Context
和 Queue/Link Fence，不允许仅凭裸地址恢复 Principal。

## 6. C2 DIRECT

| Offset | 长度 | 字段 |
| ---: | ---: | --- |
| 0 | 3 | Common Header |
| 3 | 2 | Flow Context ID |
| 5 | 4 | Origin Sequence |
| 9 | N | Payload |

基础头 9 B。下一跳必须是最终 Destination。Context 必须展开出完整双方 Principal/Binding、
Service、Policy、Security、MTU 和可选 Realtime/Transfer 约束。Context 不唯一时拒绝。
Control/Diagnostic 的 2 B code 和 Transfer subtype 均从 Payload offset 0 开始；Data 无 code。

## 7. C3 ROUTED_HOP

| Offset | 长度 | 字段 |
| ---: | ---: | --- |
| 0 | 3 | Common Header |
| 3 | 2 | Forwarding Label |
| 5 | 2 | Flow Context ID |
| 7 | N | Payload |

基础头 7 B。仅允许 C3 固定组合；中继验证 Hop Protection、查当前 Link/Label、递减 Hop Limit、
替换 egress Label/Context 并重做 Hop Protection，不解析业务 Payload。

## 8. C4 ROUTED_ORIGIN

| Offset | 长度 | 字段 |
| ---: | ---: | --- |
| 0 | 3 | Common Header |
| 3 | 2 | Forwarding Label |
| 5 | 2 | Flow Context ID |
| 7 | 4 | Origin Sequence |
| 11 | N | Payload |

基础头 11 B。raw Label/Context/Hop Limit 可逐跳变化；所有 Hop 的 Context 必须展开为同一个
canonical Flow Fingerprint。Origin Tag 绑定 Fingerprint 和 Origin Sequence，不绑定 raw alias。
Control/Diagnostic 的 2 B code 和 Transfer subtype 均从 Payload offset 0 开始；Data 无 code。

## 9. C5 GROUP

| Offset | 长度 | 字段 |
| ---: | ---: | --- |
| 0 | 3 | Common Header |
| 3 | 2 | Tree Label |
| 5 | 2 | Group Context ID |
| 7 | 2 | Sender Slot |
| 9 | 4 | Origin Sequence |
| 13 | N | Payload |

基础头 13 B。默认只允许 O1/O2；O0 仅由签名 Manifest 为无 Authority 的公开发现用途显式
开启。Sender Slot 必须映射唯一 Principal/Binding/Key；共享对称 Key 不能被解释为唯一发送者证明。
Control/Diagnostic 的 2 B code 从 Payload offset 0 开始；C5 禁止 Transfer，Data 无 code。

## 10. Payload 与 Trailer 顺序

精确总长度：

```text
origin_tag_bytes = O0 ? 0 : 16
hop_trailer_bytes = (H1 or H3) ? 16 : 0
payload_bytes = carrier_core_length - base_header - origin_tag_bytes - hop_trailer_bytes
require checked subtraction and payload_bytes <= selected context budget
```

| 顺序 | 内容 | 长度 |
| ---: | --- | ---: |
| 1 | Header + Contract fields | 固定 |
| 2 | Payload 明文或 O2 ciphertext | N |
| 3 | Origin Tag | 0/16 |
| 4 | Hop Sequence | H1/H3 为 4，否则 0 |
| 5 | Hop Tag | H1/H3 为 12，否则 0 |

H2 只允许 C2+O1/O2，并复用 16 B Origin Tag 同时认证 Direct Link Context；不得再追加 Hop
Trailer。Padding 不属于 Core Packet；Carrier padding 必须为零并由 Carrier 校验或认证。

## 11. Security Suite 与 Nonce

Suite/Key 不逐帧携带，由当前唯一 Context 映射：

| Suite ID | 用途 | Tag |
| ---: | --- | ---: |
| 1 | O1 HMAC-SHA-256-128 Auth-only | 16 B |
| 2 | O2 AES-128-GCM | 16 B |
| 3 | O2 ChaCha20-Poly1305 | 16 B |
| 16 | H1/H3 HMAC-SHA-256-96 | 12 B |

同一 Context 同时只能有一个 Active Suite/Key Generation；换钥先 Fence/drain 旧映射，再发布
新映射。接收端不得尝试 previous/current 多把 Key。每个 AEAD traffic key 都有独立、同代际、
同方向的 256-bit nonce key；Nonce KDF 固定为 HMAC-SHA-256 并截取前 96 bit。

C1/C2/C4/C5 的 Nonce 输入为：

```text
ASCII "UCN6-NONCE-SEQ-V1" without NUL
canonical origin context fingerprint
u32be(origin_sequence)
```

C0 没有 Origin Sequence，固定使用另一不可混淆输入：

```text
ASCII "UCN6-NONCE-C0-V1" without NUL
canonical C0 origin context fingerprint
u64be(c0_transaction_id)
u16be(protocol_opcode)
u8(sender_direction)       # 0=initiator->responder, 1=responder->initiator
u8(interaction_role)
```

同一个 `{Context Fingerprint,Transaction ID,Opcode,Direction,Role}` 只能对应一份不可变明文和
一份 sealed artifact；精确重传复用原 ciphertext/tag，任何同元组不同 Payload 都按冲突重放
拒绝。每个方向使用不同 traffic/nonce key，所以双向相同事务值不会复用 Nonce。事务结束后同一
父域内不得复用 Transaction ID。O1 的 MAC 输入使用同一 canonical AAD 和 Payload。

## 12. Canonical Origin AAD

Origin AAD 按以下顺序拼接，不编码 C 结构体：

```text
ASCII "UCN6-ORIGIN-V1" without NUL
u32be(canonical_length_after_this_field)
byte0 VersionContract
byte1 MessageMeta
byte2 with HopLimit bits forced to zero (`byte2 & 0xC0`)
u8 contract
contract-specific immutable identity
u32be(payload_length)
```

Contract-specific immutable identity：

| Contract | canonical identity |
| --- | --- |
| C0 | Realm、双地址、双 Binding Generation、C0 Transaction ID、Opcode |
| C1 | canonical 双 Principal/Binding、Service ID、Origin Sequence |
| C2 | canonical Flow Fingerprint、Origin Sequence、Direct Context Fingerprint（H2 时） |
| C3 | 不允许 Origin Tag |
| C4 | canonical Flow Fingerprint、Origin Sequence |
| C5 | canonical Group Fingerprint、Sender Principal/Binding、Origin Sequence |

构造 canonical AAD 时必须先复制 Header，再把副本的 HopLimit 六位清零；Wire 原帧中的 HopLimit
保持不变。Golden/oracle 禁止把原始 `byte2` 直接拼入 Origin AAD。O1 对
`AAD || plaintext payload` 认证；O2 把上述字节作为 AEAD AAD，并对 ciphertext 认证。
Hop Limit、raw Label 和 raw hop-local Context ID 不进入 Origin AAD。

## 13. Canonical Hop AAD

H1/H3 的 12 B Tag 覆盖：

```text
ASCII "UCN6-HOP-V1" without NUL
u32be(canonical_length_after_this_field)
exact Common Header including current HopLimit
exact current-hop Contract fields including raw Label/Context aliases
exact Payload bytes and Origin Tag
u32be(Hop Sequence)
canonical Hop Context Fingerprint
```

因此中继修改 HopLimit/Label/Context 后必须重新分配 Hop Sequence 并重签；它不能修改
MessageMeta、Origin Sequence、Payload 或 Origin Tag。raw 本地 Link Generation 不直接上 AAD，
而由双方认证一致的 Hop Context Fingerprint 间接绑定。

## 14. Protocol Opcode Registry v1

未列值一律拒绝；模块关闭时在分配模块状态前返回 unsupported/rejected。

| 值 | 名称 | Owner |
| ---: | --- | --- |
| `0x0001` | BOOTSTRAP_HELLO | Identity/Admission |
| `0x0002` | BOOTSTRAP_COOKIE_CHALLENGE | Identity/Admission |
| `0x0003` | BOOTSTRAP_HELLO_COOKIE | Identity/Admission |
| `0x0004` | IDENTITY_CHALLENGE | Identity/Security |
| `0x0005` | IDENTITY_RESPONSE | Identity/Security |
| `0x0006` | ADDRESS_OFFER | Identity/Admission |
| `0x0007` | DEVICE_COMMIT | Identity/Admission |
| `0x0008` | FINAL_COMMIT | Identity/Admission |
| `0x0009` | ADDRESS_RENEW | Identity/Admission |
| `0x000A` | ADDRESS_REVOKE | Identity/Admission |
| `0x0010` | PEER_REAUTH_HELLO | Security |
| `0x0011` | PEER_REAUTH_CHALLENGE | Security |
| `0x0012` | PEER_REAUTH_RESPONSE | Security |
| `0x0013` | PEER_SESSION_PREPARE | Security |
| `0x0014` | PEER_SESSION_ACCEPT | Security |
| `0x0015` | PEER_SESSION_COMMIT | Security |
| `0x0016` | PEER_SESSION_ABORT | Security |
| `0x0101` | NEIGHBOR_HELLO | Capability/Neighbor |
| `0x0102` | NEIGHBOR_KEEPALIVE | Capability/Neighbor |
| `0x0103` | CAPABILITY_ADVERTISE | Capability |
| `0x0104` | CAPABILITY_SELECT | Capability |
| `0x0105` | CAPABILITY_ACK | Capability |
| `0x0106` | CAPABILITY_REVOKE | Capability |
| `0x0201` | ROUTE_RREQ | Route |
| `0x0202` | ROUTE_RREP | Route |
| `0x0203` | ROUTE_RERR | Route |
| `0x0204` | ROUTE_PROBE | Route |
| `0x0205` | ROUTE_PROBE_ACK | Route |
| `0x0301` | FLOW_PREPARE | Flow |
| `0x0302` | FLOW_ACCEPT | Flow |
| `0x0303` | FLOW_COMMIT | Flow |
| `0x0304` | FLOW_ABORT | Flow |
| `0x0305` | FLOW_RETIRE | Flow |
| `0x0310` | PATH_ACTIVATE_STAGE | Flow/Path |
| `0x0311` | PATH_STAGE_ACK | Flow/Path |
| `0x0312` | PATH_ACTIVATE_COMMIT | Flow/Path |
| `0x0313` | PATH_COMMIT_ACK | Flow/Path |
| `0x0314` | PATH_ACTIVATE_ABORT | Flow/Path |
| `0x0315` | PATH_TERMINAL_RECEIPT | Flow/Path |
| `0x0316` | PATH_PROBE | Flow/Path |
| `0x0317` | PATH_PROBE_ACK | Flow/Path |
| `0x0401` | DELIVERY_ACK | Transport |
| `0x0410` | TRANSPORT_PARENT_PREPARE | Transport |
| `0x0411` | TRANSPORT_PARENT_ACCEPT | Transport |
| `0x0412` | TRANSPORT_PARENT_COMMIT | Transport |
| `0x0413` | TRANSPORT_PARENT_ABORT | Transport |
| `0x0420` | TRANSFER_SETUP | Transport |
| `0x0421` | TRANSFER_SETUP_ACK | Transport |
| `0x0422` | TRANSFER_ABORT | Transport |
| `0x0423` | TRANSFER_TERMINAL_RECEIPT | Transport |
| `0x0424` | TRANSFER_SACK_CREDIT | Transport |
| `0x0501` | OPERATION_QUERY | Service/Operation |
| `0x0502` | OPERATION_RECEIPT | Service/Operation |
| `0x0503` | OPERATION_CANCEL | Service/Operation |
| `0x0601` | TIME_SYNC | Time |
| `0x0602` | TIME_FOLLOW_UP | Time |
| `0x0603` | TIME_DELAY_REQUEST | Time |
| `0x0604` | TIME_DELAY_RESPONSE | Time |
| `0x0605` | TIME_SYNC_ABORT | Time |
| `0x0701` | GROUP_PREPARE | Group |
| `0x0702` | GROUP_ACCEPT | Group |
| `0x0703` | GROUP_COMMIT | Group |
| `0x0704` | GROUP_ABORT | Group |
| `0x0705` | GROUP_RETIRE | Group |
| `0x0710` | GROUP_KEY_PREPARE | Group/Security |
| `0x0711` | GROUP_KEY_ACK | Group/Security |
| `0x0712` | GROUP_KEY_COMMIT | Group/Security |
| `0x0720` | GROUP_TREE_STAGE | Group |
| `0x0721` | GROUP_TREE_ACK | Group |
| `0x0722` | GROUP_TREE_COMMIT | Group |
| `0x0723` | GROUP_TREE_ABORT | Group |
| `0x0730` | GROUP_MEMBERSHIP_SNAPSHOT | Group |
| `0x0801` | CLUSTER_ADVERTISE | Cluster |
| `0x0802` | CLUSTER_JOIN | Cluster |
| `0x0803` | CLUSTER_CONFIG_PREPARE | Cluster |
| `0x0804` | CLUSTER_CONFIG_ACK | Cluster |
| `0x0805` | CLUSTER_CONFIG_COMMIT | Cluster |
| `0x0806` | CLUSTER_BACKUP_ASSIGN | Cluster |
| `0x0807` | CLUSTER_BACKUP_READY | Cluster |
| `0x0808` | CLUSTER_TAKEOVER_VOTE | Cluster |
| `0x0809` | CLUSTER_TAKEOVER_COMMIT | Cluster |
| `0x080A` | CLUSTER_HANDOVER_PREPARE | Cluster |
| `0x080B` | CLUSTER_HANDOVER_READY | Cluster |
| `0x080C` | CLUSTER_HANDOVER_COMMIT | Cluster |
| `0x080D` | CLUSTER_RECOVERY_VOTE | Cluster |
| `0x080E` | CLUSTER_RECOVERY_COMMIT | Cluster |
| `0x080F` | CLUSTER_REKEY_COMMIT | Cluster |
| `0x0810` | CLUSTER_DIRECTORY | Cluster |
| `0x0811` | CLUSTER_TUNNEL | Cluster |
| `0x0901` | DIAGNOSTIC_PING | Diagnostics |
| `0x0902` | DIAGNOSTIC_PONG | Diagnostics |
| `0x0903` | DIAGNOSTIC_STATUS_QUERY | Diagnostics |
| `0x0904` | DIAGNOSTIC_STATUS_RESULT | Diagnostics |
| `0x0905` | DIAGNOSTIC_ERROR_REPORT | Diagnostics |

具体 Opcode Body 在对应模块实现前另行冻结字段级 schema；它不能改变本文的 Opcode 数值、
Payload 起点、基础标量宽度或认证覆盖。Cluster 不再使用“单一 CLUSTER_CONTROL Opcode +
未认证子 kind”扩大 ACL；ACL 直接匹配上表的精确 16-bit Opcode。

## 15. Carrier 与 MTU

```text
path_frame_mtu = min(all verified per-hop Core Packet MTU)
payload_budget = checked_sub(path_frame_mtu,
    base_header + origin_tag + hop_trailer + selected_payload_envelopes)
```

`carrier_mtu`、`carrier_framing_overhead`、`link_frame_mtu`、`path_frame_mtu` 和
`payload_budget` 必须分开。Encoder 不自动分片；单帧预算不足时由 Resolver 选择更短但语义等价
的 Contract，或显式进入 Transfer。安全要求不能因 MTU 不足而降级。

Stream Carrier 固定前缀候选为 `Sync 2 B + CarrierVersion 1 B + Flags 1 B + Length 2 B`；
仅 H0/O0 且底层无 FCS 时再附 CRC32C 4 B。该 framing 属于 Adapter/Carrier，不进入 Core AAD；
若 Carrier padding 存在，必须为零并在交付 Core 前验证。

## 16. V6S-00-03 自审

| 检查 | 结果 |
| --- | --- |
| C0～C5 每个 offset 和基础长度可由公式机械计算 | PASS |
| 64-bit C0 Transaction 只增加低频 C0，不污染稳态帧 | PASS |
| Origin Tag 与 Hop Trailer 顺序唯一，H2 无双 Tag | PASS |
| Origin/Hop AAD 明确区分不可变身份和逐跳 alias | PASS |
| Core CRC、Carrier CRC/FCS 和 padding 责任分离 | PASS |
| Opcode 数值唯一、按 Owner 分域、ACL 匹配精确 Opcode | PASS |
| 未冻结业务 Body 明确后置，不授权提前实现 | PASS |

结论：`V6S-00-03 = DONE / SELF-REVIEW PASS`。Golden bytes 尚未计算；由 `V6S-00-04`
使用独立 fixture 和 oracle 冻结。
