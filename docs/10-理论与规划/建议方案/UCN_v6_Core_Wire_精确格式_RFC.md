# UCN v6 Core Wire 精确格式 RFC

> 状态：V6-03 隔离实现与分项自审基线；最终统一外审延期。
> 适用范围：`UCN_BUILD_V6_EXPERIMENTAL=ON` 的 default-OFF Codec。
> 禁止事项：本 RFC 尚不授权生产 RX/TX、真实 Key 查找、Tag 验证或 Authority 副作用。

## 1. 目标与边界

本文件把《UCN v6 最终协议架构与破坏性重构 RFC》第 6 章的逻辑字段冻结成唯一字节布局。
所有多字节整数均为网络序（big-endian），没有 C 结构体直拷、隐式 padding 或本机字节序。
Decoder 只接受 Version 6；使用相同 Magic 的 v4/v5 帧仍会因 Version/长度/合同不符被拒绝。

当前 Suite Registry 只登记 `suite_id=1`，其 E2E 和 Link Tag 均固定为 16 B。这只是让 Codec
可以冻结长度与 Key Selector，不表示 V6-07 的密码算法、Nonce、KDF 或 Key 生命周期已经实现。

## 2. 公共前缀

| Offset | 长度 | 字段 | 编码 |
| ---: | ---: | --- | --- |
| 0 | 1 | Magic 0 | `0x55` |
| 1 | 1 | Magic 1 | `0x43` |
| 2 | 1 | Version/Class | bit 0..5=`6`；bit 6..7=`A0..A3` |
| 3 | 1 | Frame Type | 1 Bootstrap、2 Control、3 Data、4 Transfer、5 Diagnostic |
| 4 | 1 | Flags | 第 4 节的八个存在位 |
| 5 | 1 | Traffic/Guarantee | bit 0..1=Q0..Q3；bit 2..3=Guarantee；bit 4..7 必须为零 |
| 6 | 1 | Hop Limit | 非零；逐跳递减 |
| 7 | 1 | Header Contract | 当前唯一合法值 `1` |

Guarantee 值为 0 Best Effort、1 Latest、2 Reliable；3 保留并拒绝。Frame Type 只表示大类，
Bootstrap/Control/Transfer/Diagnostic 的具体操作只能来自 Protocol Context。

## 3. 基础头与地址档位

令 `A` 为地址字节数（A0=1、A1=2、A2=3、A3=4），固定字段如下：

| Offset | 长度 | 字段 |
| ---: | ---: | --- |
| 8 | 4 | Realm ID |
| 12 | A | Source Address |
| 12+A | A | Destination Address |
| 12+2A | 4 | Source Binding Generation |
| 16+2A | 4 | Destination Binding Generation |
| 20+2A | 4 | Session Generation |
| 24+2A | 4 | Packet Sequence |
| 28+2A | 2 | Payload Length |
| 30+2A | 0 | 固定顺序扩展起点 |

没有扩展、Payload 或 Tag 时，加末尾 CRC32C 的基础 Frame 长度固定为：

| Class | Address Bytes | 最大普通地址 | Base Frame Bytes |
| --- | ---: | ---: | ---: |
| A0 | 1 | `0xFE` | 36 |
| A1 | 2 | `0xFFFE` | 38 |
| A2 | 3 | `0xFFFFFE` | 40 |
| A3 | 4 | `0xFFFFFFFE` | 42 |

每档全 1 地址是 Link-local 保留目的地址，不是普通节点地址。普通 Frame 的 Source、Destination、
双 Binding、Session 和 Packet Sequence 必须非零；32-bit 所有权序列还必须不超过
`0xFFFFFFFE`。

Bootstrap 唯一允许 Source/Binding/Session/Sequence 为零；Destination 必须是本档全 1，
Hop Limit=1，Traffic=Q0，Guarantee=Best Effort。Group HELLO 唯一允许 Destination Binding
为零；Destination 同样是本档全 1，Hop Limit=1，Traffic=Q1，Guarantee=Latest。

## 4. Flags 与扩展顺序

| Bit | Mask | 扩展 | 字节数 |
| ---: | ---: | --- | ---: |
| 0 | `0x01` | Peer Hop Security Context | 7 |
| 1 | `0x02` | Group Security Context | 15 |
| 2 | `0x04` | E2E Security Context | 8 |
| 3 | `0x08` | Protocol Context | 2 |
| 4 | `0x10` | Message/Endpoint Context | 13 |
| 5 | `0x20` | Route Context | 4 |
| 6 | `0x40` | Path Context | 6 |
| 7 | `0x80` | Hop Scheduling Context | 16 |

扩展严格按 bit 0 到 bit 7 的顺序连续编码，不允许重排或 TLV。Peer 与 Group 互斥；Route 与
Path 互斥。缺失扩展时，对应 semantic 字段和 Tag 必须全零，禁止依赖本地默认值形成第二种
canonical 表示。

### 4.1 Peer Hop Security Context

```text
suite_id:u8 | key_id:u16 | key_generation:u32
```

### 4.2 Group Security Context

```text
group_id:u32 | group_generation:u32 |
suite_id:u8 | key_id:u16 | key_generation:u32
```

### 4.3 E2E Security Context

```text
mode:u8 | suite_id:u8 | key_id:u16 | key_generation:u32
```

Mode 只允许 1 Auth-only 或 2 AEAD。没有扩展才表示 None；存在却填零必须拒绝。

### 4.4 Protocol Context

```text
protocol_opcode:u16
```

Opcode 必须非零。Data 禁止携带 Protocol Context；其他四类必须携带。当前
`CONTROL/GROUP_HELLO=1`，后续 Opcode 由各模块的固定 Registry 分配。

### 4.5 Message/Endpoint Context

```text
source_endpoint:u16 | destination_endpoint:u16 |
interaction_role:u8 | operation_id:u64
```

Data 必须携带该扩展，两个 Endpoint 必须非零。One-way 固定 Operation ID=0；Request、Result
和 Error 固定使用非零、且不超过 `0xFFFFFFFFFFFFFFFE` 的 Operation ID。Delivery Guarantee
不从 Interaction Role 推导。

### 4.6 Route、Path 与 Hop Budget

```text
Route: route_generation:u32
Path:  path_id:u16 | path_generation:u32
Budget: initial_budget_us:u64 | remaining_budget_us:u64
```

Route/Path Generation 必须是非零 checked serial，Path ID 非零。Budget 满足
`0 < remaining <= initial`。中继只能减小 remaining，不能修改 initial。

## 5. Payload、Tag 与 CRC

扩展结束后依次为：

```text
Payload[payload_length]
E2E Tag[16]                    // 仅存在 E2E Context
Peer Hop Tag[16] 或 Group Tag[16]
CRC32C[4]
```

CRC32C 使用 reflected Castagnoli polynomial `0x82F63B78`、初值 `0xFFFFFFFF`、终值按位取反，
覆盖除 CRC 自身外的完整 Wire Frame。CRC 只用于随机错误早拒绝，不代替安全 Tag。

理论最大 Frame 为 65,669 B（A3、最大互斥扩展组合、65,535 B Payload、双 16 B Tag 和 CRC）。
Decoder 在 CRC 扫描前拒绝超过该值的输入；产品实际接受上限仍由 Path Frame MTU 限制。

## 6. Frame 类型约束

### 6.1 Bootstrap

- 只允许 Protocol Context；
- 不允许 Peer/Group/E2E/Message/Route/Path/Budget；
- 使用 Bootstrap transcript 自身的认证流程，不能伪造普通 Session Tag。

### 6.2 Group HELLO

- Frame Type=Control、Protocol Opcode=1；
- 必须有 Group Context，禁止 Peer Context；
- 禁止 E2E、Message、Route、Path、Budget；
- 只能产生有界重认证提示，不可路由转发或写入长期权限状态。

### 6.3 普通 Peer Frame

- 必须有 Peer Hop Context，禁止 Group Context；
- Data 必须有 Message Context 且无 Protocol Context；
- Control/Transfer/Diagnostic 必须有 Protocol Context；
- 是否强制 E2E 由 V6-07 的 Opcode/Endpoint ACL 决定，Codec 只冻结结构。

## 7. Canonical E2E AAD

AAD 固定 80 B。缺失的 E2E、Protocol、Message、Route/Path 或 Budget 语义按规范零值写入，
因此同一语义只有一种 AAD：

| Offset | 长度 | 字段 |
| ---: | ---: | --- |
| 0 | 1 | Protocol Version=6 |
| 1 | 1 | Address Class |
| 2 | 1 | Flags |
| 3 | 1 | Header Contract |
| 4 | 1 | Frame Type |
| 5 | 2 | Protocol Opcode；缺失为 0 |
| 7 | 1 | Traffic Class |
| 8 | 1 | Delivery Guarantee |
| 9 | 1 | Interaction Role；缺失为 One-way/0 |
| 10 | 1 | E2E Mode；缺失为 None/0 |
| 11 | 1 | E2E Suite；缺失为 0 |
| 12 | 2 | E2E Key ID；缺失为 0 |
| 14 | 4 | E2E Key Generation；缺失为 0 |
| 18 | 4 | Realm |
| 22 | 4 | canonical Source Address |
| 26 | 4 | Source Binding Generation |
| 30 | 4 | canonical Destination Address |
| 34 | 4 | Destination Binding Generation |
| 38 | 4 | Session Generation |
| 42 | 4 | Packet Sequence |
| 46 | 2 | Source Endpoint；缺失为 0 |
| 48 | 2 | Destination Endpoint；缺失为 0 |
| 50 | 8 | Operation ID；缺失为 0 |
| 58 | 1 | Context Kind：0 None、1 Route、2 Path |
| 59 | 4 | Route Generation；非 Route 为 0 |
| 63 | 2 | Path ID；非 Path 为 0 |
| 65 | 4 | Path Generation；非 Path 为 0 |
| 69 | 1 | Initial Budget Presence：0/1 |
| 70 | 8 | Initial Budget；缺失为 0 |
| 78 | 2 | Payload Length |

Hop Limit、Peer/Group selector、remaining budget 和 Link Tag 不进入 E2E AAD，但由逐跳 Tag
保护。测试固定验证修改这些逐跳字段不改变 AAD；修改 initial budget 必须改变 AAD。

## 8. 四档 Golden

四条向量均为 Bootstrap Opcode `0x0102`，Realm `0x11223344`，Payload `DE AD BE EF`，
Destination 为本档 Link-local 全 1：

```text
A0/42B 55430601080001011122334400FF0000000000000000000000000000000000040102DEADBEEF93C9D2DB
A1/44B 5543460108000101112233440000FFFF0000000000000000000000000000000000040102DEADBEEF78D68D5C
A2/46B 554386010800010111223344000000FFFFFF0000000000000000000000000000000000040102DEADBEEFAB419569
A3/48B 5543C601080001011122334400000000FFFFFFFF0000000000000000000000000000000000040102DEADBEEF039415BE
```

Golden 是独立字节常量，测试分别比较完整输出，不用 parser/builder 往返自行证明自己。

## 9. 失败关闭和当前门禁

- Encoder 在参数、容量或语义失败时不修改 output 与 output length；
- Decoder 使用局部对象，只有完整长度、CRC、字段与 canonical 合同全部通过后才写回；
- 39/41 B、截断、追加、旧 Version、坏 Magic、坏 CRC、保留 Traffic bit、双 Context、零 Key、
  Route/Path 双存、Operation 规则、Budget 反向和保留地址误用均拒绝；
- fixed-seed 4096 次随机输入验证失败不写回，若随机输入恰好合法则重新编码必须逐字节相同；
- `UCN_BUILD_V6_EXPERIMENTAL=OFF` 时默认 `ucn_core` 不含任何 `ucn_v6_*` 符号；
- V6-07 外审 GO 之前，生产 Node/Adapter/Service 不得调用本 Codec。
