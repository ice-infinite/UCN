# UCN v6 简化版 Rust 独立实现

本目录承载 UCN v6 简化协议的独立 Rust 实现。它与仓库中的 C99 实现共享协议规范、Wire
Registry、Golden、Negative 和持久化恢复合同，但不链接 C archive，也不通过 FFI 转发协议逻辑。

2026-11-06新增首轮`ucn-native`：静态发布订阅、最小`CoreNode::send`和typed Message外观，
复用真实Core/Adapter，不增加普通头。请先看[可运行接口说明](../examples/native/README.md)及
[本轮实现与整体自审报告](../docs/08-实现与验证/版本演进/UCN_V6S_原生API首轮四项实施与整体自审报告.md)。
首轮仅接显式可信静态O0网络，不是加密/RPC/RMW已接通的声明；Rust软件矩阵通过不覆盖C
全Profile遗留资源门禁或真实MCU。下列较早阶段状态保留其时间边界，不替代本轮范围报告。

当前状态（2026-10-18）：`RUST-08/09/10 = PARTIAL`。已列明 Owner/Codec 软件切片自审通过，
完整线上 Runtime、部分共用合同与硬件仍未完成；没有外审、生产或 RC 放行。

当前已经建立：

- Cargo Workspace；
- `no_std` 与禁止 `unsafe` 的默认边界；
- `ucn-types` 的强类型标量、Generation 与标准 Opcode/Suite Registry；
- `ucn-wire` 的 C0/C1 `O0/H0` 无分配、big-endian Codec；
- `ucn-owner` 的单写 Owner、精确 Callback Lease、公平 Mailbox 和 typed Coordinator；
- `ucn-adapter` 的固定容量 TX/RX Token、同步早到 completion 与原子 Driver Facts ingress；
- `ucn-core` 的静态 Binding/Path、Endpoint、C1/O0/H0 单帧 TX/RX、易失 Replay、四级固定队列、
  `6:3:2:1` 公平调度以及 `INITIALIZED→RUNNING→STOPPING→QUIESCENT` 生命周期；
- `ucn-persistence` 的 96 B Record Envelope、16 B Commit Marker、CRC32C、BLAKE2s-128、
  Durable Manifest、双槽、独立 Witness、同步/异步 Provider continuation、恢复选择和
  reload 后 immutable proof；
- `ucn-security` 的静态 Peer Session、完整握手候选证明、Persistence proof 后发布、O1/O2
  Origin 保护、H1/H2 逐跳/直连保护、固定 Replay Window、两阶段 mutation、精确 ACL 与
  Key/Session Generation 高水位；
- `ucn-identity` 的 Address Authority、保守 Lease Deadline、Binding Certificate、固定 Record、
  persist-before-use 与 reload-before-proof；
- `ucn-admission` 的认证前 Cookie、无放大/限流、严格六阶段 JOIN FSM、固定 pending 与有界
  Bootstrap 分片重组；
- `ucn-capability` 的 Capability Record/Summary/Query、认证 Peer Cache、Profile Select/Ack 和
  无副作用 Contract Resolver；
- C/Rust 共用的逐字节 Golden 与 Negative fixture；
- Host、Cortex-M 编译和静态检查入口。

后续已新增：

- `ucn-routing` 与 `ucn-flow`：SoftRoute、动态 RREQ/RREP/RERR 与不可变 Flow Stage/Commit；
- `ucn-digest` 与 `ucn-durable`：固定 scratch、规范摘要、恢复基线与精确 Foundation Proof；
- `ucn-transport`：Reliable/Receipt、Parent/ID、Setup/Fragment/SACK 与固定重组；
- `ucn-service`：RPC、结果保留、四类 QoS、Operation Journal/对账与 ID 区间；
- `ucn-realtime`：Domain/Envelope、真实 TimedLink reservation、原子 RX 与四报文模拟；
- `ucn-group`：Static Group fanout/receipt、独立耐久 Realm allocator 和逐槽共同 v2 Record；
- `ucn-cluster`：Stable/Joint、当前 quorum、完整票记录、Snapshot、Takeover/Handover/Fence；
- `ucn-runtime`：实际 Security/Replay/Receipt 的直接 C1 Reliable/C0 ACK 桥和真实 Adapter
  提交/退休桥，以及12类控制正文的认证/ACL/Replay→固定命令队列；不是完整业务 dispatcher。

当前明确没有实现：

- C2～C5 通用 raw Codec；RUST-05 仅在 Security Owner 内验证并生成当前 C2/C3 受保护布局；
- H3/C5 Group Security 与 C4 Routed Origin Flow Context；
- 全 Node 对 Identity/Admission/Capability/业务/可选模块的统一动态编排；已完成的固定 Peer
  JOIN/Reauth/Capability Root 和自动多跳单向 Data 子集不等于所有模块共用一个生产入口；
- 所有高级模块的统一 raw Wire/Control Body/安全 Context/Adapter 生产接线；
- 全部 Cluster 线上证书、Delta/Merge/Dissolve/Federation 与 Group Tree/H3；
- C admin 对独立 Group allocator proof 的完整编排（共同 Codec 已完成）；
- 新模块的 ESP32 固件/硬件证明、持久化 Replay 状态与生产密码 Provider。

历史 RUST-07 三节点动态路由实测不含新模块；本轮 xtensa compile-only 也不是烧录或运行。
剩余软件清单与硬件清单分开登记，不宣称“仅剩实机”。

## 裁剪与最短路径

| 组合 | 固定依赖边界 |
| --- | --- |
| ucn-core | 不引入 Persistence/Security/Transport/Service/Optional |
| Transport reliable-only | 不引入 Persistence/durable |
| Service rpc-only 或 qos-only | 不引入 Persistence/durable/Owner callback |
| Realtime local-only | 不引入 Persistence/durable/Group/Cluster |
| Group static-only | 不引入 Persistence/durable/Identity/Realtime/Cluster |
| Cluster | 不引入 Realtime/Group |

Optional Owner 由应用 Coordinator 显式装配，不直接调用其他 Owner。Header 不因编译高级
模块自动增大。未实现能力必须 Unsupported，不能 Advertise 成可用。某些 typed SPI 接收
“已认证事实”不等于该 SPI 自己完成密码验证。

## 构建

```bash
cd rust
cargo fmt --all --check
cargo clippy --workspace --all-targets -- -D warnings
cargo test --workspace
cargo check --workspace --target thumbv7em-none-eabi
cargo check --workspace --target thumbv7em-none-eabihf
```

仓库根目录可运行完整、无串口访问的矩阵：

```powershell
python tools/v6/run_rust_software_matrix.py --cargo C:\Users\y2389_4rq4ld9\.cargo\bin\cargo.exe
```

2026-10-13 共用合同收口后，Debug/Release/MSRV 各171/171、独立 Oracle 24/24、9个裁剪
组合、Cortex 两 target、ESP32-S3 编译通过。最新原始矩阵绑定364个源码/合同依赖。
这些结果不代表完整动态 Runtime 或硬件已经完成，详见[共用合同推进报告](../docs/08-实现与验证/版本演进/UCN_V6S_C_Rust共用合同第一轮收口与Rust推进自审报告.md)。

2026-10-14，RUST-SW-01 第一段补入 C1 Parent 的 Transfer 控制分派、固定缓存回复和真实
Adapter 退休。Debug/Release/MSRV 各177/177，13裁剪组合、24组Oracle与目标编译通过；
原始矩阵绑定367源码依赖。第一段范围见[控制分派自审报告](../docs/08-实现与验证/版本演进/UCN_V6S_RUST_SW_01_Transfer控制分派与缓存回复自审报告.md)。

同日第二段接通受保护 C1/H0 直达 OneWay 分片、固定重组、一次业务 Buffer 交付和实际
SACK/Terminal 回复。逐项及整体两轮自审通过；该段 Debug/Release/MSRV 各183/183，
13裁剪组合、24组Oracle与目标 compile-only通过，原始矩阵绑定370源码依赖。
Parent线上发布、Service/Operation/Time、发送端自主调度与动态/Flow仍未完成，
详见[认证分片与业务交付报告](../docs/08-实现与验证/版本演进/UCN_V6S_RUST_SW_01_认证分片重组与业务交付自审报告.md)。
没有操作实机，没有获得外审或生产放行；不要把复制 Buffer 解释为业务副作用成功。

同日第三段接通C0/H0 Operation Query/Cancel/Receipt观察、实际Foundation耐久取消和
失败围栏退休。逐项与整体自审发现并关闭终态分派方向缺口，补了完整回执字节容量门禁。
第三段Debug/Release/MSRV各193/193，14裁剪组合、25组三方Oracle与目标compile-only通过，
原始矩阵绑定372源码/合同依赖。详见
[Operation控制分派与耐久取消报告](../docs/08-实现与验证/版本演进/UCN_V6S_RUST_SW_01_Operation控制分派与耐久取消自审报告.md)。
这不是Service原始Request/Result完整接线，也不是执行器对账或durable结果ACK/GC闭环；
第三段结束时RUST-SW-01仍IN PROGRESS，下一软件项为Service RPC，不操作实机。

第四段接通C1/H0受保护BestEffort/Q2易失Data Request/Result/Error：实际Security/ACL、
原子RX、固定结果/报文预留、一次交接、不可续期缓存重试及Client Complete。缓存退休后
保留固定64位Operation高水位；同Session换Key/Policy也不复活旧请求。普通OneWay零附加。
第四段Debug/Release/MSRV各201/201，15裁剪组合、28组三语言向量、双Cortex及ESP32-S3
compile-only通过；原始矩阵绑定374源码/合同依赖，源稳定。详见
[Service原始请求响应与易失去重报告](../docs/08-实现与验证/版本演进/UCN_V6S_RUST_SW_01_Service原始请求响应与易失去重自审报告.md)。
不是C生产Runtime已互通、O2生产密码或实板证据。RUST-SW-01仍IN PROGRESS，下一项
原业务耐久Request建立/执行交接和Result生命周期，不操作实机。

同日第五段接通可裁剪durable-rpc：实际OID1区间、Data耐久Request、PREPARED/EXECUTING
两次Foundation回读、当前Journal代际与授权门、共享Gate内一次执行交接、终态回读与Data结果。
确认终态可恢复失去的回复缓存，不再执行；恢复EXECUTING只走InDoubt/对账。以既有2 B码
区分NoEffect/InDoubt，普通Header和OneWay不增加字段；对账RAM结果仍必须再次持久化。
第五段当时Debug/Release/MSRV各213/213，16裁剪组合、30组三语言向量、双Cortex/ESP32-S3
compile-only通过，376源码/合同依赖源稳定；C Service/Operation/Control Wire定向3/3。
详见[耐久业务请求与执行交接报告](../docs/08-实现与验证/版本演进/UCN_V6S_RUST_SW_01_耐久业务请求与执行交接自审报告.md)。
第五段结束时结果ACK/GC与跨新Session续接待做，以下第六段推进终态子集。

第六段接通C0/H0真实认证Resume/Result/ResultAck、当前Journal核对、ACK不续期保留、
实际Foundation墓碑回读。新Session/Key/Policy只允许稳定身份/Binding不变且当前ACL
有效时恢复终态结果，绝不重绑旧执行权限；IN_DOUBT无GC ACK，墓碑不得复用旧ID。
普通数据Header不增加字段；恢复控制Body分别28、54+N、48 B，最大结果Frame245 B。
结果正文GC不等于物理擦除或Journal槽复用，缺耐久高水位/重放寿命证明仍拒绝释放槽。
最终Debug/Release/MSRV各225/225，16裁剪组合、33组三语言向量、双Cortex和ESP32-S3
compile-only通过；11项矩阵绑定378源码/合同依赖，源稳定，哈希复算差异0。
详见[结果ACK、GC与安全续接报告](../docs/08-实现与验证/版本演进/UCN_V6S_RUST_SW_01_结果ACK_GC与安全续接自审报告.md)。
第六段结束时RUST-SW-01仍IN PROGRESS；统一调度、未完成执行/Binding迁移续接和其他动态管线
仍待实现，不冒充C↔Rust完整Runtime、真实Flash/生产密码或硬件GO。

第七段新增可裁剪`journal-runtime`，为静态单Peer/单Journal服务端接通C0/Data/Journal
七类有界step、持续轮转游标、串行取消/GC及精确Proof/Token退休。预算1和RX洪泛仍不饿死
其他类别；stop不抹掉已接纳写入，UNKNOWN不回收Holder；背压按原字节/截止有限重试。
实际当前Record Generation和Digest一起核对，同代际不同正文恢复对象在推进前拒绝。
普通Header、Tag、Envelope/OPS1 schema不变；本地队列/工作区和Digest增加RAM，非零成本。
最终Debug/Release/MSRV各232/232、17裁剪组合、33组三方字节Oracle、双Cortex与ESP32-S3
compile-only通过；11项矩阵绑定380源码/合同依赖，源稳定，独立哈希复算差异0。
详见[统一调度与退休自审报告](../docs/08-实现与验证/版本演进/UCN_V6S_RUST_SW_01_C0_Data_Journal统一有界调度与退休自审报告.md)。
需要专属单Link Adapter，不与其他Core/RX Owner共用；不是全节点Q0～Q3的替代调度器。
源端facade、多Peer/多Journal、易失/可靠/Transfer RPC、Parent/Time和动态全管线仍待做，
RUST-SW-01顶层仍IN PROGRESS。仅软件自审通过，无实机、外审或提交推送。

权威架构和实施顺序见：

第八/九段（2026-10-14）新增可裁剪 `source-runtime` / `duplex-runtime`：实际 OID1
回读签发、认证 Data/终态恢复/ACK/GC、原字节有限重试；同一个单 Link Adapter 上
唯一领取 RX 并实际认证分派，双向请求共享一份七类预算及 Foundation。
停止需排空 RX/Proof/Token，不自动回收 UNKNOWN。普通 Header/Tag/Schema 不变。
Debug/Release/MSRV 各245/245，19裁剪组合及33组三方Oracle通过；仅Host/目标编译。
详见[源端与共享调度报告](../docs/08-实现与验证/版本演进/UCN_V6S_RUST_SW_01_源端与共享收发调度自审报告.md)。
单Peer/单活动请求仍非通用Node；Parent/Time、动态/Flow、Group/Cluster等继续软件实施。

第十/十一段新增可裁剪 `parent-control` / `transfer-sender`：共同Parent32 B正文与
C/Rust纯Transport规则、实际媒体借用证明和认证发布、耐久ID→Setup→分片→SACK→
业务交付→Terminal。终态丢包重发缓存最后一片，不重新保护/烧nonce或交付业务。
普通数据头不增加字段；TX-only不配置RX缓存。250/250、21裁剪和41三方Oracle通过。
范围见[Parent与自主发送自审报告](../docs/08-实现与验证/版本演进/UCN_V6S_RUST_SW_01_Parent发布与自主Transfer发送自审报告.md)。
C同步共同Codec/摘要/纯规则，但未接通Parent完整Root；Rust亦非多Peer/Flow/RPC通用Node。
Time、动态/Group/Cluster及全Runtime互通仍是软件任务；保持外审/实机/生产HOLD。

第十二段新增可裁剪 `time-runtime`：实际Foundation/Domain binding、固定可信Master、
实际Security/ACL/Replay、同一原子完整帧/物理Timestamp、内部精确DELAY_REQ与T3，
四报文完成后交给实际TimeDomain。线性Sync序号不随UNSYNCED重置，退休失败保留义务。
普通数据头0新增；A1/H0/O1同步四帧为252 B/轮。Debug/Release/MSRV各257/257，
22裁剪、41Oracle、双Cortex及ESP32-S3 compile-only通过；Host组件sizeof2600 B非MCU总RAM。
范围见[Time认证事件链报告](../docs/08-实现与验证/版本演进/UCN_V6S_RUST_SW_01_时间同步认证事件链自审报告.md)。
仅固定安全直连H0单Peer；C完整Time Root、动态/多Peer/诊断及全局调度仍未完成。

第十三段（RUST-SW-02第一段）增加可裁剪 `capability-control`：已认证单Peer的
HELLO→QUERY→两端ADVERTISE→SELECT→ACK，通过实际Security/ACL/Replay和原子RX/
Driver推进；有限重试保留原认证帧、固定缓存和物理Holder。SELECT源视角、精确事务摘要与
REVOKE/过期高水位在C/Rust统一。普通数据头不增大；A1首次无丢包握手584 B。
它不完成动态JOIN、Suite/Key变更或多跳Route。C同步Codec/Catalog/认证分派，但C自动
握手Root尚未完成；不能把跨语言字节相等写成全Runtime互通。
见[共同合同](../docs/10-理论与规划/建议方案/UCN_v6_C_Rust共用合同收口/06-Capability认证协商与精确事务共同合同.md)
和[自审报告](../docs/08-实现与验证/版本演进/UCN_V6S_RUST_SW_02_认证能力协商自审报告.md)。

第十四段（RUST-SW-02第二段）增加可裁剪 `bootstrap-runtime`：专属单Link的原始
HELLO→Cookie→Echo→CookieVerified Pending，使用实际Adapter原子RX/Driver。
两端Link命名空间独立，Challenge沿用40 B；Owner传可信now给Cookie verifier，固定
16 B Cookie及独立签发/验证预算。早到回复、丢包、背压、UNKNOWN和停止均保留真实义务。
普通数据头0新增；A1首次三帧265 B，**不是完整JOIN费用**。完整身份互认证、耐久Binding/
Session发布及Peer Reauth还未接通，CookieVerified绝不等于ADMITTED。
Debug/Release/MSRV各269/269，24裁剪、52三方Oracle、7共享Golden及12配置矩阵通过；
ESP32-S3仅compile-only。C同步Codec/SPI/已有分派，未声称全Runtime互通或生产GO。
详见[共同合同](../docs/10-理论与规划/建议方案/UCN_v6_C_Rust共用合同收口/07-Bootstrap原始收发Cookie与待审事务共同合同.md)
和[逐项及整体自审](../docs/08-实现与验证/版本演进/UCN_V6S_RUST_SW_02_Bootstrap原始收发与待审事务自审报告.md)。

第十五段（RUST-SW-02第三段）接通实际AuthorityProof/DeviceProof分片收发和完整证据
验证；旧131～136草案值拒绝，使用Registry0004～0008。C同步严格379 B逻辑事件及36 B
分片Codec；普通数据头不变。认证前不知道对端MTU，默认按Cookie共同保证用62 B分块，
127 B/256 B异构Link也可收敛，不增加MTU协商字段。签名验证是本地认证进展，不是地址/
ADMITTED/Session发布；耐久JOIN后半段仍待。详见[共同合同](../docs/10-理论与规划/建议方案/UCN_v6_C_Rust共用合同收口/08-Bootstrap认证事件分片与身份校验共同合同.md)
及[逐项与整体自审](../docs/08-实现与验证/版本演进/UCN_V6S_RUST_SW_02_Bootstrap认证事件与身份校验自审报告.md)。

第十六段（RUST-SW-02第四段）新增可裁剪 `bootstrap-join`：最初本地Challenge锁存，
Authority地址预留回读后Offer，Device Binding回读后Commit，Authority精确复验后Final；
两端分别完成自己的Session Record回读才本地Active，不声称跨设备原子提交。共享实际
Callback Gate，当前Link/租约再校验；Evidence、方向Key槽和SessionDigest完整绑定。
普通数据头0新增；A1/Evidence32/无重传首次八类建链共4600 B，是建链费用而非逐包开销。

C同步共同Session种子和Oracle，尚非C自动JOIN Root或完整跨语言Runtime互通。本段结束时
H1到Capability尚未拼接；后续第十七段已接满足MTU的链路，Peer Reauth、多Peer和Route/Flow仍待。
详见[共同合同](../docs/10-理论与规划/建议方案/UCN_v6_C_Rust共用合同收口/09-Bootstrap耐久发布与Session建立共同合同.md)
及[分项与整体自审](../docs/08-实现与验证/版本演进/UCN_V6S_RUST_SW_02_Bootstrap耐久发布与Session建立自审报告.md)。
全程Host/compile-only，无实机、提交或推送；独立外审/生产继续HOLD。

第十七段（RUST-SW-02第五段）接通实际耐久JOIN到H1认证能力协商的一次性交接。旧JOIN
网络入口停止使用，新Coordinator继续复验实际Identity父域、Session和Link；精确ACL不放宽。
重试保持Origin事务/正文/Tag不变，仅用共享方向计数器刷新Hop。C/Rust同时分离本地入口
与远端Record的Link代际，禁止跨节点直接比数值。

普通Data头不变；第十七段未分片ACK136 B，128 B当时尚需后续认证分片，第十八段已接通，
不降H0。Crypto Scratch需大于Frame，集成静态使用192 B，不把该内存当线上开销。
详见[共同合同](../docs/10-理论与规划/建议方案/UCN_v6_C_Rust共用合同收口/10-JOIN后H1能力协商与交接共同合同.md)
及[分项/整体自审](../docs/08-实现与验证/版本演进/UCN_V6S_RUST_SW_02_JOIN后H1能力协商自审报告.md)。
仍无实机操作；Peer Reauth、多Peer、C自动Root、动态Route/Flow及外审/生产继续HOLD。

第十八段（RUST-SW-02第六段）补齐小MTU认证能力双片：新增0108，只有放不下的Record/
Ack带6 B局部元数据，普通Data头和七类原正文不变。每片独立Origin/H1认证；连续C0事务
推导组ID、75 B固定重组及双层ACL，完整后只发布一次。物理Token先退休再发下一片。

实际128 B Adapter/Root缓冲完成127/128 B链路；127↔136 B也通过，不猜对端MTU。坏Tag、
有效认证坏元数据、竞争组、旧组迟到、第二片背压/UNKNOWN、密码失败不发半组及Deadline
均有定向回归。C同步Codec/Registry和四条三方raw向量，尚非C自动Root或完整跨语言互通。

详见[共同合同](../docs/10-理论与规划/建议方案/UCN_v6_C_Rust共用合同收口/11-小MTU认证能力分片与重组共同合同.md)
及[分项/整体自审](../docs/08-实现与验证/版本演进/UCN_V6S_RUST_SW_02_小MTU认证控制分片自审报告.md)。
Capability ON每缓存槽静态增加第二帧，Feature OFF移除；无新增普通Data字段。仍不操作
实机；本段结束时Peer Reauth仍待，后续第十九段已接固定已知Peer子集。多Peer、动态
Route/Flow、生产Provider及独立外审HOLD。

第十九段（RUST-SW-02第七段，历史切片）接通Peer Reauth专属Root：双live Binding、真实前驱Session、
旧Session围栏与物理义务收口后，用四条身份签名消息推进后继代际；本端实际Foundation
回读前不发Response/Accept。不分配地址、不续租，不伪造认证View。C/Rust同步156 B正文及
18 B局部片Codec；真实127/128 B Host链建立后完成O1+H1 C1收发，普通Data头仍13 B。

详见[共同合同](../docs/10-理论与规划/建议方案/UCN_v6_C_Rust共用合同收口/12-Peer-Reauth邻接重新认证共同合同.md)
与[分项/整体自审](../docs/08-实现与验证/版本演进/UCN_V6S_RUST_SW_02_Peer_Reauth邻接重新认证自审报告.md)。
`peer-reauth`当前复用bootstrap-join基座；上述V1范围及未完成项是第十九段结束时的历史。

第二十段（RUST-SW-02第八段）接通Reauth自动Capability交接及两端耐久历史不一致恢复。
当前C/Rust共用208 B/V2正文，双方身份签名绑定各自真实前驱，专用入口推进共同最大值+1；
普通静态Session仍限制精确+1。保留ACCEPT重答缓存，唯一真实RX分派，发送/退休持续轮转，
单TX槽和127/128 B真实Host缓冲可运行。旧Link、错ACL、Unknown、父域到期均失败关闭。

32 B身份签名时每次Reauth控制交换1716 B；普通C1数据头仍13 B、新增0 B。Host实例
Reauth-only2304 B；同时编译Capability-control时6840 B，即使尚未arm也预留内嵌存储。
详见[当前共同合同](../docs/10-理论与规划/建议方案/UCN_v6_C_Rust共用合同收口/13-Reauth能力交接与耐久分歧恢复共同合同.md)
与[逐项/整体自审报告](../docs/08-实现与验证/版本演进/UCN_V6S_RUST_SW_02_Reauth能力交接与耐久分歧恢复自审报告.md)。
C自动Root、多Peer、跨语言Runtime互通、动态Route/Flow仍待；硬件/外审/生产继续HOLD。
本轮没有实机、暂存、提交或推送。

第二十一段（RUST-SW-02第九段）接通固定已知多Peer的认证路由控制Root：完整RREQ54/
RREP77/RERR99保留原始发现域；0206支持127 B局部有界分片。实际三节点、共享物理Link
双Peer、报告方本地代际翻译、丢回复/丢片重试及Unknown退休均有回归。C同步完整正文/
Codec/认证typed分派，C自动多Peer和分片Root仍未实现。
详见[第九段自审与证据](../docs/08-实现与验证/版本演进/UCN_V6S_RUST_SW_02_多邻居认证路由控制自审报告.md)。

第二十二段是[C1不透明Hop分层原语](../docs/10-理论与规划/建议方案/UCN_v6_C_Rust共用合同收口/15-C1不透明逐跳转发与计数器共同合同.md)，
不是自动业务Node。真实Host三节点先认证寻路，再用独立A/C Origin和AB/BC Hop收发；
中继不解密，目标独立校验Origin/Endpoint ACL。新增live Session计数器高水位，防止
模块交接时重置counter或Provider失败后重用值。普通数据头不变，O1/O2/H1尾部仍计费。
O2三节点Provider、统一Ingress/业务队列、C1的C适配、完整Flow和跨语言Runtime仍待。

第二十三段在上述原语之上新增 `route-data-runtime` 和固定容量 `RoutedNode`：普通业务
`send()` 后自动寻路，单一原子 RX 分派 C0 路由和 C1 Data，使用独立端到端 Session 与逐跳
Session。实际三节点正向/反向交付、127 B 控制分片、背压、TTL、Replay、Unknown 物理
义务和有限停止均有回归。本地 `PeerTxDriver` 明确提供共享 Link 的下一跳定位，不加线上
字段；A1 普通头13 B，O1/O2+H1完整额外费用32 B，不能只报 Header。

该子集只支持尽力单向 Data，端到端 Session 仍须产品事先真实发布。不是统一 Reliable/
Transfer/Journal/Time、动态端到端握手、Flow 或 Group/Cluster Root；C 自动接线与跨语言
Runtime 仍待。不操作实机、不提前生产放行。详见[共同合同16](../docs/10-理论与规划/建议方案/UCN_v6_C_Rust共用合同收口/16-自动多跳Data协调器与本地下一跳合同.md)
及[分项和整体自审报告](../docs/08-实现与验证/版本演进/UCN_V6S_RUST_SW_02_自动多跳Data协调器自审报告.md)。

第二十四段接通本地Origin登记的 `fence_origin → bounded step/drain → rebind_endpoint`：
共享Session的全部Service撤权，保留旧物理Holder/未读消息；真实ACTIVE后继通过当前父域、
严格代际和O1/O2策略检查后，才替换登记并更新Handle。不能借新Service空槽或旧墓碑跳过
Unknown义务。三节点在新Key下收发、有效Hop下拒绝旧Origin及五次固定槽复用均有回归。

这是已发布后继的本地接线，不产生跨多跳握手，也不是其他Runtime的全局排空证明；
C对等自动Root仍待。普通头/Body/Record不增字段，Host每Endpoint增加8 B。详见
[共同合同17](../docs/10-理论与规划/建议方案/UCN_v6_C_Rust共用合同收口/17-Origin登记围栏排空与耐久后继交接合同.md)
及[本轮自审](../docs/08-实现与验证/版本演进/UCN_V6S_RUST_SW_02_Origin登记续接与排空自审报告.md)。

第二十五段统一直连与多跳Origin/Hop接线：同一个真实H1 Session可分别提供两层Context，
不增加同Peer活动Session。精确认证邻居直接复验后发送，非邻居继续自动发现；相同send
接口可混发。Origin-only入口不证明Hop，完整packet入口不放宽Profile匹配。
共享Session撤权同时停止邻居角色，Control/Data/中继Unknown都进入本Root排空屏障。

本段没有增加普通头、Body或Record；C同步Origin投影原语，C自动Root、O2自动邻居控制、
邻居热续接及跨语言Runtime仍待。详见[共同合同18](../docs/10-理论与规划/建议方案/UCN_v6_C_Rust共用合同收口/18-直连多跳统一Origin与HopContext接线合同.md)
与[逐项及整体自审](../docs/08-实现与验证/版本演进/UCN_V6S_RUST_SW_02_直连多跳统一Context自审报告.md)。

第二十六段接通已知邻居的 `fence_neighbor → neighbor_is_drained → rebind_neighbor`。
新Session必须实际耐久ACTIVE，Capability须重新认证并原槽替换父域，不先删历史；
Control/Data/App/中继/RX义务排空前拒绝接纳。后继不继承旧Route/MTU/Profile，不自动
复活各Service；已有Endpoint仍分别交接。相同Root恢复直连/三节点、连续五轮槽复用。

普通Wire/Record新增0 B。Capability仍是易失缓存，耐久指Session。C同步叶Owner原语，
不是C自动Root已完成；完整自动Reauth→唯一Ingress/Node和本地Capability变更/多Bearer
热迁移仍待。详见[共同合同19](../docs/10-理论与规划/建议方案/UCN_v6_C_Rust共用合同收口/19-邻居围栏真实排空与耐久后继Capability热交接合同.md)
及[本轮自审](../docs/08-实现与验证/版本演进/UCN_V6S_RUST_SW_02_邻居围栏与耐久后继热交接自审报告.md)。

- [Rust 独立实现总体设计](../docs/10-理论与规划/建议方案/UCN_v6_简化版_Rust独立实现总体设计.md)
- [V6 简化版任务表](../docs/00-项目管理/00-任务表.md)
- [项目操作记录](../docs/00-项目管理/01-项目操作记录.md)
- [RUST-01 实施及自审报告](../docs/08-实现与验证/版本演进/UCN_V6S_RUST_01_基础类型Registry与C0C1_Wire实施及自审报告.md)
- [RUST-02 实施及自审报告](../docs/08-实现与验证/版本演进/UCN_V6S_RUST_02_Owner_Coordinator与AdapterToken实施及自审报告.md)
- [RUST-03 实施及自审报告](../docs/08-实现与验证/版本演进/UCN_V6S_RUST_03_最小静态通信实施及自审报告.md)
- [RUST-04 实施及自审报告](../docs/08-实现与验证/版本演进/UCN_V6S_RUST_04_Persistence_Foundation实施及自审报告.md)
- [RUST-05 实施及自审报告](../docs/08-实现与验证/版本演进/UCN_V6S_RUST_05_Security_Session实施及自审报告.md)
- [RUST-06 实施及自审报告](../docs/08-实现与验证/版本演进/UCN_V6S_RUST_06_Admission与Capability实施及自审报告.md)
- [RUST-07 实施及自审报告](../docs/08-实现与验证/版本演进/UCN_V6S_RUST_07_自动路由与Flow实施及自审报告.md)
- [RUST-08 Owner 实施及自审报告](../docs/08-实现与验证/版本演进/UCN_V6S_RUST_08_Transport_Service实施与自审报告.md)
- [RUST-09 可选 Owner 实施及自审报告](../docs/08-实现与验证/版本演进/UCN_V6S_RUST_09_可选模块实施与自审报告.md)
- [RUST-10 软件结果及后续门禁](../docs/08-实现与验证/版本演进/UCN_V6S_RUST_10_软件矩阵与后续门禁.md)
- [后续实机计划](../docs/08-实现与验证/实机测试计划/UCN_V6S_Rust后续实机测试计划.md)
