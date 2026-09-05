# Full 架构、函数与调用顺序

## 1. Full 是什么

Full 在 Lite 的动态 Mesh 和 Security 基础上增加：

- 多条 Candidate Route 的保存、探测、验证和激活；
- 显式 Path 的逐跳安装、能力绑定、转发、撤销和过期；
- 针对 destination + endpoint + traffic class 的 Route Policy；
- Pinned Strict、Pinned Failover、Auto Best、Auto Balance；
- Q1 Flow 绑定和租约，避免每帧无规律换路；
- Link Quality、Path Egress 和 Policy 统计；
- Path Trace、Node Snapshot、Policy Diagnostic；
- 对管理诊断和 Path Control 的显式授权与预算。

Full 适合一个节点具有多条 UART/CAN/Wi-Fi/USB 等 Bearer，需要自动选路、指定路径、故障回退或负载均衡的产品。

### 本文怎样覆盖 Full 的全部函数

Service=ON 的当前基线中，Full 由公共层 **211** 个函数定义、完整 Node **294** 个函数定义、Path **7** 个函数定义和 Policy **33** 个函数定义组成，共 **545** 个函数定义。本文从架构和调用链解释这些函数为什么存在、参数怎样传递；[公共基础层](01-公共基础层架构与函数.md)解释三档共用部分；[函数签名与源码位置索引](08-函数签名与源码位置索引.md)逐项列出 545 个定义的完整参数、可见性和行号。数量包含内部 `static` helper，不等于公共 API 数量。

## 2. 实际构建架构

Full 链接：

```text
src/node/ucn_node.c
src/routing/ucn_path.c
src/routing/ucn_policy.c
```

不会链接 `ucn_profile_stubs.c`。公共高级 API 都有真实实现。

```text
Node
 ├─ Link[]
 ├─ Neighbor[] → Bearer[]
 ├─ Active Route[]
 ├─ Candidate Route[]
 ├─ Path State[]
 ├─ Policy State
 │   ├─ Route Policy[]
 │   ├─ Local Policy Path[]
 │   ├─ Q1 Flow Binding[]
 │   └─ Link Quality Snapshot[]
 ├─ Diagnostic Pending/Reverse/Reply[]
 ├─ Security / Replay / Control Budget
 ├─ Q0～Q3 Queue
 └─ Endpoint Handler[]
```

需要特别区分：

- **Route**：按目标寻址得到的下一跳；
- **Candidate Route**：尚未取代活动 Route 的候选；
- **Path Forward Entry**：带 owner/session/path ID 的显式逐跳状态；
- **Policy Path**：应用侧本地 handle，绑定到已经验证的 Wire Path；
- **Q1 Flow Binding**：一段时间内让同一目标/Endpoint 继续使用同一 Policy Path。

## 3. Full 继承的函数

Full 原生实现 Lite 文档中的全部函数：

- 初始化与 Wire Profile；
- Security；
- Neighbor/多 Bearer；
- 静态 Route 与 AODV-Lite；
- Endpoint/Queue/Send/Receive/Step。

这些函数签名相同，但内部会进入额外的 Candidate、Path、Policy 和 Diagnostic 分支。不要只看 Lite 调用链就推断 Full 一定相同。

## 4. Full 新增的 Authorizer API

```c
ucn_result_t ucn_node_set_node_snapshot_authorizer(
    ucn_node_t *node,
    ucn_node_snapshot_authorize_fn authorize,
    void *context);

ucn_result_t ucn_node_set_path_trace_authorizer(
    ucn_node_t *node,
    ucn_path_trace_authorize_fn authorize,
    void *context);

ucn_result_t ucn_node_set_policy_diagnostic_authorizer(
    ucn_node_t *node,
    ucn_policy_diagnostic_authorize_fn authorize,
    void *context);

ucn_result_t ucn_node_set_path_control_authorizer(
    ucn_node_t *node,
    ucn_path_control_authorize_fn authorize,
    void *context);
```

共同参数：

- `node`：唯一 Owner 管理的 Node；
- `authorize`：产品决定管理源、Session、目标和请求是否允许；
- `context`：产品上下文。

这些 setter 只安装授权边界，不会自动允许远端管理。`authorize == NULL` 的准确语义必须对照当前实现和测试，审计时不能默认“NULL 等于允许”。

## 5. Path State 低层函数

源文件：`src/routing/ucn_path.c`。

| 函数 | 参数 | 作用 |
| --- | --- | --- |
| `ucn_path_is_expired(entry, now_ms)` | Path entry、当前时间 | 按回绕安全 deadline 判断过期 |
| `ucn_path_find(state, owner, owner_session_id, path_id, destination)` | Path 表和完整 Identity | 查找尚未过期的精确 Path |
| `ucn_path_install(state, config)` | Path state、基础配置 | 安装不带显式能力的 Path |
| `ucn_path_install_capable(state, config, capability)` | state、配置、Wire/MTU 能力 | 安装能力绑定 Path |
| `ucn_path_revoke(state, owner, owner_session_id, path_id, destination)` | 完整 Identity | 精确撤销，不能只凭 path ID 删除 |
| `ucn_path_expire(state, now_ms)` | state、当前时间 | Step 中清除过期项 |

`owner_session_id` 是 Path Identity 的一部分，目的是防止节点重启后旧 Path ID 在新 Session 中被误用。

## 6. Node Path Control API

### 6.1 本地安装和撤销

```c
ucn_result_t ucn_node_install_local_path(
    ucn_node_t *node,
    ucn_path_id_t path_id,
    ucn_node_id_t destination,
    ucn_node_id_t next_hop,
    uint8_t remaining_hops,
    uint32_t lease_ms);

ucn_result_t ucn_node_install_local_path_capable(
    ucn_node_t *node,
    ucn_path_id_t path_id,
    ucn_node_id_t destination,
    ucn_node_id_t next_hop,
    uint8_t remaining_hops,
    uint32_t lease_ms,
    const ucn_path_capability_t *capability);

ucn_result_t ucn_node_revoke_local_path(
    ucn_node_t *node,
    ucn_path_id_t path_id,
    ucn_node_id_t destination);
```

- `path_id`：本节点源侧 Path ID；
- `destination`：最终目标；
- `next_hop`：当前节点的下一站；
- `remaining_hops`：当前节点以后还允许经过的跳数；
- `lease_ms`：Path 生存期；
- `capability`：整条 Path 的最大 Wire Profile、最小 MTU 等瓶颈能力；NULL 时只推导本地下一跳。

本地安装只创建当前节点的一段；中继节点不会因为源节点本地调用而自动拥有转发表。

### 6.2 发送管理控制帧

| 函数 | 参数 | 说明 |
| --- | --- | --- |
| `ucn_node_send_path_install(node, control_target, path_id, destination, next_hop, remaining_hops, lease_ms)` | 被管理节点和基础 Path Schema | 发送稳定基础格式 |
| `ucn_node_send_path_install_capable(..., capability)` | 同上加能力 | 只对确认支持扩展 Schema 的目标使用 |
| `ucn_node_send_path_revoke(node, control_target, path_id, destination)` | 被管理节点、Path Identity | 请求远端撤销 |
| `ucn_node_find_path_forward(node, owner, owner_session_id, path_id, destination)` | 完整 Wire Path Identity | 查询当前转发表项 |
| `ucn_node_send_path(node, destination, message_type, traffic_class, path_id, payload, payload_length)` | 目标、业务类型、QoS、Path、payload | 低层/测试或 Policy 已解析后的显式 Path 发送 |

Path Control 接收时必须先通过结构、来源、Authorizer、Token、能力和容量门，再写 Path 表。

## 7. Policy API

源文件：`src/routing/ucn_policy.c`。

### 7.1 Route Policy

| 函数 | 参数 | 作用 |
| --- | --- | --- |
| `ucn_node_set_route_policy(node, config)` | key + mode + path/failover 配置 | 新增或更新策略 |
| `ucn_node_clear_route_policy(node, key)` | destination + endpoint + traffic class | 精确清除 |
| `ucn_node_find_route_policy(node, destination, endpoint, traffic_class)` | 策略 Key | 返回只读 Entry 或 NULL |

策略 key 不是只有目标 Node；同一目标上不同 Endpoint/QoS 可以采用不同路线。

### 7.2 Policy Path

| 函数 | 参数 | 作用 |
| --- | --- | --- |
| `ucn_node_set_policy_path(node, config)` | 本地 handle、Wire Path、目标、状态等 | 建立应用可引用的本地 Path |
| `ucn_node_clear_policy_path(node, local_path_id)` | 本地 handle | 清理并使相关 Flow 失效 |
| `ucn_node_find_policy_path(node, local_path_id)` | 本地 handle | 查询只读 Entry |

### 7.3 Q1 Flow

| 函数 | 参数 | 作用 |
| --- | --- | --- |
| `ucn_node_bind_q1_flow(node, destination, endpoint, local_path_id, lease_ms)` | Flow key、Path handle、租约 | 显式建立流粘滞 |
| `ucn_node_find_q1_flow(node, destination, endpoint)` | Flow key | 查询当前绑定 |
| `ucn_policy_touch_q1_flow(state, destination, endpoint, now_ms)` | Policy state、Flow key、时间 | 内部发送成功后续租/更新时间 |
| `ucn_policy_expire_flows(state, now_ms)` | Policy state、时间 | Step 清除过期 Flow |

### 7.4 质量和 Path 状态

| 函数 | 参数 | 作用 |
| --- | --- | --- |
| `ucn_policy_refresh_link_quality(state, links, link_count, now_ms)` | Link 指针数组、数量、时间 | 读取实时 metrics 并刷新 LC-1 快照 |
| `ucn_policy_refresh_path_egress(state, local_path_id, active_egress_link, path_available)` | Path handle、当前出口、可用性 | 同步逻辑 Path 与当前 Bearer |
| `ucn_policy_mark_path_down(state, local_path_id)` | Path handle | 硬失效并触发相关 Flow 处理 |
| `ucn_node_get_link_quality(node, link)` | Node、Link | 返回质量快照 |
| `ucn_node_get_policy_stats(node)` | Node | 返回 Policy 统计 |

## 8. 高级诊断 API

```c
ucn_result_t ucn_node_request_path_trace(
    ucn_node_t *node,
    ucn_node_id_t destination,
    uint8_t record_limit,
    ucn_path_trace_handler_t handler,
    void *context);

ucn_result_t ucn_node_request_node_snapshot(
    ucn_node_t *node,
    uint8_t result_limit,
    ucn_node_snapshot_handler_t handler,
    void *context);

ucn_result_t ucn_node_request_policy_diagnostic(
    ucn_node_t *node,
    ucn_node_id_t destination,
    ucn_policy_diagnostic_section_t section,
    uint8_t index,
    ucn_policy_diagnostic_handler_t handler,
    void *context);
```

参数语义：

- `record_limit/result_limit`：调用者接受的有界结果数量，不代表永久全网拓扑；
- `section/index`：Policy 固定表分页/槽位选择；
- `handler/context`：异步结果回调，运行于 Owner；
- 请求占用固定 Pending/Reverse 槽并受独立管理预算限制。

诊断是按需控制流，不能进入普通高频业务路径。

## 9. Full 内部函数怎样分区

当前 Full 的 `ucn_node.c` 编译出约 55 个公共定义和 239 个静态 helper；再加 `ucn_path.c` 和 `ucn_policy.c`。推荐按以下子系统定位，精确签名见索引。

### 9.1 Lite 共用子系统

- Wire 控制 Payload；
- Neighbor/Bearer；
- Security/Replay；
- RREQ/RREP/RERR；
- Queue、Heartbeat 和 Route Maintenance。

先按 Lite 文档读一遍这些部分。

### 9.2 Candidate Route

代表函数：

```text
candidate_is_expired
candidate_is_sufficiently_better
candidate_route_is_locally_better
find_candidate_route
learn_candidate_route
activate_candidate_route
expire_candidate_routes
find_candidate_link
send_due_candidate_probes
handle_path_probe / handle_path_probe_ack / handle_path_activate
```

调用逻辑：先保存 Candidate → 多次 Probe → ACK/RTT/Cost 验证 → 分配新 route_epoch → 激活为活动 Route。收到一个看起来更低的 Cost 不应立即切路。

### 9.3 Path Forward

代表函数：

```text
path_capability_is_valid
resolve_path_capability
install_path_from_control
handle_path_install
handle_path_revoke
find_path_forward
resolve_path_egress
forward_path_frame
```

每跳都验证 owner/session/path/destination、remaining hops、next hop、Link 能力和 MTU。

### 9.4 Policy 选择

代表流程：

```text
find_route_policy
  → select policy mode
  ├─ PINNED_STRICT: 只用指定Path，失败直接返回
  ├─ PINNED_FAILOVER: Path失败后自动Route
  ├─ AUTO_BEST: 当前最低有效Cost
  └─ AUTO_BALANCE: 候选集合 + Flow粘滞
```

Policy 选择出的是本地逻辑 Path/Route，真正发送前仍需重新解析当前 Bearer 状态和 MTU。

### 9.5 Diagnostic

代表函数族：

```text
path_trace_* request/reply/reverse
node_snapshot_* request/reply/reverse
policy_diagnostic_* request/reply
expire_*_pending
take_*_diagnostic_token
```

## 10. Full 发送调用链

### 10.1 Endpoint + Policy

```text
ucn_node_send_endpoint
  → ucn_endpoint_is_static
  → 查 route_policy(destination, endpoint, traffic_class)
  ├─ 无Policy/AUTO_BEST
  │    → send_endpoint_auto_best
  ├─ PINNED_STRICT
  │    → local_path_id → verified wire_path_id
  │    → ucn_node_send_path
  ├─ PINNED_FAILOVER
  │    → 先Path，失败条件允许时再自动Route
  └─ AUTO_BALANCE(Q1)
       → 查/建flow binding
       → 选可用Policy Path
       → 发送成功后touch flow
  → protect_outbound_business
  → prepare_outbound_wire_profile
  → send_frame_on_logical_egress
  → send_frame_on_link
```

### 10.2 Candidate 切换

```text
收到更优RREP
  → learn_candidate_route
  → Step发送Path Probe
  → 收到足够Probe ACK
  → 分配route_epoch
  → PATH_ACTIVATE
  → activate_candidate_route
  → 更新活动Route和Policy egress
```

## 11. Full 接收调用链

Full 在 Lite RX 链上增加：

```text
decode/security/replay后
  ├─ PATH_PROBE / ACK / ACTIVATE → Candidate验证
  ├─ PATH_INSTALL / REVOKE      → Authorizer+Token+Path State
  ├─ Path业务帧                 → 查完整Path Identity并逐跳转发
  ├─ TRACE                       → 有界记录和反向结果
  ├─ SNAPSHOT                    → 受限广播/反向槽/多Reply
  └─ POLICY_DIAGNOSTIC           → section/index有界查询
```

所有管理帧都不能绕过普通 Wire、Network、Hop、Security 和来源校验。

## 12. Full Step 调用链

在 Lite 维护基础上增加：

```text
ucn_node_step
  → expire_candidate_routes
  → send_due_candidate_probes
  → ucn_path_expire
  → policy_refresh_link_quality
  → policy_refresh_path_egress
  → policy_expire_flows
  → expire diagnostic pending/reverse slots
  → Q0～Q3 与 Essential maintenance
```

Full 的性能审计重点不是单个函数多快，而是一次 Step 的所有子系统是否都有固定预算，是否可能因大量 Link/Route/Path 同时到期导致长时间占用 Owner。

## 13. 推荐源码阅读顺序

```text
1. 先完整读 Lite 文档与 Lite 主链
2. ucn_node_storage.h 中 Candidate/Path/Policy条件字段
3. ucn_path.c 全文件
4. ucn_policy.c 的公共API和选择helper
5. ucn_node.c 的 learn_candidate_route
6. Probe/Activate控制帧
7. Path install/revoke/forward
8. ucn_node_send_endpoint 的Policy分支
9. 三类Diagnostic request/reply
10. ucn_node_step 的Full-only维护
```

## 14. Full 审计重点

| 风险 | 必须回答的问题 |
| --- | --- |
| Candidate 切换 | 是否经过连续质量、Probe ACK、RTT/Cost和route_epoch验证 |
| Path Identity | owner/session/path/destination 是否所有入口一致绑定 |
| Path 能力 | 每跳 Wire Profile/MTU 是否同时满足；异构链断开是否 RERR/撤销 |
| Strict Policy | 指定 Path 不可用时是否错误自动回退 |
| Balance | 是否只作用于允许的 Q1；Flow 是否在 lease 内稳定 |
| 动态 Cost | Unknown、陈旧、硬失效、EWMA和滞回顺序是否一致 |
| 诊断授权 | 结构合法但未授权的请求是否零状态写入 |
| 固定槽满 | Pending/Reverse/Policy/Path 表满时是否 fail-closed、不淘汰安全状态 |
| Step 预算 | 多表同时满载是否仍有界，不饿死 Q0 |

主要测试：`test_candidate_route.c`、`test_path_control.c`、`test_path_management_budget.c`、`test_path_trace.c`、`test_node_snapshot.c`、`test_policy.c`、`test_policy_diagnostic.c`、动态 Cost、Scale 和跨 Profile 测试。
