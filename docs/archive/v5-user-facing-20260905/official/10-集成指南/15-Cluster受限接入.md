# Cluster 受限接入

> 文档级别：`GUIDE`
> 实现状态：`PARTIAL（通用合同已实现，产品 BSP/SDK 需接入）`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：公共 API、Port/Adapter/Source 实现与测试
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：部分 ESP32-S3 UART/ESP-NOW；其他平台按正文标注

当前产品只能按默认 Current v3 和明确限制集成。Wire v4 encoder、生产 v4 RX/TX/FSM、M10/M11/M13 均不得因头文件或测试存在而启用。

启用 Cluster 前提供静态 storage、合法 persistence mode/provider、Endpoint、安全策略和 Owner 时间预算。先在 Host/仿真验证 Role/Phase，再做真实 Flash 和多板测试。

任何实验接线必须使用独立 CMake target、默认 OFF、独立外审和回滚配置，不能改变默认产品行为。

## 先判断是否需要 Cluster

2～几十个固定/半固定节点只需自动路由时，Core通常足够。只有当产品需要明确成员集合、多数派Authority、Backup镜像、跨簇目录或有计划的重配置时，才值得承担Cluster的RAM、Flash、控制流量和持久化复杂度。

## 当前可接范围

默认`ucn_cluster`使用v3/32 B兼容状态机，可做观察、选举、Join、成员lease等当前能力；但生产v3 Backup/Takeover Authority帧已围栏。Wire v4 40 B、Config/Authority/Backup Mirror/Takeover/Handover/Rekey存在受限组件或Archive，不构成一条完整生产FSM。

因此产品接入必须保持：

- v4 encoder默认关闭；
- 生产RX仍不接受v4权威消息；
- M10/M11/M13不进默认archive；
- Role不等于Authority；
- M05 `AUDIT HOLD`和M14 `RELEASE NO-GO`显式展示。

## 初始化清单

1. 选择单一owner translation unit分配opaque storage；
2. 配置Node ID、head-capable、score、member/voter capacity；
3. 绑定与Node相同的单调时钟；
4. send callback把Cluster payload封装到受保护Q0 Endpoint；
5. REQUIRED模式提供兼容Persistence Provider；
6. 先在Node建立admitted Neighbor，再同步给Cluster；
7. 注册Cluster Endpoint handler并调用receive；
8. Owner周期调用sync neighbors和step；
9. 应用只通过view/stats和`authority_active()`决策。

## Persistence Provider

真实产品不能用`VOLATILE_TEST`。Provider需要双槽或等价原子存储、generation/CRC、同步load、submit/poll completion和重入安全。M04软件模型通过不代表目标Flash已验证；需要在每个写入点断电并确认旧/新记录二者之一可恢复。

## Host到硬件的阶段

```text
默认v3 Host单测
  → Cluster sim选举/Join/故障
  → 产品Persistence fake/故障注入
  → 单Bearer 4板
  → Head/Member断电与重启
  → 多Bearer/拥塞/分区
  → 真实Flash掉电
  → 资源/功耗/长稳
```

任何阶段失败都保留Core数据面可单独运行的回滚配置。

## 实验组件接线规则

若研究M07～M13，建立`product_cluster_experimental` target，显式宏和source只对该target生效；禁止发布固件链接。每个组件先独立验证完整state/Persistence不变量，再设计唯一Owner集成，不能在应用层手工串API冒充生产FSM。

## 验收输出

记录commit、CMake开关、Record schema、对象/stack、固件hash、Node IDs、拓扑、每轮选举/恢复时间、控制峰值、业务丢包以及所有未测项。只给“最终选出了Head”的日志不足以证明无双Authority。
