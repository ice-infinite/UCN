# Role、Phase、Epoch 与 Authority 模型

> 文档级别：`NORMATIVE`
> 实现状态：`PARTIAL（Current 与默认关闭实验层级见正文）`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：Cluster 公共头、生产源码、独立 Archive、CMake 与测试
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：部分 ESP32-S3 实测；v4/掉电/完整多 Bearer 未发布验收

## Role 与 Phase

公开 Role 是粗粒度身份：`DISABLED`、`DETACHED`、`JOIN_PENDING`、`MEMBER`、`CANDIDATE`、`HEAD`、`BACKUP`、`STEPPING_DOWN`、`RECOVERY_HEAD`、`TERM_CONFLICT`。

内部 Phase 把过程细化为 22 个状态，覆盖观察、选举、入簇、成员运行、Backup 分配/同步/接管、Head 稳态、重配置、quorum grace、fenced、rekey、恢复和 Term 冲突等待。Role 适合应用诊断；安全判断必须使用 Phase、Epoch、配置和 Authority 的组合，不能只看 `role == HEAD`。

## Epoch

Epoch 至少绑定 Cluster ID、Term 和 Head ID。比较规则先判断 Cluster 身份域：

- 同 Cluster 才可比较 Term 的新旧；
- 不同 Cluster 是 `FOREIGN`，禁止用 Term 数值决定谁更新；
- Cluster ID、Head ID、Term 的零值、广播值和越阈值值均应 fail-closed。

## Authority

Authority 表示当前节点是否有资格产生带权威副作用的控制消息。它不是角色别名，至少受以下条件共同约束：

- 当前 Phase 允许；
- Epoch 与已提交配置一致；
- Stable 或 Joint quorum 成立；
- voter lease 未过期；
- Owner budget 未超限；
- 没有 Persistence、Handover、Rekey 或 invariant Fence。

Authority preflight 必须使用当前时间刷新租约状态。配置切换、quorum 丢失或不可逆撤权时先清 Authority，再处理后续动作。

## Fence

Fence 是 fail-closed 边界。进入 fenced/撤权状态后，旧 Authority 不得通过 reset、重入或缓存状态恢复；真正的掉电安全还需要持久化记录和重启 reload 证明，RAM Fence 本身不等于持久化保证。

## Role 为什么不够做安全判断

两个节点都显示 `HEAD`，一个可能处于稳定 authority-active，另一个刚进入重配置且 quorum 已丢失。UI 可显示 Role，但发送 `ADVERTISE/CONFIG/HANDOVER` 前必须检查 Phase、Epoch、Config、Lease、Persistence 和 Fence。

Role 是压缩视图；Phase 才描述过程位置。例如 Backup 同一 Role 下还可能处于分配、同步、Ready、接管准备等不同 Phase。

## Epoch Domain 比较

概念结果不是简单 `-1/0/+1`：

| 关系 | 条件 | 允许结论 |
| --- | --- | --- |
| SAME | Cluster/Term/Head 全部相同 | 同一 Authority 身份 |
| LOWER/HIGHER | 同 Cluster，Term 合法可比较 | 判断陈旧/更新 |
| FOREIGN | Cluster ID 不同 | 进入 foreign/merge policy，禁止比较 Term |
| INVALID | 0/broadcast/越阈值/组合非法 | 拒绝且不写状态 |

`Cluster A/Term 100` 与 `Cluster B/Term 1` 是 FOREIGN，不能因为 100>1 就让 A 统治 B。跨簇优先级来自稳定 Authority、Config、策略和 Handover，而非数字巧合。

## Authority 是一个动态结论

Authority 会随当前时间、Voter Lease、Config 和 Owner budget变化。缓存 `authority_active=true` 不能永久使用；RX/TX/Federation 等副作用入口必须用 `now_ms` 做 preflight。

```text
role/phase允许
AND epoch/config匹配
AND stable或joint quorum
AND leases未过期
AND persistence无pending/fault
AND fence未置位
AND owner budget允许
= 当前可产生Authority副作用
```

任一条件消失时先撤权，再决定 grace/recovery；不能先发消息后更新 Authority。

## Fence 的类型和生命周期

Fence 可来自 invariant failure、persistence I/O、quorum loss、stepdown、rekey/retired identity 或 handover。不同来源的恢复条件不同，但共同规则是“普通 reset/角色赋值不能清除不可逆 Fence”。

RAM 模型中的 Fence 防同一运行期 ABA；掉电后要靠 Record/Tombstone/boot incarnation 恢复。只实现前者不能写成掉电安全。

## 典型状态示例

### Head 丢失一个 Voter Lease

preflight 重新计算 quorum；若仍有多数派可保持，若不满足则进入 grace/fenced 并停止权威 TX。普通数据 Route 可以继续，Cluster Config/Locator 等副作用停止。

### 安装新 Joint Config

先清旧 `authority_active`，用 C_old/C_new 分别计算 quorum。两者都满足才重新授权；不能沿用旧 Stable Config 的 true 缓存。

### 收到 foreign 更高数字 Term

Epoch compare 返回 FOREIGN，按观察/merge policy，不更新本簇 max term，也不直接 step down。

## 诊断字段

至少同时显示 Role、Phase、Active/Max Epoch、Stable/Joint Config、quorum、lease age、Authority、Fence reason、persistence pending/fault。只打印 `role=HEAD` 会误导运维。

## 验证清单

- [ ] 22 Phase 与 Role 映射有合同测试；
- [ ] foreign 高/低 Term 对称，不参与数值比较；
- [ ] 每个权威 TX/RX/Federation 入口做 current-time preflight；
- [ ] Config 安装先撤权再重算；
- [ ] Fence 后 reset/reentry 不恢复 Authority；
- [ ] 重启从 durable record 恢复而非仅 RAM 字段。
