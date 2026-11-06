# RUST-08｜Reliable、Transfer、Service 与耐久 Operation

> 日期：2026-10-13。分支：`v6-simplified-rust`。
> 基线：`03eb7eb03098bfc7abd63f16e911e9d8fe45d26f` 加本轮完整工作树。
> 状态：`OWNER HOST SELF-REVIEW PASS / FULL WIRE INTEGRATION HOLD`。
> 本报告不是外部审计 GO，也不是 MCU 或生产放行。

## 1. 本轮交付与职责

新增 `ucn-digest`、`ucn-durable`、`ucn-transport`、`ucn-service`。所有生产 crate
均为 `no_std`、禁止 `unsafe`、固定数组，不链接 C 库。C 程序仅作为独立测试进程运行。

| crate | 唯一职责 | 不拥有的职责 |
| --- | --- | --- |
| ucn-digest | 调用方 scratch 内的 SHA-256、规范长度前缀与 streaming digest | 不读取业务 Owner、不分配堆 |
| ucn-durable | 精确恢复基线、不可变 Requirement、reload Proof 匹配 | 不调用 Provider、不推进 Foundation |
| ucn-transport | Reliable、Receipt、Parent/Transfer-ID、分片与重组 | 不授权 Peer、不调用 Driver、不执行业务 |
| ucn-service | RPC 关联、重复抑制、QoS 引用、Operation Journal 与执行交接 | 不提供链路成功证明、不直接调用其他 Owner |

跨模块由 Coordinator 编排。下文所有测试的 `flash::commit` 都是测试 Coordinator 将
Requirement 送入真实 Rust Foundation 的驱动器，不是业务 Owner 直调 Persistence。

## 2. Reliable：逻辑交付与物理提交分开

`ReliableKey` 精确绑定 Realm、两端 Principal/Address/Binding、Service、Origin Sequence、
Session/Key/Policy Generation、安全 Profile 与 Context Fingerprint。

发送先冻结完整 Origin 字节。重试读取原字节，不重签、重加密或重新分配 Origin Sequence。
Driver 的 `NOT_SUBMITTED` 退休本次物理预留、保留逻辑事务，并推进有限重试时刻；只有真实
`SUBMITTED` 才计入发送次数。达到 Deadline 或发送上限后可终结逻辑事务，但不能丢掉物理 holder。

```text
freeze origin -> reserve attempt -> actual Driver disposition
                   | not submitted: retire reservation, retain logical work
                   | submitted: retain holder until real completion/cancel
                   | unknown: retain holder, IN_DOUBT
target ACK -> logical terminal
all physical holders retired -> logical slot can recycle
```

ACK 必须来自反向精确身份域并匹配 Service/Sequence。已保存的终态 ACK 只能精确幂等重放，
不能被冲突 ACK 改写。Receipt 在 Replay commit 和应用发布之前预留；精确重复不再次交付。
Receipt 表满拒绝新消息，不驱逐旧完成记录；过期清理由显式有预算维护完成。

对应回归：`rust/crates/ucn-transport/tests/reliable.rs`。实际 Security/Adapter 组合见 RUST-10
软件报告，不能把低层 typed 单测当作所有 Wire Profile 的组合测试。

## 3. Transfer：Setup 冻结后才允许分片

| Body | 固定长度 | 关键约束 |
| --- | --- | --- |
| Delivery ACK | 9 B | 版本、Service、Sequence、status、credit |
| Setup One-Way | 43 B | Parent、Transfer-ID、总长、完整 digest、分片几何、寿命 |
| Setup Request | 52 B | 另含 Operation-ID 和 Flags |
| Setup Result/Error | 54 B | 另含结果码 |
| C1 Fragment Prefix | 12 B | Transfer-ID、Parent Generation、index、count |
| Flow Fragment Prefix | 8 B | 父域由活动 Flow 提供 |
| C1 SACK | 20 B | 明确子型、原 Service、Parent Generation、32-bit bitmap、credit |
| Flow SACK | 14 B | 明确子型及已绑定的成对反馈 Flow |

多字节字段均为 big-endian。Transport 仅搬运条件 Flags 字节，不私自分配尚未完整冻结的
Service 位含义；One-Way 不允许夹带 Operation 字段。不能据此宣称未知 Flags 可执行。

TX 必须先收到精确 Setup ACK 才能发送。`fragment()` 只预留准备项与 credit；实际
`fragment_disposition()` 得到提交结论后才允许 SACK 确认。NOT_SUBMITTED 返还 credit。
不能让“已经构造分片”冒充“已经提交硬件”。反馈必须匹配同一 Parent/Generation 与反向域。

RX 支持乱序、精确重复、冲突拒绝和固定重组。最终分片发布完成前计算规范全消息 digest；
最后一片错误时不发布 bit 或完成状态。应用通过不可复制的 `PayloadClaim` 持有完成 Buffer；
应用未释放时超时、取消和 GC 不能复用该 Buffer。

对应回归：`tests/transfer.rs`，包含 Setup-first、有限 credit、提交前 SACK 拒绝、背压 credit
回收、错反馈域、乱序、摘要错误、重复不续租、APP_HELD 与表满。独立 Oracle 同时比较实际 C
Codec、Rust Codec、Python 字段构造；不是双方 encode/decode 同错自洽。

## 4. Parent 与 ID 的恢复

C1 Parent 独立于 Session Generation。88 B `UCPT` Record 精确保存 Parent/安全域、
Transfer-ID high-water 与 Foundation transaction。真实 Foundation reload proof 前不发布
Parent 或 ID。线性的 `TransferPermit` 只能取一次；恢复后不重发已经分配的 ID。

Flow 的 allocator 只用于全新活动 Flow；Coordinator 必须为该 Flow 唯一持有 allocator，
不能用重新构造 allocator 复活旧命名空间。本轮尚未建立完整线上 Parent 握手和统一 Flow
allocator 注册，见第 8 节。

所有恢复 API 重新核对 supplied body 的长度与完整 digest，不能把另一份语法合法的正文搭配
旧 `DurableBase` 导入。Owner 保存已恢复/已确认的 durable record generation；未经恢复的
新 Owner 不能拿一个非空基线直接从 ID=1 再次写入。此门禁独立于 Foundation 的 CAS。

## 5. Service 与 QoS

One-Way 不分配 RPC 槽。Request/Result/Error 按完整 Service/Operation/身份/安全域关联。
同一请求的重复只产生 Pending 或稳定 Result Replay，不再次调用应用；冲突参数拒绝。
结果字节有固定保留空间，完成记录未退休时不会被新请求静默淘汰。

QoS 保存 Request 引用，不再复制整帧。四类固定队列按 `6:3:2:1` 调度，同类持久轮转游标
避免热点 Q1 使其他 Key 饥饿。Latest 只替换同一完整域下尚未选中/提交的项，不改写在飞
Attempt。Q0 需要明确控制授权；来源/Flow 配额不能被极小 Deadline 越权绕过。

## 6. 耐久 Operation

```text
PREPARED reload -> EXECUTING reload -> one linear executor handoff
                                      | confirmed result -> COMMITTED_RESULT reload
                                      | unknown -> IN_DOUBT -> query/reconcile, never execute again
retained result + ACK + retention -> durable tombstone
```

`OPS1` Record 与当前 C 的 110 B 固定部分及条件 Result 格式对应。结果观察、执行器交接、
结果确认各自有独立状态；EXECUTING proof 本身不是业务结果证明，不能据此提交默认空结果。
EXECUTING 恢复后禁止再次 handoff。外部副作用与 Journal 不能原子提交时保留 IN_DOUBT，
不承诺“无条件 exactly-once 且一定返回结果”。对账 Provider 只能 query，不能再次执行。

Operation-ID 通过 `OID1` Record 预留耐久区间；重启烧掉整个已预留区间。每次新区间仍须
Foundation Proof 后才可分配。已有非空 Record 但未 restore 的 Owner 拒绝新预留。

## 7. 分项及全体自审

分项回归、第一次全体回读、第二次逆向失败路径回读均已执行。自审期间修复了：

- 构造分片即标记 sent，导致尚未提交的分片可被 SACK 确认；现分离 prepared/disposition。
- EXECUTING Proof 与结果观察混淆；现只有实际观察/对账才设置 result_ready。
- supplied restore body 未重新绑定已验证基线；现所有新增 durable consumer 精确验证。
- 新 Owner 跳过 restore 后可能从初值重新分配；现绑定当前 durable record generation。
- C0 transaction 与 C1 origin sequence 共用 Replay 命名空间；见 RUST-10 报告。

本候选全 Workspace Debug/Release/MSRV 各 **166/166**；并不等于此 crate 单独有 166 个
测试。精确命令、源码哈希和目标编译结果见 RUST-10 软件报告及原始 JSON。

## 8. 未完成的软件与硬件边界

Owner/Codec 阶段通过，不把 RUST-08 顶层标记为生产 DONE：

1. C1 Parent 的线上 PREPARE/ACCEPT/COMMIT 及错误恢复尚未整合。
2. Setup/Fragment/SACK/终态反馈尚未全部经统一 raw Security/Adapter ingress 分派。
3. Service/Operation 共享 Opcode Body 和 Flags 的完整执行解释仍需共同合同冻结。
4. 真实 Flow allocator 注册与 Flow/C1 多跳高级 Runtime 尚未统一。
5. 真实 Flash、执行器、密码 Provider、MCU 栈、物理丢包与长稳未验证。

前四项是软件工作，不得归类为“等待实机”。本轮不修改 C 生产实现、不冻结新 Wire、不提交或推送。

后续见 [RUST-10 软件阶段与剩余门禁](UCN_V6S_RUST_10_软件矩阵与后续门禁.md)。
