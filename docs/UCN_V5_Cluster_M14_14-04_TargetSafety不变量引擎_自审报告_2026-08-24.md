# UCN V5 Cluster M14 / 14-04 Target Safety 不变量引擎自审报告

> 日期：2026-08-24  
> 状态：`CODE COMPLETE / SELF-AUDIT PASS / WAIT M14 EXTERNAL REVIEW`

## 1. 实现边界

新增只读 `ucn_cluster_invariant_check()` 与 bounded network checker：

- 返回 Safety-1..10 对应的确定性 violation bit，不修复、不推进、不分配内存；
- Debug 且非 test-hook 的 Cluster 在每次 Step/RX 结束后检查，任何 violation 立即断言；Release 不会因该 Hook 把检查器对象强制拉入最终镜像；
- Network checker 检查同一 Stable `cluster_id` 不得有两个 writable Authority；
- local checker 检查 Authority/当前 quorum、legacy takeover 不可写、Recovery identity 隔离、Vote tuple、canonical voter set、history/pending replay identity、Fence 顺序、serial 阈值以及 persistence pending/fault/retry descriptor。

该引擎检查的是**当前对象可观测状态不变量**，不是形式化证明。M10/M11/M13 是 default-OFF 独立事务 Owner；它们的跨事务时序/组合性质由 14-05 property model 检查，不能把本项位图当作它们的生产接线许可。

## 2. 自审与反例

- 十个 Safety category 各注入一个确定性坏状态，验证对应 bit 命中；
- 两个节点在同一 Cluster 同时标记 Authority，Network checker 命中 Safety-1；
- M08 真实 canonical Stable Config + quorum 获权状态检查为零，lease 到期进入 Grace 后仍为零；
- persistence pending 缺 operation/token/fingerprint、fault 与 pending/retry 并存、retry descriptor 残留均 fail-closed；
- invalid argument 不写 output；零初始化 disabled 对象为合法零 violation。

## 3. 运行结果

| 门禁 | 结果 |
|---|---:|
| GCC Full + M10/M11/M13 实验模块 | 44/44 |
| 64/256/1000 clean/impaired + mobility/score-shift | Debug Step invariant 零违例 |
| 定向 `ucn_tests` 十类反例 | PASS |
| M08 Authority 正向/Grace | PASS |
| `git diff --check` | 无空白错误；仅既有 CRLF 提示 |

## 4. 结论

14-04 软件范围完成并通过分项自审。它增加了可执行诊断和 Debug fail-fast，不解除 M05 `AUDIT HOLD`，也不替代 14-05 的状态机 property/model、14-08 实机或 14-11 最终逐条协议证据审查。
