# UCN V5 Cluster M05-04：v4 Snapshot 语义实施计划

- 日期：2026-08-21
- 状态：`CODE COMPLETE / AUDIT HOLD（待独立审计）`
- 前置：`CLV2-05-01`、`CLV2-05-02`、`CLV2-05-03` 已获受限范围外部 GO

## 1. 目标

RFC4 的 Type 12 `BACKUP_MEMBER_SYNC` 已冻结为 40 B：Common Header 是完整
Epoch，`P0..P5` 依次为 backup generation、snapshot ID、membership sequence、
member ID、member nonce、member lease。05-04 不改变任何 Wire 字节；它在
private codec 层增加一个完整 Snapshot 对象与转换 helper，使调用者不能只传
成员字段而遗漏 Epoch、generation、snapshot 或 sequence。

```text
private Snapshot
  = Epoch(cluster_id, term, head_node_id)
  + backup_generation + snapshot_id + membership_sequence
  + kind(record / begin / end / delta)
  + member_id + member_nonce + member_lease_ms
  <-> private semantic Type 12
  <-> validated raw RFC4 40 B frame
```

`BEGIN`/`END` 是 marker：成员三元组必须为零；`RECORD`/`DELTA` 是成员记录：
成员 ID、nonce 与 lease 必须有效。所有写出路径均由既有 raw structural gate
最终裁决，失败不得写 caller output。

## 2. 边界

- 只改 `src/extended/cluster/` 的 private semantic codec 与定向测试。
- 不改 RFC4、format/type/flag 编号、网络字节序、v3/v4 分派或 public header。
- 不改生产 `src/extended/ucn_cluster.c`，不接 RX/TX/FSM，v4 encoder 继续
  default-disabled。
- 不新增动态内存，不把 Snapshot 对象嵌入 `ucn_cluster_t`。
- `UINT32_MAX-1` 仅用于协议允许的 ID/nonce 边界；generation、snapshot ID、
  sequence 与 term 仍遵守 M03 serial rotation threshold，lease 仍遵守
  `UCN_MAX_SAFE_DURATION_MS`。这不是放宽 serial/duration 合法域。

## 3. 执行任务

| 子项 | 内容 | 验收 |
|---|---|---|
| 04-01 | **完成**：新增 private `snapshot_kind` 与完整 Snapshot 对象。 | 固定大小、显式 Epoch，零值 kind 无效。 |
| 04-02 | **完成**：实现 Snapshot ↔ Type 12 semantic/raw helper。 | 只接受 Type 12；marker/record/delta flags 与成员字段一致；失败不写 output。 |
| 04-03 | **完成**：增加边界和负向回归。 | `UINT32_MAX-1` ID/nonce、serial threshold、最大安全 lease 精确回环；超域 serial/duration、错 marker、错 Type 拒绝。 |
| 04-04 | **完成（软件自审）**：自审隔离边界、构建矩阵、静态分析和文档一致性。 | 证明无生产调用；M05 继续 `AUDIT HOLD`，等待独立审计。 |

## 4. 非目标

本项不实现 Snapshot 接收状态机、序列推进、丢帧重传、Backup READY、Takeover
或任何 Authority 副作用。这些属于后续生产 RX owner/FSM 与 M07/M10/M11 等
里程碑，不能因本 helper 存在而提前开启。
