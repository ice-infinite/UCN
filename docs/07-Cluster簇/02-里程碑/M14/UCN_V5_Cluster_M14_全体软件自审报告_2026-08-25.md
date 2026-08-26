# UCN V5 Cluster M14 全体软件自审报告

## 总结论

M14 当前为 **SOFTWARE SELF-AUDIT COMPLETE / PARTIAL / RELEASE NO-GO**。

本轮完成了所有不依赖 production v4 集成或真实硬件的工作，并对每个小节分别自审；最后又执行了跨模块、跨 Profile、Sanitizer、Analyzer、MSVC Release、规模、资源与文档契约的全体自审。未发现新的协议运行时 P0/P1，但仍存在明确的发布阻断。软件候选随后提交为 `a093862`，尚未推送，也没有创建 release tag。

## 分项结果

| 子任务 | 自审结果 | 最终边界 |
|---|---|---|
| 14-01 Phase 唯一源 | PASS | 等待 M14 外审 |
| 14-02 opaque storage/API v2 | PASS | 全量重编译要求 |
| 14-03 Wire 模块裁剪 | PARTIAL PASS | production switch 被 M05 阻断 |
| 14-04 invariant | PASS | 可执行诊断，不是形式化证明 |
| 14-05 property model | PASS | bounded state space |
| 14-06 fuzz | PASS | 固定 seed/有限序列 |
| 14-07 scale | PARTIAL PASS | Current matrix 完成，Target churn 阻断 |
| 14-08 hardware | BLOCKED | 四板/Flash/掉电/多 Bearer 缺失 |
| 14-09 resource | PASS | Host 证据，不是 MCU map/栈水位 |
| 14-10 docs | PASS | 机器合同 + 人工边界核对 |
| 14-11 final review | COMPLETE / NO-GO | Safety/Liveness 仍有 PARTIAL/BLOCKED |
| 14-12 release | BLOCKED | NO TAG / NO RELEASE |

## 最终测试矩阵

| 门禁 | 结果 |
|---|---:|
| Windows GCC Full | 57/57 |
| Windows GCC Lite | 50/50 |
| Windows GCC Nano | 40/40 |
| Windows GCC Service-OFF | 50/50 |
| WSL ASan/UBSan + leak | 54/54 |
| WSL GCC `-fanalyzer -Werror` | 36/36 |
| MSVC Release | 50/50 |
| v4 Debug/O1/O2/O3 | 包含于 Full，全部 PASS |
| 64/256/1000 Current scale | 15/15 |
| Resource gate | PASS |
| Phase/storage/Wire/resource/docs source gates | PASS |
| `git diff --check` | 无空白错误，仅 CRLF 提示 |

Analyzer 首轮发现 `test_staging_and_frozen_quorum_property()` 的 `voters[]` 未整体初始化，工具无法证明 quorum 上界。测试 fixture 已改为全数组清零并显式断言 `0 < quorum <= count`；重跑 Analyzer 与 ASan 均通过。该修复不改变协议业务代码。

## 资源复核

- Host Cluster object：1608 B / 默认预算 2048 B；
- archive text：133559 B；rodata：992 B；bss：8 B；
- GCC 最大静态栈帧：1840 B / 阈值 2048 B；
- Cluster 动态分配调用：0；
- Full/Lite/Nano 显式链接 Cluster 成本相同，不能宣称 Profile 内部已经裁剪；
- Core-only 不生成 Cluster archive。

## 安全边界复核

1. production v4 encoder 仍默认关闭；
2. Current production RX/TX/FSM 仍是 v3 32 B / Type 1..19；
3. Config/Takeover/Handover/Rekey archive 仍 default-OFF；
4. v3 Backup/Takeover Authority frame 仍 fail-closed；
5. M05 顶层 `AUDIT HOLD` 没有被 M14 绕过；
6. 未建立 release tag，未声称真实 Flash/掉电或 MCU 资源完成。

## 外部审计入口

外审应重点检查：

- 14-01 Phase-only 是否仍有隐式 state mint；
- 14-02 public/storage v2 是否泄漏内部 layout；
- 14-04/05 的 invariant/oracle 是否独立且无“被测实现自证”；
- 14-06 pending deadline/replay no-write；
- 14-09 noinline 资源优化是否改变语义；
- 14-10 文档自动契约是否遗漏关键公开常量；
- 14-11 对未完成项的降级是否足够保守。

外审即使签署软件范围 GO，也不能自动解除 14-08 或允许 14-12 发布。
