# Cluster 簇控制平面

> `MIGRATION / MIXED EVIDENCE`：本目录保留 Cluster RFC、任务表、自审和外审链；当前能力边界先看[官方 Cluster 文档](../official/07-Cluster簇/README.md)。

Cluster 是建立在 UCN Core 之上的可选控制平面，负责成员关系、Head/Backup、Authority、配置、接管、恢复、合并和 Rekey；它不负责普通业务数据逐包转发。

## 目录

- [规范与状态机](00-规范与状态机/)：Current/Target FSM、Wire、Persistence、Config 和 API 合同。
- [早期设计与实测](01-早期设计与实测/)：C05～C07 与自动分簇早期阶段资料。
- [M03～M14 里程碑](02-里程碑/)：每个里程碑的计划、自审与整改证据。
- [审计与复审](03-审计与复审/)：跨里程碑复审材料。

## 当前权威入口

- [Current → Target v2 详细任务表](00-规范与状态机/UCN_V5_Cluster_Current_to_Target_v2_详细修改方案与任务表.md)
- [Target FSM v2](00-规范与状态机/UCN_V5_Cluster_FSM_Design_v2.md)
- [Current FSM](00-规范与状态机/UCN_V5_Cluster_CURRENT_FSM.md)
- [Wire v4 RFC](00-规范与状态机/UCN_Cluster_Wire_v4.md)

当前仍需遵守 M05 `AUDIT HOLD` 和 M14 `RELEASE NO-GO`，不得把 default-OFF 实验组件写成生产已接线。
