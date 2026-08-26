# UCN V5 Cluster M05-05：v4 Takeover / Certificate 语义实施计划

- 日期：2026-08-21
- 状态：`CODE COMPLETE / AUDIT HOLD（待独立审计）`
- 前置：`CLV2-05-01`、`CLV2-05-02`、`CLV2-05-03`、`CLV2-05-04` 已获受限范围外部 GO

## 1. 目标

RFC4 的 Type 8 `HEAD_TAKEOVER` 与 Type 33 `TAKEOVER_CERTIFICATE` 共同表达新的 proposed Epoch 和可分片 Vote Certificate。05-05 不改任何 Wire 字节，而是在 private codec 层建立两个不能省略绑定字段的固定大小对象：

```text
private Takeover
  = proposed Epoch(cluster_id, term, head_node_id)
  + backup_generation + snapshot_id + certificate_anchor_config_id
  + takeover_txid + required_set_mask + certificate_crc32
  <-> private semantic Type 8 <-> validated RFC4 40 B raw frame

private CertificateFragment
  = same proposed Epoch + backup_generation + snapshot_id
  + fragment config_id + takeover_txid + OLD/NEW set
  + fragment index/count + vote bitmap word
  <-> private semantic Type 33 <-> validated RFC4 40 B raw frame
```

Stable Certificate 固定使用 `required_set_mask=OLD`，anchor 对应 `C_old`；Joint Certificate 固定使用 `OLD|NEW`，anchor 对应 `C_new`。对象间关联必须使用已有的 receiver-side frozen-Config admission context：Stable 的 OLD fragment 绑定 `C_old`；Joint 的 OLD/NEW fragment 分别绑定 `C_old/C_new`。这让后续 M10 能在不猜测字段含义的条件下验证完整证书。

## 2. 严格边界

- 只改 `src/extended/cluster/` 下的 private semantic codec 与 `test_cluster_wire_v4_codec.c` 的定向测试。
- 不修改冻结的 `UCN_Cluster_Wire_v4.md`、任何 v4 字节布局、公共 header、v3/v4 分派或现有 public pending-cache 合同。
- 不接入 `src/extended/ucn_cluster.c`、生产 RX/TX/FSM、Adapter 或真实 RX owner；`UCN_CLUSTER_WIRE_V4_ENCODER_ENABLED` 继续 default-disabled。
- 不计算或接受 Certificate CRC、确定性 voter order、VoteId、bitmap quorum、持久化或 Authority；它们属于 M10/M08 以及后续 FSM。05-05 只证明数据载体与 frozen Config 准入关系可完整表达。
- 不动态分配、不把对象嵌入 `ucn_cluster_t`；对象必须有编译期 bounded-size 断言。

## 3. 执行任务

| 子项 | 内容 | 验收 |
|---|---|---|
| 05-05-01 | **完成**：新增 private Takeover 与 CertificateFragment 对象、set enum 与大小断言。 | proposed Epoch、generation、snapshot、Config、txid、required mask/CRC、descriptor/bitmap 无隐式缺失。 |
| 05-05-02 | **完成**：实现 Type 8/33 各自的 `from_frame()` / `to_frame()`。 | 只接受准确 Type；写出先经过 semantic/raw structural gate；失败不写 output。 |
| 05-05-03 | **完成**：实现无状态的 Takeover/fragment/admission 关联检查。 | Stable/Joint `C_old/C_new`、outer source、Epoch/key/set 关系精确；不创建 pending 或 Authority。 |
| 05-05-04 | **完成**：增加 Stable/Joint 正向、伪造/缺片/越界 descriptor 与 output/slot 无副作用回归。 | 只有完整 required sets 才被 cache 报告为 complete；非法候选不能占用或破坏已有 slot。 |
| 05-05-05 | **完成（软件自审）**：自审并运行受影响矩阵、生产隔离扫描与文档核对。 | M05 继续 `AUDIT HOLD`，待独立审计签署前不进入 05-06。 |

## 4. 非目标

本项不是 Takeover 接收状态机，也不授权新 Head、计算 CRC 或票数。它不能把一个 Type 8 或一组 Type 33 转变为 Authority。后续真实 RX owner 必须先完成 transport/source/frozen Config 准入，再调用现有 pending cache；M10 才能以 frozen voter set 验证 CRC、VoteId 与 Stable/Joint quorum。
