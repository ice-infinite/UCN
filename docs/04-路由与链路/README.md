# 路由与链路

> `MIGRATION / NOT CURRENT`：本目录保留建议与历史报告，当前路由合同以[官方路由与链路](../official/03-路由与链路/README.md)为准。

这里覆盖 Neighbor、Bearer、Link Cost、路由发现、路径缓存、追踪和负载均衡：

- [Link Metrics 与 Cost 契约](UCN_Link_Metrics与Cost契约.md)
- [Link Cost 计算规范](UCN_Link_Cost计算规范.md)
- [路由缓存与自动选路建议](UCN_路由缓存与自动选路建议.md)
- [路由策略与负载均衡执行建议](UCN_路由策略与负载均衡执行建议.md)
- [路径追踪诊断建议](UCN_路径追踪诊断建议.md)
- [节点入离网与稳定性建议](UCN_节点入离网与网络稳定性建议.md)

Cluster Head 不接管普通数据路由；数据面仍由 Core 路由和 Bearer 策略决定。
