# 实验组件

这里记录“已有源码/RFC，但默认产品未启用或生产接线未完成”的能力。实验文档必须回链[官方当前边界](../official/07-Cluster簇/README.md)，不得单独声明为生产功能。

| 组件 | 当前边界 |
| --- | --- |
| Cluster Wire v4 | 40 B Codec/semantic；encoder 默认关 |
| M07 Config/Joint | 受限实验软件范围 |
| M08 Authority/Fence | 受限组件，生产接线受 M05 限制 |
| M09 Backup Mirror/Coverage | 模型，未接旧生产 handler |
| M10 Majority Takeover | 默认关闭 Archive |
| M11 Merge/Handover | 默认关闭 Archive |
| M12 Recovery/Lineage | Target 模型与 Current 边界并存 |
| M13 Rekey/No-wrap | 默认关闭 Archive |
| Federation | 可选 Archive，完整生产跨簇能力未放行 |

详细设计和阶段自审目前仍保留在旧 `07-Cluster簇`/`09-审计与整改` 主题树，迁移时保持原日期和审计上下文。
