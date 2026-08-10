# UCN 路径追踪诊断（T23）

> 状态：**Core 已实现，实机待验证**。本文件描述 UCN v4 的按需路径查询能力；普通业务帧、普通 Route Cache 和应用发送 API 不因该功能增加常态开销。  
> 目标：本节点显式查询远端 Node 时，返回**该次 Trace 请求实际经过的 Node ID 列表**。这是一时刻的路径快照，不锁定后续业务路径。  
> 关联：[整体架构](UCN_整体架构设计.md) · [任务表](00-任务表.md) · [路径缓存与自动选路建议](UCN_路由缓存与自动选路建议.md)。

## 1. 线协议与兼容边界

新增两个控制消息和一个诊断 Flag：

```c
UCN_MSG_PATH_TRACE_REQ   = 0x18
UCN_MSG_PATH_TRACE_REPLY = 0x19
UCN_FRAME_FLAG_DIAGNOSTIC = 0x04
```

- `DIAGNOSTIC` Flag 本身不改变 Trace 控制帧的 32 B/36 B 头尺寸；控制帧禁止 Path ID，因此不会使用 40 B Path Header。
- v4 Core 将 Trace 消息视为控制帧；v3 帧会先在版本检查阶段明确拒绝，不会被当作普通业务交给应用或错误转发。
- 因而路径上的所有中继都必须运行 v4 并支持 T23，Trace 才能成功；旧节点不能与 v4 节点在同一 Network ID 内通信。
- Trace 控制帧不允许 `E2E_PROTECTED`：中继必须能读取、追加 Node ID 并按反向表返回。生产网络是否允许诊断由 Provider/ACL 决定；T23 不声称提供拓扑保密。

## 2. Payload 与容量

Request 与 Reply 使用同一固定前缀，后接 Node ID 列表：

```text
0..3   trace_id       uint32，源节点本地唯一
4      record_count   当前记录数量
5      record_limit   本次最多允许记录的 Node 数
6      status         Request 为 0；Reply 返回结果状态
7      reserved       必须为 0
8..    node_ids       record_count × uint32，大端
```

源节点先写入自身；每个中继和目标节点各追加一次自身 Node ID。目标收到后保留完整列表生成 Reply。状态至少包含：

| 状态 | 含义 |
| --- | --- |
| `OK` | 到达目标，列表完整。 |
| `NO_ROUTE` | 某中继无法选择下一跳。 |
| `TTL_EXCEEDED` | 请求已无可用 Hop Limit。 |
| `TRUNCATED` | 编译期/调用方记录上限不足，列表不完整。 |
| `TIMEOUT` | 仅源节点本地结果：等候 Reply 超时。 |

`UCN_PATH_TRACE_MAX_NODES` 按当前 `UCN_MAX_FRAME_BYTES` 推导并受 `UCN_MAX_HOPS + 1` 限制。64 B Profile 中基础 32 B 头 + 8 B 前缀最多记录 6 个 Node ID（5 Hop）；若实际路径更长，必须返回 `TRUNCATED`，不得静默省略节点。

## 3. 转发与返回流程

```text
A 请求 C：REQ(id=7, [A])
  ↓ 当前 Route Cache
B：追加 B；确定下一跳后写入反向临时项 (A,7) → ingress=A；转发 [A,B]
  ↓ 当前 Route Cache
C：追加 C；构造 REPLY(id=7, OK, [A,B,C])
  ↓ 按临时反向项
B：REPLY 从 C 入站，查 (A,7) → 送回 A
  ↓
A：匹配本地 Pending Trace，调用结果回调
```

每个中继的反向项为固定容量，仅保存：`origin`、`trace_id`、`ingress_link`、过期时间。它不保存完整路径，也不依赖 C 到 A 的普通 Route Cache，因此 Reply 尽量沿 Request 的反向链返回。中继表和源端 Pending 表都在固定超时后复用。

### 3.1 无路由时的行为

默认 API 是 `CACHE_ONLY`：源没有现有直连/Route Cache 时立即返回 `UCN_ERR_NOT_FOUND`，不会为了诊断触发 RREQ。初版不加入“查询自动寻路”模式，避免诊断接口产生意外控制洪泛；需要时应用可先显式执行既有寻路，再发 Trace。

### 3.2 中途失败

中继在追加自身后若发现无下一跳或 TTL 用尽，直接通过当前入站 Link 向上一跳返回相应状态和已收集的部分路径；上游中继再查 Reverse 表继续回送。若记录容量已经不足，则保留已有列表并通过当前入站 Link 返回 `TRUNCATED`（当前中继不会被写入列表）。只有确定能向下一跳转发时才分配 Reverse 项。若反向 Reply 自身发送失败，源端最终以本地超时结束。

## 4. API、调度与安全边界

建议公开固定结果类型与回调：

```c
ucn_node_request_path_trace(node, destination, record_limit, callback, context);
```

- `record_limit=0` 表示使用当前 `UCN_PATH_TRACE_MAX_NODES`；非零值可让调用方主动限制诊断载荷，达到上限返回 `TRUNCATED`。回调一次性拿到 `status`、`trace_id`、`node_count` 和固定数组 `node_ids[]`；不分配堆内存。
- Trace 仅允许单播，使用独立的、低频的诊断 Token；不占用 Q0 关键控制预算，也不应抢占 Q0/Q1 业务队列。
- 当前 Core 固定编入这组按需诊断代码，但不会自行发送 Trace；产品若要进一步裁剪代码，需要在目标 Profile 增加专门的构建开关，当前仓库尚未提供该开关。安全 Provider 存在时，既有入站 `authorize_rx()` 必须先通过；此外每个中继/目标必须显式安装 `ucn_node_set_path_trace_authorizer()`，默认 `NULL` 拒绝远端 Trace。通过授权后仍受“直连对端 + Trace 类型”的独立入站令牌桶限制，授权失败不会分配 Reverse 状态。
- Trace 不是路径锁定或负载均衡机制。T22.2～T22.5 已提供可安装的业务 Path、固定主备、Q1 流亲和均衡和同 Neighbor Bearer 联动，但 Trace 结果不会自动创建、更新或选择任何 Path；只有已配置 Policy 的 `PINNED_*` 或 `AUTO_BALANCE` Endpoint 才按其规则发送，其他业务仍按当时 Active/Previous Route 与 Cost 规则发送。

## 5. T23 实施门禁

| 子任务 | 内容 | 单元/模拟验证 |
| --- | --- | --- |
| T23.1 | 已完成：新增 Flag、消息类型、固定 Trace/Pending/Reverse 数据结构、API 与统计。 | `DIAGNOSTIC` 编解码、未知 Flag 拒绝、参数/表满/超时。 |
| T23.2 | 已完成：Request 逐跳追加、目标 Reply、反向临时表与中途失败 Reply。 | A→B→C 返回 `[A,B,C]`；无路由、TTL、调用方限额截断、反向 Reply。 |
| T23.3 | 已完成：控制/安全/调度联动。 | E2E Trace 拒绝、未准入 Link 拒绝、显式 Authorizer 默认拒绝、按直连对端/类型入站预算与源端低频预算；全量 Q0 回归。 |
| T23.4 | 已完成：全量回归与 Profile 验证。 | Debug、Release、64 B Profile、现有 v4/路由/安全/Adapter 测试全绿。 |
| T23.5 | 实机验证。 | 至少三板 A→B→C 查询、重启/断链/重组网、Trace 时延和控制开销。 |

T23.1～T23.4 完成只代表 C99 虚拟拓扑闭环；T23.5 前不得宣称已在 ESP-NOW/WiFi/CAN/UART 实网可靠观察完整路径。
