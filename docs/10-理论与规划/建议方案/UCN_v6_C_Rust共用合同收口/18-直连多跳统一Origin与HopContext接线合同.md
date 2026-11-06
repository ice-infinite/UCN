# 直连、多跳统一 Origin / Hop Context 接线合同

2026-10-18；当前软件实施与自审候选，未获独立外审或生产 GO。继续
[合同15](15-C1不透明逐跳转发与计数器共同合同.md)、[合同16](16-自动多跳Data协调器与本地下一跳合同.md)
和[合同17](17-Origin登记围栏排空与耐久后继交接合同.md)。本合同不新增普通头字段、
Opcode、控制正文、Record、Context selector 或第二套协议格式。

## 1. 要解决的实际问题

此前 Rust 自动 Data Root 把 Endpoint 限为 H0 Session，把邻居限为 H1 Session。
三节点 A→B→C 可用 AC Origin 和 AB/BC Hop，但 A→B 直连时，若为同一个 B 再建立
一个 H0 Session，就会触犯“同 Peer 只有一个 ACTIVE Session”的安全规则。

正确解法不是允许双 ACTIVE、复制 Security Owner、伪造 Session View 或关闭 Hop。
Origin 和 Hop 是两个认证角色，不是必须存在的两个 Session 对象。直连时可以从
**同一个真实 ACTIVE H1 Session** 分别取得 Origin 和 Hop 上下文；多跳时 Origin
取最终对端，Hop 取实际邻居。普通应用不选择两套报文，也不提供认证成功布尔值。

## 2. 连接与认证顺序

```text
ordinary send(endpoint, payload, options)
    -> Coordinator: current Identity / exact Endpoint / Origin ACL
    -> exact currently admitted neighbour is the target?
         yes: validate actual neighbour Session / Capability / MTU
              no RREQ or fabricated RREP receipt
         no: automatic discovery + exact Origin-domain RREP receipt
    -> Security: protect_c1_origin(actual endpoint Session)
    -> Security: wrap_c1_hop(actual next-hop Session)
    -> bounded immutable TX queue -> actual Adapter token -> Driver

atomic raw RX
    -> Coordinator: current Link + exact registered neighbour
    -> Security: open_c1_hop -> pending exact-packet proof
    -> target?
         no: exact route proof -> TTL-- -> new neighbour Hop -> queue
         yes: exact Endpoint -> open_c1_origin -> pending Origin proof
              preflight both -> commit both in exclusive quantum -> app queue
    -> application pull
```

箭头表示 Coordinator 逐次编排；业务 Owner 不直接调用另一 Owner。Driver locator 仅
定位本地物理下一跳，不是线上地址扩展或认证证明。中继不解密 Origin。

直连快捷路径只接受完整相等的目标 Binding 和已登记邻居，而非同地址、同 Link、历史
RREQ 或用户声明“这是直连”。实际父域、Session、ACL、Capability 和 MTU 仍在使用时
复验；缺少必需 capability 时在 Origin 序号消耗与 Driver 提交前拒绝。已准备直连帧
提交时还须复验冻结的 Peer Handle 和完整目标 Binding；不临时换到另一邻居。

## 3. 两层 Context 的唯一含义

| 项目 | Origin | Hop |
| --- | --- | --- |
| 证明对象 | 原始发送者与最终目标 | 当前物理邻居 |
| 真实来源 | Endpoint 实际 ACTIVE Session、Binding、Policy、Key、Fingerprint | 邻居实际 ACTIVE Session、Binding、Link、Capability、Key、Fingerprint |
| ACL | 最终 Peer、Service、Data opcode=0、方向 | 实际邻居、Service、Data opcode=0、方向 |
| 序号 | 同 Session 普通 Origin Sequence，不按 Service 重置 | 同邻居 Session 的共享 Hop Sequence，C0/C1 不能各自从 1 开始 |
| Replay | 端到端 Origin 窗口 | 独立邻居 Hop 窗口 |
| 中继行为 | 保留密文/明文与 Tag，不消费其 Origin Replay | 验入站、减少 TTL、重封出站 |
| 直连 | 可与右列使用相同 Session Handle | 仍须独立验 Tag 和 Replay，不能因 Handle 相等而省略 |

Session Handle 相等不代表两个 Fingerprint、Key selector、计数器或 Replay 可以合并。
反之，两个 Handle 不同也不构成端到端认证，必须逐层复验真实父域。

## 4. C / Rust 对应入口

| 角色 | C 内部入口 | Rust Security 入口 |
| --- | --- | --- |
| 只保护 Origin | ucn_i_security_c1_origin_protect | protect_c1_origin |
| 只打开 Origin | ucn_i_security_c1_origin_open | open_c1_origin |
| 完整单 Session packet | ucn_i_security_c1_protect / open | protect_packet / open_packet |
| 独立 H1 封装/验证/不透明中继 | C 自动 C1 中继接线仍待实施 | wrap_c1_hop / open_c1_hop / forward_c1_hop |

新增 Origin-only 入口只允许 C1 Data、O1/O2、真实 H0/H1 Session。Rust plan 的 H0
只表示这次输出/输入不含 Hop 尾部，是显式 Origin 投影，不修改 Session 协商的 H1。
C0、C2～C5、Control、O0、H2/H3 不能借此入口绕过原有的专用合同。

原完整 packet API 保持原语义：Hop Profile 必须与 Session 精确匹配；不能向 H1
Session 传 H0 plan 或去掉尾部后声称整个报文已认证。Origin-only 结果只证明 Origin，
不包含邻居认证；Root 在完整 Hop 与 Origin 成功并提交前不得发布应用消息。

C 保留原有完整直连 API，内部共用受限实现，不建立新 Owner 副本或额外 staging。
C/Rust 的 canonical AAD、Nonce input、端序、Tag 长度和序号域均沿用既有共同合同。
本轮 C 的 O2 回归使用 Host 可控测试 Provider 验证接口/Nonce/失败行为，不是生产 AES/
ChaCha 密码实现验收；Rust 自动路由 Root 仍限 O1/H1 邻居，不能外推为加密直连已验收。

## 5. 失败、计数器与原子提交

1. 错 Session、Binding、Fingerprint、Service、方向、层/Contract 或容量在输出写入前
   拒绝。Origin-only 不消费 Hop counter，也不预留或提交 Hop Replay。
2. Origin 与 Hop 使用相同 Session 时仍分别预留；终点先预检二者，再在无回调、独占
   借用的同一 quantum 提交。当前实现与回归证明 Origin commit 不会错误退休 Hop。
3. 任一 Origin 验证/容量失败释放尚未提交的 Hop reservation；失败输入不使合法后续
   帧变成已消费 Replay。Hop 验证失败不能产生 Origin/业务权限。
4. 外部 counter reset 不复用 Security 已消耗的值；Provider 已见的计数器不退款。
   Driver 明确 NOT_SUBMITTED 只重试原精确帧，不再次保护 Origin 或重新消费序号。
5. Source 的 LocalSuccess 不是目标交付。Unknown 不是失败退休，stop/reopen 不是物理
   释放证明。等待路由与等待实际 Token 分开报告。

## 6. 共享 Session 的撤权与排空补充

对 H1 直连 Endpoint 调用 fence_origin 时，实际 Security Session 已围栏，因此同
Session 的邻居角色也不能继续授权。Root 同步标记邻居围栏、停止其未提交 Control、
取消本地未完成控制组装并使相关 Route 失效；不删 Driver Token 或伪造 completion。

合同17的本地 drain 现在取本 Root 中共享该 Session 的 **Endpoint Data/App、邻居
Control 和出站中继 Data** 的真实义务并集。即使 Endpoint 自身从未发送，只要同
Session 的 C0 Unknown 尚未退休，也不得宣称已排空。

该并集仍不包括 Root 外的其他 Runtime/Flow/Provider/Driver 私有义务。Endpoint
后继发布不自动替换邻居登记；不凭新的 Origin Handle 复活旧邻居，也不为表满或
后继交接偷偷丢弃 Unknown。后续
[合同19](19-邻居围栏真实排空与耐久后继Capability热交接合同.md)现已接通已知Peer的
显式邻居热接纳；完整自动Reauth/Capability/全Node阶段机仍待，不混淆两种范围。

## 7. 字节与静态资源

| 地址档 | 既有 C1 头 | O1/O2 Origin Tag | H1 Sequence+Tag | 合计，不含业务 |
| --- | --- | --- | --- | --- |
| A0 | 11 B | 16 B | 16 B | 43 B |
| A1 | 13 B | 16 B | 16 B | 45 B |
| A2 | 15 B | 16 B | 16 B | 47 B |
| A3 | 17 B | 16 B | 16 B | 49 B |

本轮新增线上字节 **0 B**。直连不做 RREQ 可减少控制流量，但没有取消所选安全费用，
也没有声称吞吐/延迟已实测改善。Owner、Endpoint、TX 队列项和 workspace 不新增字段；
private helper 和投影枚举不增加持久化状态。Host size 不等于 MCU 总 RAM 或最坏栈。

## 8. 小项自审与正式回归

| 小项 | 可区分的验收 |
| --- | --- |
| UH-01 两层 API | 完整 H1 API 仍拒绝 H0；Origin-only 不消耗 Hop；重置 Origin counter 被拒 |
| UH-02 自动直连 | 实际双向单 Session 交付；没有 RREQ；使用已消耗过 C0 的共享 Hop 域 |
| UH-03 混合拓扑 | 同一 Root、同一 send API 同时给邻居与远端发送；中继和目标身份不混淆 |
| UH-04 双层拒绝 | 错 Hop、有效 Hop 下错 Origin、原合法帧后到均区分；失败不提交 Replay |
| UH-05 共享撤权 | Data Unknown 与 C0 Unknown 都阻塞排空；不以 stop 或墓碑伪造静止 |
| UH-06 C 对等原语 | O1/O2 三种测试 Suite，短输出零写、错 Fingerprint/Tag/Replay 拒绝，投影后完整发送保持 Origin2/Hop1 |
| UH-07 总体自审 | Full/Lite/Nano C、Rust 完整软件矩阵、裁剪、资源、文档与源码依赖哈希 |

Rust [实际 Root 回归](../../../../rust/crates/ucn-runtime/tests/bootstrap_runtime/route_tests/data_node/contexts.rs)
与 [Security 层矩阵](../../../../rust/crates/ucn-security/src/packet.rs)；C
[对等回归](../../../../tests/simplified/test_security_session.c)。实际结果与范围见
[本轮自审报告](../../../08-实现与验证/版本演进/UCN_V6S_RUST_SW_02_直连多跳统一Context自审报告.md)。

## 9. 未完成边界

当前 Rust 自动 Root 是已知、真实发布 Session 的 O1 邻居、尽力单向 Data 子集。
O2 自动邻居控制、完整自动邻居热续接阶段机、动态端到端 Session 握手、C 自动 Data Root/C1 中继、
Flow、统一 Reliable/Transfer/Journal/Time Node、Group/Cluster 和跨语言 Runtime 互通
仍有软件任务；不能把本合同或所有普通测试通过称为只差实机。

本轮没有实机、COM/USB 枚举、烧录、复位或调试，没有真实 Flash、Witness、生产密码、
硬件时间戳、WCET/栈水位、性能或长稳证明。独立外审、生产及实机均维持 HOLD。
