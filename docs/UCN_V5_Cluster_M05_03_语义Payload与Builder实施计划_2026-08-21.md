# UCN V5 Cluster M05-03：语义 Payload 与 Builder 实施计划

- 日期：2026-08-21
- 状态：`CODE COMPLETE / AUDIT HOLD（待独立审计）`
- 依赖：`CLV2-05-01`、`CLV2-05-02` 已获受限范围外部 GO

## 1. 本项目标

现有 v4 codec 的 `ucn_cluster_wire_v4_frame_t` 是正确的固定 40 B raw container，但其 `words[0..5]` 不表达 Type 的字段所有权。本项在 **codec 内部**增加 tagged semantic message：公共 Header 加上 Type-specific payload union，并建立两条无副作用的转换：

```text
validated raw frame -> semantic message
semantic message -> zeroed raw frame -> existing raw structural validator
```

每条 builder 只读取/写入当前 Type 的规范字段。它不接收 transport、source、ConfigState、persistence 或 FSM 状态。

## 2. 固定边界

- 不修改 RFC4、40 B 固定布局、Type 编号、网络字节序或 v3/v4 严格分派。
- 不修改 `UCN_CLUSTER_FORMAT_VERSION=3`、生产 `src/extended/ucn_cluster.c`，不接入 RX/TX/FSM。
- 生产 v4 encoder 继续 default-disabled；builder 只构造内存中的 raw frame，不发送。
- 不实现 M05-04+ 的 Snapshot 语义、Capability/FSM 准入、Adapter 40 B 迁移、quorum/CRC 或 Handover/Rekey 状态机。
- 不新增动态内存；semantic message 由调用方栈或固定存储提供。

## 3. 执行任务

| 子项 | 内容 | 验收 |
|---|---|---|
| 03-01 | **完成**：新增 private semantic header：Header、33 Type 的 payload struct（可复用相同布局但保留 Type 分支）和 tagged union。 | public API/当前 v3 ABI 零变化。 |
| 03-02 | **完成**：实现 `from_frame` / `to_frame` builder；`to_frame` 先清零 raw frame，仅写 active payload，再调用既有 `frame_is_valid`。 | 任意无关 union tail 脏值不能出现在 raw frame。 |
| 03-03 | **完成**：扩展 codec test：Type 1..33 全量 semantic round-trip、inactive storage 污染、非法语义 payload output 不写回。 | 所有 raw golden 仍字节相同；失败无输出副作用。 |
| 03-04 | **完成（软件自审）**：完成 Full/Lite/Nano、MSVC、ASan/UBSan、`-fanalyzer` 与生产接线复扫。 | 软件证据齐全，但仍不等于生产 v4；等待独立审计。 |

## 4. 自审门禁

1. semantic union 内不含 `words[6]` 或任何 generic field bag；Type 是唯一 discriminant。
2. `to_frame` 的 failure 不写 caller output；`from_frame` 只接受已通过 raw structural gate 的输入。
3. 所有 Type 1..33 都有显式 payload mapping；不能以 fallback memcpy 代替。
4. `rg` 证明新增 API 不被 `src/extended/ucn_cluster.c` 或生产 TX/RX/FSM 调用。
5. M05 整体状态在本项完成后仍为 `AUDIT HOLD`，等待独立审计。
