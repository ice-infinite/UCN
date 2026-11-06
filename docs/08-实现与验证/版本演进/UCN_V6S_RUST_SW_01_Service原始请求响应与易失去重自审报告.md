# Rust RUST-SW-01 第四段：Service原始请求响应与易失去重

> 日期：2026-10-14。分支`v6-simplified-rust`，`03eb7eb`加完整工作树。
> 本段逐项及整体回读后补了错Owner退休门禁；整改后最终软件矩阵全部通过。外审、实机和生产仍HOLD。
> 不操作COM、USB、烧录、复位或物理板；没有提交或推送。

## 1. 目标、前因和边界

上一段Operation C0 Query/Receipt/Cancel已接通，但Receipt48 B是耐久观察，不是RPC结果
正文，不能让普通Service请求自动Complete。本段接通已有Data Request/Result/Error的
条件Envelope，以及最小直达易失RPC闭环，不继续向普通Header添加字段。

原合同已经允许C1 Data的Request/Result/Error，Rust Security却统一拒绝非OneWay Data。
本段解除这条额外限制，并在Wire/Security共享严格的9/11 B Envelope结构校验。
零Operation、短正文、保留Flags和错误FINAL位在写输出/Replay前拒绝；OneWay不检查
RPC前缀，不增加字节。最高合同与低开销说明也明确“应用Data Envelope”，消除把协议
Control Body再套一层的旧泛化措辞；本段没有发明新的Wire版本或第二套RPC格式。

实际接线范围：C1、H0直达、BestEffort、Q2、VOLATILE_DEDUP；要求O1/O2。
Host端到端用实际HMAC认证测试Provider验证O1；O2使用既有Provider接口，本段未新增
O2加解密端到端证据，更不是生产密码验收。Repeatable/Durable、Reliable/Transfer RPC、
多跳/Flow、动态Binding、自动Planner和自主重试定时调度均不冒充完成。

## 2. 模块分工和成功路径

`ucn-runtime/service-rpc`是可裁剪Coordinator桥，不是把Security/Adapter塞入Service。
Service Owner保持业务关联、固定结果区及易失receipt；Security负责真实认证/ACL/Replay；
Adapter负责原子RX item、准确物理Token和Driver终态。跨Owner调用只有Runtime编排。
Obsidian仅用于只读核对MCU-first、固定容量和边界，不以旧笔记覆盖当前源码。

1. Product安装`RpcEndpoint`，指定固定Service、Peer Principal、Request/Result权限、
   业务容量及最低安全级别。普通Data ACL不会自动允许业务Request回调。
2. Client begin从当前唯一OriginSequenceOwner取第一次序号，作为本Session的Operation ID；
   本地完整ServiceKey绑定Realm、两端Principal/Address/Binding、Session、原请求Key、
   Policy、Fingerprint、Service和Operation。序号不得复位或回绕。
3. 预检固定报文/Service容量和Link MTU，建立WaitSend，再经真实Security保护一次。
   seal失败清理未发送关联；允许烧掉不再复用的代际/序号，不保留可发送伪结果。
4. submit持有不变报文，先登记Token Holder再调用Driver。NOT_SUBMITTED只退休本次Token，
   保留逻辑Request；实际Submitted才进入WaitResult。发送成功只证明本地物理Attempt完成。
5. Server从原子RX item取完整字节/Link facts，经Security认证，验证Envelope模式及完整域；
   先预留业务结果槽、允许的最大回复容量和Rpc缓存，再提交Replay及线性Service预检。
   只有新请求返回Invoke和一次业务正文；重复返回Pending/Replay，不复制正文。
6. Product在该Invoke交接中处理业务；respond一次冻结Result/Error正文。seal失败仍保持
   AwaitSeal，可重新保护冻结结果，不能再次调用业务或覆盖结果。
7. Server回复也经过实际Security/Adapter；Client实际验证其身份、精确原请求域、ID、模式、
   Deadline和正文，先完成线性结果预检再提交Replay/Complete。相同认证结果幂等，冲突拒绝。

本桥签发唯一新请求交接，不是防恶意Caller重复执行自己代码的沙箱，也不提供外部执行器
原子性。Caller不得直接改写Owner；保留Driver的DMA/异步Buffer期间必须保持静态存储地址。

## 3. 固定资源、开销和状态语义

| 报文 | 条件Payload布局 | 本段A1/H0/O1的非业务开销 |
| --- | --- | ---: |
| 普通OneWay | business | C1 13 B + Tag16 B，未增加RPC字段 |
| Request | Operation8 + Flags1 + business | 13 + 9 + 16 = 38 B |
| Result/Error | Operation8 + Flags1 + code2 + business | 13 + 11 + 16 = 40 B |

不增加2 B Data Opcode；规范ACL opcode固定0。Request Flags=1；Result/Error Flags=5。
线上code仍u16；本地Service结果用`Result=code`、`Error=-1-code`，避免两角色丢失或混淆，
合法区间为0..65535与-1..-65536。Deadline达到边界即失效，重复不续期。

RpcBridge使用编译期N个固定报文槽、每槽F字节、一个F字节staging和固定64位Operation窗口。
Service结果区仍单独由其Profile决定；没有动态内存，没有整Owner/Journal快照副本。
Server receipt缓存退休不清除去重历史；同Session换Key/Policy仍不能复活已见Operation。
64窗口允许有限乱序，太旧ID拒绝，不用驱逐历史来重新放行。新Session只在相同精确父域
下重置，旧Session拒绝；动态地址/绑定迁移本段Unsupported。

这是易失去重，不是掉电后至多一次。Source重启须由既有Session持久化/回读与新代际防复用；
Server掉电不保存业务执行事实。要求durable模式的业务必须等下一项Journal接线，不能
在无法兑现时自动降级为易失执行。普通Core不链接这个可选Runtime桥。

## 4. 每小项自审

| 小项 | 自审重点 | 实际回归与结论 |
| --- | --- | --- |
| SW01-RPC-01 | Data合法角色不能被额外拒绝；坏Envelope先拒绝；普通OneWay不增大 | Wire条件9/11 B、短/零ID/坏Flags/FINAL及零写回归通过 |
| SW01-RPC-02 | 资源预检失败不能写Service或消耗Replay | 两种线性Service admission丢弃无副作用；短输出、满槽、超模式、回复MTU不足后同序号仍可接纳 |
| SW01-RPC-03 | 原始Request→业务交接→原始Result必须实际认证 | 实际HMAC、原子RX claim/retire、Adapter Submit/Complete/Retire，业务正文与冻结结果精确核对 |
| SW01-RPC-04 | 发送完成、丢回复和重试不得伪造业务完成 | 本地成功仍WaitResult；Pending/Replay不再Invoke；原字节/Tag/序号缓存重发，计数器不再消耗 |
| SW01-RPC-05 | 缓存退休和Key/Policy变化不复活旧Operation | 退休后新OriginSequence重放旧Operation拒绝；64窗口乱序、过旧、换Policy/Key、新旧Session边界通过 |
| SW01-RPC-06 | stop/背压/未知Driver不丢Buffer义务 | 连续3次NOT_SUBMITTED后真实Submit；持续背压到原Deadline；Unknown保持Holder，reopen后准确晚到终态才退休 |
| SW01-RPC-07 | 结果角色、全键和权限不能混淆 | Error65535映射-65536；错误Operation、相同generation的外部Link、坏Tag、冲突正文、发送前结果拒绝；O2最低策略拒绝O1 |
| SW01-RPC-08 | 裁剪和三语言合同不得靠Rust FFI“互证” | 新service-rpc独立组合；三组native C fixture / Rust encoder / Python struct对照；不声称C Runtime互通 |

新增5组runtime集成/对抗、1组窗口边界、1组Service线性预检、1组Wire回归，总计8组。
它们是成组测试，不把每个断言、循环或已有重跑重复累计为新增测试数量。

## 5. 整体自审

第一轮顺着成功路径全文回读Types→Wire→Security→Service→Runtime→Adapter，确认实际
证据来自Security，不接受Caller的“已认证”布尔；应用只收到业务正文，结果须再次认证。
将入站Result全Realm/地址/Binding/Session/Policy/Fingerprint逐项匹配显式写入入口。
线性Replay handle一次消费，失败preflight明确abort，不能克隆或重复退休。

第二轮逆向检查所有失败/stop/超时/退休分支：Driver Unknown必须保留物理Holder，而不是
凭迟到软件成功立即释放；回归尊重既有Adapter围栏，先reopen再交回准确物理终态。
Service结果完成与Rpc缓存的物理退休分离；Server未到期不能退休，过期不自动复活请求。
普通Data ACL与RpcEndpoint独立，回复最大容量必须在Invoke前保证，禁止业务已处理后
才发现没有任何回复Buffer。检查器不能代替上述逐分支回读。

收口回读进一步发现Server退休错误地把任意Service ERR_STATE当成“过期槽已清除”，
传错Service Owner时可能清掉本地缓存。已改成Service精确Owner/私有Handle命名空间
校验的幂等退休：已退休旧代际零写成功，不能删除新的槽；错Owner拒绝。
Runtime不再吞掉ERR_STATE；回归同时检查错Owner零写、Service提前step后正确退休，
以及旧Handle不能删新receipt。整改后重跑整套矩阵，先前矩阵不作为最终候选。

未发现本段自审范围内新的未整改问题，但这不是全协议“P0/P1=0”或外部GO声明。
所有检查覆盖的Host Provider与边界仍须独立复审及物理验证。

## 6. 本轮可复跑证据

最终11项矩阵全部exit=0，374项源码/合同依赖开始与结束完全一致，
`source_stable=true`。历史193项证据不代替本段新增逻辑的最终201项快照。命令：

```powershell
python tools/v6/run_rust_software_matrix.py --cargo C:/Users/y2389_4rq4ld9/.cargo/bin/cargo.exe
python tools/v6/check_v6_current_docs.py --root .
git diff --check
```

| 门禁 | 本轮实际结果 |
| --- | --- |
| Workspace Debug / Release / Rust 1.85 MSRV | 各201/201 PASS |
| Clippy all-targets / fmt | PASS |
| 裁剪组合 | 20 packages / 15 combinations / 3 isolated optional Owners PASS |
| native C fixture / Rust / Python Oracle | 28/28 PASS |
| Cortex soft/hard / ESP32-S3 | 三target compile-only PASS |
| 定向RPC集成、Service、窗口/Wire回归 | 新增8组已包含在201中，不重复累计 |

原始副本：
[software-matrix-sw01-service-rpc-final.json](../软件证据/Rust/2026-10-14/software-matrix-sw01-service-rpc-final.json)，
SHA256=`9E19241B37858DC4D95998D67125E8742D80E72B88CC0EC26551DFA59F6D919F`。
保留前三段证据，不覆盖它们。最终逐项源码SHA256复算差异0。第一次
`software-matrix-sw01-service-rpc.json`同样201项，但在错Owner退休整改前生成，
仅保留为历史中间证据，不能据测试数量相同把它当成最终候选。

本轮Host RpcBridge的`<1,128>/<2,256>/<4,512>`示例分别672/1432/3728 B，alignment8。
这包含固定报文槽与staging，不包含另行选择的Service结果区和Security/Adapter；
不是整机Nano/Lite/Full总RAM，也不是目标Flash/最坏栈实测。开启与静态装配才有该资源。
普通Core仍无runtime/Service链接依赖，service-rpc不额外启用Journal/ucn-durable。
Security自身的当前Session Provider依赖不因此消失。

不是实板测试；C全量CTest、TSan/Miri/ASan本轮没有新跑。
未新增修改C生产实现，只修改native C测试fixture与共同JSON/Oracle。
文档门禁135 documents / 325 links通过；差异检查与18项本轮文件的尾随空白检查通过。

## 7. 剩余工作

RUST-SW-01仍IN PROGRESS。下一软件项是原业务耐久Request建立、Journal
PREPARED/EXECUTING真实回读、一次Executor交接、Result发布、重启InDoubt、ACK/GC闭环。
本段易失Result不能充当durable commit proof，不能据它删除Journal。

Repeatable/可靠与Transfer RPC、Parent线上发布、发送端自主调度、动态Flow/多跳、Time、
Group/Cluster全管线及C↔Rust运行时互操作仍按任务表后续推进。外审与实机另行进行，
本轮不提交、不推送，不声称已经达到硬件或发布边界。
