# UCN V5 Cluster M07：Committed / Joint Membership Reconfiguration 连续实施与自审计划

日期：2026-08-22  
范围：`CLV2-07-00` 至 `CLV2-07-12`  
状态：**IN PROGRESS / 连续实施、分项自审；尚未授权外部审计或生产放行。**

## 1. 目标与完成判据

M07 使 voter quorum denominator 成为受保护、持久化且可恢复的 Config 状态。任何成员变更必须遵守：

```text
C_old (Stable) → C_joint (old + new 双 quorum) → C_new (Stable)
```

完成时必须同时满足：

- 每一项 `07-00..12` 有单独自审记录、定向测试和失败路径证据；
- Config Prepare / ACK / Joint / Commit / Abort 的安全承诺均先经过持久化；
- 加入者在 Commit 前恒为 `PROVISIONAL/non-voting`，移除者在 Commit 前仍位于 `C_old` denominator；
- 每个可恢复阶段重启后只恢复一个合法 Active Config；
- M06 legacy auto-commit bridge 被移除，voter 变更只能走 Config transaction；
- 完成 M07 全量自审矩阵后，才移交一次外部审计。

## 2. 不可跨越的边界

M05 顶层仍为 `AUDIT HOLD`。因此本轮：

- 默认产品不启用 v4 encoder、production v4 RX/TX/dispatcher/FSM、Authority 或 Adapter/Link 接线；
- Config/Joint/Persistence 联动只允许明确命名的 Host 测试/实验 target 使用，且编译定义必须 target-private；
- `VOLATILE_TEST` 只用于状态机/接口验证，绝不作为 Flash 原子性、掉电恢复或 ACK/Commit 安全承诺的证据；
- 真实 MCU、Flash 双槽掉电、网络互通和资源门禁均留待后续实测；
- 任何默认产品路径若试图处理 M07 v4 authority/config frame，必须继续 fail-closed。

## 3. 07-00：Record v2 与 PREPARED 来源区分（先决门禁）

### 3.1 问题

Record v1 能承载 `CONFIG_PREPARE` / `REKEY_PREPARE`，但不存在“它是 M04 历史遗留事务还是 M07/M13 新事务”的不可混淆来源。R23 的 `LEGACY_PREPARED_ABORT` 只能处理旧 v1 记录；如果它清除了未来新事务，会破坏 Prepare 的重启安全。

### 3.2 固定方案

1. Record writer 升级至 schema v2；decoder 仍只读兼容 v1 与 v2。
2. v1 decode 的 `PREPARED` 被标为 `LEGACY_V1` provenance；只有它可在 controlled REQUIRED boot 走一次 `LEGACY_PREPARED_ABORT`。
3. v2 的 Config / Rekey `PREPARED` 使用各自 current provenance；它们绝不可走 legacy abort。
4. v2 PREPARED 重启后由所属 owner 恢复为 `RESUME_REQUIRED`：在 M07 中 Config 只能 resume、显式 abort 或匹配 commit；M13 前 Rekey 仍不生成新的 PREPARED，并保留 fail-closed。
5. 物理 v2 encoder 永远不产生 legacy provenance；将 v2 伪装成 legacy、混合 provenance、双 PREPARED 或篡改保留字节均拒绝。

### 3.3 07-00 自审门禁

- v1 Config / Rekey PREPARED：重启仅迁移一次，得到 v2 no-transaction record + incarnation 前进；
- v2 Config PREPARED：重启不得 abort，必须进入 Config owner 的 resume state；
- v2 Rekey PREPARED：M13 未开启时 fail-closed，绝不转 legacy abort；
- v1/v2 schema、CRC、双槽 torn-write、伪造 provenance、generic replay/legacy abort 交叉矩阵；
- Full/Lite/Nano、Sanitizer/Analyzer 的相关定向矩阵通过。

## 4. 连续实施顺序

| 顺序 | 子项 | 实现边界 | 单项自审最低证据 |
|---|---|---|---|
| 07-00 | Record v2 provenance | 先区分 legacy/current PREPARED；不开放新 Config hook | schema/migration/restart/双槽负例 |
| 07-01 | Config state | 固定 `config_id/phase/C_old/C_new/hash`；Stable 归一 | canonical hash/serialize/invalid state |
| 07-02 | 单一 config_tx | 一次仅一个 transaction，静态 bounded storage | concurrent add/remove 不覆盖 |
| 07-03 | Addition | provisional 形成 C_new，self vote | Commit 前无 voting |
| 07-04 | Removal | REMOVING 仍保留 C_old denominator | 双失联不降 quorum |
| 07-05 | Joint quorum | `quorum(old) && quorum(new)` | 双向缺 quorum 拒绝 |
| 07-06 | Persistence | Config prepare/ack/commit persist-before-promise | I/O failure 不 ACK/Commit |
| 07-07 | Backup staging | Backup C_new mirror 和 HA 标识 | 无 Backup 不伪称 HA |
| 07-08 | Joint | durable Joint 后进入 CONFIG_JOINT | Head reset / Backup 恢复 |
| 07-09 | Commit / Abort | C_new commit 或 C_old abort，幂等 | duplicate/replay/timeout |
| 07-10 | Serial | config ID checked-next，阈值 fail-closed | threshold/no wrap |
| 07-11 | Crash matrix | 所有 persist edge / dual-slot recovery | 只恢复一个 Active Config |
| 07-12 | 删除 bridge | 全部 voter 变化走 config_tx | production/test 扫描与反例 |

## 5. 自审与最终移交

每一个子项完成后必须：

1. 对照本计划、任务表、RFC4 与实际源码做一次范围自审；
2. 执行本项定向正向/负向/无写回测试；
3. 扫描 production `ucn_cluster`、Adapter、Link，确认没有越界 v4 接线；
4. 在 `docs/` 新增自审报告，在 `01-项目操作记录.md` 追加记录，并同步任务表；
5. 遇到 P0/P1/P2 不进入下一项，先登记并修复。

`07-12` 后再执行 Full/Lite/Nano、产品配置、Service OFF、Sanitizer/Analyzer、配置事务并发、双 quorum、重启/双槽掉电、消息重放、超时 Abort、Bridge 删除以及 v3/v4 隔离的全量自审。此报告和全量自审结论共同构成外部审计输入；在外审 GO 前，M07 仍为 `AUDIT HOLD`。
