# UCN V5 Cluster M05-07：Capability / Wire Offer 私有语义自审报告

- 日期：2026-08-22
- 范围：`CLV2-05-07`
- 结论：`CODE COMPLETE / AUDIT HOLD（待独立审计）`
- 基线：RFC4 冻结不变；当前工作区含其他尚未提交的 M03/M04/M05 改动，本报告只签 05-07 新增的 private helper 与定向回归。

## 1. 实现核对

| 核对项 | 结果 | 证据 |
|---|---|---|
| 能力位 | PASS | private enum 固定 BACKUP、TAKEOVER、JOINT_CONFIG、PERSISTENCE、RECOVERY_LINEAGE、REKEY 六位；`0x003F` 外的 capability bits 被拒绝。 |
| 值对象 | PASS | `WireOffer { minimum_format, maximum_format, capabilities }` 与 `SelectedWireOffer { format, capabilities }` 均为 private transient value，且有不大于 raw word 的编译期上限。 |
| raw 编解码 | PASS | `wire_offer` 与 `selected_wire_offer` 的双向转换复用 RFC4 structural validator；失败路径不写 caller output。 |
| 选择规则 | PASS | 只接受双方均为合法 RFC4 offer、required bits 合法且被双方满足的输入；结果取双方共同 capability 集和最高共同 format。合法 RFC4 offer 都必须包含 format 4，因此合法输入的 range 必定有交集。 |
| 字段归属 | PASS | 回归固定 ADVERTISE.P3、JOIN_REQUEST.P1、HEAD_DECLARE.P3 使用 `wire_offer`，JOIN_ACCEPT.P4 使用 `selected_wire_offer`；没有改写任何 RFC4 wire byte。 |

## 2. 自审中修正的文档/覆盖缺口

初始实施计划只列出 ADVERTISE 与 JOIN_REQUEST 两个 `wire_offer` 字段。复读 RFC4 后确认
HEAD_DECLARE.P3 同样使用该编码。本轮补充该字段的回归，并同步任务表、计划和操作记录。

计划中“合法 offer 无交集”也已更正：RFC4 要求每个合法 v4 offer 的 range 都包含
format 4，所以该条件不可能由合法输入触发；实现仍保留 defensive `no common range`
分支，非法 range 在 word validator 阶段以 `UCN_ERR_ARGUMENT` 拒绝。

## 3. 负向与边界回归

- required capability 缺失返回 `UCN_ERR_STATE`，selected output 保持原样；未知 required bit 返回 `UCN_ERR_ARGUMENT`。
- 最小 format 不包含 4、unknown capability bit、selected word 高保留 byte 非零、selected format 为零均拒绝，所有 output 不写回。
- raw word `0x04060003` 与 selected word `0x00050001` 进行精确往返。
- 四个 RFC4 field owner 的 raw word 保持既有 layout。

## 4. 生产边界核对

- `ucn_cluster_wire_v4_wire_offer*` 只存在于 isolated codec、private semantic header 和 focused codec test；`src/extended/ucn_cluster.c` 没有调用。
- `UCN_CLUSTER_WIRE_V4_ENCODER_ENABLED` 仍为默认 `0`；没有启用生产 v4 send。
- 没有变更 public header、Cluster object、Adapter、Head/Backup 选择、JOIN、RX/TX owner、FSM、Authority、quorum 或 persistence。

因此这只能提供能力语义的无状态输入，不构成任何节点资格、信任、Authority 或 mixed-version 策略。上述接线仍留给 M06/M08 和后续独立审计授权。

## 5. 已执行验证

| 环境 | 结果 |
|---|---|
| Windows GCC Full | CTest `28/28` PASS |
| Windows GCC Lite | CTest `28/28` PASS |
| Windows GCC Nano | CTest `18/18` PASS |
| Windows MSVC Debug Full | CTest `15/15` PASS；仅既有 C4819 编码警告 |
| WSL ASan/UBSan | CTest `18/18` PASS |
| WSL GCC `-fanalyzer -Wall -Wextra -Werror` | CTest `5/5` PASS |
| 格式检查 | `git diff --check` 无空白错误；仅既有 CRLF 提示 |

## 6. 送外审范围

请独立核对：capability 位和值对象是否与 RFC4 一致；所有四个 field owner 是否覆盖；失败无写回；合法 offer 的 format-4 交集不变量；以及是否存在任何 production RX/TX/FSM/encoder 接线。本报告不请求 M05 总体放行。
