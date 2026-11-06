# RUST-SW-02 第三段：Bootstrap认证事件与身份校验自审

## 1. 候选、范围和阶段

2026-10-15，`v6-simplified-rust@03eb7eb`加完整工作树，包括未跟踪文件。
用户已授权补共同控制正文并同步C/Rust；保持普通数据头不增加字段，不操作实机。
知识库只用于MCU-first、固定容量和Owner边界原则核对，当前源码/合同为准，未修改Vault。

本段接通实际AuthorityProof/DeviceProof事件传输、完整证据验证与本地认证阶段。
不是完整JOIN，不发布Address Binding、ADMITTED、Session或Authority写权限。
RUST-SW-02仍IN PROGRESS，整体外审/生产HOLD；本轮没有提交、推送或枚举COM/USB。

共同合同：[方向、379 B字段、分片、费用和生命周期](../../10-理论与规划/建议方案/UCN_v6_C_Rust共用合同收口/08-Bootstrap认证事件分片与身份校验共同合同.md)。

## 2. 小项与逐项自审

| 小项 | 实施内容 | 自审依据 |
| --- | --- | --- |
| AUTH-01 | Registry与阶段统一 | Rust旧131～136草案拒绝，沿用0004～0008；未登记Abort不生成逻辑Wire事件；C/Rust未来字段组必须全零 |
| AUTH-02 | C/Rust完整逻辑事件 | 379 B Transcript + 长度 + 1～128 B Evidence；Big-endian、精确阶段、长度、reserved、别名及失败零输出 |
| AUTH-03 | 有界MTU几何 | 原36 B前缀byte5携带chunk；32～128通用Codec、最多16片、固定16 bit位图；默认预认证Root用共同62 B预算 |
| AUTH-04 | 实际认证收发 | 借用真实Adapter/原子RX/Driver，Cookie后不可逆交接；配置Authority根，完整重组后Verifier验证，不接伪opened DTO |
| AUTH-05 | 反例与生命周期 | 丢片重发、错Peer/Realm/nonce/MAC、坏完整事件清理、精确重复免重验；原deadline与真实Token不丢失 |
| AUTH-06 | 裁剪、栈与交付边界 | C Decoder改用调用方静态workspace，原256 B门限不变；源/目标编译、OFF组合及来源快照重新验证，不签实机或生产 |

## 3. 实际端到端测试说明

[正式Root回归](../../../rust/crates/ucn-runtime/tests/bootstrap_runtime.rs) 从两端实际Adapter、
单Link Client/Listener和Cookie交换开始；Client=Link1/gen3、Listener=Link2/gen9。
证明阶段使用实际127 B与256 B异构MTU，不能假设两端本地编号、MTU或Token相同。

Echo真实完成并退休后才允许借出客户端认证传输；服务器只有真实CookieVerified Pending
才能交接。Root自己构造C0完整帧，实际Driver输出后进入对端原子RX，再按Key完整重组。
服务端本地AuthorityProof先验后发，客户端验证完整证据后发DeviceProof，服务端使用实际
Admission Owner验证并推进。没有传入authenticated布尔值来跳过验证。

测试Provider以分角色专用HMAC密钥验证事件Opcode和完整379 B内容，固定测试身份映射；
它不是生产公钥证书、阈值Authority证明或密码硬件隔离验收。Cookie与所有原始Driver
facts仍不能证明Principal。只读phase/Transcript表示本地进展，不授予租约/Session。

反例涵盖本地坏Proof、线上错Peer/Realm、即使被签名也错误的device_nonce、错误MAC和
丢失一片后的整事件重发。坏完整事件不能永久污染下一次重发；原deadline与密码预算不
刷新。精确重复不再次调用Verifier，Host实际计数断言可以区分重复执行/重复密码调用。

## 4. 两轮整体自审及收口回读

第一轮从共同合同到C/Rust实现：

- 发现Rust逻辑事件使用旧数值131～136，改用真实Registry；不登记多余Abort Opcode，
  也不让Cookie/Abort从逻辑编码器绕过Registry限制。
- Rust此前用“组有效=false”代表“该组不存在”，会漏掉部分非零未来字段；现与C一样
  明确要求未填充组全零。补阶段污染、旧Opcode与失败不写回回归。
- 固定128 B数据片无法放入128 B Link，改为显式冻结chunk，不增加分片头长度。
  16片边界/位图、精确重复、切chunk、offset/count/长度错误均有回归。
- C同步字段级逻辑/分片Codec，拒绝输入输出和workspace别名；不把C结构体padding上网。

第二轮从物理生命周期到发布面：

- 认证交换只借用现有Io；失败不会因消费整个Client对象而丢失Holder。退出借用后原Root
  不能回到Cookie起点，仍必须显式stop/retire；drop不会伪造完成。
- 最后一片已实际提交但物理完成未退休时，可验证合法响应，但不删除Token。取消失败/
  UNKNOWN仍由共用Io保持隔离，reopen旧Root也不能退休并消费新Link RX。
- 原C Decoder局部快照产生576 B直接栈帧，原有256 B门禁发现后改为调用方静态workspace；
  没有提高阈值或关闭告警。最终Full/Nano Host直接栈帧：逻辑Decode48 B、分片Decode224 B。
  这些不是完整调用链或MCU任务栈测量。
- Clippy最初发现通配导入、文档、较大类型按值传递和多个bool；已使用显式导入、借用
  Transcript与真实TerminalOutcome改正，没有全局放宽检查。

收口时再按异构条件回读：认证前没有可信Peer MTU，不能仅凭本地大MTU选择片长。
当前Root使用双方Cookie合同已要求的98 B正文预算，chunk=98-36=62；不增加协商字段。
127/256 B正式回归可以区分旧错误。通用Codec的32～128 B能力仍保留，但不能被Root
当作对端能力证明。收口修改后必须重跑最终矩阵，早期矩阵不当作最终候选证据。

## 5. 验证与证据

收口修改后已重新跑完最终矩阵；下表全部来自最终来源快照，不沿用中途成功结果。

| 门禁 | 最终结果 |
| --- | --- |
| Rust Debug / Release / MSRV 1.85 | 各272/272 |
| Clippy / fmt | PASS，Clippy -D warnings |
| Cortex soft/hard no_std | 两目标check PASS |
| ESP32-S3 | compile-only PASS，未烧录/运行 |
| Feature/OFF/独立依赖 | 24组合PASS |
| 原生C/Rust/Python独立Codec | 68向量PASS，含五个完整逻辑阶段、九片及两个完整帧 |
| Admission/Capability共享Golden | 7项PASS |
| 原始矩阵 | 12配置全部exit0、404项依赖稳定，独立PowerShell复算0差异 |

[不可变原始矩阵](../软件证据/Rust/2026-10-15/software-matrix-sw02-auth-events-final.json)
SHA256：`4318A1991DC99B8D4D638D8C708F54DF4B13C44CC04A3D5D125172F8FDABEB4E`。
来源快照含未跟踪源码、CMake和本轮C边界工具，不只包含git diff中已跟踪的差异。
本次与之前的Cookie/Capability证据分别保存，没有覆盖历史原始结果。

收口文档门禁144篇/366链接、公共API索引318项通过；本段4份相关文档另核验22个
本地链接，4个新增手写文件尾随空白为0。真实Git暂存区为空；diff-check无空白错误。
机械结果不能代替完整JOIN、独立外审或实机证明。

当前C实现已成功重建并通过Windows GCC Full 108/108，Lite/Nano定向各5/5，包括实际
Admission、Control Wire、Composition、模块边界与栈门禁；不是本轮fresh全工具链矩阵。
本轮没有运行MSVC、WSL ASan/TSan/Miri或MCU栈/Flash/掉电测试。

## 6. 费用、资源和后续

普通数据Common/C1～C5、Tag/Envelope不增加任何字段，Registry仍98项。
A1默认62 B分块、32 B HMAC Evidence时，每个412 B逻辑事件用7帧、867 B；Cookie三帧
265 B加双向证据合计1999 B，无丢包、无Carrier，**不是完整JOIN费用**。

最终Host x64 sizeof/alignment：Client128为552 B/8，Listener128为1192 B/8，
借用AuthExchange128为1656 B/8，小Cookie仍17 B/1。借用对象不是另复制一个Io。
这些组件不包括外置Adapter/Ingress/Driver、Provider scratch、调用栈或整个Node，
不能外推为目标MCU总RAM/Flash，也没有本轮Rust最坏栈证明。

下一步仍是软件工作：真实Identity/Foundation挑战与Owner捕获起点、Offer/Commit/Final
耐久发布、reload/Witness证明、Session准备/确认、Peer Reauth和失败/重启恢复。
不得在迟到Final时新建“现在开始”的Lease，不得仅凭phase或远端声明已durable就发Session。
还缺C自动认证Root、多Peer、公用调度及整套C↔Rust Runtime互通。

本轮自审不代替独立外审。真实Flash/Witness、掉电、生产密码、物理Bearer、目标栈、
精度、性能功耗和长稳仍独立HOLD；未提交/推送，未操作实机。
