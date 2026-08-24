# CLV2-14-11 最终 Safety / Liveness 自审清单

## 总结论

本轮已完成“逐条审查”，结论为 **RELEASE NO-GO**。10 条 Safety 都已有源码合同、负向测试或 bounded model 证据，但完整 Target v2 仍没有 production Wire v4 RX/TX/FSM 接线和真实四板/掉电证明；不能把组件测试数量当成端到端协议证明。

状态含义：

- `PASS-COMPONENT`：独立组件/模型的安全合同通过；
- `PARTIAL`：存在当前软件证据，但 production Target 组合尚未建立；
- `BLOCKED`：缺少必要接线、外审或真实硬件证据。

## Safety 1..10

| 性质 | 组件证据 | Target v2 最终状态 | 判定 |
|---|---|---|---|
| S1 Single Writable Authority | M08 Authority owner、14-04 network invariant、14-05 27 组分区 | production v4 Config/Takeover/Handover 未接线 | `PARTIAL` |
| S2 No Authority Without Quorum | Authority preflight、lease 到期/RX/Federation-first、Config install fail-closed | 默认产品没有 v4 CommittedVoterSet owner | `PARTIAL` |
| S3 Takeover Majority | M10 frozen Stable/Joint quorum、VoteId、certificate、durable terminal | M10 仍待外部复审，默认 archive 不接入 | `BLOCKED` |
| S4 Recovery Isolation | M12 lineage/round/new ID、M13 persisted scope/Tombstone、14-04 invariant | M12/M13 外审与 production v4 Recovery 接线未完成 | `PARTIAL` |
| S5 Persistent Vote | M04 Vote durability、M10 current-Epoch single vote、历史 vote rotation | 真实 Flash/掉电未测；M10 外审未结束 | `BLOCKED` |
| S6 Config Safety | M07 durable PREPARED→JOINT→COMMIT、双 quorum、exact Backup gate | Type 20..25 production Wire/FSM 未接入 | `PARTIAL` |
| S7 Replay Isolation | Record journal/fingerprint、v4 pending、M10/M11/M13 replay/Tombstone | 完整网络重放与真实重启介质未测 | `PARTIAL` |
| S8 Fence Before Split Brain | M08 同 Step 撤权、Grace/Fenced no-TX、M10/M11 Fence ordering | production Takeover/Handover 组合未接线 | `PARTIAL` |
| S9 No Serial Reuse | checked-serial、source gate、Rekey/Recovery threshold/history | 实际长期运行、介质耗尽与产品 rotation 未实测 | `PARTIAL` |
| S10 Persist Before Promise | M04 sync/async/PENDING/reentry/reload、M07/M10/M13专用 owner | 真实 Flash 双槽和断电窗口缺失 | `BLOCKED` |

没有发现允许将某一条标成“已被反例推翻”的新缺陷；但也没有任何理由把 10 条组件证据合并成 production Target 全 PASS。

## Liveness 1..5

| 性质 | 当前证据 | 缺口 | 判定 |
|---|---|---|---|
| L1 稳定 Majority 最终形成 Stable Head | Current FSM clean 64/256/1000 均 8920 ms 收敛 | v4 voter/Authority production 组合及实机 | `PARTIAL` |
| L2 READY Backup + Majority 完成 Takeover | M09 mirror + M10 bounded model | v3 路径刻意 fenced；v4 production/外审/实机缺失 | `BLOCKED` |
| L3 可互联 Cluster 确定性 Merge | M11 hysteresis/domain/ABA/hold-down 实验模型 | production Handover Wire/FSM 与规模 churn 未接入 | `BLOCKED` |
| L4 无 Stable Authority island 形成 Recovery Cluster | Current/M12 软件场景存在恢复证据 | 真实分区、重启、持久介质、多 Bearer 未测 | `PARTIAL` |
| L5 Recovery Island 看见 Stable Head 后让位 | M12 stable precedence/lineage adoption 软件回归 | production v4 联合与四板网络未测 | `PARTIAL` |

## M14 任务关闭情况

| 任务 | 状态 |
|---|---|
| 14-01、02、04、05、06、09、10 | 软件完成，等待 M14 外审 |
| 14-03 | 模块拆分完成；production switch 被 M05 阻断 |
| 14-07 | Current-FSM 规模矩阵完成；Target churn 被 M05 阻断 |
| 14-08 | 真实硬件阻断 |
| 14-11 | 本清单完成；最终结论 NO-GO |
| 14-12 | BLOCKED / NO TAG |

## 必须解除的发布阻断

1. M05 production Wire v4 集成获得独立审计放行；
2. M08/M09/M10/M12/M13 剩余外审/依赖状态同步完成；
3. 14-03 在默认产品完成 strict-v4/v3 policy 切换；
4. 14-07 完成 Config/Backup/Recovery/Rekey Target 规模 churn；
5. 14-08 完成四板、真实 Flash/掉电、多 Bearer 重复实测；
6. 再执行一次从空构建环境开始的最终矩阵与审计。
