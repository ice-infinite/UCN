# RUST-SW-02 小MTU认证控制分片自审报告

## 1. 本轮范围

日期2026-10-15，分支`v6-simplified-rust@03eb7eb`加继承工作树。继续用户授权的软件推进，
不操作实机、COM/USB、烧录或复位；不提交/推送，不改Obsidian。只读知识库用于MCU-first、
固定容量和Host无隐式特权核对，旧v5结论不作当前实现证据。

本段接通127/128 B H1能力分片，保留普通数据头。统一C/Rust的局部6 B正文和Opcode0108，
不是完整C自动Root、全部Control/Transfer分片、跨语言运行时互通或生产密码放行。
共同规范见[第11篇](../../10-理论与规划/建议方案/UCN_v6_C_Rust共用合同收口/11-小MTU认证能力分片与重组共同合同.md)。

## 2. 每小项完成及即时自审

| 小项 | 实施 | 区分性反例 | 自审 |
| --- | --- | --- | --- |
| CF-01 | Registry0108、C/Rust固定双片Codec | 独立Record/Ack×两片raw向量；长度±1、Opcode、总长、count/index、短输出和哨兵 | 通过 |
| CF-02 | 两片独立Origin/H1认证，推导连续C0组base | 非零、相邻、checked无回绕；不同组/域/原Opcode不得混合 | 通过 |
| CF-03 | 双片seal成功才可提交，复用原Origin只刷新Hop | 第二片密码失败，两片都未发且序号burn；重建新组再收敛 | 通过 |
| CF-04 | 每条回复最多一个实际Token，先退休第0片再发第1片 | 精确按片号注入第二片NOT_SUBMITTED和UNKNOWN；不误伤QUERY | 通过 |
| CF-05 | 固定重组、逐片Replay/双层ACL、完整才发布 | 只有0108权限但无原ADVERTISE权限拒绝；原权限有而包装权限缺失也拒绝 | 通过 |
| CF-06 | 无覆盖、无续期和重放高水位 | 有效认证坏count拒绝；竞争组NoSpace不破坏旧组；旧ADVERTISE不能覆盖部分ACK | 通过 |
| CF-07 | 原子RX、完整聚合等待队列及有界退休 | 原截止到期不能完成部分ACK；stop清易失槽但UNKNOWN Token仍保留 | 通过 |
| CF-08 | 当前双方MTU和预协商下限 | 实际128 B缓冲跑127/128 B；另跑127↔136 B，所有物理帧不超过接收端MTU | 通过 |
| CF-09 | 旧单帧/裁剪与共同Oracle | 136 B及H0旧路径保持；关闭Capability不带新缓存；四条三方独立规范字节 | 定向通过，完整矩阵结案段记录 |

正式入口`rust/crates/ucn-runtime/tests/bootstrap_runtime.rs`共31项、旧Capability Runtime5项；
新增纯Codec2项。内部Fixture使用真正Foundation、Identity、Session、原子RX与实际Driver
接口，不伪造OpenedPacket/Authentication/PersistenceProof；密码为Host HMAC测试Provider。

## 3. 第一轮整体自审：认证、状态和失败

1. 每片携带完整Origin与H1保护，没有为了MTU降级O0/H0或删除Tag。
2. 原Opcode、长度、Role和Service约束在认证之后复核；原权限和包装权限必须同时成立。
3. 两片间的Source/Destination、Session、Key/Policy和Fingerprint精确一致。首片凭据只作
   身份/缓存键，新增is_fragmented说明，未把它伪装成全正文Digest。
4. 第二片seal失败不出现Ready或Driver发送，已暴露Nonce不复用；分片物理失败不丢原缓存。
5. 乱序第二片先到不发布命令，竞争组不能覆盖未完成槽；完整正文只处理一次。
6. 高水位跨已发布组替换保留；旧组即使Hop重签有效也不能覆盖新组。绝对Deadline不续。

## 4. 第二轮整体自审：调度、MTU和资源

1. 发现完整重组等待入队可能阻止旧命令出队，已改为先处理已排队命令，再发布等待聚合。
2. 发现只按本机MTU选择会破坏异构链路，已用预协商共同档及认证后双方最小MTU；新增
   127↔136 B正式回归，不以大本机缓冲冒充远端支持。
3. 定向测试从144 B夹具补强到真实128 B Adapter/Root容量；修正reopen测试本身错误的
   136 B参数，不扩大缓冲或跳过该检查。127/128 B全链实际收敛。
4. 停止退休显式清理重组；quiescent必须同时满足重组、逻辑队列和全部物理Holder退休。
5. 第二片故障改按Opcode/片号定向注入，避免轮转中的QUERY提前消耗注入造成假覆盖。
6. 6 B只属于分片正文。Capability ON的通用回复缓存每槽增加静态第二帧存储，明确记录
   RAM取舍；Feature OFF移除该状态。不将Host资源或交叉编译当目标实测。
7. 冷启动费用重算：小MTU899 B；双方大MTU因首个Record仍未知对端，默认753 B，而非
   把已知双方MTU的理论680 B写成冷启动默认值。普通数据头仍0 B新增。
8. 首轮完整299项矩阵后，最终回读补强H1物理Attempt至少2的配置门。预算1在交接前
   原子拒绝、Session及Driver帧数不变；定向复测后重跑全部12配置，不沿用修改前快照。

## 5. 验证与最终证据

源码/JSON/工具冻结后执行完整Rust矩阵；完成后只补报告和不可变证据，独立复算全部依赖。
初步定向：Bootstrap31/31、旧Capability5/5、纯分片Codec2/2、Clippy全workspace通过；
C Full重建，Lite/Nano重建定向各4/4；三方Oracle由69增加到73向量。
最终结果、哈希和机械门禁见结案段，不把前轮289项快照套用于本轮。

## 6. 保留限制

- 本段是A1、单Peer专属H1 Capability链；O2没有在本段Host集成中使用生产AEAD实测。
- C同步Codec/Registry而非完整自动Coordinator；不声称C↔Rust实际入网互通。
- Peer Reauth、多Peer、动态Route/Flow、其他Optional控制接线仍需软件工作。
- 真实Flash/Witness/掉电、生产密码、物理Bearer、MCU运行/栈/性能及独立外审继续HOLD。
- 不提交、不推送、不操作实机；RUST-SW-02仍IN PROGRESS，不宣布整体协议闭环。

## 7. 最终结案证据（本段Host自审）

最终源码冻结后完整重跑，矩阵时间为2026-10-15 12:40:32 UTC；并非沿用第4节配置修正前
的结果。[独立保存的最终矩阵](../软件证据/Rust/2026-10-15/software-matrix-sw02-small-mtu-final.json)
包含12项配置、411项源文件/合同/工具SHA256，`source_stable=true`。保存后独立逐项复算，
411/411匹配、差异0。

矩阵文件SHA256：`BCB466635F60F94A0A14C3B1271423D8448C91A87F539BAD8845E561C50C6809`。

| 门禁 | 最终结果 | 证据范围 |
| --- | --- | --- |
| Rust Debug / Release / MSRV 1.85 | 各299/299 | 本轮Host测试，不含实机 |
| Clippy / fmt | 通过 | workspace、所有测试目标；格式检查 |
| Cortex soft/hard、ESP32-S3 | 通过 | 编译检查，未连接或运行开发板 |
| Optional/Feature-OFF | 26组合通过 | 包边界与独立可选Owner检查 |
| C/Rust/Python共同Codec Oracle | 73向量通过 | 含四条新分片raw向量，不代表跨语言Runtime入网 |
| Admission共享Golden | 7/7 | 当前共同正文合同 |
| C Full重建及CTest | 108/108 | 本轮GCC现有Full构建，不冒充fresh全工具链矩阵 |
| C Lite / Nano重建定向 | 各4/4 | Control Wire、Admission、Capability及Security栈门禁 |

Host资源工具当前测得示例`capability_runtime=4392 B`、`nano_control_replies=808 B`、
`lite_control_replies=2104 B`、`full_control_replies=6240 B`，对齐均8 B。这些数值对应
该工具的具体泛型/Feature配置，不是全协议RAM或MCU栈；开启Capability后第二帧静态缓存
的代价不能忽略。未在本轮新跑MSVC/Clang、ASan/TSan或目标板栈水位，不引用旧轮数字。

本段CF-01～09及两轮整体自审完成；独立外审仍待。后续软件任务是Peer Reauth、多Peer、
C自动Root、动态Route/Flow及其他可选控制接线，不得写成“仅剩实机”。

结案机械门禁：当前文档147篇/381条链接、API索引318项通过；报告更新后CTest的文档/
API两门再次2/2通过。`git diff --check`退出0（仅98条既有换行提示），本轮新增4份Rust
源码/测试及2篇文档尾随空白0；真实暂存区0项。报告收口未改源码，最终411项哈希仍0差异。
