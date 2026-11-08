# 多邻居认证路由控制与局部小MTU合同

状态：实施中；本段为路由控制面合同与Host实现，不是整个RUST-SW-02完成或生产放行。
遵守MCU-first、固定容量、Coordinator唯一跨Owner编排；不操作实机。

## 1. 为什么不能从外层帧头恢复原始发现身份

在A→B→C寻路时，B向C发送的认证邻居帧，Source必须是B，密钥和H1也属于B/C。
原始发现者仍是A。如果C从该帧头重建发现键，就会把A的事务改成B的事务，RREP无法
沿原来的反向记录返回。因此外层和内层分开：外层认证当前邻居，正文保留发现因果键。
内层Principal只是经授权路由邻居转述的路由提示，不能替代业务端到端认证或Endpoint ACL。

唯一运行格式是本篇完整正文；原有9/29 B函数仅编解码叶子字段，运行时不回退接收它们。
普通A1 C1数据头保持13 B，不为本段增加字段。

叶子请求中的`minimum_frame_mtu`明确使用**完整UCN帧字节数**；旧名称
`minimum_payload_budget`已从C/Rust代码删除，不保留误导性的别名。业务入口必须先按
选定Contract/O/H计算帧开销：例如A1 C1/O1/H1发送100 B业务需要145 B帧，不能把100
直接当帧要求。改名不改变9 B叶子布局或任何既有Golden字节。

## 2. 共同精确正文

全部多字节字段为big-endian，地址在正文统一32 bit，仍校验当前地址宽度及保留值。

| 消息 | Opcode/Role | 精确正文 |
| --- | --- | --- |
| RREQ | 0201/Request | DiscoveryKey44 + remaining_hops8 + leaf9 = 54 B |
| RREP | 0202/Result | DiscoveryKey44 + leaf29 + reporter_local_route_generation32 = 77 B |
| RERR | 0203/Error | 既有完整RouteDomain/因果键/报告方/失败Link正文99 B |
| 局部分片 | 0206/OneWay | original_opcode16 + total16 + index8 + count8 + bundle64 + chunk |

DiscoveryKey字段按顺序：realm32、origin_address32、origin_binding_generation32、
origin_principal128、origin_route_scope32、target_address32、discovery_id64，共44 B。
当前实现字段名`origin_session_generation`对应源RouteOwner冻结的发现域，绝不能用当前
逐跳Peer Session代替；它也不证明最终业务Session。普通业务/Flow接线必须另行验证实际
端到端Security上下文，不能把SoftRoute提升为授权证据。

三种逻辑消息及包装统一C0/Q0/BestEffort/HopLimit=1/O1/H1。Q0是既有路由控制合同，
不改变6:3:2:1数据调度或给予邻居无限发送权限；固定队列、次数、期限和轮转限制工作。
未满足O1/H1时失败，不自动降级。包装与原始Opcode的精确方向ACL都必须通过。

## 3. RREQ与RREP如何工作

1. Coordinator读取实际Identity Authority/双方Binding、实际Security Session和实际
   Capability缓存，验证完整Realm、地址宽度、Runtime、Link及冻结快照；不接收认证布尔值。
   本地Capability精确冻结为本Root的共同上界；帧MTU取本地/远端Frame与Processing、
   实际Adapter和固定缓冲的最小值，能力位取双方交集，不只相信远端较大的声明。
2. 本机开始发现前预检所有出站Peer及队列，建立一次发现并逐邻居发送；同目标精确请求合并。
   不可用或不满足所需MTU/能力的邻居不参与扩散；一个失效邻居不能挡住其他合格邻居。
3. 中继认证外层邻居后才把完整RREQ交RouteOwner；排除入口邻居，减少正文remaining_hops，
   累加受检Cost/MTU/能力。返回本机原始发现键的环路RREQ直接拒绝，不安装到自己的回程路由。
4. 目标仅对精确活跃反向记录生成零跳RREP；正文目标Binding必须等于该认证邻居本身，
   reporter generation固定0。中继返回时使用自己新安装的非零本地Route Generation。
5. 返回中继先预检上游当前Peer、ACL和队列，再安装本地SoftRoute及反向转发义务。
   只有实际Driver终态及Token退休后，才完成这项反向义务。
6. 源收到精确当前发现的RREP才结束发现；迟到、跨域或没有反向记录的RREP不安装路由。

认证的是相邻路由器的转述，不是对最终目标功能、权限或可达性的永久证明。

## 4. RERR不能比较两台机器的本地编号

例如B的Route Generation=2，而A对应路由的Generation=1，这是合法的。Link ID和Link
Generation同样属于各自机器。接收RREP时冻结`完整RouteDomain + discovery causal ID +
实际下游Peer/Session/Link + 下游报告的Route Generation`回执。后续RERR必须精确命中
该回执；先验证真实认证报告方，再翻译成本机Route Generation及本机下一跳Link。

报告方的失败Link编号仅作其本地诊断，不与接收端编号数值比较。零跳目标回执没有下游
Route Generation，不能伪造一个非零RERR来删除它。中继向上游报告时，重新认证并填入
本机报告者Binding和本机对应Route Generation；绝不透传下游编号冒充自己的编号。
共享物理Link上一个逻辑Peer失效时，只删除该完整Peer LinkRef指向的动态路由，保留其他Peer。

## 5. 重试、组装与停止

每个逻辑路由尝试使用新的外层C0事务和H1序号，内层发现键/正文、原绝对期限不变。
明确`NOT_SUBMITTED`不属于新逻辑尝试：退休该次Adapter token后逐字节复用原保护帧，
不再分配Origin/C0/Hop计数。实际已发送而回复丢失后的逻辑重试才重新保护。
计数器由调用方按精确Session共享，不能在Capability→Route交接时从1重新计数。
提交不确定时不能发起下一尝试；已接受Token不因超时、停止或重开而消失。
反向记录的期限在首次RREQ接纳时固定；重复RREQ、迟到RREP或分片不能重新从接收时刻续期。
本地新控制提交的deadline不得晚于该精确反向记录原期限；已接纳的Unknown物理义务可以越期保留，
但不因此取得继续发送权限。C由Route锁内Action携带原期限，Rust沿用原Reverse句柄期限。

整条正文放不进帧才局部分片，RREQ必须整帧可容纳。只允许77 B RREP和99 B RERR，
2～4片，均分步长`ceil(total/count)`，单块最多50 B。精确计算：

```text
base = 25 + 2*address_bytes + 32      # C0 + O1 tag + H1 trailer
if base + total <= frame_mtu: direct
else:
    count = ceil(total / (frame_mtu - base - 14))
    require 2 <= count <= 4
    piece = ceil(total / count)
```

实现最低接受127 B帧；A1下RREQ115 B、两片RREP114/113 B、两片RERR125/124 B。
A3下127 B帧的RERR需三片，每片112 B。以上均含O1/H1；不含物理承载自身头、CRC或填充。
与大帧直接发送相比，分片增加独立帧头/Tag与14 B局部前缀，是控制交换的明确费用，
不改变普通数据头。不得因为省控制字节省掉认证或隐式接受半条命令。

每个已登记Peer只有一个固定99 B组装槽。第一片必须index0；bundle从共享C0计数器的
当前值取得，非零、不回绕，并且不得超过该片实际认证的C0事务。所有片精确绑定同一
实际身份/Realm/Session/Key/Policy/Fingerprint和原始Opcode、长度、片数、组键。
首片Deadline不刷新。实际同一认证上下文中，更高bundle的index0允许丢弃**全部**旧私有
字节/位图并前进高水位，但必须保留原首片Deadline；旧尾片不得混入，跨上下文和非首片
不允许替换。这些暂存字节没有Driver、RouteOwner或待提交Replay义务；不能类推为允许
抹除这些义务。超时清理继续保留bundle高水位。
只有完整且结构合法的正文才进入RouteOwner；迟到旧组不能借新外层nonce重新组装。

此限定替换用于丢首片/末片后的新一轮完整RREP重答：若一律禁止，旧半条会占到发现已过期，
绿灯的整帧重试测试也不能证明小MTU收敛。本段不为此增加额外ACK或每帧16 B摘要。

同一Peer的分片发送串行化，不能互相顶掉组装槽。单次drive/retire使用持久轮转游标。
空闲访问也保留可信时间高水位，后续不能用更早时间复用租约。容量按实际消息预检：
源端接收结束发现的RREP不需要新发送槽，不能因唯一槽仍由原RREQ占用而死锁。
业务到期只撤销可用性，反向转发的物理义务仍保留到精确退休；Unknown、重开后封存的
InDoubt均需真实迟到完成事实，重开本身不是退休证明。调用方必须保存Root至quiescent。

## 6. 实现与验收边界

- C/Rust：共同完整正文/分片Codec、独立三方raw向量；C认证typed分派已不再从外层猜原始身份，
  并原子绑定RREP报告方代际。C的分片Opcode目前仅登记/Codec，通用生产入口继续拒绝，
  不伪称C自动分片/多Peer Root完成。
- Rust：固定已知多Peer Coordinator；实际耐久Identity/Security、实际受保护Capability输入、
  原子Adapter帧及Driver提交/退休的三节点Host模拟，包含共享物理Link与127 B帧。
- C新增封存身份/专用Link产品中的可选认证控制Coordinator，可自动形成多跳业务路由；
  并非动态身份Admission或共享Link定位。详细范围及实现验收见[合同25](25-C动态发现控制与可选后端完整栈合同.md)。
- 后续产品化仍须验证C/Rust统一多跳业务交付、独立Origin/Hop上下文、完整
  Flow激活，以及与既有业务/Group/Cluster的唯一Ingress编排。不得用本段绿灯覆盖这些工作。
- 实机、生产密码Provider、Flash掉电、MCU栈/WCET、物理时间戳、独立外审均未放行。

## 7. 本段逐项自审门禁

| 小项 | 可区分错误实现的验收 |
| --- | --- |
| MC-01 | 原始Source=A、实际逐跳Source=B；C侧同一正文独立解码，不接受9/29 B旧完整消息 |
| MC-02 | 实际双Peer共物理Link；错Realm/父代际/Fingerprint/ACL/判别值/Tag拒绝 |
| MC-03 | 丢RREP后重试用新外层事务、相同内层发现；不重复消耗业务Authority |
| MC-04 | B的代际2/Link代际9与A的1/3不等，正确RERR仍精确失效；错因果不得删新路由 |
| MC-05 | 127 B双片/三片Codec；实际127 B三节点RREQ→RREP→RERR，不装半条路由 |
| MC-06 | 非首片换组/跨上下文拒绝；更高首片全量清旧片但不续期；两段返回链路各丢首/末片均可重试收敛；超时等值/清理后旧组拒绝 |
| MC-07 | NOT_SUBMITTED有限重试；Unknown停止、反向记录到期、重开及精确迟到退休 |
| MC-08 | Feature OFF、MSRV、Clippy、Cortex/ESP32仅编译、C多Profile及源码指纹稳定 |

这是软件自审清单，不由实现者自行签独立外审GO。
