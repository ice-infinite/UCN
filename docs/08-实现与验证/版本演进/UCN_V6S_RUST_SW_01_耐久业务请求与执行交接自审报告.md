# Rust RUST-SW-01 第五段：耐久业务请求与执行交接

> 2026-10-14；`v6-simplified-rust`，HEAD`03eb7eb`加完整工作树。
> 用户授权继续软件实现，并要求逐项和整体自审，不操作实机。
> 本段逐项及两轮整体自审通过；最终软件矩阵通过。外审、实机和生产HOLD。

## 1. 已接通的范围

第四段只有易失Data RPC；耐久业务不能因有Request/Result就自动获得掉电至多一次。
第五段复用其RpcBridge和固定报文缓存，新增可裁剪`durable-rpc`及单Journal的固定
`DurableRpcCoordinator<A,R>`，接通真实认证请求、实际Foundation回读、一次受控执行
交接、终态持久化和受保护Data回复。本段的跨Owner、持久化和物理链路由Runtime编排，
Service不直接访问Driver/Persistence Provider；既有typed执行器查询仍需后续统一调度。

范围为C1、H0直达、BestEffort/Q2、受保护O1/O2接口，执行Flags=2。Host端到端实际用
HMAC测试Provider验证O1；没有新增O2完整密码验证、生产密码认证或实板证据。对账/Executor
接口由Product在平台侧实现，须有界、非阻塞，并明确外部执行结果能否证明。
只读Obsidian的MCU-first/固定容量原则影响了装配；其旧版本能力描述不作为本段证据。

新主要实现入口：

- `rust/crates/ucn-runtime/src/durable_service.rs`：prepare/submit/finish_write/arm/execute/
  persist_outcome/publish及失败、停止、退休；跨Owner顺序集中于Coordinator。
- `rust/crates/ucn-runtime/src/service_data.rs`：durable endpoint、begin_durable_request、
  Persist接纳、终态Journal缓存恢复、认证结果分类，仍复用原报文生命周期。
- `rust/crates/ucn-service/src/ids.rs`：真实耐久区间签发的线性ID reservation。
- `rust/crates/ucn-service/src/operation.rs`：现有OPS1消费者、私有ExecutionPermit；
  新增当前Record代际查询，修正对账暂存结果在回读前不可发布。

这不是全动态Node自动调度器。Caller仍按有界事件/预算驱动Foundation step与这些入口；
不能将公开的Persist接纳动作当成执行权限，也不能直接改写Owner绕过流程。

## 2. 成功链路与实际运行顺序

1. Client按独立Manifest域预留OID1 ID区间，经真实write/readback/marker/witness/reload
   确认后才签发线性ID reservation。Request容量/配置/密码预检失败时可丢弃reservation；
   成功保护并发布请求后才一次commit ID。Origin Sequence仍由独立Owner分配。
2. Client请求经过实际Security和Adapter发送。Server原子RX item包含完整字节和Link事实；
   实际E2E/ACL、完整键、业务/回复容量预检之后提交Replay，接纳为Persist而不是Invoke。
3. Coordinator重新计算`Envelope9 || exact_business`的Payload摘要，匹配认证请求证据，
   核对Journal域、Schema、恢复基线和R不超过已预留回复容量，再冻结PREPARED。
4. submit只登记Foundation要求和私有I/O Handle；真正Provider工作只在有界step。
   finish_write必须取得精确实际reload Proof，确认后还要退休Proof，才进入Prepared。
5. arm复验当前权限/原RPC截止，冻结EXECUTING。第二次精确回读和Proof退休后只是
   ReadyToExecute；没有单靠“Provider返回OK”或Caller布尔启用执行器。
6. execute再次核对当前Foundation Ready/non-pending及同Journal代际，复验当前Security/
   Session/Key/Policy/Binding/Link和原截止，在共享CallbackGate内消费一次ExecutionPermit。
   回调拿到准确OperationKey、固定参数切片和已预留结果切片，没有事后堆分配。
7. 已知业务成功/失败都可成为COMMITTED_RESULT；Provider Err表示结果不能证明，不能
   用普通业务错误代替。未知、越界结果长度或保留状态码故障只形成InDoubt，围栏后不重执行。
8. persist_outcome写已知事实，不重新签发权限；即使原请求到期或stop，也可用新的有界I/O
   Deadline保全事实。其实际终态回读前，RPC结果仍不公开。失败须先围栏，再精确退休I/O。
9. publish复核当前Foundation代际、完整域、当前权限及原RPC截止，读取已确认Journal结果，
   解锁已预留Data回复；之后仍经原Security/Adapter链。Driver发送成功不删除Journal。

每个Product执行器只能承诺其能够证明的边界。协议保证的是不自动再次交接同一执行，
不是外部执行器内部不会重试，也不是“必定执行一次”或“必定拿到原结果”。

## 3. 不增加报文头的共同合同

| 项目 | 本段规则 |
| --- | --- |
| Request | 原Operation8 + Flags1 + business；Flags=2，仍9 B |
| Result/Error | 原Operation8 + Flags1 + code2 + business；Flags=6，仍11 B |
| Header/Tag | Common3 B、C1和Tag长度不变；A1/H0/O1的Request开销38 B，Result/Error40 B |
| OneWay/易失RPC | 不加Journal字段；普通OneWay仍0 B额外Envelope |
| Data opcode | 规范Data opcode=0；OperationWireKey的原业务opcode也允许0，外层Control Registry opcode仍非零 |
| 特殊码 | 仅耐久Data Error保留0xFFFD=NoEffect、0xFFFE=InDoubt，正文必须空；不增加phase字段 |
| 本地分类 | RpcPacketView.result_kind区分Business/NoEffect/InDoubt，不从C/Rust本地错误enum猜语义 |

本地Service status保持`Error=-1-code`，所以两个特殊码为-65534/-65535；它们不是
Journal中保存的取消/故障原因status，不能直接拿来与OperationReceipt摘要的status比较。
两保留值不能被普通业务失败冒用，Result角色和易失模式没有这些耐久结论语义。
Service Complete只是“最终观察已收到”，InDoubt不能触发新Operation自动重执行。

OperationWireKey仍28 B，Receipt仍48 B，Cancel仍32 B；Data业务opcode0及保留0xFFFF
校验在C/Rust同步，不重复添加Data Opcode。C99生产只改两处Service键校验及共同常量，
没有重接其全部Data Runtime，也没有改变C结构体/Storage Layout或Wire版本。
共同JSON及native C/Rust/Python Oracle核对这些值，不是Rust链接C实现来“互证”。

## 4. 每小项自审与正式回归

| 小项 | 回读及反例 | 结果 |
| --- | --- | --- |
| D01 ID分配与请求 | 未确认区间不能分配，丢弃预检不烧ID；耐久ID与Sequence独立；长参数拒绝 | PASS，实际Foundation区间与真实保护请求 |
| D02 PREPARED/EXECUTING | 两次回读前均不得执行，忙CallbackGate不消耗执行权限 | PASS，真实私有Proof/Handle，无布尔durable |
| D03 当前执行门 | 完整权限与原截止复验，不能凭旧RAM Proof跳过当前Journal | PASS，撤权/到期、实际新代际替换的反例 |
| D04 结果发布 | RAM执行结果、未回读终态不得公开；普通已知失败与未知结果分开 | PASS，已知负status提交、未知/越界InDoubt |
| D05 缓存恢复 | 已确认终态Record可恢复失去的回复，不暴露Replay明文或调用执行器 | PASS，实际raw认证Replay及媒体恢复，Witness不再递增 |
| D06 恢复执行 | 恢复EXECUTING只能InDoubt/对账；PREPARED须重新得到精确认证参数 | PASS，重建Foundation读原始双槽/Witness，不复制旧Owner |
| D07 I/O失败/stop | 失败回读前禁止丢I/O；先围栏再退休；超时后仍持久化真实NoEffect | PASS，改写已写raw字节导致真正readback失败 |
| D08 容量与状态码 | Executor结果容量必须提前与回复槽一致，未知结论不得与业务错误混淆 | PASS，过大R零Journal写、真实入站特殊码/错误正文回归 |
| D09 对账观察 | Query得到的新RAM结果不是durable，待再次终态回读才可见 | PASS，copy_result/Receipt零输出与确认后发布 |
| D10 裁剪及共同键 | 普通RPC不新增Journal装配；Data业务opcode0不等于外层Opcode0 | 定向Rust/原生C与字节Oracle通过，最终矩阵见第6节 |

本段新增9组durable集成/对抗、1组特殊码分类、1组业务opcode、1组对账发布回归，共12组。
多分支/循环断言不重复计为独立测试。旧5组易失RPC继续通过；定向service_dispatch共14组。

## 5. 整体自审发现与整改

第一轮从实际Source ID区间→Request→Security→Service→Journal→Executor→Data结果逐层
回读。没有把Source已认证、Event完成或typed DTO当作提交证明；参数摘要在Coordinator
中重新计算，Root类命名空间与原截止在每次授予新权限时复核。

这一轮补了执行器结果容量与实际预留回复槽的显式匹配；否则R比Endpoint/Service更大，
会在业务执行后才发现没有可用结果通路。回归验证R不匹配时Journal仍Factory Empty。

第二轮逆向检查失败、缓存、重建、stop和潜在旧Snapshot。发现并修正只核对RAM代际的
风险：执行与publish现在读真实Foundation的Ready/non-pending/Schema/Generation；
保留旧RAM Owner，同时由独立恢复实例推进实际媒体，原执行和原结果发布均拒绝。

同时收口特殊码的安全语义，用已有u16码明确NoEffect/InDoubt，与普通业务错误分离，
客户端实际认证这些结果并检查空正文。还发现已有reconcile会改写RAM结果而Journal
仍是InDoubt；现在reconciled暂存阻断copy_result/Receipt，确认新终态后才重新开放。

缓存恢复只允许已确认终态，生成新的有界检索回复不等于执行续期；未退休物理Token仍
由RpcBridge持有，Coordinator退休不会删除它或Journal。没有把超时当GC或执行许可。
新Controller无法据当前旧记录自动恢复已撤销Authority；caller-owned RAM也不是内存破坏
或掉电防护。未发现本段自审范围内新的未整改项，但不宣称全协议P0/P1为0或外部GO。

## 6. 本轮验证

最终11项软件矩阵全部exit=0，376项源码/合同依赖开始/结束一致，
`source_stable=true`，最终逐项哈希复算差异0。复跑命令：

```powershell
python tools/v6/run_rust_software_matrix.py --cargo C:/Users/y2389_4rq4ld9/.cargo/bin/cargo.exe
python tools/v6/check_v6_current_docs.py --root .
git diff --check
```

| 验证层级 | 本轮实际结果 |
| --- | --- |
| Workspace Debug / Release / Rust 1.85 MSRV | 各213/213 PASS |
| Clippy all-targets / fmt | PASS |
| 可裁剪组合 | 20 packages / 16 combinations / 3 isolated optional Owners PASS |
| native C/Rust/Python Oracle | 30/30 PASS，包括Data业务opcode0和特殊结果码 |
| Cortex soft/hard / ESP32-S3 | 三target compile-only PASS |
| 新durable集成/分类/业务键/对账回归 | 12组已包含在213中，不重复累计 |
| Windows GCC Full C定向 | Service/Operation/Control Wire CTest3/3 PASS |

最终原始副本：
[software-matrix-sw01-durable-rpc.json](../软件证据/Rust/2026-10-14/software-matrix-sw01-durable-rpc.json)，
SHA256=`77E073EFD0B4BA84ECD15FF56FB7D8AEF20FD0FFC7D1F8EF55951CF38DDCF1AC`。
保留前四段独立证据，不覆盖它们；上一段201项矩阵不代替本段213项快照。
文档门禁136 documents / 329 links通过；差异检查无空白错误（仅既有CRLF提示），
25项本轮文件显式检查尾随空白0；真实Git暂存区仍空，没有据测试结果自行提交。

Host `DurableRpcCoordinator<16,16>/<64,64>/<128,128>`为272/368/496 B，alignment8，
仅含单Continuation及预留参数/结果，不含OperationOwner、Foundation、Security和RpcBridge。
当前默认durable装配下的RpcBridge示例`<1,128>/<2,256>/<4,512>`为680/1448/3760 B；
新增的是按需本地结果容量/分类信息，不是每帧字段。普通Core不链接这些Runtime状态，
service-rpc单独配置不加Journal；它原有的Security Session依赖不会因此消失。

资源以Host sizeof及目标compile-only分别标注；不把数组公式冒充MCU Flash/栈水位。
本轮未运行真实Flash掉电、生产密码Provider、物理Bearer、MCU IRQ或执行器实物测试，
也未新跑C全配置矩阵、TSan/Miri/ASan。没有提交或推送，已有C/Rust工作树修改全部保留。

## 7. 未完成的边界

RUST-SW-01顶层仍IN PROGRESS，不能以本段结果标为全协议完成。

- 结果ACK/GC及持久化墓碑/槽复用的完整证明尚待接线。收到Data Result、发送完成或
  业务窗口过期均不删除Journal；不能用特殊结果码代替Receipt摘要或对账证明。
- 冷启动Session重建及跨新Session/Key/Policy/Binding的安全续接仍待共同合同和Runtime；
  当前必须完整域匹配，未知变化拒绝，不为了回放旧结果重新激活旧权限。
- Host恢复测试重建的是Journal/Foundation媒体状态，并保留独立的已认证当前Security
  Context；它不是整机断电重连实测，更不证明真实物理Witness。
- C0取消与Data Continuation的统一并发编排、Repeatable/可靠/Transfer RPC、Parent发布、
  自主发送调度、动态Flow/多跳、Time以及其他可选模块的完整管线仍按后续任务推进。
- C生产Runtime仍未声明耐久Data RPC互通；C↔Rust端到端、目标ELF/最坏栈、长稳、
  物理Flash/执行器原子性与产品放行是独立门禁。

下一软件小项可继续结果ACK/GC与安全续接；任何实机操作必须等待用户另行通知。
