# Cluster 当前使用边界

本文不是 Cluster 上线教程，而是告诉普通 UCN 用户：什么时候不需要 Cluster、当前仓库中的 Cluster 能用于什么、哪些能力仍不能当成产品承诺。

## 1. Core 与 Cluster 是两套层次

UCN Core 解决的是节点通信基础问题：

- 多种 Bearer 统一 Link 接口。
- Node ID 寻址、动态邻居、路由和转发。
- Endpoint、Service、Transfer 与安全边界。
- Full 的策略选路、指定路径和可选负载均衡。

Cluster 是建立在 Core 之上的可选控制面，目标是为大规模网络提供成员身份、簇头、Backup、配置变更、权威租约、接管、合并、恢复和 Rekey 等机制。普通两节点、多跳传感器网、UART/CAN/Wi-Fi 网关都可以只使用 Core，不需要 Cluster。

```text
业务 Endpoint / Service / Transfer
                |
         UCN Core 数据平面
                |
      可选 Cluster 控制平面
```

Cluster 不替代 Core 路由，也不要求所有 UCN 产品启用。

## 2. 当前仓库状态

截至当前 V5 开发分支：

- Cluster 已有大量 Host 模型、状态机、持久化合同、Wire v4 codec、Membership、Joint Config、Authority/Fence、Backup Mirror、Takeover、Handover、Recovery 与 Rekey 软件工作。
- 相关里程碑经过分项自审和多轮外部审计，但文档仍保留顶层 `AUDIT HOLD` / 发布门禁。
- Wire v4 的 40 B codec、语义和测试能力不等于默认产品 RX/TX/FSM 已放行。
- 默认 encoder、实验模型或测试 archive 的开启方式不能直接推导为量产配置。
- Host 上的同步/异步持久化和双槽测试不等于真实 MCU Flash 擦写、掉电、磨损和启动恢复已经验证。
- Cluster 的资源、时序、规模、真实多 Bearer、断电和长期运行仍需目标硬件证据。

因此，本用户手册不给出“打开一个宏即可量产 Cluster”的步骤。

## 3. 普通用户现在应该怎么选

| 需求 | 建议 |
| --- | --- |
| 两块 MCU 通信 | 只使用 Nano/Core |
| 多 MCU 自动组网与寻路 | 使用 Lite/Core |
| 多介质策略、指定路径、负载均衡 | 使用 Full/Core |
| 节点内任务与跨 MCU 统一消息 | 使用 Service |
| 32 B～8 KiB 消息 | 使用 Transfer |
| 大规模网络研究、簇头/Backup/合并实验 | 可在受控 Host/实验构建研究 Cluster |
| 量产依赖 Cluster 权威、掉电恢复或自动合并 | 当前不要宣称完成，等待对应发布门禁解除 |

## 4. 允许的 Cluster 使用范围

当前合理用途：

- 阅读和评审状态机、Wire 与持久化合同。
- 运行仓库已有 Host 单元、属性、Fuzz、Scale 和实验 archive 测试。
- 在隔离实验固件中采集资源和时序数据。
- 为未来产品定义节点容量、成员资格、Flash Provider 和安全需求。

不应做的事情：

- 把测试宏或实验 archive 直接并入默认产品固件。
- 把 `VOLATILE_TEST` 当成掉电安全证明。
- 在真实设备上依赖尚未放行的 v4 Authority 产生安全关键动作。
- 把一次 Host 收敛模拟外推为万级实网可用。
- 混用未冻结/未协商的 Cluster Wire 格式并期待自动兼容。

## 5. 未来放行前的产品清单

Cluster 真正进入用户快速手册前，至少需要：

1. 生产 v4 RX/TX/FSM 和 encoder 的明确放行边界。
2. M04 持久化接入真实目标 Flash，完成同步、异步、双槽、撕裂写、掉电和磨损测试。
3. Authority/Fence、Config、Takeover、Handover、Recovery、Rekey 的跨模块全链路证明。
4. v3/v4 混合版本策略、升级和回滚方案。
5. 多节点、多 Bearer、分区、恢复、节点抖动和长期稳定性实测。
6. 目标 MCU 的 RAM、Flash、栈、CPU、延迟和功耗门禁。
7. Cluster 控制帧的安全、授权、重放防护和密钥生命周期。
8. 独立外审签字并解除仓库的顶层 `AUDIT HOLD` / Release NO-GO。

## 6. 如果你要参与 Cluster 开发

用户集成文档不是 Cluster 内核设计的权威来源。请依次阅读：

1. `docs/07-Cluster簇/README.md`
2. `docs/07-Cluster簇/00-规范与状态机/UCN_Cluster_Wire_v4.md`
3. `docs/07-Cluster簇/00-规范与状态机/UCN_V5_Cluster_FSM_Design_v2.md`
4. `docs/07-Cluster簇/00-规范与状态机/UCN_V5_Cluster_Current_to_Target_v2_详细修改方案与任务表.md`
5. 各 M03～M14 自审、外审和发布阻断材料

对普通 UCN 用户，结论很简单：先把 Core、驱动、Endpoint、Service、Transfer 和产品安全做好；只有产品确实需要簇级权威治理，而且 Cluster 发布门禁解除后，才把它加入默认系统。
