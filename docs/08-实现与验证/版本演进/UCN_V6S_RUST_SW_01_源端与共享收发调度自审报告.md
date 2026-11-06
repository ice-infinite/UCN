# RUST-SW-01 第八、九段：耐久源端与共享收发调度

## 1. 范围与结论

2026-10-14，`v6-simplified-rust@03eb7eb` 加完整工作树。
两段均为 SELF-REVIEW PASS；RUST-SW-01 顶层仍 IN PROGRESS，外审和生产 HOLD。
本轮不操作 COM/USB、开发板、烧录或复位，不提交推送。
普通 Wire Header、Tag、Data Envelope、OPS1/OID1 Schema 和 C Storage Layout 未改变。

`SourceRuntime<F,B>` 是静态单 Peer、单活动耐久请求的客户端；
`DuplexRuntime<C,F,A,R,B>` 在同一个单 Link Adapter 和同一 Foundation 上组合客户端与服务端。
不是多 Peer/多 Journal 通用 Node，也没有把七类生命周期调度冒充 Q0～Q3 业务调度。

## 2. 调用与所有权

应用 `request()` 只复制业务到私有固定槽，不申请 ID、不调用 Provider/Driver。
后续 `step()` 核对当前 Session、Data ACL、最低保护等级以及未来恢复回复的真实 MTU，
才通过实际 Foundation 冻结 OID1 区间、submit/poll、回读确认并退休 Proof。
从确认区间签发的线性 ID 建立原请求；背压或丢包复用同一受保护原字节，
不更换 Operation ID、Origin Sequence 或执行 Deadline。

认证 Data 结果或原请求到期后，客户端进入独立、有界的终态查询寿命。
实际 C0 Resume 经 Security/ACL/Replay；Result 精确绑定原键和完整结果摘要。
在结果 ACK 槽与当前出站权限已预留后，才允许业务观察。ACK 到服务端后，
实际 Journal 保留、GC、墓碑回读和 Proof 退休沿用第六、七段路径。
查询丢失只重发原查询字节；不再次执行、不重新分配业务 ID。

Duplex 的唯一 Ingress Owner 领取原子完整 RX item，只用未认证 Header 作为分派提示：
Data Request / C0 Query、Cancel、Resume、ACK → 服务端；
Data Result/Error / C0 Operation Result → 客户端。
两个分支仍必须实际认证，Header 提示不能充当身份或权限证明。
子 Runtime 不公开可变句柄、不再次领取 RX；共同精确退休该 claim。
七类全局预算只计一次，每类 Source/Server 持久轮转；Foundation 一次访问最多推进一步。

## 3. 逐项自审

| 小项 | 结果与可区分反例 |
| --- | --- |
| 源端实际 ID 回读 | Provider 异步 poll；错当前 Record Digest/Generation 零推进；停止中已接纳区间完成并烧号，不发布请求。 |
| Source 当前安全预检 | O2 最低策略配 O1 Session、Data 可发送但恢复 MTU 不足，均在 ID I/O/nonce 前拒绝。 |
| 丢包与恢复 | 首两次 Request、全部 Data Result、首个终态回复分别丢失/损坏；同 ID/原字节重试，执行器始终一次。 |
| UNKNOWN 与停止 | Holder 不随截止或 stop 回收；仅模拟精确 late completion/reopen 证明退休，不自行操作硬件。 |
| 全局调度 | 单预算 RX 洪泛 140 次，各七类获得 20 次；两个节点同时作为客户端/服务端完成双向请求与 ACK/GC。 |
| 单 Foundation | 两个独立耐久 Domain 共用一个 Provider，实际 callback 次数不超过 Foundation 类访问次数。 |
| 时间/回调门 | global now 回退、零时间、Gate busy 零推进；调用门从实际 Security 配置取得。 |
| RX 静止 | stop 关闭 RX 后仍需退休队列/claim；publish 在分配门内重查 enabled，禁止停用检查与分配之间的窗口。 |

新增 Source 10、Duplex 2、Adapter 1 共 13 组正式回归。
两轮整体自审额外修正：RPC Complete 不能在客户端切入恢复前被退休；
源端最低保护不能只留在构造时；Data 已提交不代表远端接纳，需原字节有限重试；
全局时间水位不能依赖本次是否访问子 Runtime；停止不能遗漏未领取 RX。

## 4. 验证与证据

最终 Debug / Release / MSRV 1.85 各 **245/245**；Clippy `-D warnings`、fmt、
19 裁剪组合、33 组 native C/Rust/Python 字节 Oracle、双 Cortex、ESP32-S3 compile-only 通过。
11 项矩阵全部退出 0，386 项源码/合同依赖前后稳定，独立 SHA256 复算差异 0。

[原始矩阵副本](../软件证据/Rust/2026-10-14/software-matrix-sw01-source-duplex.json)
SHA256：`480206E0428A61A31944F45906FD54F359A20CE03DCB35F768A71774C4F03C6E`。
它绑定本阶段源码快照；后续 Parent 等实现不能继续冒用该结果证明新代码。

| Host 静态装配示例 | Nano 尺寸组合 | Lite 尺寸组合 | Full 尺寸组合 |
| --- | --- | --- | --- |
| Source Runtime | 5904 B | 7376 B | 10192 B |
| Duplex Runtime | 12912 B | 18240 B | 30112 B |

均为 Host `sizeof`、align 8，不包含外置 Foundation/Security/Adapter/Ingress/Provider、
运行时栈或 MCU 目标资源；不是最终 Profile 内存预算、MCU 栈或真实 Flash 证明。

## 5. 尚未闭环与后续授权

当前源端一次活动请求和服务端单 Journal 槽不自动循环复用；
客户端整机重启恢复还要求产品保存原 Operation Key，耐久 ID 分配器本身不是客户端请求 Journal。
多 Peer/多 Journal、可靠/Transfer RPC、Parent 发布、Time 物理打点 raw 管线、
动态 Admission/Route/Flow、Group/Cluster 全控制管线仍属软件工作，不是实机阻塞。
本段没有新增 C 全量 Runtime 互操作、生产密码、TSan/Miri/ASan、Flash/掉电或长稳证明。

用户本轮已明确授权继续补齐共同控制正文合同并同步 C/Rust；普通数据头不得增加字段。
后续先冻结 Parent 发布的精确正文、角色和失效规则，再实现两侧 Codec/Owner/调度，
不得用调用方 `peer_ready=true` 代替认证 Accept 或 Foundation Proof。
