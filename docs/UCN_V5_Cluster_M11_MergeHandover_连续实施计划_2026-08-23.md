# UCN V5 Cluster M11：Merge / Handover 连续实施计划（2026-08-23）

## 1. 授权、依赖和硬边界

用户已授权连续实施 `CLV2-11-01..10`：每项完成须留下定向自审和测试证据，全部完成后再做全量自审并整理外部审计材料。

现场依赖仍有 M05 `AUDIT HOLD`、M08 `WAIT EXTERNAL` 和 M10 `WAIT EXTERNAL RE-REVIEW`。因此 M11 的完整协议模型必须是 **caller-owned、default-OFF 的实验 archive**，不能把 RFC4 Type 26..29 编解码、发送、RX、Authority 或 Adapter 接入默认产品。R01–R07 的既有外部复审 GO 保留各自签字范围；R08 原设计因 ABA history-loss 已撤回，R08-A 当前为 **SELF-AUDIT PASS / WAIT EXTERNAL RE-REVIEW**，M11 整体仍为 `AUDIT HOLD`，不改变上述依赖或默认产品边界。

默认产品允许且必须执行的安全收敛只有两项：

- Member 不得再依据 foreign/same-cluster Head score 自主 `LEAVE → JOIN_PENDING`；只接受当前 Head 续租、已验证 Stepdown、Takeover 或 lease failure 的既有安全入口。
- Backup 不得再因 score 优势调用 legacy `backup_challenge()` 直接创建 `Term + 1` election；同簇活 Primary 的领导权优化只能由 M11 Planned Transfer 实验事务表示，失败时旧 Primary 保持不变。

## 2. 分层设计

```text
default libucn_cluster.a
  ├─ 保留同簇 Epoch/Authority 安全分类
  └─ 移除 v3 score 自主切换和 score→Term++ challenge

default-OFF ucn_cluster_handover_experimental.a
  ├─ Foreign / same-cluster offer classifier
  ├─ 固定容量 Merge candidate + hysteresis
  ├─ feasibility / capability / config / backup policy gate
  ├─ Prepare → Ready → revoke → Stepdown → Commit transaction
  ├─ Member / Provisional / Backup Stepdown target join model
  └─ Planned same-cluster Backup leadership transfer model
```

实验对象使用 RFC4 的双 Epoch、txid、target Config 语义，但只生成 typed value；它不调用 v4 encoder、不发送帧、不写 `ucn_cluster_t`、不获得 Authority，也不替代 M10 Takeover Certificate。

## 3. 子任务、实现与自审

| 顺序 | 任务 | 实现 | 分项自审 |
|---:|---|---|---|
| 11-00 | Scope fence | **PASS**：archive 默认 OFF、header 编译门、默认 archive/production call-site 扫描。 | 默认 `ucn_cluster.lib` 无 M11 transaction/candidate 符号；裸 include 失败、显式宏成功。 |
| 11-01 | Offer classifier | **PASS**：仅同 cluster 进入 Authority；foreign 仅进入 Merge，Term 不比较。 | B/T100 不压制 A/T2。 |
| 11-02 | Candidate state | **PASS**：固定槽保存 foreign identity、score samples、cluster size/capacity、wire offer、Config、tenure/hold-down/replay。 | stale/replay/过期均无写。 |
| 11-03 | Hysteresis | **PASS**：improvement、连续样本、minimum tenure、hold-down；安全输入不受 hold-down。 | 临界抖动不启动事务。 |
| 11-04 | Feasibility | **PASS**：capacity、v4 offer、required capability、target Config/Backup policy 精确验证。 | capacity=0 可发现但不可 READY。 |
| 11-05 | Handover protocol | **DONE / 外部复审 GO（受限实验范围）**：typed Prepare/Ready/Commit 仅绑定 RFC4 实际承载的双 Epoch、target Config、txid、mode/source；它们的 nonce 固定为零。retry/transaction deadline 使用 safe duration，serial 不越 rotation threshold；撤权会写入不可由 public reset 清除的 re-entry Fence。 | duplicate 幂等；Type 26..28 nonce、illegal duration、Term/Config/txid 越界、conflict 与撤权后的 Begin/Reset→Begin 均零写拒绝。 |
| 11-06 | Authority ordering | **PASS**：local Losing Head trace：READY 后 revoke → Stepdown → Commit/Join。 | 撤权后超时进入 Observe，不恢复旧 Authority。 |
| 11-07 | Stepdown target | **PASS**：Member/Provisional/Backup 只验证 old/target Epoch、mode/nonce；Type 9 不携带 Config；target lost 转 Observe。 | 不要求成员见过 READY，伪造 Config 字段拒绝。 |
| 11-08 | Remove Member switch | **PASS**：默认 v3 score offer 不再触发 LEAVE/Join。 | foreign 高 Term/score 不撕裂当前 Cluster。 |
| 11-09 | Planned Backup transfer | **PASS**：same Cluster、exact next term、confirmed Backup、same frozen Config、Backup READY 无 Authority。 | 失败时 Head 稳定，无 Term++ challenge。 |
| 11-10 | Merge suite | **DONE / 外部复审 GO（受限实验范围）**：双簇、抖动、丢包/重复、winner lost、backup/provisional、hold-down reverse score 与流程组合；另将 RFC 字段、duration/serial、target timeout/no-fake-durable、公开结构越界及撤权后的 Begin/Reset→Begin 对抗例固定为正式回归。 | 定向与全量矩阵通过；结果只覆盖实验 archive。 |

## 4. 不变量

1. foreign `cluster_id` 永远不以 Term 值做 authority、rank、stale 或 tie-break 判断。
2. Candidate 的所有比较只在同一 foreign identity 的 nonce/replay 域内进行；不同 cluster 不共享 serial 域。
3. 只有 feasibility + hysteresis 均成立的 local losing Head 能发 Prepare；Ready 前绝不撤权或 Stepdown。
4. Ready 的 exact `{old epoch,target epoch,txid,target config,mode,source}` 是 Losing Head 唯一撤权触发；Type 26/27/28 不承载、也不得比较 nonce。撤权严格早于生成 Type 9/29 nonce、Stepdown，Stepdown 严格早于 Commit。
5. 同簇 Planned Transfer 只允许 old Head 事务、exact next term、different confirmed Backup 和 unchanged frozen Config；Backup Ready 不是 Authority。
6. Member 不以本地 score 选择 Cluster；Stepdown 只给 Join target 意图，不能单独改 Config、Authority 或持久 Epoch。
7. 任何缺包、timeout、winner infeasible、target lost 或 replay conflict 都保持旧簇稳定或进入 Observe，绝不凭空授予 Authority。
8. M11 在接入 M04 Provider submit/reload continuation 前，`TARGET_COMMITTED` 仅是等待持久化证明的无 Authority 状态；deadline 到期即 Abort/Observe，caller 提供相同 Epoch 不能伪造 durable。
9. caller-owned public transaction 必须先通过固定容量结构校验：`trace_count <= 8`、合法 state/trace、Epoch/Config/txid serial、deadline 和 nonce phase 均成立；失败不得读取 trace 或写 output。
10. `transaction_begin()` 是新 transaction 的唯一构造入口，但仅可消费零对象；`revoke_authority()` 会写入独立 re-entry Fence，public `transaction_reset()` 对已 Fence 的撤权、Stepdown-sent、Commit-sent transaction 必须 no-op。此类对象只能保留、推进到 Observe/Recovery，不能被 Begin 或 Reset→Begin 清零复活 Authority。
11. candidate 的 `score_samples` 只能记录同一 hysteresis context、同一 local score、improvement percent、required samples 与 required capabilities 输入下连续达到 improvement threshold 的 fresh offer；任一不合格 score、size/capacity/capability/wire/Backup-policy 或这些 qualification inputs 改变都必须清零。Epoch+Config 是独立且单调前进的 replay namespace：context 变化不清 nonce high-water，旧 context 不能借 D1→D2→D1 ABA 重放复用历史；只有严格前进的 Epoch/Config 才能从低 nonce 开始。

## 1.1 R07/R08 补充整改与重新外审边界

最新复审核实两项 candidate 迟滞缺口：过去的实现把任意 fresh nonce 计为 sample，且 replay/history 只绑定 `{cluster_id, head_node_id}`。这会让低质量 packet 累积成资格，也会让新的 Epoch/Config proposal 继承旧 nonce/sample。

- R07：只在 score 满足 `candidate * 100 >= local * (100 + improvement_percent)` 时递增；不满足时清零。local score、improvement 百分比或 required samples 改变同样清零，eligible 检查拒绝任何 stale context。
- R08：原整改将 full Epoch、Config ID/hash、cluster size/capacity、capabilities、wire format 与 backup policy 共同当作 proposal/replay 域；其后外审发现 capacity 等可逆字段导致 D1→D2→D1 ABA history-loss，原 R08 结论撤回并由 R08-A 取代。
- R08-A：replay namespace 仅为 full Epoch + Config ID/hash，必须严格向前。cluster size/capacity、capabilities、wire format 与 Backup policy 为 hysteresis/feasibility context；它们改变时清 `score_samples/first_seen`，但本 namespace nonce 仍严格递增。故 capacity `8→7→8` 必须使用 nonce `100→101→102`，历史 `50/51` 永远不能变成 fresh samples；真正新 Epoch/Config 才可合法从 nonce `1` 开始。

R08-A 已完成源码与定向/全量 Host 自审，当前仍不增加生产接线；M11 仅可在 R08-A 外部复审重新签字后恢复受限实验范围 GO。

## 5. 最终自审和外审材料

完成 11-00..10 后执行 Full/Lite/Nano/Service-OFF、Release、config-contract、默认产品、WSL ASan/UBSan 与 `-fanalyzer`；再核对 default archive、生产 Cluster/Adapter 和 v4 encoder 仍无 M11 接线。外审材料必须明确：软件模型不证明真实 RFC4 互通、Flash/掉电、MCU 资源、无线/多跳或任何生产 Authority 结论。
