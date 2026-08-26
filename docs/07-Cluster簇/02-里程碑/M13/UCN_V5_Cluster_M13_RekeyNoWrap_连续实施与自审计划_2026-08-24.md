# UCN V5 Cluster M13 Rekey / No-wrap 连续实施与自审计划

> 日期：2026-08-24
> 状态：`CODE COMPLETE / SELF-AUDIT PASS / WAIT EXTERNAL REVIEW`
> 范围：`CLV2-13-01..13`
> 硬边界：M05 顶层仍为 `AUDIT HOLD`；默认产品不启用 v4 encoder、production v4 RX/TX/FSM、Authority、Adapter 或 Rekey。

## 1. 前置基线

1. `CLV2-07-00` 已完成：Record v1 的 legacy `PREPARED` 才允许受控迁移；v2/v3 的真实 `PREPARED` 不会被启动迁移误删。
2. RFC4 已冻结 Type 30/31/32 的 40 B 字段布局；M13 只消费已严格解码、已完成 outer-source admission 的 typed message，不修改冻结字段。
3. M04 已提供 `REKEY_PREPARE/REKEY_COMMIT` 持久化操作与 Tombstone 基础合同，但 M13 开始前公共运行时 Hook 仍 fail-before-I/O。
4. M12.3 已把三个未闭环问题移交 M13：Stable/Recovery Epoch scope、Cluster ID allocation history、Recovery round/nonce no-wrap。

## 2. 实施架构

M13 新增独立、default-OFF 的 `ucn_cluster_rekey_experimental` archive：

- `Rekey Owner`：只接受 Authority Owner 已证明的 Stable Head、当前 Stable Config 与 quorum；持有单一 bounded transaction。
- `Wire admission`：仅接收 RFC4 Type 30/31/32 的 typed 值；验证 source、old/new Epoch、Config、txid、nonce、capability 与 voter 身份，不接入生产 dispatcher。
- `Quorum`：冻结旧 Stable Config；只有旧 Config voter 的精确 ACK 可计票；v3 或缺少 Persistence/Rekey capability 的 voter 不可进入可 Rekey Config。
- `Persistence continuation`：先 durable PREPARE，再收 ACK；达到旧 quorum 后原子 durable successor Epoch、successor Config 与旧 ID Tombstone，之后才允许 COMMIT 输出。
- `No-wrap`：Term、Config、Backup generation、Snapshot generation、Recovery round、Cluster-ID round 与 Recovery nonce 统一走 checked serial/rotation；没有安全 continuation 时 fail-closed。
- `Identity history`：产品 Provider 必须证明候选 ID 未与持久化历史冲突；默认 mix 仅是候选生成器，不是唯一性证明。

## 3. 分项执行与自审门禁

| 小节 | 实施重点 | 当项自审门禁 |
|---|---|---|
| 13-01 | `HEAD_REKEYING`、单事务状态、Stable Authority/Config/quorum 发起门 | 非法 Phase、无 Authority、Joint/无 quorum、Record provenance 不满足均零写拒绝；默认 archive 无新符号。 |
| 13-02 | 全部 serial threshold/rotation 判定 | `MAX→1/0` 扫描为零；threshold 前最后一步和 threshold 拒绝均有回归。 |
| 13-03 | 新 Cluster ID Provider + collision admission | 0/broadcast/父 ID/历史冲突拒绝；失败不消耗 round；accepted ID 可进入 durable staging。 |
| 13-04 | Type 30/31/32 typed admission | old/new Epoch、Config、source、txid、nonce 任一错配拒绝且 transaction 不写；v3/缺 capability voter 拒绝。 |
| 13-05 | 旧 Stable Config quorum | 非 voter、重复 ACK、旧/跨事务 ACK 不计票；Joint/Config PREPARED 与 Rekey 不并发。 |
| 13-06 | persist-before-prepare/commit | Provider sync/async/reentry/failure/crash matrix；未 reload exact proof 不释放 ACK/COMMIT；失败立即 Fence。 |
| 13-07 | Tombstone/replay | Commit 与 Tombstone 同一 durable snapshot；重启后旧 Cluster Type 1..33 均不能建立 continuation。 |
| 13-08 | successor 原子运行时安装 | successor 固定 Term/Config/generation/snapshot 初值；成员与 Backup 不得部分跨域计 quorum。 |
| 13-09 | no-wrap 静态门禁 | 静态脚本扫描禁用 raw serial increment/wrap pattern，并纳入 CTest。 |
| 13-10 | Rekey safety suite | ACK 丢失、旧 quorum 不足、persist failure、restart、Backup 替换、旧帧 replay 全覆盖。 |
| 13-11 | Record Epoch scope/Recovery tombstone | Stable 与 Recovery scope 可恢复区分；Recovery create→restart 不污染 Stable history；撕裂写 fail-closed。 |
| 13-12 | allocation history | 固定碰撞向量、重启、满历史、并发候选与换号收敛；无全局证明时不赋 Authority。 |
| 13-13 | Recovery serial no-wrap | round/ID-round/nonce threshold 前进、阈值拒绝、重启不回退与旧值不复活。 |

每完成一个小节：

1. 新建该项自审记录；
2. 执行定向测试、默认产品隔离构建和差异检查；
3. 将任务表更新为 `CODE COMPLETE / SELF-AUDIT PASS`；
4. 只有当项无阻断问题才进入下一小节。

## 4. M13 全体自审

13-01..13 完成后重新从零审查：

- Config/Rekey 事务互斥与跨重启恢复；
- 旧 Config quorum、successor continuation 与 Tombstone 原子性；
- Provider `load/submit/poll` 同步重入和异步 Pending；
- Stable/Recovery scope 与 ID allocation history；
- 所有 serial no-wrap；
- Full/Lite/Nano、Service OFF、默认 M13 OFF、ASan/UBSan、`-fanalyzer -Werror`、资源报告；
- production Cluster/Adapter 无 M13/v4 新接线。

最终仅形成 `SELF-AUDIT PASS / WAIT EXTERNAL REVIEW` 候选；不自行解除 M05 或宣称实机/掉电完成。

## 5. 实施结果

- 13-01..13 均已形成独立自审报告；全体结论见 `UCN_V5_Cluster_M13_全体自审报告_2026-08-24.md`。
- 全体自审新增并关闭了 deadline preflight、完整 durable-state validation、Provider 重入覆盖、history canonical 化和 `history durable-before-PREPARE` 五类缺口。
- Record 当前 writer 为 schema v4 / 388 B；v1/v2 280 B、v3 292 B 只读。
- allocation history 为独立 280 B bounded record；主 Record v4 绑定其 generation 与完整 fingerprint，重启 Resume 必须同时 reload 两者。
- 当前状态只允许送外部审计；M05 顶层 `AUDIT HOLD`、默认 v4 encoder OFF 与无 production RX/TX/FSM 接线保持不变。
