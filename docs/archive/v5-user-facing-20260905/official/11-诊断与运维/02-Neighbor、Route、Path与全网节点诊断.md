# Neighbor、Route、Path 与全网节点诊断

> 文档级别：`GUIDE`
> 实现状态：`CURRENT（诊断方法）`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：状态视图、统计 API、测试与现有实测记录
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：部分实测；未测项不得推断

Neighbor 表只表示直连节点及 Bearer；Route 表记录到目标的 next hop；按需 Path Trace 才返回完整经过节点。节点不需要保存所有端到端完整路径。

全网 Node Snapshot 是低频诊断控制事务，不是持续广播目录。查询时记录发起时间、超时、已响应节点和遗漏原因，避免把未响应解释为不存在。

## 先画清三张表

```text
Neighbor:  本节点 --直连Link/Bearer--> Peer
Route:     TrafficOrigin + Destination -> NextHop + EgressLink + Cost + Expiry
Path:      Owner/Session/PathID/Destination -> NextHop + Lease
```

Neighbor回答“一跳能否直接通信”；Route回答“下一站给谁”；Path回答“已显式安装的本地转发段”。其中任何一张都不是全网拓扑。

## Neighbor排查

读取summary时关注Node ID、state、last seen和各Bearer状态。排查顺序：

1. Link status是否up、MTU是否非零；
2. Source是否收到合法HELLO；
3. peer physical address是否绑定正确；
4. Join policy/authorize是否admit；
5. Heartbeat是否在一跳内更新；
6. suspect/remove deadline是否符合配置。

如果两块板物理能收包但一直candidate，通常是Network ID、Node ID、Security/Join授权或Wire Class不匹配，不要先改Route。

## Route排查

发送返回`NOT_FOUND`时查看是否已有pending discovery、RREQ是否被rate limit、邻居是否admitted、缓存/发现槽是否满。诊断必须同时打印 Route Origin；只打印 Destination 会把共享中继上的不同实例误认为同一条。沿路径逐节点看：

```text
A route(origin=A,dst=C) = next B
B route(origin=A,dst=C) = next C
C is destination
```

B不需要知道“A最终想要C的哪种数据”，只看Destination/Endpoint并转发。A同时要C的IMU/温度时Route相同，Endpoint不同。

断链后检查依赖该Link/next hop的Route是否失效、RERR是否沿前驱返回、源节点是否重新发现。多 Origin 情况还要确认 RERR 只清理对应 `(origin,destination)`，`route_instance_table_full` 是否增长，以及另一 Origin 的业务是否连续。缓存30s只是未刷新上限，不应阻止显式Link Down立即撤销。

## Path Trace

按需调用Path Trace并设置record limit/handler。每个中继把自身Node ID追加到有界记录并转发；返回结果显示当前一次探测经过的完整节点。它不是永久路径保证，之后Cost/拓扑仍会变化。

Trace需要authorizer/token，避免任意节点高频枚举网络。超时要区分请求没发、某跳不支持、返回路由断开和record被截断。

## Node Snapshot

Snapshot请求低频广播/受控收集当前节点摘要。结果表应含发起session/transaction、响应Node、到达时间、能力/状态和truncated。未响应可能是离线、丢包、未授权、限流、路径不可达或结果上限，不等于节点不存在。

## 远程诊断安全

Path/Node/Policy诊断Endpoint必须配置ACL和E2E策略；中继只转发。诊断结果可能泄露拓扑、设备ID和容量，生产默认应关闭或只允许维护节点。

## 输出示例

```text
neighbor 0x22 ADMITTED via link=1 cost=12 age=80ms
route    origin=0x11 dst=0x44 next=0x22 link=1 epoch=7 hops=2 cost=28 expires=+9200ms
path     owner=0x11/session=7 id=3 dst=0x44 next=0x33 lease=+5000ms
trace    0x11 -> 0x22 -> 0x33 -> 0x44 (4/limit8)
```
