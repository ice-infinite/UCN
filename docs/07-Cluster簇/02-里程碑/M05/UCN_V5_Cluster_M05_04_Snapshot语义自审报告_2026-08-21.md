# UCN V5 Cluster M05-04：v4 Snapshot 语义自审报告

- 日期：2026-08-21
- 状态：`CODE COMPLETE / AUDIT HOLD（待独立审计）`
- 范围：仅 `CLV2-05-04` 的 private Type 12 Snapshot semantic helper。

## 1. 结论

本项已在隔离 v4 codec 内完成。Snapshot 不再以零散 member 字段表达，而是使用
固定大小的 private `ucn_cluster_wire_v4_snapshot_t` 同时携带：

| Snapshot 字段 | RFC4 Type 12 位置 |
|---|---|
| `cluster_id`、`term`、`head_node_id` | Common Header 的完整 Epoch |
| `backup_generation`、`snapshot_id`、`membership_sequence` | `P0`、`P1`、`P2` |
| `member_node_id`、`member_nonce`、`member_lease_ms` | `P3`、`P4`、`P5` |
| `kind` | Type 12 的 record / BEGIN / END / DELTA flag |

转换链固定为：

```text
private Snapshot <-> private semantic Type 12 <-> validated raw RFC4 frame
```

`snapshot_to_frame()` 固定 Type 12 与 HEAD role，写入完整 Epoch 和全部 `P0..P5`，
最后调用既有 `semantic_to_frame()`/raw structural gate。`snapshot_from_frame()` 先走
既有 raw→semantic gate，再拒绝非 Type 12。两个 API 在失败时都不写 caller output。

## 2. 安全与资源边界

- `BEGIN`/`END` 只能携带零 member tail；`MEMBER`/`DELTA` 必须携带有效 member ID、
  nonce 与 lease。
- cluster ID、head ID、member ID 与 nonce 可以精确保存合法边界
  `UINT32_MAX-1`；Term、generation、snapshot ID、sequence 仍不超过
  `UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD`，lease 不超过
  `UCN_MAX_SAFE_DURATION_MS`。没有借本项放宽 M03 no-wrap 或时间安全域。
- Snapshot 是 private transient object，不在 public include path、不嵌入
  `ucn_cluster_t`、不动态分配；编译期断言其大小不超过 40 B raw v4 frame。
- 搜索确认 Snapshot API 仅出现在 private semantic header、isolated codec 与
  `test_cluster_wire_v4_codec.c`；`src/extended/ucn_cluster.c` 没有调用。

## 3. 回归覆盖

定向测试覆盖：

1. 完整 Epoch + generation/snapshot/sequence/member 三元组的 round-trip；
2. `UINT32_MAX-1` 合法 ID/nonce、serial threshold、最大安全 lease 的精确保留；
3. BEGIN/END marker 的零 tail、DELTA flag、marker raw 非零 tail 拒绝；
4. cluster ID、term、head ID 任一缺失时拒绝；超域 serial/duration、`INVALID` kind、
   非 Type 12 输入均拒绝且 output 不变。

本轮最终工作区验证：

| 门禁 | 结果 |
|---|---|
| Windows GCC Full | `28/28` CTest PASS |
| Windows GCC Lite | `28/28` CTest PASS |
| Windows GCC Nano | `18/18` CTest PASS |
| Windows MSVC Debug Full | `15/15` CTest PASS（仅既有 C4819 编码警告） |
| WSL Full ASan/UBSan | `18/18` CTest PASS |
| WSL GCC `-fanalyzer -Wall -Wextra -Werror` | `5/5` CTest PASS |
| 空白检查 | `git diff --check` 无空白错误（仅既有 CRLF 提示） |

## 4. 非目标与交付边界

本 helper 不保存 Snapshot、推进 sequence、处理重传、发送 `BACKUP_READY`，也不会调用
任何 Cluster RX/TX/FSM。它不授权 v4 production encoder、真实 RX owner、Adapter 40 B
迁移、证书 quorum/CRC、Capability、Config、Takeover、Handover 或 Rekey 逻辑。

因此 `CLV2-05-04` 仅可送独立审计；M05 整体继续 **AUDIT HOLD**。
