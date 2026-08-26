# Cluster 簇

> 文档级别：`NORMATIVE`
> 实现状态：`PARTIAL（Current 与默认关闭实验层级见正文）`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：Cluster 公共头、生产源码、独立 Archive、CMake 与测试
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：部分 ESP32-S3 实测；v4/掉电/完整多 Bearer 未发布验收

本目录描述 UCN 的可选 Cluster 控制平面。Cluster 用于在节点规模扩大后组织成员、选举 Head、维护 Backup、约束 Authority，并为恢复、合并和重配置提供模型；它不替代 Core 的寻址、转发、Transfer 和 Service。

读者应先建立两个概念。第一，Core 网络在没有 Cluster 时仍能发现邻居、寻路和转发，适合小型 MCU 网络；第二，Cluster 是规模化控制面，用额外的状态、控制消息和持久化成本换取明确的成员集合、多数派权威和故障恢复。启用 Cluster 不会让业务自动变快，它主要让大型或高可靠网络的“谁有权做决定”变得可证明。

## 当前必须区分的四层

| 层级 | 当前状态 | 可否按生产能力描述 |
| --- | --- | --- |
| 默认 `ucn_cluster` | Current v3，固定 32 B，已有默认 FSM 与测试 | 只能按当前实现和限制描述 |
| Wire v4 Archive | 固定 40 B、Type 1–33、严格 Codec | 否；encoder 默认关闭，未接生产 FSM |
| M07～M09 受限组件 | Config、Authority、Backup Mirror 等软件模型 | 否；属于受限实验软件范围 |
| M10/M11/M13 Archive | Takeover、Handover、Rekey，CMake 默认关闭 | 否；不能声称已进入默认产品 |

M05 顶层仍处于 `AUDIT HOLD`。软件测试通过不等于真实 Flash、掉电、多 Bearer 或 MCU 资源验收完成。

## 从开机到稳定权威的逻辑链

```text
节点启动
  → load 持久状态并建立新 boot incarnation
  → 发现邻居/Advertise，按 Epoch domain 判断加入或选举
  → 成为 Member/Head 候选，建立 Membership
  → 冻结 Stable 或 Joint VoterSet
  → 收集有效 Lease/Quorum
  → Authority preflight 通过后才允许权威 TX/RX 副作用
  → Head 配置 Backup，完成 Snapshot/Coverage
  → 故障时走 Takeover、Recovery、Merge/Handover 或 Rekey
```

任一步缺少证明时，正确结果可能是等待、拒绝、撤权或 Fence，而不是继续沿用旧状态。

## 章节地图

| 想了解的问题 | 应阅读 |
| --- | --- |
| Cluster 是否适合我的产品 | 01、16、17 |
| Role/Phase/Term 为什么不能直接等同 Authority | 02、08、15 |
| 当前默认固件实际上运行什么 | 03 |
| v4 为什么是 40 B、为什么还不能默认开启 | 04、17 |
| 节点如何变成 voter | 05、07 |
| 为什么 ACK 前必须落盘 | 06 |
| 配置变化为什么要 Joint Consensus | 07 |
| Head 如何获得和失去发送权 | 08 |
| Backup 如何证明自己有完整状态 | 09 |
| Head 故障、计划切换、跨簇合并 | 10、11 |
| 全簇失去权威后怎么办 | 12 |
| Term/ID 接近上限怎么办 | 13 |
| 多簇之间如何定位/隧道 | 14 |
| 如何排障和判断发布 | 15、17 |

## 阅读顺序

1. [Cluster 与 Core 的关系和适用场景](01-Cluster与Core的关系和适用场景.md)
2. [Role、Phase、Epoch 与 Authority 模型](02-Role、Phase、Epoch与Authority模型.md)
3. [当前默认 Cluster v3 32B 行为](03-当前默认Cluster-v3-32B行为.md)
4. [Wire v4 40B 实验规范与双格式边界](04-Wire-v4-40B实验规范与双格式边界.md)
5. [发布阻断、硬件验收与回滚](17-Cluster发布阻断、硬件验收与回滚.md)

其余章节按 Membership、Persistence、Config、Authority、Backup、Takeover、Handover、Recovery、Rekey、Federation 的依赖顺序展开。

## 阅读时必须区分的状态词

- `CURRENT`：默认产品路径中存在并执行；
- `PARTIAL`：只有部分机制进入默认路径，结论必须阅读限制；
- `EXPERIMENTAL`：独立 Archive/实验开关，不能作为产品能力；
- `AUDIT HOLD`：即使代码和测试存在，也因外审/集成边界未满足而不得放行；
- `NO-GO`：当前候选不满足发布门禁。

如果设计文档、历史建议与本目录冲突，以当前源码、构建开关和本目录标注的事实边界为准；仍有歧义时按更保守的 fail-closed 结论处理。
