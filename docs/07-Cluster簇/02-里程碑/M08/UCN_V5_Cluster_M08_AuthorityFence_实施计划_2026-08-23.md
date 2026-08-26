# UCN V5 Cluster M08：Authority / Quorum / Grace / Fence 实施计划

> 日期：2026-08-23  
> 状态：`SELF-AUDIT PASS / WAIT EXTERNAL`；R31/R32 P0 已整改并复测，尚未形成提交，等待本轮外部审计。  
> 前置：M07 已获“受限实验软件范围”外部 GO；M05 顶层仍为 `AUDIT HOLD`。

## 1. 本轮目标

M08 收敛 Head 的两个概念：

- `role == HEAD` 仅保留 Head 身份上下文；
- `authority_active == true` 才能发送或写入 Authority 级控制数据。

安全不变量为：

```text
authority_active => 当前 canonical Config 的 quorum 仍有效
```

一旦 Owner Step 首次检查到 quorum 不足，必须按以下顺序完成：

```text
authority_active = false
-> 阻止 Authority TX / Directory 写
-> HEAD_QUORUM_GRACE
```

Grace 只保存旧 Head 身份和恢复机会，不保留写权限。Grace 超时、same-term conflict、已验证更高 Authority 或持久化故障均进入 `HEAD_FENCED`；同一 `(cluster_id, term)` 永远不可重新取得 Authority。

## 2. M05 与产品边界

M08 允许把 Authority 逻辑接入一个**明确安装 canonical Config 的受控实验实例**，以验证 M07 Config/Joint/Persistence 的结果如何约束 Head。

以下边界在整个 M08 期间不变：

- 默认产品不启用 v4 encoder、v4 production RX/TX/FSM 或 v4 Authority。
- 没有 M07 canonical Config / lease 证据的 `REQUIRED` Cluster 不得获得 Authority；不能借用 v3 role、邻居瞬时 `SUSPECT` 或 `VOLATILE_TEST` 作为生产证明。
- `VOLATILE_TEST` 只可用于 Host 的受控时序与分区模型，不能成为 Flash、掉电、MCU 或真实互通证据。
- M08 不实现 M09 Backup 双缓冲、M10 Certificate Takeover、M11 Handover、M12 Recovery lineage 或 M13 Rekey。

## 3. 模块与状态落点

新增 `ucn_cluster_authority` 受控 Owner：它持有 immutable M07 Config snapshot、每个 voter 的 Cluster Keepalive/lease deadline、timer budget 和 Fence 原因；它不从 Core Neighbor `SUSPECT` 直接减票。

`ucn_cluster_t` 只保存可诊断、可统一门禁的最小状态：

```text
authority_active
authority_phase
authority_fence_reason
head_resume_phase
quorum_loss_deadline_ms
quorum_restore_since_ms
fenced_dissolve_deadline_ms
```

Public view 复制以上只读状态；TX 与 Federation 只能查询该唯一 Authority 状态，而不能再以 `role == HEAD` 作为写权限。

## 4. 连续实施清单与自审点

| 顺序 | 任务 | 实现与定向自审 |
|---|---|---|
| 08-01 | Phase / state | 加入 `HEAD_RECONFIGURING`、`HEAD_QUORUM_GRACE`、`HEAD_FENCED` 与合法边；逐边 transition test。 |
| 08-02 | Authority 独立状态 | Public view + `ucn_cluster_authority_active()`；验证 role 与 Authority 可以分离。 |
| 08-03 | quorum / lease | Stable、Joint old/new 独立 quorum；self vote、lease 过期、`SUSPECT` 无直接影响。 |
| 08-04 | 同 Step 撤权 | trace 断言撤权早于 phase 进入 Grace 和任何发送。 |
| 08-05 | 集中 TX 矩阵 | Grace/Fenced 阻断 Head advertise、heartbeat、join、Backup/Config Authority 类发送；逐 Phase/Type 表测试。 |
| 08-06 | 恢复 hold | 连续 quorum、无 conflict/higher/persistence fault 才恢复保存 Head phase。 |
| 08-07 | 永久 Fence | timeout/conflict/higher/fault 均 Fence；same term 无法重新激活。 |
| 08-08 | Fence cleanup | higher stable Authority 走 Join pending；否则有界 dissolve 到 Recovery observe，保留 lineage。 |
| 08-09 | Federation / Directory | Locator publish/renew/handover 全部走 Authority gate；旧 Head 在 Grace/Fenced 无写入。 |
| 08-10 | timer budget | 用 owner/network/retry/jitter/drift/margin 派生 lease，非法组合 init 拒绝。 |
| 08-11 | Member grace | 验证 Member Takeover Grace 与 Authority/lease budget 的关系。 |
| 08-12 | partition property | 3/4/5/6 节点所有多数切分：至多多数侧可写，少数 Head 同 Step 撤权。 |
| R31 | current-time preflight | Cluster TX、RX 的 Head 副作用、Federation step/query/public handover 均先以当前时钟刷新 Owner；过期缓存不可作为权限。 |
| R32 | atomic Config switch | `install_config(..., now_ms)` 对候选 Stable/Joint 先计算 quorum；旧 active 先撤销，无 quorum 立即进入 Grace。 |

每一项完成后记录：源文件、反例、定向测试、全量回归影响与仍未覆盖的边界。08-12 后执行 M08 全量自审：事务/重入、Stable+Joint quorum、lease 回绕、分区恢复、fence、Directory、v3/v4 隔离、Profile/ASan/`-fanalyzer` 矩阵。只有该自审通过后才提交给外部审计。

## 5. 实施结果与送审范围

`08-01..12` 已完成分项自审。`authority_active` 的唯一正向来源是显式安装的 M08 Owner、canonical M07 Config 与 voter lease；未安装 Owner、legacy v3 role、Neighbor `SUSPECT` 或任意 v4 codec/offer 都不能使其为 true。

受控 Owner 仅接受 `VOLATILE_TEST`，以避免将 value-only Config 误称为 REQUIRED 持久化证明。真实产品把 M07 durable Config owner 接到 M08 的工作仍被 M05 顶层 `AUDIT HOLD` 阻断。

完整的逐项证据、反例和矩阵见 `UCN_V5_Cluster_M08_分项与全量自审报告_2026-08-23.md`；当前状态是等待外部审计，而不是 M08 DONE 或生产发布。
