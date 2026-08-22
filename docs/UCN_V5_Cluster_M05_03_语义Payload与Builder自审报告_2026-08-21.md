# UCN V5 Cluster M05-03：语义 Payload 与 Builder 自审报告

- 日期：2026-08-21
- 范围：`CLV2-05-03`
- 结论：**CODE COMPLETE / AUDIT HOLD（待独立审计）**

## 1. 实现结果

新增 private header `src/extended/cluster/ucn_cluster_wire_v4_semantic.h`。它定义：

```text
semantic message
├── Common Header: type / role / flags / cluster_id / term / head_node_id
└── tagged payload union
    └── Type 1..33 的明确字段结构
```

`ucn_cluster_wire_v4_semantic_from_frame()` 只接受已经通过既有 raw structural gate 的 40 B v4 frame，并按 Type 显式读取字段。`ucn_cluster_wire_v4_semantic_to_frame()` 先清零临时 raw frame，再按当前 `header.type` 显式写入对应字段，最后调用 `ucn_cluster_wire_v4_frame_is_valid()`；任何失败都不写调用方 output。

因此 private semantic layer 不再携带通用 `words[6]` field bag；所有非 active Type 的 union storage 都不会被 builder 读取。private payload union 受编译期断言限制为最多 `6 × u32 = 24 B`，只作为调用方临时栈对象，不嵌入 `ucn_cluster_t`。

## 2. 自审要点

| 检查项 | 结果 |
|---|---|
| RFC4 raw 40 B / Type / 字节布局 | PASS；没有修改 raw decoder、encoder 格式或 frozen vectors。 |
| Type 1..33 显式 mapping | PASS；`from_frame`、`to_frame` 均为逐 Type switch，无 fallback memcpy。 |
| 无关字段污染 | PASS；按 active payload size 对 union tail 写入 `0xA5` 后，重建 raw frame 仍逐字节等于原 frame。 |
| 零字段 / marker | PASS；Type 12 `SYNC_BEGIN` marker 的 `P3..P5=0` 经 semantic builder 回环保持。 |
| 失败无副作用 | PASS；非法 semantic header 与非法 raw frame 均不写 output。 |
| 私有边界 | PASS；CMake 仅把 private include path 给 `ucn_cluster_wire_v4_codec_tests`；复扫确认 `src/extended/ucn_cluster.c` 无 semantic/v4 builder 调用。 |
| 常驻 RAM | PASS；无 `ucn_cluster_t` 字段、无动态分配、payload union 编译期上限 24 B。 |

## 3. 测试证据

| 门禁 | 结果 |
|---|---|
| Windows GCC Full | `28/28` CTest PASS |
| Windows GCC Lite | `28/28` CTest PASS |
| Windows GCC Nano | `18/18` CTest PASS |
| Windows MSVC Debug Full | `15/15` CTest PASS |
| WSL Full ASan/UBSan | `18/18` CTest PASS |
| WSL Full GCC `-fanalyzer -Wall -Wextra -Werror` | `5/5` CTest PASS |
| 空白检查 | `git diff --check` 无空白错误（仅既有 CRLF 提示） |

`ucn_cluster_wire_v4_codec_tests` 新增或扩展覆盖：

1. 所有 Type `1..33` 的 raw → semantic → raw 精确回环；
2. 9 条冻结 RFC4 vector 的 decode → semantic → raw 与原始字节一致；
3. 存在 inactive payload tail 的 Type，其 tail 污染不影响结果；
4. Type 12 marker 零尾字段保持；
5. 非法 raw / 非法 semantic input 的 output 不变。

## 4. 未实现与审计边界

本项不让任何真实帧进入 Cluster FSM：生产 public dispatch 的 v4 arm 仍是 raw frame，encoder 仍 default-disabled。没有实现或接线：生产 RX owner、source/replay/Config admission、证书 quorum/CRC 业务验证、Capability/混合版本、40 B Adapter、Config/Handover/Rekey FSM，或任何硬件/掉电验证。

因此本报告不是生产 v4 协议签字；`CLV2-05-03` 需要独立审计，M05 整体继续 **AUDIT HOLD**。
