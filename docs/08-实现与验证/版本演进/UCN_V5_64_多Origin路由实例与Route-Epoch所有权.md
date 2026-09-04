# UCN V5-64 多 Origin 路由实例与 Route Epoch 所有权

> 文档级别：`IMPLEMENTATION / VERIFICATION`
> 状态：软件实现与内部自审完成，等待外部复审；多板实机未验证
> 适用范围：Lite / Full 动态 Mesh；Nano 仅保留静态 Route
> 日期：2026-09-04

## 1. 本次解决什么问题

V5-63 已经解决单一 Origin 在扩展环重试时 Route Epoch 不一致的问题，但不能证明多个 Origin 共享中继和目标时安全。

考虑以下拓扑：

```text
A ─┐
   ├─ R ─ X ─ T
B ─┘
```

A 与 B 都需要访问 T。Request ID 和 Route Epoch 都由各自的 Origin 独立生成。A 的 Epoch=100 与 B 的 Epoch=7 之间没有“100 更新、7 陈旧”的全局关系。如果中继只保存 `destination=T` 的单一表项，会产生三类错误：

1. B 的 RREP 覆盖 A 的转发表项；
2. A 的合法业务因 B 的 Epoch 不同而被拒绝；
3. A 的 RERR、刷新或 Previous Grace 误删 B 的路径。

V5-64 把动态路由身份冻结为：

```text
RouteInstanceKey = (traffic_origin, destination)
```

Route Epoch 只属于一个 Route Instance，不再跨 Origin 比较、覆盖或失效。

## 2. 最终所有权合同

### 2.1 Traffic Origin

`traffic_origin` 是最终业务帧的 Source，而不是“当前执行查表的中继”或“最后发送控制帧的节点”。

- 本机新发业务：`traffic_origin = local node_id`；
- 中继业务：`traffic_origin = frame.source`；
- RREQ 反向状态：使用未来回程业务的 Source，也就是目标 T；
- RREP 正向状态：使用原始请求者 A；
- RERR 回程：使用 Payload 中的不可达目标 T，而不是发出 RERR 的中继 X。

### 2.2 固定键

| 状态 | 固定键 | 说明 |
| --- | --- | --- |
| 动态 Route | `(route_origin,destination)` | Current/Previous Epoch、Cost、Hop、Expiry 都属于该键 |
| Candidate | `(route_origin,destination,candidate_id)` | 不同 Origin 可合法复用相同 Candidate/Request ID |
| RREQ seen | `(origin,session,request_id)` | 原实现已经分域，本轮复核保留 |
| 本地 Discovery | `(local_origin,destination)` 的本地槽 | Discovery 只能由本节点发起，因此结构中不重复保存 local Origin |
| 静态 Route | `(0,destination)` | 0 明确表示人工配置的 Source 通配，不是动态 Epoch 域 |

禁止的行为：

- 只按 Destination 找动态 Route；
- 比较不同 `route_origin` 的 Epoch 数值；
- Candidate ID 相同就跨 Origin 合并；
- 某个 Origin 收到 RERR 后删除同目标的全部 Route；
- 表满时覆盖另一个 Origin 的有效实例。

## 3. RREQ/RREP 如何建立两个方向

一次 A→T 发现实际建立两个方向的 Route Instance：

```text
RREQ: A → R → X → T
      沿途建立 (T,A)，供 T→A 的 RREP/回程控制使用

RREP: T → X → R → A
      沿途建立 (A,T)，供 A→T 的业务使用
```

### 3.1 RREQ 接收

中继读取：

```text
origin = frame.source
target = RREQ.payload.target
```

然后学习：

```text
learn_route(route_origin=target,
            destination=origin,
            egress=ingress_link,
            epoch=epoch_from(origin_request_id))
```

这里的反向 Route Origin 是目标，因为 RREP 以及目标发起的回程流量以目标为 Source。

### 3.2 RREP 接收

RREP 的外层身份是：

```text
frame.source      = target
frame.destination = original origin
```

每个中继从 RREP 入站 Link 学习：

```text
learn_route(route_origin=original_origin,
            destination=target,
            egress=ingress_link,
            epoch=RREP.route_epoch)
```

RREP 自身返回 Origin 时，查找的是 `(target,origin)` 反向实例。这样正反方向都由实际帧 Source 所有。

## 4. 普通业务、Current 与 Previous

本机发送查找 `(local_node,destination)`；中继查找 `(frame.source,frame.destination)`。若 Frame 带 Route Extension，Current/Previous Epoch 必须在同一个二元键内匹配。

扩展环或 Candidate 激活更换 Epoch 时，只把本实例的旧 `{egress,epoch}` 放入 Previous Grace。另一个 Origin 即使 Destination 相同，也拥有独立 Current/Previous 和 Deadline。

目标节点接收 A 的业务时，使用本地已有的 `(target,A)` 反向实例验证 A 携带的 Route Epoch。B 域中相同或更大的 Epoch 都不能替代这一证明。

## 5. Candidate 与无缝切换

Candidate 表新增 `route_origin`，完整身份为：

```text
(route_origin, destination, candidate_id)
```

这允许 A 与 B 各自在本地 Request ID 空间使用 `candidate_id=200`，共享中继仍保存两个不同 Candidate。Probe、ACK、Activate 与 Activate ACK 全程携带已有的 Source/Destination，并按方向查找 Candidate：

- A→T Probe/Activate 使用 `(A,T,id)`；
- T→A ACK 使用 `(T,A,id)`；
- 中继激活时分别把正反 Candidate 原子映射到对应动态 Route Instance。

Candidate 激活不会把所有同 Destination Route 一起切换。

发起端的激活事务使用固定状态：

```text
PROBED
  → WAIT_ACK(candidate_id, route_epoch, ack_deadline)
  → ACKED                         // 精确 ACK
  → RETRY_SAME_IDENTITY           // ACK 丢失
  → EXHAUSTED / Candidate 清除    // 旧 Active Route 不变
```

ACK 必须同时绑定本地发起、Probe 发送数与匹配 ACK 数均达到门限、至少一次已提交 Activate、Candidate ID、Source、
Destination、Wire Profile 和 `candidate->route_epoch`。错误、提前、迟到或换 Epoch ACK
在写 Route、Candidate 和统计之前拒绝。中继/目标 Candidate 在第一次合法 Activate 时也
绑定同一 Epoch，之后相同 Candidate ID 不允许改绑。

Candidate 一旦出现任一 Probe、RTT 或 Activate 事务证据，其 `egress_link`、Cost、Hop 和
Wire Profile 就成为不可变路径快照。同 ID 的精确重复 RREP 只允许幂等返回，不能续期；
路径字段不同的 RREP 返回 `UCN_ERR_STATE` 且零写。若 Neighbor 主 Bearer 在事务期间改变，
Core 清除该 Candidate，调用方必须以新的 Candidate ID 重新发现、Probe 和 Activate；不能
把路径 A 的 Probe/Epoch/ACK 证明迁移给路径 B。ACK 提交直接消费这个冻结快照，因此不再
从可变的“最新 Candidate”推导出口。

冻结不是 Origin 独占的状态。每个节点只管理自己的本地 Candidate，因此每一跳必须在
首次向外部 Link 回调提交 `PATH_PROBE` 或 `PATH_PROBE_ACK` 之前设置
`path_snapshot_frozen=true`：Origin 冻结正向 Candidate 后发 Probe；中继转发 Probe 前
冻结正向 Candidate、转发 ACK 前冻结反向 Candidate；Target 回 ACK 前冻结反向 Candidate。
`PATH_ACTIVATE` 的双向预检要求相关 Candidate 都已冻结。冻结先于外部回调，可阻止同步
Send callback 在函数返回前用同 ID RREP 改写路径。物理提交失败也不解冻；同路径可重试，
换路径必须建立新的 Candidate ID。

## 6. RERR 的特殊映射

RERR 外层 Source 是检测到故障的中继，例如 X；但 A→T 的发现只保证沿途存在 `(T,A)` 反向实例，不保证存在 `(X,A)`。

因此普通与 Path-scoped RERR 的回程选路使用：

```text
forwarding_route_origin = payload.unreachable_target
destination             = original_traffic_origin
```

收到 RERR 后失效：

```text
(route_origin=frame.destination,
 destination=payload.unreachable_target)
```

结果是 A 的 RERR 只清理 `(A,T)`；B 的 `(B,T)` 不受影响。该规则不改变 RERR Wire 格式。

## 7. 静态 Route 与直连 Link

本次没有取消原有优先级：

1. 可用直连 Link/Bearer 优先；
2. 精确的动态 `(origin,destination)` Route；
3. 人工安装的静态 Destination 通配 Route。

静态 Route 的 `route_origin=0`，不参与动态 Epoch 所有权。调用 `ucn_node_add_route()` 安装静态 Route 时，同 Destination 的动态实例被清理，使管理员配置具有明确覆盖语义；后续动态学习不会顶替该静态项。

当上游动态 Origin 已经在业务帧中携带非零 Route Epoch，而当前中继只配置了静态
Destination Route 时，中继仍允许该静态 Route 作为通配回退。查找顺序是：先尝试同
`(origin,destination,epoch)` 的动态 Current/Previous，再使用静态 Route；静态项不比较、
不吸收、也不发布该动态 Epoch。目标节点的静态反向 Route 同样只证明“该 Source 可经此
静态路径到达”，不会把静态项伪装成动态 Epoch Owner。

## 7.1 Candidate 激活的原子边界

`PATH_ACTIVATE` 在中继需要把 Forward `(A,T)` 与 Reverse `(T,A)` 两个 Candidate 同时
转换为 Route。V5-64 的最终实现把该过程拆成两阶段：

1. **零写入预检**：核对两个 Candidate 的三元键、Wire Profile、Route Epoch、Link
   可用性，并为两个方向预留不同的 Route 槽；
2. **本地提交**：只有全部预检成功才同时写入两条 Route；
3. **外部发送**：再向下游发送 `PATH_ACTIVATE`；同步 Peer 可以递归返回 ACK，因为此时
   双向本地状态已经完整；
4. **失败回滚**：下游发送或目标 `PATH_ACTIVATE_ACK` 发送失败时，恢复提交前 Route
   与 Candidate 快照；Candidate 保留到固定 Deadline，供同一事务重试；
5. **幂等重放**：相同 `(origin,destination,candidate_id,epoch,profile)` 已激活时不再次
   修改 Route/Previous，也不重复增加切换统计。

因此只剩一个 Route 槽时，中继在发送任何控制帧之前返回 `UCN_ERR_NO_SPACE`，Route 与
Candidate 均逐字节不变。异步 Driver 已经接受 Activate 但 ACK 丢失时，发起端每
`UCN_PATH_ACTIVATE_RETRY_INTERVAL_MS` 使用相同 Candidate ID/Epoch、全新外层 Sequence
重发，最多重试 `UCN_PATH_ACTIVATE_MAX_RETRIES` 次，并受
`UCN_PATH_ACTIVATE_ACK_TIMEOUT_MS` 总截止期约束。耗尽后清除 Candidate，旧 Active
Route 保持不变；不把“本机入队成功”伪装成远端已经 Commit。

## 8. 固定内存与满载行为

V5-64 不引入堆、不按全网节点数扩容。`UCN_MAX_ROUTES` 现在限制的是本节点同时保存的 Route Instance 数，而不是不同 Destination 数。

例如一个共享中继为 8 个 Origin 转发到同一个汇聚点，最坏占用 8 个动态 Route 槽。表满时：

1. 只允许使用空闲或已到期槽；
2. 不覆盖其他 Origin 的有效实例；
3. 学习返回 `UCN_ERR_NO_SPACE`；
4. `stats.route_instance_table_full` 饱和增长；
5. 原 Route 表逐字节保持不变。

RREQ seen 可能已经记录该请求，因此发起端需按已有扩展环/超时规则使用新 Request ID 重试；Core 不无限缓存 Payload，也不动态扩容。

## 9. Profile 与资源影响

| Profile | 动态 Route | Candidate | V5-64 行为 | Host x64 Debug Node |
| --- | --- | --- | --- | ---: |
| Nano | 否 | 否 | 静态 Destination Route；诊断 Origin=0 | 3496 B |
| Lite | 是 | 否 | 多 Origin Route Instance | 6952 B |
| Full | 是 | 是 | 多 Origin Route + Candidate 激活事务 | 11152 B |

对 Layout 8 的增量：

- Nano：0 B；
- Lite：+64 B；
- Full：+72 B。

Layout 9 在 Layout 8 上增加 Origin 所有权，增量为 Nano/Lite/Full `0/+64/+72 B`。
Layout 10 又为 Full 的 8 个 Candidate 增加 ACK 状态、尝试次数、总截止期、重试时刻和
四项诊断统计，使 Full 再增加 144 B；Lite/Nano 不编译 Candidate Routing，因此不变。
Layout 11 为 Candidate 增加显式的每跳路径证明冻结标志。该字段落入既有结构对齐空隙，
Host Nano/Lite/Full 的 `sizeof` 仍为 `3496/6952/11152 B`，但字段语义和布局版本已经改变。
这些数字是 Host ABI 的 `sizeof(ucn_node_t)`，不是 ESP32/STM32 的最终 RAM、Task 栈、
Driver Ring 或 SDK Heap。

## 10. Storage/API 兼容性

Core Wire v5 没有改变，RREQ/RREP/RERR Payload 也没有改变；已有 Source、Destination 和不可达目标已经足以表达所有权。

Node Storage Layout 在 V5-64 初版从 8 升到 9，有界 Activate/ACK 事务升到 10，每跳
Candidate 路径证明冻结再升到 11。
这是本地 ABI/存储布局破坏性变化：

- 库与应用必须用同一配置头全量重编；
- 不能把 Layout 8/9/10 的 `ucn_node_t` 内存交给 Layout 11 的库；
- Node 状态是易失状态，升级后重新初始化；
- 本次不改变 Cluster 持久化 Record schema。

新增只读 API：

```c
size_t ucn_node_copy_route_summaries(
    const ucn_node_t *node,
    ucn_route_summary_t *output,
    size_t capacity);
```

先以 `(NULL,0)` 查询有效数量，再传入调用者数组。动态项显示真实 `route_origin`；静态项显示 0。API 只在 Owner 上读取当前有界快照，不保证之后拓扑不变。

## 11. 软件回归

`test_multi_origin_route_epoch_isolation()` 使用 A、B、R、X、T 五节点：

1. A 和 B 的 Discovery 同时处于 pending；
2. A 使用 2→4 Ring，B 使用 2→3 Ring；
3. RREP 被暂存后按 B→A 逆序交付；
4. 中继同时保存 `(A,T)` 与 `(B,T)`，且 Epoch 不同；
5. A、B 业务都送达 T；
6. 把 B Epoch 塞入 A 业务，目标明确拒绝且不交付；
7. Full 中 A、B 复用相同 Candidate ID，Candidate 仍分离；
8. Candidate 激活复用已过期的外域 Route 槽时，不继承外域 Previous Epoch；
9. 尾链路 Down 触发 A 的 RERR，只删除 A 域；B 随后仍可送达。
10. 动态 Origin 的业务经过静态中继和目标静态反向 Route，携带 Epoch 仍可交付；
11. Candidate 激活覆盖单槽不足零副作用、下游发送失败回滚、目标 ACK 背压回滚、成功
    重试与精确幂等。
12. 错 Epoch、未发送、Probe 未完成和非本地发起的 Activate ACK 均零写拒绝；
13. ACK 丢失后按 250 ms 重发，相同 Candidate ID/Epoch、不同外层 Sequence；后续 ACK
    可收敛，ACKED 后停止重发；
14. 默认 3 次重试耗尽或 1000 ms 总截止期到达后清 Candidate，旧 Active Route 不变；
15. 中继 Candidate 首次绑定 Epoch 后，换 Epoch Activate 零写拒绝；
16. 等待 ACK、Probe 完成但尚未 Activate、以及本地 Activate 提交失败后三种状态下，
    同 ID 更优 RREP 均不能迁移冻结路径或复用旧证明。

`test_route_instance_table_full_is_fail_closed()` 用静态 Route 填满表，再向中继注入合法 RREQ，验证 `NO_SPACE`、表不写回、满载统计和诊断数量。

现有 V5-63 回归继续覆盖单 Origin 扩展环 Epoch 对齐与 Previous Grace；动态压力和规模模拟的 Route 遍历也已改成携带 Origin 域，防止测试工具用旧模型掩盖错误。

## 12. 当前验证结果

| 门禁 | 结果 |
| --- | --- |
| Windows GCC Full Debug | 58/58 |
| Windows GCC Lite Debug | 58/58 |
| Windows GCC Nano Debug | 48/48 |
| Windows GCC Full Release | 58/58 |
| Windows GCC Full Service OFF | 58/58 |
| Windows MSVC 2019 Full Debug | 58/58 |
| Windows Ninja 中文源码目录 + 中文构建目录 Full Debug | 58/58 |
| WSL GCC ASan/UBSan | 60/60 |
| WSL GCC `-fanalyzer -Werror` | 60/60 |

上述矩阵已在最后一次 Activate/ACK 绑定、重试状态机、测试和文档收口后重新构建并运行。
中文路径门禁固定使用 Ninja；本机 MinGW Makefiles 生成器本身不能可靠处理中文源码根
路径，不把该生成器限制误记为 UCN Runtime 缺陷。

## 13. 尚未证明的内容

本轮不能被解释为：

- 多块 ESP32 的 UART/ESP-NOW 多源并发已经实测；
- 超过固定 Route/Candidate 容量仍可无损运行；
- RREQ 广播风暴、RF 冲突、CPU、功耗或长稳已经通过；
- Control Plane 逐跳认证已经生产完成；
- 任意数量 Origin 都能同时共享一个小内存中继。

下一阶段实机应至少让两块源板同时经一个中继访问同一目标板，使用不同 Ring/速率，注入 RREP 延迟、尾链路断开和恢复，并从中继导出 `ucn_node_copy_route_summaries()` 与 `route_instance_table_full`。

## 14. 自审结论

软件范围内，V5-64 已把 Route、Candidate、转发 Epoch、目标 Epoch 接收、RREQ/RREP、RERR、刷新、失效、诊断和模拟工具统一到同一个 Source-owned Route Instance 合同。Wire 不变，Nano 边界不变，容量满失败关闭。

正式状态保持：**Host 软件自审完成；按用户要求暂缓单项外审，等待 v6 全任务完成后的统一
外审与多板硬件验证**。在这些门禁完成前，不将其写成生产放行结论；V6-01 只能把它保存为
“自审开发快照”，不能命名或描述为 v5 正式发布版。
