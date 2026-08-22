# UCN V5 Cluster M05-05：v4 Takeover / Certificate 语义自审报告

- 日期：2026-08-21
- 状态：`CODE COMPLETE / AUDIT HOLD（待独立审计）`
- 范围：仅 `CLV2-05-05` 的 private Type 8 / Type 33 semantic helper。

## 1. 结论

本项把冻结 RFC4 中分散的 Type 8 `HEAD_TAKEOVER` 和 Type 33
`TAKEOVER_CERTIFICATE` 收敛为两个完整的、固定大小的 private 对象：

| 对象 | 一次性绑定的字段 |
|---|---|
| `ucn_cluster_wire_v4_takeover_t` | proposed `cluster_id/term/head`、backup generation、snapshot、anchor Config、takeover txid、required set、certificate CRC carrier |
| `ucn_cluster_wire_v4_takeover_fragment_t` | 同一 proposed Epoch、generation、snapshot、fragment Config、txid、OLD/NEW set、fragment index/count、bitmap word |

转换链是：

```text
private Takeover / CertificateFragment
  <-> private semantic Type 8 / Type 33
  <-> existing validated RFC4 raw 40 B frame
```

所有写出先通过既有 `semantic_to_frame()` 和 raw structural gate；所有读取先通过
`semantic_from_frame()`。错误 Type、非法 epoch/serial/descriptor/set 或局部字段都不
写调用方输出。新的 `fragment_matches_admission()` 只判断 RFC4 carrier key 与 frozen
Config admission 的关系：outer source 必须等于 proposed Head；Stable 只能是 anchor
对应的 `C_old`；Joint 的 anchor 必须是 `C_new`，OLD/NEW fragments 分别匹配
`C_old/C_new`。

## 2. 明确未实现的安全判断

05-05 **不**把数据载体误当作有效 Certificate，也不产生 Authority。它没有计算或接受：

- canonical Certificate CRC32；
- frozen voter order、bitmap 的超界 bit 或 VoteId；
- Stable/Joint quorum；
- persistence、Head promotion、Cluster FSM 或真实 RX owner。

这些是 M10/M08 以及随后生产接线的职责。现有 one-slot pending cache 只作为“是否收齐
required fragment set”的无 Authority 测试载体；后续 RX owner 仍须先做 transport、source
和 frozen Config 准入。

## 3. 自审覆盖

定向回归实际覆盖：

1. Type 8 的完整 proposed Epoch 和 `P0..P5` round-trip；Type 33 的完整 common key、
   Config、set、descriptor 和 bitmap round-trip。
2. Stable 两片证书：第一片后 `pending_has_all_fragments()==false`，精确第二片后才为
   `true`。
3. Joint 证书：Type 8 anchor=`C_new`，OLD/NEW fragments 各自匹配 admission 的
   `C_old/C_new`，仅双 set 都到齐才 complete。
4. 伪造 term、backup generation、snapshot、txid、Config 或 set 都不能通过 private
   admission relation；送入已有 pending cache 的 key/Config/set 反例不改变 slot/mask。
5. fragment count 越界、invalid set、缺失 term、错误 Type 均拒绝，输出保持原样。

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

范围复扫确认：新 API 只出现在 private semantic header、isolated codec 和
`test_cluster_wire_v4_codec.c`；`src/extended/ucn_cluster.c` 没有任何
`ucn_cluster_wire_v4` 调用。public v4 encoder 仍 default-disabled。

## 5. 交付边界

因此 `CLV2-05-05` 仅可提交独立审计。M05 仍是 **AUDIT HOLD**：不得由本项推断 v4
已可接收、发送、重组后授权、完成 Certificate、提升 Head 或进入任何生产 FSM。未创建
提交或推送，且未开始 05-06。
