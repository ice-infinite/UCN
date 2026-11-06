# UCN V6 简化版 IMPL-09-05 Reliable/Transfer、Service/Operation 与 QoS 接线及自审报告

## 1. 本阶段结论

`IMPL-09-05` 已完成私有 Composition 层的高级业务语义装配。它不新建第二条发送授权链，而是在
`IMPL-09-04` 已冻结的当前状态门禁之后，原子建立本次发送真正需要的状态：

```text
Identity -> Admission -> Security/ACL -> Capability -> Route -> Flow
  -> optional volatile Service Request
  -> exactly one Reliable or Transfer record
  -> one Q1/Q2/Q3 scheduler reference
  -> immutable Delivery Plan
```

任一步失败都按严格反向顺序撤销已经建立的状态，Flow Sequence 也回到未占用状态。只有
Composition gate 正常退出后才把候选 Plan 一次性写给调用方。

本阶段还没有接入 raw Wire 解码、Security `open/protect`、Adapter submit、生产 RX dispatch 或
Q0 typed control。当前接口仍是内部 typed API，是 `IMPL-09-07` 生产分派必须复用的唯一装配入口，
不能据此宣布公共 Runtime 或实机协议已经闭环。

## 2. Delivery 与 Interaction 保持正交

协议不会把“怎么送达”和“这条消息扮演什么角色”混成一个枚举。本轮继续使用两条独立轴：

| 维度 | 当前取值 | 本轮语义 |
| --- | --- | --- |
| Delivery | Best-Effort / Reliable / Transfer | 决定是否建立 Transport 状态以及正文如何保持 |
| Interaction | One-Way / Request / Result / Error | 决定是否建立 Request 关联以及 Operation ID 规则 |

因此：

- One-Way 可以使用 Reliable 或 Transfer，但不会占用 Service Request 槽；
- Request 可以使用 Best-Effort、Reliable 或 Transfer；
- Result/Error 携带非零 Operation ID，但不在发送端重新创建 Request；
- Durable Operation 不伪装成普通 Request，而使用独立的持久化转换入口；
- Latest 不进入本高级入口，继续复用 Foundation 已有的唯一 Latest 队列，避免双队列和双覆盖语义。

Best-Effort Request 在本阶段只建立 Service Request 与 QoS 引用，不复制正文。真正的 Runtime Attempt
与 raw Frame Body 由 `IMPL-09-07` 统一建立和提交。

## 3. Reliable 接线

Reliable 记录使用本次 Flow Plan 派生的 Origin Sequence，并精确绑定：

- Realm、Source/Destination Address、Binding Generation 与 Principal；
- Service ID 与 Delivery；
- Security Session Generation、Origin TX Key Generation 和 Origin Security；
- AAD Digest、Payload Digest、正文长度和正文内容；
- Transport 自己的半开 Deadline。

正文长度必须与 Flow 请求声明的 `payload_bytes` 完全相等，不能用较小声明绕过 Capability/MTU
预算。Reliable 创建成功后立即读取当前 View，QoS Deadline 再与 Transport Deadline 取最小值。

## 4. Transfer 接线

调用方只能提供尚未绑定父事务的 Transfer Template。Composition 会覆盖并冻结以下字段：

- `parent_kind = FLOW`；
- `parent_id = flow_context_id`；
- `parent_generation = current_flow_generation`；
- `transfer_id = reserved_origin_sequence`；
- Service ID、Delivery、Interaction 与 Operation ID；
- Message Digest 与当前 Flow Fingerprint；
- Security Session/Key、Policy Generation 和 Origin Security。

这样调用方不能拿路径 A 的 Flow 授权去创建路径 B 的 Transfer，也不能把旧 Flow Generation、旧
Sequence 或另一个 Service 的正文绑到本次事务。Transfer Setup、Parent Facts 和正文会在同一
Composition gate 中完成校验和预留。

## 5. Service Request 与 Operation

### 5.1 易失 Request

仅当 Interaction 为 Request 时才建立 Service Request。Key 由当前 Route、Security、Service 与
Opcode 派生，Deadline 取 Request Deadline 与 IMPL-09-04 Effective Deadline 的较早者。

One-Way 不创建 Request；Result/Error 不在响应侧虚构新的 Request。这样 Interaction Role 不会因
选择 Reliable 或 Transfer 而改变。

### 5.2 Durable Operation

Durable Operation 使用独立入口：

1. 要求 Composition Plan 同时拥有 Service、Persistence 和 Durable Operation 能力；
2. 要求 Runtime 正在 RUNNING，两个 Owner 都是 ACTIVE；
3. 创建 Service Operation 的 PREPARED 状态；
4. 返回该状态的不可变 Persistence Requirement；
5. 不创建 Flow、Transport 或 QoS 状态。

只有上层在后续阶段完成 Requirement 持久化、reload 证明和状态推进后，Operation 才能进入执行/结果
路径。当前入口不接受 `authenticated_replay_candidate`，也不把 PREPARED 当作可执行授权。

本阶段没有实现 raw RX 的身份认证和 Operation 执行器分派；调用该私有入口的 canonical key 必须由
未来 `IMPL-09-07` 的已认证 dispatch 产生。此边界不得在外审或用户文档中描述成生产授权已完成。

## 6. QoS 接线

本轮业务入口只接受 Q1、Q2、Q3：

- Q1、Q2、Q3 共用 Service Owner 中现有的有界 QoS Scheduler；
- QoS 只保存一个可发送对象 Handle，不复制 Transport 或 Service 状态；
- Source quota 使用 Origin Address；
- Flow quota 使用当前 Flow Generation；
- Deadline 取 Flow/current-use、Request 与 Transport 的最小值；
- 队列满、配额满或任何冲突都触发整个 Delivery 事务回滚。

Q0 业务提交失败关闭。Q0 需要明确的 typed control authority，不能让普通业务请求仅靠
`traffic_class=0` 获得控制级资源；它将在 `IMPL-09-07` 的 Control Opcode/AAD/dispatch 中接线。
底层 Q0 调度算法和授权检查已有独立单元测试，但当前 Composition 入口不会提前放行它。

## 7. 失败原子性与取消

### 7.1 Prepare 失败

Prepare 的回滚顺序是：

```text
QoS remove
  -> Reliable cancel+retire 或 Transfer abort+retire
  -> Service Request cancel+retire
  -> Flow Sequence abort
```

候选状态全部放在 caller-owned Workspace 中；`plan_out` 只在成功离开 Composition gate 后写回。
参数错误、逐层门禁失败、Transport/Service/QoS 容量失败和 gate 退出失败都保持输出哨兵不变。

### 7.2 显式取消

取消不是盲目删除。它先逐项验证：

- QoS 仍未 selected、未 submitted，且引用精确的 Transport/Service Handle；
- Reliable 仍为 READY，或 Transfer 仍为 SETUP_PENDING；
- Request 仍为 WAIT_SEND；
- Interaction 为 Request 时必须存在 Service Reservation，其他 Interaction 必须不存在；
- Plan 的 Runtime、Schema、Transport Mode 和所有 Handle 关系一致。

只有所有状态仍处于可取消的预提交阶段才执行反向回滚。过期 Plan、错误 Handle、已经选择或提交的
状态都失败关闭。

## 8. Persistence-OFF 与 Feature 边界

Persistence 不是普通通信的隐藏依赖：

- Reliable、Transfer、易失 Service Request 和 Q1～Q3 在 Persistence-OFF 组合仍可被 Plan 选中；
- 只有 Durable Operation 能力要求 Persistence；
- 在 Persistence-OFF 下请求 Durable Operation 会在 Composition Plan 预检阶段失败，输出不写回；
- Adapter-OFF 或全可选关闭时，对应高级 Composition 测试/符号按既有 Feature 规则裁剪；
- Static C1 Fast Path 不因本轮接线增加高级 Owner 或状态。

## 9. 分节自审

### 9.1 架构自审

- Composition 只做顺序编排，不拥有 Reliable、Transfer、Service、Operation 或 QoS 的业务状态；
- Owner 之间没有直接函数调用，所有状态都由各自 Owner 管理；
- 复用 IMPL-09-04 的唯一当前状态 preflight，没有建立旁路；
- Delivery 与 Interaction 没有重新合并成旧式混合模式；
- Durable 与非 durable 路径明确分离。

### 9.2 安全绑定自审

- Transport Key/Parent 由当前 Route、Flow、Security 派生；
- Service Key 同时绑定 Service ID 和 Opcode；
- Security 使用当前 Origin TX Key Generation，而不是硬编码或调用方自报；
- Body 长度与 Flow 声明完全相等；
- Transfer Template 不能预填 Parent/Transfer/Service/Operation 字段；
- Q0 普通业务和 Latest 混合路径均拒绝。

### 9.3 状态与回滚自审

- Flow Sequence 仍是最后的高级授权预留，并带 Reservation Generation；
- Service、Transport、QoS 任一步失败均撤销此前状态；
- cancel 只接受尚未选择/提交的精确 Plan；
- Durable Operation 不提前排队；
- replay-candidate 拒绝时 Operation 与 Requirement 输出逐字节不变。

### 9.4 资源与栈自审

首次实现把 Cancel View 和 Durable Requirement 候选放在任务栈上，普通 GCC 栈报告分别达到
304 B 和 480 B，超过 Runtime 256 B 单函数门限。现已改为 caller-owned Workspace：

| 函数 | Full GCC 单函数栈 |
| --- | ---: |
| `ucn_i_composition_delivery_prepare` | 192 B |
| `ucn_i_composition_delivery_cancel` | 128 B |
| `ucn_i_composition_durable_operation_begin` | 112 B |

没有动态内存、递归等待、无界重试或隐藏线程。插桩构建会放大 `.su` 并产生 dynamic 标记，因此普通
栈阈值只以未插桩 GCC 构建为证据，ASan/Analyzer 另行验证运行语义。

## 10. 对抗回归

新增/扩展的集成测试覆盖：

- Reliable Request + Q1 完整装配与精确取消；
- Transfer Request + Q3，Parent/Flow Generation/Sequence/Digest 精确绑定；
- Reliable One-Way + Q2 不占 Service Request；
- Best-Effort Request + Q2 只占 Service/QoS，不占 Transport；
- Q0 业务请求拒绝；
- Delivery/Transport 语义混配拒绝；
- 声明 Payload 长度与实际正文长度不一致拒绝；
- QoS 容量失败时 Flow、Service、Transport 全部回滚；
- caller-owned Plan 清除 Request Reservation 的篡改被拒绝，原四类状态完全保留；
- Durable Operation 仅产生 PREPARED Requirement，不产生 QoS/Flow；
- replay-candidate 对 Operation/Requirement 输出零写；
- Persistence-OFF 允许非 durable Transport/Service，拒绝 Durable Operation 能力；
- Flow 单元测试同步钉住 Flow Generation 的输出绑定。

## 11. 验证证据

| 门禁 | 结果 |
| --- | --- |
| Windows GCC Full Debug 全量 | `106/106` |
| Windows GCC Lite Debug 全量 | `106/106` |
| Windows GCC Nano Debug 全量 | `106/106` |
| Windows GCC Persistence-OFF 全量 | `95/95` |
| Windows GCC Adapter-OFF 全量 | `66/66` |
| Windows GCC 全可选关闭全量 | `70/70` |
| MSVC 19.29 Release `/W4 /WX` 全量 | `88/88` |
| WSL ASan/UBSan 适用矩阵 | `103/103` |
| WSL `-fanalyzer -Werror` 适用矩阵 | `116/116` |
| 普通 GCC Runtime/Kernel Stack Gate | PASS |

ASan 全量 CTest 中普通 `.su` 阈值、Call Stack 解析和未携带 sanitizer runtime 的独立安装消费者
并不适用于插桩 archive；Analyzer 构建中的 Call Stack 解析同样会把 analyzer 的 dynamic frame 当作
未知栈。报告没有把这些工具不兼容项计作通过，而是排除后运行全部适用测试：ASan `103/103`、
Analyzer `116/116`。普通 GCC 与 MSVC 构建继续负责栈基线、安装消费者和 ABI 门禁。

## 12. 状态与下一步

```text
IMPL-09-05                                  = DONE / SELF-REVIEW PASS
Reliable / Transfer typed composition      = IMPLEMENTED
Volatile Request + Q1/Q2/Q3 composition    = IMPLEMENTED
Durable PREPARED requirement boundary      = IMPLEMENTED
Q0 typed control production dispatch       = NOT YET WIRED
Raw Wire / Security / Adapter RX-TX         = NOT YET WIRED
Public Composition API                     = NOT EXPOSED
MCU / Flash / physical bearer              = NOT PROVEN
External review                            = HOLD
```

下一项是 `IMPL-09-06`：分别接入 Realtime、Group 与 Cluster，并持续证明三者互不链接、不会给
Static C1 或普通 Reliable/Transfer 增加隐藏状态。生产 raw Wire/AAD/dispatch 仍统一留在
`IMPL-09-07`，不得在 Optional Feature 接线时偷偷形成另一套入口。
