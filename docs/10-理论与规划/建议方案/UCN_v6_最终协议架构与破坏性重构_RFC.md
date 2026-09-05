# UCN v6 最终协议架构与破坏性重构 RFC

> 文档级别：`PROPOSED / PRE-IMPLEMENTATION RFC`
> 目标版本：`UCN v6 Draft`，稳定后作为 `UCN 1.0` 的候选基础
> 兼容策略：**不兼容现有 v4/v5 测试固件、Core Wire、Cluster Wire、公共对象 ABI 或持久化记录**
> 当前状态：V6A-01～V6A-25 对应整改均已外审通过；`V6-00 = DONE / EXTERNAL FINAL REVIEW GO`（仅最终架构 RFC/纯文档范围）。这不表示 v6 代码、Wire、安全、实机或掉电能力已经实现；`V6-01` 仍阻塞于 V5-64 A06 可追溯独立外审和用户对提交、v5 快照/Tag及 v6 基线的明确授权
> 日期：2026-09-04

## 1. 决策摘要

UCN 目前仍处于发布前测试阶段，没有对外提供旧 Wire、旧 API、旧 ABI、旧
Storage Layout 或旧固件互通承诺。因此 v6 的首要目标不是兼容已有实验实现，而是消除
过渡设计、重复语义和长期运行风险，建立一次可以接受正式发布审计的单一协议合同。

本 RFC 作出以下顶层决定：

1. v4/v5 通过 Git 分支、Tag 和测试报告保存，不进入 v6 运行时代码；
2. Core、Transfer、Realtime 和 Cluster 使用同一个协议主版本和安全身份模型；
3. W0～W3 改称 **Wire Address Class**，只表达线上地址宽度，不表达节点能力或权限；
4. Nano/Lite/Full 是编译期资源和功能 Profile，所有 Profile 都必须解析全部 Wire
   Address Class；
5. 只有地址和长度类字段允许随 Wire Class 缩短，Address Binding Generation、Session、
   Sequence、Route/Path Generation 等安全或所有权字段不得随地址宽度缩短；
6. 稳定 Device Identity、网络 Node Address、Address Binding Generation、Session
   Generation 与用户可读 Alias 相互分离；
7. 删除 `DATA_Q0/DATA_Q1` 的重复类型语义，Traffic Class、Delivery Guarantee、
   Interaction Role、Endpoint 和可选 Realtime Metadata 各自独立；
8. 未绑定地址的设备只使用不可转发的一跳 Bootstrap 事务；地址和 Session 建立后才发送
   普通 HELLO。HELLO 保持小型，完整能力通过带 Generation/Digest 的认证记录交换；
9. Link、Peer、Path 三层能力分别建模，Path Frame MTU、Payload/Fragment Budget 和功能
   能力必须由全路径瓶颈与精确 Frame Contract 计算；
10. 动态 Route、固定 Path、多 Bearer、手动指定和自动负载均衡统一为有界
    `RouteSet`，但每个 Flow 默认固定一条 Route，避免逐包乱序；
11. 生产安全是 v6 的组成部分，不再把真实 JOIN、逐跳控制认证、E2E AEAD、Replay、
    ACL 和 Key Rotation 留成发布后的可选补丁；
12. Transfer 采用固定内存的流水发送、选择确认和逐跳流控，避免 Stop-and-Wait 或
    Go-Back-N 在多跳/丢包下形成不必要的吞吐崩塌；
13. Realtime 和 Cluster 保持可选模块；普通业务不承担时间字段，Core 不依赖 Cluster；
14. 内部状态改为不可由调用者直接改写的私有对象，同时继续由调用者提供静态内存；
15. v6 只有一个配置事实源和一个发布事实源，不再长期保留头文件局部默认、兼容别名、
    双 Wire 与双状态机。

## 2. 为什么需要破坏性收敛

### 2.1 当前过渡状态

当前仓库已经证明了许多可保留的机制，但也同时存在以下发布前过渡状态：

- Core 使用 `UCN_PROTOCOL_VERSION=5` 和 W0～W3；
- Cluster Current Wire v3 为 32 B，实验 Wire v4 为 40 B，v4 Encoder 默认关闭；
- HELLO Payload 只有 1 B RX Ceiling；
- PATH_INSTALL 存在基础和 capability 两套精确长度；
- Transfer Peer Class、Window、Concurrency 主要由产品静态写入；
- Network、Source、Destination、Session 共用同一 `address_bytes`；
- Route Epoch 在 W0 只有 1 B，在其他 Class 也只有 2 B，并允许有限空间复用；
- Message Type 既表达协议控制类型，又承担 `DATA_Q0/DATA_Q1` 和静态 Endpoint；
- `ucn_node_t`、Cluster/Realtime 事务对象和部分内部字段仍通过公共结构暴露；
- `ucn_config.h` 与多个公共头文件各自保留默认值，Translation Unit 配置一致性依赖
  集成者纪律；
- 生产安全接口位置已经存在，但真实身份、逐跳认证、生产 AEAD 与密钥生命周期尚未
  成为默认发布闭环。

如果继续在这些过渡结构上增加兼容分支，后续每个字段都会同时承担“保持旧行为”和
“表达最终语义”两类目标，最终代码体积、测试矩阵和安全证明成本都会持续增加。

### 2.2 本轮允许失去什么

v6 明确允许：

- v4/v5 Frame 无法被 v6 解码；
- v4/v5 固件无法加入 v6 网络；
- 旧公共结构初始化无法编译；
- 旧持久化记录无法加载；
- 旧 Golden Vector、Cluster Wire、Path 控制帧和 Endpoint 编号整体变化；
- ESP32 测试工程需要重新编译和重新烧录；
- 旧节点配置需要通过离线工具转换或直接重建。

不允许以“测试方便”为由继续保留运行时降级解析。需要对照旧行为时，应切换 Git
分支、Tag 或独立固件，而不是让生产 v6 代码同时维护旧协议。

## 3. 不变的架构原则

破坏性升级不等于推翻所有设计。以下原则继续冻结：

- MCU-first；没有 Linux、网关或云端时网络仍可成立；
- C99、固定内存、无隐藏堆分配、无无限表项和无限重试；
- 单一 Protocol Owner 持有协议状态；ISR/Driver Callback 只搬运数据、登记事件和通知；
- Linux/ROS 2/MAVLink 是 Host Adapter，不拥有隐式路由或准入特权；
- HELLO 和 Heartbeat 都是一跳控制，不转发；
- 路由发现有 Hop、Token、Deadline、重复表和固定容量上界；
- 中继默认只处理外层路由，不解密端到端业务；
- Traffic Class 不等于可靠性；
- Realtime、Cluster、诊断、大消息是可裁剪扩展；
- 软件、模拟、目标构建、实机、掉电和安全审计是不同证据等级。

## 4. 最终分层

```text
Application / Control / Sensor Tasks
        │
        ▼
Service API
  Endpoint + Traffic + Delivery Guarantee + Interaction Role + Optional Time Policy
        │
        ├──────── Local Fast Path ───────► Local Inbox
        │
        ▼
Message / Transfer
  Best-effort / Latest / Reliable + One-way / Request / Result + Fragment
        │
        ▼
RouteSet / Path Contract / Capability
  direct / active / standby / candidate / pinned / weighted
        │
        ▼
Core Wire v6 + Security
  address class / session / sequence / route or path context
        │
        ▼
Link + Carrier + Event Runtime
  UART / RS-485 / CAN / CAN-FD / USB / Wi-Fi / Ethernet / ...
```

Cluster 通过同一 Service/Core Wire/Security 平面运行，但它不成为普通 Core 通信的
依赖。Realtime 为选择它的 Endpoint 添加 Payload Envelope 和 Timed Link 能力，普通
Endpoint 不增加时间字节。

## 5. 身份、地址和代际模型

### 5.1 Identity、Address 与事务对象必须分离

| 对象 | 建议宽度 | 生命周期 | 是否逐帧携带 | 用途 |
| --- | ---: | --- | --- | --- |
| Device Identity | 128 bit 或公钥指纹 | 出厂至撤销 | 否 | 认证、ACL、设备替换和审计 |
| Network Realm ID | 32 bit | 网络创建至重建 | 是 | 隔离不同 UCN 网络 |
| Node Address | 8/16/24/32 bit | 入网租约 | 是 | 日常路由和转发 |
| Address Binding Generation | 固定 32 bit | 单次 Realm/Address 绑定 | 是 | 防止地址复用 ABA，绑定 Source/Destination |
| Node Alias | 产品定义 | 可修改 | 否 | 人员管理和诊断显示 |
| Session Generation | 固定 32 bit | 启动/轮换 | 是 | Replay、Nonce 与权限代际 |
| Origin Sequence | 固定 32 bit | 单 E2E/Group 重放域 | 是 | 跨中继保持不变，绑定端到端或 Group 消息身份 |
| Hop Sequence | 固定 32 bit | 单下一跳 Peer Session | 是 | 每跳重新分配，用于 Hop Replay 和重签 |
| Bootstrap Transaction ID | 固定 64 bit | 单次一跳入网事务 | Bootstrap only | 区分未分配地址的并发设备 |

Device Identity 不允许直接拿 MAC、串口号或 CAN ID 代替。MAC 可以参与出厂导入或
诊断，但不能成为唯一安全身份。Node Address 可手动配置、持久租约分配或由受认证
Coordinator 分配；在地址启用前必须完成冲突检测和 Address Binding Generation
持久化。任何普通业务、Route、Path、ACL 或 Cluster Member 身份都不能只用裸地址作
所有权键。

### 5.2 地址分配

v6 应同时支持：

1. **STATIC**：产品写死地址，JOIN 仍验证 Device Identity 与地址授权；
2. **LEASED**：Coordinator/Cluster Authority 发放带期限的地址；
3. **SELF_PROPOSED**：节点提出地址，经冲突检测和授权后生效。

地址冲突不得通过“后启动者覆盖先启动者”解决。冲突节点必须保持未准入或重新申请，
并在诊断中同时报告 Device Identity 摘要和冲突地址。

### 5.3 未绑定地址时的 Bootstrap

未进入 `ADDRESS_BOUND` 的节点没有普通 Source Address，因此它不能发送普通 HELLO。v6
单独定义 Link-local Bootstrap Frame：

```text
source_address             = UNBOUND（全零，仅 Bootstrap 合法）
destination_address        = LINK_LOCAL_ALL 或本跳临时目标
source/destination_binding = 0，仅 Bootstrap 合法
hop_limit                  = 1
route/path/endpoint        = absent
forwarding                 = forbidden
identity_digest            = 128 bit
bootstrap_transaction_id   = fresh nonzero 64 bit
```

Bootstrap 只有两个合法角色：`JOINING_DEVICE` 与当前 `REALM_ADDRESS_AUTHORITY`。其固定
消息顺序为：

```text
JOINING_DEVICE                         REALM_ADDRESS_AUTHORITY
BOOTSTRAP_HELLO(device_nonce, txid)  →
                                    ← BOOTSTRAP_COOKIE_CHALLENGE(cookie)
BOOTSTRAP_HELLO_COOKIE(device_nonce, txid, cookie,
                       lease_freshness_challenge_nonce) →
                                    ← IDENTITY_CHALLENGE(authority_nonce,
                                        authority_identity/generation, realm,
                                        offered_suites, txid)
IDENTITY_RESPONSE(device proof over transcript) →
                                    ← ADDRESS_OFFER(realm, address,
                                        binding_generation, binding_lease_id/duration,
                                        suite/key context, lease freshness proof,
                                        transcript proof)
ADDRESS_COMMIT(device acceptance proof)         →
                                    ← ADDRESS_COMMIT(authority final proof)
```

设备必须在出厂/受控 commissioning 时拥有 Realm Trust Anchor 和自身 Enrollment
Credential；Authority 必须提供由该 Trust Anchor 或当前 Realm quorum 认可的
`AuthorityLeaseCertificate`。设备使用自身私钥/Enrollment PSK 证明 Device Principal，
Authority 使用证书对应 Key 证明当前 Authority Principal。没有任一可信根、凭据被撤销、
证书链未知或时效无法证明时，只能报告未认证发现诊断，不能获得地址或进入普通网络。

Bootstrap Opcode 仅允许 `BOOTSTRAP_HELLO`、`BOOTSTRAP_COOKIE_CHALLENGE`、
`BOOTSTRAP_HELLO_COOKIE`、`IDENTITY_CHALLENGE`、`IDENTITY_RESPONSE`、`ADDRESS_OFFER`、
双向 `ADDRESS_COMMIT` 和 `BOOTSTRAP_ABORT`。除本节 Bootstrap 的零 Source/Binding，以及
8.1 `CONTROL/GROUP_HELLO` 唯一允许的零 Destination Binding 外，其他 Frame 使用零地址或
零 Binding Generation 必须严格拒绝。双方都必须验证对端身份；
仅验证 JOINING_DEVICE 而不验证 Address Authority，或者只验证 Authority 而不验证设备，
都不得进入 `ADDRESS_OFFERED`。

每个新事务必须产生彼此独立的非零 `device_nonce`、`authority_nonce` 和 `txid`。以下字段
按固定顺序进入双方签名/MAC、密钥派生和最终 Commit proof 的 canonical transcript：

```text
protocol_version + bootstrap_header_contract + ordered_message_roles
joining_device_full_principal + joining_device_identity_digest
address_authority_full_principal + address_authority_generation
device_nonce + authority_nonce + bootstrap_transaction_id
lease_freshness_challenge_nonce
realm + proposed_address + address_binding_generation
binding_lease_id + binding_lease_duration_us + authority_lease_sequence
selected_hop_suite/key_context + selected_e2e_suite/key_context
transcript_hash_of_all_prior_messages
```

`ADDRESS_OFFER`、设备 Commit 和 Authority Final Commit 必须分别绑定同一个 transcript；
任何字段、顺序、角色、nonce、txid、Suite、Key Context 或前序 transcript hash 不同均视为
不同事务并拒绝。设备只有在验证 Authority proof 后才能持久化候选 Binding；Authority 只有
在验证设备 acceptance proof、完成冲突复核并持久化租约/高水位后才能发送 Final Commit。
设备收到 Final Commit 后执行 reload/proof，再把 Binding 标为可用。异步持久化期间不得发送
普通 Frame，也不得让重复消息创建第二个事务。

接收端用
`{ingress_link_instance, local_peer_discriminator, identity_digest, bootstrap_transaction_id}`
区分并发未绑定节点；Digest 相同不能直接视为同一设备，必须在 Challenge Transcript 中
验证完整 Device Identity/Public Key。Bootstrap 记录不得写入 admitted Neighbor、Route、
Path、Endpoint ACL 或 Cluster 表。`ADDRESS_COMMIT` 持久化成功并完成冲突复核后，节点才
获得 `{Realm, Address, Address Binding Generation}`，随后建立 Session 并发送普通 HELLO。

Bootstrap 身份只在本跳事务中有效，不是可路由地址、临时 ACL Principal 或降级的匿名
Session。随机数源不可用、Transaction ID 复用、表满、Challenge 冲突或持久化失败时必须
保持未绑定并失败关闭。

Bootstrap 和尚未建立新 Peer Session 的 Reauth 必须在分配 pending 槽、执行公钥验证、
访问持久化 Provider 或产生大于请求的响应之前先通过无状态 Cookie 与认证前预算。Cookie
固定绑定：

```text
protocol_version + bootstrap_or_reauth_role
ingress_link_instance_generation + local_peer_discriminator
identity_digest + device_nonce + transaction_id
cookie_time_bucket
```

Cookie 使用 boot-scoped secret 的 MAC；secret 每次可信启动生成，禁止持久复用。实现只接受
当前和前一个 time bucket，默认 bucket 为 2 s，因而任何 Cookie 最长不超过 4 s；bucket
serial 到达 no-wrap 阈值时必须轮换 secret 并清空所有认证前状态。首次
`BOOTSTRAP_HELLO/PEER_REAUTH_HELLO` 的响应不得大于请求；若 Cookie Challenge 固定帧更大，
请求必须使用规范零 padding 达到相同长度。Cookie 只证明回程可达和请求新鲜度，不证明
Device Identity，也不创建 Session、Binding 或 ACL Principal。

通过 Cookie 后才能占用固定 pending。默认资源合同为：全局 8 槽、每个 ingress Link 2 槽、
每个 `{link,local_peer_discriminator,identity_digest}` 1 槽、绝对 pending timeout 3 s；
每 Link 的认证前 Token Bucket 默认 burst 4、每秒恢复 2 个请求。产品可以在编译期减小或
增大这些值，但必须使用有限非零值、写入 Product Manifest，并通过对象大小/CPU budget
门禁。满载时不驱逐已有事务；`BUSY` 也必须先消耗该 Link 的响应预算。无效、冲突、重复或
超限输入不得刷新 Cookie bucket、pending deadline 或 Token Bucket，精确合法重传只能读取
原事务结果，不能重新开始 3 s。Owner 每次推进的 Cookie 校验、签名验证和 Provider 操作数
必须有固定上限，避免合法 Cookie 洪泛把 MCU 长期锁在昂贵密码运算中。

Pending Deadline 从首次验证 Cookie 成功、准备占槽之前锁存的本地单调时刻开始，用公共
checked deadline primitive 建立；结果为 0、溢出或 Unknown 时不占槽，`now==deadline` 即
过期。过期清理必须由 Owner/timer 独立执行；不匹配或恶意输入不得借 lazy expiry 清除另一个
合法事务，也不得把失效槽自动替换为当前攻击请求。

认证前接收顺序固定为：`最小长度/版本/Opcode/零尾检查 → per-Link Token Bucket → Cookie
生成或 MAC 校验 → per-discriminator/global 槽检查 → pending 写入 → Identity/证书密码验证
→ Provider`。前一步失败时后续步骤的计数必须为零；尤其不能先验公钥再判断 Cookie，也不能
先占槽后判断 per-Link 配额。Cookie MAC 属于有预算的廉价对称操作，Identity signature、
证书链和 threshold proof 属于昂贵操作，分别具有独立的每次 Owner run 上限。

已处于 `ADDRESS_BOUND`、但因移动/reopen 在新 Link 上尚无 Peer Session 的设备，不回退到
UNBOUND Bootstrap，也不能伪造普通 HELLO。它使用同样不可转发、`hop_limit=1` 的
`PEER_REAUTH_HELLO → PEER_REAUTH_COOKIE_CHALLENGE → PEER_REAUTH_HELLO_COOKIE →
PEER_REAUTH_CHALLENGE → PEER_REAUTH_RESPONSE → PEER_REAUTH_ACCEPT` 流程；Frame 携带
现有非零 Realm/Address/Binding，双方证明当前 Device
Principal、Binding Certificate、全新 nonce 和新 Link Instance Generation。该流程只能建立
新逐跳 Peer Session，不能修改 Realm、Address、Binding Generation、租约或 Endpoint ACL。
旧 Link 的 token、Replay Window 和 pending 必须由旧 Link owner 独立失效；迟到事件不能
进入新 Link Session。Reauth 使用与 Bootstrap 相同的无状态 Cookie、per-Link 预算、固定槽和
绝对 timeout，但两者使用不同 role/KDF label 和独立 pending 表，不能跨流程重放 Cookie。

普通 HELLO 默认是已认证 Peer Session 上的逐 Peer 单播。共享介质可选组播 HELLO 时，必须
使用 6.6 定义的显式 Group Security Context、Group Key Generation、Link-local Group
Destination 和独立 Replay Window；组播只提供低成本发现/存活提示，不能
创建 Peer Session、续期单播身份验证、改变 Binding/Capability/ACL 或进入 `ADMITTED`。
收到未知或 Generation 改变的组播摘要后，仍必须完成上述逐 Peer 重认证后才能建立邻接。

### 5.4 Realm Address Authority、租约与分区

每个 Realm 只有一个**逻辑 Address Authority**。它可以由一个静态安全协调器实现，也可以
由具备 quorum、持久化和 Fence 的 Cluster 实现，但线上只能有一个持有有效写租约的
Authority Epoch：

```text
AddressAuthorityEpoch = {
  authority_principal,          // 128-bit principal / public-key identity
  authority_generation,         // nonzero checked 32-bit serial
  durable_fence_token,          // fresh nonzero 128-bit lease/fence identity
  lease_sequence,               // nonzero checked 64-bit serial in this realm
  lease_duration_us,            // 本 Epoch 可签发的最大时长；不是共同绝对起点/截止时间
  allocation_high_water_digest  // canonical digest of durable allocation state
}
```

`AuthorityLeaseCertificate` 固定绑定完整 `AddressAuthorityEpoch`、Realm、quorum/config
digest、签发者集合摘要和 threshold proof。静态单协调器模式等价于固定 voter set 为 1，
仍必须持久化 Fence/Generation/high-water；Cluster 模式由 V6-12 提供 quorum proof，但
证书验证接口属于 V6-07 安全基座。两种模式使用同一证书语义，不允许“Cluster 不可用时
自动降级成无 quorum 自签名”。证书声明 Authority/Fence/quorum 身份；真正允许本节点在
有限时间内使用它的，还必须是下述每个验证者单独取得的 `AuthorityLeaseFreshnessProof`。

固定 voter set 为 1 的产品不具备自动安全换主能力：原设备故障后只能由保留旧
high-water/Fence 的安全存储恢复同一 Authority，或使用离线 Realm Trust Anchor 签发
`AuthorityTransferCertificate`，明确撤销旧 Fence、指定新 Principal 和 checked-next
Generation。拿不到旧高水位且没有离线 Transfer Certificate 时，Realm 必须进入
`ADDRESS_ALLOCATION_FAULT`；不得让替换设备从 Generation 1 重新开始。

产生、续期或撤销 Address Binding 前，Authority 必须同时证明：自身身份已认证、Generation
与持久化高水位一致、当前写租约未过期、Fence token 尚有效，以及配置要求的 quorum 仍成立。
缺少合法 Authority、失去 quorum、租约到期、Provider 无法 reload 或分区侧无法证明自己持有
最新 Fence 时，必须禁止创建和续期 Binding；不得以 `SELF_PROPOSED` 或本地时间更晚为由
继续分配。

`SELF_PROPOSED` 只产生候选地址，最终 Binding 仍由 Authority 签发。`STATIC` 地址也必须
由签名产品配置预留，或在 JOIN 时登记到当前 Authority；它不是绕过冲突表和 Generation
高水位的后门。离线静态 Realm 若明确不启用动态分配，可预置不可重叠的 Binding Certificate，
但不得在运行中新增地址。

Authority 换主必须先让旧 Authority Fence 不可逆失效，再由 quorum 持久化
`authority_generation=checked_next(old)`、继承完整 Binding high-water 和冲突表，最后才能
发布新 Lease。少数分区不得换主或分配。分区期间，已有且未过期的 durable Binding 可继续
用于受认证通信，但不能续期；到期后停止新 Session/Route/ACL 写入。分区合并时，任何无法
证明连续 Fence 链的分配都视为未授权，不允许通过比较本地 Generation 数值来选胜者。

Binding Generation 在同一 `{realm,address}` 域内由这个逻辑 Authority 全生命周期严格单调；
Authority 换主不重置它。Binding Certificate 必须包含并认证 Authority Principal/
Generation、Realm、Address、Binding Generation、非零 128-bit Lease ID、有限
Lease Duration、Authority Lease Sequence 和 Device Principal。证书不携带也不暗示一个
所有节点共同可验证的绝对起点/截止时间。没有可信 Realm Time 时，设备重启后不得根据保存的
uptime 推算剩余租期，必须先向当前 Authority 重新验证 Lease，才能建立新 Session。
因此只要 Authority lease/quorum/fence 与高水位合同成立，就不会出现两个合法设备获得相同
`{realm,address,binding_generation}`。若产品无法提供该保证，则不得启用 `LEASED` 或
`SELF_PROPOSED`，只能使用离线签名的静态 Binding。

#### 5.4.1 租约新鲜度与本地截止期

UCN v6 不要求所有节点在完成 JOIN 前共享 Domain Time，也不把不同 MCU 的 uptime 当作同一
时钟。每个需要使用 Authority Lease 或 Binding Lease 的验证者，必须先生成全新非零
`lease_freshness_challenge_nonce`，并在发出 Challenge **之前**锁存
`challenge_started_local_us`。Authority/quorum 返回的 `AuthorityLeaseFreshnessProof` 必须
认证以下字段：

```text
realm + complete AddressAuthorityEpoch
verifier_device_principal + lease_freshness_challenge_nonce
bootstrap_or_reauth_transaction_id
authority_lease_sequence + max_remaining_lease_us
binding_lease_id + binding_generation（验证 Binding 时）
proof_transcript_hash + threshold proof
```

`max_remaining_lease_us` 是 proof 生成时 Authority 能安全承诺的最大剩余时长，不是发送端
uptime、Unix 时间或共同 Deadline；它必须满足
`0 < max_remaining_lease_us <= AddressAuthorityEpoch.lease_duration_us`，验证 Binding 时还
不得超过 Binding Certificate 的签发时长，并固定取 Authority 写租约与该 Binding 租约的
保守剩余上界较小值。验证者用自己的单调时钟建立保守半开区间：

Proof 生成端也必须保守计算：每个 quorum signer 以自身已持久化 Lease Deadline 减去
`producer_now_upper`、本地 timer resolution/slow-clock margin 和签名排队上界，只能给出
不大于真实剩余时间的值；聚合器取所有有效 quorum response 的最小值。任一 signer 无法
给出上述上界、Fence/Lease Sequence 不同或聚合超时，均不得生成 freshness proof。这样
安全性不依赖某个协调者声称的“当前时间”，也不因多个 signer 的时钟偏差而放大租期。

```text
clock_margin_us = ceil(max_remaining_lease_us * local_timer_max_slow_ppm / 1_000_000)
resolution_margin_us = local_timer_resolution_us
read_margin_us = checked_mul(2, local_timer_read_uncertainty_us)
verifier_quantization_margin_us = checked_add(resolution_margin_us, read_margin_us)
safe_duration_us = checked_sub(max_remaining_lease_us, clock_margin_us)
safe_duration_us = checked_sub(safe_duration_us, verifier_quantization_margin_us)
effective_duration_us = min(safe_duration_us, local_policy_max_lease_us)
local_deadline_us = checked_add(challenge_started_local_us, effective_duration_us)
valid := now_local_us < local_deadline_us
```

`local_timer_resolution_us` 是验证端本地单调计时器一次量化可能漏计的完整 tick 上界，必须
已知且大于零；`local_timer_read_uncertainty_us` 是单次读取相对于该计时器真实值的绝对误差
上界，Challenge 起点和验证时刻各计一次，因此固定扣除两倍。只有 Timer Port 能证明读取
原子且无额外读数误差时，后者才允许为零；“未测量”不能编码为零。即使实现改为显式计算
`verifier_now_upper_us`，也必须与上述扣减式给出不晚于它的判定，不能二选一后遗漏慢钟、
量化或两次读取误差。

所有乘法、减法和加法必须 checked；任一输入 Unknown、`max_remaining_lease_us=0`、
`local_policy_max_lease_us=0`、本地慢钟上界未知、Timer Resolution 未知/为零、读取误差
未知、`safe_duration_us=0`、下溢、溢出或 `now==deadline` 均失败关闭。以 Challenge 发出
时刻而不是 Final Commit 到达时刻计时，保证网络排队、密码验证和持久化耗时只会减少可用
租期。Final Commit 到达时如果本地 Deadline 已过期，事务必须 Abort，不能先绑定再补续期。

区分性例子：验证端 Timer Resolution 为 `10,000 us`、读数误差为 `0`（由 Port 证明）、
慢钟上界为 `0 ppm`，proof 只剩 `15,000 us` 时，`safe_duration_us` 最多为 `5,000 us`。
即使真实已经过了 `19,000 us`、量化后的原始计时器读数仍只前进 `10,000 us`，也必须判定
过期，不能用原始 `10,000 < 15,000` 放行。非整 tick 租期、真实已过期但原始 now 尚未跳变、
刚好 Deadline、扣减至零和扣减下溢都必须成为独立负向向量。

相同 proof 的重传只能完成同一 pending，不得重写 `challenge_started_local_us` 或延长
Deadline；新的新鲜度验证必须使用新 nonce、新 transaction 和新的 quorum proof。同一
Authority Lease Sequence 内，后续 proof 的 `max_remaining_lease_us` 不得大于本节点已经
接受的上一值；只有 quorum 持久化真正的 Lease 续期、使用 checked-next Lease Sequence 后，
才允许剩余时长重新增大。Binding 续期还必须签发新的 Lease ID/Certificate。设备重启、
本地单调时钟复位、Authority/Fence/Generation 改变或 proof 状态
损坏时，本地 Deadline 一律失效并重新 Challenge。各验证者可以因为消息延迟不同而得到不同、
但都不晚于认证上界的本地截止期；协议不宣称它们拥有相同绝对截止时间。Authority 自身的
写租约也使用同一保守算法；Fence/quorum 决定排他安全，租约只限制缓存的新鲜度，不能用
“本地时间更晚”取代 Fence 或 quorum。

### 5.5 Generation 所有权、父域与重置

下列所有权代际和线上 Sequence 统一使用非零 32-bit checked serial。每个子代际
只有在父域已经不可逆变化后才允许从 1 重新开始：

| 对象 | Owner | 父域/唯一性范围 | 持久化要求 | 合法重置条件 |
| --- | --- | --- | --- | --- |
| Address Authority Generation | Realm Authority Quorum/Fence Owner | `{realm}`，更换 Principal 也连续 | Authority Epoch、Fence 与 allocation high-water 必须原子持久化/复制 | 仅完成旧 Fence 失效和合法换主后的 checked-next；不得因分区或更换 Principal 重置 |
| Address Binding Generation | 当前 Realm Address Authority | `{realm,address}`，跨 Authority 换主连续 | Authority anti-rollback high-water、Binding Certificate 必须持久化 | 新 Realm；同 Realm/Address 永不重复 |
| Session Generation | Device Principal | `{realm,address,binding_generation,principal}` | 启动/重键前 persist-before-use | 新 Address Binding；同 Binding 下不重复 |
| Origin Sequence | E2E/Group Sender | E2E 为 `{source principal,source session_generation,e2e selector}`；Group 为 `{group key selector,claimed source binding/session}` | 发送区间预留，高水位不得回退 | 仅新 E2E Session Generation 或 Group Key Generation |
| Hop Sequence | Peer Session Sender | `{next-hop principal,next-hop session_generation,hop selector}` | 发送区间预留，高水位不得回退 | 仅新下一跳 Session Generation |
| Link Instance Generation | Link Owner | `{local boot incarnation,link slot,peer binding}` | 若父 boot incarnation 持久化，可随新父域重置 | reopen 必须先推进父域或本代际 |
| Capability Generation | Capability Owner | `{principal,session_generation}` | 已发布值不得在同 Session 回退 | 仅新 Session 或 checked-next |
| Group Policy Generation（线上 `group_generation`） | Realm Manifest 选定的唯一 Group Policy Owner：签名静态配置或当前逻辑 Realm Address Authority/quorum，二者不可同时写 | `{realm,group_id}`；动态 `group_id` 只能等于 Realm 分配高水位的 checked-next，禁止稀疏指定/回收；静态 `group_id` 只能来自固定 Manifest 槽 | 动态模式持久化单个 `group_id_allocation_high_water`；静态模式持久化固定槽的 `NEVER_ACTIVATED/ACTIVE/RETIRED` 状态；Policy 摘要、Owner/Fence 与 Generation high-water 均 persist-before-publish | 仅新 Realm 或从未分配/激活的 Group ID 建立新父域；删除不降低分配高水位、不把静态槽恢复为未使用；Generation 到阈值时退休该 Group 并分配新 ID，容量/ID 耗尽则 Fault |
| Group Key Generation（线上 `group_key_generation`） | 当前 Group Policy 明确授权的 Group Key Rotation Owner | `{realm,group_id,group_generation,group_key_id}`；`group_key_id` 是签名 Group Policy Manifest 中固定 Key 槽的不可变 ID，不允许运行时稀疏分配；同一父域内跨 Key Owner/Address Authority 换主连续 | 每 Group 的固定 Key 槽保存 `NEVER_ACTIVATED/ACTIVE/PREVIOUS/RETIRED`、current/previous Generation high-water、撤销和激活状态，persist-before-use；摘要只校验完整性，不作为“是否用过”的成员判定 | 日常轮换保持 Key ID 不变并 checked-next Key Generation；仅新 `group_generation` 或从未激活的固定 Key 槽可从 1 开始；槽/Generation 耗尽时推进 Policy Generation 或 Fault，禁止驱逐、复用或回绕 |
| Route Generation | Traffic Origin | `{origin binding,origin session,destination binding}` | 已激活/已发布值按策略持久化或由新 Session 隔离 | 仅父 Binding/Session 改变或 checked-next |
| Path Generation | Path Installer | `{origin binding,session,path_id}` | 已安装值不得回退 | 仅新父域、Path ID 或 checked-next |
| Config/Cluster Generation | Cluster Authority | Cluster Identity/Epoch | persist-before-promise | Rekey/新 Cluster Identity 或 checked-next |
| Time Domain/Authority Generation | Time Authority | Time Domain Identity | 独立 witness + persist-before-publish | 新 Authority Identity 或 checked-next |

到达冻结阈值时必须轮换表中明确的父身份、Session、Realm、Path ID 或进入 Fault，禁止直接
回绕到 1。仅用于统计的计数器可以饱和；参与所有权、Replay、地址绑定或 Authority 的
计数器不能饱和后继续运行。

Group 的 Owner 模式由签名 Realm Manifest 固定。静态模式只能加载 Trust Anchor 签名、
代际不低于本地 high-water 的 Group Policy，不允许运行时自增或降级为动态自签；动态模式
只能由持有当前 Address Authority Lease/Fence 和所需 quorum 的逻辑 Authority 推进。
Authority 换主、设备重启、配置恢复或分区合并都不是重置条件。无法证明 high-water 连续时，
必须停止发布/接受新 Group Policy 和 Group Key；已有 Key 是否继续使用只按其已持久化租约、
撤销和本地安全策略判断，不能通过回退 Generation 延寿。

禁止实现无界“已用 Group/Key ID 历史集合”。V6 固定采用以下可判定、固定容量布局；具体宏值
由产品 Manifest 在编译期产生，运行时不得扩容：

```text
RealmGroupAllocator {
  dynamic_group_id_high_water:u32,
  active_groups[UCN_V6_MAX_ACTIVE_GROUPS]
}

StaticGroupSlot[UCN_V6_MAX_STATIC_GROUP_SLOTS] {
  immutable_group_id:u32,
  state: NEVER_ACTIVATED | ACTIVE | RETIRED,
  policy_generation_high_water:u32
}

GroupKeySlot[UCN_V6_MAX_GROUP_KEY_SLOTS_PER_GROUP] {
  immutable_group_key_id:u16,
  state: NEVER_ACTIVATED | ACTIVE | PREVIOUS | RETIRED,
  key_generation_high_water:u32
}
```

动态 Group 创建只能执行
`new_group_id = checked_next(dynamic_group_id_high_water)`，先持久化高水位预留，再发布任何
Policy/Key/HELLO。删除动态 Group 可以释放一个 `active_groups[]` 运行槽，但不得降低高水位；
下一次创建必须继续使用更大的 ID。禁止指定任意稀疏 ID、寻找历史空洞或垃圾回收后复用。
活动表已满时创建零写拒绝；高水位达到冻结阈值时进入 `GROUP_ALLOCATION_FAULT`，已有且仍合法
的 Group 可按策略继续运行，但不得创建新 Group。

静态模式的全部生命周期 Group ID 必须预先列入固定 Manifest 槽。删除后槽原子进入
`RETIRED` 并永久占位，重刷产品配置、重启或恢复旧 Manifest 都不能变回 `NEVER_ACTIVATED`；
所有槽用尽后拒绝创建。常规 Group Key 轮换保持 `group_key_id` 不变，只推进
`group_key_generation`。只有签名 Policy 中从未激活的固定 Key 槽才能引入另一个 Key ID；
退休槽永久占位，满载不驱逐。若必须获得新的 Key 槽集合，只能先以持久化、checked-next 的
`group_generation` 提交全新父策略，旧完整 selector 同时进入撤销/过期域；不能在同一父域
清空槽表。删除整个 Group 后，动态 ID 高水位或静态 `RETIRED` 槽仍保留，因此不需要保存
无界 Key 历史也不会复活旧 selector。任何摘要只用于校验固定记录完整性，不能代替高水位或
槽状态做“是否曾使用”的成员查询。

## 6. Core Wire v6 候选合同

本节冻结逻辑字段和安全边界。精确 Golden Byte Offset 由 `V6-03` 形成最终 Wire RFC；
该任务只能实现隔离、默认关闭的 Codec、Golden 和负向测试。生产 RX/TX/Encoder 必须等待
`V6-07` Security 与唯一 JOIN FSM 获得外部复审 GO，不能以 V6-03 完成为理由提前放行。

### 6.1 Wire Address Class

| Class | Node Address | 最大普通地址 | 建议场景 |
| --- | ---: | ---: | --- |
| A0 | 1 B | 254 | 极小局部网络、Classic CAN/UART |
| A1 | 2 B | 65,534 | 边缘设备网络 |
| A2 | 3 B | 16,777,214 | 大型 Mesh/分区网络 |
| A3 | 4 B | 4,294,967,294 | 骨干、网关和全宽地址 |

名称从 W0～W3 改为 A0～A3，是为了明确它只决定地址表达能力。所有 Profile 必须拥有
A0～A3 Decoder。发送方选择能够表达 Realm 内 Source/Destination 且符合 Path Frame MTU 的
最小 Class。

### 6.2 建议公共前缀

| Offset | 字段 | 长度 | 说明 |
| ---: | --- | ---: | --- |
| 0 | Magic 0 | 1 B | 固定 |
| 1 | Magic 1 | 1 B | 固定 |
| 2 | Version + Address Class | 1 B | 低 6 bit 版本，高 2 bit Class |
| 3 | Frame Type | 1 B | Bootstrap/Control/Data/Transfer/Diagnostic 大类 |
| 4 | Flags | 1 B | 扩展与安全存在位 |
| 5 | Traffic + Guarantee | 1 B | 2 bit Traffic；Delivery Guarantee 独立编码 |
| 6 | Hop Limit | 2 B | 网络序无符号数；`1..65534`，每跳递减，受 Hop Auth 保护；`0/65535` 拒绝 |
| 8 | Header Contract | 1 B | 冻结扩展组合/保留位 |

公共前缀之后按固定顺序编码：

```text
Realm ID                 4 B fixed
Source Address           1/2/3/4 B
Destination Address      1/2/3/4 B
Source Binding Generation      4 B fixed
Destination Binding Generation 4 B fixed
Session Generation       4 B fixed
Origin Sequence          4 B fixed
Hop Sequence             4 B fixed
Payload Length           2 B fixed
Optional Peer Hop Security Context or Group Security Context
Optional E2E Security Context
Optional Protocol Context
Optional Endpoint Context
Optional Route Context
Optional Path Context
Optional Hop Scheduling Context
Payload
Optional E2E Tag
Optional Peer Hop Authentication Tag or Group Authentication Tag
CRC32C
```

在不含扩展和 Tag 时，候选基础头加 CRC 的长度为：

```text
A0: 41 B
A1: 43 B
A2: 45 B
A3: 47 B
```

这里有意接受 A0 比 v5 W0 更大的开销，以换取地址复用防 ABA、固定安全代际、固定
Origin/Hop Sequence、16-bit Hop Limit、固定 Realm 和 CRC32C。若后续实测证明 Classic CAN 无法接受该成本，应优化
Carrier 或增加经过独立安全审计的邻接压缩上下文，不能再次把 Binding/Session 代际压缩
成 1 B。

### 6.3 扩展必须固定顺序

v6 不采用任意 TLV 链。MCU Decoder 根据 Flags 以固定顺序处理有限扩展：

1. Link Authentication Context 必须由 Header Contract 无歧义选择以下一种，两个存在位互斥：
   - Peer Hop Security Context：`hop_suite_id:u8 + hop_key_id:u16 +
     hop_key_generation:u32`；
   - Group Security Context：`group_id:u32 + group_generation:u32 +
     group_suite_id:u8 + group_key_id:u16 + group_key_generation:u32`；
2. E2E Security Context：`e2e_security_mode:u8 + e2e_suite_id:u8 +
   e2e_key_id:u16 + e2e_key_generation:u32`；Mode 只允许 `AUTH_ONLY/AEAD`，缺失该扩展
   才表示 `E2E_NONE`；
3. Protocol Context：`protocol_opcode:u16`；`BOOTSTRAP/CONTROL/TRANSFER/DIAGNOSTIC` 必须存在，
   `DATA` 必须不存在；
4. Message/Endpoint Context：`source_endpoint + destination_endpoint + interaction_role +
   operation_id`；
5. Route Context：`route_generation`；
6. Path Context：`path_id + path_generation`；
7. Hop Scheduling Context：`initial_budget_us + remaining_budget_us`；
8. 以后新增扩展必须升级协议版本或使用明确的扩展容器。

`Frame Type` 只标识 `DATA/CONTROL/TRANSFER/DIAGNOSTIC/BOOTSTRAP` 大类；控制和传输的
具体 Opcode 只编码在固定宽度 Protocol Context 中。Opcode 的解释域固定为
`{protocol_version, frame_type, protocol_opcode}`，不能藏在可由业务自由解释的 Payload、
Endpoint ID 或任意 TLV 中。Bootstrap 使用独立 Bootstrap Frame Type 和 Bootstrap Opcode
表；普通控制/Transfer Opcode 均由对应 Hop/E2E 安全合同认证。

各 Security Context 的 Suite/Key 字段存在时都必须非零且属于编译期登记的 Suite/Key 域；
Context 不存在时对应 Tag 必须不存在。不得通过逐个试 Key、猜测默认 Suite 或把未知 ID 映射为旧算法完成
解码。精确 Flag bit、字节 Offset 和 Suite registry 由 V6-03/V6-07 联合冻结，但字段宽度、
存在条件和顺序不得再改变。普通 Peer Frame 只能携带 Peer Hop Context；Group HELLO 只能
携带 Group Context；Bootstrap 在 Session 前两者都不携带。两个 Context 同时存在、Context
与 Frame/Opcode 不匹配或对应 Tag 缺失时，必须在任何 Replay、邻接或发现状态写入前拒绝。

Route Context 与 Path Context 是两个正交的存在位：动态 Route 使用 Route Generation，
Pinned Path 使用 Path ID/Generation；当精确 Path 属于一个可替换的 RouteSet 时两者必须同时
存在并同时进入端到端认证域。静态直连/静态 Route 可以不携带二者。Canonical AAD 的
Context Presence Mask 固定为 bit0=Route、bit1=Path，不得把同时存在降格解释成任一单独类型。

### 6.4 可变外层与不可变内层

字段分为：

- **端到端不可变域**：Protocol Version、Address Class、不可变 Flags、Header Contract、
  Realm、Source/Destination Address 与 Binding Generation、Session、Origin Sequence、Frame
  Type、Traffic、Delivery Guarantee、Interaction Role、Endpoint、Route/Path Context、
  Protocol Opcode、E2E Security Context、Operation ID、Payload Length 与 Payload；
- **逐跳可变域**：Hop Limit、Peer Hop/Group Security Context、remaining Hop Budget、局部
  Bearer/next-hop 信息以及明确允许更新的控制累计量；
- **本地状态**：物理地址、Link ID、队列槽、Driver token，不上 Wire。

E2E AAD 必须由 V6-03 冻结成唯一 canonical byte sequence，至少按固定字节序依次包含：

```text
protocol_version + address_class
immutable_flags + header_contract + frame_type + protocol_opcode（存在时）
traffic_class + delivery_guarantee + interaction_role
e2e_security_mode + e2e_suite_id + e2e_key_id + e2e_key_generation
realm
canonical_source_address + source_binding_generation
canonical_destination_address + destination_binding_generation
session_generation + origin_sequence
source_endpoint + destination_endpoint + operation_id（存在时）
route_context + path_context（各自按存在位写入；允许两者同时存在）
initial_hop_scheduling_budget（存在时；remaining 不进入 AAD）
payload_length
```

Canonical AAD 不依赖扩展是否省略产生歧义：缺少 E2E Security Context 时固定编码
`mode=E2E_NONE,suite=0,key_id=0,key_generation=0`；缺少 Protocol Context 时固定编码
`protocol_opcode=0`。线上扩展存在位与这些值必须一一对应，禁止“扩展存在但填零”或
“扩展缺失却按本地默认 Suite/Opcode”解释。

地址在线上虽然是 A0～A3，进入 AAD 前统一扩展为规范 32-bit；Address Class 本身仍进入
AAD，禁止中继静默重编码 Class。E2E Auth-only 的 Tag 输入为
`canonical_aad || plaintext_payload`；E2E AEAD 将 canonical AAD 作为 Associated Data，
将 Payload 作为加密/认证的数据输入，不能把 Payload 再重复解释为 Header AAD。逐跳可变
的 Hop Limit、Peer Hop/Group Security Context 和 remaining 调度预算不进入 E2E AAD，但由
对应 Link Authentication 覆盖。`initial_budget_us` 属于不可变合同并进入 E2E AAD。普通
可转发 Frame 的每个中继先验证入站 Peer Hop Tag，再递减 Hop、收紧可变预算、选择下一跳并
生成新的出站 Peer Hop Tag；Group HELLO 固定禁止进入该转发步骤。未知 Flag、保留位非零、Header Contract 与实际扩展不符
或 Route/Path Context 双存时，必须在认证前失败关闭。

### 6.5 完整性顺序

建议 Trailer 顺序为：

```text
Payload → E2E Tag(可选 16 B) → Peer Hop Tag 或 Group Tag → CRC32C(4 B)
```

普通 Peer Frame 与 Group HELLO 的接收顺序如下；Session 前 Bootstrap 使用 5.3 的
Cookie/transcript 专用顺序，不伪造本表的 Link Authentication Context：

1. 长度/Magic/Version/Class；
2. CRC32C 早拒绝随机错误；
3. 只读检查本地 ingress Link 与 Realm 是否允许该 Frame 类；此步不得创建 Neighbor、pending
   或任何远端可控状态；
4. 按 Header Contract 验证 Peer Hop Tag 或 Group Tag 及其独立 Replay Window；
5. Peer Frame 验证已认证 Neighbor/Session；Group HELLO 执行 8.1 的只提示发现策略；
6. 路由/转发或本机 Endpoint 分派；
7. 目标节点执行 E2E Open、精确 Opcode/Endpoint ACL 和业务校验。

### 6.6 Security Context、Key 选择与轮换

每个受保护 Frame 必须携带无歧义的 Key 选择上下文，禁止依赖“当前默认 Key”或接收端
试遍 Key Ring：

```text
HopKeySelector = {
  authenticated_peer_session,
  hop_suite_id,
  hop_key_id,
  hop_key_generation
}

GroupKeySelector = {
  realm,
  local_link_security_domain,
  group_id,
  group_generation,
  group_suite_id,
  group_key_id,
  group_key_generation
}

E2EKeySelector = {
  source_device_principal,
  destination_device_principal,
  session_generation,
  e2e_security_mode,
  e2e_suite_id,
  e2e_key_id,
  e2e_key_generation
}
```

`suite_id` 唯一决定算法、Nonce/Tag 长度和 KDF label；`key_id` 选择该 Principal/Peer 域内
的逻辑 Key；`key_generation` 是不可回绕的具体版本。相同 selector 只能映射到一把 Key。
轮换时允许 current 与 previous 两个明确 Generation 在固定 grace 内并存，但 Frame 必须
指明其中一个；到期/撤销后旧 Generation 即使 Tag 正确也必须拒绝。发布新 Generation 前
必须 persist-before-use，接收 Replay Window 按完整 selector 分域。

`GroupKeySelector` 在线上由 Realm 与完整 Group Security Context 表达；
`local_link_security_domain` 是本地配置绑定的物理共享介质/Link Instance 集合，不是可由
远端选择的 Wire 字段。相同线上 selector 在一个本地 Link Security Domain 中只能映射到
一把 Group Key。`group_generation` 表示成员/策略集合，`group_key_generation` 表示该集合内
的具体 Key 轮换；两者的 Owner、唯一性域、持久化、合法重置和耗尽行为严格服从 5.5 的
统一 Generation 表。跨 Address Authority/Group Key Owner 换主必须继承 high-water，不能
从 1 重启；两者均为非零 checked serial，任一变化都建立新的 Replay 域。Group Key
的 current/previous grace、persist-before-use、撤销和禁止试 Key 规则与 Peer Hop Key 相同，
但 Group 与 Peer selector、KDF label、Replay Window 永不共用。Group Key 只能来自签名产品
配置，或由已经通过 E2E 认证的 Realm Authority 控制事务下发并持久化；不得从 Group HELLO、
未认证 Bootstrap、普通 Capability 声明或邻居自报字段学习/替换。

Peer Hop Tag 或 Group Tag 都覆盖从 Magic 到 E2E Tag 末尾的完整实际编码字节，其中认证 Tag
字段自身不参与，CRC32C 不参与；因此 Header Contract、Frame Type、Protocol Opcode、
Peer Hop/Group Security Context、
Hop Limit、remaining budget 和 E2E ciphertext/tag 都受上一跳认证。E2E Tag 则覆盖 6.4
定义的 canonical AAD 与 Payload，明确包含 E2E Suite/Key selector、Protocol Opcode 和
安全合同，但不包含每跳可变的 Peer Hop/Group selector。CRC32C 最后覆盖除 CRC 字段自身以外的完整
Wire Frame，只用于随机错误早拒绝，不能代替任一 Tag。

Bootstrap 尚未建立 Session 时不伪造普通 Hop/E2E selector；它使用 5.3 的双方签名/MAC
Transcript 和允许的 bootstrap suite registry。Final Commit 建立 Key selector 后，普通
Control/Data/Transfer 才能进入上述 Tag 流程。任何控制消息是否还必须具有 E2E 认证，由
其权限语义冻结：改变 Binding、ACL、Route Authority、配置、Timed Command 或 Cluster
Authority 的控制必须 E2E 认证；仅一跳、不会提升权限的邻接控制至少必须 Hop 认证。

## 7. 消息、Endpoint、Traffic、交互与交付

### 7.1 删除重复 DATA Type

v6 删除 `DATA_Q0`、`DATA_Q1`。业务统一使用一个 `DATA` Frame Type，并由彼此独立的字段
表达：

```text
traffic_class         Q0 / Q1 / Q2 / Q3
delivery_guarantee    BEST_EFFORT / LATEST / RELIABLE
interaction_role      ONE_WAY / REQUEST / RESULT / ERROR
source_endpoint       uint16
destination_endpoint  uint16
operation_id          fixed uint64，REQUEST/RESULT/ERROR 时非零
time_mode             由 Endpoint 合同和可选 Realtime Envelope 表达
```

Control Opcode 与 Endpoint ID 不再共用一个编号空间。协议控制、Cluster、Realtime
Service 和用户 Endpoint 分别拥有命名空间与 ACL。

`delivery_guarantee` 只回答“怎样交付”，`interaction_role` 只回答“消息在业务事务中的
角色”。因此 Q2 Reliable Request、Q1 Latest Result、Q0 Best-effort One-way 都可以明确
表达，不允许再把 Request/Result 当作一种可靠性。

### 7.2 Operation ID、重复执行与幂等

`operation_id` 是发起 Device Principal 持有的非零 64-bit checked counter。它的唯一性域
固定为：

```text
{initiator_device_principal, operation_id}
```

- `ONE_WAY` 必须使用 `operation_id=0`，除非 Endpoint 明确选择可去重 Event Contract；
- `REQUEST/RESULT/ERROR` 必须使用同一个非零 `operation_id`；
- Request 的发起 Principal 由已验证 E2E Source 得到；Result/Error 的发起 Principal 是
  已验证 E2E Destination，自身 Header Source 只是执行方。这样 Session 轮换后也不会把
  Result 错归到执行方的编号域；
- 发起方以持久化区间预留 Operation ID；在同一 Device Identity 生命周期内不得回绕或
  重复。没有可证明的高水位时，只允许 `NON_RETRYABLE` One-way，不得发布可重试 Request；
- 相同 Operation Key、相同 Payload Digest 的重复 Request 不能重复执行副作用；
- 相同 Operation Key、不同 Payload Digest 必须作为协议冲突拒绝并记录安全诊断；
- `RELIABLE` 只保证协议重传与重组，不自动保证应用“恰好执行一次”；
- Endpoint 必须声明 `IDEMPOTENT_REPLAYABLE`、`DURABLE_AT_MOST_ONCE` 或
  `NON_RETRYABLE` 操作合同；
- `DURABLE_AT_MOST_ONCE` 只承诺同一 Operation 最多被协议分派一次，不等于任意外部
  执行器都能实现“副作用和结果恰好一次”；
- Recent Result Cache 只能优化同一运行期重放，缓存驱逐后不能伪装成持久幂等证明。

Durable Operation Journal 使用固定槽和以下单向状态机：

```text
EMPTY
  → PREPARED                 // key + request digest + endpoint contract durable
  → EXECUTING                // durable before handing the operation to executor
  → COMMITTED_RESULT         // result/failure durable and replayable
  → TOMBSTONED               // result retention/ack policy satisfied

EXECUTING -- reboot/unknown external outcome --> IN_DOUBT
IN_DOUBT -- authenticated reconciliation ----> COMMITTED_RESULT or TOMBSTONED
```

只有以下两类执行器允许从重启后的 `EXECUTING` 自动恢复并承诺结果重放：

1. 外部副作用与 Journal Result 位于同一可证明的原子事务；
2. 执行器接受 Operation Key，并能经认证查询“未执行/已执行及结果”，Owner 可据此对账。

其他执行器在 `EXECUTING` 后发生掉电时必须恢复为 `IN_DOUBT`：不得自动再次执行，也不得
伪造成功/失败结果；对端获得明确的 `IN_DOUBT`，由 Endpoint 产品策略执行人工或设备级
对账。这样的 Endpoint 只能承诺“最多一次尝试”，不能承诺副作用必然发生或结果必然可重放。
`PREPARED` 尚未交给执行器时可以安全继续或 Abort；`COMMITTED_RESULT` 的精确重复只能重放
相同 durable result。

Journal 表满必须在发送 ACK、进入 `EXECUTING` 或触发任何副作用之前返回 `NO_SPACE`。
`PREPARED`、`EXECUTING` 和 `IN_DOUBT` 不允许按时间自动 GC。`COMMITTED_RESULT` 只有在结果
已被认证对端 ACK，且最小 retention/replay 窗口已过后才可转为 Tombstone；Tombstone 仍
保留 Operation Key、request digest 与终态摘要，直到发起 Principal 的持久化 Operation ID
高水位已越过且最大重放寿命结束。GC 必须先持久化 Tombstone/高水位再复用槽，任何撕裂写
保留旧有效记录并失败关闭。固定容量、驱逐顺序、retention 和表满策略都属于 Endpoint
Manifest，不能运行时静默扩大或删除未决记录。

### 7.3 完成层级

API 和统计必须区分：

1. `LOCAL_ACCEPTED`：进入本机有界队列；
2. `LINK_SUBMITTED`：交给本跳 Driver；
3. `REMOTE_REASSEMBLED`：目标 Transfer 完整重组；
4. `REMOTE_INBOX_ACCEPTED`：目标 Endpoint Inbox 接受；
5. `APPLICATION_RESULT`：远端任务执行并返回结果。

任何 `UCN_OK` 都必须能说明它属于哪一级，不能把本机入队写成远端执行成功。

### 7.4 Traffic Class

| Class | 默认队列 | 目标 | 禁止行为 |
| --- | --- | --- | --- |
| Q0 Critical | 有界 FIFO/EDF | 控制、停机、故障动作 | 等待动态寻路、大消息、无限重试 |
| Q1 Realtime | Latest per Flow | IMU、状态、实时设定值 | 旧值堆积、单 Key 饥饿其他 Key |
| Q2 Normal | FIFO/Reliable | RPC、配置、普通消息 | 抢占 Q0 预留 |
| Q3 Bulk | DRR/剩余带宽 | 日志、文件、Transfer Fragment | 占满所有 Link Credit |

### 7.5 正交组合与非法合同

Traffic、Delivery Guarantee 和 Interaction Role 正交。例如 Q0 可以是 Best-effort
One-way，也可以是 Reliable Request；Q2 可以是 Reliable Request；Q1 可以是 Latest
One-way 或 Latest Result。具体 Endpoint Binding 必须冻结允许组合、最大 Payload、操作
幂等类别、安全策略和时间策略，非法组合在占用队列、Operation Journal 或 Transfer 槽前
拒绝。

## 8. HELLO Discovery 与认证后的能力协商

### 8.1 HELLO 只承担发现摘要

未绑定节点只发送 5.3 定义的 `BOOTSTRAP_HELLO`，不能发送本节的普通 HELLO。普通 HELLO
只允许已经具备非零 Address Binding Generation 和已认证 Session 的节点发送，建议只携带：

- Link Instance Generation；
- Session Generation；
- Capability Generation；
- Capability Digest；
- Admission State/最小发现标志。

普通 HELLO 不重复携带 Source Address；Source 与 Source Binding Generation 来自 Frame
Header。周期 HELLO 不携带完整能力表，避免共享无线/总线上的持续广播开销。接收 HELLO
只能刷新对应已认证 Link/Session 的发现租约，不能创建身份、绑定地址、授予角色或提升
Endpoint/Cluster 权限。

普通 HELLO 默认逐 Peer 单播并由该 Peer Session 的 Hop selector 认证。可选 Group HELLO
必须显式标记、绑定 Group Key Generation 和组播 Replay Window；它只能提示“某个已绑定
身份可能在线”，不能建立/续期 Peer Session、刷新单播认证租约或触发 Capability/ACL
写入。对未知 Peer、移动到新 Link 或 Link Generation 改变的已绑定节点，接收者必须启动
5.3 的 `PEER_REAUTH_*` 单播 Challenge，不能直接把组播 HELLO 当成安全邻接。

Group HELLO 的唯一 Wire 合同为：

```text
frame_type                    = CONTROL
protocol_opcode               = GROUP_HELLO
destination_address           = LINK_LOCAL_GROUP（当前 Address Class 的全 1 保留值）
destination_binding_generation = 0（仅此 Opcode 合法）
source_address/binding/session = 非零、仅作为 claimed identity hint
hop_limit                     = 1
Group Security Context        = present
Peer Hop/E2E/Endpoint/Route/Path/Hop Budget Context = absent
Group Authentication Tag      = present
forwarding                    = forbidden
```

`LINK_LOCAL_GROUP` 与 Bootstrap 的 `LINK_LOCAL_ALL` 可以使用相同全 1 保留编码，但必须由
Frame Type、Opcode、Source/Binding 合法域和 Security Context 严格区分；二者都不是可路由
地址。Group Tag 按 6.6 的 `GroupKeySelector` 选择唯一 Key 并认证完整实际 Frame。Group Replay
Window 固定按 `{GroupKeySelector, claimed source address/binding/session}` 分域，Packet
Sequence 重放、旧 Group/Key Generation 和未知 selector 必须在写任何状态前拒绝。

共享 Group Key 只证明发送者持有该组 Key，不证明 claimed Source 对应的独立 Device
Principal；任何组成员理论上都能伪造另一个成员的发现提示。因此 Group HELLO 不能满足
Endpoint ACL、Peer Identity、Capability、Authority、Binding Lease 或单播 Neighbor Lease，
也不能延长这些对象的 deadline。它最多在通过 per-Link/per-group Token Bucket 后，建立一个
有界的“建议发起 Peer Reauth”提示；提示表满时不驱逐已有项，超时不续期，攻击流量不能
触发无界签名验证。若产品需要可归责的组播身份，必须使用每发送者签名/认证的另一套明确
协议，不得把共享 Group Tag 解释成单播 Principal proof。

Group discovery hint 默认全局 8 槽、每 Link 2 槽、同一 claimed Binding 1 槽、绝对 1 s
timeout，并写入 Product Manifest；产品可以选择其他有限非零配置并重新通过资源门禁。相同 hint/replay 不刷新
deadline，同一 key 在 Reauth 已 pending 或 cooldown 未结束时不重复触发。`now==deadline` 才由
Owner 清理；不匹配 Group Frame 不得清槽或抢占。提示表和 Token Bucket 的表满/拒绝统计与
已认证 Neighbor、Capability、ACL 统计必须分开，避免诊断把组发现误报为安全邻接。

### 8.2 完整 Capability Record

当已认证节点首次发现对端、Generation 变化或 Digest 不匹配时，通过 Hop-authenticated
`CAPABILITY_QUERY/ADVERTISE` 交换完整记录。V6-06 只负责 Discovery、Capability 数据
模型、认证后的交换和缓存失效；它不拥有 JOIN FSM，也不能令节点进入 `ADMITTED`。
能力记录按作用域分为：

#### Link/Carrier Capability

- `carrier_mtu`：一次物理 Carrier 单元可承载的最大字节数；
- `link_frame_mtu`：Adapter 经过本地 Carrier 分段/重组后，可完整交付的最大 UCN Frame；
- Carrier 分片头、padding、CRC 和硬件 Tag 的固定开销及最大分片数；
- 有序/无序、可靠/不可靠、广播/单播；
- 速率与硬件优先级数量；
- RX/TX timestamp 类型和误差上界；
- Link Security 能力；
- 当前 Link Instance Generation。

#### Peer Capability

- 支持的 Core Feature Bit；
- 最大逻辑消息 Class；
- 最大 RX Window；
- 最大并发 Transfer；
- 支持的 AEAD/Hop Auth Suite；
- Realtime/Cluster/Service 能力；
- 固定表容量只发布安全可承诺值，不发布瞬时剩余槽作为权限。

#### Path Capability

Path 建立时逐跳归约：

```text
path_frame_mtu        = min(all link_frame_mtu,
                            all ingress/egress processing frame limits,
                            path policy limit)
max_message_class     = intersection/min(all hops and destination)
security_suites       = intersection(all required participants)
max_window            = min(source, destination, path policy)
max_concurrency       = min(source, destination, path policy)
timestamp_capability  = intersection + proven uncertainty bounds
```

`carrier_mtu` 与 `path_frame_mtu` 不能混用。Classic CAN 的 Carrier 可能只有 8 B，但其
Adapter 可以用固定 Carrier 协议重组更大的 UCN Frame；反之，Bearer 能发送大 Carrier 也
不代表中继 Node 有足够 Frame Buffer。

发送端必须对精确 Frame Contract 做 checked subtraction：

```text
max_payload(path, frame_contract, hop_suite, e2e_suite, address_class)
  = path_frame_mtu
  - common_and_address_header_bytes
  - enabled_security_protocol_endpoint_route_path_operation_extensions
  - e2e_suite_tag_bytes
  - hop_suite_tag_bytes
  - crc_bytes
```

Transfer 的 `fragment_data_budget` 还必须继续减去 Message ID、Fragment Index/Count、
SACK/Credit 所需的实际 Fragment Header；Realtime Frame 则减去 Time/Scheduling 扩展。
任一步下溢、Unknown 或结果为 0 都必须在占用 TX/RX/重组槽前拒绝。Path Contract 应缓存
`path_frame_mtu` 和按允许 Contract 计算的 Payload Budget，而不是把单一 `path_mtu` 同时
解释为 Carrier、Frame 和业务 Payload。

能力未知、过期、Digest 不一致、Bearer/Route/Path/Session Generation 改变时必须在发送
前失败关闭。不得先分配 8 KiB 重组资源，传到中间节点后才发现 MTU 或能力不支持。

### 8.3 能力不是授权

Capability 只表示“能够处理什么”，Authorized Class/ACL 表示“允许做什么”。远端自报
支持 Q0、Cluster 或 Realtime 不会提升权限。权限必须由认证 Device Identity、Session
和本地策略产生。

## 9. 生产安全

### 9.1 JOIN 状态机

V6-07 是唯一拥有 JOIN 状态和 `ADMITTED` 写权限的模块。V6-06 不建立第二套无认证 JOIN。
未绑定设备的唯一 JOIN 必须实现：

```text
UNBOUND
  → BOOTSTRAP_DISCOVERED(device nonce + txid allocated)
  → RETURN_PATH_COOKIE_ECHOED(no Authority pending or expensive proof yet)
  → AUTHORITY_CHALLENGED(authority identity/generation verified)
  → MUTUALLY_AUTHENTICATED(device proof verified; transcript frozen)
  → ADDRESS_OFFERED(offer proof + lease/fence/quorum verified)
  → ADDRESS_COMMIT_PENDING(both acceptance proofs; persist-before-use)
  → ADDRESS_BOUND(final authority proof + reload verified)
  → SESSION_ESTABLISHED(explicit Suite/Key selectors installed)
  → ADMITTED
```

Bootstrap 只完成一跳发现、双向身份 Challenge 和地址提议。JOINING_DEVICE 必须先验证
Authority Principal、Authority Generation、有效写 Lease/Fence 和 transcript proof；Authority
必须验证完整 Device Principal 与 device proof。`ADDRESS_BOUND` 必须先持久化 Address
Binding Generation、Lease/Certificate 与 Authority high-water；`SESSION_ESTABLISHED` 必须
绑定 5.3 的完整 Challenge Transcript、双方 Device Principal、Realm/Address/Binding、
双方 nonce/txid、Security Suite、Key ID 和 Key Generation。
`ADMITTED` 只表示基础安全 Session 获准参与普通网络控制；某项 Endpoint、Realtime、
Cluster 或 Q0 权限仍需认证 Capability 和 ACL。

任一步失败都不得写入已准入 Neighbor、Route、Path、Cluster Member 或 Endpoint 权限。
JOIN 重放、并发 Challenge、Bootstrap ID 冲突、地址冲突、Cookie/认证前预算失败、Provider
PENDING、掉电和表满必须原子失败关闭。精确重复事务只允许重发已经持久化并经 reload
证明的相同结果；不能刷新 Cookie/pending/Lease deadline。

已绑定设备在新 Link 上没有 Peer Session 时使用独立重认证 FSM：

```text
ADDRESS_BOUND_NO_PEER
  → PEER_REAUTH_COOKIE_ECHOED
  → PEER_REAUTH_CHALLENGED
  → PEER_IDENTITY_AND_BINDING_VERIFIED
  → PEER_SESSION_ESTABLISHED
```

它使用现有 Binding Certificate和全新双方 nonce/Link Instance Generation，不经过
`ADDRESS_OFFERED`，也不能改变地址或写 `ADMITTED`。只有仍有效的 Binding/Authority
Certificate、未撤销 Principal 和完整 transcript 都验证通过，才可在该 Link 上发送普通
HELLO。普通 HELLO、Group HELLO 和 `PEER_REAUTH_*` 三者不得共享 Replay/nonce 域。

### 9.2 两层认证

- **Peer Hop Authentication**：保护普通 HELLO、控制面、Hop、Route/Path 控制和已认证每跳来源；
- **Group Link Authentication**：只保护共享介质上的组发现完整性/Replay，不能证明独立来源 Principal；
- **E2E Auth-only**：不要求保密、但需要证明原始 Device Principal 的业务；
- **E2E AEAD**：同时需要端到端保密、完整性和原始 Principal 的业务。

Peer Hop/Group/E2E Suite、Key ID 与 Key Generation 必须来自 6.6 的显式 selector。接收者不得用当前
默认 Key、隐式 Realm 配置或试遍候选 Key 来解释 Frame。Control/Transfer Opcode 位于
Protocol Context；Peer Hop/Group Tag 覆盖其线上编码，凡会改变端到端权限/Authority 的 Opcode 还必须
进入 E2E AAD。Bootstrap 在 Session 前只使用冻结的 bootstrap suite/transcript proof，不与
普通 Frame 的 selector 混用。

物理 Link 自带加密不自动等于 UCN Hop Authentication，除非 Adapter 能提供经审计、
绑定 UCN Neighbor/Session 的等价证明。

威胁模型默认不完全信任中继。Hop Authentication 只能证明“上一跳是谁”，不能让目标
节点把外层 Source Address 当作端到端 Device Principal。凡 ACL、Timed Command、配置
写入、业务 Request、Cluster Authority 或审计记录依赖原始 Principal 的消息，必须使用
E2E Auth-only 或 E2E AEAD，并验证 6.4 的完整 canonical AAD。只有显式声明为
`PUBLIC_UNAUTHENTICATED` 且 ACL 不引用原始 Principal 的 Endpoint 才可只靠 Hop 安全；
该模式不得承载 Q0 权威控制、Request 副作用或权限提升。

Security 解封后交给 Capability、Route、QoS、Transfer、Realtime 与 Cluster 的
`ucn_v6_security_open_result_t` 是已验证语义 DTO，不是跨进程、跨地址空间或可抵御任意
本地写内存攻击的不可伪造权能。Security 与这些进程内直接使用方共同构成 MCU 固件的可信计算基；
本地任意内存破坏、恶意调用者伪造 DTO 属于内存安全/固件完整性问题，不能由 C99 结构体解决。
所有使用方仍必须独立校验字段合法域及自身需要的父代际、ACL、Deadline 与 Authority 条件，
但不得把“结构体由调用方可写”误述成远端网络攻击者可绕过 Security 的线上认证。

### 9.3 Replay 与持久化

- Origin Sequence 在端到端/Group 重放域内严格前进，中继不得修改；
- Hop Sequence 在每个下一跳 Peer Session 内严格前进，中继验证入站后为出站重新分配；
- 发送端通过持久化区间预留，不能每帧写 Flash；
- 接收端为授权 Source/Session 维护固定 Replay Window；
- Session Generation、Key ID、Key Generation、Sequence 高水位和撤销状态不得回退；
- 持久化失败时不得发送可能复用 Nonce 的受保护帧；
- Rotation 必须 persist-before-use。

### 9.4 Endpoint ACL

ACL Key 至少包含：

```text
Device Principal
Source/Destination Address Binding Generation
Session Generation
Source/Destination Endpoint
Frame Type
Protocol Opcode（非 DATA 必须精确匹配；DATA 固定为 0）
Traffic Class
Delivery Guarantee
Interaction Role
Operation ID（适用时）
Direction
```

ACL 查找使用完整 canonical tuple；`Frame Type` 不是其下全部操作的通配授权。CONTROL、
TRANSFER、DIAGNOSTIC 和 BOOTSTRAP 必须在解析并认证固定 Protocol Context 后，以精确
`protocol_opcode` 再查 ACL/控制策略，未知 Opcode 在状态分派前拒绝。特权操作不得仅凭
`Frame Type=CONTROL`、Endpoint 通配或同 Traffic Class 获权；若产品要授权一组 Opcode，
必须在签名产品配置中显式枚举，而不是用隐式 wildcard。DATA 不携带 Protocol Context，
进入 ACL key 时使用唯一 canonical 值 0，避免“字段缺失”和“任意 Opcode”混淆。

未建立 Session 的 Bootstrap 使用 5.3 的 Realm Trust/commissioning policy 和 transcript
准入，不借用普通 Endpoint ACL。Group HELLO 使用独立 Group discovery policy；共享 Group
Tag 不能满足需要 Device Principal 的 ACL。转发节点只执行逐跳控制/资源 ACL；目标节点若
按原始 Principal 授权，还必须在 E2E Open 后用同一个 authenticated Opcode tuple 再检查，
不得让外层和内层对 Opcode 得出不同解释。

Q0、Timed Command、Cluster Authority 和配置写入必须使用比普通 Telemetry 更严格的
授权与预算。

## 10. RouteSet、Path 与多 Bearer

### 10.1 统一对象

每个 `(traffic_origin,destination)` 对应一个固定容量 RouteSet：

```text
RouteSet
  - active_flow_routes[N]
  - standby_routes[N]
  - candidate_transactions[N]
  - pinned_paths[N]
  - current route_generation
  - previous generation + bounded grace
```

动态 Route 的主键是：

```text
{realm,
 origin_address, origin_binding_generation, origin_session_generation,
 destination_address, destination_binding_generation,
 route_generation}
```

Candidate 的唯一主键是 `{完整 Route Domain, Candidate Transaction ID}`，事务高水位也
由同一 Route Domain 持有；不同业务 Origin/Route Domain 可合法复用相同数值的 Transaction
ID，不得互相拒绝或命中。Activate/ACK 必须继续绑定该完整主键、Route Generation 与冻结
Proposal Digest。Candidate 一旦开始 Probe、分配 Generation 或发送 Activate，其 Path
Snapshot 永久冻结；任何路径变化必须新建事务。
同一地址被另一 Device 重新获得后，即使它碰巧使用相同 Session Generation，也不能命中
旧 RouteSet、Replay、Capability、ACL 或 Path；Address Binding Generation 是地址租约
所有权的一部分，而不是诊断字段。

### 10.2 手动指定与自动负载均衡

- **Pinned**：Flow 只能走指定 Path；Path 失效后按策略失败或重新寻路；
- **Active/Standby**：默认单主备，保证顺序；
- **Weighted Multipath**：只对允许乱序或具备重排的 Flow 开启；
- **Per-flow Hash**：同一 Flow 固定 Route，不逐包随机；
- **Policy Override**：每个 Node 可以按本地任务需求选择策略，但不能篡改 Wire 身份或
  远端权限。

### 10.3 指标向量

Link 不再只输出一个无法解释的通用 Cost。规范化指标至少包含：

```text
administrative_cost
latency_us
jitter_us
loss_ppm
available_bitrate_bps
queue_occupancy_permille
energy_cost
stability_score
freshness
```

协议定义单位、Unknown、采样窗、EWMA、饱和和陈旧规则；每个 Traffic Class 使用冻结的
默认权重，产品可在允许范围内覆盖。RREQ 累积的是带 Algorithm ID 的可比较结果，不能
让不同节点使用不兼容公式后仍直接比较总 Cost。

### 10.4 端到端路径健康

一跳 Heartbeat 只证明 Neighbor/Link。Path 健康由：

- 业务成功/失败；
- Probe RTT/丢失；
- Route Error；
- Capability/MTU Generation；
- 可选端到端 Keepalive；
- Transfer ACK/Result；

共同判断。不得把一跳 Heartbeat 当成多跳路径可达证明。

Route Owner 是下游模块使用路径的唯一真相源。Transfer、Realtime 或业务发送方只能保存
`{Route Domain, Route Generation, Path ID, Path Generation}` 稳定引用；每次产生发送、SACK、
Deadline 或执行副作用前，必须由 Route Owner 重新解析当前或仍在有界 Grace 内的精确 Path，
并同时重验即时下一跳 Capability 与目标 Path Capability。调用方不得直接构造完整
`RoutePath` 作为授权输入。

Capability、Route 与 QoS 的累计跳数统一使用 16-bit 无符号语义，合法范围为
`1..65534`；`0` 表示无有效路径，`65535` 保留为无效/耗尽哨兵。Route Proposal Digest 必须
编码完整 16-bit 跳数，不能截断成 8-bit，也不能让 256 跳以上的合法 Path 在模块边界别名。
Wire Hop Limit 使用同一 16-bit 合法域，因此任何能被 Route Owner 安装的路径都有
唯一、可表达的逐帧转发范围；不允许再存在“路径模型合法，但帧因 8-bit TTL 无法到达”
的第二套跳数上限。产品仍应在 Manifest/Policy 中把实际可用上限收窄到已验证的
实时范围，而不是默认使用 65534 跳。

## 11. Transfer、流水和拥塞控制

### 11.1 消息等级

继续保留能力桶：

```text
T32 / T64 / T128 / T256 / T512 / T1K / T2K / T4K / T8K
```

等级表示最大承诺，不要求消息必须恰好等于桶大小。Endpoint 可接受任意不超过其等级、
Path 的实际 Payload/Fragment Budget 和当前资源合同的长度。

### 11.2 选择确认

v6 建议以固定窗口 Selective Repeat 取代默认 Go-Back-N：

- Fragment Sequence 和 Message ID 固定宽度；
- ACK 带 cumulative base + received bitmap；
- 只重传缺失 Fragment；
- Window 上限编译期固定；
- RX 缺槽在首片前明确拒绝；
- Recent Completion 防止最后 ACK 丢失导致重复执行；
- Result 与 Reassembly ACK 分离。

发送中的 Transfer 只冻结 Route Owner 给出的一个规范依赖链
`{本机出口 Link, next-hop Session, next-hop Capability, Route, Path}`，以及稳定的
`{Route Domain, Route Generation, Path ID, Path Generation}` 引用。Path Capability 中的
本地父级必须与本机即时出口完全相同；目标 Capability/Session 则是该 Path 声明的一部分，
由 Route Owner 在每次发送和 ACK 推进前重新解析并校验。依赖链精确失效，或 Route Owner
无法重新解析同一 Route/Path 引用时，后续副作用立即失败关闭；清理路径仍须返还
caller-owned Buffer Token。尤其在 `A→B→C` 中，A-B 出口断开必须终止 A 的 Transfer，
不能只检查 C 的目标能力租约，也不能让调用方用自构造 Path 绕过 Route Owner。

### 11.3 逐跳 Credit

端到端 ACK 解决完整性，不解决中继队列被快速源端压满。Q2/Q3 增加有界逐跳 Credit：

- 每个 Link/Class 有固定可用 Credit；
- 中继只有在下一跳有 Credit 时接纳新的 Bulk Fragment；
- Q0/Q1 拥有独立预留，不被 Q3 借尽；
- Credit 租约到期后立即停止借用，但槽位保留 Peer Session、Link Generation、Credit
  Generation 和 Sequence 高水位；不得因超时清槽而从 1 重新开始。只有精确 Session/Link
  失效或显式安全重建才释放该代际历史；
- 不允许负 Credit、无限累计或由未认证对端提高预算。

接收端重组超时只回收尚未完成的消息。完整消息一旦进入 `COMPLETE`，其 Payload 与重放
身份归应用消费生命周期所有；只有应用显式复制/领取并退休后才可释放。应用迟滞必须形成
可见背压，不能靠普通重组定时器静默丢弃完整消息或重新开放同一 Message ID 的执行窗口。

### 11.4 多跳流水

中继不等待完整 8 KiB 消息，只对单 Fragment 做验证、路由和有界排队。不同 Fragment
可以同时位于不同 Hop，从而让延迟随 Hop 增加，但稳态吞吐主要由瓶颈 Link 和调度开销
决定，而不是天然按 `1/N` 下降。

## 12. QoS 调度

默认调度建议采用两层结构：

1. Admission：Q0 预留、源/目标本地 Deadline 和 Endpoint Budget；
2. Link Scheduler：Q0 在 Flow 间执行有界 round-robin、在同一 Flow 内执行 bounded
   priority/EDF；Q1 执行 per-flow Latest round-robin；Q2/Q3 使用 Weighted DRR。

中继默认只看到 Traffic Class，不读取 E2E 加密 Payload 内的绝对 Deadline。只有 Frame
显式携带 15 节定义的 Hop Scheduling Budget 时，中继才可在本地 EDF 中使用该预算；
缺少扩展时按 Class/Flow 公平策略调度，不能猜测端到端 Deadline。

Hop Scheduling Budget 不是优先级凭证。每一跳必须先完成 Hop Authentication，并按已认证
Peer、Source Binding 和 Flow 落入编译期固定的 admission/token bucket；随后只能在 Frame
原有 `traffic_class` 的已获配额内部改变顺序：

- 不能把 Q1/Q2/Q3 提升为 Q0，也不能改变 Delivery Guarantee；
- 不能占用其他 Class、其他 Source 或其他 Flow 的保留 Credit；
- Q0 的固定预留只由本地 ACL/Endpoint policy 授予，极小 Budget 不能扩大该预留；
- 同 Class 内先执行 per-source/per-flow quota，Flow 之间保持有界公平轮转，只在本 Flow
  已获准的候选中使用本地 priority 与 remaining budget 做 EDF；因此单个恶意 Peer 不能靠
  持续声明极小 Budget 饿死同级 Flow；
- 接收必须验证 `initial_budget_us > 0`、`remaining_budget_us > 0` 且
  `remaining_budget_us <= initial_budget_us <= endpoint/path_policy_max_budget_us`；
- 每跳以 checked-subtract 扣除已测 residence time 与保守 transmit bound，只能减不能增；
  无可信扣减上界、算术下溢或 Budget 耗尽时不得继续以 Deadline-aware 身份传播。

Budget 耗尽的唯一 v6 行为是 `DROP_EXPIRED`：立即丢弃并产生有界诊断。不得清除扩展后
回到普通队列继续无限传播，也不得降低或改写已经进入 E2E AAD 的 Traffic Class。目标端
E2E Deadline 仍独立检查；Hop Budget 既不能延长期限，也不能跳过 Endpoint ACL、Credit
或安全门禁。

`6:3:2:1` 可以作为默认 Weight，但不再被误认为绝对带宽比例。每个 Link 可以根据
硬件能力映射 CAN ID、802.11e/ESP-NOW 队列、USB Endpoint 或 DMA Queue。软件统计必须
区分 enqueue、scheduler select、link submit、physical complete、remote ack 和 app result。

## 13. Protocol Owner 与 Driver

### 13.1 唯一执行模型

```text
ISR / SDK callback
  → fixed driver ring
  → atomic event record
  → notify Protocol Owner
  → owner drains with source budget
  → Core/Service state transition
```

Driver Callback 不能执行路由、Cluster、解密或应用任务。只有明确标为 ISR-safe 的 API
可以从 ISR 调用。

### 13.2 Owner API

建议统一为：

```c
ucn_result_t ucn_owner_run(
    ucn_owner_t *owner,
    uint64_t now_us,
    const ucn_owner_budget_t *budget,
    ucn_owner_run_result_t *result);
```

`result` 至少返回：

- 是否仍有工作；
- 下一绝对 Deadline；
- 哪类 Source 仍积压；
- 是否需要立即再次调度；
- 最近错误与固定统计快照。

定时器只负责 Deadline 和漏通知兜底；正常 RX/TX completion 到达时立即通知 Owner。

### 13.3 Buffer 所有权

每个接口必须明确：

- RX Buffer 何时转交 Core；
- TX Buffer 借用到 submit 还是 physical completion；
- Cancel/Timeout 后谁回收；
- DMA/Cache 一致性由谁维护；
- 同一 Token 的 `RESERVED→SUBMITTED→COMPLETED/CANCELLED→RETIRED` 生命周期。

## 14. 公共 API、对象与配置

### 14.1 私有状态

Node、Transfer、Realtime、Cluster 和各事务状态不再由应用直接写字段。公共 API 只暴露：

- `*_config_t`：初始化输入；
- `*_handle_t`：不透明句柄；
- `*_view_t` / `*_snapshot_t`：只读诊断输出；
- 编译期生成的 `UCN_*_STORAGE_BYTES`、`UCN_*_STORAGE_ALIGNMENT` 和声明宏；
- `*_storage_required()`：只用于启动时复核编译期容量和对齐；
- `*_init_in_place()`：在调用者内存中构造。

这样既不使用堆，也让正常应用代码无法依赖或直接赋值私有字段。Opaque 不是内存隔离：
与库处于同一地址空间的错误或恶意调用者仍可 `memset`、越界写或借助未定义行为破坏
caller-owned Storage，协议无法物理阻止这种覆盖。公共合同明确**不支持**应用直接清零、
复制或改写已初始化 Storage；API 通过 magic、Schema、Layout Hash、不可逆 Fence、可选
canary/CRC 和入口结构验证尽早检测损坏并失败关闭。需要抵御同地址空间恶意代码的产品，
必须另用 MPU/TrustZone/进程隔离，这不属于 Opaque C API 的保证。

UCN 使用 C99，不能依赖函数返回值作为文件作用域数组长度，也不能假设 C11 `_Alignas`
可用。产品配置生成器必须产生类似以下的编译期合同：

```c
#define UCN_NODE_STORAGE_BYTES      ... /* integer constant expression */
#define UCN_NODE_STORAGE_ALIGNMENT  ... /* integer constant expression */

typedef union ucn_node_storage {
    ucn_port_storage_alignment_anchor_t _alignment;
    uint8_t bytes[UCN_NODE_STORAGE_BYTES];
} ucn_node_storage_t;

#define UCN_DECLARE_NODE_STORAGE(name) ucn_node_storage_t name
```

`ucn_port_storage_alignment_anchor_t` 由受支持工具链/Port 在编译期定义；配置生成阶段必须
证明它的实际对齐不小于 `UCN_NODE_STORAGE_ALIGNMENT`。GCC/Clang/MSVC 的 attribute/
declspec 只能封装在 Port 宏中，不能泄漏为通用 API。每种 Feature/Profile 组合都生成
相应 Storage 类型，并用 C99 typedef assertion/`offsetof` 探针验证大小与对齐。

`*_init_in_place(storage, storage_bytes, config, manifest)` 必须在写入任何字节前检查：

1. 指针非空；
2. `storage_bytes >= UCN_*_STORAGE_BYTES`；
3. `(uintptr_t)storage % UCN_*_STORAGE_ALIGNMENT == 0`；
4. 编译进库和应用的 Feature Manifest/Layout Hash 完全相同；
5. Storage Schema/API Version 与本实现相同；
6. 配置容量与生成布局一致。

任一检查失败，Storage 保持不变。`storage_required()` 返回的运行期 view 只能用于诊断或
动态宿主分配，不能替代文件作用域可用的编译期常量。

### 14.2 单一配置事实源

最终只允许一个产品配置入口：

```text
ucn_product_config.h
```

规则：

- 公共头不再重复定义对象布局相关默认值；
- CMake/编译命令、所有静态库和应用必须使用同一配置指纹；
- 链接时检查 Feature Manifest/Layout Hash；
- 编译期配置管理容量、Feature 和静态资源；
- 运行期配置管理 Node Address、Link 实例、密钥 Provider、策略和 Endpoint；
- 非法容量在编译期失败，而不是缩窄到 `uint8_t` 后运行时回绕。

### 14.3 Profile

- Nano：静态直连/静态 Route/基础安全/四级解码；
- Lite：增加 Neighbor、HELLO/JOIN、Heartbeat、AODV-Lite；
- Full：增加 Candidate、RouteSet、Pinned Path、Policy/Balance 和诊断；
- Transfer、Realtime、Cluster、Service Directory 为正交 Feature，不隐式等于 Full；
- 未编译 Feature 的 API 可以不导出，Feature Manifest 负责集成期检查，不再依赖大量
  “链接存在但返回 CONFIG”的兼容 Stub。

## 15. Realtime

Realtime 继续采用按 Endpoint 选择的 Payload Envelope：

- `NONE`：零额外时间字节；
- `LOCAL_STAMP`：仅来源本机时间；
- `SYNCED_STAMP`：受认证 Time Domain；
- `DEADLINE`：在入队前和执行前各检查一次。

v6 集成必须补充：

- Capability Advertise 中声明 Timestamp/Clock/Uncertainty 能力；
- 固定 Path 才能建立 v1 同步事务；普通动态 Route 只允许诊断或 LOCAL/NONE；
- Domain Generation、Authority Generation 和高水位持久化复用统一 checked serial；
- Timed Command 的 ACL、E2E 和 Source Identity 是强制条件；
- 中继不解析 Realtime Payload，也不能从加密 Payload 取得绝对 Deadline。默认 Deadline
  只在源端入队和目标端接收/执行时生效；
- 需要逐跳 Deadline-aware 调度时，使用可选 `HOP_SCHEDULING_BUDGET` 固定头扩展：源端
  把 `initial_budget_us` 纳入 E2E AAD，同时在逐跳域携带 `remaining_budget_us`；每个诚实
  中继必须按本地 residence/transmit 上界只减不增，并由 Hop Tag 认证当前值；
- Hop Budget 只允许在原 Traffic Class、已认证 Peer/Source/Flow 的固定配额内排序，不能
  升级 Class、侵占其他 Class/Flow 的预留或授予 Authority；必须满足
  `0 < remaining <= initial <= policy_max`。Unknown/溢出拒绝；耗尽时固定
  `DROP_EXPIRED`，不能删除扩展、改写 Traffic Class 或回退到普通调度继续传播；
- 恶意中继仍可延迟或丢包，但目标端 E2E Deadline 门禁不能被它放宽；
- 真实 ESP32/CAN-FD/USB/UART 硬件时间戳误差必须实测后才能发布对应能力。

## 16. Cluster

### 16.1 独立但不平行造轮子

Cluster 保持 Optional Extended：

- Core 不依赖 Cluster；
- Cluster 控制使用同一个 v6 Frame、Address、Session、Hop Auth 和 Capability；
- 删除 Cluster v3/v4 双 Wire 和 Mixed Version Policy；
- 只保留 Target FSM，不保留 Current/Legacy 行为桥；
- Cluster Member 使用 Device Principal/Node Address，不另造身份；
- Cluster Transfer/Directory/Tunnel 使用统一 Message/Transfer/Path Frame MTU 与 Payload
  Budget；
- Authority 必须通过 Core Security 和 Cluster Quorum/Persistence 双重门禁。

### 16.2 持久化

新 Record Schema 直接表达最终：

- Active/Max Epoch；
- Vote ID；
- Config Stable/Joint Transaction；
- Backup/Takeover/Handover 证明；
- Rekey/Lineage/Tombstone；
- boot/session incarnation；
- operation journal 和 anti-rollback witness。

不再保留 legacy PREPARED abort 或旧 Record migration operation。旧 Flash 必须由刷机流程
显式擦除或离线迁移，v6 Runtime 不猜测旧记录。

## 17. Forwarding 性能

中继必须区分 Fast Path 与 Slow Path：

### Fast Path

1. Prefix/长度/CRC；
2. Link/Neighbor/Hop Auth/Replay；
3. RouteSet/Path 查表；
4. Hop 递减；若存在 `HOP_SCHEDULING_BUDGET`，只收紧 remaining budget；更新出站
   Hop Tag/CRC；
5. 零业务解析地提交下一 Link。

### Slow Path

HELLO/JOIN、RREQ/RREP/RERR、Capability、Path Install、Cluster、Realtime Sync 和诊断进入
对应控制状态机。普通中继不得解析 Service Payload、Transfer 业务内容或 Realtime
Envelope。

目标实现可使用 caller-owned frame buffer/slice 避免完整 Payload 二次复制；是否启用必须
由 DMA/Buffer 生命周期测试证明。FPGA 可实现相同 Fast Path，但 MCU Core 仍是完整协议
实现，FPGA 不成为普通网络的必需组件。

## 18. 错误与诊断

`ucn_result_t` 继续表达调用结果，但每个有状态模块提供结构化诊断：

```text
module
operation
reason
source/link/path/route generation
traffic/delivery
queue high-water
deadline/age
security/capability state
counter snapshot
```

错误统计使用饱和计数。诊断读取不得改变状态、续租、消费 Token 或泄露密钥。完整 Path
Trace、Node Snapshot、Cluster Directory 和安全信息必须经过独立 ACL 与速率限制。

## 19. 明确删除清单

v6 实现阶段应删除，而不是继续隐藏在宏后的内容：

- Core v4/v5 Runtime Decoder/Encoder；
- Cluster Wire v3、实验 v4 双分派和 Mixed Version Policy；
- `DATA_Q0`、`DATA_Q1` 重复类型；
- PATH_INSTALL 基础/扩展双格式；
- `UNSPECIFIED → W3` 的旧兼容落点；
- 旧六字段 Port 初始化和兼容 Stub；
- public transaction reset/直接可写状态字段；
- per-header 对象布局默认值；
- legacy PREPARED migration 和旧 Storage Layout load；
- 将 Wire Profile 当 Capability/Authorization 的字段和文档；
- 仅为了旧节点存在的 downgrade、fallback、双 Golden 和互操作测试。

V6-01 必须建立 `Compatibility Removal Manifest`，不能只用本节概括代替逐项销账。每行
至少包含：

| 字段 | 含义 |
| --- | --- |
| category | Wire/API/ABI/Storage/CMake/test/doc/fallback |
| current_file_or_symbol | 当前文件、符号、宏、Option 或 Target |
| removal_stage | V6-02～V6-15 中负责删除的唯一任务 |
| v6_replacement | 新合同、替代 API 或明确“无替代” |
| negative_gate | 旧输入必须被拒绝，或旧符号必须不存在 |
| verification | `rg`/`nm`/CMake configure/build/test/文档检查命令 |
| status_and_commit | 未开始、已删除、外审通过及绑定 commit |

Manifest 初始扫描必须覆盖 Core v4/v5 Codec、Cluster v3/v4、Mixed Policy、Record v1/v2、
legacy PREPARED abort/recovery、`UNSPECIFIED→W3`、兼容别名、旧 Port 初始化、Profile
compatibility Stub、旧测试 Target、CMake Option 和当前文档入口。V6-15 将其转换成
`rg`、符号表和 CMake denylist，所有发布构建必须归零。

Version/Magic/Schema 拒绝、`struct_size/api_version`、Feature Manifest/Layout Hash、v6
Capability 协商、Nano/Lite/Full、Optional Feature 以及“旧 Frame/Record 必须拒绝”的负向
测试不是兼容层，必须保留。

历史源码只保存在 v4/v5 Git Branch/Tag，不复制到 v6 当前分支的 `archive/*.c` 或
`archive/*.h`。当前分支可以保留明确标记的历史审计 Markdown 和不可执行 evidence，但
任何旧源码都不能进入构建扫描、安装包、头文件发布或静态库。

## 20. 实施顺序与依赖

```text
V6-00  RFC 和范围冻结
  ↓
V6-01  v5 实验快照、分支和清洁基线
       + Compatibility Removal Manifest
  ↓
V6-02  Identity/Bootstrap/Address Binding/Generation
  ↓
V6-03  Core Wire v6 + Golden/Negative
  ↓
V6-04  Message/Endpoint/Traffic/Guarantee/Interaction
  ↓
V6-05  Opaque API/Config/Owner contract
  ↓
V6-07  Production Security + unique JOIN FSM
  ↓
V6-06  Authenticated HELLO/Capability/Path Frame MTU/Payload Budget
  ↓
V6-08  RouteSet/Path/Multipath
  ↓
V6-09  Metrics/QoS/Hardware priority
  ↓
V6-10  Transfer Selective ACK/Credit/Pipeline
  ↓
  ├─ V6-11  Realtime production integration
  └─ V6-12  Cluster Target on unified v6 Wire
       （并行 Optional Feature，互不链接、互不依赖）
  ↓
V6-13  Adapter/RTOS/reference product
  ↓
V6-14  Full validation and resource gates
  ↓
V6-15  UCN 1.0 release candidate
```

V6-03 只生成隔离、default-OFF 的 Decoder/Encoder、Golden 和 Negative target，不得链接
生产 Node/Adapter，也不得发送真实 v6 Frame。Security 设计从 V6-02 开始参与评审，V6-07
是唯一 JOIN 与完整安全实现门；只有 V6-07 外部复审 GO 后，才允许把经过审计的 v6 Codec
接入生产 RX/TX。V6-06 只能在
该门之后交换认证 Capability，不能创建第二条 admission 路径。Cluster 可以在 V6-03～10
期间维护纯模型测试，但生产接线必须等待统一 Identity、Security、Capability、Transfer
和 Persistence 合同。

V6-11 Realtime 与 V6-12 Cluster 只共享 V6-03/05/06/07 形成的 Wire、Owner、Security、
Capability、Generation 和 Persistence 基座。两个 Feature 的库、对象、配置和链接依赖
必须保持独立；启用 Cluster 不得隐式启用 Realtime，反之亦然。二者可以并行开发和外审，
V6-12 不依赖 V6-11 完成。

## 21. 每阶段统一验收

每个 V6 子项至少执行：

1. 字段/状态机/所有权合同；
2. 正向 Golden 或确定性模型；
3. 每条规则独立负例，失败输出不写回；
4. 表满、超时、回绕、重放、乱序、重复和 Callback 重入；
5. Nano/Lite/Full 与 Feature ON/OFF；
6. Debug/Release、GCC/MSVC/Clang；
7. ASan/UBSan、静态分析；涉及并发共享对象时增加 TSan；
8. 中文源码/构建目录和公开头独立编译；
9. 固定对象大小、栈、Flash 和最小 MTU；
10. 分项自审、全体自审、外部审计；
11. 适用时执行目标 MCU、故障注入、掉电和长期运行；
12. 文档、测试、固件和报告绑定同一 commit。

测试必须能让旧错误实现失败，不能只证明新实现的正向样例可以运行。

V6A-19～V6A-22 还要求以下区分性门禁：从 Challenge 起点延迟或重放 Final Commit 不得
延长 Lease；Cookie 未回显时零 pending/零签名验证/零 Provider I/O，Cookie 洪泛受固定
per-Link 配额和 Owner budget 限制；Group HELLO 的 Peer/Group Context 互斥、保留目的地址、
Tag/Replay 和零权限副作用逐项测试；同 Frame Type 内替换为未授权 Protocol Opcode 必须在
任何业务或控制副作用前拒绝。所有失败路径都要对完整状态和输出做无写回比较。

V6A-23/V6A-24 还要求：Lease 验证端的慢钟、Timer Resolution 和两次读取误差必须全部进入
保守 Deadline；Timer Resolution 未知/为零、读取误差未知、非整 tick、真实已过期但量化
now 未跳变、扣减至零/下溢均失败关闭。Group Policy/Key Generation 必须分别覆盖静态和
动态 Owner、Authority 换主、重启/reload、分区合并、父域变化和耗尽；同一父域内任何
high-water 回退、旧 Key/Policy selector 复活或 Generation 回绕都必须在查 Key 和副作用前拒绝。

V6A-25 还要求：动态 Group ID 连续分配高水位、静态 Group 固定槽和 Group Key 固定槽必须
全部具有编译期容量。删除动态 Group 后下一 ID 仍为 high-water 的 checked-next；静态 Group/
Key 槽退休后永久占位；普通换钥保持 Key ID 并只推进 Key Generation。表满、Generation/ID
到达冻结阈值、持久记录损坏或 reload 回退必须零写 Fault，不得驱逐、稀疏回收、复用旧 ID，
也不得用固定摘要冒充历史成员查询。

## 22. v6 完成定义

只有同时满足以下条件，才可以将 v6 改称 UCN 1.0 RC：

- 单一 Core Wire 和单一 Cluster Target Wire；
- 所有 Profile 能解析 A0～A3；
- Identity、Bootstrap、Address Binding、Session、Route/Path 与 Group Policy/Key Generation
  无歧义且无回绕 ABA；
- 唯一 JOIN FSM、认证前 Cookie/固定资源、挑战相对 Lease Freshness、Peer Hop/Group/E2E
  Auth、canonical AAD、Replay、精确 Opcode ACL、Rotation 和撤销闭环；
- Capability、Path Frame MTU 与实际 Payload/Fragment Budget 在发送前失败关闭；
- 动态 Route、Pinned Path、主备和负载均衡共享统一 RouteSet；
- Q0～Q3、Delivery Guarantee、Interaction Role/Operation ID、Transfer、Credit 和多跳
  流水有固定资源与时延边界；
- 普通中继 Fast Path 不解析业务 Payload；
- Realtime 对普通业务零开销，Timed Endpoint 通过实机时间误差门禁；中继只使用显式
  Hop Scheduling Budget，不读取 E2E Envelope；
- Cluster 不成为 Core 依赖，Target FSM/持久化/Authority 通过掉电与分区验证；
- 至少一个 `ESP32-S3 + FreeRTOS + UART/RS-485 + ESP-NOW` 参考产品完整闭环；
- 若发布声明支持 CAN/CAN-FD/USB，对应实机门禁全部通过；
- 24 h 长稳、资源、功耗、故障注入和恢复通过；
- 外部审计关闭全部 P0/P1；
- Compatibility Removal Manifest 全部销账，旧源码/符号/CMake target denylist 归零；
- 不再存在“默认关闭但发布声称完成”的核心能力。

## 23. 当前边界

本文只是最终架构和破坏性重构基线。它没有：

- 修改 `UCN_PROTOCOL_VERSION`；
- 修改任何 Encoder/Decoder、Frame、Node、Transfer、Realtime 或 Cluster 源码；
- 建立 v6 Git 分支或 Tag；
- 擦除任何开发板 Flash；
- 宣称新 Wire、安全、吞吐、实机或掉电能力已实现。

V6-00 已在最终架构 RFC/纯文档范围完成外部终审。后续必须先满足 V6-01 的 V5-64 A06
可追溯独立外审与用户明确授权，再建立 v5 快照/Tag 和 v6 基线；不得绕过 V6-01 直接开始
V6-03。V6-03 完成也只表示隔离 Codec 可测试；生产 Encoder/Decoder 与 Node/Adapter
接线必须等待 V6-07 Security/JOIN 外部复审 GO。
