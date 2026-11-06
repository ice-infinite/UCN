# Rust RUST-SW-01 第六段：结果 ACK、GC 与安全续接

> 2026-10-14；`v6-simplified-rust`，HEAD `03eb7eb` 加完整工作树。
> 用户授权软件实现、逐项自审和最终整体自审；不操作实机。
> 逐项及两轮整体自审通过，最终软件矩阵通过；外审与生产 HOLD。

## 1. 实现范围与重要限制

本段新增 `operation-control` 下的终态恢复和 GC Coordinator，接通实际认证的
`OperationResume → OperationResult → OperationResultAck`，再经保留期、实际 Foundation
写入/回读/Proof 退休把 OPS1 结果正文墓碑化。不是只把调用方的“已认证/已收到”布尔值
传给 Journal。跨 Owner 顺序均在 Runtime，Service 不链接 Security/Adapter/Foundation
Provider；所有状态有编译期容量，生产代码没有堆分配或新增 unsafe。

这里的“续接”只恢复已确认的终态观察及 ACK，不恢复旧执行权限：

- 可跨新的 Session/Key/Policy；Realm、双端 Principal、Address、Binding Generation 必须
  与原 Operation 完全相同。新授权必须是 Security 实际认证的当前 ACL，不继承旧 ACL。
- Session、原发起端 Key、Policy 不得回退；原 O2 不得降为 O1/O0；相同代际三元组不能
  更换 Fingerprint。查询的回复冻结在本次认证域；保护和提交时仍复验该域及当前 Link。
- PREPARED、EXECUTING 和已墓碑化正文不通过终态恢复入口。IN_DOUBT 可恢复明确的
  不确定观察，但没有可 GC 的 ACK；不能据此建立新 Operation 自动重执行。
- 不改写 Journal 中的原完整 OperationKey，也不刷新原 RPC 的执行 Deadline。
  查询有独立的、从实际接纳计时且不续期的有界 Deadline。
- 地址/Binding 迁移、续接未完成执行、执行器异步对账以及 C0/Data 自动统一调度仍待后续。

Obsidian 的 MCU-first/固定资源原则用于边界核对，旧版本能力描述不是当前实现证据。
本段 Host 验证使用测试 HMAC Provider；接口允许 O2，不等于有新 O2 生产密码证明。

## 2. 共同 Wire 合同和开销

三项新 Opcode 登记在 C/Rust 的同一 Registry，数量由 94 增至 97；旧枚举值不变。
普通 Common/C1 Header、Tag、OneWay 及原 Data RPC Envelope 均未增加字段。
这些正文只在显式恢复/ACK 时出现，不附加到每个正常业务帧。

| 控制消息 | Opcode / Role | 精确正文 |
| --- | --- | --- |
| OperationResume | `0x0505` / Request | 原 OperationWireKey，28 B |
| OperationResult | `0x0506` / Result | Receipt48 + status:i32be + length:u16be + exact payload；54..200 B |
| OperationResultAck | `0x0504` / OneWay | 原 Receipt，48 B；phase 仅 3/4 |

Receipt 绑定 Operation ID、Service、原业务 opcode、请求摘要、终态 phase、结果摘要。
结果摘要仍用已有共同 `UCN6-OPERATION-RESULT-V1`，包含精确 signed status、长度和正文。
Result 的 phase 仅 3/4/5，只有 3 可携带正文，最大 146 B；保留字节与未使用本地尾部必须零。
接收方重新计算摘要，不因为外层 MAC 正确就接受不一致的结果。

A1/H0/O1 下 C0 prefix 29 B、Tag 16 B：Resume 总长 73 B；ACK 总长 93 B；
恢复结果总长 `99 + business_length`，最大 245 B。普通 Data Result/Error 原 40 B 开销不变。
最大恢复结果要求 Frame/工作区至少 245 B；128 B 实例不会悄悄截断，预检即 NO_SPACE。
多地址宽度预算按 `25 + 2*address_bytes + body_bytes + 16` 计算。

C 生产本轮仅同步 Registry；新恢复/ACK 的 C Runtime handler 尚未接通，保持未实现终结
失败关闭/不透明中继的原边界。三方字节 Oracle 中 native C 新增的是独立正文 fixture，
不是 C Runtime 实现，也不是 C↔Rust 全运行时互通证明。OPS1 Body/Schema 和 C Storage
Layout 未改变。外审需覆盖此次 Registry/JSON 新差异，历史签字不能自动继承。

## 3. 从恢复到 GC 的调用顺序

1. Product 已完成独立 Security 会话建立/回读；续接入口不自行创建或恢复 Session。
   Client 以原 WireKey 发出受保护 Resume，Server ControlCoordinator 实际执行认证、ACL、
   严格 Codec、固定槽预留和 Replay 提交，得到私有 ControlHandle。
2. `dispatch_operation_resume()` 复验当前 Session/ACL、稳定双端身份及不回退代际，
   从实际 Foundation 复制当前正文、核对完整 DurableBase 与精确 Record Generation。
   只读取已确认终态。预检完整回复容量后保留不可变结果，再退休接纳命令；没有执行器调用。
3. ControlReplies 在当前认证域保护回复，再由真实 Adapter Token/Driver 提交和退休。
   不因本地发送完成就确认客户端收到，更不会删除 Journal。
4. Client `OperationRecovery` 显式登记原 OperationKey 和查询 Deadline。
   `dispatch_recovered_result()` 验证原键、当前授权、结果摘要及完整容量；phase3/4 先核对
   ACK 的当前 Outbound ACL 并预留 ACK Holder，之后才复制业务正文。phase5 不签发 ACK。
   相同观察的再次分派不重复复制正文；ACK 在 ControlReplies 中独立有界重试。
5. Server `dispatch_result_ack()` 必须再次取得当前精确 Foundation 基线，并把实际认证 ACK
   与 Journal 的完整 Receipt 精确比较。错身份、摘要、业务键、代际或失效权限均不改 Journal。
   首个合法 ACK 设置易失保留水位 `ack_now + retention`；重复 ACK 不续期。
6. 到达保留水位后，`OperationGc.begin()` 复验实际基线并冻结 Tombstoned，提交 Foundation
   要求且保留实际 I/O Handle。Provider 只在有界 `step()` 内运行；`finish()` 精确消费实际
   reload Proof，确认后再退休 Proof，之后才回收结果正文。
7. I/O admission 失败时仍保留冻结要求，可 `submit()` 重试；已接纳的 I/O 不能遗忘。
   未成功 Proof 不能当成功退休，实际失败先围栏 Operation 再 `failed_retire()`，不恢复执行。

## 4. GC 不等于释放防重放历史

本段 GC 回收的是结果正文，墓碑仍保留原完整身份和请求摘要。原 OPS1 inline 结果长度
清零、Record 恢复为 110 B，不能把它解释为可立即复用 Journal 域或 Operation ID。
物理双槽介质可能暂时保留上一槽的旧结果字节；本段不是安全擦除或密码擦除实现。

ACK 与本地时间保留水位是易失权限，不写入 OPS1：掉电恢复后必须重新认证 ACK 并重新
等待保留期，只会延迟 GC，绝不从旧 uptime 推导提前 GC。确认墓碑恢复后仍不再执行。
PREPARED/EXECUTING/IN_DOUBT 没有自动超时 GC。此次整体自审也移除了原 typed
IN_DOUBT 在 RAM 对账后直接 Tombstone 的分支：须先确认 COMMITTED_RESULT，再 ACK/保留。

槽复用要求独立证明该 Principal 的耐久 Operation ID 高水位和最大重放寿命已越过旧键。
当前没有这种完整回收证据入口，所以明确拒绝复用；不能用 Caller bool、计时到期、
任意 RAM 高水位或表满驱逐来补齐。固定表满仍失败关闭，这是后续独立软件小项。

## 5. 逐项自审与回归

| 小项 | 检查和实际反例 | 结论 |
| --- | --- | --- |
| A01 共同 Codec | 精确 28/48/54+N、phase、短/长一字节、最大146、零写回；三语言独立向量 | PASS |
| A02 安全续接 | 实际 Session1→2 回读、新 Key/Policy；旧 Journal 不重绑定、不再 handoff | PASS |
| A03 原始结果交付 | 实际认证结果与重新计算摘要、ACK预留再复制；错误摘要/截止不改输出 | PASS |
| A04 精确 ACK | 错摘要/ID、不同认证 Client、接纳后撤权，均不 arm GC；重复 ACK 不续期 | PASS |
| A05 有界 GC | 保留期前拒绝、边界接受、实际墓碑回读、未成功 I/O 保留、失败围栏 | PASS |
| A06 掉电/不确定边界 | 假介质真实 raw-slot 重开，ACK丢失延迟GC、墓碑不复用、IN_DOUBT无ACK | PASS，Host假介质 |

新集成回归位于 `rust/crates/ucn-runtime/tests/operation_dispatch/retention.rs`，沿用真实
Security、Foundation 和假 Adapter/Driver，而不是伪造 event/proof/accepted 布尔值。
NoEffect/最大结果/故障案例分别检查其独立边界；旧 Query/Cancel/Receipt 回归均保留。
另扩展第五段真实耐久 Data 请求集成测试：OID1 签发 Operation2、Data 业务 opcode0、
原始请求、实际执行和终态 Data 回复后，再走真实 C0 恢复/ACK/GC，执行器始终只调用一次。
最后补独立媒体前进、RAM Journal 陈旧的反例，以及缺少 ACK Outbound ACL 的零交付反例。

逐项发现并修复了两处原 64 B 临时限制：seal 需要容纳最大200 B恢复正文，submit 不应
再次编码到64 B临时数组来取 Opcode，改为无正文复制的类型映射。最大结果正式回归达到
保护和 Driver 245 B 帧提交/退休；不是只有 Codec 双方自洽的短数据测试。

## 6. 整体自审和最终证据

第一轮回读检查合同→Codec→认证→回复→ACK→GC→恢复，落实了 IN_DOUBT 禁止直接GC、
ACK fault/reconciled 失败关闭和合法 retention 范围，以及最大回复容量；第二轮检查全域
绑定、真实当前媒体基线、逻辑/物理 Holder 分离、Feature OFF 和生产代码固定容量。
所有查询只是观察，公开 Owner 的 typed API 仍属于可信 Product 代码边界，不能被任意
内存改写攻击或不可信调用方用作防护承诺。MCU 最坏调用栈尚未测量。

Host x64 示例：OperationRecovery 224 B、OperationGc 168 B、完整 ControlCommand 368 B；
ControlCoordinator<2,128> 888 B，ControlReplies 的1/2/4槽示例为664/1560/4128 B。
恢复正文放在固定槽中，所以可选Control内存确实增加；不能宣称结果恢复“零RAM开销”。
该正文状态严格受 operation-control 裁剪；单独 service-rpc 不携带146 B结果槽，
已登记恢复Opcode在Owner关闭时返回Unsupported，不产生接纳状态。独立关闭配置lib测试3/3。
reply seal 在启用 operation-control 时使用200 B固定临时正文，其余装配保持64 B；
这只是明确的容量上限，不是MCU最坏栈或整机RAM证明，后续需测量真实调用链。

新增11组集成/对抗及1组Codec回归，共12组；另运行关闭配置lib测试3/3。
最终矩阵11项全通过，绑定378源码/合同依赖，运行中源稳定，最终逐文件SHA256复算差异0。
阶段性223/225结果不代替最后裁剪收口后的225项源码快照：

| 验证 | 最终结果 |
| --- | --- |
| Rust workspace Debug / Release / Rust1.85 | 各225/225 |
| Clippy all-targets / fmt | PASS / PASS |
| Feature/依赖隔离 | 16组合 PASS；Realtime/Group/Cluster互不依赖 |
| native C / Rust / Python字节Oracle | 33组 PASS；不是全Runtime互通 |
| no_std Cortex soft/hard | 编译检查 PASS |
| ESP32-S3 | 仅编译 PASS；未烧录、连接或复位 |

原始矩阵副本：[software-matrix-sw01-ack-resume.json](../软件证据/Rust/2026-10-14/software-matrix-sw01-ack-resume.json)。
SHA256=`2772E839F6A3236F8ED84338BA6D9ACFF9AD1A3C11B1FF13604BEC5A54CCEAB3`。
Windows GCC 14.2 Full 的既有 C Service/Operation/Control Wire 定向 CTest 已实跑 3/3；
这不是本轮 C 全配置矩阵。未新跑 TSan/Miri/ASan，未运行硬件或生产密码 Provider。
文档门禁137 documents / 333 links通过；`git diff --check`无空白错误，仅既有换行提示。
本轮29项文件全文扫描：源码/新增报告无尾随空白，台账保留3处既有Markdown双空格硬换行，
不把合法历史排版当源码缺陷。真实Git暂存区为空，源码哈希在台账写入后再次复算差异0。

## 7. 交接状态

本段是 RUST-SW-01 的有限 Host 软件接线；顶层仍 IN PROGRESS，外审、实机、生产 HOLD。
下一项为 C0/Data/Journal 统一有界调度和退休；Journal 槽安全复用、动态 Binding 续接、
可靠/Transfer RPC、Parent/Time 自主调度、动态/Flow、Group/Cluster 完整线上管线及
C↔Rust 全 Runtime 互通仍待做。真实 Flash/Witness、掉电注入、MCU 最坏栈、物理 Bearer、
生产密码及性能证据须用户另行通知并单独执行。没有提交或推送。
