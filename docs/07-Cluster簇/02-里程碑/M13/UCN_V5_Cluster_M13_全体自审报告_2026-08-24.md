# UCN V5 Cluster M13 全体自审报告

> 日期：2026-08-24
> 结论：**CODE COMPLETE / SELF-AUDIT PASS / WAIT EXTERNAL REVIEW（受限实验软件范围）**

## 1. 范围结论

`CLV2-13-01..13` 已连续实施并逐项自审。M13 提供 default-OFF Rekey owner、旧 Stable quorum、persist-before-promise、退休 Tombstone、successor materialize、no-wrap、Recovery scope 与持久 allocation-history 合同。

本结论不解除 M05 顶层 `AUDIT HOLD`：没有接入 production v4 RX/TX/FSM、默认 encoder、真实 Authority 切换或 Adapter；没有把 Host fake 当成 MCU Flash/掉电/实机完成证据。

## 2. 全局不变量复核

| 不变量 | 自审结果 |
|---|---|
| 无旧 Stable quorum 不 Rekey | PASS：voter/profile 冻结，只有 old Stable canonical bitmap 计票。 |
| history 未 durable 不 PREPARE | PASS：`ID_HISTORY_DURABLE_REQUIRED` 阻断 M04 submit；exact generation/body reload 后才推进。 |
| PREPARE 未 durable 不收 ACK | PASS：Wire build/ACK owner 均要求 exact Record v4 PREPARED。 |
| Commit 前先撤旧 Authority | PASS：commit submit 前 latch `HEAD_FENCED/REKEY_COMMIT`；失败保持 Fence。 |
| successor 与 Tombstone 原子 | PASS：同一 Record v4 next-state 写入 Epoch/Config/Rekey/Tombstone/incarnation。 |
| durable proof 不可伪造 | PASS：state canonical validation、operation journal、exact reload、history generation+fingerprint。 |
| 退休身份不可复活 | PASS：旧 Cluster ID 不比较 Term，重启后仍 replay。 |
| serial 不回绕 | PASS：checked-next + threshold router + CTest 静态 gate。 |
| Recovery 不污染 Stable history | PASS：Record v4 scope 决定恢复投影。 |
| 默认产品不付 M13 代码 | PASS：实验 target 默认 OFF；默认 `libucn_cluster.a` 无 transaction/history 符号。 |

## 3. 全体自审中主动发现并关闭的问题

1. ACK deadline 只在 `step()` 检查，Wire/ACK/Prepare I/O 入口可能先使用过期事务：已给三条入口增加时间 preflight 与零写/零 I/O 回归。
2. durable helper 只比较字段、未先验证完整 state canonical：已统一要求 `ucn_cluster_persist_state_is_valid()`。
3. allocation history 只有 RAM admit/codec，未强制 durable-before-PREPARE：新增两阶段 durable gate，并把 generation + 完整 fingerprint 绑定进 Record v4。
4. history encoder 可接受手工冲突状态：新增 canonical rebuild、一一映射与非法 history 回归。
5. Provider init/submit/poll 重入矩阵不完整：补齐递归 Init/Prepare/Commit/Abort/Poll。
6. 旧 schema v3 注释与当前 writer 不一致：已同步为 schema v4。

## 4. 验证矩阵

| 门禁 | 最终结果 |
|---|---:|
| Windows GCC Debug Full | 20/20 |
| Windows GCC Debug Lite | 18/18 |
| Windows GCC Debug Nano | 18/18 |
| Windows GCC Service OFF | 18/18 |
| Windows GCC Release | 20/20 |
| Windows MSVC Debug（含 Scale） | 30/30 |
| WSL GCC ASan/UBSan | 20/20 |
| WSL GCC `-fanalyzer -Wall -Wextra -Werror` | 18/18 |
| no-wrap source gate | PASS，31 files / 10 fields |
| default-OFF archive | PASS，无 transaction/history 符号 |
| production source reference | PASS，M13 API 只存在于 3 个实验源文件 |
| `git diff --check` | PASS，仅既有 CRLF 转换提示 |

资源观测：Host Full `cluster_bytes=1616`；Release 实验 archive 三个 object 的 text 合计 26376 B，BSS 16 B。该数值不是 MCU section/stack 实测，不能作为产品预算签字。

Record codec 源码 SHA256：`76C0B187E518AD41A62660557C0A0FDC27B296903A060CA856218F218F97A4DC`。

## 5. 保留限制

- Record v4 与 history record 的真实 Flash 双槽、掉电、磨损、任务栈和 MCU RAM/Flash 仍需 M14 实机门禁。
- 当前只保留一个 committed Rekey Tombstone；多代退休 lineage 未实现时第二次 Rekey保守 fail-closed。
- 8-entry history 满载不淘汰，安全返回 `UCN_ERR_NO_SPACE`；产品容量策略属于 M14。
- successor materialize 是不可分割值对象，不是生产 FSM 接线或 Authority 授权。

因此 M13 现在仅可提交外部审计，不得进入 M14 或宣称生产完成。
