# UniLink 工程架构

> **UniLink**（品牌名）即 **UCN — Unified Communication Network**（正式协议名）。代码符号保持 `ucn_*` 前缀。

本目录描述当前仓库如何组织，而不是重复协议字段或产品业务 ABI。UCN 保持 MCU-first：Core 能在没有 Linux 时独立运行；Linux/ROS2/PX4 只在未来以可选 Host/Adapter 方式接入。

## 阅读顺序

1. [系统边界](01-系统边界.md)：Core、产品 Port/Adapter、Extended、Host 与 Build Profile 的关系。
2. [代码模块与依赖规则](02-代码模块与依赖规则.md)：每个目录负责什么、允许依赖什么。
3. [目录迁移与兼容策略](03-目录迁移与兼容策略.md)：内部移动源码时如何保持公开 API 不变。
4. [构建目标与测试地图](04-构建目标与测试地图.md)：CMake Target、Profile 与测试分类。
5. [V5-48 ISR 队列与容量合同修复](../../08-实现与验证/版本演进/UCN_V5_48_ISR队列与容量合同修复报告.md)：Task/ISR token 边界、低容量 Scale 选择与旧 Path 配置源码兼容。
6. [V5-62 Port API V2 与审计缺陷修复](../../08-实现与验证/版本演进/UCN_V5_62_Port_API_V2与审计缺陷修复报告.md)：预发布破坏性 API 迁移、Transfer 权威时钟和经典 CAN 完成优先提交。

当前实施任务：V5-62 / DOC-057 已完成 Host 软件闭环。协议总体设计仍以 ../UCN_整体架构设计.md 为准，接入 API 以 ../UCN_使用与调用手册.md 为准。
