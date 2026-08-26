# UCN V5 Cluster M14 连续实施与自审计划

> 日期：2026-08-24
> 状态：`SOFTWARE SELF-AUDIT COMPLETE / PARTIAL / RELEASE NO-GO`
> 范围：`CLV2-14-01..12`

## 1. 实施边界

- 用户已明确授权进入 M14；M13 仍保持 `WAIT EXTERNAL REVIEW`，不会因 M14 开工而被追认为外审通过。
- M05 顶层 `AUDIT HOLD` 继续生效。任何生产 v4 RX/TX/FSM、Authority、Adapter 或默认 encoder 接线，都必须在对应门禁与外审后才能启用。
- UCN 继续遵守 MCU-first、固定资源、无动态内存、Host/Linux 可选的架构边界。
- Host 单元、Property、Fuzz、Scale 和资源观测不能替代真实 MCU、Flash 双槽、掉电、栈、CPU、功耗与四板介质实测。

## 2. 连续实施顺序

| 阶段 | 子任务 | 实施重点 | 当项自审 |
|---|---|---|---|
| A | 14-01 | Phase 成为唯一状态源；删除 Shadow mapper、运行时 role 镜像和直接 role 写 | 源码写点扫描、Phase/Role 全映射、Golden/OBSERVED、全 Profile |
| B | 14-02 | Public handle 与内部 storage 分离；API/storage version bump | public-header、示例产品 include、静态 owner、三 Profile ABI 门禁 |
| C | 14-03 | Strict v4 推荐默认；v3 decoder 独立可裁剪且永不进入 voter/Backup safety | v3 OFF 符号/尺寸、v3 ON 混合、严格拒绝、默认产品姿态 |
| D | 14-04 | Target Safety-1..10 Debug invariant engine | 每 Step/RX/事务边界检查、故障注入、失败无越权写 |
| E | 14-05 | 随机状态机 property/model | 固定 seed、Single Authority、quorum/config/vote/replay/persistence 属性 |
| F | 14-06 | v3/v4 codec 与 stateful replay fuzz | 长度、角色、flags、字段、乱序/重复、Sanitizer 门禁 |
| G | 14-07 | 64/256/1000 节点规模模拟 | clean/impaired、partition/heal、Config/Backup/Recovery/Rekey churn |
| H | 14-08 | 四板真实硬件门禁 | 固件 Hash、拓扑、掉电、UART/CAN/ESP-NOW、多轮可复现日志 |
| I | 14-09 | Profile/Feature 资源门禁 | RAM/Flash/栈/控制流量、Feature OFF 不付费、无动态内存 |
| J | 14-10 | CURRENT/TARGET/Wire/Persistence/API/调用树文档同步 | 自动检查 phase/message/task 与源码一致 |
| K | 14-11 | 最终 Safety/Liveness checklist | 未证明项必须降级，不以测试数量替代协议证据 |
| L | 14-12 | Release gate | 仅在 14-01..11 全部满足后建立 tag、兼容矩阵和 rollback 包 |

## 3. 自审纪律

每完成一个子任务：

1. 建立独立自审报告；
2. 执行定向测试、受影响 Profile、Sanitizer/Analyzer 和差异检查；
3. 检查默认产品是否产生新代码/RAM/符号；
4. 更新任务表与 `docs/00-项目管理/01-项目操作记录.md`；
5. 无 P0/P1 自审阻断后才进入下一项。

所有可实施子任务完成后，再执行一次跨模块全体自审。`14-08` 或 `14-12` 缺证据时，M14 必须保持 `AUDIT HOLD/PARTIAL`。

## 4. 2026-08-25 最终状态

- 软件完成并自审：14-01、02、04、05、06、09、10；
- 部分完成：14-03、14-07，均被 M05 production v4 边界阻断；
- 硬件阻断：14-08；
- 最终清单：14-11 已审查，结论 `RELEASE NO-GO`；
- 发布：14-12 `BLOCKED / NO TAG`。

全体软件矩阵已经执行，但不改变上述阻断，也不追认 M05/M08/M09/M10/M12/M13 的既有外审状态。
