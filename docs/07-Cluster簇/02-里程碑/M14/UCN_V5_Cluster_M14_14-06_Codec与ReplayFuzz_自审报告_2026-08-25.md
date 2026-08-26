# UCN V5 Cluster M14 / 14-06 Codec 与 Replay Fuzz 自审报告

> 日期：2026-08-25  
> 状态：`CODE COMPLETE / SELF-AUDIT PASS / WAIT M14 EXTERNAL REVIEW`

## 1. 覆盖范围

14-06 使用三层固定 seed 门禁，不依赖不可复现的随机输入：

1. v3 codec/生产 RX：19 类合法 seed，20000 次 1..4 bit mutation、错误长度与真实 `ucn_cluster_receive()` 状态推进；只接受协议声明的返回集合。
2. v4 raw/semantic/dual dispatcher：33 类 Type 的独立字段合同矩阵、4096 次 raw mutation、39/40/41 B 长度、Debug 与 O1/O2/O3 全部执行。
3. v4 Certificate pending：新增固定 seed `0x14F00D06` 的 65536 步长序列，持续组合 begin、duplicate、fragment、source/config/key mismatch、deadline、显式 expiry 与 Active Epoch change。

## 2. Stateful oracle

每一步均核对：

- source/admission/key/config 不匹配在 `now == deadline` 时仍保持完整 slot、deadline、mask 和 bitmap 不变；
- 同 key duplicate 不延长 deadline；不同 key 在占用期 fail-closed；
- 合法 fragment 只设置声明范围内的 mask/bitmap；count、mask、Config ID 互相一致；
- 只有全部必需 set 的 fragment 齐全时 `pending_has_all_fragments()` 才成立；
- timer expiry、合法 fragment 到期和 Active Epoch change 清为 canonical zero slot；
- codec-only pending 对象不连接 Authority、生产 RX/TX 或 FSM。

## 3. 自审结论

本项没有修改协议 codec 或 pending 实现；新增测试在现有实现上直接通过。检查确认 v4 随机输入成功时仍必须经过 raw validator 与 semantic parser，失败时 output 不写回。14-06 的结论只说明给定固定状态空间内未发现 crash、OOB、错误写回或 pending 越权，不等价于无限输入形式化证明。

## 4. 验证

- Windows GCC Full（含 M08..M13 实验目标）：48/48 PASS；
- v4 codec Debug/O1/O2/O3：4/4 PASS；
- WSL ASan/UBSan Full：44/44 PASS，`detect_leaks=1`；
- `git diff --check`：无空白错误，仅既有 CRLF 提示。

## 5. 边界

M05 顶层 `AUDIT HOLD` 不解除；没有启用 production v4 encoder、RX/TX/FSM 或 Authority。实机、真实 Bearer 和 MCU 资源仍分别属于 14-08/14-09。
