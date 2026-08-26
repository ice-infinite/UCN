# 诊断与运维

> 文档级别：`GUIDE`
> 实现状态：`CURRENT（诊断方法）`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：状态视图、统计 API、测试与现有实测记录
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：部分实测；未测项不得推断

诊断遵循“物理 Source → Adapter Queue → Link/Neighbor → Route/Path → Transfer/Service → Cluster”的顺序。先确认数据是否进入协议，再判断路由和业务，避免只看最终超时。

远程诊断、全网节点快照和管理命令必须受 Endpoint ACL/安全策略保护；诊断读取不得改变租约、路由或 Authority。

## 标准排查路径

```text
版本/配置/固件hash
  → Driver是否收到/发出
  → Source/Carrier是否成帧
  → Adapter Queue是否背压
  → Link/Neighbor是否有效
  → Frame/Security/Duplicate是否通过
  → Route/Path/Policy是否选中
  → Transfer/Service是否完成
  → Cluster Authority/Persistence（如果启用）
  → 应用任务是否ready/执行
```

不要从“应用没响应”直接跳到重启或调Cost。

## 阅读导航

- [日志、统计与快照](01-日志、统计与状态快照.md)
- [Neighbor/Route/Path/全网诊断](02-Neighbor、Route、Path与全网节点诊断.md)
- [Link Cost与负载](03-Link质量、Cost与负载诊断.md)
- [入网/离网/恢复](04-入网、离网、断链与恢复排查.md)
- [背压/Transfer/吞吐](05-队列背压、Transfer超时与吞吐排查.md)
- [Cluster](06-Cluster角色、Authority与Persistence排查.md)
- [升级/回滚/介质](07-升级、回滚与持久化介质处理.md)
- [故障码索引](08-故障码与排查索引.md)

## 一份有效故障报告

至少包含：复现步骤、期望/实际、首次发生时间、板卡/Node/端口/接线、commit/固件hash、产品配置、完整错误码、相关统计前后差值、原始日志和是否可稳定复现。只写“又不行了”无法区分代码、配置、接线和硬件故障。

## 安全边界

诊断信息本身可能泄露拓扑、设备身份和运行状态。生产远程诊断按最小权限开放，敏感dump加密/脱敏；任何诊断API都不得续租、投票、改变Route或清除Fence。
