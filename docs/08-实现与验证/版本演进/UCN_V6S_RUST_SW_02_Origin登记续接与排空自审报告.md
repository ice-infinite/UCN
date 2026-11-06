# RUST-SW-02 Origin 登记续接与排空自审报告

## 1. 实施范围

2026-10-18，继续[自动多跳Data](UCN_V6S_RUST_SW_02_自动多跳Data协调器自审报告.md)之后
的软件任务。本段按[共同合同17](../../10-理论与规划/建议方案/UCN_v6_C_Rust共用合同收口/17-Origin登记围栏排空与耐久后继交接合同.md)
新增本地 Origin Session 围栏、真实 Holder 排空和耐久后继 Endpoint 交接。

不是跨多跳端到端握手实现，也不是所有Runtime共用的全局排空协调器。当前C对等自动Root、
全Node/Facade、Flow、Group/Cluster与跨语言运行时仍待软件实施。线上Frame、Opcode、Body、
Record和普通数据头均不增加字段。没有实机操作、COM/USB枚举、烧录/复位、目标调试、
Git暂存/提交/推送或Vault修改。

## 2. 每小项自审

| 小项 | 代码与合同 | 验收 |
| --- | --- | --- |
| OR-01 | Endpoint缓存实际Session代际、保护模式和不可逆本地Fence | fence_origin调用实际Security撤权，覆盖共享Session的全部Service；旧send/receive立即拒绝 |
| OR-02 | 只查询实际Data/App/Token的本地屏障 | 未读消息不重挂；Unknown、stop/reopen均不构成退休证明 |
| OR-03 | 当前真实父域和ACTIVE后继的原子替换 | PREPARED、仅Foundation durable、错Binding/Fingerprint、过期Identity、降级均拒绝 |
| OR-04 | 跨Service/旧墓碑防绕过及Handle防ABA | 不能冷注册新服务跳过Holder；不能用Session1墓碑跳过Session2 Unknown；五次真实后继固定槽复用 |
| OR-05 | 三节点实际交付及旧域隔离 | 新Key下Sequence1成功；迟到旧Origin在真实新鲜Hop proof下仍被拒绝 |
| OR-06 | 两轮整体回读、裁剪与完整矩阵 | 13项新增正式回归通过，完整最终证据另列，不继承上轮364项结果 |

生产入口在[Endpoint交接实现](../../../rust/crates/ucn-runtime/src/route_runtime/data/endpoint.rs)，
共用预检在[Data协调器](../../../rust/crates/ucn-runtime/src/route_runtime/data.rs)，
正式回归在[renewal.rs](../../../rust/crates/ucn-runtime/tests/bootstrap_runtime/route_tests/data_node/renewal.rs)。
命令：`cargo test -p ucn-runtime --test bootstrap_runtime data_node::renewal`（rust目录）。

## 3. 第一轮整体自审：状态与物理义务

1. Fence先撤销实际Security Session，再标记全部共享Endpoint。旧登记不能凭同Handle
   register/Begin/reset重新激活；本段没有提供清零或删除墓碑的接口。
2. Data/App按精确Endpoint Handle归属，Handle Generation参与匹配。旧工作终态且实际
   Token退休后才满足屏障；跟踪回执仍可保留查询，不为了新发送静默淘汰。
3. rebind所有预检先于槽写入：当前Gate/停止态/时间、旧Fence/屏障、实际父域、当前ACTIVE
   后继、Binding、Policy、安全模式和严格递增代际。最后才更新一个槽与其局部代际。
4. Security的ACTIVE不等于Endpoint已发布；测试分别检查准备态、Foundation已durable但
   未activate、真实activate三个阶段。旧SessionGen或新代际数字本身不当作认证证明。
5. 不自动释放Security槽、counter bank或其他Runtime的Holder。这里的drained只描述本
   Data Root；统一Node仍需汇合其他模块的真实屏障，不把一个局部bool当全局静止证明。

## 4. 第二轮整体自审：对抗旁路和证据区分度

初稿按待替换旧Session检查屏障。回读时扩展了两个跨服务反例并补强：

- 即使新Session已真实耐久ACTIVE，也不能占另一个Service空槽绕过旧Unknown Holder；
- Service A已到Session2且Unknown未决，不能借Service B已排空的Session1墓碑发布Session3。

修复后预检同Peer全部不同Session的本地登记；冷注册也不能隐式跨Session切换。共享同
一个后继的其他服务仍需各自明确交接，不会因为第一个Service成功就全部恢复。

迟到帧用真实Driver已生成但测试承载尚未送达的旧完整帧，不是伪造认证DTO。更新后先
用实际Security验证该Hop proof仍新鲜并abort reservation，再将原帧放入实际原子RX；
Node返回Security错误。随后新Key下Origin Sequence=1真实送达，证明不是单靠Hop重放
或错误Provider把全部旧/新包都拒绝。Host新Session采用不同实际MAC Key ID/代际。

O2→O1测试只验证模式策略拒绝，没有声称AEAD数据通路或真实生产密码已通过。Session
更新成功还要求当前Identity有效；测试中Session尚未到期但Binding已过期，仍拒绝交接。
停止Root不因新登记恢复；回调Gate占用期间，围栏和发布均拒绝。

后继建立使用既有静态Session测试Provider的精确候选校验，再经过实际Foundation提交/
回读/Proof和Security activate；不是新增线上Reauth签名交换。成功数据路径的HMAC Tag
真实计算/验证，这两类证据不能混称为生产认证或跨多跳握手已经完成。

## 5. 正式回归清单

| 测试 | 结论 |
| --- | --- |
| queued_old_work_is_fenced_and_cannot_reactivate_through_registration | 未提交旧请求需排空，旧Session/Handle不复活 |
| higher_durable_session_does_not_bypass_explicit_registry_fence | 更高真实代际不替代显式围栏，错Binding拒绝 |
| unknown_old_holder_blocks_even_a_real_durable_successor | 新后继/其他Service/Link重开都不能消除Unknown |
| every_service_of_one_session_shares_the_fence_and_drain_barrier | 同Session全服务共用屏障，不继承Service ACL |
| unread_old_message_is_not_relabelled_as_a_new_session_message | 旧未读消息清理前不可换槽或交付 |
| renewed_endpoints_deliver_and_reject_a_delayed_old_origin_with_fresh_hop_proof | 有效Hop下拒绝旧Origin，新Key Sequence1成功 |
| prepared_or_only_foundation_durable_is_not_an_active_successor | 准备态/仅durable/错Fingerprint不发布 |
| an_older_service_tombstone_cannot_bypass_a_newer_unknown_holder | 跨代际、跨Service墓碑旁路被阻断 |
| repeated_real_successors_reuse_bounded_registry_and_security_slots | 连续五次实际后继及安全槽复用，不建无限历史表 |
| stopped_root_cannot_be_reactivated_by_a_fresh_registry_entry | stopped无恢复旁路 |
| current_identity_expiry_blocks_rebind_even_when_successor_session_is_live | 当前Identity权限先于缓存Session可用性 |
| encryption_policy_cannot_downgrade_during_registry_handoff | 原O2模式不能降为O1 |
| callback_gate_prevents_both_fence_and_successor_publication | 回调中不改权限/登记 |

## 6. 资源与最终验证

本轮Host每Endpoint多8 B。小Root/三节点测试Root为7832/19640 B（此前7824/19624 B），
对齐8 B；不包含Identity、Security、Capability、Adapter、workspace、Provider和最坏调用栈。
这不是MCU RAM/栈、Flash掉电、时延、功耗或实机证明。

最终矩阵UTC `2026-10-18T14:32:33.477984+00:00`，
[不可变LF归档](../软件证据/Rust/2026-10-18/software-matrix-sw02-origin-registry-renewal-final.json)
SHA256=`967BFB15DC48D3C89B04B770B6C55493CCDE0192D55A90592D3B5C5A6757DE95`。
归档与Windows工作目录的CRLF JSON语义一致；字节哈希只对应本链接LF文件。
428项源码/工具/合同依赖已独立逐文件复算，0漂移。

| 门禁 | 本轮实际结果 |
| --- | --- |
| Rust Debug / Release / MSRV1.85 | 各377/377 |
| 本段定向 / Data Node总定向 | 13/13；合计23/23，完整矩阵同时覆盖 |
| 全矩阵 / 源码稳定 | 12配置exit0，source_stable=true，428项独立复算0漂移 |
| Optional裁剪 | 32组合通过，最小Core/Optional边界保持 |
| C/Rust/Python raw / Admission Golden | 91向量 / 7项通过；不增线上字段 |
| Clippy / fmt | workspace all-targets和格式检查通过 |
| rustdoc | 全workspace no-deps，RUSTDOCFLAGS=-D warnings通过 |
| Cortex-M soft/hard / ESP32-S3 | compile-only通过，不是板卡运行 |
| C Full | 本轮重建检查，CTest108/108；没有修改C生产源码 |
| C Lite / Nano | 本轮重建路由/Wire/Capability/Security定向各5/5 |
| C公共函数索引 | 318项最新，无C公共API变更 |
| Git/文档 | 最终链接和空白门禁通过；真实暂存区保持为空 |

OR-01..06逐项及两轮整体自审完成；没有独立复跑全部C MSVC/Clang/TSan，不沿用历史数字。
整体回读记录下一项软件缺口：当前Root的独立H0 Origin与H1邻居只覆盖不同端到端/
相邻Peer的组合；同一直接邻居不能用两个Session并存来绕过Security单Active规则。需补
同一实际Context的直连Origin/Hop分层接线，不能把现有三节点成功外推为所有直连场景。
当前仍是受限软件自审候选，RUST-SW-02 IN PROGRESS；外审、生产和实机HOLD。跨多跳端到端
握手、自动协商和全Node/Facade、完整Flow、Group/Cluster、C自动Root及运行时互操作仍是
后续软件任务，不宣称“只剩硬件”。
