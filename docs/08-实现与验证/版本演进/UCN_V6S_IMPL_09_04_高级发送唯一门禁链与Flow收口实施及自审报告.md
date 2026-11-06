# UCN V6 简化版 IMPL-09-04 高级发送唯一门禁链与 Flow 收口实施及自审报告

## 1. 本阶段结论

`IMPL-09-04` 已完成认证事实之后的私有高级发送 use-time 门禁。它没有把六个 Owner 合并成一个
大状态机，也没有让业务 Owner 互相直调，而是在 Composition 中按冻结顺序逐个读取当前事实：

```text
typed application requirement
  -> Composition lifecycle gate
  -> Identity current binding
  -> Admission current membership
  -> Security current session + exact ACL
  -> Capability current peer contract
  -> Route current path/link generation
  -> Flow current policy + sequence reservation
  -> immutable advanced-use plan
```

只有全部检查通过，Flow 才保留一个 Sequence。调用方完成后必须使用 Plan 中精确的 Flow Handle、
Sequence 和 Reservation Generation 执行 commit 或 abort。任一前序失败均不保留 Sequence，也不
写入输出 Plan。

本阶段不包含 raw Frame 解码、Security `open/protect`、认证后 RX 分派或 Driver 提交。它冻结的是
这些生产入口未来必须复用的唯一授权链，完整 raw Wire→Security→业务分派仍属于 `IMPL-09-07`。

## 2. 为什么必须在每次使用时重读

Identity、Admission、Session、Capability、Route 和 Flow 的有效期并不相同。初始化时或上一次调用
得到的 View 只能说明过去，不能授权当前发送。若缓存一个“曾经通过”的组合，以下任一变化都可能
被绕过：

- 身份 Binding 被 Fence 或换代；
- 成员资格被撤销、到期或换 Admission Generation；
- Security Session 被撤销，ACL 的 Service/Opcode 改变；
- Peer Capability 换代或不再支持所需 Feature/MTU；
- Route 换 Path、Route Generation 或下一跳 Link Generation；
- Flow 被 Fence、到期或策略改变。

因此 Plan 只能由本次 preflight 产生，不能作为下一次 preflight 的输入，也不能替代任何 Owner 的
当前状态。代码没有在 Composition Runtime 内缓存“已授权”位。

## 3. 唯一门禁顺序

### 3.1 Identity

首先按精确 Identity Handle 读取当前 Binding View，并核对：

- Runtime、Owner Instance、Slot、Slot Generation；
- Realm、Device ID、Address、Binding Generation；
- Phase 必须为 Active，且当前时间位于半开有效期内。

Identity 失败时不会访问 Flow，也不会产生可提交 Plan。

### 3.2 Admission

随后读取当前 Admission Handle，核对同一 Realm、Device、Address、Binding Generation，以及
Admission Generation、Session Generation 和当前 Active 状态。这样“身份仍存在但已经退网”的
节点不能只靠旧 Identity 继续发送。

### 3.3 Security 与 ACL

Security preflight 使用与本次请求相同的可信 `now_us`，并要求：

- Session Handle 与当前 Session Generation 精确匹配；
- Peer Principal 与 Identity/Admission 绑定一致；
- ACL 的 Source、Destination、Service ID、Protocol Opcode 与本次请求逐项一致；
- 请求的 Security Mode、Suite 和 Key 仍被当前 Session 接受；
- Session 未到期、未 Fence。

本轮交叉自审发现 Flow Request 原来只绑定 Opcode，没有绑定 Service ID。这样两个 Service 共用同一
Opcode 时可能把一个 Service 的 ACL 扩大到另一个 Service。现已把 `service_id` 纳入 Flow Request
和 Flow Requirement 的精确比较。

### 3.4 Capability

Capability 只接受当前 Peer Capability Ref，并要求其 Realm、Peer、Binding/Session Generation
与前序事实一致。所需 Feature 位必须全部满足，当前可用 Payload Budget 必须不小于请求下限。

### 3.5 Route

Route 解析绑定完整 Route Domain 和当前 Link Fact。除 Route/Path Generation 外，还必须核对本次
传入的 Link Generation。Route 的下一跳可以不是 E2E Security Peer，因此它与 Security Session
的 Link Generation 分开表达，不能错误地强制二者相等。

### 3.6 Flow

最后一步才调用 Flow use preflight。Flow 再次核对：

- Flow Handle 与 Active Record；
- Source/Destination、Service ID、Opcode；
- Traffic Class、Delivery Guarantee、Interaction Role；
- Route/Path、Security/Capability 依赖；
- 当前时间和 Flow 生命周期；
- 当前是否已有未收口的 Sequence Reservation。

前五层任何一层失败都不会触碰 Sequence。

## 4. Plan、期限与失败原子性

成功 Plan 记录本次授权所依赖的代际和最终 Flow Reservation，不保存 Owner 指针，也不授予未来
调用权限。它只能由将来的 raw-frame 提交路径在真实使用点即时取得，并收口同一次 Reservation；
不得先取得 Plan、排队任意时间后把它当作 Authority。`effective_deadline_us` 是 Identity、Security、
Capability、Route 和 Flow 半开 Deadline 的最小值。

所有带输出的 API 遵循局部候选再发布：

1. 入口只验证参数、Runtime 和生命周期；
2. 中间结果写入 caller-owned Workspace；
3. Flow Sequence 保留成功后才组装候选 Plan；
4. Composition gate 正常退出后才一次写入 `plan_out`；
5. 任一失败保持输出逐字节不变。

Workspace 由调用方静态提供，避免在任务栈上复制多个 Owner View 和 Route Facts。

## 5. Flow Reservation ABA 修复

首次实现只用 `{flow handle, sequence}` 收口。对以下顺序进行对抗检查时发现 ABA：

```text
preflight -> Plan A(sequence=1) -> abort
preflight -> Plan B(sequence=1)
late finish(Plan A) -> 可能错误收口 Plan B
```

因为 abort 不消耗 Sequence，第二次可以合法地再次保留同一 Sequence。仅比较 Sequence 无法区分两次
Reservation。

修复后每个 Flow Active Record 维护 no-wrap `reservation_generation`：

- 每次成功保留前 checked increment；
- Plan 同时携带 Sequence 和 Reservation Generation；
- commit/abort 必须两者都精确匹配；
- Generation 耗尽时失败关闭，不回绕；
- 旧 Plan 不能提交或取消新 Reservation。

该字段只属于本地事务身份，不进入 Wire，也不会增加报文开销。

## 6. 生命周期与并发边界

高级 preflight/finish 只在 Composition `RUNNING` 且所需 Owner 全部 `ACTIVE` 时开放。两者使用
Composition lifecycle gate，因此：

- init/start/stop/destroy 与高级门禁不能交错；
- callback 内不能发起新高级发送事务；
- 同一 Runtime 的 preflight/finish 不会并发改写 Flow Reservation；
- 错 Runtime、错 Owner 或已停止 Runtime 失败关闭。

当前接口仍是内部 C API，不扩大公共安装 ABI。

## 7. 分节自审

### 7.1 架构自审

- Owner 仍各自拥有状态，Composition 只做顺序编排；
- 没有业务 Owner 直接持有或调用另一个 Owner；
- 没有新增第二套 Identity、Session、Route、Flow 或 Sequence 状态；
- Static C1 Fast Path 不依赖本高级链，最小配置仍保持原开销；
- Production raw Wire 接线没有被本阶段提前放行。

### 7.2 安全与顺序自审

- Identity/Admission 先于 Security，未入网身份不能靠 Session 单独通过；
- Security/ACL 先于 Capability/Route/Flow；
- Capability 与当前 Security Peer/Session 精确绑定；
- Route 在 Sequence Reservation 前确认；
- Service ID 和 Opcode 都进入 ACL/Flow 精确匹配；
- 多层同时失效时返回最早门禁错误，后层无副作用。

### 7.3 状态与 ABA 自审

- Flow Reservation 是唯一可变提交点，且位于最后；
- finish 精确绑定 Reservation Generation；
- abort 后旧 Plan 重放不能命中新 Reservation；
- commit 后 Sequence checked increment，耗尽不回绕；
- Deadline 使用半开边界，到点即拒绝。

### 7.4 资源自审

初版 preflight 因在栈上同时保存 Route Facts 与候选 Plan，GCC Stack Usage 报告 352 B，超过
Runtime/Persistence 单函数 256 B 门限。整改后这两项进入 caller-owned Workspace：

| 函数 | Full GCC 单函数栈 |
| --- | ---: |
| `ucn_i_composition_advanced_use_preflight` | 208 B |
| `ucn_i_composition_advanced_use_finish` | 96 B |

实现没有动态内存、递归等待、无界表或隐藏轮询。

## 8. 对抗回归

新增 Composition 集成测试使用真实 Composition Storage 和六个真实 Owner 对象，覆盖：

- 完整成功、abort、commit；
- abort 后旧 Plan Reservation ABA 重放；
- 相同 Opcode、不同 Service ID 的 ACL/Flow 错绑；
- Identity Fenced；
- Admission Aborted；
- Security Fenced；
- Capability 缺失；
- Route 缺失；
- Route Link Generation 不一致；
- Flow Fenced；
- 仅 Flow 半开 Deadline 到点；
- Security 与 Capability 同时失效时必须先停在 Security；
- callback/lifecycle gate 活动期间拒绝；
- 参数失败和所有逐层失败均保持 Plan 哨兵逐字节不变；
- 所有失败均不保留 Flow Sequence。

Flow 单元测试同步覆盖 Service ID 和 Reservation Generation 的独立合同。

## 9. 验证证据

| 门禁 | 结果 |
| --- | --- |
| Windows GCC Full Debug 全量 | `106/106` |
| Windows GCC Lite Debug 全量 | `106/106` |
| Windows GCC Nano Debug 全量 | `106/106` |
| Windows GCC Nano Adapter OFF | `66/66` |
| Windows GCC Nano Persistence OFF | `95/95` |
| MSVC 19.29 Release `/W4 /WX` 全量 | `88/88` |
| WSL ASan/UBSan 定向 | `6/6` |
| WSL `-fanalyzer -Werror` 构建与定向 | `6/6` |
| Runtime/Routing Stack Usage Gate | PASS |
| Archive/Feature-OFF Gate | PASS |
| `git diff --check` | PASS，仅既有 CRLF 提示 |

## 10. 状态与下一步

```text
IMPL-09-04                             = DONE / SELF-REVIEW PASS
Private advanced outbound use gate     = IMPLEMENTED
Use-time current-state reread           = IMPLEMENTED
Flow reservation ABA protection         = IMPLEMENTED
Public Composition API                  = NOT EXPOSED
Raw Wire decode / Security open-protect = NOT YET WIRED
Production RX/TX dispatch               = NOT YET WIRED
MCU / physical bearer                   = NOT PROVEN
External review                         = HOLD
```

下一项是 `IMPL-09-05`：接入 Reliable/Transfer、Service/Operation 与 Q0～Q3。该阶段必须复用
本轮 Plan 和 Flow Reservation，不能建立另一条绕过 Identity/Security/Capability/Route 的发送链；
真正的 raw Frame 接收、解密认证和类型分派仍留到 `IMPL-09-07` 统一闭环。
