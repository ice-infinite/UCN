# UCN V5 Cluster M10 最终 Majority Takeover 连续实施计划（2026-08-23）

## 1. 授权与硬边界

用户已明确授权连续实施 `CLV2-10-01..11`，要求每小节完成后进行自审、全部完成后再做一次全量自审并提交外部审计。

当前 M09-R01 已获受限外审 GO，但 M08 仍为 `WAIT EXTERNAL`，M05 顶层仍为 `AUDIT HOLD`。因此本计划的实现范围是 **caller-owned、受控实验软件模型**：

- 可以新增 M10 value model、完整 VoteId、Record schema 迁移、Provider 事务 owner、Certificate builder/verifier 和专用 CTest；
- 不得在 `src/extended/ucn_cluster.c`、Adapter、默认 Cluster 发送/接收路径接入 M10；
- 默认 `UCN_CLUSTER_WIRE_V4_ENCODER_ENABLED` 保持 `0`；测试 target 可使用既有 codec semantic/certificate helper；
- 任何 `VOLATILE_TEST` 成功只证明状态机，不证明 Flash 掉电、生产 Authority、MCU 资源或实机互通。

## 2. 总体对象关系

```text
M09 committed mirror + exact SnapshotEpoch
                 + M07 canonical Stable/Joint Config
                              |
                              v
                  M10 Takeover transaction
          { frozen Config, frozen BackupEpoch/snapshot,
            proposed Epoch, VoteId, old/new vote bitmaps }
                              |
          +-------------------+--------------------+
          |                                        |
          v                                        v
  Member durable VoteId                     Certificate verifier
  (M04 Provider / Record v3)          (frozen voter order + CRC + quorum)
          |                                        |
          +-------------------+--------------------+
                              v
              durable proposed Epoch -> experimental Head result
```

M10 的所有 quorum 分母只来自 transaction 冻结的 Config，绝不从后续 Runtime member 表、legacy v3 record 或未完成 staging snapshot 推导。

## 3. 分项执行顺序与自审门禁

| 顺序 | 任务 | 实现要点 | 分项自审重点 |
|---:|---|---|---|
| 10-00 | Scope fence | 新 API 不进入 production Cluster/Adapter；专用 target 独立注册。 | 扫描 `ucn_cluster.c`/Adapter、确认 encoder 默认 0。 |
| 10-01 | Frozen transaction | 从 M09 **committed** mirror 创建 transaction；冻结 Config、BackupEpoch、snapshot、`old_term + 1`、txid。 | staging/Runtime member 变化不能改 frozen denominator。 |
| 10-02 | Full durable VoteId | Vote 持久化 `{cluster,old_term,proposed_term,config_id,backup,generation,snapshot}`；Record v3 显式迁移旧 v1/v2。 | 旧 schema partial vote 不得作为 M10 投票证明。 |
| 10-03 | Member vote gate | 仅 explicit takeover grace + old Head lease expired 的 committed v4 voter 可投票。 | ACTIVE/provisional/v3/过早 lease 全部零写拒绝。 |
| 10-04 | Stable/Joint quorum | old/new frozen voter sets 各自 bitmap 与 self-vote。 | 所有 1..max voter 边界、Joint 双 quorum。 |
| 10-05 | Certificate | 固定 voter 排序、两片上限、canonical CRC32、完整 fragment/quorum 验证。 | forged/duplicate/out-of-range/partial certificate 无结果写回。 |
| 10-06 | Proposed Epoch persistence | quorum 后 `submit -> load -> exact journal`，成功才产生 experimental Head result。 | I/O pending/failure/重入不得产生 Authority/announce。 |
| 10-07 | Old primary fence | 只接受完整 higher-term certificate；一经接受永久 Fence/Join intent。 | score、旧/迟到帧、重启重放不能抢回。 |
| 10-08 | Refresh overlap | staging refresh 期间只使用 exact committed snapshot/config；否则拒绝开始。 | staging 永不进入 VoteId/certificate。 |
| 10-09 | Legacy exclusion | frozen voter proof 只允许 committed + voting + v4；v3/provisional 不计票。 | 不能以降 denominator 或补位绕过。 |
| 10-10 | Timeout/impossible | 可达票数不足时立即 abort；deadline 到期进入 Recovery intent。 | 不降低 frozen denominator、不复用 txid。 |
| 10-11 | Crash/property | Vote/epoch persist 各阶段、乱序/重复/分区随机序列。 | Safety-1/3/5/10、no-write、可重启证明。 |

## 4. Record v3 迁移规则

M10 的完整 VoteId 比 M04 v2 的 `{old Epoch, candidate, generation}` 多出 `proposed_term/config_id/snapshot_id`。不能把这些字段从 Runtime 推测，也不能复用 v2 的保留字节。因此将定义 **Record schema v3**：

1. v3 采用显式长度、CRC 和 canonical 大端字段；旧 v1/v2 只读解码保持可用；
2. 旧记录中的 partial Vote 只能用于历史诊断/旧路径兼容，M10 owner 不得把它当作完整 Vote proof；
3. 正常 M10 Vote/epoch 写入 v3；既有 partial Vote 可保留为只读历史状态，但 M10 必须拒绝它，不能静默补全、计票或把它当作 durable proof；
4. Provider 仍只提交完整 logical snapshot；任何 pending/failure 均不产生 ACK、Head result 或 certificate acceptance。

## 5. M10 不变量

1. `proposed_term == checked_next(old_term)`，且 proposed Head 始终等于 frozen Backup。
2. 输入必须来自 M09 committed SnapshotEpoch，`staging_active` 不能成为取消/替代 committed proof 的理由。
3. Stable 必须 `quorum(C_old)`；Joint 必须 `quorum(C_old) && quorum(C_new)`；self vote 只在 Backup 属于对应 set 时计入。
4. 单个 Member 对一个完整 VoteId 只能做出一个 durable promise；Config/snapshot/backup/generation 任一变化都是冲突，不得 ACK。
5. Certificate 必须按 frozen voter ID 升序、全部 fragment、合法 bit 上界和 RFC4 canonical CRC 验证；不能相信 source 或计数器。
6. 只有 durable proposed Epoch 后才允许产生 experimental `HEAD_READY` 结果；本计划阶段不发送任何 production Head frame。
7. Timeout/impossible 只 abort 当前 transaction 并提出 Recovery intent，不能缩小 denominator 或继续相同 txid。

## 6. 全量自审与外审材料

M10 结束后统一执行 Full/Lite/Nano/Service-OFF/产品配置、Release、ASan/UBSan、`-fanalyzer`、Record v1/v2/v3 decode/upgrade、Provider sync/async/reentrant failure matrix 和 property/reorder 试验。外审必须另外确认：

- M10 没有越过 M05 默认生产 v4 RX/TX/FSM/encoder/Authority 门；
- 旧 v3 Backup handler 不会调用 M10；
- 旧 Primary Fence 和 Member Vote 都需要 full frozen certificate，不接受 count/source shortcut；
- 新 Record v3 不会把 v1/v2 partial Vote 错当为完整 M10 Vote；
- 尚未完成真实 Flash 掉电、MCU RAM/stack、实机多跳/多板验证。

## 7. 实施结果（2026-08-23）

`10-00..11` 已按本计划完成代码和分项自审。为了不突破 M05/M08 边界，实际实现采用比原计划更严格的物理隔离：

- `UCN_BUILD_CLUSTER_TAKEOVER_EXPERIMENTAL=OFF` 为默认；M10 仅在显式打开时构建为独立 `ucn_cluster_takeover_experimental` archive；
- 默认 `libucn_cluster.a` 不含 M10 object，`ucn_cluster.c`、Adapter 和 `ucn_cluster.h` 没有 M10 调用；
- Record writer 已从 M07 的历史 v2 提升为 append-only v3（280 B legacy body + 12 B VoteId extension = 292 B）；v1/v2 保持只读；
- M10 persistence owner 的 `apply_durable_vote/epoch` 是唯一公开的 model progression 入口，内部标记函数只在 private header 中供 owner/test 使用；
- 已完成 Full/Lite/Nano/Service-OFF、Release/O1-O3、配置契约、产品默认关闭、WSL ASan/UBSan 和 `-fanalyzer` 矩阵。详细结果、资源与限制见 `UCN_V5_Cluster_M10_分项与全量自审报告_2026-08-23.md`。

**原状态已被外审撤回：** `CODE COMPLETE / SELF-AUDIT PASS / WAIT EXTERNAL` 只适用于首轮自审，不构成外审通过。

## 8. 外审 R31–R34 整改计划

| 编号 | 风险 | 根因 | 修复与验收 |
|---|---|---|---|
| R31 | P0 | generic `EPOCH_COMMIT` 不知道 current state 已存在 complete M10 VoteId。 | 在 generic transition 中拒绝“complete VoteId 属于 current Active Epoch”的 state；保留 dedicated `TAKEOVER_EPOCH_COMMIT` 作为唯一 successor 写入。 |
| R32 | P0 | terminal `EPOCH_DURABLE` 被 collecting/quorum 的通用 step/input 路径重新推进。 | terminal-first guard：step no-op，remote vote/unreachable reject；exact durable epoch apply 可幂等。 |
| R33 | P1 | 新 vote 无条件拒绝 `last_vote.valid`，没有区分 current 与 historical Epoch。 | 只拒绝 current Active Epoch 的任意已持久 Vote；允许将历史 vote（含 v1/v2 partial）原子替换成新的 v3 full Vote。 |
| R34 | P1 | 10-11 仅覆盖部分软件 fault/reorder，没有覆盖上述状态交叉；首个 R32 回归还把终态门禁与旧 deadline/bitmap-overlap 拒绝混在一起。 | 把 bypass、terminal、two-round 和 legacy-history counterexamples 加入 CTest；R32 同时固定未到期 `step(150)` 与未投票节点 `1` 的 unreachable；外审材料撤回首轮“全覆盖”表述。 |

**当前状态：** `AUDIT HOLD / R31–R34 CODE COMPLETE / SELF-AUDIT PASS / WAIT EXTERNAL RE-REVIEW`。四项整改及其正式 CTest 已完成；该状态不是外部 GO。整改前后都不改变 M05 `AUDIT HOLD`、M08 `WAIT EXTERNAL` 或任何 production v4/Authority/实机门禁。
