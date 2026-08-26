# UCN v4 节点快照诊断

> 状态：**v4 C99 Core 已实现、虚拟拓扑已验证**；真实 ESP-NOW/WiFi/CAN/UART 实机行为仍待验证。  
> 协议版本：`UCN_PROTOCOL_VERSION = 4`；v3 与 v4 不在同一 Network ID 内直接互通。  
> 关联：[整体架构](../../02-总体架构/UCN_整体架构设计.md) · [协议档案](../../01-入门与使用/UCN_协议分层与配置档案.md) · [路径追踪诊断](../../04-路由与链路/UCN_路径追踪诊断建议.md) · [任务表](../../00-项目管理/00-任务表.md)。

## 1. 解决的问题与边界

`ucn_node_request_node_snapshot()` 让被授权的管理节点按需查询**当前与自己连通、且在查询窗口内成功回复**的 Node 列表。它是台架调试和运维诊断能力，不是周期性发现协议，也不让 MCU 保存完整网络拓扑。

它返回 Node ID 和该节点当时的直接已注册 Link 数量；不返回 MAC、密钥、完整邻接表、业务 Endpoint 或全局最短路径。没有回包只表示本次窗口未观察到回复，不能直接断言该节点永久离网。

`PATH_TRACE` 与它的职责不同：前者查询一个已知目标的当前缓存路径；Node Snapshot 查询当前连通域中所有响应节点。两者都不会锁定业务路径、改变 Route Cache 或触发普通业务 RREQ。

## 2. 线协议

| 项目 | 值/规则 |
| --- | --- |
| 请求 | `NODE_SNAPSHOT_REQ = 0x1A`，目的地址 `UCN_NODE_BROADCAST`。 |
| 回包 | `NODE_SNAPSHOT_REPLY = 0x1B`，目的地址为请求源 Node。 |
| Flag / QoS | 必须 `DIAGNOSTIC=0x04`、Q1；控制帧不得带 `E2E_PROTECTED`，不得带 Route Extension。 |
| Request Payload | 固定 8 B：`query_id(4)` + 4 B 保留（必须为零）。 |
| Reply Payload | 固定 12 B：`query_id(4)` + `node_id(4)` + `direct_link_count(1)` + `flags(1)` + 2 B 保留（必须为零）。 |
| 版本边界 | 帧头版本已升至 4；v3 帧由解码器返回 `UCN_ERR_VERSION`。五节点必须同批升级后才能参与 Snapshot。 |

`node_id` 同时存在于 Reply 的帧头 Source 和 Payload 中；Core 要求两者相等，防止中继或错误实现将回复归属到错误节点。

## 3. 请求、泛洪与反向回包

```text
A（管理节点）
  └─ Snapshot_REQ(query=42, 广播)
       └─ 每个节点首次收到：ACL → 限流 → 建立临时 Reverse → 排队自身 Reply → 向其他一跳 Link 转发一次

B / C / D
  └─ Snapshot_REPLY(query=42, self Node ID, direct Link count)
       └─ 沿 (origin, query_id) 的临时 Reverse 回送 A

A
  └─ 固定结果数组按 Node ID 去重；窗口结束后一次回调 COMPLETE 或 TRUNCATED
```

请求在每个节点由固定 Duplicate Source Window 按 `(source, session, sequence)` 去重，因此有环网络中每个节点只首次处理和转发一次。中继不需要到 A 的普通 Route Cache：只保存本次请求来自的上一跳。Reverse 项在超时后自动回收，并可承载多个下游 Reply，不能像单次 `PATH_TRACE_REPLY` 一样首包后立即释放。

## 4. 固定资源、调度和权限

| 资源/门限 | 默认值 | 作用 |
| --- | --- | --- |
| `UCN_NODE_SNAPSHOT_MAX_RESULTS` | 8 | 仅请求源保存的最大节点结果数。 |
| Pending / Reverse / Reply Queue | 1 / 2 / 2 | 源端一个活动查询；中继短期回程状态与随机延迟回包。 |
| 查询窗口 | 2 s | 到期后交付一次结果并释放状态。 |
| 诊断 Token | 突发 1、10 s 补 1 | Snapshot 与 Path Trace、Q0 控制预算彼此独立。 |
| Reply 抖动 | 0～200 ms | 由 `query_id ^ node_id` 确定，减少多节点同时回复。 |

正常 Q0、Q1、Heartbeat 与 Path Probe 均优先于已到期的 Snapshot Reply。Snapshot 从不进入普通业务队列，也不占用 HELLO/Heartbeat/RREQ 的 Q0 控制 Token。

远端处理默认关闭：`node_snapshot_authorize == NULL` 时请求被拒绝。产品必须通过 `ucn_node_set_node_snapshot_authorizer()` 明确配置允许发起诊断的管理 Node ID；已有 Security Provider 的 `authorize_rx()` 仍在该检查之前执行。

## 5. API 与验证边界

```c
ucn_node_set_node_snapshot_authorizer(node, allow_manager, context);
ucn_node_request_node_snapshot(node, 0U, on_snapshot, context);
```

`result_limit=0` 使用编译期上限；较小值只限制源端结果表，超出时回调状态为 `TRUNCATED`。回调包含自身条目和已收集的唯一远端 Node；它只在 2 s 查询窗口结束时调用一次。

`tests/test_node_snapshot.c` 已覆盖四节点 `A→B→{C,D}` 的受限泛洪、所有 Reply 经 Reverse 回源、结果去重、固定结果表截断、低频 Token 与默认 ACL 拒绝。Debug、Release 和 64 B Profile 的通过只能证明 C99 虚拟 Link 闭环；后续五块 ESP32 必须测量实际收集率、空口负载、窗口时延、干扰、断链和重组网。
