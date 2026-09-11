# V6S-00-02 基础标量、Generation 与 Registry 命名空间

> 状态：`DONE / SELF-REVIEW PASS / FINAL EXTERNAL REVIEW PENDING`
>
> 本文先冻结线上可见标量的宽度和所有权。`V6S-00-03` 只能把这些标量放入具体 offset，
> 不能重新选择第二套宽度或重置规则。

## 1. 编码基础

| 项目 | 冻结规则 |
| --- | --- |
| Octet | 固定 8 bit；文档中的 B 均指 octet |
| 字节序 | 所有多字节无符号整数为 big-endian |
| 有符号整数 | Core Wire 禁止；业务 Payload 自己定义 |
| 结构体直拷 | 禁止；必须逐字段 encode/decode |
| 隐式 padding | 禁止；长度由 Contract 精确计算 |
| Version | 4 bit；当前唯一合法值 6 |
| Header Contract | 4 bit；C0～C5 为 0～5，6～15 保留并拒绝 |
| 时间单位 | 未带后缀不得上 Wire；`*_us` 与 `*_ms` 必须显式命名 |

Decoder 不根据长度猜 Version/Contract，不尝试旧协议，不在认证失败后换另一种 Suite/Profile
重试。任何字段非法时，在分配 Sequence、写 Replay/统计、调用 Provider 或改写输出前失败。

## 2. 基础标量注册表

| 类型 | Wire 宽度 | 合法值 | 唯一分配者/解释者 | 耗尽或非法 |
| --- | ---: | --- | --- | --- |
| `ProtocolVersion` | 4 bit | `6` | 协议 Registry | 其他值拒绝 |
| `HeaderContract` | 4 bit | `C0=0`～`C5=5` | Wire Registry | `6..15` 拒绝 |
| `TrafficClass` | 2 bit | Q0～Q3 | Endpoint Policy/Resolver | 无保留值 |
| `DeliveryGuarantee` | 2 bit | BestEffort/Latest/Reliable；3 保留 | Endpoint Policy/Resolver | 保留值拒绝 |
| `InteractionRole` | 2 bit | OneWay/Request/Result/Error | Operation/Message Owner | 与 Payload Kind 冲突时拒绝 |
| `PayloadKind` | 2 bit | Data/Control/Transfer/Diagnostic | Protocol Registry | 与 Contract 冲突时拒绝 |
| `OriginSecurity` | 2 bit | O0/O1/O2；3 保留 | Security Policy/Context | 保留值拒绝；REQUIRED 不得降级 |
| `HopLimit` | 6 bit | `1..63` | Origin 初始化；中继只递减 | 0 拒绝；不得回绕 |
| `RealmId` | 32 bit | `1..0xFFFFFFFE` | Realm Manifest/Authority | 0、全 1 拒绝；不可运行期改写 |
| `NodeAddress` | 8/16/24/32 bit | 0 和该宽度全 1 保留 | Realm Address Authority/Manifest | 普通地址不得使用保留值 |
| `ServiceId` | 16 bit | `1..0xFFFE` | Service Registry | 0、0xFFFF 拒绝 |
| `ProtocolOpcode` | 16 bit | 第 5 章分区内非零值 | 对应模块 Registry | 未登记/跨域解释拒绝 |
| `C0TransactionId` | 64 bit | `1..UINT64_MAX` | 对应 Control FSM Owner | 不回绕；到顶 Fault/换父域 |
| `OperationId` | 64 bit | `1..UINT64_MAX` | Operation Allocator | 不回绕；重试保持相同值 |
| `OriginSequence` | 32 bit | `1..UINT32_MAX` | Core/Flow/Group Sender Owner | 到顶轮换父代际或 Fault |
| `HopSequence` | 32 bit | `1..UINT32_MAX` | Peer/Group Hop Security Owner | 到顶轮换 Hop Session |
| `TransferId` | 32 bit | `1..UINT32_MAX` | Transport Parent/Flow Owner | 到顶轮换父代际 |
| `CandidateId` | 32 bit | `1..UINT32_MAX` | Route/Flow Candidate Owner | 不在活动/迟到窗口内复用 |
| `PathId` | 32 bit | `1..UINT32_MAX` | Path Installer | 到顶换父域或 Fault |
| `ContextId` | 16 bit | `1..0xFFFE` | 对应 Flow/Group Context Owner | 0、0xFFFF 拒绝；退休前不复用 |
| `ForwardingLabel` | 16 bit | `1..0xFFFE` | 当前入站 Link 的 Label Owner | 0、0xFFFF 拒绝；Link 代际内不复用 |
| `SenderSlot` | 16 bit | `1..0xFFFE` | Group Policy Owner | 必须精确映射 Principal/Binding |
| `GroupId` | 32 bit | `1..0xFFFFFFFE` | Manifest 固定槽或动态高水位 | 删除不回退；0/全 1 拒绝 |
| `KeyId` | 16 bit | `1..0xFFFE` | Security/Group Policy Manifest | 不作每帧协商；未知拒绝 |
| `SuiteId` | 8 bit | Registry 登记的非零值 | Security Registry | 0/未知拒绝，不试探 |

`C0TransactionId` 统一为 64 bit，因为 C0 是低频控制路径，增加 4 B 不会污染 C1～C5 稳态
数据，却能让 Bootstrap、Admission、Setup、Recovery 共用一个无歧义事务标量。未绑定 Bootstrap
还必须在 Opcode Payload 中携带独立的双方新鲜 nonce；Transaction ID 不能替代密码 nonce。

## 3. checked 单调规则

所有 32-bit Generation/Sequence 和 64-bit Transaction/Operation ID 使用不回绕整数，不使用
RFC1982 环形比较：

```text
allocate_first(state):
    require state == NEVER_ALLOCATED
    reserve value = 1

checked_next(current):
    require 1 <= current < TYPE_MAX
    return current + 1

compare(a, b, parent_a, parent_b):
    require parent_a == parent_b
    compare only inside that exact parent domain
```

`0` 永远是 invalid/unset；只有持久化或只读 Manifest 明确证明 `NEVER_ALLOCATED` 时，第一次
分配才能从 0 状态产生线上值 1。看到存储为 0 不能自行推断“从未使用”。

跨 Realm、Binding、Session、Route、Path、Group、Cluster 或 Time Domain 的数值不得比较大小；
它们只能先比较完整父域，再解释子代际。表中 `TYPE_MAX` 是可编码、可接收的合法终值，
不得被 raw decoder 当作越界；只有分配器对该终值再次执行 `checked_next()` 时才返回
`EXHAUSTED/FAULT`，且高水位、输出和持久化状态完全不写回。最大值之后不得回到 1。

## 4. Generation 所有权表

| Generation | 宽度 | 唯一 Owner | 精确父域 | 持久化/发布规则 | 合法重新从 1 开始 |
| --- | ---: | --- | --- | --- | --- |
| Boot Incarnation | 32 | Platform/Runtime bootstrap Owner | Device Principal + Manifest Generation | 需要跨重启 anti-ABA 的产品必须 witness before use | 仅新 Device/Manifest 安全域 |
| Link Instance | 32 | Link Owner | Boot Incarnation + Link Slot + Peer Binding | 本地代际；reopen 前 checked-next/Fence | 新 Boot Incarnation；同 boot 不重置 |
| Address Authority | 32 | Realm Authority Quorum/Fence Owner | Realm | Epoch/Fence/allocation high-water 原子持久化 | 新 Realm；换主只 checked-next |
| Address Binding | 32 | Realm Address Authority | Realm + Address | Certificate/high-water persist-before-use | 新 Realm/Address 父域；同地址永不重复 |
| Peer Session | 32 | Security Owner | Realm + 双方 Principal/Binding | 高水位 persist-before-use/reload | 任一 Binding 改变，或同域 checked-next |
| Capability | 32 | Capability Owner | Principal + Session Generation | 已发布值同 Session 不回退 | 新 Session，或 checked-next |
| Route | 32 | Traffic Origin Route Owner | Origin Binding + Session + Destination Binding | ACTIVE/已发布高水位不得回退 | 新父域，或 checked-next |
| Path | 32 | Path Installer | Origin Binding + Session + Path ID | 已安装值不得回退 | 新父域/Path ID，或 checked-next |
| Flow | 32 | Flow Owner | 双方 Binding + Session + canonical Flow Identity | Stage/Commit 后不可就地改写 | 新父域/Flow ID，或 checked-next |
| C1 Transport Parent | 32 | Transport Parent Owner | 双方 Binding + Transport Policy | Generation/Transfer high-water persist-before-use | C0 三阶段建立 checked-next；单个 Transfer 结束不重置 |
| Group Policy | 32 | Manifest 或唯一动态 Group Policy Owner | Realm + Group ID | 固定槽或持久高水位；删除不回退 | 新 Group ID 父域，或 checked-next |
| Group Key | 32 | Group Key Rotation Owner | Realm + Group + Policy Generation + Key ID | 固定 Key 槽 persist-before-use | 新 Policy/未激活 Key 槽，或 checked-next |
| Group Tree | 32 | Group Tree Owner | Realm + Group + Policy Generation + Tree ID | ACTIVE Tree 不就地改写 | 新 Policy/Tree ID，或 checked-next |
| Cluster Config | 32 | Cluster Authority | Cluster Identity + Epoch | persist-before-promise/reload | 新 Cluster Identity/Epoch，或 checked-next |
| Cluster Epoch/Term | 32 | Cluster Authority/Quorum | Cluster Identity | Vote/Config/Epoch proof 持久化 | 新 Cluster Identity；同簇 checked-next |
| Network Time Domain | 32 | Time Authority | Time Domain Identity | 独立 witness + persist-before-publish | 新 Time Domain Identity，或 checked-next |
| Persistence Record | 64 | Persistence Owner | Persist Domain + Record Schema | 双槽 commit/witness 高水位不回退 | 新 anti-rollback Persist Domain；否则不重置 |
| Manifest/Layout | 32 | Product build/secure update authority | Product/ABI domain | 由签名 Manifest 或构建常量固定 | 仅新签名产品代际 |

同一个字段名如果落在不同父域，必须使用不同的 C typedef 或带 kind 的 typed wrapper；不得把裸
`uint32_t generation` 在模块间传递后依赖注释猜语义。

## 5. Registry 命名空间

`ProtocolOpcode` 的高字节固定模块域，低字节由 `V6S-00-03` 登记具体操作：

| 范围 | Owner | 用途 |
| --- | --- | --- |
| `0x0001..0x00FF` | Identity/Security | Bootstrap、Reauth、Session、Address Binding |
| `0x0100..0x01FF` | Capability/Neighbor | Capability、邻接声明与诊断 |
| `0x0200..0x02FF` | Route Owner | RREQ/RREP/RERR、Route Probe |
| `0x0300..0x03FF` | Flow/Path Owner | Stage/Commit/Abort/terminal receipt |
| `0x0400..0x04FF` | Transport Owner | Delivery ACK、Transfer Setup/SACK/Abort/receipt |
| `0x0500..0x05FF` | Service/Operation Owner | Operation/Result/Error/Durable Operation 控制 |
| `0x0600..0x06FF` | Time Owner | Time Sync 与 Realtime 控制 |
| `0x0700..0x07FF` | Group Owner | Group/Tree/Member/Key 控制 |
| `0x0800..0x08FF` | Cluster Owner | Config/Vote/Takeover/Handover/Recovery |
| `0x0900..0x09FF` | Diagnostics Owner | 明确无 Authority 的诊断控制 |
| `0x0A00..0x7FFF` | Standards registry | 当前保留，收到即拒绝 |
| `0x8000..0xBFFF` | Product-private | 需由签名产品 Manifest 登记且不能冒充标准 Opcode |
| `0xC000..0xEFFF` | Experimental | 发布构建默认拒绝 |
| `0xF000..0xFFFF` | Future protocol | 永久保留给后续版本；当前拒绝 |

`ServiceId` 与 `ProtocolOpcode` 是不同命名空间。Control/Transfer/Diagnostic 的 Opcode 不得藏在
Service ID、Endpoint ID、Frame 长度或实现状态中。ACL 的 canonical key 必须同时包含 Payload
Kind 和精确 Protocol Opcode；只授权整个 Control 大类不等于授权全部操作。

## 6. 时间与长度标量

| 类型 | 宽度 | 规则 |
| --- | ---: | --- |
| 本地单调 `now_us/deadline_us` | 64 bit，本地不直接上 Wire | 半开区间 `now_us < deadline_us`；加减乘 checked |
| Wire duration ms | 32 bit | 仅在具体 Opcode 定义；接收时 checked 转为 us |
| Payload length | 不在 Core Header | 必须由 Carrier 给出可信精确边界 |
| Frame/MTU budget | 至少 32 bit 本地计算 | `carrier - contract - envelopes - tags - trailer` checked-sub |
| Fragment index/count | 15 bit 有效值 | bit15 保留给 SACK kind；Count `1..32767` |

未知 Timer resolution、时钟误差、Carrier 长度或算术溢出都失败关闭，不能用 0 表示“无限”或
“默认允许”。

## 7. V6S-00-02 自审

| 检查 | 结果 |
| --- | --- |
| 基础标量均有宽度、合法域、Owner 和耗尽行为 | PASS |
| Bootstrap 与普通 C0 共用 64-bit Transaction，稳态帧不增加成本 | PASS |
| 所有 Generation 均有父域、持久化和合法重置规则 | PASS |
| 0/最大值/no-wrap/跨域比较规则明确 | PASS |
| Opcode、Service、Context/Label 命名空间没有混用 | PASS |
| `V6S-00-03` 只能分配具体值和 offset，不能重新选宽度 | PASS |

结论：`V6S-00-02 = DONE / SELF-REVIEW PASS`。精确 Opcode 数值和 Wire offset 仍由
`V6S-00-03` 冻结；本文不授权 Codec 实现。
