# RUST-SW-02 邻居围栏与耐久后继热交接自审报告

## 1. 范围与结论边界

2026-10-18；按用户要求接邻居围栏、真实排空、耐久 Session 后继与重新认证 Capability
的本地热交接。C/Rust共同遵循[合同19](../../10-理论与规划/建议方案/UCN_v6_C_Rust共用合同收口/19-邻居围栏真实排空与耐久后继Capability热交接合同.md)。
本轮为软件自审候选，不是独立外审或生产 GO。没有实机、COM/USB 枚举、烧录/复位、
目标调试、Git 暂存/提交/推送或 Vault 修改；保留继承工作树。

本轮耐久的是实际 Foundation 发布/回读的 Session，Capability 仍为易失租约缓存。
不新增 Frame、Opcode、控制正文或 Record；不把 C 叶模块对等实现当作 C 自动 Root。

## 2. 每小项实施与自审

| 小项 | 实施 | 定向结果 |
| --- | --- | --- |
| NH-01 | fence_neighbor / neighbor_is_drained，与Origin共享围栏/义务并集 | 活动邻居不是drained；Data/C0 Unknown、未读App阻塞；精确晚到完成才退休 |
| NH-02 | C/Rust Capability原槽替换 | 不删旧父域；严格更新Session、相同完整Binding/Realm/Owner；错误零写 |
| NH-03 | rebind_neighbor复用冷注册的纯预检 | 当前ACTIVE后继、新live缓存、实际父域/Link/MTU后才替换原槽及Handle代际 |
| NH-04 | 显式Endpoint交接与冷注册防绕过 | 同Peer旧项不能占空槽绕过；邻居接纳不偷偷复活旧Service |
| NH-05 | Route/Reverse历史退休 | 待完成RREP证明保留；退休保留槽代际，共享Link其他Peer不删 |
| NH-06 | 直连/三节点/五轮真实后继 | 同Root恢复发送；只换Hop时AC Origin继续到2；固定槽重复复用 |
| NH-07 | 新MTU与reopen本地发布复核 | 新MTU126拒绝而非继承256；新域真实更新可重试；旧本地Link发布拒绝 |
| NH-08 | 两轮整体回读与全量门禁 | 最终矩阵与依赖哈希另列；不沿用OP-584的385项作为本轮结果 |

入口：[Rust邻居交接](../../../rust/crates/ucn-runtime/src/route_runtime/data/neighbor.rs)、
[共享围栏/排空](../../../rust/crates/ucn-runtime/src/route_runtime/data/endpoint.rs)、
[统一登记预检](../../../rust/crates/ucn-runtime/src/route_runtime.rs)、
[Rust Capability](../../../rust/crates/ucn-capability/src/owner.rs)、
[C Capability](../../../src/capability/ucn_capability.c)；精确历史退休位于
[Rust Route](../../../rust/crates/ucn-routing/src/owner.rs)和[C Route](../../../src/routing/ucn_route.c)。

## 3. 第一轮整体自审：生命周期与认证

1. 两种Fence复用实际Security撤权，不复制Owner、不重建Root、不退还已消耗序号。
   Session共享Endpoint/Hop时全部本地角色围栏；已有Token仍由原Adapter精确退休。
2. drain除Control/Data/App/出站中继外，包含Root已claim且尚未退休的精确邻居RX；
   不是全Node或外部Provider的静止证明。控制完成但尚未退休也不提前算完成。
3. 新Session经真实prepare、Foundation commit/reload、activate；新Advertise经过实际
   Driver、原子RX、Security、ACL、完整Record/分片与Replay预检，才替换Capability。
4. Capability替换先校验精确旧引用和新父域，再写一个固定槽；不先invalidate旧记录。
   新缓存不继承旧Profile、MTU或Deadline；相等/历史Session不能重新安装。
5. Root新登记再次校验实际当前Identity/Session/Capability/Link，保留单Peer单ACTIVE。
   新Capability发布与Root接纳分开；后者失败时旧Root仍fenced，可在同一新域合法更新后重试。
6. Neighbor Handle代际推进，旧Handle不复活；Endpoint仍分别交接。所有失败点在替换
   登记槽前完成，不声称整个Root的单调clock字段也逐字节不变。

## 4. 第二轮整体自审：补强的合同边界

### 4.1 Control与Reverse不能只看Data Token

本轮用只有C0 Unknown、没有该Session Origin Data的反例验证并集。即使新Session及
新Capability都成立，Root仍拒绝接纳；stop/reopen也不清义务，精确晚到completion后
才退休。未读App在围栏后拒绝读取，后续有界维护丢弃，而不是重标为新Session消息。

另发现不能把旧Reverse/Receipt原样留给同槽后继。新增C/Rust精确Peer历史退休；仍有
已接纳RREP转发证明时零写拒绝，完成后才退休。共享物理Link其他Peer不受影响，局部
槽generation保留。失效且无物理Holder的旧上游Receipt是可释放元数据，不是TX退休。

### 4.2 新能力不继承旧资源

实际新Advertise把可处理MTU从256收紧为126，Root返回NoSpace，旧槽仍围栏。随后在
同新Session认证generation2更新为可用值才允许接纳，证明不是只验证新Session数字。

使用reopen后的实际Link generation与新Session/新远端缓存，又发现本机不可变Capability
发布仍属旧Link，故在共用登记预检加入精确本机Link generation校验。此场景现在拒绝；
全Root本地Capability重发和多Bearer热迁移不在本轮冒充完成。

### 4.3 跨编译器测试门禁

MSVC /WX补验暴露既有测试中的before局部变量遮蔽，改为revoke_snapshot，不改断言和
生产语义。第一次正在执行的矩阵主动中止，不作为最终候选；测试修正与全部代码冻结后
重新运行最终矩阵，避免哈希或工具链证据混用。

## 5. 正式回归

[neighbors.rs](../../../rust/crates/ucn-runtime/tests/bootstrap_runtime/route_tests/data_node/neighbors.rs)
新增11项：

- durable_h1_successors_and_authenticated_capabilities_resume_direct_on_the_same_roots；
- a_real_new_session_does_not_bypass_unknown_or_cold_registration；
- unread_old_application_message_blocks_adoption_until_fenced_cleanup；
- expired_successor_capability_and_foreign_handle_cannot_replace_the_slot；
- callback_gate_rejects_neighbor_lifecycle_without_reactivating_or_fencing；
- repeated_durable_handoffs_reuse_fixed_neighbor_and_capability_slots；
- multihop_data_rediscovers_after_neighbor_handoff_without_resetting_origin；
- smaller_new_capability_cannot_inherit_old_mtu_and_can_retry_after_real_update；
- old_authenticated_views_and_equal_generations_cannot_restore_the_old_capability；
- control_unknown_blocks_hot_adoption_even_without_origin_data_on_that_session；
- reopening_a_bearer_cannot_reuse_the_old_local_capability_publication。

另有[Route正式回归](../../../rust/crates/ucn-routing/tests/routing_contracts.rs)验证待完成
证明与同Link其他Peer；[C Capability](../../../tests/simplified/test_capability.c)覆盖
相等/错Binding/过期/别名零写、五轮原槽复用、新Profile和旧父域重放；
[C Route](../../../tests/simplified/test_route.c)覆盖pending证明退休拒绝与完成后退休。
C这些是叶Owner SPI单测，认证DTO不冒充真实生产密码证明；Rust集成使用真实受控Host MAC。

## 6. 最终验证证据

11项热交接定向、1项Route定向通过。最终冻结源码验证如下；仅本表所列为本轮证据，
未把历史ASan/TSan结果或中止的首次矩阵作为当前通过项。

| 检查 | 本轮结果 |
| --- | --- |
| Rust工作区Debug / Release / MSRV 1.85 | 各397/397 |
| 完整软件矩阵 | 12配置全部exit0；Clippy全目标-D warnings、fmt通过 |
| Cortex-M软/硬浮点、ESP32-S3 | 均compile-only通过，无烧录或板上运行 |
| Optional / 跨语言Codec / Admission Oracle | 32裁剪组合、91组三方raw向量、7项共同Golden通过 |
| C GCC Full重建、全量CTest | 108/108 |
| C Lite / Nano重建定向 | 相关5项及Routing/Admission两项栈门禁，各7/7 |
| C MSVC2019 Release重建定向 | Capability、Route共2/2；不是全量MSVC矩阵 |
| rustdoc | 全工作区--no-deps，RUSTDOCFLAGS=-D warnings通过 |
| 依赖完整性 | 431项来源在矩阵期间稳定，独立重新计算SHA256，漂移0 |

最终矩阵UTC时间为2026-10-18T15:45:32.528079+00:00。
[不可变LF归档](../软件证据/Rust/2026-10-18/software-matrix-sw02-neighbor-capability-handoff-final.json)
SHA256：`E9194D45E1F0BFC611BE2B40DBEAFD32110E6F0ED805C1A3E9D9729AEBB3FA0F`。
归档只含命令、结果和精确源码哈希，不把构建缓存作为交付物。公共API索引318保持当前。
文档门禁155篇/449条链接通过；另外对本轮17个源码/测试和2个新增文档单独扫描，
18条本地链接均有效、尾随空白0。git diff --check通过，真实暂存区为空。

Host资源示例中small_routed_data_node为7832 B、mesh_routed_data_test_node为19640 B，
对齐均8 B，与本轮前布局相同；这只是指定泛型实例的sizeof，不包括外置Owner、Driver、
队列和调用栈，不是MCU总RAM、最坏栈或WCET测量。

## 7. 费用与剩余软件任务

线上新增0 B，A1 C1头仍13 B，Origin/H1既有安全费用32 B另计。生产Owner、Endpoint、
邻居登记、队列、workspace未增加字段，没有新持久化Schema、动态集合或Owner栈副本。
所有表扫描受现有编译期容量约束，不把有界扫描当作MCU WCET证明。

本轮完成已知Peer后继的本地接纳，不是完整自动Reauth到多Peer唯一Ingress/全Node的
全协议产品。热交接阶段必须由产品Coordinator独占Adapter；不能让Root和另一个Runtime
抢RX。C自动多Peer Data Root/C1中继、O2自动邻居、全Root本地Capability变更、多Bearer
热迁移、动态端到端握手、Flow、统一Node/Facade、Group/Cluster和跨语言Runtime仍待。

后续应把实际Reauth完成事实、Capability交换和本轮邻居接纳接进同一有界Node阶段机，
再做C对等自动接线；不靠复制缓存或重建Root跳过Holder。独立外审、生产及实机HOLD。
Host、目标编译和受控Provider不能证明真实Flash/Witness、掉电、MCU栈水位、生产密码、
物理Bearer、硬件时间戳、性能功耗或长稳。
