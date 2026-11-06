# RUST-SW-02 JOIN后H1能力协商自审报告

## 1. 范围

日期2026-10-15；`v6-simplified-rust@03eb7eb`加继承工作树。继续上一小节，接实际耐久JOIN
后的H1 Session与认证HELLO/Capability。保留所有既有改动，不提交、不推送、不操作实机、
COM/USB、烧录或复位；Obsidian只读，旧v5能力不作本轮完成证据。

共同规范见[第10篇](../../10-理论与规划/建议方案/UCN_v6_C_Rust共用合同收口/10-JOIN后H1能力协商与交接共同合同.md)。
Rust单Peer专属Root本段实现；C同步缓存域规则，不声称完整C自动入网、跨语言运行时互通、
128 B H1控制闭环或生产放行。普通数据头新增0 B。

## 2. 小项实施和即时自审

| 小项 | 实施 | 正向及反例 | 状态 |
| --- | --- | --- | --- |
| JC-01 | 一次性交接，Peer从实际Final/Session派生 | 未耐久/旧Token沿用原门禁；重复交接拒绝；成功后旧JOIN drive/receive/stop拒绝 | 定向自审通过 |
| JC-02 | H0/H1显式配置，实际Session profile冻结 | H1缺Hop计数器立即拒绝；不偷偷回退H0 | 定向自审通过 |
| JC-03 | 请求/回复共享每方向Hop计数，重试只刷新Hop | NOT_SUBMITTED、丢QUERY、丢ACK后收敛；同事务Origin字节相同、Hop严格递增 | 定向自审通过 |
| JC-04 | 当前Identity父域在交接后仍实际复验 | 私有只读Owner借用，不接受任意父域DTO；租期到点和Link reopen拒绝 | 定向自审通过 |
| JC-05 | C/Rust分离本地与远端Link代际 | 实际JOIN两端3/9成功；Summary错远端代际只QueryRequired；同父域改远端Link被拒绝 | 定向自审通过 |
| JC-06 | 明确最坏帧/处理MTU/固定缓冲/Scratch | A1 H1最坏136 B；128 B原子拒绝不交接、不发帧、不降级；短Scratch零写 | 定向自审通过 |
| JC-07 | 认证错误和计数生命周期 | 坏Hop、有效Hop配坏Origin、错Service ACL均拒绝；Provider失败输出不变但序号已burn | 定向自审通过 |

正式新增集成场景在`rust/crates/ucn-runtime/tests/bootstrap_runtime/capability_join.rs`，由
原Bootstrap测试入口加载。真实Foundation、Identity、Session、原子Adapter RX、Driver、
认证和Replay协同执行，不伪造OpenedPacket、PersistenceProof或AuthenticatedPeerView。
Host Provider为HMAC测试实现，不将其描述为生产密码套件；本段全链路验证O1+H1。

## 3. 第一轮整体自审：信任与状态

1. 没有绕过原始Challenge/Binding/Session回读；交接失败不改变原Root所有权。
2. 成功后旧Root不能再抢RX或stop共享Adapter；只读Session查询不等于远端可用。
3. 回读中发现原Capability Runtime会丢Identity父依赖，已改为借用实际Identity并逐次
   查询Authority/Binding。借用期间安全Rust不允许同时改变这个Identity；不是多Peer动态装配。
4. 当前Security、实际Gate、Link、Key/Policy和租期仍每次复验，历史Confirmed不单独授权。
5. Capability不是Key重协商，也不是业务ACL；缺精确Opcode/Service权限即使Session Active
   仍无法启动HELLO。退休职责可在父域过期后继续，不能借超时删除物理Holder。

## 4. 第二轮整体自审：线上费用、重放与共同实现

1. 发现双方把远端Link代际与本地入口代际比较的共同缺陷，C/Rust同时修正。首个Record
   冻结远端Link；同父域后续改变远端Link仍拒绝，不用“删除比较”放开所有变化。
2. 原始H1整帧重试会命中Hop Replay，已保持Origin不可变并刷新Hop。接收端仍验证双Tag，
   只允许精确Origin重放走有界缓存回复，不重复业务分派。
3. Packet刷新前复验当前本端身份/ACL；短Scratch、错ACL、旧序号拒绝且不burn。Provider
   已观察序号后失败则保留burn，Packet输出不变。普通Data头及七类能力正文没有增加字段。
4. 测试最初遇到144 B Crypto Scratch不足，原因是Hop AAD大于线上帧，改用显式静态192 B
   Scratch，不放宽帧MTU、不掩盖NoSpace。合同已区分两种预算。
5. 128 B不够放ACK136的问题保留为明确软件缺口，未宣称“全部软件完成、只等实机”。
6. Optional检查增加bootstrap-join+capability-control组合，不反向引入Service、Realtime、
   Group或Cluster；单独关闭两项Feature不要求对方存在。

## 5. 验证状态

定向Bootstrap测试23/23，既有Capability Runtime5/5；共同Capability Owner/Codec回归通过；
Clippy workspace/all-targets通过。C Full本轮重建108/108；Lite/Nano重建定向各3/3
（Admission、Capability、Security栈门禁）。未重跑fresh C MSVC/Clang/ASan/TSan全矩阵。

源码、共同JSON、工具冻结后运行12配置完整Rust矩阵，最终结果和不可变证据见下文结案段；
在矩阵完成和哈希复算前，不把先前286项旧快照当成本轮结果。

## 6. 未关闭事项

- 小MTU认证控制分片、Peer Reauth、多Peer、动态Route/Flow与后续业务Root交接仍需软件工作。
- C自动JOIN/Capability Coordinator、C↔Rust完整实际互通未由本轮证明。
- 实际Flash/Witness/掉电、生产密码、MCU任务栈/运行时/性能、物理Bearer和独立外审HOLD。
- 该自审不是外审GO，也不是协议整体RC或生产接口冻结。

## 7. 最终冻结结果

- Rust Debug、Release、MSRV1.85各289/289；Bootstrap定向23/23、原Capability5/5。
- Clippy workspace/all-targets、fmt、双Cortex和ESP32-S3 **compile-only**通过。
- 26个裁剪组合、69个C/Rust/Python共同Oracle、7个Admission/Capability Golden通过。
- 完整矩阵12配置全部exit0、`source_stable=true`；407项源码/合同/工具哈希独立复算，差异0。
- [本轮不可变快照](../软件证据/Rust/2026-10-15/software-matrix-sw02-join-h1-capability.json)
  SHA256=`106A746411B850022A85BBBB74330A5E7DE323FDB0ACC7C51E2B5F5D50A782B1`。
- Windows GCC C Full已重建并通过108/108；Lite/Nano定向各3/3，不冒充本轮全C工具链矩阵。
- 当前文档门禁146篇/376链；公共C API索引318项；`git diff --check`通过，仅既有CRLF提示。

矩阵后只补报告/台账和生成不可变证据，没有修改矩阵覆盖的源文件。缓存回复首次提交前
也刷新Hop，因为seal与submit是不同调度访问，可能被其他共享发送方向的工作拉开；序号
允许空洞。该低频控制路径不为省一次Tag计算而重用陈旧Hop，不增加线上字段。

最终状态：`RUST-SW-02 = IN PROGRESS / 本段Host自审通过`；独立外审、实机及生产仍HOLD。
