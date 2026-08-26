# 测试与一致性

> 文档级别：`NORMATIVE`
> 实现状态：`CURRENT（测试规范）`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：CMake、tests、tools、results 与审计门禁
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：实机证据按具体报告；规范本身不代表已验收

UCN 的证据分为单元、集成、虚拟拓扑、规模模拟、静态分析、优化构建和实机。每一层只能证明其覆盖的合同；Host 全绿不能替代 MCU 时序、Flash 掉电、无线环境或功耗。

正式报告必须绑定 commit、配置、工具链、命令和原始结果。未执行项写“未测”，不能用理论推断填充。

## 门禁链

```text
单元/负向
 → 模块集成/Owner时序
 → Profile/Feature/配置矩阵
 → Release/Sanitizer/Analyzer/Fuzz
 → 虚拟拓扑/规模
 → 实机介质/故障/资源/长稳
 → Security/真实掉电
 → 文档/兼容/外审
 → Release签字
```

后层不能替代前层；Host全绿也不能跳过硬件。

## 导航

- [测试策略与证据等级](01-测试策略与证据等级.md)
- [本地构建与CTest](02-本地构建、CTest与常用目标.md)
- [模块追踪矩阵](03-模块到测试的追踪矩阵.md)
- [Profile/Feature矩阵](04-Full-Lite-Nano与Feature矩阵.md)
- [Sanitizer/Analyzer/Release/Fuzz](05-Sanitizer-Analyzer-Release与Fuzz.md)
- [Wire测试](06-Wire-Golden-Negative与兼容性测试.md)
- [规模模拟](07-规模模拟、流量模型与结果解释.md)
- [硬件规范与介质矩阵](08-硬件实测通用规范.md)、[09](09-UART-CAN-WiFi与多Bearer实测矩阵.md)
- [性能资源长稳](10-性能、资源、长稳与功耗验收.md)
- [安全掉电](11-安全与掉电恢复验收.md)
- [发布清单](12-发布一致性与回归清单.md)

## 当前边界

本目录规定应怎样验证，不表示所有项目都已经验证。实际完成状态必须查对应evidence/实测报告与当前任务表；M05 `AUDIT HOLD`和M14 `RELEASE NO-GO`不因规范完整而改变。
