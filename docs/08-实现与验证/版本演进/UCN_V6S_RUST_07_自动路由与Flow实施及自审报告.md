# UCN V6S RUST-07 自动路由与 Flow 实施及自审报告

## 1. 结论

```ini
RUST-07 = DONE / SELF-REVIEW PASS
RUST-08 = ALLOWED TO START
EXTERNAL_REVIEW = DEFERRED UNTIL THE COMPLETE RUST WORKSTREAM
```

本阶段完成 Rust 独立实现的两个不同层次：

1. `ucn-routing` 负责“当前怎样到达目标”：RREQ/RREP/RERR、Reverse obligation、易失
   SoftRoute、静态 fallback 和 Route Generation；
2. `ucn-flow` 负责“高级业务能否安全使用这条路径”：Candidate、Probe、不可变 Proposal、
   Stage/Commit/Abort、Forwarding Label、Receipt、`IN_DOUBT`、使用前重验和 Flow Replay。

二者没有共用 `ACTIVE` 状态。RREP 在中继逐跳安装 SoftRoute 不等于 Flow 原子激活；只有完整
Stage/Commit 链按 Target→Relay→Origin 逆序收敛后，Flow 才可用于 C2/C3/C4。

结论仅覆盖 Host 软件模型、独立 Wire Oracle 和两个 Cortex-M `no_std` 编译目标。没有完成统一
动态 Runtime、生产密码 Provider、真实 Flash、物理多跳、ESP32-S3 资源水位、吞吐、时延或长稳。

## 2. 权威输入与实施边界

实现逐项核对：

- V6S 基础通信与自动路由简化设计；
- Advanced Flow、标签和两阶段激活合同；
- 低开销 C2/C3/C4 前缀、RREQ/RREP/RERR 与 Path Control Registry；
- Owner/Coordinator 唯一跨模块编排边界；
- Security Session、Capability Cache 和 Profile Resolver 的现有 Rust typed View；
- 固定容量、半开 Deadline、不回绕 ID、失败零写、禁止 lazy eviction 的共同规则。

明确不属于 RUST-07：

- Reliable Receipt、Transfer 分片/重组、Service/Operation；
- Realtime、Group、Cluster；
- Route/Flow 与 Adapter、Admission、Persistence 的统一生产 Runtime 接线；
- 持久化 Route/Flow 或跨重启延续动态路径；
- 将 Host Fake/软件 HMAC 当作生产密码或安全元件证明。

## 3. crate 与 Owner 依赖

```text
ucn-security ── Binding / authenticated session facts ─┐
ucn-types ──────────────────────────────────────────────┼──> ucn-routing
                                                       │
ucn-routing ── immutable SoftRouteView ─────────────────┤
ucn-capability ── immutable CachedPeerCapability ───────┼──> ucn-flow
ucn-security ── Flow Security / Replay DTO ─────────────┤
ucn-types / ucn-wire / persistence hash workspace ──────┘
```

- Route Owner 不依赖 Flow Owner；
- Flow Owner 读取 `SoftRouteView`，不调用或修改 Route Owner；
- Flow Owner 读取认证 Capability 快照，并在 Stage 和每次 Flow 使用时要求 Coordinator 提供
  当前事实再次比对；
- Security 不调用 Flow Owner。Flow Owner 导出完整 Requirement，经 Coordinator 路由后，由
  Security Owner 签发字段私有、普通业务不可构造的 `FlowSecurityBinding`；
- Owner 均不直接调用 Driver、Adapter、时间源或其他 Owner。

## 4. RUST-07-00：基础 Route 与高级 Flow 分界

### 4.1 冻结结果

SoftRoute 只表达“在给定父代际和租期内，下一跳可用于尝试转发”。它不表达：

- 路径已经端到端 Probe；
- 所有中继均预留 Label/Flow 槽；
- 目标已接受 Commit；
- C4 Origin Security Context 已建立。

Flow Proposal 则冻结完整 Route Domain、Route Generation、Candidate/Transaction ID、正反 Label、
Profile、MTU、Capability、Session、Policy 和绝对 Deadline。Proposal 一旦形成不再被 RREP 改写。

### 4.2 分项自审

对依赖图和全部公开类型回读，确认没有：

- `ucn-routing -> ucn-flow` 反向依赖；
- 共享可写 Route/Flow 表；
- 用 SoftRoute 的 `valid` 冒充 Flow `Active`；
- 在 Route/Flow Owner 内直接提交 Adapter 或 Persistence。

结论：模块与 Owner 边界通过。

## 5. RUST-07-01：Route Domain 与 Wire Codec

### 5.1 固定布局

| 对象 | 长度 | 关键字段 |
| --- | ---: | --- |
| RREQ payload | 9 B | Transaction、Hop limit、最低 MTU、Capability bits |
| RREP payload | 29 B | Transaction、Route Generation、MTU、Cost、Hop、Causal ID |
| RERR payload | 99 B | 完整 Route Domain、Route Generation、Causal ID、失败 Link 代际、原因 |
| Label Setup | 16 B | Transaction、正反 Label |
| C2 prefix | 9 B | Flow Context 与 Origin Sequence |
| C3 prefix | 7 B | Forwarding Label 与 Hop Sequence |
| C4 prefix | 11 B | Forwarding Label、Flow Context 与 Origin Sequence |

所有整数使用 big-endian；Decoder 要求 exact length、合法 reserved/enum/ID；Encoder 先完成所有
验证再写输出。失败不得改变输出缓冲区。

### 5.2 分项自审

初始实现把 Decoder 的底层长度错误直接透传，造成同一 malformed Wire 输入得到多个外部错误类别；
已统一为 `Malformed`。同时补齐 target 产生 RREP 时合法 `hop_count=0`，避免把目标的第一份合法
响应误判为坏包。

结论：Codec 分项通过。

## 6. RUST-07-02：Discovery、Reverse 与 SoftRoute

Route Domain 精确包含：

```text
Realm
+ Origin Binding
+ Origin Peer Session Generation
+ Destination Binding
```

本地 `ensure_discovery()` 仅在完整要求相同时合并已有请求；目标相同但最低 MTU、Capability bits、
Hop limit 或 Domain 不同，必须占用独立事务。非法输入在分配 Transaction ID 之前拒绝。

中继接收 RREQ 后先建立固定容量 Reverse obligation，再决定转发或由目标产生 RREP。RREP 沿该
Reverse obligation 返回；每一跳可以安装易失 SoftRoute，但只有本地 Origin 收到完整 RREP 才完成
本地 Discovery。重复请求只能返回原事务，不能刷新绝对 Deadline。

动态 Route 查找优先，未找到动态实例后才使用静态 fallback。静态 Route：

- 不持有动态 Route Generation；
- 不被动态 RERR 删除；
- 不因上游业务帧携带动态 Route Epoch 而失去 fallback 资格。

### 6.1 分项自审

自审关闭三项：

1. “同目标”不能替代完整 Discovery Requirement 相等；
2. RREP 必须满足原 RREQ 的最低 MTU、Capability bits 和 Hop limit，不能只凭 txid 接受；
3. 表满时即使存在过期槽，也不能在输入路径隐式驱逐；必须先执行显式有界 maintenance。

结论：Discovery/SoftRoute 分项通过。

## 7. RUST-07-03：使用时重验与 RERR

业务使用 SoftRoute 前必须提交当前 `RouteUseFacts`，逐项匹配：

- 本机 Origin Binding 与 Session Generation；
- 下一跳 Binding、Link ID 与 Link Instance Generation；
- Route Generation 与 Causal ID；
- 当前 MTU、Capability bits 与半开 Deadline。

普通转发成功不会续租。Link 失效和 Deadline 回收由显式 API/持久游标推进。

RERR 不能依赖“Destination + 当前表项”推断因果关系，而是携带完整 Route Domain、Route
Generation、Causal ID、失败 Link ID/Generation 和 Reason。只有全部字段精确匹配当前 SoftRoute
才删除；迟到 RERR、其他 Origin、更新后的 Route、静态 fallback 均保持不变。

结论：Route use/RERR 分项通过。

## 8. RUST-07-04：Candidate、Probe 与不可变 Proposal

`import_candidate()` 只接受当前 Route/Capability/Requirement 的一致组合。验证包括：

- Route Realm/Origin/下一跳与本机、Capability Peer Ref 一致；
- Link、Binding、Session、Capability Generation/Digest/Deadline 一致；
- Route 和 Capability 均满足 MTU、feature bits、Hop/Profile；
- C2/C3/C4 的 Delivery、Interaction、Payload Kind、Origin/Hop Security 组合合法；
- Policy Generation 与所有 Deadline 当前有效。

`begin_probe()` 分配 Proposal 与绝对 Probe Deadline。`on_probe_ack()` 只接受完整 Proposal Key、
不降低 MTU/Capability、合法 RTT 的 ACK。完成 Probe 后，Candidate 进入 `ReadyToStage`；后来的更优
RREP 不能修改已冻结路径，必须创建新 Candidate。

### 8.1 分项自审

正向实现后从“过期对象怎样释放”反查，发现：

- 未 Probe 的 Candidate 可在 Route/Capability/Requirement 到期后长期占槽；
- `ReadyToStage` 可超过 Probe Deadline 后仍启动 Stage。

现已要求 `begin_stage()` 在 exact Probe Deadline 失败关闭，并由固定游标显式回收 Candidate、
Probing 和 ReadyToStage。失败不消耗 Activation/Flow/Receipt 槽。

结论：Candidate/Probe 分项通过。

## 9. RUST-07-05：Stage、Commit、Abort 与 InDoubt

### 9.1 正常激活

```text
Origin: StagePending --Stage--> Relay: StagePending --Stage--> Target: Staged
Origin: StagePending <- ACK --- Relay: Staged      <- ACK --- Target
Origin: Committing  --Commit--> Relay: Committing  --Commit--> Target: Active
Origin: Active      <- ACK ---- Relay: Active       <- ACK ---- Target
```

每个节点先预留 Activation/Flow/Receipt 槽，再产生外部消息。Target 最先发布；Relay 必须拿到下游
Commit ACK 才发布并向上游 ACK；Origin 最后发布。错 Key、错 Proposal、错父事实、错阶段、过期或
容量不足全部在写状态前拒绝。

### 9.2 不确定与撤销

- Commit 已提交或结果未知：进入 `IN_DOUBT`，保留事务键和对账义务；不得当作 Abort 成功；
- Commit 尚未提交，Stage 已下发：Abort 沿冻结路径向下游传播，收到 terminal receipt 后逐跳
  Fenced；
- Stage 从未提交：本地直接 Fenced，零 Wire；
- Target 没有下游：Stage lease 到期时直接 Fenced 并保留 receipt；
- Relay/Origin 已向下游提交 Stage：到期只进入 `ABORTING`，不能无证据回收；
- `IN_DOUBT` 只由精确、未过期的认证 terminal receipt 收敛。

### 9.3 分项自审

反向回读发现原超时逻辑把 Target 和未提交 Stage 的 Origin 也推进 `ABORTING`，但它们没有可发送
或必须发送的下游撤销，最终会无意义占槽。已按“是否可能存在远端 Stage 副作用”分流，并增加
三类精确回归：未提交 Origin 直接 Fenced、已提交 Origin 进入 Aborting、Target 自动 Fenced。

结论：两阶段激活与撤销分项通过。

## 10. RUST-07-06：Active Flow 与 Security/Replay

### 10.1 每次使用重验

发送与中继转发都要求 `FlowCurrentFacts` 精确匹配：

- Local/Destination/Next-hop Binding；
- Origin Session Generation；
- Capability Runtime/Security Owner/Binding/Session/Link Ref；
- Capability Generation、Digest、Deadline；
- Policy Generation 与 Flow Deadline。

任何父代际变化都拒绝使用并可显式 Fence Flow。缓存 Proposal 本身不是永久权限。

### 10.2 C2/C3/C4

- C2：只允许一跳，支持需要 Origin Security/H2 的直接高级路径；
- C3：只允许多跳 BestEffort + OneWay + O0，并使用逐跳 Label/Hop Sequence；
- C4：多跳 Flow，Origin Security Context 和 Origin Sequence 必须绑定当前 Flow。

Flow Owner 实现 Security 所需的 `OriginSequenceOwner`，但 Sequence 只有在密码 Provider 首次观察
前才 burn。Flow Owner 的 Origin/Hop API 在当前父事实通过后直接导出完整 `FlowSecurityRequest`，
Coordinator 不再手工拼接 Flow fingerprint、Label、Route Generation 或业务合同。Security Owner
再按当前 Session、父 ACL、Policy 和 Deadline 签发 Binding。

RX 通过 Flow 域执行 Replay `reserve -> commit/abort`：认证成功只预留，业务副作用成功后才
commit，失败则 abort。Security Session 不再重复推进 C4 Origin Replay。

`FlowSecurityBinding` 的构造字段保持私有，普通业务调用方不能用 struct literal 伪造 C4 Context。

结论：Active use-time 与安全绑定分项通过。

## 11. RUST-07-07：Oracle、Negative、容量与故障矩阵

共享语言无关 fixture：

`rust/tests/conformance/v6s_routing_flow_v1.h`

独立 Python oracle：

`tools/v6/check_rust07_routing_flow_oracle.py`

Oracle 不调用 Rust Codec，而是独立使用 `struct.pack` 重建七个冻结对象。Rust conformance 测试
直接读取同一 C header，避免 encoder/decoder 同错自洽。

测试覆盖：

- exact length、reserved、enum、ID 终值、wrong-domain 和失败零写；
- 4,096 个固定 Seed 的 C2/C3/C4 前缀 roundtrip；
- 三节点 RREQ/RREP 与 Stage/Commit/Abort；
- 表满、不 lazy-evict、重复不续时、错 ACK、迟到 RERR、Link/Session/Capability/Policy 变化；
- Stage/Commit 发送失败与 `IN_DOUBT`；
- Flow Replay duplicate/stale/in-flight、commit/abort 和 Deadline；
- Nano/Lite/Full 固定对象尺寸。

Host ABI 对象尺寸：

| Owner | Nano | Lite | Full |
| --- | ---: | ---: | ---: |
| Routing | 1,840 B | 5,408 B | 14,368 B |
| Flow | 3,448 B | 12,712 B | 41,768 B |

这些数字是当前 Host target 的结构布局，不是 ESP32-S3 的最终 RAM、Flash 或任务栈水位。

结论：Oracle/Negative/Resource 分项通过。

## 12. RUST-07-08：全体正向与反向自审

### 12.1 第一轮：Owner→Wire→FSM

```text
Authenticated Link/Capability facts
  -> Route Discovery / Reverse obligation
  -> volatile SoftRoute
  -> Candidate / Probe / immutable Proposal
  -> Stage / Commit
  -> Active Flow
  -> C2/C3/C4 preflight and Security/Replay
```

逐入口检查：输入是否合法、ID 在何时分配、首次写状态在哪里、输出何时写入、Deadline 是否半开、
重复是否刷新租期、表满是否会隐式驱逐。

### 12.2 第二轮：Failure→Proof→Owner

```text
wrong/late packet
  <- exact Route/Activation/Replay key
  <- current Link/Session/Capability/Policy facts
  <- immutable Proposal/SoftRoute View
  <- sole writable Owner
```

重点检查并关闭：

1. target RREP 合法零 Hop 被拒绝；
2. 同目标但不同 Requirement 被错误合并；
3. RREP 未复核原始 MTU/Capability/Hop 合同；
4. Stage/Stage ACK 未复核当前父事实；
5. Probe/Candidate 到期后占槽或继续 Stage；
6. Target/未提交 Stage 到期后进入无法完成的 Abort 链；
7. Commit 不确定被错误回滚；
8. C4 Replay 被 Flow 与 Security 双重消费；
9. 迟到 RERR 删除新 Route 或静态 fallback；
10. 过期槽在新输入路径被 lazy-evict；
11. Origin 输出 forward label、Relay 却接受 reverse label 的编解码同错；
12. 测试由调用方手工拼 Flow Security Requirement，遗漏 Coordinator typed dependency 边界。

当前自审没有发现仍开放的软件 P0/P1。该结论不替代后续独立外审。

## 13. 验证矩阵

| 门禁 | 结果 |
| --- | --- |
| `cargo test --workspace` | 133/133 PASS |
| `cargo test --workspace --release` | 133/133 PASS |
| `cargo +1.85.0 test --workspace` | 133/133 PASS |
| `cargo fmt --all -- --check` | PASS |
| `cargo clippy --workspace --all-targets -- -D warnings` | PASS |
| `RUSTDOCFLAGS=-Dwarnings cargo doc --workspace --no-deps` | PASS |
| `thumbv7em-none-eabi` | PASS |
| `thumbv7em-none-eabihf` | PASS |
| RUST-07 独立 Wire oracle | 7/7 PASS |
| V6 当前文档门禁 | 108 documents / 278 links PASS |
| V6S 合同冻结门禁 | PASS |
| C 当前非历史签字测试 | 51/51 PASS |
| `git diff --check` | PASS，仅 CRLF 提示 |

完整 C CTest 还包含 `v6s_impl02_candidate_gate`。该门禁绑定早期 IMPL-02 外审候选；RUST-07 修改
任务表和 Rust 总体设计后，旧清单按设计报告 stale。此处没有重写历史签字文件，也没有把 51/51
伪称为 52/52。失败表达的是“旧候选签字不可复用于当前工作树”，不是 C Runtime 回归。

## 14. 未证明事项与下一阶段

RUST-07 完成后仍未证明：

- 自动路由/Flow 已接入统一生产 Runtime；
- 真实 Wi-Fi、ESP-NOW、CAN、UART 或其他 Bearer 的多跳路径；
- 生产密码库、TRNG、安全元件与侧信道；
- Flash/掉电后 Route/Flow 延续——当前二者有意为易失状态；
- ESP32-S3 的实际 RAM、任务栈、Flash、吞吐、时延、功耗和 24 小时长稳；
- Reliable、Transfer、Service/Operation 的端到端语义。

因此 RUST-08 只获得实施顺序放行。进入 RUST-08 后必须继续保持：每个分项完成即定向自审，
Receipt/重试/分片/Operation 不能绕过 Flow/Security/Persistence 的 Owner 边界，最终再做正反两轮
全体自审与独立外审。
