# UCN V5 Cluster M05-06：v4 Type 注册表 / Parser 一致性自审报告

- 日期：2026-08-21
- 状态：`CODE COMPLETE / AUDIT HOLD（待独立审计）`
- 范围：`CLV2-05-06`；只核对冻结 RFC4 Type `20..33` 的 registry/parser。

## 1. 核对结论

RFC4、public `ucn_cluster_wire_v4_type_t`、private named semantic payload、
`semantic_from_frame()`、`semantic_to_frame()`、payload-size table 与
`ucn_cluster_wire_v4_frame_is_valid()` 的 Type `20..33` 分派已逐项一致：

| 类型范围 | 结论 |
|---|---|
| 20..25 Config | 编号、P0..P5、Type 21 ADD/REMOVE flag、Type 24/25 P5=0 一致。 |
| 26..29 Handover / Withdraw | 三个 6-word Handover payload 与 5-word Withdraw 一致；Type 27 继续按 target Cluster 同/跨关系严格分派 BACKUP/HEAD。 |
| 30..32 Rekey | successor identity、Term=1、txid/config/nonce/persistence generation 的 raw gate 与 named payload 一致；Type 31 接受 MEMBER 或 BACKUP。 |
| 33 Certificate | OLD/NEW 唯一 flag、六个 word、descriptor 域与 Type 8/33 私有 helper 的既有边界一致。 |

没有改 enum、RFC4 字节、payload 字段或 codec 业务逻辑。唯一代码改动是新建 focused
immutable registry regression，防止未来插入/改号后局部 parser 仍自洽却偏离 RFC。

## 2. 新增回归门禁

`test_extended_type_registry_contract()` 固定断言：

1. `CONFIG_BEGIN=20` 至 `TAKEOVER_CERTIFICATE=33` 的精确、连续 numeric ID；
2. 每 Type 的 named semantic payload word 数（5 或 6）；
3. Type 24、25、29 的 unused `P5` 必须为零，semantic parser 拒绝且不写 output；
4. Type 21 只接受 ADD/REMOVE，Type 33 只接受 OLD/NEW，组合或零 flag 拒绝；
5. Type 23/31 的 MEMBER/BACKUP 双角色；Type 27 的同 Cluster BACKUP 与错误 HEAD 拒绝。

现有全 Type `1..33` raw encode/decode、semantic round-trip、invalid role/flag、frozen
vector 与 strict dispatch tests 继续运行，形成与新注册表 test 的交叉覆盖。

## 3. 范围与安全边界

- 新 test 只被 `ucn_cluster_wire_v4_codec_tests` 使用；不修改 private codec/header 的
  wire 逻辑，也不新增 public API。
- `src/extended/ucn_cluster.c` 仍无任何 `ucn_cluster_wire_v4` 调用；v4 encoder 仍为
  default-disabled。
- 本项不解释 Config/Handover/Rekey 事务，不计算 Certificate CRC/quorum，不创建
  Authority，也不接生产 RX/TX/FSM。M07/M10/M11/M13 的职责未前移。

## 4. 验证结果

| 门禁 | 结果 |
|---|---|
| 定向 RFC4 codec | `ucn_cluster_wire_v4_codec_tests` PASS |
| Windows GCC Full | `28/28` CTest PASS |
| Windows GCC Lite | `28/28` CTest PASS |
| Windows GCC Nano | `18/18` CTest PASS |
| Windows MSVC Debug Full | `15/15` CTest PASS（仅既有 C4819 编码警告） |
| WSL Full ASan/UBSan | `18/18` CTest PASS |
| WSL GCC `-fanalyzer -Wall -Wextra -Werror` | `5/5` CTest PASS |
| 空白检查 | `git diff --check` 无空白错误（仅既有 CRLF 提示） |

## 5. 交付边界

`CLV2-05-06` 可提交独立审计，但 M05 仍为 **AUDIT HOLD**。本项不授权进入生产 RX/TX/FSM、
真实 RX owner、v4 发送、Takeover Authority 或后续业务里程碑；未创建提交或推送。
