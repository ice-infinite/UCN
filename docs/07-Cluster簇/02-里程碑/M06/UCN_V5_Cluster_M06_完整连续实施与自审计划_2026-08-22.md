# UCN V5 Cluster M06：完整连续实施与自审计划

日期：2026-08-22  
范围：`CLV2-06-01` 至 `CLV2-06-09`  
执行方式：用户授权后连续实施；不在每一个子项完成时等待外部审计。

## 1. 本轮执行纪律

1. `06-01`、`06-02` 已完成代码与各自自审；它们的外审状态合并到 M06 最终外审。
2. 从 `06-03` 开始，每一小项必须依次完成：设计核对、最小代码改动、定向测试、源代码边界扫描、独立自审记录。任何一项自审失败，先修复，不跳过。
3. `06-09` 结束后执行一次 M06 全量自审：Full/Lite/Nano/Service-OFF、配置契约、Sanitizer、`-fanalyzer`、规模模拟、资源变化、遗留 v3 回归，以及 M05 隔离扫描。
4. 只有全量自审通过后，才将整套 M06 交由外部审计；在此之前不把单项“自审通过”描述为 M06 已放行。

## 2. 不可跨越的边界

M05 顶层仍为 `AUDIT HOLD`。因此本轮：

- 不在 `src/extended/ucn_cluster.c` 接入 v4 40 B 的生产 RX/TX/dispatcher/FSM；
- 不启用 v4 encoder，不把 codec compatibility/admission 结果当作 Authority、Head/Backup 资格或 quorum；
- 不让 Adapter 或通用 Link 引用 Cluster v4；
- `06-04` 的“v4 Join 成功”仅实现为**已经由未来 RX Owner 严格验证完成后**可调用的、与 wire codec 解耦的 provisional-admission 语义。该接口的生产 wire 接线留待 M05 总体放行及后续 FSM 任务；
- `06-09` 的 auto-commit 只能编译进明确命名的 Host 测试副本（`ucn_tests` 或历史 simulator bridge），不能出现在产品库、产品配置或默认行为中。

## 3. 子任务与验收

| 顺序 | 任务 | 实现要点 | 本项自审最低证据 |
|---|---|---|---|
| 06-01 | 状态模型 | `NONE/PROVISIONAL/COMMITTED/REMOVING`、合法域、legacy bridge | 已完成；并入最终复审 |
| 06-02 | 成员表封装 | `primary_members` 表，Runtime 与 mirror 命名分离 | 已完成；并入最终复审 |
| 06-03 | Voter set | 固定上限、升序 Node ID、hash、contains、quorum；bitmap 覆盖 Head 在内的全部 voter | 模型单测、上限/重复/未排序拒绝、无 v4 生产引用 |
| 06-04 | Provisional admission | 经验证的 Join 仅加入 Runtime provisional，Active voter set 不变 | Head 立即失效的 quorum 排除测试；无 v4 RX/FSM 接线 |
| 06-05 | Provisional deadline | deadline 到期清理，未 commit 不永久占 Runtime slot | 超时、重复 join、容量重新可用测试 |
| 06-06 | legacy v3 | v3 只能 non-voting provisional；不得成为 Backup/Head candidate | mixed v3/v4 模型测试，生产无 v3 auto-commit |
| 06-07 | 只读摘要 | status/voting/config_id 公开可读，Owner table 不可从 API 修改 | 诊断字段与无写回契约测试 |
| 06-08 | 双容量语义 | Runtime 与 Voter 容量独立；拒绝原因可诊断 | Runtime 满、Voter 满、重复 Join、Head 计数边界测试 |
| 06-09 | 测试过渡桥 | 仅 Host 测试副本可恢复旧 v3 回归，产品 Strict 不可 auto-commit | 编译定义/符号/源码扫描、旧回归与 Strict 负例 |

## 4. 预期数据关系

```text
已验证 Join 意图
       │
       ▼
Runtime member table ──> PROVISIONAL (占 Runtime 容量、非 voting)
       │ CONFIG_COMMIT（M07 以后才定义真实事务）
       ▼
Committed member + Active voter set (占 Voter 容量、参与 quorum)

legacy v3 Join ──> PROVISIONAL / non-voting
test-hook auto-commit ──> 仅测试副本的旧回归桥，非产品语义
```

M06 不实现真实 `CONFIG_COMMIT`、Joint Config、voter 变更持久化或 certificate/quorum authority；这些仍属于 M07、M10 与后续里程碑。

## 5. 最终 M06 自审清单

- 每一 member record 与 member table 的 canonical/合法域；
- voter set 的排序、hash、Head 包含、bitmap 宽度与 quorum 边界；
- provisional 永远不进入 active voter set、Backup 选择或 takeover 计数；
- v3 legacy 在产品构建永远不获得 Backup/Head 候选资格；
- deadline 清理不会误删 committed record，也不会泄漏 Runtime 容量；
- public summary 只读，外部不可直接改 owner table；
- test-only bridge 不进入 production archive，产品 Strict 无 auto-commit；
- Full/Lite/Nano/Service-OFF、配置契约、Sanitizer/Analyzer、模拟与资源报告；
- `ucn_cluster.c`、Adapter 与通用 Link 无 v4 RX/TX/FSM/Authority 接线；v4 encoder 默认关闭。

## 6. 最终外部复审结论

2026-08-22，外部复审已确认 `CLV2-06-R01` P0 闭环：production RX 在任何写入前拒绝 v3 Backup/Takeover authority frame，production archive 回归不使用测试桥，且 Host legacy bridge 保持 target-private。M06 因此标记为 **DONE / 外部复审 GO（受限软件范围）**。

该结论不改变第 2 节边界：M05 顶层仍为 `AUDIT HOLD`；不得提前接入 production v4 RX/TX/FSM、Authority、Adapter，亦不得将 Host/软件结果表述为真实 Flash、掉电或 MCU 实机验证完成。
