# UCN v6 官方文档

> 文档级别：当前实现说明
> 适用分支：`v6-development`
> 发布状态：软件实现与自审阶段，尚非 UCN 1.0 RC

本目录只描述当前 v6 公共头、源码和构建入口。v4/v5 用户资料已移入
[`docs/archive/v5-user-facing-20260905`](../archive/v5-user-facing-20260905/README.md)。

## 推荐阅读顺序

1. [项目定位与成熟度](00-项目总览/01-项目定位与成熟度.md)
2. [模块、数据流与所有权](01-总体架构/01-模块、数据流与所有权.md)
3. [Wire v6 与消息模型](02-核心协议/01-Wire-v6与消息模型.md)
4. [身份、入网、会话与 ACL](03-身份与安全/01-身份、入网、会话与ACL.md)
5. [RouteSet、动态代价与 QoS](04-路由与QoS/01-RouteSet、动态代价与四级调度.md)
6. [32 B～8 KiB 可靠传输](05-Transfer/01-32B到8KiB可靠传输.md)
7. [可选 Realtime](06-Realtime/01-时间同步、uncertainty与Deadline.md)
8. [可选 Cluster](07-Cluster/01-Cluster-Target与持久Authority.md)
9. [Adapter、Bearer 与平台](08-Adapter与平台/01-Event-Owner与Bearer接入.md)
10. [Profile、Feature 与资源](09-配置与资源/01-Nano-Lite-Full与Feature-Manifest.md)
11. [集成顺序](10-集成指南/01-从配置到收发的完整顺序.md)
12. [公共 API 导航](11-API参考/01-公共头与Owner调用顺序.md)
13. [验证与发布门禁](12-测试与发布/01-验证矩阵、证据等级与发布阻断.md)
14. [v5 快照与 v6 断代规则](13-迁移/01-v5快照与v6断代规则.md)

## 事实边界

“源码存在”“Host 测试通过”“目标板验证”和“发布放行”是四个不同结论。当前软件已经形成
完整 v6 模块和测试基线，但真实驱动、Flash 掉电、硬件时间戳、性能长稳及缺失工具链尚未
全部闭环。任何产品结论都必须引用与同一提交绑定的硬件和日志证据。
