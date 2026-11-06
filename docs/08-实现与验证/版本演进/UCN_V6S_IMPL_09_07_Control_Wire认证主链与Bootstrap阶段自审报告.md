# UCN V6 简化版 IMPL-09-07 Control Wire 认证主链与 Bootstrap 阶段自审报告

## 1. 阶段结论

本轮完成 `IMPL-09-07` 的第一段，而不是完成全部控制协议：

```text
typed control input
  -> C0 encode
  -> Security origin/hop protect
  -> Adapter submit/retry
  -> remote Adapter selective claim
  -> C0 structural decode
  -> Security open + ACL/use-time validation
  -> authenticated fact
  -> Composition module dispatch
```

未绑定 Bootstrap 另走严格受限的 O0/H0 一跳路径，并已有三个精确 typed Body。其余 Opcode 已获得
统一编号、结构验证、安全认证和目标模块分类，但尚未逐个实现业务 Body 与 Owner 状态转换。因此：

```text
Control common transport      = IMPLEMENTED
Bootstrap typed slice         = IMPLEMENTED
Route RREQ/RREP/RERR typed RX = IMPLEMENTED
Capability six-message RX     = IMPLEMENTED
Diagnostic Ping/Pong typed RX = IMPLEMENTED
Bounded ingress scheduler     = IMPLEMENTED
Multi-hop Origin/Hop relay    = IMPLEMENTED
Typed-handler replay fence    = IMPLEMENTED
All registered opcode semantics = NOT IMPLEMENTED
IMPL-09-07                    = IN PROGRESS / PARTIAL SELF-REVIEW PASS
```

## 2. C0 共用线上结构

C0 固定承载低频控制与诊断事务。当前实现按地址宽度 `W=1..4` 形成：

```text
Common(3)
+ Realm(4)
+ Source(W) + Destination(W)
+ Source Binding Generation(4)
+ Destination Binding Generation(4)
+ C0 Transaction(8)
+ Protocol Opcode(2)
+ Body(N)
+ optional Origin Tag(16)
+ optional H1 {hop_sequence(4), hop_tag(12)}
```

基础 Header 为 `25 + 2W` B。所有多字节整数为 big-endian；普通地址禁止 0 和全 1 保留值；
Transaction、Realm、普通 Binding Generation 必须非零。Codec 在首次输出写入前完成参数、范围、
长度、保留值和输入/输出别名检查，失败时 `frame_out/output/output_bytes` 保持不变。

Registry 固定 94 个 Opcode，并按十个域分类：Identity/Security、Capability/Neighbor、Route、Flow、
Transport、Service/Operation、Time、Group、Cluster、Diagnostics。Registry 只证明编号和路由域合法，
不能替代每个 Opcode 自己的 Body validator。

## 3. Origin AAD 与安全主链

Origin AAD 包含不可由中继改写的 C0 身份：Version/Contract、Traffic/Delivery/Role/Kind、Origin
Security、Realm、双地址、双 Binding Generation、Transaction 和 Opcode。Common byte 2 中的
HopLimit 位在 AAD 中强制为零，中继只能递减 HopLimit，不能借此破坏端到端 Tag。

O1/O2 只能使用当前 Security Session 和精确 Opcode ACL：

1. TX use-time 重新核对 Session、selector、Binding、方向、Opcode 和期限；
2. checked high-water 保留 C0 nonce 身份；
3. Provider 生成 Auth-only 或 AEAD 结果；
4. 完整 packet 冻结后才交给 Adapter；
5. `NOT_SUBMITTED` 退休本次 Adapter Token，但同一逻辑重试复用完全相同的 packet；
6. RX 先完成结构解析，再按当前 selector/ACL 验证 Origin/Hop Tag 和 replay；
7. 只有 `authenticated control fact` 能进入 Composition dispatch。

调用者不能把裸 Driver 字节、任意 plaintext 或只含 Opcode 的伪造结构直接交给业务 Owner。

## 4. Adapter 与 Composition 分派

Adapter 的 C0 claim 只接受精确 Runtime/Owner/Link Generation，返回的 Frame View 仍是未认证事实。
Composition 在同一生命周期与 Coordinator gate 内执行：

```text
claim -> open -> authenticated fact validate -> target module resolve
      -> selected/active module check -> frozen dispatch plan
```

未选择模块、Feature OFF、Owner 非 ACTIVE、Opcode 域不匹配、Session/Capability/ACL 失效或 Plan 被
改写时均拒绝，业务 Owner 不获得正文。prepare/finish 重新验证相同 fact，避免旧 dispatch plan 在
状态变化后继续授权。

## 5. O0 Bootstrap typed 切片

O0 不等于“任意未认证控制帧”。它只允许：

```text
source address              = 0
destination address         = 当前地址宽度的全 1 保留地址
source/destination binding  = 0
origin security             = O0
hop profile                 = H0
hop limit                   = 1
opcode domain               = Identity/Security
```

当前只实现三类精确 Body：

- `BOOTSTRAP_HELLO`；
- `BOOTSTRAP_COOKIE_CHALLENGE`；
- `BOOTSTRAP_HELLO_COOKIE`。

TX 先 typed encode，再形成 C0 packet 并提交 Adapter；背压重试保持 packet 字节不变。RX 先选择性
claim，随后验证完整 C0/O0 合同和 Body 长度/字段，最后发布 typed message。错误 Opcode、错 Link、
H1/O1/O2、HopLimit 非 1、普通地址、截断 Body 或 finish identity 不匹配均失败且不发布消息。

## 6. 已认证 Route 直达 typed 切片

Route 域先接入冻结的 `RREQ(9 B) / RREP(29 B) / RERR(99 B)` Body Codec，不允许把任意
`bytes + Opcode` 直接交给 Route Owner。接收路径严格执行：

```text
C0 structural decode
  -> Origin/Hop verification + exact ACL + replay reserve
  -> authenticated fact AAD/body revalidation
  -> exact Route Owner/Ingress Link/current binding check
  -> typed Body decode
  -> unique Route handler
  -> replay commit/abort by the original RX plan
```

`RREQ/RREP/RERR` 分别只接受 `Request/Result/Error`，并固定为 Q0、Best-Effort、Control。
认证 Fact 现在保留并重新验证 Traffic、Delivery、Interaction、Payload Kind、Origin Security 与
HopLimit；正文摘要正确但这些元数据被改写时也不能通过。RREP 的目标 Principal/Binding 必须等于
当前认证 Peer；RERR Reporter 必须等于当前认证 Peer，不能用合法会话替另一个节点报告故障。
Ingress Link 是独立逐跳事实，必须精确匹配当前 Security Link Generation 与 Peer Binding；错误
Link、旧本地 Binding、伪造 Reporter、错 Role 均在 Route 状态写入前拒绝，输出保持逐字节不变。

现实现已把端到端 Origin Session 与当前逐跳 Hop Session 拆开。源端可用独立 Origin/Hop Context
生成 H1 Packet；Relay 只验证当前 Hop、预留逐跳 Replay、按当前 Route 解析下一跳、递减 HopLimit
并重封 Hop Tag，不解密或改写 Origin Payload/Tag；终点分别验证 Hop 与 Origin，并分别提交两个 Replay
obligation。Link reopen、Route 不唯一、出站 Hop Session 不唯一、HopLimit 耗尽、Adapter 背压或 Plan
篡改都在唯一退休路径上失败关闭。三节点回归还刻意让 Origin Session 的 Link Generation 与实际
Hop Link Generation 不同，证明两类 Context 未再次被错误合并。

## 7. Capability 六消息真实 RX typed 切片

Capability 域已接入 `NEIGHBOR_HELLO/KEEPALIVE/ADVERTISE/SELECT/ACK/REVOKE` 六个精确
Body Codec 与 Owner 语义。Handler 不接受调用方另传 Link ID，而是直接消费真实
`control_rx_plan` 冻结的 Adapter Link slot/generation，并依次执行：

```text
Adapter RX plan current-link recheck
  -> authenticated C0 fact/AAD revalidation
  -> current Security Session re-read
  -> exact peer/binding/session/policy/link/lease match
  -> 68 B Capability Record decode
  -> unique Capability Owner ingest
  -> caller commits or aborts the same Replay/RX obligation
```

Capability Reference 的 Link ID 取 Runtime 内固定槽号 `link_slot + 1`，Link Generation 取当前
Adapter Instance Generation；同槽 reopen 后旧引用不会匹配。错误 Role、错误 Body 长度、Record
中的 Link Generation 与认证 Link 不一致、旧 Session/Policy/Binding 或关闭 Capability Owner，均在
缓存提交前拒绝，结果对象不写回。SELECT/ACK 还精确绑定双方协商结果和 transcript；REVOKE 只能
撤销当前 Session/Link 的目标缓存。成功路径均由真实 Adapter publish/claim 驱动，并回读 Owner
状态确认 Principal、Binding、Session、Link 与 Record 完整一致。

## 8. 诊断终结能力、Replay 围栏与统一有界 Ingress

Diagnostics 域已接入 `PING/PONG` 的固定 8 B big-endian Nonce Body。PING 只接受 `Request`，
PONG 只接受 `Result`；两者均固定为 Q0、Best-Effort、Diagnostic、O1/O2，零 Nonce、错误 Role、
截断/扩展 Body 或未认证输入均在业务状态前拒绝。PING 成功后只发布一个携带相同 Nonce 的 PONG
响应义务，不把收到的正文直接当作任意输出缓冲。

Registry 的合法性现已拆成三个互不替代的属性：`REGISTERED` 只证明编号存在，`RELAY` 只允许中继
保持 Origin 密文不变地转发，`AUTH_TERMINAL` 才允许当前 Runtime 在完成认证后本地终结。未实现精确
Body 的 Flow/Transport/Service/Realtime/Group/Cluster Opcode 目前只有 `RELAY`，目的节点收到时明确
返回 `UCN_ERR_UNSUPPORTED`，不能因为 Registry 合法就进入 Owner。

Security Replay 预留还增加了 typed business-completion 围栏。Route、Capability 与 Diagnostics 的
唯一 handler 成功后，才可标记同一 Replay reservation 已完成业务语义；通用 finish 即使收到
`consumed=true`，也必须再次核对该标记，否则拒绝提交 Replay。这样调用者不能跳过 Body validator，
把“已认证但语义尚未实现”的任意正文冒充已执行消息。

发送侧的通用 protect/submit 边界也不再接受“任意 bytes + 已登记 Opcode”。它先核对该 Opcode 具有
本地强类型实现，再复验 Traffic/Delivery/Role/Kind 与精确 Body Codec，之后才允许进入 Security
Provider、Sequence 预留和 Adapter。尚未实现的 `OPERATION_QUERY/TIME_SYNC` 即使拥有合法 Registry
编号和 ACL 也返回 `UCN_ERR_UNSUPPORTED`；零 Nonce Diagnostic 则在 Provider 调用前按畸形 Body
拒绝，并保持输出、长度与密码调用次数不变。

### 8.1 有界 Ingress 与固定栈整改

Composition 已用一个持久轮转游标在 Foundation C1、Bootstrap C0 与认证 C0 三类入口之间调度；
每类每次只消费一个有界 operation，Foundation 仍受调用者 `StepBudgetPlan` 约束。被选中的
Bootstrap、终止型 Control 或 Relay obligation 完整保存在 caller-owned workspace，直到
`ingress_finish()` 精确提交/撤销 Replay 并退休相同 Adapter Token；不存在第二个隐式消费者。

多跳接入后，四个热函数一度因完整 Frame/Fact staging 使用自动对象而超过 256 B 单函数栈门限。
整改没有抬高门限，而是把 Origin Resolve、Hop Relay Fact、RX Prepare 和 Scheduler Result 移入
调用者固定 workspace。Full 构建的 Kernel/Security stack-usage gate 与语义回归均已通过。

## 9. 本轮自审整改

### 9.1 Plan 完整性

Plan validator 现在重新推导 available/compiled/selected/durable mask、初始化顺序、资源字节和 digest，
不再通过构造另一份大型 Plan 做递归比较。测试分别改写 mask、init order、bytes 和 digest，均被拒绝。

### 9.2 Persistence 与生命周期栈

Composition 的 Persistence step 结果移入 Runtime-owned staging；Persistence init 的 manifest digest
使用 caller-owned digest workspace 尾部；stop 路径使用窄私有 step。普通 Nano 的 Persistence init
最坏调用链为 768 B，达到但不超过当前硬门限，后续任何调用链变化都必须重新测量。

### 9.3 Adapter 热路径

Adapter lock/handle 热路径删除只做重复包装的栈帧，同时保留 Runtime、Owner、slot、generation、kind
与 reserved-zero 的完整校验。归档门禁仅放行精确的简化 Kernel 私有符号，不扩大通配规则。

### 9.4 跨编译器测试

Control Wire 测试不再依赖 Release 下会消失的标准 `assert`，而是使用始终执行的检查宏；MSVC
Release `/W4 /WX` 因而能真实执行全部断言，而非只编译空壳。

### 9.5 共享 RX 队列的入口所有权分区

Composition 启动 Foundation 时显式置入私有 `external_ingress_demux` 状态。该状态只允许 `0/1`，
并在 Owner 发布前完成设置：组合产品中的 Foundation C1 worker 只可选择性认领
`{version=6, contract=C1}`，不得消费 C0 或未来其他 Contract；独立 `ucn_node_t` 则继续由通用
Decoder 认领并丢弃未知 Contract，保持原有 standalone 合同。停止阶段仍由窄 stop worker 回收
全部未决 RX obligation，避免关停时遗留无法退休的非 C1 帧。

新增对抗顺序固定为“先发布 C0、再发布 C1”：一次 Composition C1 step 必须交付 C1，同时 C0
保持 READY；随后 Bootstrap 路径可精确认领并按不支持的 typed Opcode 退休该 C0。该回归能够区分
旧实现中 Foundation 抢先认领 C0、按 malformed C1 丢弃的错误。协议 Version/C0/C1 Contract 号也
已收敛到单一私有 Wire 常量，避免各消费入口继续使用散落 magic number。

## 10. 验证结果

| 门禁 | 结果 |
| --- | --- |
| Windows GCC Full Debug | 有效 `108/108` |
| Windows GCC Lite Debug | 有效 `108/108` |
| Windows GCC Nano Debug | 有效 `108/108` |
| Windows GCC Persistence-OFF | 有效 `97/97` |
| Windows GCC Optional-OFF | `80/80` |
| MSVC 19.29 Release `/W4 /WX` | `90/90` |
| WSL ASan/UBSan 语义与并发矩阵 | `105/105` |
| WSL `-fanalyzer -Werror` 构建与适用矩阵 | `116/116` |
| Control/Composition/Archive/Nano 栈定向 | `5/5` |

上述历史矩阵之后，本轮新增的 scheduler、多跳与栈整改又在 Windows GCC Full 上完成一次完整
`108/108`；其中 Kernel/Security 单函数栈门限、三节点 Origin/Hop 分离、源端 Path TX、Relay、
终点双 Replay 与重复包拒绝均在同一构建中通过。其他 Profile/编译器的最终数字将在 IMPL-09-07
全部 Body/Handler 完成后统一重跑，不能拿本次 Full 数字代替。

Full/Lite/Nano/Persistence-OFF 最初的唯一失败是归档检查把新增精确私有符号误判成旧 v6 残留；修正
检查器的精确 allowlist 后同一归档门禁均单独复跑通过，所以表中记为“有效”总数。ASan 排除了会被
sanitizer frame 膨胀污染的 `.su`/call-stack 门禁和未传递 sanitizer runtime 的独立安装消费者；
`-fanalyzer` 排除了把 analyzer dynamic frame 当未知栈的 call-stack 门禁。普通 Nano GCC 负责真实
产品栈证据，MSVC/GCC 普通构建负责安装与 ABI 证据。

## 11. 未完成项

`IMPL-09-07` 继续保持 IN PROGRESS，下一段必须完成：

1. 为已完成 Bootstrap、Route、Capability 与 Diagnostics 之外的控制域冻结并实现精确 Body Codec，不能用
   “任意 bytes + Opcode”代替 typed 合同；
2. 将每个可终止 Body 路由到对应 Owner 的唯一 handler，并验证 Feature OFF/未选择能力零状态拒绝；
3. 增加其余 Opcode 的 raw↔typed Golden/Negative、错误 Role/方向/代际与重放测试；Route 已复用
   Owner 独立 Golden，并补认证元数据、Ingress Generation、Binding 与 Reporter 对抗回归；
   Capability Advertise 已复用 Record Golden，并补真实 RX、错误 Role、长度和 Link Generation 回归；
   Diagnostics 已补 8 B Golden、零 Nonce、Role 与 typed-completion 旁路回归；
4. 保持 Realtime、Group、Cluster 物理 archive 不互链；
5. 完成后再进入 IMPL-09-08 资源总账，不能用本阶段 Host PASS 替代 MCU/物理 Bearer。
