# Origin 登记围栏、排空与耐久后继交接合同

2026-10-18；软件实施/自审候选，不是外部审计或生产 GO。本篇继续[自动多跳 Data 合同](16-自动多跳Data协调器与本地下一跳合同.md)，
不新增 Wire 字段、Opcode、Header、Body 或 Record Schema。当前 Rust 已接入对应协调器；
C 应遵守相同安全语义，但本轮不冒充 C 等价自动 Root 已完成。

## 1. 为什么不能直接换一个 Session Handle

Session 已更新，不代表旧消息已经退出本地系统。旧 Origin 可能仍关联：

- 尚未封装/提交的业务请求；
- 已交 Driver、尚无可信终态的精确 Token；
- 已认证、还没有交给应用的消息；
- 共享相同 Session 的其他 Service；
- 已终态但由应用保留查询的本地回执。

若把旧 Endpoint 原槽直接覆盖，新 Context 会继承旧工作；若删除旧槽再冷注册，旧 Handle
或另一个 Service 又可能绕过围栏。正确交接必须把授权、内部排空、物理退休和新登记区分。

## 2. 两层状态机

```text
Security Owner:
    actual old Session -> FENCED
    actual successor -> PREPARED -> Foundation durable/reloaded -> ACTIVE

RoutedNode local registry:
    registered old Endpoint
      -> FENCED / DRAINING
      -> FENCED / locally DRAINED
      -> exact ACTIVE successor replaces the slot
         with a strictly newer local handle generation
```

Security 的 ACTIVE 只表示它已通过自己的真实发布门禁。即便新 Session 已 ACTIVE，旧登记
仍须显式围栏并通过本地屏障，不能凭“新代际更大”跳过旧物理义务。Foundation durable
但尚未由 Security activate 的候选也不能注册。普通 register 不是恢复或续接入口。

允许新 Session 在旧登记排空前准备/耐久发布，但 **Endpoint 默认映射的替换** 必须等待
本地排空。这个顺序不承诺撤销已经发生的远端执行，也不承诺双方会同时完成发布。

## 3. 精确接口与作用域

| Rust 入口 | 行为 |
| --- | --- |
| fence_origin(handle, security, now) | 先围栏实际 Session，再标记共享该 Session 的全部本地 Endpoint；不调用 Driver，不删除 Token |
| origin_is_drained(handle) | 只查询该 Root 的 Session 本地屏障；不授予 Authority，不替代新 Session proof |
| rebind_endpoint(old, new_session, actual_facts, fingerprint, parents, identity, security, now) | 复验真实后继和全部本地屏障，最后一次性替换一个槽并推进 Handle Generation |

这些入口是产品 Coordinator 的接口，不是业务调用方自造认证事实的接口。通过接口名传入
Session Handle 不能绕过 Security 对该 Handle、阶段、当前父域和 Deadline 的实际校验。

`origin_is_drained`只涵盖这个 RoutedNode 的实际义务。共享Origin/Hop Session的
[合同18](18-直连多跳统一Origin与HopContext接线合同.md)将其扩为本Root内Endpoint
Data/App、邻居Control和中继出站Holder的并集。它不是所有 Runtime、RPC、
Transfer、Journal 或物理承载的全局静止证明。未来统一 Coordinator 必须检查各自的真实
屏障，不能拿这里的一个 bool 退休其他模块的 Holder。本段不自动调用 retire_fenced 来释放
Security 槽，也不自动销毁产品 counter bank。

围栏和Handle代际位于caller-owned RAM，不是内存防篡改措施。任意覆盖内存或在物理未
静止前丢弃Root/Driver缓冲不属于受支持的生命周期，不能算作协议取消或恢复。重启后的
安全继续依靠既有Foundation高水位、真实回读和新Session/Key规则，不靠恢复这个bool。

## 4. 围栏后的行为

1. 共享同一个 Origin Session 的所有 Service 都被本地围栏，不只围栏传入 Handle 的服务。
2. send、Endpoint receive 和准备/发送前当前事实复验均立即拒绝旧登记；不等待维护游标
   才停止权限。维护只负责有界清理已拒绝的旧工作。
3. 已接纳的 Driver Token 独立保留。NOT_SUBMITTED、已确认终态和真实取消可退休；Unknown
   保持不确定态。stop、reopen、Session 更新都不能当作物理完成。
4. 未读消息不能重挂到新 Session。围栏后取消息失败且输出不写；维护清理后才满足屏障。
5. 已终态且物理退休的跟踪回执可以继续查询；新登记不得把旧回执改写成成功，也不自动
   驱逐这些回执给新发送让槽。旧 Job Handle 的生命周期仍由显式 forget 结束。

## 5. 替换的全部预检

同一独占 Coordinator quantum 内必须先全部通过：

- Root 未停止、Gate 未被回调占用，now 单调且有效；
- 旧 Handle 精确匹配，旧登记已显式 fenced；
- 旧 Session 对应的所有本地请求已终态、Token 已真实退休、App 消息已清理；
- 同 Peer 的其他不同 Session 登记也已 fenced 且 locally drained；
- 新 Handle 不是旧 Session，实际 Security 阶段为 ACTIVE；
- 实际 Authority、两端 Identity Binding、Realm、Width、当前 Lease、Policy 和 Fingerprint
  均可复验；本段要求两端完整 Binding 与旧登记相同，不把地址变更混入续接；
- Session Generation 严格大于旧登记及同 Peer 其他旧登记的已知高水位；
- Service、Traffic ceiling、最大 Payload 等本地 Endpoint Policy 原样保留；O1/O2 保护
  模式也必须精确一致，不暗中降级加密或改变产品安全需求；
- 本地槽 Generation checked-add 成功，不回绕。

然后才替换一个固定槽。失败不替换登记，不写消息、不消耗发送计数器或提交 Driver。
时间高水位仍按 Root 的既有 clock 合同保持，不能借错误请求倒退可信时间。

同一 Peer 包括相同 Principal 或相同地址的潜在冲突项，不只检查一个 Service。冷注册
不能在仍存在旧 Session 登记时，用另一个服务和空槽悄悄引入不同 Session。已存在的
服务必须显式 rebind；其他旧服务不会被连带激活。当前不提供删除墓碑来重置历史的接口。

更隐蔽的情况也必须拒绝：Service A 已迁移到 Session 2 并持有 Unknown，而 Service B
还保留已排空的 Session 1 墓碑。不能通过 B 的旧墓碑直接发布 Session 3；A 的未决义务
仍属于同一 Peer 的屏障。必须检查所有相关登记，而不只检查待替换槽的旧 Session。

## 6. 代际、Key 和路由域不是一回事

真实 Security/Foundation 仍负责 Session/Key persist-before-use、reload、后继单调性和
重启后的防复用。本地 Registry Generation 只防 Handle ABA，不替代持久化 Key Generation。
计数器 bank 按精确 Session 维护；新耐久 Key Context 可以从其合法初值开始，不能复用旧
Key/Nonce。生产 Provider 必须真正按 Suite/Key ID/Key Generation 区分 Key Material。

RouteOwner 的 `origin_route_scope`是冻结发现域；当前源码字段名仍是
`origin_session_generation`，语义沿用[合同14](14-多邻居认证路由控制与局部小MTU合同.md)，
它不是每个邻居或端到端密码 Session。仅更新端到端密码 Context 不偷偷改变路由发现域，
也不让路由回执变成端到端授权；终点仍用实际新 Context 验证 Origin。

本轮Host实测每 Endpoint 静态增加8 B，不新增历史表；小/测试 Root 的 Host 本体因此从
7824/19624 B 变为7832/19640 B。不含其他 Owner、workspace、Driver 和最坏栈；不是 MCU
资源或实时性证明。连续后继可以复用合法退休后的固定槽，槽代际与 Foundation 高水位不回退。

## 7. 实际验收与未完成项

13项定向回归覆盖真实 Foundation 后继、准备态/仅durable拒绝、全服务围栏、Unknown、
旧墓碑绕过、旧 App 消息、停止态、回调重入、当前 Identity 过期、O2→O1 降级拒绝和五次
连续槽复用。三节点在新 Key 下从 Origin Sequence=1 成功收发；迟到旧 Origin 在有效且新鲜
的 Hop proof 下仍被拒绝，避免用“刚好 Hop 重放”冒充新旧 Origin 域隔离。

O2只验证策略拒绝，成功数据链仍是 Host HMAC/O1；不代表 AEAD、实机或生产密码验收。
本段不产生跨多跳握手报文、不自动协商新端到端 Context、不处理 Binding 变更，也没有完成
全 Node 用户 Facade、C 自动 Root、完整 Flow、Group/Cluster 或运行时互操作。

本篇实施时整体回读发现直连缺口：当时Endpoint仅接受H0、邻居仅接受H1，但同Peer
只能有一条ACTIVE Session。后续合同18已用真实Context分层投影接通直连，不放宽单
ACTIVE、不复制Security Owner，也不省略Hop。历史OP-583及其矩阵只代表当时切片，
当前接线和验证应同时阅读合同18及其新报告。
