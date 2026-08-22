# UCN V5 Cluster M08：Authority / Fence 分项与全量自审报告

> 日期：2026-08-23  
> 结论：`SELF-AUDIT PASS / WAIT EXTERNAL`。R31/R32 外审 P0 整改已完成并复测；未提交、未推送。  
> 适用范围：受控 Host/实验 Authority Owner；**不是** production v4 RX/TX/FSM、实机、Flash 或掉电签字。

## 0. 不变量与隔离边界

本轮实现并验证：

```text
authority_active => canonical Config 的当前 quorum 有效
```

Owner 在发现 quorum 不足的同一个 step 内按 `revoke -> gate -> GRACE` 顺序执行。`role == HEAD` 只保留身份上下文，不能作为写权限判断。

受控 Owner 只能由调用方明确安装，且当前只接受 `UCN_CLUSTER_PERSISTENCE_VOLATILE_TEST`。这是防止把 value-only Config 误称为持久化授权的 fail-closed 边界，不是对生产 REQUIRED 的支持声明。生产默认 v3 仍为 32 B；v4 encoder enable 仍只存在于两个独立 codec test target。

## 1. 分项自审

| 任务 | 自审结论 | 实现/反例证据 |
|---|---|---|
| 08-01 Phase/state | PASS | `ucn_cluster.h` 仅追加 18..20，不改 M01 shadow phase；Owner 只在 operational→Grace/Fenced、Grace→resume/Fenced、Fenced→Recovery/Join 的受控边写入。 |
| 08-02 独立 Authority | PASS | `authority_active`、`authority_phase`、fence reason 进入 public view；`ucn_cluster_authority_active()` 对 NULL/未安装 Owner 一律 false。 |
| 08-03 quorum | PASS | `ucn_cluster_authority.c` 对 Stable 与 Joint 分别计算 old/new set；self voter 只在 canonical set 包含本 Head 时有效；`SUSPECT` fixture 不改变结果。 |
| 08-04 同步撤权 | PASS | `authority_enter_grace()` 先写 false 后写 phase；`ucn_cluster_step_inner()` 在 Head send 分支前调用 Owner。 |
| 08-05 TX matrix | PASS | `cluster_transmit()` 在 Token Bucket 前统一调用 Authority gate；Head/Backup authority message 在 Grace/Fenced 拒绝，普通 Member keepalive 保留。 |
| 08-06 restore hold | PASS | `quorum_restore_since_ms + restore_hold_armed` 要求连续 quorum 完整保持；首次恢复只开始计时。 |
| 08-07 permanent Fence | PASS | Grace expiry、Term conflict、higher-term observation、persistence fault、owner step 预算超时均 latch Fence；之后的 quorum/lease refresh 不会 re-enable。 |
| 08-08 cleanup | PASS | `note_higher_authority()` 明确要求 future M10/M11 已验证证书，才到 Join Pending；现存 v3 高 term 仅走 `note_higher_term_observed()` Fence，dissolve 后进入 Recovery Observe。 |
| 08-09 Directory | PASS | Federation managed Head view 要求 `authority_active`；失权时不把本地 locator 标为 withdraw，故没有续租、注册、撤销或 handover 写入。 |
| 08-10 timer algebra | PASS | `W=owner+one-way+retry+jitter+drift+margin`；lease=`3W`、Grace=`2W`、restore=`W`、dissolve=`3W`。Owner gap `> owner_step_budget` Fence；lease 小于 profile 时 init 返回 `UCN_ERR_CONFIG` 且不写 runtime。 |
| 08-11 member grace | PASS | helper 精确计算 `max(0,B-M)+T+W`，非法/溢出输入不写 output。M10 的真实 Member takeover FSM 仍未接线。 |
| 08-12 partition | PASS | 3/4/5/6 voter、Head 所在侧所有远端 voter mask 均已枚举；只有 `self + live remote >= floor(N/2)+1` 可 active。 |
| R31 current-time preflight | PASS | `preflight(now_ms)` 前置至 Cluster TX、RX local-Head gate、Federation step/query/public handover；过期缓存不能越过成员、token 或 Directory 写入。 |
| R32 atomic Config switch | PASS | `install_config(..., now_ms)` 在候选集合 quorum 重算后才提交；无 quorum 的 Stable/Joint 立即撤权并进入 Grace。 |

## 2. 定向回归

`tests/test_cluster_authority.c` 覆盖：

- Stable 与 Joint old/new quorum；self vote、remote lease、Neighbor `SUSPECT` 分离；
- 同 step revoke、Grace 中 TX matrix、restore hold；
- Grace timeout、Term conflict、higher verified/unverified、persistence fault、Owner scheduling late；
- no-write timing/profile failure；
- 3/4/5/6 全部 Head-containing partition mask；
- 对 Federation 的 production archive fixture：active 时可写，失权后发送统计不增长且不生成 withdrawal。
- R31：lease/Owner budget 过期后先 Cluster TX、先 `JOIN_REQUEST`、先 Federation public handover/step，均不发送、不消耗 token、不写 member 或 Directory。
- R32：active→无 quorum 的 Joint 与 Stable Config 均立即 inactive，Head/Backup Authority TX 均拒绝。

## 3. 全量自审矩阵

| 门禁 | 结果 |
|---|---|
| Windows GCC Full | `ucn_tests` + M08 定向测试通过；全量 CTest 在本轮主 GCC build 通过。 |
| Windows MSVC Full Debug | `24/24` 通过（含 M08 Authority target 与 scale cases）。 |
| Windows GCC Lite | `37/37` 通过。 |
| Windows GCC Nano | `27/27` 通过。 |
| Windows GCC Service OFF | `13/13` 通过。 |
| WSL ASan/UBSan Full | `27/27` 通过。 |
| WSL GCC `-fanalyzer -Wall -Wextra -Werror` | `16/16` 通过。 |
| whitespace | `git diff --check` 无空白错误；只有既有 CRLF 提示。 |

## 4. 源码隔离复核

- `UCN_CLUSTER_WIRE_V4_ENCODER_ENABLED=1` 仍只位于 `ucn_cluster_wire_v4_codec_tests` 与 `ucn_cluster_wire_v4_host_dual_stack_tests`。
- production `ucn_cluster.c` 未调用 v4 codec；M08 只增加 Owner tick 与 Authority TX gate。
- M08 没有解析/发送 v4 frame、没有给 v3 member/voter/Backup 授权、没有实现 M09 mirror、M10 certificate、M11 handover、M12 lineage 或 M13 rekey。
- v3 高 Term 当前最多触发保守 revoke/Fence，不能成为 `JOIN_PENDING` 的证书依据。

## 5. 自审结论与外审请求

M08 软件实验范围可送外部审计：请重点复核当前时刻 `preflight()` 是否覆盖 Cluster TX、RX Head 副作用、Federation locator/query/handover，Config 切换是否先撤旧权再计算 Stable/Joint quorum；并复核 Owner 生命周期、phase 转移、Fence cleanup、Profile 裁剪以及 M05 隔离。

在外部审计签字前，M08 不是 `DONE`；M05 顶层 `AUDIT HOLD` 仍有效，不能据此宣称生产 Wire v4、Authority、Flash/掉电或 MCU 实机完成。
