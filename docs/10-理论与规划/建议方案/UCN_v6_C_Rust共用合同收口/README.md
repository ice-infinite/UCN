# C/Rust 共用合同收口与 Rust 软件闭环

> 用户授权：2026-10-13，允许统一 C/Rust 共用合同后继续 Rust，实机另行通知。
> 当前为实施/自审候选，不冒充已获外部审计冻结。

本轮C动态Native接线见[合同26](26-C动态接入原生后端与受控热续接合同.md)：真实首次JOIN、
耐久Session、认证Capability及同Root热更新。显式区分原地Profile重协商、Rust固定借用
Native Backend和默认Nano动态Unsupported边界，不把合同统一写成所有高级工厂完成度相同。

## 1. 权威与不变边界

`protocol/contracts/v6-shared-v1.json` 登记机器可核对的标量、长度与执行位；本目录冻结其
语义。原 Core Wire C0～C5、O/H 与 canonical AAD 规则继续适用。两种语言服从同一合同，
不保留“Rust 专用协议”或对旧实现自动 fallback。普通 One-Way Data 不增加字段。

任何声明 DONE 都要同时给出生产入口、Owner、Codec、失败路径、测试和不支持项。只有
枚举/Registry、能编译的 DTO 或模型回归不足以证明端到端功能完成。

## 2. 执行小项

| ID | 内容 | 自审与放行门 |
| --- | --- | --- |
| SH-01 | Flags、标量范围、Setup/响应、Operation Body 与规范 digest | C/Rust/Python 实际 Codec Oracle、逐字段 Negative、输出零写 |
| SH-02 | Parent/Transfer/Service 与原子控制分派 | 认证/ACL/资源预检先于 Replay commit；重复不得执行；失败不丢义务 |
| SH-03 | Realm Group allocator 与逐 Context Record | 两语言同字节；有限槽；ID 先持久化；重启、损坏、耗尽不复用 |
| SH-04 | Group/Tree/C5/H3 与 Realtime 完整控制事务 | 唯一 Sender/Key/Tree Context，真实 Fake TimedLink、释放重试 |
| SH-05 | Cluster 完整票证书/Delta/移交/恢复/目录边界 | 持久化前不承诺，use-time 重算，历史票不复活、旧 Authority 不恢复 |
| SH-06 | 统一动态 Coordinator 与 Host 集成 | 各 Owner 不互链，实际 Security/Adapter/Provider/Facts 路径 |
| SH-07 | 全体自审与矩阵、原始候选、硬件前置计划 | 每小项回归后两轮全体回读，Profile/Feature/no_std/目标编译与资源证据 |

这是一张工作表，不自动宣布所有小项已完成。结果逐项记录在项目操作台账。

## 3. Flags 与条件 Envelope

| bit | 值/含义 |
| --- | --- |
| 1..0 | 0=REPEATABLE；1=VOLATILE_DEDUP；2=DURABLE_AT_MOST_ONCE；3 保留 |
| 2 | FINAL，Result/Error 必须置位，Request 必须为零 |
| 7..3 | 保留零，任何非零拒绝 |

Request 固定 `OperationId(u64) + Flags(u8)` 为 9 B；Result/Error 加结果码 u16，为 11 B。
One-Way 不携带这些字节。该 Envelope 只用于应用 Data RPC；协议 Control/Diagnostic
Body 按各自 Registry 承载事务键，不再额外添加一个应用 Operation Envelope。
Transfer 在 Setup 中承载相同规则，不再把它们加到每片。
Result 的低两位必须与原 Request 模式精确相同；不能用 FINAL 让一个未完成 Operation 变终态。
FINAL 只代表消息角色，不是 Flash durable proof。普通请求不写 Flash；耐久模式必须有 Journal。

## 4. Control Identity 与 Profile

C0 的 transaction 是该发送方在当前 Origin Key/Session 域中的**控制报文身份**。
每个新 Opcode/phase 报文必须使用新的 transaction；精确重发保留原密文、transaction 与 Tag。
它与 C1/C2/C4/C5 Origin Sequence 分属不同命名空间，不能混用一个 Replay bitmap。

多阶段业务事务通过 Body 的 Parent Generation、完整 Proposal/Setup digest、Operation-ID、
Epoch/Config/投票身份等精确键关联，不要求不同 Opcode 复用同一外层 transaction。反向 ACK
也使用发送方自己的新控制身份，以 Body 指向原业务。Bootstrap 的逻辑 JOIN txid 在认证
transcript 中关联，不能拿它重置已建立 Session 的报文计数器。

所有授权控制必须 O1/O2＋精确 ACL；Hop 由本地安装 Context 唯一决定，不能失败后试另一 Key。
业务 Source/目的/Realm/Binding 来自已认证 Header/Context，Body 不复制这些长期身份字段。

## 5. 规范 digest

`SHA256(domain_ascii_no_nul || u32be(length) || exact_bytes)[0..16]`，唯一 Suite 1：

| 域字符串 | 输入 |
| --- | --- |
| UCN6-AAD-V1 | 完整 canonical AAD |
| UCN6-PAYLOAD-V1 | 认证后的 Origin 明文 Payload，包括实际存在的语义 Envelope |
| UCN6-TRANSFER-MESSAGE-V1 | 完整纯业务消息，不含逐片前缀 |
| UCN6-TRANSFER-SETUP-V1 | 精确 43/52/54 B Setup |
| UCN6-OPERATION-RESULT-V1 | 完整Operation Wire Key、观察phase、status、正文长度与精确结果正文 |

不使用本地结构体内存、padding、裸 SHA 或 Foundation 的 BLAKE2s 摘要替代。
Foundation 的 Record Meta/Body integrity digest 保持自己的既有合同，不因此改成 Wire digest。

## 6. Transfer 与 Operation Control Body

Transfer 响应都有 29 B 完整键：Kind1、ParentId2、Generation4、TransferId4、Service2、
SetupDigest16。ACK 再加 fragment budget2/credit2（33 B）；Abort 加非零 reason1/zero3
（33 B）；Terminal 加 state1/result2（32 B）。Terminal state：1 Delivered、2 Aborted、
3 Failed；其他值拒绝。分片上限的 Wire 是 32767，Owner 的 Profile 容量另行检查，不混为一谈。
duration 必须 `1..INT32_MAX ms`；到达 deadline 即过期，重复不续租。

Operation Query 固定键28 B：Operation8、Service2、业务 Opcode2、RequestDigest16；Receipt
再加 ResultDigest16、durable observed phase1、zero3（48 B）；Cancel 加 reason1/zero3（32 B）。
Receipt phase 仅3/4/5/6，Cancel reason仅1/2/3/4。取消请求不证明外部副作用已撤销；IN_DOUBT
只能查询对账，不得据收到 Cancel 再次执行。

结果摘要唯一采用 `UCN6-OPERATION-RESULT-V1`：规范 Body 为 `OperationWireKey28 ||
phase:u8 || status:i32be || payload_length:u16be || exact_payload`，固定部分35 B；按第5节的
domain + u32be长度 + SHA256/128规则计算。只接受phase3/4/5/6，只有3能携带正文。
此摘要不是Foundation Record摘要；native C/Rust/Python独立向量核对，不增加Receipt字段。

## 7. Group 持久化方向

删除 Rust-only GRC1 catalog 作为正式实现的方向。采用单独 Realm allocator 高水位 Record，
以及每个固定 Manifest slot 的有界 Context Record。这样 Full 不必把所有 Group 的成员表
塞进一个超过 Profile Body 上限的巨型 Record；删除 Context 不回收历史 ID。

allocator 先 durable/reload 后签发线性 Group-ID permit；再写 Context，失败只烧掉 ID。
Record 不保存易失 Runtime/Owner/slot Handle，也不因读取到旧 Key/Tree 字段就恢复权限。
恢复先验证全部固定域，再根据当前认证 Security/Tree/Realm Authority 重建 Handle，最后发布。
Key/Generation/ID 耗尽不回绕；未知或损坏 Witness 不回退旧记录。

共同 Context Schema 为 `0x0802/version=2`，长度 `64 + 28 × Manifest member capacity`。
Nano/Lite/Full 分别为 176/288/512 B，低于各自 256/512/1024 B Foundation 上限。
不再保留旧 `version=1` 解码兼容，旧 GRC1 目录代码已移除。

| offset | 长度 | 内容 |
| ---: | ---: | --- |
| 0 | 2 | schema version=2 |
| 2 | 1 | durable phase：2 ACTIVE logical record，4 RETIRED；reload 不恢复 ACTIVE 权限 |
| 3 | 1 | mode=2 DYNAMIC |
| 4 / 8 / 12 | 各4 | Realm / Group ID / Group Generation |
| 16 / 20 | 各4 | Policy / Member Generation |
| 24 / 26 | 各2 | Service / application Opcode |
| 28 / 29 / 30 / 31 | 各1 | member count / fanout limit / quorum weight / allowed scope mask |
| 32 / 33 / 34 / 35 | 各1 | secure required=1 / public static=0 / exact Principal=1 / Tree present |
| 36 / 40 / 44 | 各4 | Key Generation / Tree Generation / issued allocator high-water stamp |
| 48 | 16 | logical Security Context digest，非易失 Handle |
| 64+28i | 28 | Address4 / Binding4 / Principal16 / Sender Slot2 / Weight1 / zero1 |

未使用成员槽全零，成员按 Principal 无符号字典序严格递增；正文不包含 Runtime、Owner、
槽 Handle、物理 Link generation 或旧路由句柄。恢复只发布逻辑 View，必须 Fenced。

单独 allocator Schema=`0x0804/version=1`，正文16 B：`GID1 + Realm4 + high_water4 +
Authority Generation4`。同 Realm 只有一个逻辑 allocator，Domain ID 固定为
`(u64(realm)<<32)|0xFFFFFFFE`，由唯一 Coordinator/Manifest 装配，不能创建两个相同
Owner/Domain 实例。首次空域只允许 Realm Authority Generation=1；换主必须迁移/验证原
Witness 和高水位，不允许当作空 Factory Realm。Authority 代际不得低于已恢复高水位。

Rust 已用独立 Foundation 域实现 allocator 与逐槽 Context 消费者；C 已实现两类共同 Codec
及逐 Context v2 import/fence，但 C 的 admin→单独 allocator proof 编排仍需下一段接线。
不能把“Record 字节一致”误写为“C/Rust 全部 Group 控制路径已经互通”。

## 8. 当前 Control ingress 的语义范围

Rust `ControlCoordinator` 已经通过实际 Security verify/ACL 接纳 Delivery ACK、Transfer
Setup/ACK/Abort/Terminal/SACK、Operation Query/Receipt/Cancel、Time 四报文正文。
其中C0只允许完整20B C1 SACK；14B Flow SACK必须由已安装的成对反向Flow Header
提供隐含Parent，禁止放进没有Flow Context的C0来猜测匹配。
只有严格正文验证与固定队列容量均通过后才提交 Replay。队列中的 `ControlCommand` 不可由外部
构造，携带实际认证来源，且不是 durable proof、Owner mutation 完成证明或回复已送达证明。

当前该入口不是完整业务 dispatcher：Coordinator 还必须按完整 Parent/Operation/Time
Domain 精确匹配目标 Owner，保留尚未完成的 Provider/Driver/回复义务，成功后再退休命令。
必须补重复请求的缓存回复、回复丢失重发，以及动态 Origin/Hop 双 Context；不得先消费
命令再丢失其业务或物理 obligation。该软件工作未完成前，`RUST-SW-01/02` 不标 DONE。

2026-10-14，Rust 已补入 C1 Parent 的 Setup 与反向 ACK/SACK/Abort/Terminal 分派，以及
固定缓存 Setup 回复的实际 Security→Adapter 生命周期。精确重复查询缓存，不再次接纳
Setup；时限从认证接纳计时，物理 Token 必须退休成功才清除。

同日第二段已接通受保护 C1/OneWay 分片的实际认证、线性预检、固定重组与一次 Buffer
交付。先预留累计 SACK 再提交 Replay/分片；Complete 不产生业务成功，实际交付前必须
复核当前 ACL 并预留 Terminal。SACK 是可显式退休的临时通知，从保留证据再生成时使用
新 C0 transaction；保留中的回复及 Terminal 重试使用原字节，不能混淆两个窗口。
停止/丢弃逻辑 RX 不删除独立物理 Token。详见
[第二段自审报告](../../../08-实现与验证/版本演进/UCN_V6S_RUST_SW_01_认证分片重组与业务交付自审报告.md)。

范围仍为已发布 C1 Parent/H0 直达；Parent 发布协议、Service/Operation/Time、发送端
自主调度与动态/Flow 管线仍需接线，不标整个 RUST-SW-01 完成。

第三段接通C0/H0直达Operation Query/Cancel/Receipt观察：取消回复先进入AwaitCommit，
真实Journal reload与Proof退休后再核对当前权限并解锁；已交接/恢复Executing只允许
InDoubt，不冒充无副作用。失败请求精确退休前围栏Journal，退休不恢复执行权限。
Watch只保留认证时点观察，不能代替RPC Result正文、执行器对账或GC确认。详见
[Operation分派与耐久取消报告](../../../08-实现与验证/版本演进/UCN_V6S_RUST_SW_01_Operation控制分派与耐久取消自审报告.md)。
第三段结束时Service原始Request/Result、Parent发布、Time及动态/Flow尚未完成；
第四段的有限Service接线范围见第10节，不能回溯宣称第三段已完成RPC。

## 9. 复核范围与普通报文开销

普通 Common Header 仍3 B，C1/既有 Contract 字段和 Tag 长度没有增加。规范 digest、
nonce direction 和 Record 改动不增加普通传感器 Payload 的附加字段；One-Way 不加 RPC
Envelope，Time/Group/Transfer 状态只存在于相应可选模块。

C 的 Storage Layout 更新为4，初始化用新 Manifest/Layout 拒绝旧 storage 组合。新候选
只能标 SELF-REVIEW；此前冻结文档、旧 Schema 或旧工作树的外审签字不能自动覆盖本轮改动。

## 10. 直达 Data RPC 的第四段共同合同与实现范围

Data 没有 Protocol Opcode，ACL 的规范 code 固定0；不能为了RPC再加一个2 B Opcode。
Request Payload 为 `Operation:u64be || Flags:u8 || business`，Result/Error 为
`Operation:u64be || Flags:u8 || ResultCode:u16be || business`；它们已经属于条件
9/11 B Envelope。普通OneWay没有Envelope，Common/C1 Header与Tag均未变大。

第四段结束时，Rust `service-rpc`只接通C1、H0直达、BestEffort、Q2、受保护O1/O2的
VOLATILE_DEDUP（Request Flags=1，Result/Error Flags=5）。其他合法Wire组合、Repeatable、
Durable、可靠RPC、Transfer RPC、Flow/多跳本段明确不支持，不静默降级。
O1经过Host HMAC测试Provider的实际验证；O2复用已有Security Provider接口，
本段没有新的O2加解密端到端或生产密码证据。

Product必须安装额外的RpcEndpoint权限：固定Service、Peer Principal、可接纳Request/Result、
业务容量和最低安全等级。普通Data ACL不自动允许业务回调。所有跨Owner编排只在Runtime
Coordinator，Service不链接Security/Adapter/Persistence，也不直接调用Driver。

易失模式Operation ID取第一次请求的唯一Origin Sequence，扩展成u64；重试保留同一ID、
原字节/序号/Tag。Source必须由当前Session下唯一的OriginSequenceOwner分配，不得复位
或换钥后从小值重来；掉电重启必须完成已有的Session代际更新，不冒充耐久业务去重。
Server另保留固定64位Operation高水位窗口：缓存退休、同Session换Key/Policy不清除历史；
已见Operation换新报文序号也不能再次Invoke。窗口外旧ID拒绝；新Session只在精确相同
Realm/双端Principal/Address/Binding父域下重建，旧Session拒绝。动态Binding迁移归后续。

ServiceKey完整保存在本地，不重复塞进每帧：Realm、两端身份/地址/绑定、Session、
原请求Key、Policy、Fingerprint、Service和Operation。反向Result须精确匹配原请求域，
反向密码Key由当前唯一Session Context解析。结果码用已有Role区分：Result映射为
`i32(code)`，Error映射为`-1-i32(code)`，范围分别0..65535和-1..-65536；线上仍u16。

Server认证后先预留业务结果槽和最大允许回复报文容量，线性预检成功再提交Replay并发布
Invoke；预检丢弃不写Service。相同请求得到Pending/Replay，不复制业务正文、不再次调用。
业务respond只冻结一次结果，seal失败保留AwaitSeal；NOT_SUBMITTED只退休本次Token，
有界重发保留原请求/回复截止。Driver完成不等于Service完成；stop/超时不抹掉物理Holder。
Unknown由Adapter围栏，只有准确物理终态才能退休，不能据逻辑超时回收Buffer。

三组新增native C测试fixture、Rust Envelope encoder与Python struct一致，只证明共同
9/11 B字节合同，不证明C生产Runtime已接通保护Data RPC或C↔Rust全运行时互通。
详见[第四段逐项与整体自审报告](../../../08-实现与验证/版本演进/UCN_V6S_RUST_SW_01_Service原始请求响应与易失去重自审报告.md)。

## 11. 第五段：耐久 Data 请求和执行交接

`durable-rpc`是独立可裁剪装配，普通`service-rpc`不额外启用Operation Journal。
Request Flags=2，Result/Error Flags=6，仍用原9/11 B Envelope，Data opcode仍0。
Client的Operation ID来自确认回读后的OID1耐久区间，与Origin Sequence独立；失败预检
丢弃线性ID reservation，不消耗逻辑ID。已消费/重启烧掉的ID不能回收复用。
Product必须为同一发起Principal的ID命名空间安装唯一固定Allocator/Manifest域。

Server只接纳为Persist，不能从收到Data请求直接进入Invoke。Runtime固定Continuation
重新核对认证参数的完整Payload摘要、精确Journal Domain/Schema/当前Generation和
回复容量，随后依次完成PREPARED回读、EXECUTING回读、当前授权和真实Foundation
代际校验，才在共享CallbackGate内交接一次执行器。正常业务失败用已证明的signed status
返回，与Provider不能证明是否完成的`Err`分开；后者与越界长度/状态合同故障只能InDoubt。
调用执行器后不新分配Journal/参数/结果/回复槽；固定结果和Continuation保持到终态回读。

Data Error的既有u16结果码中，耐久模式保留`0xFFFD=ABORTED_NO_EFFECT`、
`0xFFFE=IN_DOUBT`。两码只在Flags=6的Error中有上述含义，正文必须为空；普通
已知业务错误不得占用它们，Result角色和易失模式不把这两数值误解为耐久结论。
本地`RpcPacketView.result_kind`区分Business/NoEffect/InDoubt；Service的Complete
只是“最终观察已收到”，不是“成功执行”。本地status仍`Error=-1-code`，因此这两码
分别映射-65534/-65535；不要把它们与Journal中的原因status或本地Error enum混同。
不得据IN_DOUBT自动建立新Operation重执行，也不得据普通发送成功GC Journal。

恢复到EXECUTING的Journal不再交接执行，可持久化InDoubt或走执行器对账。
PREPARED只有重新取得精确认证参数及当前授权后才可继续。终态结果须经过确认回读才能
发布；执行器对账得到的新RAM结果也必须再写终态并回读，不能只凭查询成功发布。
缓存退休后，真实认证Replay证据可重新绑定已确认的终态Journal并生成结果回复；只
恢复结果缓存，不交付Replay明文、不重新执行。结果检索可有新的有界回复截止，但不能
续期原执行权限，Client原请求截止也不变。

Operation控制Body中的“原业务opcode”允许0表示Data；它不是外层非零的ProtocolOpcode。
C/Rust同时拒绝0xFFFF保留值；共同控制Body长度28/48/32 B不变。新规则和两种特殊码
纳入共同JSON和native C/Rust/Python字节Oracle，不声称C生产Runtime已接通耐久Data RPC。

第五段结束时，范围仍是当前精确Session/Key/Policy/Binding下的C1/H0直达；Journal重建不能自行恢复
Security或Authority。真实整机冷启动、跨新Session的续接重绑定、C0取消与Data写入的
统一调度、结果ACK/GC和C↔Rust完整Runtime仍归后续小项；不据本段签顶层完成或生产GO。
详见[第五段自审报告](../../../08-实现与验证/版本演进/UCN_V6S_RUST_SW_01_耐久业务请求与执行交接自审报告.md)。

## 12. 第六段：认证结果 ACK、正文 GC 和终态安全续接

共同 JSON 与 C/Rust Registry 新增 `OperationResultAck=0x0504`、
`OperationResume=0x0505`、`OperationResult=0x0506`，该阶段已登记数为97。后续 Capability
补登记 `CapabilityQuery=0x0107` 后为98；既有 Opcode 数值不变，不为旧语义保留 fallback。
普通 Header、Tag、Data9/11 B Envelope 和 OneWay 不变；新字段只存在于显式控制正文。

| 消息 | Role | 正文合同 |
| --- | --- | --- |
| Resume | Request | 原 OperationWireKey28，绑定 ID/Service/业务opcode/请求摘要 |
| Result | Result | Receipt48 + status:i32be + length:u16be + exact payload；54..200 B |
| ResultAck | OneWay | 精确 Receipt48，仅 phase3/4；IN_DOUBT 无可GC的ACK |

Result 仅 phase3/4/5，只有3可含正文，最大146 B。原共同结果摘要绑定全部内容，
接收方须重新计算并精确比较。A1/H0/O1 的 Resume73 B、ACK93 B，恢复结果99+N B，
最大245 B。需要至少245 B完整Frame实例；短Holder在状态发布之前拒绝，不能截断。

Rust Runtime 的 ControlCoordinator 实际认证、ACL、Replay 后签发命令；恢复分派
还核对真实 Foundation 的完整基线和当前确认代际。只观察终态，不改 Journal 原键，
不重新交接执行器、不续期原执行 Deadline。新查询有独立接纳相对的半开截止。
新安全域只在 Realm/双端Principal/Address/Binding 全部精确不变、当前 ACL 有效，
Session/原发起端Key/Policy不回退且O2不降级时可使用；等代际三元组不得换Fingerprint。
结果回复冻结在本次认证安全域；客户端验证结果后先核对ACK Outbound ACL/容量并
预留Holder，再复制观察正文。旧Service请求不会被重绑定或重新执行。

ACK必须与当前实际Journal Receipt逐字段相同，首个合法ACK产生易失且不续期的
保留水位。到水位后GC才冻结OPS1 Tombstone，并经过真实 write/reload/Proof退休
清除结果正文；持久化失败保留精确义务并围栏。Crash丢失ACK水位后重新认证/等待，
不能以旧uptime继续GC。墓碑保留完整键，恢复后仍禁止执行与ID/Journal槽复用。
PREPARED/EXECUTING/IN_DOUBT不得超时GC；IN_DOUBT对账须先耐久确认结果，再ACK。
缺少独立Principal耐久ID高水位和最大重放寿命证明时不释放槽，不做表满驱逐。
逻辑正文回收不等于物理安全擦除，双槽上一副本可能仍保留旧字节。

C本轮只同步三项Registry，native C三项独立byte fixture参与Oracle；C生产Runtime
未实现恢复/ACK终结handler，仍失败关闭/不透明中继，不声称完整C↔Rust互通。
未改变OPS1 schema、C Storage Layout，不引入旧版兼容。此新合同须重新外审。
未完成执行的跨域续接、Binding迁移、槽安全复用、统一调度及完整动态管线仍归后续。
详见[第六段逐项与整体自审报告](../../../08-实现与验证/版本演进/UCN_V6S_RUST_SW_01_结果ACK_GC与安全续接自审报告.md)。

## 13. 第七段：静态服务端统一有界调度与退休

Rust新增可裁剪`journal-runtime`，装配静态单Peer/单Journal服务端；不是共同Wire的
另一个版本。C0与耐久Data沿用第六段合同，Registry、Opcode、Header/Tag、Data Envelope、
OPS1 schema、C Storage Layout均未改变。本段没有新增C生产代码。

Runtime Coordinator独占编排固定命令/回复、Service、Durable Continuation、取消及GC；
业务Owner不直接调用其他Owner。原始完整Frame从专用Adapter RX进入，分类后实际执行
Security认证/ACL/Replay，不从Driver获得“authenticated facts”。本facade要求单Link
Adapter不与其他Core/RX Owner分享；全节点共用分类器、多Peer/多Journal和源端装配归后续。

七类工作为Ingress/Foundation/Control/Request/Timer/Transmit/Retire，与Wire Q0～Q3无关。
每step总访问1..64、每类1..16且不允许零配额，总数不大于配额和；空/PENDING检查也计费。
游标跨调用保留，内部TX/Timer/退休子支也轮转，不用固定严格优先级吞掉低频义务。
Foundation每访问预算1；Executor/Provider/Driver/Crypto必须由产品保证回调有界非阻塞。
该合同不承诺微秒WCET、不修改Core原6:3:2:1队列，也不冒充完整QoS调度。

取消只在实际认证及完整Operation匹配后立即闩锁，防止配额先耗尽时后续执行边越过已到达的
合法取消。Data写入中不启动第二次同Journal写入；已接纳I/O先确认/退休，再耐久NoEffect。
若执行已交接，停止不能伪造NoEffect，仍须写回已观察结果或明确InDoubt。
ACK/GC也在同一个Journal串行域内；Failure先精确围栏/退休，不直接遗忘请求。

NOT_SUBMITTED保留原字节、Sequence/Tag、Deadline和有限尝试计数；UNKNOWN保留Holder。
停止不把逻辑过期当物理完成，不自行重开Driver。quiescent只指本Runtime停止后的私有
命令、Continuation、I/O和物理义务收口，已确认历史及其他共享Domain仍独立存在。

确认/恢复的OperationOwner保存实际Record Digest，并与当前Foundation的Generation共同
精确核对；同代际不同正文的正常API错装配零推进拒绝。该Digest原本就在共同Record中，
不是每个业务Frame新增字段，也不是任意RAM篡改防护。自有私有I/O允许回读先于RAM确认，
但不能以调用方声明的pending绕过实际Consumer/Runtime/Schema绑定。

共享CallbackGate由实际Security配置取得；Crypto、Foundation Provider、Driver submit/cancel
和Executor使用同一门，原子完成ingress保留规定例外。固定本地资源新增，不宣称RAM零增量。
本轮Debug/Release/MSRV各232/232、17裁剪组合、33组三方Oracle、双Cortex/ESP32-S3
compile-only通过；380项依赖前后稳定。仅Host自审，无新增生产密码、C全Runtime互通、
Flash/掉电、MCU栈、物理Bearer或长稳证明；下一阶段源码变更需要重新验证。
详见[第七段逐项与整体自审报告](../../../08-实现与验证/版本演进/UCN_V6S_RUST_SW_01_C0_Data_Journal统一有界调度与退休自审报告.md)。

## 14. 源端与共享收发调度（第八/九段）

Source Runtime 在实际当前 Session、Endpoint最低保护、出站ACL及恢复结果MTU满足后，
才冻结/写入/回读/退休 OID1 区间并签发线性ID。请求重试复用原ID/原保护字节/原执行Deadline。
Data结果和超时都经独立有界Resume查询；认证Result精确匹配原键及摘要，ACK资源和出站
授权预留后才交付观察。原Data Result并不单独授权GC或证明无副作用。

Duplex唯一领取原子RX；未认证Header仅分派提示，两个分支各自实际认证和ACL/Replay。
两侧共享一份七类预算/持续游标和唯一Foundation访问。停止关闭RX并排空未领取和已领取项，
Driver UNKNOWN、已接纳I/O保留精确义务。单Peer/单请求，不冒充多Peer/通用Node。
此两段没有新增Header/Tag/Record字段；资源成本及245/245软件证据见
[源端与共享调度自审报告](../../../08-实现与验证/版本演进/UCN_V6S_RUST_SW_01_源端与共享收发调度自审报告.md)。

用户已授权继续补共同控制正文及同步C/Rust；后续Parent、Time、动态/Flow及Group/Cluster
仍属软件工作，不因上述局部通过宣布RUST-SW-01..05完成。普通数据头保持不增加字段。

## 15. Parent发布与自主Transfer发送（第十/十一段）

沿用Parent四项Opcode、共同32 B控制正文及完整UCPT规范摘要；普通数据头不变。
C/Rust同步Codec/纯Transport阶段规则；Rust实际Foundation→认证发布→Adapter→缓存确认。
Source必须借用并复验当前媒体，不靠对端bool或历史Record自己变成线上Active。
Rust自主TX-only发送把耐久ID/Setup/分片/SACK/业务交付/终态丢包重试接成实际Host链。
当前不含Flow/RPC、自动Parent轮换及C完整Root，不能把共同Codec算作整套Runtime已完成。
详见[精确共同合同](04-Parent发布与传输发送共同合同.md)与
[逐项及整体自审](../../../08-实现与验证/版本演进/UCN_V6S_RUST_SW_01_Parent发布与自主Transfer发送自审报告.md)。

## 16. Time认证报文与实际事件链（第十二段）

沿用四项Time Opcode、12/24 B Body和128 B Domain Record；普通数据头0新增字节。
Rust可裁剪time-runtime在实际当前Domain/Foundation借用证明、固定STATIC_MASTER和
Security/ACL/Replay下，内部构造并提交四帧、绑定真实T1～T4，再生成不可伪造Sample。
同一完整RX与Timestamp原子入队；Driver/Queue/Sync退休失败保留精确义务，原Deadline不续。
C/Rust拒绝aggregate.asymmetry低于Path上界；Rust样本补双钟交换处理期漂移。
257/257、22裁剪、41三方Oracle与393依赖稳定证据是受限Host结果，非精度/掉电/生产签字。
详见[共同合同](05-时间同步认证报文与事件链共同合同.md)和
[逐项及两轮自审](../../../08-实现与验证/版本演进/UCN_V6S_RUST_SW_01_时间同步认证事件链自审报告.md)。
C完整认证Time Root、诊断/动态/多Peer、全模块共用调度与完整互通继续属于软件待办。

## 17. 已认证Peer能力协商（RUST-SW-02第一段）

Capability六类既有正文与新登记QUERY20明确区分；C/Rust共同Record68、Summary24、
Query20、Select59和Ack75，以SELECT源视角比较完整交集，并绑定152 B完整因果域摘要。
普通数据头0新增；A1/H0/O1无丢包首次六帧握手共584 B，不是逐包开销。

Rust可裁剪capability-control实际贯通Session、ACL、Replay、原子RX、Driver与固定缓存；
NOT_SUBMITTED重试原字节，丢包确认不重做业务，独立发送/退休游标保证预算1也能推进。
C同步认证QUERY分派、目标重建视角、REVOKE/过期保留同Session历史高水位。能力确认不是
ACL/Authority证书，也不执行Suite/Key重协商。两端当前仍不是完整自动动态Node。

详见[共同合同](06-Capability认证协商与精确事务共同合同.md)。本阶段的软件验证和已知范围
记录在[自审报告](../../../08-实现与验证/版本演进/UCN_V6S_RUST_SW_02_认证能力协商自审报告.md)。
Dynamic Admission、多Peer、C自动握手Root、Route/Flow及Group/Cluster仍是软件待办。

## 18. Bootstrap原始收发与待审事务（RUST-SW-02第二段）

HELLO40/Challenge40/Echo98沿用三类Opcode；Challenge用原保留区携带issuer-local
Link ID/代际，两端本地Link编号不要求一致。Cookie验证SPI接Owner可信now，完整32 B
前序摘要绑定两份canonical外层帧。普通数据头0新增；A1首次无丢包三帧265 B。

Rust专属单Link Root接实际原始RX/Driver，固定16 B小Cookie、一个Pending和一个回复缓存；
分别限制签发/验证预算，原截止期不续，保留早到回复、背压和UNKNOWN下的物理Holder。
C同步Codec/SPI/已有分派；无状态Cookie验证不等于身份认证，CookieVerified不发布Session。

详见[精确共同合同](07-Bootstrap原始收发Cookie与待审事务共同合同.md)和
[逐项及两轮自审](../../../08-实现与验证/版本演进/UCN_V6S_RUST_SW_02_Bootstrap原始收发与待审事务自审报告.md)。
本段最终269/269、24裁剪、52三方Oracle、7共享Golden及400项稳定快照仅证明受限软件结果。
完整JOIN互认证、持久化/reload、Peer Reauth和动态Route/Flow继续待办，整体外审/生产HOLD。

## 19. Bootstrap认证事件与身份校验（RUST-SW-02第三段）

使用既有0004～0008，Registry仍98；修正旧草案别名，C/Rust统一阶段未填充组必须全零。
379 B逻辑Transcript、最多128 B Evidence、36 B分片前缀和32～128 B通用分块共同冻结。
默认预认证Root只依赖Cookie已有保证，采用62 B分块；不根据本机MTU推断对端能力。
普通数据头0新增，分片byte5使用原保留位，位图固定16 bit，不引入动态容量。

Rust实际Adapter/原子RX/Verifier接通AuthorityProof和DeviceProof，保持原deadline和真实
物理Holder；丢片、坏nonce/Realm/Peer/MAC拒绝及完整重发有正式回归。C提供共同结构Codec，
不是完整自动Root。身份校验结果绝不等于Authority租约、ADMITTED或Session。

详见[字段/方向/费用/生命周期合同](08-Bootstrap认证事件分片与身份校验共同合同.md)和
[逐项与整体自审](../../../08-实现与验证/版本演进/UCN_V6S_RUST_SW_02_Bootstrap认证事件与身份校验自审报告.md)。
Offer/Commit/Final实际耐久发布、Session、Peer Reauth、多Peer和完整C↔Rust互通仍待；
本段只自审，整体外审/实机/生产HOLD。

## 20. Bootstrap耐久发布与Session建立（RUST-SW-02第四段）

Rust 可裁剪 `bootstrap-join` 从原有双向认证继续：Authority 地址预留回读后 Offer，设备
Binding 回读后 Commit，Authority 核对 Commit 和存活预留后 Final；双方再各自持久化
Session Record，只有 exact reload Proof 才能本地 Active。挑战起点在最初本地捕获，
绝不到 Final 才开始计时；共享实际 Callback Gate，提交/激活/查询复核当前 Link。

C/Rust 统一方向槽对和 domain-separated SessionDigest；原生 C/Rust/Python 独立 Oracle
验证。C 本段新增纯种子算法和回归，不声称已完成 C 自动 JOIN Root 或完整跨语言入网。
普通数据头0新增；A1/Evidence32/无丢包首次八类建链共4600 B，不是逐包开销。

详见[共同合同](09-Bootstrap耐久发布与Session建立共同合同.md)与
[逐项及整体自审](../../../08-实现与验证/版本演进/UCN_V6S_RUST_SW_02_Bootstrap耐久发布与Session建立自审报告.md)。
本段结束时H1到Capability尚未串联，后续第五段进展见第21节。Peer Reauth、多Peer、
Route/Flow、生产密码、真实Flash/掉电、目标资源和独立外审仍待办；不操作实机。

## 21. JOIN后H1能力协商与交接（RUST-SW-02第五段）

实际耐久JOIN可一次性交接到认证HELLO/QUERY/ADVERTISE/SELECT/ACK，不再强制H0。原JOIN
入口随成功交接禁用，Capability Coordinator保留对实际Identity父域的只读借用及逐次复验。
请求/回复共享每方向Hop计数器，重试仅刷新Hop，不重复执行或重用Origin Nonce。

C/Rust同步修复本地入口与远端Record的Link Generation混比；同Session仍冻结远端Link。
七类正文和普通Data头不增加字段。第五段A1 H1最大ACK136 B，128 B当时明确拒绝；
后续第22节已接认证控制分片，不靠降级安全绕过。

详见[完整合同与费用](10-JOIN后H1能力协商与交接共同合同.md)及
[分项/整体自审](../../../08-实现与验证/版本演进/UCN_V6S_RUST_SW_02_JOIN后H1能力协商自审报告.md)。
C自动Root、Peer Reauth、多Peer、动态Route/Flow、生产Provider及独立外审继续HOLD。

## 22. 小MTU认证能力分片（RUST-SW-02第六段）

Registry新增0108，原七类正文和普通Data头不变；仅放不下的Record/Ack用6 B局部元数据
拆两片，每片保留Origin与H1认证。连续C0事务推导组ID，不再附加8 B组号。固定75 B重组、
两份认证凭据、当前原Opcode/包装ACL和绝对Deadline共同约束，不发布半条命令。

Rust真实128 B缓冲下127/128 B完成JOIN到能力协商，127↔136 B也通过。预协商不猜对端
MTU，认证后取双方最小值。完整小MTU能力交换典型899 B；冷启动大链路首Record仍拆片，
典型753 B；普通业务数据开销不变。Capability ON每回复槽多预留一帧，明确RAM取舍。

详见[精确布局、费用及生命周期合同](11-小MTU认证能力分片与重组共同合同.md)与
[逐项及整体自审](../../../08-实现与验证/版本演进/UCN_V6S_RUST_SW_02_小MTU认证控制分片自审报告.md)。
C同步Codec/Registry，不声称C自动Root或完整运行时互通；第六段结束时Peer Reauth仍待，
后续进展见第23节。多Peer、动态Route/Flow、实机、生产密码及独立外审继续待办。

## 23. Peer Reauth邻接重新认证（第七段历史切片）

固定已知Peer在双live Binding和真实耐久前驱上重新认证，不重新分配地址或续租。四条
身份签名消息冻结Nonce、双Link、历史摘要及精确后继代际；实际Session回读前不发布
Response/Accept，UNKNOWN与Pending停止仍保留物理/耐久义务。建立后实际O1+H1 C1收发
通过，普通Data头0新增。C/Rust共同156 B Record、18 B局部片Codec及七条独立raw向量。

详见[共同合同](12-Peer-Reauth邻接重新认证共同合同.md)与
[逐项及整体自审](../../../08-实现与验证/版本演进/UCN_V6S_RUST_SW_02_Peer_Reauth邻接重新认证自审报告.md)。
此处保留上一轮156 B/V1证据；当前源码已由第24节V2取代，不提供V1兼容路径。

## 24. Reauth能力交接与耐久分歧恢复（RUST-SW-02第八段）

两端真实前驱可以不同，通过新鲜双身份签名确定共同最大值+1，本端Foundation仍必须
真实回读。普通静态Session入口不放宽；相同Session但历史分叉拒绝。当前C/Rust正文
208 B/V2，比上段多52 B局部控制信息；普通C1 Data头仍13 B、不增加任何字段。

Rust自动交接到认证Capability，保留ACCEPT重答缓存，唯一原子RX分派及轮转发送/退休
支持单TX槽127/128 B Adapter。Unknown、旧Link、错ACL、父域到期均失败关闭。
Reauth-only Host实例2304 B；启用Capability-control后6840 B，明确静态RAM代价。

详见[当前共同合同与费用](13-Reauth能力交接与耐久分歧恢复共同合同.md)及
[逐项/整体自审与最终证据](../../../08-实现与验证/版本演进/UCN_V6S_RUST_SW_02_Reauth能力交接与耐久分歧恢复自审报告.md)。
C自动Root、多Peer、跨语言Runtime和动态Route/Flow仍未完成；独立外审、生产和实机HOLD。

## 25. 多邻居认证路由控制（RUST-SW-02第九段）

外层C0认证实际邻居，正文冻结原始DiscoveryKey44，不再从中继帧头猜原始Principal。
共同RREQ54/RREP77/RERR99；0206提供14 B局部前缀、2～4片有界认证分片，普通C1数据头
仍13 B、没有新增字段。C/Rust独立Codec及三方raw向量同步；Rust实际三节点Root已覆盖
127 B帧、共享物理Link上的双Peer、丢回复重试、RERR本地代际映射和Unknown退休义务。

详见[共同合同、精确费用与边界](14-多邻居认证路由控制与局部小MTU合同.md)。
这是控制面子集；C自动多Peer/分片Root、统一多跳业务与Flow、各模块唯一Ingress编排
仍须继续，不把前文第八段或本段绿灯当作整个RUST-SW-02完成。

## 26. C1不透明逐跳转发原语

在第九段认证寻路之上，新增[端到端与逐跳分离合同](15-C1不透明逐跳转发与计数器共同合同.md)。
Rust实际三节点Host测试已串联独立A/C Origin与AB/BC Hop、动态Route查询及原子Adapter。
保持原数据头，不让中继解密或取得Endpoint权限；修复共享counter重置可重封的防误用边界。
这是密码/转发原语层，不是自动业务Node、C1的C中继或完整Flow已经完成。

## 27. 自动多跳 Data 协调器

新增[合同16](16-自动多跳Data协调器与本地下一跳合同.md)，以独立Origin/Hop原语接普通
业务send、精确RREP后自动路径选择、单一原子RX、有限队列和物理退休。下一跳定位在本地
Driver SPI，不增加普通数据头；H1/Origin安全费用照常计入。反向业务必须完成自己的
Origin域发现，不能仅凭RREQ本地返回提示冒充全路径。

本段只接Rust已知Peer/预发布端到端Session的尽力单向Data；C自动Root、动态端到端
Session、Reliable/Flow、全部模块统一Node和跨语言Runtime未因此完成。逐项和整体
自审见[报告](../../../08-实现与验证/版本演进/UCN_V6S_RUST_SW_02_自动多跳Data协调器自审报告.md)。

## 28. Origin登记围栏、排空与耐久后继交接

[合同17](17-Origin登记围栏排空与耐久后继交接合同.md)补齐已知端到端Session更新后的
本地交接。旧Session全部Service先围栏，旧Data/App/Token排空后才替换精确登记；普通
register、其他Service空槽或更旧墓碑都不能绕过屏障。新登记必须来自实际Security ACTIVE
后继，Foundation仅durable尚不足，旧Handle不复活、O2策略不自动降级。

本段不建立跨多跳握手报文，C/Rust线上格式和Record不变；Rust实际三节点更新后收发、
有效Hop下拒绝迟到旧Origin、连续槽复用等13项回归见[自审报告](../../../08-实现与验证/版本演进/UCN_V6S_RUST_SW_02_Origin登记续接与排空自审报告.md)。
这里的drained只是当前Data Root屏障，不是全Node/其他Runtime的静止证明；生产和实机仍HOLD。

## 29. 直连、多跳统一Origin/Hop Context

[合同18](18-直连多跳统一Origin与HopContext接线合同.md)取消“Endpoint必须是H0 Session”
的错误接线前提：直连可从同一真实H1 Session分别使用Origin和Hop，既不建立双ACTIVE，
也不把两层计数器/Replay合并。精确已认证邻居无需RREQ；远端仍自动取得精确RREP。
同Root可按相同send接口混发直连与多跳，普通数据头新增0 B，安全费用不隐去。

C/Rust同步显式Origin-only原语，完整packet API仍拒绝Profile错配。共享Session围栏
包含本Root的C0/中继Holder，Unknown不能假装排空。C自动Data Root、O2自动邻居控制、
热续接、完整Node/Flow等仍待软件实施，不冒充整套协议已闭环。
详见[本轮逐项/整体自审](../../../08-实现与验证/版本演进/UCN_V6S_RUST_SW_02_直连多跳统一Context自审报告.md)。

## 30. 邻居围栏、真实排空与耐久后继Capability热交接

[合同19](19-邻居围栏真实排空与耐久后继Capability热交接合同.md)接通Rust本地邻居
fence/drain/rebind，复验真实耐久ACTIVE后继和新认证缓存；C/Rust同步原槽Capability
父域替换及精确Route/Reverse退休。旧Control/Data/App/中继/RX义务不清零，Unknown
只能凭真实退休解决；普通冷注册、旧Handle和Service墓碑均不能绕过。

同Root恢复直连与多跳，五轮固定槽复用；新MTU不继承旧值，reopen后旧本地Capability
拒绝。普通Wire新增0 B，Capability本身没有变成耐久Record。产品Coordinator仍需独占
热交接阶段Ingress，完整自动Reauth到全Node、C自动Root与多Bearer热迁移尚未完成。
详见[逐项/整体自审](../../../08-实现与验证/版本演进/UCN_V6S_RUST_SW_02_邻居围栏与耐久后继热交接自审报告.md)。
