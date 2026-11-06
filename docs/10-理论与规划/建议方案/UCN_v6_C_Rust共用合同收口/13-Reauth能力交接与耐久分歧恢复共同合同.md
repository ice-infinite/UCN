# Reauth 能力交接与耐久分歧恢复共同合同

## 1. 范围和权威关系

日期：2026-10-15。接续[第12篇的历史切片](12-Peer-Reauth邻接重新认证共同合同.md)，
本篇及共同 JSON 的 `peer_reauth` 是当前 Reauth V2 合同。第12篇的156 B/V1、固定三片、
严格共同前驱限制只描述上一轮，不能与本篇拼装成另一种实现。运行时不保留V1兼容路径。

本轮接通两项固定已知Peer的Host软件能力：

1. Reauth成功后自动进入当前Session上的认证Capability协商；收到最后ACCEPT不可靠，
   因此交接不能销毁它的重答缓存或物理Token。
2. 两端Session耐久高水位不同后，以双方新的身份签名约定共同后继，不回退任何一端历史。

C/Rust同步精确Record/片Codec，三方Oracle逐字节比较。实际Identity、Foundation、
Security、Adapter及Capability的组合Root在Rust；**C自动Root和跨语言Runtime互通未完成**。
本篇不是外审GO、全协议完成、实机或生产放行。MCU-first、固定容量、非阻塞、Host无
特权原则不变；Obsidian旧笔记只用于原则核对，不覆盖当前源码。

## 2. 为什么必须携带两个历史

假设旧共同Session为1：Initiator把Session2写入并回读，Responder还没有提交便中断。
重建后的真实基线是`(2,1)`。要求共同前驱会永远拒绝；把2回退到1会重用Session/Key域；
把另一端声称的“已提交”当成本地Proof，又会绕过本地Flash门禁。

正确过程是：

```text
local_base := actual Foundation READY body + verified digest
HELLO := identity_sign(initiator exact durable history, fresh nonce A, current bindings/link)
CHALLENGE := identity_sign(HELLO domain + responder exact durable history + fresh nonce B/link)
for each of Session, OriginKeyGeneration, HopKeyGeneration:
    successor := checked_add(max(initiator_history, responder_history), 1)
require exact local history == local_base.history
require both identity signatures over the same final domain
prepare narrow Reauth requirement
Coordinator submits requirement to Foundation
wait exact local reload proof, activate local Session
publish RESPONSE / ACCEPT in their original durable order
```

远端历史是被该Principal认证的声明，不是远端Flash物理证明，也不是本端Proof。它只能
使共同后继更大，不能降低本端高水位或绕过本地Foundation。对端若不配合，保证安全拒绝，
不保证可用性。相同Session但摘要/Key历史不同视为分叉，拒绝自动合并；较大Session的
Origin/Hop历史必须也严格较大。这个范围不包含任意分区历史修复、地址变化或重新授权。

## 3. 当前208 B正文和签名

沿用Opcode `0010/0011/0012/0014`，Stage为0/1/2/3，Role为Request/Result/Request/Result。
外层仍为已绑定地址的C0/O0/H0、Control/Q2、BestEffort、HopLimit1，禁止普通转发。
整个逻辑消息必须验证身份签名；Driver只提供未认证的物理事实。

所有整数大端，固定208 B，不接收旧版本或尾部多余字节：

| Offset | Bytes | 当前字段与约束 |
| --- | --- | --- |
| 0 | 1 | version=2 |
| 1 | 1 | Stage0..3，绑定外层Opcode/Role |
| 2 | 1 | AddressWidth编码1/2/3/4，各地址仍必须适合该Realm |
| 3 | 1 | reserved=0 |
| 4 | 4 | Realm |
| 8 | 24 | Initiator address32、Binding32、Principal16 |
| 32 | 24 | Responder address32、Binding32、Principal16 |
| 56 | 4 | Initiator真实前驱Session |
| 60 | 4 | `max(双方前驱Session)+1`，checked |
| 64 | 1 | Origin Suite，与既有定向槽策略相同 |
| 65 | 1 | Hop Suite=16 |
| 66 | 2 | Origin正向Key ID，反向=base+1 |
| 68 | 2 | Hop正向Key ID，反向=base+1 |
| 70 | 2 | reserved=0 |
| 72 | 4 | `max(双方Origin前驱)+1`，checked |
| 76 | 4 | `max(双方Hop前驱)+1`，checked |
| 80 | 2 | Initiator本地Link ID |
| 82 | 2 | Responder本地Link ID |
| 84 | 4 | Initiator本地Link Generation |
| 88 | 4 | Responder本地Link Generation |
| 92 | 8 | Initiator事务ID，非零，不重起算 |
| 100 | 8 | Initiator产品新鲜Nonce |
| 108 | 8 | Responder产品新鲜Nonce |
| 116 | 8 | Session最长时长/us，不是父租约续期 |
| 124 | 32 | Initiator前驱完整transcript digest |
| 156 | 4 | Initiator前驱Origin Key Generation |
| 160 | 4 | Initiator前驱Hop Key Generation |
| 164 | 4 | Responder前驱Session |
| 168 | 4 | Responder前驱Origin Key Generation |
| 172 | 4 | Responder前驱Hop Key Generation |
| 176 | 32 | Responder前驱完整transcript digest |

HELLO的Responder Link、Nonce及最后44 B历史为零；后续必须完整非零。HELLO阶段的建议
后继暂按Initiator历史+1，CHALLENGE加入Responder历史后确定最终共同后继；从此逐字段
冻结。不能要求Responder把HELLO建议值当成本端耐久值，也不能在RESPONSE/ACCEPT时换历史。

逻辑消息为`record208 || proof_length:u8 || exact_identity_signature`，签名输入精确为
`ASCII("UCN6-PEER-REAUTH-V2") || record208`。产品固定身份算法、每Principal唯一Key解析
及签名长度16..64 B。没有尝试旧Key/算法的fallback。Security还需验证同一最终域上的
`challenge_signature || response_signature`，分别验证两端Principal，并执行产品Session
证明策略；不能仅由Runtime说“已验证”便放行跳代。

最终Context为`SHA256(domain || record208(stage=2))`，前16 B为Origin Fingerprint，
后16 B为Hop Fingerprint。普通静态Session API仍只接受本地前驱+1；仅专用Reauth入口在
双证明及本端基线精确匹配后可接受共同最大值+1。Factory域不能借此跳过JOIN。

## 4. 物理分片、时间和费用

沿用18 B局部前缀：`version=1 / stage / index / count / total16 / offset16 /
chunk=80 / reserved=0 / transaction64`。这是片格式版本，不是Record版本。
逻辑长度225..273 B，按80 B切为3或4片；count、offset、末片长度都由总长唯一决定，
禁止额外填充、跨事务混拼。局部片本身没有身份认证，完整到齐验证签名后才推进。

同一阶段缓存所有签名字节，重试不重新签名、不改Nonce、事务、历史、绝对Deadline。
物理提交总预算计入NOT_SUBMITTED，每次提交一个真实Token，真正退休后才发下一片。
UNKNOWN、取消失败和Foundation Pending均保留原Holder，不能清零伪造静止。

本地constructor锁存原start；`start <= trusted_now`，不从迟到消息重建起点。
Session截止为`min(original_start+duration, 双Binding截止, Authority截止)`，握手另受
原TTL及Foundation截止限制，全部checked且半开。重建后旧耐久正文不是活Session/租约；
Identity必须独立重新证明存活。本轮媒体恢复测试保留独立live Identity，不声称重启恢复
地址租约或整个设备自动成网。

| A1、无丢包 | 逻辑消息 | 每消息物理帧 | 四消息总字节 |
| --- | --- | --- | --- |
| 签名16 B | 225 B | 127+127+112 B | 1464 B |
| 签名32 B | 241 B | 127+127+127+48 B | 1716 B |
| 签名64 B | 273 B | 127+127+127+80 B | 1844 B |

以上含C0头29 B和每片18 B，不含Bearer、重传和后续Capability。典型32 B签名相比上段
增加396 B/次重新认证；原因是两份历史和每消息多一片，不加到普通数据上。小MTU后续
Capability典型899 B，合计控制交换2615 B。**普通C1 Data头仍13 B，新增0 B。**

## 5. 自动交接但不销毁最终确认缓存

产品先调用`enable_capability(ReauthCapabilityConfig)`显式配置本端能力、要求、服务域、
重试/租期；不是继承旧ACL。Reauth的角色需符合既有定向Key槽，当前固定身份对不能在
重新认证时交换角色或顺带换Suite/槽。真正创建Capability Root必须等待：

- 本端Established、精确Foundation Proof已消费；Initiator还必须已验证ACCEPT。
- Reauth实际物理Token为空；当前双Binding、Authority、Link、Session仍成立。
- 真实MTU、资源、配置及当前ACL预检成功，才发布新的Capability实例。

Responder发送完ACCEPT的最后一片且Token已退休，只说明本端完成发布，不说明对端收到。
因此同一个Coordinator保留Reauth缓存，同时开启Capability lane：

```text
receive_one():
    validate current Identity pair and local Session
    claim one actual atomic Adapter frame+metadata+token
    validate Link/timestamp/physical-peer hint; retire that exact raw token
    if opcode is Reauth:
        require original handshake deadline/budget and exact frozen signed transaction
        rearm cached ACCEPT only; never republish a durable Session
    else:
        route privately to Capability's real Security/Replay/ACL admission
        only successful authenticated admission can mark peer_session_observed

drive_one():
    if established and no local physical holder: preflight/create Capability once
    rotate Reauth/Capability lane persistently
    if shared Adapter TX slot occupied: wait for actual retirement
    submit at most one physical frame
```

没有合成RX、重入队原始帧或伪造`OpenedPacket/PacketEvidence`。分派Opcode在认证前只
用于选择验证器，不授予权限。收到当前Peer的真实认证Capability事实后才停止ACCEPT
重答；此前丢ACCEPT时Initiator重试RESPONSE，Responder仍可回复相同缓存。达到原握手
Deadline/重试上限也停止重答，但不撤销已经耐久的本地Session。

发送和退休都有独立持续轮转游标。一个物理TX槽足够运行，但两条lane必须等Token真正
退休，不能把NO_SPACE当作可以丢掉缓存的理由。停止仍统一禁用原RX、围栏本端新Session、
停止Capability并分别退休义务。Link已重开时，旧Root的stop在任何lane变更前拒绝；
它不能关闭新Link，也不能声称已静止，产品需要按新Link生命周期处置。

`selected_capability()`每次通过原Identity只读借用复验**双方完整Binding View和Authority**，
再检查Session、Link、Key/Policy和能力租期。外部不能取得内部Capability可变对象绕过
这些父域检查。Capability确认、业务投递成功、两端原子提交是三件不同的事。

## 6. API/SPI与裁剪边界

| 层 | 当前职责 |
| --- | --- |
| C/Rust Codec | 严格208 B Record、225..273 B片几何、负向输入零写入；不是认证View |
| Rust Security | 专用双身份证明/本端历史检查，产生不可变耐久Requirement；不直接调用其他Owner |
| Rust Coordinator | 路由Identity、Security、Foundation、Adapter与Capability事件；持有原始起点/物理义务 |
| 产品Provider | 身份密钥/撤销、真实新Key材料、生产密码、可信计时和实际Flash/Witness |

运行入口为`PeerReauthRuntime`的`start/receive/persist/finish_durability`；启用自动交接后
统一使用`receive_with_capability/drive_with_capability/retire_with_capability`，最后
`selected_capability`读取当前结果。驱动不能把构造函数、签名或介质PENDING放进忙等循环。

`peer-reauth`仍依赖Bootstrap基座复用IO；`capability-control`关闭时没有组合Root字段。
Host测得128 B实例：Reauth-only 2304 B；同时编译Capability-control时6840 B、alignment8。
后者即使尚未arm也预留Option内嵌存储，不隐藏RAM成本；未测MCU最坏栈或资源水位。
最小Core不依赖Reauth，也不依赖Linux、Group、Cluster或Realtime。

## 7. 自审和仍待事项

正式测试含双端真实Identity发布、双槽/Marker/Witness恢复、fresh Security Owner、两端
不同的历史和新鲜签名，再通过实际Adapter通信。覆盖`2/1→3`、`1/2→3`、`3/1→4`、
恢复后自动Capability、丢ACCEPT时两lane重叠、错ACL、Unknown、重入、父域到期及Link重开。
普通静态跳代、双签名任一损坏、本端历史错误、future start均另作反例。

详见[逐项与整体自审报告](../../../08-实现与验证/版本演进/UCN_V6S_RUST_SW_02_Reauth能力交接与耐久分歧恢复自审报告.md)。
仍待多Peer共享Root、C自动Root/跨语言运行时互通、动态Route/Flow，以及独立外审。
真实Flash原子性/掉电、物理Witness、生产密码及六板实测仍未证明，本轮不操作硬件。
