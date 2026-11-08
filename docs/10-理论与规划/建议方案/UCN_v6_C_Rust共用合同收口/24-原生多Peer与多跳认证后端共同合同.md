# 原生多Peer与多跳认证后端共同合同

本合同增补[合同23](23-原生发布订阅认证后端共同合同.md)，不删除其单Session H0后端，也不把原签字扩大到本轮。

## 1. 固定资源与角色

一个数据Coordinator独占Adapter RX和物理退休；端到端Endpoint表与逐跳Neighbor表分离。
Endpoint绑定完整Principal/地址/Binding、Session及Service、Origin fingerprint、最低保护。
Neighbor绑定实际Link/Generation、精确物理来源/目的定位、当前Hop Session与ACL/Capability。
所有表编译期或const generic确定，满载拒绝，不驱逐未退休义务。新登记不得覆盖原代际。

首个C工厂的实现子集是**封存Static身份/能力配置、强制绝对装配到期、每邻居独占物理Link**。
它不声称接入可变Identity/Parent/Capability Owner：配置在Root存活期间不得变化，变更先围栏/排空。
当前Security Session/ACL/期限和真实Link代际仍逐次检查。Rust接已有RoutedNode，可复验实际动态
Identity父域及认证Capability。两侧安全底线相同，但工厂支持集合不同，不能据此宣称C动态准入完成。

C首版还有两项保守装配限制：每个Peer最多登记一个Service Endpoint；同Service登记的全部
Endpoint都必须满足使用该Service的Channel最低保护，即便某Endpoint未被该Channel引用。
因此混合O1/O2 Peer时，不能据此宣称C可建立只引用O2子集的Encrypted Channel。
Rust已有Endpoint支持同Session不同Service，按Channel引用集合核验；这两项不是共同Wire限制。
未来放宽C工厂时须保持同地址完整Binding/Session/标签一致、拒绝重复(Peer,Service)，并增加
共享Origin计数器及“未引用低保护Peer不阻断、实际引用则拒绝”的区分性回归，不得直接删除预检。

Native Channel继续唯一Service、固定Schema与长度，多个Peer可以共享同Channel/Service。
本轮不新增动态Topic/Schema、RPC、Reliable、Transfer、Group或Timed语义。

## 2. 路由与安全

源端先解析既有Route Owner；Origin以终点Session保护，Hop以实际下一跳Session认证。
中继只验证当前邻居Hop、查精确Origin/Destination路由、递减HopLimit并重做下一跳Hop Tag。
终点须同时满足入站Hop与Origin认证/Replay及当前Endpoint ACL，不能将中继Principal投影为原始Source。
Origin密文字节和原始身份不因中继变化重建；禁止O0或没有已验证Hop的绕过。
Route scope由Route Owner管理，不能拿业务Security Session generation填充发现域；两种代际允许不同。

无Route保持有界等待或明确失败，不刷新原Deadline，不临时换成未认证路径。数据后端不另建路由控制FSM。
Rust已有RoutedNode负责自动认证发现；C若仅接消费现有Route Owner的静态/已形成动态路由，必须如此报告，
不能用测试手工安装路由宣称C控制面自动发现闭环。
本轮[合同25](25-C动态发现控制与可选后端完整栈合同.md)新增可选C控制装配：`control=NULL`保持原数据消费子集，
装配固定control存储及共享计数银行后才启用真实C0发现；它不改变上述封存身份/专用Link限制。

## 3. Scope、接收时间与分发

Scope精确绑定Coordinator实例和Endpoint槽代际，不是仅用Peer地址或digest。
Source表里的digest仍只是配置标签；真实授权与回调Principal来自完整当前Security/Identity事实。
队列保留原物理RX时间；分发寿命不得从应用poll时间重算。每个订阅回调前重新核验当前Endpoint、
Session/Parent/ACL及保护。中继Hop授权丢失阻止新入站；已验证终点样本不伪造来源。
每个Origin的Latest高水位相互独立。首版不热换Endpoint；后继注册必须新Scope并遵循真实围栏/排空，
不得在相同Native来源槽上以清零高水位复活旧样本。
本地样本仍为LOCAL、零/None Principal；只有完整当前出站ACL及适用的长度/类别/保护策略满足时才交付。

## 4. 发送与退休

publish只代表本地复制。fanout逐目标独立，不承诺网络原子性。
原Deadline跨发现/排队/重试不延长；真实Driver背压不重复分配同一业务的Origin sequence。
这是源端本地Deadline，不是Wire携带的全网绝对时间。中继只能以原物理RX时间加本机有界转发生命期，
再受当前装配/Session/Route期限限制；不能在每次重试或应用poll时续期。Rust运行时的产品上限只能缩短源端原期限。
Origin、Hop计数器均借用原Owner，不能为新Backend从1重新创建。
Stop后禁止新接纳、关闭RX、丢弃未执行业务、持续真实退休。Unknown必须保留精确物理Holder，
不能因逻辑到期或Root释放归零；所有Root/Driver借用在真实排空前持续有效。

## 5. 费用与验收

C1普通头仍为11/13/15/17B。O1/O2原有Origin Tag16B，H1既有Hop Trailer16B，不是新增普通头。
H1帧总长度=`header + payload + 16 + 16`；选择已有H0直连后端时不附带H1费用。
不得以隐去Tag换取“低开销”，也不因多Peer把完整Principal重复放入每帧头。

正式软件验收须有多Peer同Service、真实A-B-C Origin/Hop独立、错来源/Tag/Replay/TTL、原RX时间、
到期/撤权、短MTU、容量满、背压/Unknown/stop及可选边界回归。Host Provider替身不证明生产算法，
实机/Flash/Witness/功耗性能仍不在本轮范围。

## 6. 独立共同Hop AAD向量

C实际`c1_hop_wrap`工作区与Rust实际`build_hop_aad`分别匹配同一66 B字面向量，不比较两个测试
Provider的假密码算法。输入是31 B已保护Origin、Hop sequence1、fingerprint为`51..60`：

```text
55 43 4e 36 2d 48 4f 50 2d 56 31 00 00 00 33
61 40 43 01 04 12 01 00 00 00 01 01 02 03 04
00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00 00 00 01 51 52 53 54 55 56 57 58 59 5a 5b 5c 5d 5e 5f 60
```

Domain/length都是canonical认证输入，不是新增业务头。对端身份、Key/Suite、Owner计数器仍由既有安全合同约束。
