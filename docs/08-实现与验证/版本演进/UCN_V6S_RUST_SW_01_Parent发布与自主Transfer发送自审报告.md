# RUST-SW-01 第十、十一段：Parent 发布与自主 Transfer 发送

## 1. 范围

2026-10-14；`v6-simplified-rust@03eb7eb` 加完整工作树。
用户已授权共同控制正文与C/Rust同步；无实机、COM/USB、烧录/复位、提交或推送。
本轮受限Host子集SELF-REVIEW PASS；顶层RUST-SW-01..05/外审/生产不自动关闭。

[共同合同](../../10-理论与规划/建议方案/UCN_v6_C_Rust共用合同收口/04-Parent发布与传输发送共同合同.md)
沿用0410～0413，新增精确32 B低频控制正文，不增加普通Data/Header/Fragment Prefix/Tag字段。
UCPT88 B、OPS1/OID1 Schema、C Storage Layout不变。

## 2. 小项与接线

| 小项 | 实现、自审与范围 |
| --- | --- |
| Parent Body | C/Rust精确Codec与UCPT前76 B规范摘要；5类Golden、长度±1、reserved、tx/gen/digest零、错disposition、C别名零写。 |
| 发布阶段规则 | C Transport/Rust Transport中纯规则；960组Source/Phase/Opcode/Disposition/Sent三方独立对照。Wire只做结构Codec，不持有业务状态。 |
| 实际源记录 | Rust分配器增加当前完整Record Digest；实际Foundation Consumer/Runtime/Schema/当前正文、Generation+Digest及未退休义务核对；借用证明不可自行构造/Clone。 |
| 认证发布 | Rust实际ControlCoordinator/Security认证PREPARE/ACCEPT/COMMIT/Committed；源只有COMMIT实际提交后才接受Committed，不接受`peer_ready=true`。 |
| 缓存与退休 | 背压、丢Prepared确认重发不变字节；早到ACK不丢Token；UNKNOWN、stop及假重开仍保留义务，精确完成才退休。 |
| 自主发送 | RustTX-only固定窗口；当前Active Parent+媒体借用证明、耐久线性ID、Setup原字节、每片一次保护、RTO轮转和实际Driver disposition。 |
| 实际接收闭环 | 同一Host集成接实际Foundation、单Link Adapter原子RX、真实Security、SetupACK、两片、SACK、业务交付与Terminal；不是伪造认证DTO。 |
| 终态丢包 | 所有SACK仍Sending；丢Terminal后重发原最后一片，认证Replay查缓存，原终态回复，无第二次业务交付、无新nonce。 |

Source Parent需要每个动作核对完整当前基线；分配高水位推进但canonical Parent不变时，
可以重新签发借用证明继续使用。Parent/Session/Key/Policy/Binding/Link改变须围栏原实例，
本轮不实现原地替换或自动轮换。Receiver是当前Session内有界soft Parent。

## 3. 整体自审

第一轮按Role/Domain/Deadline、消息和Provider/Driver所有权回读：

- 拒绝未退休Proof发布；旧Allocator配已推进媒体拒绝；Runtime/Consumer/Schema跨装配拒绝。
- 均值Hash或Generation不能单独当证明，完整canonical Parent在私有Pending保存并逐字段复验。
- Source专用许可不能走Target只读接口绕过借用证明；固定Service、Peer/Link与当前保护绑定。
- Feature关闭时不接纳Parent命令，不消费其Replay；纯Codec存在不代表业务功能可用。
- 将C纯状态规则从Wire移到Transport，消除结构Codec与业务状态规则混装。

第二轮对发送与失败路径回读：

- 驱动UNKNOWN的真实状态在返回错误前保留，不丢Token，也不能凭重开认为物理完成。
- 发送侧允许纯TX装配，去掉为构造器强行配置的无用RX缓存；空TX+RX和不足Receipt拒绝。
- Fragment Header用强类型CommonHeader构建，不手工覆盖Traffic Class位。
- counter预检先于prepared/credit写入；保护失败清理准备标记，停止排空未提交准备及物理义务。
- Deadline到期进入不可恢复Failed；所有片SACK不假造Done；原字节终态探测不重新执行业务。

Formal回归以独立字节向量和实际收发为准，未用“编解码同错自洽”或调用方bool替代安全证明。

## 4. 验证证据

最终11项矩阵全部退出0：Debug/Release/MSRV各 **250/250**，Clippy/fmt、21裁剪组合、
41组三方字节Oracle（含960种阶段组合）、双Cortex与ESP32-S3 compile-only通过。
391项源码/合同依赖前后稳定，独立SHA复算差异0。
[最终完整原始证据](../软件证据/Rust/2026-10-14/software-matrix-sw01-parent-transfer-final.json)，
SHA256：`F59E2E69162B2E346CBECA1E378BDB15735A717C6606B3B18186B02EA86857BA`。
原始报告绑定当前代码快照；后续改动必须重新验证，不能继续沿用为新代码签字。

Host `sizeof/align`：Parent Coordinator **600/8 B**；
TransferSender `<64,4,128>` **1360/8 B**，`<512,16,128>` **3616/8 B**。
不含外部Owner/Ingress/Provider/任务栈，不能充当MCU实测或最终Profile预算。
发送侧独立UNKNOWN/每片连续背压和所有窗口变异仍需扩大覆盖；本轮UNKNOWN正式反例在
Parent/Adapter链，不能声称已经重复验证全部Sender失败组合。

第十段历史矩阵为249/249、20裁剪、41Oracle、390依赖稳定；仅绑定其旧快照：
[历史副本](../软件证据/Rust/2026-10-14/software-matrix-sw01-parent.json)，
SHA256 `6607DD81C8332358615C37487A1E40D43E4B49CFAACADF843ED20EA44DD28B50`。
最终C侧在成功重建后定向Control Wire/Parent/Persistence 3/3；不引用构建失败后的旧可执行文件。

## 5. 不能外推的能力

当前Rust Parent与Sender是静态单Peer、一次发布/一次Transfer、至多32片的直接H0子集。
尚未接全Node预算、共享Ingress全部类别、多Peer/多Journal、跨重启客户端请求Journal、
循环槽复用、Flow/多窗口Transfer及Transfer承载的RPC执行。
C已同步共同Codec/摘要/纯阶段规则，但其Parent发布认证handler/自动Root未接通，
不能宣布C完整Runtime互通或共同生产GO。

Time raw+TimedLink、动态Admission/Capability/Route/Flow、Group C5/H3/Tree、
Cluster证书/Delta/Merge/Dissolve/Federation，以及整体C↔Rust互操作仍是软件任务。
本轮也没有真实Flash/Witness/掉电、生产密码、物理时间戳、MCU栈/整机资源、长稳或外审证明。
