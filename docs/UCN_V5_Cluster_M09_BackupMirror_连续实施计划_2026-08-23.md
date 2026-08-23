# UCN V5 Cluster M09 Backup 双缓冲连续实施计划（2026-08-23）

## 1. 授权范围

用户已授权启动 M09。M08 仍处于 **SELF-AUDIT PASS / WAIT EXTERNAL**，M05 总体仍为 **AUDIT HOLD**。因此 M09 只能在受限实验软件范围内推进：可新增数据模型、纯函数、production-archive 单元测试和故障模拟；不得启用 production v4 RX/TX/FSM、默认 v4 encoder、Authority 授权或 M10 Takeover Commit。

`VOLATILE_TEST` 仅能证明内存状态机，不得作为掉电、Flash 原子性或持久化安全证明。

## 2. 当前基线与目标差异

当前 Backup 复用 `primary_members` 作为唯一镜像：`SYNC_BEGIN` 会直接清空该表，刷新期间 Primary 故障会丢失最后可用镜像；`membership_sequence` 也跨 snapshot 重用。目标模型应为：

```text
Role-state union
  Head/Member: Runtime member table
  Backup:      { committed mirror, staging mirror }

SYNC_BEGIN -> 仅清 staging
SYNC_MEMBER -> 写 staging
SYNC_END(valid exact epoch/config/hash/coverage) -> atomic swap staging -> committed
```

`SnapshotEpoch = { BackupEpoch, snapshot_id }`。Snapshot 内 sequence 只表示该 snapshot 的帧顺序，不能作为跨 snapshot 永久序号。

## 3. 连续任务顺序

| 顺序 | 任务 | 本轮边界与验收 |
|---|---|---|
| 09-01 | 双缓冲状态 | committed/staging 独立、canonical reset/validity、Role-state union；不改 wire/FSM。 |
| 09-02 | SnapshotEpoch | identity 绑定 BackupEpoch、Config ref，serial 不回绕；不发送 v4。 |
| 09-03 | SYNC_BEGIN | 仅接受 `sequence=0`；只开 staging，保留 committed。 |
| 09-04 | SYNC_MEMBER/END | 固定 `MEMBER=1..N`、`END=N+1`；sequence/count/hash/nonce/config/coverage 全检后才 swap。 |
| 09-05 | READY | exact source + SnapshotEpoch + Config proof，延迟 READY 不得完成新同步。 |
| 09-06 | Delta | 只作用于 exact committed snapshot/config 的既有成员 freshness；禁止成员/资格结构变更；gap 只请求 full resync。 |
| 09-07..09 | Coverage / grace / no-wrap | 首次覆盖、仅 explicit SUSPECT 的 flap grace、REMOVED/missing immediate fence、snapshot/generation exhaustion。 |
| 09-10..11 | Capability / failure matrix | 完整 Target eligibility、`head_score DESC,node_id ASC` 筛选与每边界 Primary 故障矩阵。 |

每一项完成后至少执行：定向单元测试、Full/Lite/Nano 编译测试、失败路径 no-write/committed-preserved 检查和源码隔离扫描。M09 全部完成后再执行 ASan/UBSan、`-fanalyzer`、规模模拟与全量自审。

## 4. 不变量

1. `committed_valid` 前，任何 mirror 都不能作为 M10 Takeover 输入。
2. Snapshot 控制序列固定为 `SYNC_BEGIN=0`、`SYNC_MEMBER=1..N`、`SYNC_END=N+1`；任何乱序、缺成员、错误 hash、错误 Config 或覆盖不足只能清理/废弃 staging，绝不破坏 committed。
3. `BACKUP_READY` 必须绑定完整 SnapshotEpoch 与 Config；旧 READY 不能确认新同步。
4. Delta 不得跨 committed snapshot/config，也不得新增 member、改变 static membership/eligibility 或回退 nonce；gap 只可要求全量 resync。
5. Snapshot ID/generation 不允许回绕；达到阈值只能 generation rotate/full sync 或后续 M13 Rekey。
6. 已 committed 后，protected voter 的**显式** `SUSPECT` 可进入 grace，但 Core 确认 `REMOVED` 或 coverage 中**缺失** protected voter 都必须立即永久取消该 assignment 的 future takeover eligibility；后续 ADMITTED 不得复活。
7. M09 不实现 M10 vote/quorum/certificate/takeover 提交。

## 5. 外部审计材料

M09 最终外审须覆盖：`BEGIN=0/MEMBER=1..N/END=N+1`、刷新中 Primary 故障、准备中 Config 变化、延迟/重放 READY、Delta gap、SUSPECT flap 与 REMOVED/missing immediate fence、Stable/Joint 的 ADMITTED-after-fence no-revive、snapshot/generation exhaustion、完整 candidate eligibility/rank、任何 invalid sync 后 committed 字节不变，以及 M05 production-v4 隔离持续成立。
