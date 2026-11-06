# RUST-SW-02 Peer Reauth 邻接重新认证自审报告

> 历史证据对应156 B/V1切片。当前V2及新增恢复/交接结果见
> [后续自审报告](UCN_V6S_RUST_SW_02_Reauth能力交接与耐久分歧恢复自审报告.md)，不得将本报告旧矩阵当作当前源码验证。

## 1. 本轮范围与状态

2026-10-15，`v6-simplified-rust@03eb7eb`加继承工作树。用户授权“接通Peer Reauth，C/Rust
合同统一，每小项自审，最后整体自审，不操作实机”。本轮未操作COM/USB、烧录/复位、
目标调试、提交/推送或Obsidian改写；保留既有文件改动，知识库仅用于只读原则核对。

实现固定已知Peer、双live Binding、真实前驱Session的新邻接认证；普通数据头不增加。
C同步规范Record/片Codec，Rust串联实际Identity/Foundation/Security/Adapter。共同规范见
[第12篇](../../10-理论与规划/建议方案/UCN_v6_C_Rust共用合同收口/12-Peer-Reauth邻接重新认证共同合同.md)。

本段只形成Host自审候选，不签外审或生产GO；RUST-SW-02仍IN PROGRESS。

## 2. 小项及完成后即时自审

| 小项 | 实现 | 区分性验收 | 自审 |
| --- | --- | --- | --- |
| PR-01 | 固定156 B Record、18 B局部分片、0010/11/12/14 | 四条raw Record和三条Fragment三方Oracle；保留位、长度、终值、短输出及零写入 | 定向通过 |
| PR-02 | 双Identity父域及真实Foundation前驱 | Factory拒绝、错误Binding拒绝；不因有历史Body就授予活权限 | 通过 |
| PR-03 | 旧Session围栏、RX禁用/清空、TX全部退休 | 存在Reserved Token时构造拒绝且Token不变；UNKNOWN停止后仍不静止 | 通过 |
| PR-04 | 角色/Nonce/事务/Link/代际冻结及身份签名 | 六组有效签名的域变异均在Provider验证前拒绝；坏签名不推进阶段 | 通过 |
| PR-05 | Security Requirement→实际Foundation回读 | Initiator reload前不发Response；Responder reload前不发Accept；损坏/迟到Proof不发布 | 通过 |
| PR-06 | 三片缓存重试和总物理预算 | 丢Challenge/Accept可收敛；重试字节相同且只签四次；持续背压严格耗尽上限 | 通过 |
| PR-07 | Pending停止、物理/耐久退休 | Pending不能先出队；真实终态后才退休，停止不能复活Session | 通过 |
| PR-08 | 新Context与既有业务链 | 新Session代际2、方向Key/Context一致，显式新ACL后实际O1+H1 C1数据收发 | 通过 |
| PR-08B | 单向已建立终态及握手/Session寿命分离 | ACCEPT三片物理完成前不Established；已建立后重试耗尽/握手到期仍可读，真实Session到期拒绝 | 通过 |
| PR-09 | Feature OFF、资源、全模块回归与证据 | 当前源码冻结，完整矩阵结果在结案段记录 | 通过（本段Host范围） |

Runtime正式测试入口为`bootstrap_runtime`，新增16项；Security纯Codec新增3项。测试使用
真实Identity发布及Foundation Proof，不构造伪BindingView/OpenedPacket。身份签名与数据
密码是Host HMAC测试Provider，不宣称生产身份Key、AEAD或硬件Provider已经通过。

## 3. 第一轮整体自审：信任及历史

1. 发现“用JOIN重新分配地址”会扩大Reauth语义，独立实现只更新Security域；测试确认
   Binding Record仍为generation1，不借Reauth续租。
2. 双Binding必须是当前Authority的实际live View，不能只用调用者提供的Principal或
   历史Session候选。旧Session、原Nonce/Key/Replay不能通过reset恢复。
3. 使用完整32 B前驱摘要、精确后继Session及双向Key代际，所有增量checked。有效签名的
   错Nonce、Link、历史域和Key代际仍拒绝，不能把“签名正确”当作“合同正确”。
4. 双向证明合并改为固定长度的Challenge签名拼Response签名，最大128 B，避免与现有
   C证明容量冲突；Stage参与身份签名，不把片Codec当认证。
5. 新Session的业务ACL必须显式绑定新Context，未继承旧Fingerprint权限；C1回归明确
   配置新Context的精确ACL并实际执行保护、打开和Replay提交。

## 4. 第二轮整体自审：生命周期、时间、资源及诚实边界

1. 增加Adapter TX只读静止查询，终态未退休也不算静止；构造要求RX已禁用且清空。
2. 发现逐片重置预算可能放大总提交次数，增加每阶段物理总计数，包含NOT_SUBMITTED。
   重复消息不重置计数、原截止或签名，单次驱动至多一个Driver调用。
3. 将握手TTL与新Session寿命拆开，但后者仍不超过两个Binding和Authority的本地截止。
   Provider前推进Owner时钟，高水位和回调序号checked，不从迟到报文重建起点。
4. 六组规范有效签名域变异测试发现夹具分片块长不一致；统一复用共同80 B片Codec，
   既有Echo98+C0头保证已足够，不额外增加探测字段或扩大128 B缓冲。
5. JOIN夹具增加第二Binding存储域后，修正其Manifest排序；旧JOIN仍只验证本来三个域的
   witness=1，另明确新增未使用域witness=0，不把新空域误判为旧JOIN失败。
6. 原始片先结构检查，完整逻辑后才验签；固定单槽不分配陌生Peer，无未认证回复，
   Provider与物理预算固定。该入口不替代动态未知Peer的Cookie/配额。
7. 明确暴露本段限制：两端提交不对称后的自动对账、Capability自动交接、多Peer/C Root
   仍待软件工作。没有以fail-closed代替恢复完成声明，也不声称原子双端激活。
8. 正式验证为A1、127/128 B专属链和HMAC测试Provider。没有本轮新做实机、Flash掉电、
   生产密码、TSan或目标任务栈；不能套用旧报告数字。
9. 首轮317项完整矩阵后，最终回读发现本端已建立结果被握手IO截止/故障门遮蔽的边界，
   增加PublishingAccept和发布后的独立查询门，补终态回归。最终矩阵重新完整执行，
   不沿用该修正前的317项快照。
10. 最终逐字段回读再补纯候选派生入口对原typed Stage的校验，防止规范化掩盖非法输入；
    C失败零写入测试改用字节快照，避免结构padding赋值造成跨编译器误判。改后再次冻结
    全源码重跑最终矩阵，不用先前快照冒充修改后的验证。

## 5. 最终验证及不可变证据

源码/JSON/工具冻结后执行完整Rust矩阵，再只补文档与独立证据文件；结果和全部源码哈希
在结案段记录。定向Reauth16/16、Codec3/3及Clippy已通过，三方Oracle当前80向量。

## 6. 下一步与保留阻断

本段可供Host软件外审，但没有外审签字。自动Reauth→Capability无丢帧交接、两端耐久
高水位不一致后的对账、动态/多Peer和C自动Root仍待实现，不能标成“只剩实机”。
真实Flash/Witness/掉电、生产密钥/密码、Bearer、MCU栈/资源/性能及产品验收继续HOLD。
普通Data新增0 B；32 B签名的A1无丢包重认证典型1320 B是控制交换费用，不是逐包负担。

## 7. 最终结案证据

全部最终修正之后重新冻结源码，矩阵时间`2026-10-15T07:16:01.389825+00:00`。
[不可变软件矩阵](../软件证据/Rust/2026-10-15/software-matrix-sw02-peer-reauth-final.json)
SHA256=`4880C66AA2B5D9E9E1BBB08A4CCC532442DE986C2A444DA85693366F12997FD4`。
共12项配置、415项源文件/合同/工具哈希，`source_stable=true`；保存后逐文件独立复算，
415/415匹配、差异0。没有把前两轮修正前的矩阵当最终证明。

| 验证 | 最终结果 | 边界 |
| --- | --- | --- |
| Rust Debug / Release / MSRV 1.85 | 各318/318 | Host测试 |
| Clippy workspace/all-targets、fmt | 通过 | 严格告警和格式 |
| Cortex soft / hard、ESP32-S3 | 通过 | 只交叉编译，未操作实机 |
| 可选模块/Feature OFF | 28组合通过 | 20 crate；三可选Owner互不依赖 |
| C/Rust/Python Codec Oracle | 80向量通过 | 七条新增Reauth规范字节，不是跨语言Runtime入网 |
| Admission共享Golden | 7/7 | 旧JOIN合同未改变 |
| Windows GCC C Full重建/CTest | 108/108 | 当前既有Full构建，不冒充fresh全工具链矩阵 |
| C Lite / Nano重建定向 | 各4/4 | Security Session、Control Wire、Admission、Security栈门 |

资源工具在本轮具体Host泛型配置测得`PeerReauthRuntime<128>=2152 B/alignment8`；既有
`CapabilityRuntime<128>=4392 B`未因Reauth增加字段。前者是额外可选Root存储，不是零RAM
成本，也不是完整MCU峰值RAM、Flash或任务栈。未新跑MSVC/Clang、ASan/TSan与目标板栈。

本段PR-01～09、PR-08B即时自审及整体自审完成。保留第6节软件/硬件边界，外审仍待；
项目任务表不将RUST-SW-02整体标DONE。

结案机械门禁：当前文档148篇/386条链接、C公共API索引318项均通过；最终报告链接补齐后
CTest文档/API两门再次2/2通过。`git diff --check`退出0（仅既有换行提示），四份新增Rust
源码/测试及两篇新增文档尾随空白0，真实暂存区0项。仅补文档后415项源码哈希仍0差异。
