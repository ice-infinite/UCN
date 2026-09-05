# 身份、入网、会话与 ACL

## 1. 三类身份不能混用

`Device Principal` 表示设备长期身份，`Realm Address + Binding Generation` 表示地址在某个
租约代际内绑定给谁，`Session Generation` 表示一对 Peer 当前安全会话。Route Key、Replay
Key 和 ACL 输入包含各自需要的完整域；仅比较地址会在地址复用时产生 ABA。

所有 Generation 都有 Owner、父域、持久化高水位、合法重置条件和耗尽行为。达到上限或高
水位损坏时进入 Fault，不回绕。动态 Group ID 只从单调高水位分配，删除不释放历史 ID；
静态 Group 和 Group Key 使用 Manifest 固定槽并推进 Generation。

Identity Authority 的持久状态由完整 Snapshot、单调 Record Generation 和独立 rollback
witness 共同证明。写入顺序是 witness-first、Snapshot、reload 精确比较；启动时二者不一致
不能静默回退到旧槽。Binding/Group 的分配和退休都必须走这条路径。

## 2. Bootstrap 与已绑定节点重认证

未绑定节点使用一跳、禁止转发的 Bootstrap 地址和临时事务身份。流程先经过无状态 Cookie、
认证前限流和 per-Link 固定 pending 配额，再执行双向身份认证。完整 transcript 绑定双方
Principal、nonce、txid、Realm、Address、Binding Generation、Authority Identity/
Generation、Security Suite 和 Key selector。

这里的“per-Link”不是单独的 Generation 数值，而是精确的
`{ingress_link_id, ingress_link_generation}` Link Instance。同一块设备上的两个物理 Link
即使都处于 generation 1，也必须使用彼此独立的 Token Bucket 和 pending 配额；Link reopen
后 generation 推进，旧实例的 Cookie、pending 和完成事件不能占用或复活新实例。

`ADDRESS_OFFER` 不能让设备直接进入 ADMITTED；只有 `FINAL_COMMIT` 已通过双方认证、Authority
仍持有合法租约/Quorum、持久化完成并由设备验证后，Binding 才生效。已绑定设备移动到新 Link
但没有 Peer Session 时走 Peer Reauth，不伪装成 UNBOUND，也不使用普通未认证 HELLO。

## 3. Address Authority

动态地址只由 Realm 中单一、可持久化且可 Fence 的 Address Authority 分配。Authority
Identity/Generation、租约证书、quorum、切换和分区规则必须可验证；失去合法 Authority 或
quorum 时禁止创建新 Binding。租约双方用本地单调时钟，但证书携带不可延后的共同截止依据，
producer 与 verifier 都扣除慢钟和定时器量化误差，使用半开区间判活。

## 4. Security Owner

Security Owner 管理 Peer Session、Group Policy/Key、ACL 和 Replay Window。`protect_frame()`
和 `open_frame()` 通过调用方 Provider 完成实际密码操作；Provider 返回成功不够，Owner 还要
核对输出长度、selector、nonce、Generation 和 callback gate。Key 轮换期间每个上下文只允许
规范规定的活动映射，不能由帧任意选择降级 suite。

发送序号分为端到端 `origin_sequence` 和逐跳 `hop_sequence`，二者分别预留持久区间，不能
共享计数器。中继先由 Security 验证入站原始帧，再为下一跳更新 Hop selector/sequence/tag；
Route/QoS 只能消费 Security 返回的验证结果。RX Replay Window 是易失运行态，连续收包不写
Flash；节点重启后 Peer Session 必须 Reauth、Group Key 必须 Rekey，发送端跳过全部已持久
预留的未用序号。

## 5. ACL 与业务副作用

ACL Key 至少绑定 Principal、Endpoint、Frame Type 大类、精确 `protocol_opcode` 和权限。
Frame Type 通配不能扩大到整个控制类别。接收顺序是 Hop Auth → E2E Auth/Replay → ACL →
Realtime/Operation 门禁 → 业务副作用；任何后置检查失败都不能留下成员、Route、Journal 或
外设动作。

## 6. 产品责任

仓库提供协议状态机和 Provider 合同，不提供产品的根密钥、证书体系、随机数源审计、密钥
注入和 secure element 驱动。产品必须选择 suite，给出密钥轮换/吊销流程，验证 nonce 不复用，
并对硬件故障和侧信道另行评估。
