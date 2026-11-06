# 邻居围栏、真实排空与耐久后继 Capability 热交接合同

2026-10-18；软件实施/自审候选，不是外审或生产 GO。继续
[合同17](17-Origin登记围栏排空与耐久后继交接合同.md)和
[合同18](18-直连多跳统一Origin与HopContext接线合同.md)。普通数据头、Opcode、
控制正文、Record Schema 均不增加字段。本篇不引入兼容模式或第二套协议。

## 1. 为什么要分阶段

Session 成功换代不等于邻居登记已可替换。旧邻居可能仍持有 Control/Data TX、
中继队列、未读 App、待退休 RX 或已接纳的 RREP 转发证明；旧 Capability 也包含
不能靠“删除再插入”抹掉的父域历史。

本轮的“耐久”指 **实际 Foundation 发布并回读的新 Session**。Capability 仍是
新 Session 下认证的易失租约缓存，不是新增耐久 Record 或跨掉电恢复承诺。重启后
仍需新 Session/Key 和新 Capability 认证，不能恢复 RAM 中的 live/Fence 标志。

## 2. 唯一编排顺序

```text
Coordinator owns the lifecycle:
    actual old Session / registered neighbour
      -> fence_neighbor
      -> retain and retire actual Control/Data/relay/App/RX obligations
      -> neighbor_is_drained == true

    actual successor Session
      -> prepare -> Foundation commit/reload -> Security ACTIVE
      -> actual authenticated C0 Advertise (or existing 0108 fragments)
      -> preflight replay -> replace_session(old exact Capability parent)
      -> commit replay
      -> current Identity / Session / Capability / Link / MTU / ACL checks
      -> rebind_neighbor in the original fixed slot, increment local handle generation
      -> optional explicit rebind_endpoint for each Service sharing that Session
      -> resume ordinary direct send or rediscover a multihop path
```

Owner 不互调；所有操作与认证完成事实由 Coordinator 路由。热交接阶段的原始 Advertise
仍须通过实际 Security、ACL、Tag、Replay 和完整分片组装；AuthenticatedPeerView 是
Security 产生的当前 Session 视图，不单独证明任意传入 Record 内容已在 Wire 上认证。
C DTO 也不是密码证明。Capability API 的调用方必须保持这条 custody 合同。

本轮接纳已知 Peer 的真实后继与新缓存，不建立新的线上 Reauth 协议，也未把所有模块
装成一个自动 Node。产品 Coordinator 在热交接阶段独占 Adapter/Ingress；不得让旧
Data Root 与 Capability Runtime 并行抢同一个 RX。正式回归使用实际 Driver/原子 RX
验证 Advertise 后才交接，未凭布尔或伪造 OpenedPacket 安装新能力。

## 3. Rust Root 接口

| 接口 | 作用 |
| --- | --- |
| fence_neighbor(old_handle, actual_security, now) | 实际 Session 撤权，围栏同 Session 全部邻居/Endpoint 角色；不删除 Token |
| neighbor_is_drained(old_handle) | 只读查询当前 Root 的真实义务并集；活动邻居返回 false |
| rebind_neighbor(old_handle, new_peer, parents, identity, security, capability_owner, new_cap_ref, adapter, cost, locator, now) | 所有当前父域与资源预检后，原槽替换并推进 Handle Generation |

旧 Handle 不因槽复用复活。普通 register_neighbor 对相同 Principal 或 Address 的旧
登记仍拒绝，即使旧项已围栏，也不能占空槽跳过 drain。Neighbor 接纳不会偷偷更新
Service/Endpoint；共享该 Session 的每个 Service 仍须显式执行合同17的交接。

## 4. Fence 和真实 drain

Fence 先调用实际 Security 撤权，再停止该邻居未提交 Control，取消局部未完成的路由
分片组装，使 Route/Receipt 失效，并围栏共享 Session 的 Endpoint。不调用 Driver，
不强制标记 Completed，不删除实际 TX Token。

drain 必须覆盖：

- 关联精确邻居 Handle/Session 的 Control 工作，包括尚未退休的本地完成；
- 共享 Session 的 Endpoint TX 与 App 消息；
- 以该邻居为出站的中继 Data；
- 已由 Root claim、尚未成功退休且属于该精确 Link/locator 的 RX。

Unknown/InDoubt 继续保留。stop、cancel 请求、Link reopen、逻辑终态、换新 Session
或新 Capability 都不构成物理退休证明。正式测试在 stop/reopen 后仍拒绝接纳，直到
精确迟到 completion 被 Adapter 接收且 Token 真正退休；停止的 Root 不会因排空而复活。

这个并集仍只覆盖本 Root。不能借它销毁外部 RPC/Flow/Provider/Driver 的 Holder，
也不自动调用 Security retire_fenced 或重置外部 counter bank。

## 5. Capability 的原子父域交接

| 合同 | C | Rust |
| --- | --- | --- |
| 原槽父域替换 | ucn_i_capability_replace_session | CapabilityOwner::replace_session |
| 已有同域更新/幂等 | ucn_i_capability_advertise_ingest | CapabilityOwner::ingest_advertise |

替换要求旧引用在缓存中精确存在；新认证父域的 Runtime、Security Owner、Realm、
Principal、完整 Binding 必须相同，Session Generation 严格更大。合法的代际跳跃
只能来自实际 Security 的耐久 Reauth 规则，不由 Capability 数字自行授权。

新 Record 必须完整合法，认证父域仍在半开有效期内；新 discovery/capability deadline
由此次实际接纳时间和本地租期构造，且不得超过当前 Session expiry。所有检查在固定
槽写入前完成；错误旧引用、回退/相等 Session、错 Binding、过期、溢出、别名输出均
拒绝。旧槽可已过期/撤销，但不能先删掉历史来跳过精确匹配。

跨 Session 的 Record generation 可以从合法起值重新开始，不继承旧 Record、MTU、
suite bits、feature bits、窗口或租期；同 Session 仍只接受精确重复或 checked-next
generation。新父域安装后，旧视图/Advertise 不能恢复旧父域；连续交接原槽复用，
没有全生命周期已用集合。重复同域输入不续租。

## 6. 邻居接纳的 use-time 校验

rebind_neighbor 至少检查：

1. Root 未停止、Gate 无回调重入、时间不倒退、旧 Handle 精确有效；
2. 旧项已围栏且真实义务排空，同 Peer 其他 Origin 登记不得藏有未决较新 lineage；
3. 新 Session 与旧 Handle 不同，当前确为耐久 ACTIVE，Session Generation 严格更大；
4. 当前 Identity Authority/父 Binding、Realm/地址档、完整 Local/Peer Binding 与 Policy；
5. 新 Capability 引用精确绑定新 Session、当前入口 Link 和实际 Security Owner，且 live；
6. 当前本地 Capability 发布的 Link generation 与实际 Link 一致，不复用 reopen 前的发布；
7. 新 MTU、capability bits、cost、locator、配置与固定槽 Handle 代际均合法。

完成所有预检后才替换一个注册槽。不凭新 Session ID 数字、仅 Foundation durable、
旧 Profile、旧 Capability 引用或同地址推断完整父域有效。

当前支持不改变本地 Capability 发布的已知 Peer Session 热续接。物理 Link reopen
之后若本地发布仍属于旧 generation，必须失败关闭。全 Root 的本地 Capability 重发、
全部受影响 Peer 协调和多 Bearer 原子切换仍是后续软件任务，不通过重新构造 Root 冒充。

## 7. Route/Reverse 历史退休

| C | Rust |
| --- | --- |
| ucn_i_route_retire_peer_history | RouteOwner::retire_peer_history |

围栏立即禁止使用旧动态 Route；真正排空后才能退休精确 Peer 的 Route/Reverse 历史。
存在该 Peer 的已接纳 RREP 转发证明时，退休必须零写拒绝，不能毁掉稍后 completion
需要的槽。退休保留槽 generation，高水位不回零；共享物理 Link 的其他 Peer 不删除。
静态配置不因此删除，也不直接授权使用；实际发送仍须复验当前邻居。

Root 不复制旧 Receipt、旧 Profile 或旧分片组装给后继；已无物理 Holder 的失效上游
元数据可释放，其他有效上游的失败诊断仍按既有有界维护处理。多跳恢复重新发现路径。
若只更新 AB/BA Hop Session，独立 AC Origin 的序号继续递增，不能随邻居换代重置。

## 8. 小项自审与边界

| 小项 | 验证 |
| --- | --- |
| NH-01 | 两种 fence 复用同一真实 Session 围栏；Control/Data/App/RX 并集，不删 Holder |
| NH-02 | C/Rust 原子父域替换，相等/回退/错 Binding/过期零写，五轮固定槽复用 |
| NH-03 | 当前 ACTIVE 后继与实际认证 Advertise 交接；旧引用与过期新缓存拒绝 |
| NH-04 | 未读 App、Data Unknown、C0 Unknown 均阻塞，精确晚到 completion 后才退休 |
| NH-05 | 同 Root 恢复直连/多跳；AC Origin 继续到2；旧 Handle/冷注册/停止态不能复活 |
| NH-06 | 新 MTU 不继承、可用新域更新可重试；reopen 后旧本地发布拒绝 |
| NH-07 | 精确 Route/Reverse 退休保留 RREP proof 与共享 Link 其他 Peer |
| NH-08 | 全体回读、裁剪、C多Profile/MSVC、Rust完整矩阵与依赖哈希 |

[生产接纳入口](../../../../rust/crates/ucn-runtime/src/route_runtime/data/neighbor.rs)、
[正式 Host 回归](../../../../rust/crates/ucn-runtime/tests/bootstrap_runtime/route_tests/data_node/neighbors.rs)及
[逐项/整体自审报告](../../../08-实现与验证/版本演进/UCN_V6S_RUST_SW_02_邻居围栏与耐久后继热交接自审报告.md)。

本轮没有增加普通帧字节、Owner/Endpoint/队列/workspace字段或持久化 Schema，没有
动态内存集合；不声称可控 Host Provider 等于生产密码、真实 Flash/Witness 或掉电。
C 同步的是 Capability/Route 原语及合同，C 自动多 Peer Data Root 仍待；Rust完整
自动 Reauth→多 Peer Capability→全模块 Node/Flow、O2自动邻居、跨语言 Runtime、
生产和独立外审仍未完成。未操作实机、串口枚举、烧录、复位或调试。
