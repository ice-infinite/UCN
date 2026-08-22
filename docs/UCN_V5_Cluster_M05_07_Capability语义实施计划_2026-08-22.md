# UCN V5 Cluster M05-07：v4 Capability / Wire Offer 语义实施计划

- 日期：2026-08-22
- 状态：`CODE COMPLETE / AUDIT HOLD（待独立审计）`
- 前置：05-01 至 05-05 已获受限范围外部 GO；05-06 已完成自审、仍待外审
- 授权：用户于 2026-08-22 指示“继续”；本项与 05-06 外审并行，但不越过生产边界。

## 1. 目标

RFC4 的 `wire_offer` 已占用现有字段：ADVERTISE.P3、JOIN_REQUEST.P1 与
HEAD_DECLARE.P3 为
`{min_format, max_format, capability_bitmap}`，JOIN_ACCEPT.P4 为
`{selected_format, selected_capability_bitmap}`。本项在 private codec 层把这些裸
`uint32_t` 收敛为明确类型和无状态 helper：

```text
private WireOffer(min_format, max_format, capabilities)
  <-> RFC4 word (ADVERTISE.P3 / JOIN_REQUEST.P1 / HEAD_DECLARE.P3)

private SelectedWireOffer(format, capabilities)
  <-> RFC4 word (JOIN_ACCEPT.P4)

two validated offers + required bits
  -> deterministic common format + common capabilities
```

六个冻结 capability bit 为 BACKUP、TAKEOVER、JOINT_CONFIG、PERSISTENCE、
RECOVERY_LINEAGE、REKEY；bits 6..15 和所有其他保留位必须 fail-closed。

## 2. 严格边界

- 只改 private semantic header、isolated codec、focused codec test 与文档。
- 不改 RFC4 字节、公共 header、`wire_offer` bit 布局、v3/v4 分派或 public pending API。
- Helper 只说明“某 offer 是否具备 required bits”和“两 offer 的共同支持集”；不依据
  score、member 状态、Config、持久化或当前角色作决定。
- 不接入 Head/Backup 选择、JOIN、Advertise、生产 RX/TX/FSM、Adapter 或 encoder；真正
  eligibility 留给 M06/M08 及其 future FSM gate。
- 不动态分配，不把 typed offer 嵌入 `ucn_cluster_t`。

## 3. 执行任务

| 子项 | 内容 | 验收 |
|---|---|---|
| 07-01 | **DONE**：定义 private capability bit enum、WireOffer 与 SelectedWireOffer 值对象。 | 六位精确、对象固定小于 raw word。 |
| 07-02 | **DONE**：实现严格 word encode/decode。 | min/max、format、reserved bits、unknown cap bit 失败且 output 不写回。 |
| 07-03 | **DONE**：实现 pure requirement/intersection helper。 | 只在 range 相交且 required bits 同时具备时写出最高共同 format 与共同 bits；RFC4 合法 offer 都必须包含 format 4，故合法输入必然至少在 4 相交。 |
| 07-04 | **DONE**：增加 ADVERTISE/JOIN/HEAD_DECLARE/JOIN_ACCEPT 对应 word、边界与负向回归。 | source semantic word 不漂移；cap 缺失、保留位、错误 selected word、非 RFC4 range 均拒绝。 |
| 07-05 | **DONE**：自审、构建与生产边界复扫。 | M05 保持 AUDIT HOLD；不开始 05-08。 |

## 4. 非目标

本项不建立 capability negotiation 状态机，不选择 Head/Backup，不决定 voter eligibility，
不发送或接收任何 production v4 frame，也不改变当前 v3 实机协议。
