# AODV-Lite 路由发现与 RREQ/RREP/RERR

> 文档级别：`NORMATIVE`
> 实现状态：Lite/Full `CURRENT`；Nano 不含动态 Mesh
> 事实源：Node route control、AODV/route tests
> 最近核对：当前工作区，2026-09-04

## 发现流程

```text
A 没有到 C 的 Route
  ↓ 创建有界 discovery
RREQ 向已准入邻居传播
  ↓ 每跳记录回到 A 的反向下一跳和累计 Cost
C 或有合法答案的节点返回 RREP
  ↓ 沿反向 Route 回到 A
A 安装到 C 的下一跳 Route
  ↓
后续数据直接查缓存发送，不每帧重新寻路
```

RREQ 使用 transaction/origin/sequence 与独立 Best-Cost 状态抑制重复，同时允许同一事务中成本更低的路径继续传播。动态 Route 与 Candidate 不只按 Destination 建键，而是按 `(traffic_origin,destination)` 分域；不同 Origin 的 Request ID/Route Epoch 永不直接比较。

## 多 Origin Route Instance

设 A、B 同时经中继 R 访问目标 C。R 必须同时持有 `(A,C)` 与 `(B,C)` 两个正向 Route Instance。A 的新发现、刷新、旧 Epoch Grace 或 RERR 只能改变 `(A,C)`，不能覆盖或撤销 `(B,C)`。

RREQ 与 RREP 会建立方向相反但所有权明确的实例：

```text
A --RREQ--> R --RREQ--> C
RREQ沿途反向实例：route_origin=C, destination=A
RREP沿途正向实例：route_origin=A, destination=C
```

因此 C 返回的 RREP 可以使用 `(C,A)` 回到 A，A 发出的业务可以使用 `(A,C)` 前往 C。Route Epoch 只在同一个二元键内匹配 Current/Previous；相同数值出现在另一个 Origin 域既不更新也不证明新鲜。

## 下一跳为什么正确

B 不需要知道 A 的业务内容。B 的 Route 表只需包含“目标 C 应从哪个本地 Link 发给哪个下一跳”。若 D 不是 C 的下一跳候选，B 不会因为 D 存在就随意转给 D。

## RERR

Link Down、下一跳消失、能力/MTU 不匹配或已安装 Path 无效时，节点撤销相关状态并向依赖方向发送 RERR。上游据此停止使用旧 Route，并按 Traffic/Policy 决定是否重新发现。

## 控制范围

RREQ 是受 Hop、缓存、发现槽和控制预算限制的传播，不是无限全网广播。RREP/RERR 沿已有反向/依赖路径传播。

## Q0 边界

关键 Q0 不因发送时缺 Route 自动启动无界发现。策略可要求预建 Route/Path，或明确失败，让产品决定是否安全重试。Q1 更适合在允许条件下触发自动发现。

## 发现前的发送决策

发送 API 先区分直连、已有 Route、正在发现和完全未知四种状态：

| 当前状态 | 处理 |
| --- | --- |
| 目标是本机 | 进入本地 Endpoint/Service，不寻路 |
| 有合法直连 Bearer | 建立/使用一跳 Route |
| 有未过期 Route | 直接按下一跳发送 |
| Discovery 已在进行 | 不重复创建无界 RREQ，按 API 合同返回 pending/未找到 |
| 完全未知 | 只有允许自动发现的调用才创建固定槽 Discovery |

发现槽满、控制预算不足或 Hop Scope 无效时必须明确失败。协议不会因为业务“很重要”就动态分配内存或无限扩散。

## RREQ 每一跳做什么

节点 B 收到 A 发起的 RREQ 后，概念上按以下顺序处理：

1. 严格解码并验证 Network、Source、Origin、Transaction/Sequence、Hop 和 Cost；
2. 确认入站 Neighbor/Bearer 已准入；
3. 在独立 RREQ Cache 查找该发现身份；
4. 首次见到，或同事务 Cost 足够更低时，记录“返回 Origin A 的反向下一跳”；
5. 若本机是目标，或拥有协议允许回答的足够新 Route，则生成 RREP；
6. 否则在 Hop/预算允许时累加基础 Route Cost 并转发 RREQ；
7. 重复且不更优的副本被抑制。

瞬时本地 queue pressure 不写入线上累计 Route Cost；它只参与本地出口选择，避免不同节点用不同时间快照污染同一次发现。

## RREP 如何建立正向 Route

RREP 从目标或合法应答节点沿 RREQ 留下的反向状态返回：

```text
A --RREQ--> B --RREQ--> C
A <--RREP-- B <--RREP-- C
```

C 的 RREP 到达 B 时，B 知道“对于源 A 的流量，去 C 的下一跳是收到 RREP 的那条 Bearer”；B 再把 RREP 发给 A。A 最终只需保存去 C 应交给 B。之后 A 的普通数据交给 B，B 按 `(A,C)` 查 Route 再交给 C。

这个过程回答了“B 怎么知道给 C 而不是 D”：不是从 Payload 猜，而是查目标 C 对应的 Route/下一跳。

## 多条 RREQ 和确定性选择

同一发现可能从不同 Bearer 到达目标。缓存允许更优 Cost 的后到副本继续传播，但不会无限保留所有历史。Route/Candidate 再按 Cost、Hop、序列新鲜度和确定性 tie-break 收敛。

当两个候选数值完全相同，必须有稳定 tie-break，避免不同 step 中随机来回切换。具体内部字段以源码为准，应用不应依赖表内排列顺序。

Candidate 路径的 Activate/ACK 是独立的有界事务：发起端只有在完成 Probe 后才能发送，
ACK 必须精确回显已发送的 Candidate ID 与 Route Epoch。ACK 丢失时保持该事务身份并使用
新的外层 Sequence 重发；超时或重试耗尽只放弃 Candidate，不替换仍有效的旧 Route。

## RERR 的依赖方向

RERR 不是全网广播“某节点坏了”。中继只应通知实际依赖该失效下一跳/Path 的上游：

1. Driver/Heartbeat 判定 B→C 失效；
2. B 撤销以 C 为下一跳的相关 Route/Path；
3. B 向使用 B 作为该目标下一跳的上游发送 RERR；RERR 外层 Source 是发现故障的 B，但反向选路域由 Payload 中的不可达目标 C 决定；
4. A 收到后只撤销 `(A,C)`，其他 Origin 到 C 的独立实例不受影响；
5. 后续业务按 Policy 选择备 Path、Candidate 或新 Discovery。

能力/MTU 失配同样必须走确定性失效，而不能保留一个“有路但永远发不出去”的表项。

## 路由发现与数据传输的关系

寻路不是每帧执行。第一次未知目标、缓存到期、硬失败或显式刷新才会触发发现。Route 有效期间，普通发送只是有界表查找和 Link 提交。

在发现过程中，Core 不承诺无界缓存等待业务 Payload。实时产品应在启动/拓扑稳定阶段预发现关键 Route，或使用 Pinned Path，避免第一条控制命令承担发现延迟。

## 失败与恢复示例

路径 A→B→C 正在传输时 B→C 断开：

- 已交给 B→C Driver 的 Frame 可能成功也可能丢失；
- B 撤销 Route 并发 RERR；
- A 之后尚未发送的 Frame 不再使用旧 Route；
- Q1 可触发新发现；
- 可靠大消息由 Transfer ACK/重试补齐未确认片段；
- Q0 命令是否重试由产品 Command ID、Deadline 和幂等策略决定。

## 验证清单

- [ ] 首次未知目标创建的 Discovery 数量有界；
- [ ] 同事务更低 Cost RREQ 可继续传播，不更优重复被抑制；
- [ ] Hop=0、错误 Network、未准入 Source 不污染反向 Route；
- [ ] RREP 确实沿反向状态返回并逐跳建立正向 Route；
- [ ] Link/MTU/capability 失效产生撤销和依赖方向 RERR；
- [ ] Route 有效时发送不重复 RREQ；
- [ ] Q0 缺路不会自动变成无界等待或迟到命令。
- [ ] 两个 Origin 并发访问同一目标时具有独立 Route/Candidate/Epoch；一方 RERR、刷新或 Grace 不改变另一方。
- [ ] Route Instance 表满返回 `NO_SPACE` 并保留全部既有有效实例，不按 Destination 偷换其他 Origin 的槽。
- [ ] Activate/ACK 绑定相同 Candidate ID/Epoch，ACK 丢失有界重试且耗尽不破坏旧 Route。
