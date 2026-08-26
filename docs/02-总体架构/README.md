# 总体架构

> `MIGRATION / NOT CURRENT`：本目录保留旧架构材料，当前架构以[官方总体架构](../official/01-总体架构/README.md)为准。

这里解释 UCN 的整体边界、模块关系和工程目录：

- [MCU 自组网优先的整体架构](UCN_整体架构设计.md)
- [更新后的稳定入离网与选路设计](UCN_更新后设计方案.md)
- [协议核心逻辑伪代码](UCN_协议核心逻辑伪代码.md)
- [UCN 与 ROS2 协同架构](UCN_ROS2协同整体架构.md)
- [代码模块、依赖和构建地图](项目结构/README.md)

架构原则：Core 不依赖 Linux；Cluster 是可选控制平面；驱动通过 Port/Adapter 接入。
