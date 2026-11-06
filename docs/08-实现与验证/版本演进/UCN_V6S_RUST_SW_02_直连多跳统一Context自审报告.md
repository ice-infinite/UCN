# RUST-SW-02 直连、多跳统一 Context 自审报告

## 1. 实施范围与状态

2026-10-18；按用户要求继续直连/多跳统一 Origin/Hop Context，C/Rust 共用
[合同18](../../10-理论与规划/建议方案/UCN_v6_C_Rust共用合同收口/18-直连多跳统一Origin与HopContext接线合同.md)。
每小项实施后自审，再完成整体状态、失败路径与裁剪复核。当前是软件自审候选，
不是独立外审或生产签字。没有实机、COM/USB 枚举、烧录/复位、目标调试、Git 暂存/
提交/推送或 Vault 修改；保留全部继承工作树。

## 2. 逐小项结果

| 小项 | 实施 | 自审证据 |
| --- | --- | --- |
| UH-01 | C/Rust 显式 C1 Origin-only 投影，复用真实 H0/H1 Session | 不改 Session，不建立第二 ACTIVE；完整 packet API 仍精确匹配 Hop Profile |
| UH-02 | Rust 自动 Root 直连与多跳使用相同 Origin→Hop 流程 | 精确邻居直接复验；非邻居仍需完整 Origin-domain RREP；普通 send 不增加模式参数 |
| UH-03 | 直连双向与混合路径 | 一个 Root 同时发送给 B 与经 B 到 C；实际 Driver、原子 RX、认证与应用 pull |
| UH-04 | 双层 Replay、容量和错误 | 错 Hop、有效 Hop 下错 Origin、counter reset、必需 Capability 不足均有定向 |
| UH-05 | 同 Session 共享撤权/排空 | Origin fence 同时停止邻居角色；C0 Unknown、Data Unknown 都不能凭 stop 清除 |
| UH-06 | C 对等原语 | O1、两类 O2 测试 Provider；短输出零写，错指纹/Tag/Replay，投影/完整混用保持 Origin2/Hop1 |
| UH-07 | 全体回读与最终验证 | 本文记录新的矩阵与当前源码依赖，不沿用 OP-583 的377项作为本轮结果 |

核心代码：Rust [Security 投影](../../../rust/crates/ucn-security/src/packet.rs)、
[Data Root](../../../rust/crates/ucn-runtime/src/route_runtime/data.rs)、
[共享角色围栏](../../../rust/crates/ucn-runtime/src/route_runtime/data/endpoint.rs)；C
[Security](../../../src/security/ucn_security_session.c) 与
[内部声明](../../../src/internal/ucn_security.h)。

## 3. 第一轮整体自审：认证、路径与证明

1. 同一个真实 H1 Session 可同时提供 Origin/Hop，但两套 Fingerprint、Key selector、
   序号和 Replay 仍独立。直连不因两个 Handle 相等而跳过任一层；多跳也不把邻居当终点。
2. Origin-only 是明确 API，不是向调用方暴露 skip_auth 布尔。私有公共实现继续检查
   Session、Binding、ACL、Nonce、序号高水位和回调 Gate；C1 Data/O1/O2之外拒绝。
3. 完整 protect/open 的原有 Profile 匹配未放宽；已有 H1 Session 不能把完整 packet
   当 H0 使用。测试先验证旧完整入口拒绝，再验证新投影和真实独立 Hop 均能成功。
4. 终点先验证实际邻居，再开 Origin；两个 replay 预检后在同一独占借用 quantum 提交。
   错 Origin 会 abort 待提交 Hop；后到合法原帧仍成功，证明不是单纯把所有帧都拒绝。
5. 源/目标仍按实际 Endpoint Policy 授权；直连目标必须与当前邻居完整 Binding 相同。
   中继保持 opaque payload/tag，只改 TTL 和 H1；普通数据头新增0 B。

## 4. 第二轮整体自审：补出的状态边界

最初只增加投影 API，并让直连仍等待路由控制。实际同时双向发送测试复现：反向 RREQ
的本地 return hint 可使已准备的单跳 route view 变化，待发帧失败。最终不以取消检查
或伪造 RREP 修复，而是精确已认证邻居走直连路径，根本不创建该 RREQ；多跳仍要求
原始 Origin 域的 RREP。双向并发和混合拓扑正向测试通过。此修复不宣称所有动态路由
并发/刷新情形均已验收，后续多路由压力仍是独立工作。

直连快捷路径额外复核必需 capability bits 与实际 MTU，在消费 Origin 前拒绝不满足
条件的路径。query 的 waiting_route 按尚未准备报文区分，不再把已准备的直连 Unknown
误报成等待路由。

共享 Session 会使原先局部 Endpoint drain 漏掉同 Session 的控制工作。因此本轮把
Root 内的 Control、出站中继与 Endpoint Data/App 真实义务取并集；fence 不清除任何
物理 Token。C0 Unknown 测试中 Endpoint 从未发送，围栏和 stop 后 drain 仍为 false。
该屏障不包括 Root 外其他模块，不隐式替换已围栏邻居，也不授权新 Session。

## 5. 正式定向回归

Rust [contexts.rs](../../../rust/crates/ucn-runtime/tests/bootstrap_runtime/route_tests/data_node/contexts.rs)
新增7项真实 Owner/Adapter 回归：

- direct_both_directions_use_one_actual_h1_session；
- one_root_mixes_direct_and_multihop_without_wire_or_send_mode_switches；
- origin_projection_does_not_weaken_complete_packet_profile_or_double_reserve_hop；
- shared_session_fence_keeps_control_unknown_in_the_origin_drain_barrier；
- direct_data_unknown_never_claims_a_route_wait_or_physical_drain；
- good_hop_is_not_origin_authorization_and_rejections_leave_both_replays_fresh；
- direct_path_cannot_skip_required_capabilities_or_burn_an_origin_sequence。

Security另有 C0～C5、Data/Control/Diagnostic、O0～O2、请求/协商 H0～H3 的笛卡尔
准入矩阵。它是层选择函数验收，不冒充全部组合的密码或运行时验收。

C [test_security_session.c](../../../tests/simplified/test_security_session.c) 新增三组
投影/完整交替回归，均在真实 Session 生命周期上运行。O2测试为可控 Host 假密码
Provider，不证明生产 AES-GCM/ChaCha20-Poly1305 实现或硬件秘密管理。

## 6. 验证证据

| 门禁 | 本轮实际结果 |
| --- | --- |
| Rust Debug / Release / MSRV1.85 | 各385/385 |
| Rust完整软件矩阵 | 12配置exit0，CARGO_INCREMENTAL=0，运行期间source_stable=true |
| Optional裁剪 | 20包、32组合、3个隔离Optional Owner |
| 三方Codec / Admission oracle | 91 raw向量 / 7项共同检查 |
| clippy / fmt / Cortex软硬浮点 / ESP32S3编译 | 通过，仅编译；没有访问实机 |
| rustdoc | workspace/no-deps，-D warnings通过 |
| C GCC Full重建与全量CTest | 108/108 |
| C GCC Lite / Nano | 重建后路由/控制/Capability/C1/Security定向各5/5；Security栈门禁各1/1 |
| C MSVC2019 Release | 重建Security及依赖，定向1/1；未声称本轮MSVC全量 |
| 公共API索引 | 318，--check通过 |
| 当前文档门禁 | 154 documents / 433 links，另复核本轮2份新文档全部本地链接 |
| 改动范围 | 相比OP-583快照7个既有源码/测试变化，新增contexts.rs；没有夹带其他源文件改动 |
| 源码尾随空白 | 本轮8文件，0 |

[最终不可变矩阵](../软件证据/Rust/2026-10-18/software-matrix-sw02-unified-origin-hop-context-final.json)：

```text
timestamp_utc = 2026-10-18T15:08:17.015011+00:00
SHA256 = 015D1964E6A2ADC7F4FF216F157ABCD22FC4EF9B09013048C0E1D7A96C3850E2
source_dependencies = 429
independent_hash_mismatches = 0
archive_line_endings = LF
```

归档通过逐项独立重算确认与当前C/Rust实现、测试、Cargo/CMake和合同JSON一致。它是
当前完整继承工作树的软件证据，不是HEAD提交或远端GitHub已包含这些改动的证明。
没有生成/更新旧外审候选签字，也不把旧OP-583矩阵当作当前385项结果。

## 7. 费用、未完成项与下一步

线上新增0 B，A1 C1头仍13 B；Origin Tag16 B、H1 Sequence/Tag16 B照常另计，总计
45 B加业务。直连减少寻路控制帧，不伪报任何实测吞吐或时延。没有新增 Endpoint、
DataJob、Owner/workspace 字段，不建动态集合或完整 Owner 栈副本。

Host小Root7832 B、测试Root19640 B，alignment=8，与OP-583相同；不含其他Owner、
共享workspace、Driver或调用链栈，不是MCU总内存/WCET验收。

本轮 C/Rust 同步到两层安全原语合同；C 自动 Data Root/C1 中继没有因此完成，不能
用Rust模拟给C签运行时互通GO。Rust自动Root仍限O1/H1邻居、预发布Session、尽力
单向Data；O2自动邻居控制、邻居热续接、动态端到端握手、Flow、全模块统一Node、
Group/Cluster和跨语言Runtime仍有软件任务。

后续优先补邻居本地围栏/真实 Holder 排空/耐久后继 Capability 重新接线，与已完成的
Endpoint更新合流；不是靠重新构造Root清掉旧义务。独立外审、实机与生产继续HOLD；
Host与目标编译结果不证明实际Flash/Witness、掉电、密码Provider、物理Bearer、硬件
时间戳、WCET/任务栈、性能功耗或长稳。
