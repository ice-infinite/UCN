# RouteSet、动态代价与四级调度

## 1. RouteSet 键与生命周期

RouteSet 不仅按 Destination 查找，还绑定 Realm、目标 Address Binding Generation 和本地
Session/Route Generation。地址被另一个设备复用、Session 重建或 Generation 变化时，旧
RouteSet 不能命中。

每个 RouteSet 可以保存多个已验证 Path。Active Route 在新 Candidate 完成 Probe、冻结路径
快照、发送 Activate 并收到精确 ACK 前保持不变。Candidate 的 ID、Epoch、出口、Cost、Hop、
Profile 和 Capability 是同一事务；进入 Probe 或 Activate 后，新的 RREP 不能在原事务内改写
路径。容量、发送、ACK 或 deadline 失败均保留旧 Active 并按合同重试或清理 Candidate。

## 2. 选路与故障

Route 选择先验证 Binding/Session/Generation，再按 Pin、Policy、健康度和 Cost 选择 Path。
静态 Route 是不参与动态 Route Epoch 所有权的明确 fallback；动态实例存在且合法时优先。
RERR 按完整 Route Key 精确失效，不能污染另一个 Origin 或 Binding 的 Previous Grace。

多路径不是简单轮流发包：可靠 Transfer 必须知道 Fragment 属于哪条 Path，Session 或 Path
失效时只重排未确认片段；有序业务需要在目标端按 Transfer/Operation 语义重组，不能把 Path
到达顺序当作业务顺序。

## 3. 动态 Metric

基础 Cost 来源于 Bearer 类型、速率、MTU、延迟和产品默认权重；动态项根据队列占用、RTT、
丢包、重传、错误和稳定性增加惩罚。所有算术饱和，Metric Algorithm ID 必须一致；未知或
过期测量不会被解释为零成本。

Cost 只用于相对选择，不承诺不同 Bearer 的物理含义完全相同。产品应通过实测校准默认表，
并限制变化斜率和切换滞回，避免短时队列波动造成路径振荡。

## 4. Q0～Q3

| 类别 | 典型用途 | 规则 |
|---|---|---|
| Q0 | 紧急控制/关键协议 | 固定保留与严格上限，仍不能绕过安全或容量 |
| Q1 | 低延迟状态、ACK/结果 | Latest 流使用 round-robin，热点流不能饿死其他 key |
| Q2 | 常规可靠消息 | 受公平和每源/每流配额约束 |
| Q3 | 大消息 Fragment/后台数据 | 使用剩余配额，不能反压到永久占用 Owner |

默认调度使用有界权重和游标；选择成功后必须显式 complete，Driver completion 另行归还资源。
统计区分 enqueue、schedule、queue-send 与链路总发送，避免把转发/即时发送混成队列损失。

## 5. Hop Scheduling Budget

Budget 只允许在原 Traffic Class、per-source 和 per-flow 配额内排序，不能升级 Class、侵占
其他类别预留或跳过 ACL。每跳验证 `0 < remaining <= initial`，只允许递减；耗尽后按策略
丢弃或在明确允许时降级，不能继续携带伪造紧急度传播。
