# 从现有项目迁移到 UCN v5

> 文档级别：`GUIDE`
> 实现状态：`PARTIAL（通用合同已实现，产品 BSP/SDK 需接入）`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：公共 API、Port/Adapter/Source 实现与测试
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：部分 ESP32-S3 UART/ESP-NOW；其他平台按正文标注

1. 盘点原通信通道、地址、消息类型、可靠性和实时要求；
2. 为每个通道实现 Link/Source，不先改业务；
3. 将业务类型映射到 Endpoint/Service/Transfer；
4. 统一 Node ID、Profile、产品配置和 Port API v2；
5. 双栈观测后逐步切换路由；
6. 再启用安全、Policy 和可选 Cluster。

迁移要求全量重编译；不能假设旧公共结构的 ABI 与 v5/Port API v2 兼容。Cluster Storage/Record 另按发布迁移文档处理。

## 阶段0：冻结旧系统事实

记录每条总线/连接的引脚、速率、拓扑、协议帧、地址、消息频率、最大长度、timeout、重试、安全、CPU/RAM和故障行为。保留抓包/串口日志和当前稳定固件hash，作为A/B基线。

把消息分类为：实时最新值、关键FIFO命令、普通事件、大消息、诊断。不要把旧系统所有packet都映射成同一UCN Endpoint/Q1。

## 阶段1：只替换承载边界

先保留旧业务结构，为每个物理接口实现Link/Source/Carrier，在Host/loopback验证完整帧。此时可以静态route，不启用动态Mesh、Policy或Cluster，缩小故障范围。

## 阶段2：建立统一业务地址

- 每块MCU确定可管理Node ID/Flash配置；
- 每类传感器/命令定义Endpoint/Service ID和payload version；
- 小消息走Endpoint/Service，大消息走Transfer；
- 明确Q0/Q1和delivery/completion语义；
- 建立旧消息↔新消息adapter，支持双栈对比。

## 阶段3：切入Owner架构

把原来各任务直接操作UART/CAN改为：Driver ISR→Source→唯一UCN Owner；应用通过Service/队列通信。先监控模式同时输出旧/新结果，确认时序后再让UCN控制真实业务。

## 阶段4：自动路由与多Bearer

先单Route，再RREQ/RERR，再Path/Pinned，最后Q1 Balance。每一步做断链、重连、负载和顺序测试。旧协议仍作为可回滚旁路，但不能与UCN同时驱动同一执行器。

## 阶段5：安全与Cluster

安全需要真实key/序列/Replay持久化，不能用测试Provider上线。Cluster仅在规模/Authority需求明确后加入，并保持Current/实验边界；不要把“迁移到v5”自动等同启用Wire v4 Cluster。

## 兼容与Storage

Core Wire v5、Port API v2、公共结构和Cluster Record都可能与旧版本破坏性不兼容。升级包应说明：最低可混跑Wire、是否需要全量重编译、旧Flash配置迁移、Node ID/session保留、回滚能否读取新Storage。

## 切换与回滚

```text
shadow观察
  → 少量非关键节点canary
  → 单路径业务切换
  → 故障注入
  → 扩大节点
  → 冻结旧发送
```

每阶段保留一键回到上一个固件/配置的方案。若Storage不兼容，回滚前先Fence、导出状态并执行指定迁移/擦除。

## 完成标准

不仅“能通信”，还要对比旧系统的p99延迟、吞吐、故障恢复、RAM/CPU/功耗、安全和长稳；所有消息类型有owner、版本和测试；现场日志能从Node/Endpoint追踪到物理Link。
