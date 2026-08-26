# Hop、TTL、时间与错误模型

> 文档级别：`NORMATIVE`
> 实现状态：`CURRENT`
> 事实源：`include/ucn/ucn_time.h`、Core Frame、Node/Route/Transfer 错误路径
> 最近核对：`a093862`，2026-08-26

## 1. Hop Limit 是安全边界，不是网络规模承诺

每个可转发 Frame 携带 Hop Limit。中继只有在剩余 Hop 合法时才可转发，并在下一跳 Frame 中递减；到 0 返回 `UCN_ERR_TTL`，不能继续传播。

它主要解决两件事：

1. 路由瞬态错误或环路不能让同一帧无限转发；
2. 发送方/产品可以限制一次业务允许占用的网络范围和时延上界。

编译默认 `UCN_MAX_HOPS=16`，合法配置范围 1～254。Wire Profile 进一步约束可表达的最大 Hop：W0=4、W1=16、W2=64、W3=254。实际允许值是产品配置、所选 Wire Profile、已安装 Path 能力和单次请求约束的共同最小值。

## 2. 为什么不能因为字段能表示 254 就真的跑 254 跳

每增加一跳，至少增加一次完整接收、校验、Owner 调度、重新排队和物理发送。共享无线还会重复占用同一信道。粗略端到端延迟为：

```text
T_total ≈ Σ(T_rx + T_owner + T_queue + T_tx)_每一跳
          + 路由发现/重试/冲突代价
```

254 只是 W3 字段表示能力，不是推荐拓扑。控制系统通常应按最坏时延和失效预算把业务限制在更小跳数；远距离大规模网络应通过 Cluster、网关、分层路由或更高速骨干解决，而不是无限拉长逐跳链。

## 3. Hop Limit 与 Route 跳数的关系

- Route 的 hop_count 表示当前候选路径预计要经过多少跳；
- Frame 的 Hop Limit 表示本次发送还允许多少次中继；
- 路由存在但 hop_count 超过请求/Profile 限制时，发送必须拒绝；
- 转发过程中 Link Down 或路由改变时，旧 Frame 不会自动获得额外 Hop；
- Path Trace 可以按需观察实际节点序列，但正常数据帧不携带完整路径表。

因此，节点 B 只需要根据目标和本地 Route/Path 决定下一跳 C 或 D，不需要在每一帧保存从 A 到终点的完整路线。

## 4. 32-bit 单调时间合同

UCN 的运行时间使用 32-bit 单调毫秒计数。它不要求 RTC 日期，也不要求所有节点的 uptime 相同。绝大多数 Deadline 只在创建和消费它的同一时间域内比较。

由于计数会按模 `2^32` 回绕，合法相对时长必须位于 `1..INT32_MAX` ms。公共 helper 规则为：

- `ucn_duration_is_valid()`：检查相对时长；
- `ucn_deadline_from_now()`：生成非零绝对 Deadline；
- `ucn_deadline_expired()`：用有符号差进行回绕安全比较；
- `ucn_deadline_due_within()`：判断是否在维护窗口内到期；
- `ucn_elapsed_at_least()`：计算周期经过时间。

不能用 `now_ms > deadline_ms` 代替这些 helper，因为回绕附近会得到相反结果。

## 5. Deadline、Interval、Timeout 和 Lease 的区别

| 概念 | 表示什么 | 示例 |
| --- | --- | --- |
| Interval | 两次周期动作的间隔 | HELLO 每 1000 ms |
| Timeout | 从某事件起最多等多久 | Transfer ACK 等待 100 ms |
| Deadline | 某项工作的绝对最晚有效时刻 | Q0 命令在 `now+20 ms` 后无效 |
| Lease | 权限/状态在没有续期时的有效期 | Neighbor/Authority 租约 |

Timeout 通常在开始时转换为 Deadline；之后只能比较同一个 Deadline，不能每次 step 都重新从当前时间计算，否则工作永远不会超时。Lease 续期必须来自合法事件，普通业务流量不能无条件替代 Heartbeat/权威证明。

## 6. 跨节点时间不能想当然

一个 Payload 中的 `issued_at_ms` 若要由另一节点验证，产品必须提供共享或可证明误差范围的时间域。两个 MCU 各自从开机 0 开始计数时，数值相同没有全局意义。

没有同步时钟时，可选方案包括：

- 持久单调 Command ID/generation；
- 会话内 nonce + 有界待处理表；
- 由接收方发挑战，发送方响应；
- 只使用接收方本地创建的租约 Deadline。

## 7. 错误分类与责任边界

| 类别 | 典型错误 | 谁需要处理 | 常见恢复 |
| --- | --- | --- | --- |
| 调用/配置 | `ARGUMENT`、`CONFIG` | 集成者 | 修正参数或配置，通常不应自动重试 |
| 容量 | `NO_SPACE`、`TOO_LARGE` | 调度/产品 | 消费队列、降速、分片或调整静态容量 |
| Wire | `MALFORMED`、`VERSION`、`CRC` | 接收边界 | 丢弃并统计，不修改状态 |
| 网络 | `NETWORK`、`TTL`、`LINK_DOWN`、`NOT_FOUND` | 路由/业务 | 刷新路由、故障切换或报告不可达 |
| 安全 | `SECURITY`、`REPLAY`、`ACCESS` | 安全/策略 | 拒绝，必要时告警；不能自动降级明文 |
| 状态 | `STATE`、`EXHAUSTED` | FSM/运维 | 保持 Fence、轮换身份或恢复持久状态 |

错误码表达“在哪个合同边界失败”，不是日志文本的替代。统计和诊断还应记录发生在哪条 Link、哪个 Endpoint、哪个状态机阶段。

## 8. 失败时的状态写回规则

公共解析和诊断 API 对非法输入通常遵循“返回错误且 output 不写回”。状态机更新应尽量先验证全部前置条件，再一次性提交，避免半更新。

典型禁止行为：

- CRC 失败仍更新 Neighbor；
- Security 失败仍推进 replay window；
- Link Down 被统计为 Persistence failure；
- Service 入队成功被报告成远端执行成功；
- Route 安装一半失败却保留局部 Path；
- Deadline 无效时把 0 当作无限等待继续运行。

每个模块的精确原子性仍以其 API 文档和测试为准，但调用者必须先检查返回值，不能在错误后读取未承诺的 output。

## 9. Fail-closed 与可恢复性如何同时实现

Fail-closed 意味着证据不足时不授予权限、不继续转发、不降级格式；它不等于设备永久死锁。正确设计应同时给出可恢复出口：

- Link 断开：撤销邻居/路由，允许重新发现；
- Queue 满：有界重试或上层降速；
- Route 失效：RERR + 新发现；
- Serial 到阈值：显式 rotate/rekey，而不是回绕；
- 持久化损坏：进入安全恢复/人工重置，而不是继续承诺；
- 未知版本：拒绝并通过兼容诊断说明原因。

## 10. 时延预算示例

某舵机命令要求 30 ms 内到达，路径最多 3 跳。产品可以分配：

```text
源任务与 Owner：  3 ms
3 跳排队与发送：21 ms（每跳最坏 7 ms）
目标 Inbox 唤醒： 3 ms
安全余量：       3 ms
```

发送时设置 30 ms Deadline，并限制路由最多 3 跳。若当前路由需要 5 跳，即便协议字段支持，也应在发送前拒绝或改走满足预算的 Bearer，而不是“先发出去再看”。

## 11. 验证清单

- [ ] Hop Limit 每次中继只递减一次，到 0 不再发送；
- [ ] 路由环路不会无限占用网络；
- [ ] W0～W3 的 Hop/地址上限均严格检查；
- [ ] `UINT32_MAX` 附近 Deadline/Interval 测试通过；
- [ ] 0 和大于 `INT32_MAX` 的 Duration 被拒绝；
- [ ] 超时只建立一次，不会在 step 中不断延后；
- [ ] 非法输入不污染 output、统计以外的运行状态或安全窗口；
- [ ] 网络、传输、持久化和业务错误没有互相误分类；
- [ ] 产品 Hop 限制来自实测时延预算，而不是字段最大值。
