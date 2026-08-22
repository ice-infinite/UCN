# UCN V5 Cluster M05-06：v4 Type 注册表 / Parser 一致性核对计划

- 日期：2026-08-21
- 状态：`CODE COMPLETE / AUDIT HOLD（待独立审计）`
- 前置：`CLV2-05-01` 至 `CLV2-05-05` 均已获受限范围外部 GO

## 1. 目标

RFC4 已冻结 Type `1..33`。本项不重新设计 Message Type，而是验证并钉住 Type
`20..33` 的唯一注册表，确保 public enum、private semantic payload/parser、raw
structural gate 与 RFC4 Type 表没有发生编号、Role、flags、payload tail 或字段所有权
漂移。

```text
RFC4 Type table
   == public raw enum (fixed numeric IDs)
   == private semantic payload / from_frame / to_frame switch
   == raw structural gate
   == focused immutable registry tests
```

## 2. 严格边界

- 只审查/补强 isolated v4 codec、private semantic header、focused codec test 与文档。
- Type 编号、40 B 字节布局、RFC4 语义和公共接口均冻结；发现不一致时优先修正实现或
  测试，不能借本项修改 RFC 字节。
- 不实现 Config、Handover、Rekey、Certificate 业务 FSM，不创建 Authority，不接入
  `src/extended/ucn_cluster.c`、RX/TX/Adapter 或真实 RX owner。
- v4 encoder 继续 default-disabled；不新增动态内存或 `ucn_cluster_t` 字段。

## 3. 核对任务

| 子项 | 内容 | 验收 |
|---|---|---|
| 06-01 | **完成**：审计 RFC4 Type 20..33 与 public enum 的数值和名称。 | `CONFIG_BEGIN=20` 至 `TAKEOVER_CERTIFICATE=33` 连续、唯一、不可复用。 |
| 06-02 | **完成**：审计 semantic `from_frame()` / `to_frame()` 以及 payload-size table。 | 每 Type 都有唯一 named payload arm；P0..P5 与 RFC 表逐项对应。 |
| 06-03 | **完成**：审计 raw structural gate：role、flags、serial/ID/nonce/duration 域、零 tail。 | 所有合法 Type 可构造；非法 role/flag/unused tail 被拒绝且输出不写回。 |
| 06-04 | **完成**：新增不可变注册表回归。 | 固定数值、payload words、zero-tail 与负向 role/flag/tail 反例形成门禁。 |
| 06-05 | **完成（软件自审）**：自审、构建矩阵、生产边界扫描和文档同步。 | 仅 codec/test 引用；M05 继续 `AUDIT HOLD`，待外审前不开始 05-07。 |

## 4. 非目标

本项只钉住 parser/codec 的字段表达，并不证明 Config/Handover/Rekey 事务合法、持久化
正确或可运行，也不验证 Certificate CRC/quorum。后续 M07/M10/M11/M13 仍拥有对应业务
语义与 Authority 决定权。
