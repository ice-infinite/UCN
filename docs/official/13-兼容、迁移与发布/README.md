# 兼容、迁移与发布

> 文档级别：`NORMATIVE`
> 实现状态：`CURRENT（规则）；RELEASE NO-GO`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：版本宏、Wire/API/Storage 合同、CMake 与发布门禁
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：完整发布实机门禁未完成

UCN 的“版本”不是一个数字：项目版本、Core Wire、API/ABI、Port API、Cluster Wire 和 Persistence Record 分别演进。发布判断必须逐项列出，不能只写“v5 兼容”。

当前 Cluster 仍有 `AUDIT HOLD / RELEASE NO-GO`，本目录记录规则，不代表已经生成正式 release/tag。

## 版本坐标

每次讨论兼容性都应写成坐标，而不是一个“v5”：

```text
Project 5.0.0-dev
Core Wire v5 (W0-W3)
Port API v2
Cluster Current Wire v3 / API v2
Cluster experimental Wire v4 (encoder OFF)
Persistence Record writer v4 / 388 B
```

坐标中的任一项变化都可能需要不同迁移。

## 导航

- [四层兼容规则](01-Wire-API-ABI-Storage兼容规则.md)
- [Core Wire/Profile矩阵](02-Core-Wire-v5与Profile兼容矩阵.md)
- [Cluster v3/v4隔离](03-Cluster-v3-v4兼容与隔离策略.md)
- [Port API v2迁移](04-Port-API-V2与公共结构迁移.md)
- [Cluster Storage/Record](05-Cluster-API-Storage与Record迁移.md)
- [升级与回滚步骤](06-版本升级与回滚步骤.md)
- [发布签字](07-发布门禁与签字清单.md)
- [CHANGELOG](08-CHANGELOG.md)
- [支持矩阵/已知问题](09-支持矩阵与已知问题.md)

## 发布流程

```text
冻结候选
  → 四层兼容分析
  → 软件/硬件/安全/掉电矩阵
  → 灰度升级与回滚演练
  → 官方文档/证据一致
  → 外审关闭P0/P1和HOLD
  → 多角色签字
  → 不可变artifact与tag
```

任何阶段未完成都保留开发/RC状态。

## 当前结论

当前文档可以指导继续开发和实验集成，但不能作为生产发布声明。尤其不能因M07～M13独立组件有外审GO，就启用v4生产Authority；顶层M05 HOLD仍是硬边界。
