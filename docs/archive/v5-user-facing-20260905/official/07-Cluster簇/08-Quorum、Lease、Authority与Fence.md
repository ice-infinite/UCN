# Quorum、Lease、Authority 与 Fence

> 文档级别：`NORMATIVE`
> 实现状态：`PARTIAL（Current 与默认关闭实验层级见正文）`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：Cluster 公共头、生产源码、独立 Archive、CMake 与测试
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：部分 ESP32-S3 实测；v4/掉电/完整多 Bearer 未发布验收

## Quorum

Stable Config 计算单集合多数派；Joint Config 分别计算 `C_old` 和 `C_new` 多数派，二者都满足才视为有 quorum。Provisional、过期 lease 和不属于冻结 voter set 的节点不得计票。

## Lease

每个 voter 的有效性由时间戳和有界 duration 约束。所有会产生权威副作用的入口都必须执行 current-time preflight，不能依赖上一次 `step()` 留下的缓存。

## Authority preflight

preflight 覆盖：

- Cluster TX；
- RX 中 Head 才能执行的成员/配置副作用；
- Federation locator/handover 发布；
- 配置安装和切换；
- Owner budget 与 invariant 检查。

租约过期、quorum 丢失或预算耗尽时，先进入 grace/fence 或直接撤权，再处理本次请求。

## Config 切换

安装候选 Stable/Joint Config 时先清除旧 `authority_active`，再基于候选集合重新计算。候选集合没有 quorum 时必须保持无 Authority，禁止沿用旧配置的授权缓存。

## Fence 与恢复

Fence 阻止旧 Epoch、旧 Config 或旧 Head 继续发权威消息。解除 Fence 必须来自明确的 durable transition 和 runtime continuation，不能通过普通 reset、角色赋值或重入恢复。

## 多数派计算

Stable N 个 Voter 的 quorum 为 `floor(N/2)+1`。票/Lease 只有满足 frozen VoterSet 身份、当前 Epoch/Config、未过期且状态允许才计入。重复 Voter、Provisional、旧 Session 或错误 Config 不得计数。

Joint 不是把两个集合 union 后算一次；分别计算 old/new，并要求 `old_met && new_met`。

## Lease 的建立和刷新

Lease Deadline 由接收合法、认证且匹配当前 Config 的 Voter 事件建立/刷新。任意业务数据、未认证 Heartbeat 或旧 Epoch ACK 不能续权。duration 必须 ≤INT32_MAX，比较使用统一回绕安全时间。

## Preflight 为什么要覆盖 RX

假设 t=90 Voter Lease 到期，常规 step 尚未运行，t=91 先收到 Join Request。若 RX Handler只读上次缓存的 `authority_active=true`，旧 Head 仍会修改成员/发送接受。正确做法是在任何 Head 副作用前用 t=91 重新计算，先撤权再拒绝/转入 grace。

同理 Federation locator/handover 发布和公开管理入口也要 preflight，不能只有周期 step 更新。

## Owner Budget

Authority 还受每步/控制预算限制，避免一个合法 Head 在高负载下一次产生无界控制帧。预算耗尽表示本周期不再产生副作用，不应被误报为 quorum 永久丢失；下一 step 重新预算但仍需 preflight。

## Grace 与 Fence

短暂、明确允许的 Lease/SUSPECT 可进入有界 grace，期间通常不应继续产生未经许可的 Authority。结构缺失、REMOVED、Persistence fault、invariant violation 等硬条件直接 Fence，不能用 grace 拖延。

## Config 切换原子顺序

```text
收到候选Config
→ authority=false
→ 验证/安装候选stable或joint
→ 以当前now重新计算两侧lease/quorum
→ 满足才authority=true，否则保持fenced/grace
```

旧 Config 的 vote/lease 不能在新集合里自动计数。

## 公开效果分类

| 行为 | 需要 Authority preflight |
| --- | --- |
| 发送 Head Advertise/Config | 是 |
| 接受 Join/修改成员 | 是 |
| 发布 Federation Locator/Directory | 是 |
| 普通业务 Endpoint 转发 | 通常不是 Cluster Authority，由 Core/ACL决定 |
| 只读诊断 | 不授 Authority，但需管理授权且不得续 Lease |

## 验证清单

- [ ] Stable/Joint quorum 边界；
- [ ] t=deadline 前后 RX-first/TX-first/Federation-first；
- [ ] install Config 立即清旧 Authority；
- [ ] Provisional/expired/wrong Config vote 不计；
- [ ] grace 只用于明确软状态，硬缺失立即 Fence；
- [ ] budget 耗尽不被误作 persistence/quorum fault；
- [ ] Fence 无普通 reset 旁路。
