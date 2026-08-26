# UCN V5 Cluster M12.3 全体复审整改与自审报告（2026-08-24）

## 1. 范围与结论

本轮以 `codex/v5-adaptive-wire` 的 `1fb4521` 加 M12.2 未提交工作树为对象，重新检查 M12 的 Recovery identity、lineage、成员租约、Stable precedence、Wire v3、持久化边界与默认产品隔离，不沿用此前“已经修好”的判断。

结论：**M12.3 运行期整改 CODE COMPLETE / SELF-AUDIT PASS / WAIT EXTERNAL；M12 整体继续 AUDIT HOLD。** `CLV2-12-09` 仍为 PARTIAL；Record scope、硬唯一 ID 分配与 Recovery no-wrap 转入 M13 阻断任务。未进入 M13，也未接入 production v4 RX/TX/FSM、Authority 或 Adapter。

## 2. 本轮发现与整改

| 编号 | 发现 | 整改与反例 |
|---|---|---|
| R31 | Stable Join 后保留旧 Recovery identity，迟到 Declare 可按旧身份刷新 | Stable `JOIN_ACCEPT` 原子清除 Recovery cluster/source/nonce/deadline/ACK 状态；回归证明迟到旧 Declare 不刷新 Stable lease。 |
| R32 | Stable lineage-reset timer 可跨直接 Recovery Join，之后清除新采用的 lineage | Recovery Join 成功后取消旧 reset deadline；回归跨 deadline 验证新 lineage 保留。 |
| R33 | 相同 Recovery ID 可被不同 Head/nonce/Term/parent 重新解释，首个 ACK 丢失也无可靠重发 | 身份绑定 exact `{Head,Term,parent,nonce}`；冲突拒绝且不改身份；exact redeclare 刷新 lease 并重发 ACK。 |
| R34 | Type 16/17 仍接受错误角色、广播 ID、parent reuse 或零 nonce | v3 structural gate 逐项 fail-closed；raw decode、生产 receive 与状态不写回回归全部锁定。 |
| R35 | 已有 lineage 的 idle survivor、当前 winner 与 legacy candidate 仲裁仍有旁路 | 同 parent 的 Term 只前进，不同 parent 不跨域比 Term；parentless downgrade 被拒绝；current Recovery winner 只允许显式更优 legacy candidate。 |
| R36 | Recovery 新轮次与 stepdown 没有彻底隔离旧成员；生产 Recovery provisional member 不会到期 | 新 Recovery identity/stepdown 清成员表；生产 `expire_members()` 覆盖 Recovery Head，过期 provisional member 被清除。 |
| R37 | Stable Backup 可被 Recovery Declare 剥离为 Member，takeover 状态可能形成分裂状态 | 主租约仍有效或 takeover-active 时整体拒绝；只有无接管且主租约已明确过期的 headless Backup 才能进入 legacy Recovery。 |
| R38 | 文档将 32-bit mix、round 与旧 zero ACK 描述为强保证 | 删除强唯一/no-wrap/zero-ACK 兼容承诺；代码注释与任务表改为 best-effort、严格身份和 M13 handoff。 |

## 3. 独立对抗证据

- Stable → Recovery → delayed old Declare：旧 Recovery 身份不能复活或续 Stable lease。
- same Recovery ID + wrong source/nonce/Term/parent：拒绝；对象除允许的接收/重放统计外不变。
- exact redeclare：幂等刷新 Recovery lease，并可补发首次丢失的 ACK。
- parentless、same-parent stale Term、current-winner lower/higher candidate：分别按 domain/rank 处理，不读 foreign Term 决策。
- 新 Recovery round、stepdown、租约过期：旧成员不能跨轮继续充当当前成员。
- Stable Backup：live primary lease 与 takeover-active 两类均在任何角色/身份副作用前拒绝。

## 4. 验证矩阵

| 配置 | 结果 |
|---|---:|
| Windows MSVC Full Debug | 41/41 |
| Windows MSVC Full Release | 41/41 |
| Windows MSVC Lite Debug | 41/41 |
| Windows MSVC Nano Debug | 31/31 |
| Windows MSVC Service-Off Debug | 41/41 |
| WSL GCC ASan/UBSan | 41/41 |
| WSL GCC `-fanalyzer -Wall -Wextra -Werror` | 38/38 |

定向 `ucn_tests`、生产 archive `ucn_cluster_membership_model_tests` 均通过。最终 `ucn_tests` 输出 `OBSERVED-PAIRS=30`、零违反、`cluster_bytes=1616`、`All UCN tests passed`；Golden trace 回归随 `ucn_tests` 通过。`git diff --check` 无空白错误，仅既有 CRLF 提示；Windows 编译仍只有既有 CP936/C4819 编码警告。

## 5. 本轮新确认但不在 M12 伪闭环的阻断项

### 5.1 Persisted Epoch scope 不明确

`RECOVERY_CREATE_COMMIT` 会持久化 Active/Max Epoch；REQUIRED 重启恢复又会将 `max_epoch` 写入 `last_cluster_id/max_seen_term/last_stable_head`。Record v1 没有 Stable/Recovery scope 标记，无法证明重启后的历史一定是 Stable。必须由 `CLV2-13-11` 升级 schema、定义 Recovery tombstone 和恢复顺序。

### 5.2 默认 32-bit mix 不是硬唯一分配器

对当前默认算法的固定序列搜索已观察到真实碰撞：`local=1,parent=101,term=8,config=4,incarnation=1` 时，对象 round `16459` 与 `29522`（对应 recovery round 各少 1）均得到 `3258608038`。运行期 exact-identity gate 会在观察到冲突时 fail-closed，但不能自动换号，也不能提供重启后的 collision history。产品级闭环必须由 `CLV2-13-12` 的 Provider/持久化分配历史完成。

### 5.3 Recovery serial 尚未 no-wrap

`recovery_round`、`cluster_id_round` 与 Recovery nonce 尚未统一执行达到阈值后的 rotate/rekey 或 fail-closed；当前递增/归一逻辑不能作为永不复用证明。由 `CLV2-13-13` 关闭。

## 6. 最终状态

- M12.3 运行期代码与定向回归：**SELF-AUDIT PASS / WAIT EXTERNAL**。
- M12 整体：**AUDIT HOLD**。
- `12-09`：**PARTIAL / boot-ID non-reuse evidence only**。
- M13：**未开始、未授权**。
- 真实 Flash、掉电、MCU 资源、多节点实机：**未验证**。

外部审计应先复核 R31–R38 的反例区分度和生产 archive 行为，再确认三项 M13 handoff 没有被文档误写为 M12 已完成。
