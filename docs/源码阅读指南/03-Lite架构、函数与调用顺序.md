# Lite 架构、函数与调用顺序

## 1. Lite 是什么

Lite 是“单条活动路由的动态 Mesh”档次。相较 Nano，它增加：

- HELLO、邻居候选、准入和拒绝；
- 一个 Neighbor 下的多 Bearer 生命周期与主链切换；
- Heartbeat、Suspect、Remove；
- AODV-Lite RREQ/RREP/RERR；
- Route Cache、Expanding Ring、Route Constraints 和质量查询；
- Security Provider、全局/Endpoint Policy、Session 与重放保护。

Lite 不编译 Candidate Route、显式 Path、Policy/负载均衡和高级诊断。它适合需要自动组网和安全接口，但不需要多候选路径管理的 MCU。

### 本文怎样覆盖 Lite 的全部函数

Service=ON 的当前基线中，Lite 由公共层 **211** 个函数定义、条件裁剪后的 Node **161** 个函数定义和 Profile Stub **36** 个函数定义组成，共 **408** 个函数定义。本文按职责解释公共入口、参数和内部调用顺序；[公共基础层](01-公共基础层架构与函数.md)解释三档共用部分；[函数签名与源码位置索引](08-函数签名与源码位置索引.md)逐项列出这 408 个定义的完整参数、可见性和行号。数量包含 `static` helper 和明确失败的 Stub，不代表 408 个业务 API。

## 2. 实际构建架构

Lite 使用：

```text
src/node/ucn_node.c          由 UCN_FEATURE_* 条件裁成 Lite
src/node/ucn_profile_stubs.c Path/Policy/Diagnostics 明确失败
```

不会链接：

```text
src/routing/ucn_path.c
src/routing/ucn_policy.c
```

对象关系：

```text
Node
 ├─ Link[]                    产品注册的物理/逻辑通道
 ├─ Neighbor[]
 │   └─ Bearer[]              同一 Peer 可有 UART/CAN/Wi-Fi 等多条链
 ├─ Route[]                   每目标一条活动 Route
 ├─ RREQ Cache / Discovery[]  动态发现和去重
 ├─ Duplicate/Replay state
 ├─ Security Provider/Policy
 ├─ Q0～Q3 Queue
 └─ Endpoint Handler[]
```

Lite 的关键区别是：可以动态学到 Route，但不保留 Full 的独立 Candidate 表，也不让应用安装显式 Path。

## 3. 公共函数分组

Lite 的公共 Node 函数可以分成九组。精确签名与源码行号见 [函数签名索引](08-函数签名与源码位置索引.md)。

### 3.1 初始化与 Wire

| 函数 | 参数核心 | 调用阶段 |
| --- | --- | --- |
| `ucn_node_init(node, config)` | Node 静态对象、基础配置 | 第一项 |
| `ucn_node_set_wire_profiles(node, tx_profile, max_receive_profile)` | 固定 TX 档、本地 RX ceiling | 注册 Link 前 |
| `ucn_node_get_tx_wire_profile(node)` | Node | 查询 |
| `ucn_node_get_max_receive_wire_profile(node)` | Node | 查询 |
| `ucn_node_set_wire_profile_auto(node, enabled)` | 自动最小档开关 | 流量前 |
| `ucn_node_wire_profile_auto(node)` | Node | 查询 |
| `ucn_node_set_link_wire_profile_limit(node, link, maximum_profile)` | 已注册 Link、Peer RX ceiling | HELLO 后或产品明确配置 |
| `ucn_node_get_link_wire_profile_limit(node, link)` | Node、Link | 查询 |
| `ucn_node_set_link_local_wire_profile_limit(node, link, maximum_profile)` | 单 Link 本地 RX ceiling | 可在注册前 |
| `ucn_node_get_link_local_wire_profile_limit(node, link)` | Node、Link | 查询 |
| `ucn_node_set_plain_session_id(node, session_id)` | 非零 Boot Session | 网络流量前 |

### 3.2 Security

| 函数 | 参数 | 作用 |
| --- | --- | --- |
| `ucn_node_set_security_required(node, required)` | 是否强制生产安全门 | Required 未满足时冻结协议收发 |
| `ucn_node_security_ready(node)` | Node | 判断 Provider、Policy、持久序列/Session 是否满足当前门禁 |
| `ucn_node_set_security(node, ops, context)` | Provider 操作表、产品上下文 | 安装 authorize/seal/open/sequence 等外部实现 |
| `ucn_node_set_security_policy(node, policy)` | Node 默认 Policy | 规定明文/保护、转发和接收边界 |
| `ucn_node_set_endpoint_security_policy(node, endpoint, policy)` | Endpoint 覆盖 | 对特定业务单独约束 |

安全 Provider 是产品边界。Core 负责调用顺序、AAD、授权和 fail-closed，但不内置产品密钥或 AEAD 算法。

### 3.3 邻居准入与状态

```c
ucn_result_t ucn_node_set_join_policy(
    ucn_node_t *node,
    ucn_join_policy_t policy,
    ucn_neighbor_authorize_fn authorize,
    void *context);
```

- `policy`：自动、授权或拒绝策略；
- `authorize`：产品授权回调；
- `context`：回调上下文。

其他入口：

| 函数 | 参数 | 用途 |
| --- | --- | --- |
| `ucn_node_observe_neighbor(node, link, now_ms)` | Link、当前时间 | 将物理发现转成 Neighbor Candidate |
| `ucn_node_probe_neighbor(node, link, now_ms)` | Link、当前时间 | 主动探测 |
| `ucn_node_broadcast_hello(node, link, now_ms)` | Link、当前时间 | 在指定 Bearer 发 HELLO |
| `ucn_node_admit_neighbor(node, peer_node_id)` | Peer ID | 人工/授权后准入 |
| `ucn_node_reject_neighbor(node, peer_node_id)` | Peer ID | 拒绝并清理候选状态 |
| `ucn_node_neighbor_count(node, state)` | 目标状态 | 统计固定表项 |
| `ucn_node_copy_neighbor_summaries(node, output, capacity)` | 调用者数组和容量 | Owner 上复制摘要；NULL/0 可查询数量 |

### 3.4 Link 与 Route

| 函数 | 参数 | 作用 |
| --- | --- | --- |
| `ucn_node_register_link(node, link)` | Node、具有 Ops 的 Link | 注册并建立静态 Link 所有权 |
| `ucn_node_add_route(node, destination, egress_link)` | 目标、已注册 Link | 安装/更新静态 Route |
| `ucn_node_set_default_route_constraints(node, constraints)` | Hop/Cost/RTT 等默认约束 | 改变后重新检查缓存可用性 |
| `ucn_node_get_default_route_constraints(node, constraints)` | 输出对象 | 复制当前默认约束 |
| `ucn_node_get_route_quality(node, destination, quality)` | 目标、输出质量 | 返回当前活动 Route 的 Hop/Cost/RTT/状态 |
| `ucn_node_discover_route(node, destination, now_ms)` | 目标、时间 | 启动 Expanding Ring RREQ |
| `ucn_node_refresh_route(node, destination, now_ms)` | 目标、时间 | 请求更新现有动态 Route |
| `ucn_node_route_pending(node, destination)` | 目标 | 查询 Discovery 是否仍在推进 |

### 3.5 Handler、发送、Queue 与维护

| 函数 | 参数核心 | 作用 |
| --- | --- | --- |
| `ucn_node_set_rx_handler(node, handler, context)` | 通用回调 | 未命中静态 Endpoint 时接收 |
| `ucn_node_set_endpoint_handler(node, endpoint, handler, context)` | Endpoint 回调 | 业务推荐分发入口 |
| `ucn_node_send(node, destination, message_type, traffic_class, payload, payload_length)` | 普通消息 | 立即解析 Route 并尝试发送 |
| `ucn_node_send_endpoint(node, destination, endpoint, traffic_class, payload, payload_length)` | Endpoint 消息 | 推荐业务入口；Lite 走自动最佳活动 Route |
| `ucn_node_enqueue(node, request)` | 完整发送请求 | 复制进 Q0～Q3 对应队列，后续由 Step 发送 |
| `ucn_node_step(node, now_ms)` | 统一单调毫秒时间 | 推进 Queue、Heartbeat、Neighbor、Route 和 Discovery |
| `ucn_node_get_stats(node)` | Node | 返回实时统计视图 |
| `ucn_node_receive(node, ingress_link, data, length)` | 已注册入口 Link、完整 Wire Frame | 唯一 Core RX 入口 |

## 4. Lite 内部函数怎样分区阅读

当前 Lite 编译出的 `ucn_node.c` 包含约 40 个公共定义和 121 个静态 helper。不要按源码行号从头硬读，应按职责分组。

### 4.1 Wire 控制 Payload

代表函数：

```text
read_u32_be / write_u32_be
read_uint_be / write_uint_be
route_request_*_offset
route_reply_*_offset
route_error_payload_size
select_route_request_profile
write_route_cost_for_profile / read_route_cost_for_profile
```

它们决定 RREQ/RREP/RERR 在 W0～W3 下的字段位置和可表示范围。先读这些 helper，后续看控制 handler 才不会把偏移量当成魔数。

### 4.2 Cost、Route 与 Epoch

```text
accumulate_route_cost
route_cost_is_known / route_cost_is_better
route_epoch_from_request_id
link_route_cost / link_local_select_cost
route_is_expired
route_epoch_is_accepted
cost_is_sufficiently_better
```

这里负责 Cost 饱和、Unknown 语义、路由新旧关系和滞回。Lite 没有 Candidate 表，但仍需要判断新 RREP 是否足以替换当前活动 Route。

### 4.3 Neighbor 与多 Bearer

```text
link_is_registered
find_neighbor / find_neighbor_bearer
switch_neighbor_primary
remap_neighbor_egress_references
evaluate_bearer_quality
expire_neighbor_candidates
admit_neighbor_entry
touch_neighbor
mark_neighbor_bearer_down
maintain_neighbor_liveness
```

先理解：一个 Neighbor 是逻辑 Peer；Bearer 才对应具体 Link。切换 Primary 后 Route 中的逻辑 egress 必须映射到新 Link，但不能把其他 Peer 的 Route 一并改掉。

### 4.4 Security 与控制预算

```text
security_policy_is_valid
security_policy_is_production_ready
protect_outbound_business
validate_inbound_business_security
take_control_token
take_control_rx_token
release_control_rx_peer_budget
```

安全验证和控制限流必须发生在危险状态写入之前。审计时重点比较成功、Provider失败、Replay、Peer预算满和 Source表满。

### 4.5 Route 生命周期

```text
learn_route
mark_route_used
invalidate_routes_by_link
invalidate_route_to
expire_dynamic_state
clear_discovery
send_route_discovery_ring
send_due_route_discovery_ring
begin_route_discovery
resolve_route_constraints
route_quality_meets_constraints
```

### 4.6 Frame 发送和控制处理

```text
prepare_outbound_wire_profile
send_frame_on_link
send_frame_on_logical_egress
send_control_on_link_profile
send_adaptive_control_on_link
handle_hello / handle_heartbeat
handle_route_request / handle_route_reply / handle_route_error
validate_inbound_hop_scope
```

### 4.7 Step 维护

```text
send_pending_q1_if_ready
send_due_heartbeat
send_due_bearer_quality_probe
send_due_route_discovery_ring
send_due_essential_maintenance
observe_step_interval
```

所有静态 helper 的精确参数、返回类型和行号在函数签名索引中列出。

## 5. Lite 发送调用链

### 5.1 业务立即发送

```text
ucn_node_send_endpoint
  → endpoint合法性
  → send_endpoint_auto_best
  → send_endpoint_internal
      → Security Ready/Policy
      → resolve Route constraints
      → 直连或活动 Route
      → resolve logical egress到当前Primary Bearer
      → protect_outbound_business
      → prepare_outbound_wire_profile
      → send_frame_on_link
          → get_status / effective MTU
          → ucn_frame_encode
          → link->ops->send
```

### 5.2 没有 Route

```text
send_endpoint_internal
  → Route不存在/不满足约束
  → begin_route_discovery
  → send_route_discovery_ring
  → 返回NOT_FOUND或当前调用路径规定的等待结果

后续Step
  → send_due_route_discovery_ring
  → 收到RREP后learn_route
  → Q1 pending或应用重试继续发送
```

## 6. Lite 接收调用链

```text
ucn_node_receive
  → Security Required Ready门
  → 确认ingress Link
  → ucn_frame_peek_wire_profile
  → 单Link/Node RX ceiling
  → ucn_frame_decode
  → Network与运行期Hop Scope
  → validate_inbound_business_security
  → Duplicate/RREQ专用分类
  → 未准入Link仅允许HELLO特殊入口
  → touch_neighbor
  ├─ HELLO/Heartbeat
  ├─ RREQ → 学反向Route → 回RREP或继续泛洪
  ├─ RREP → learn_route或继续回源
  ├─ RERR → 精确失效依赖Route并回传
  ├─ 本机Endpoint → handler
  └─ 非本机 → Route转发，Hop减1
```

## 7. Lite Step 调用链

```text
ucn_node_step(now_ms)
  → 记录唯一now_ms与Step间隔
  → expire_dynamic_state
      → Neighbor candidate/Route/RREQ cache过期
  → maintain_neighbor_liveness
      → Active→Suspect→Remove
  → evaluate_bearer_quality
      → 多样本/探测后切Primary
  → send_due_heartbeat
  → send_due_bearer_quality_probe
  → send_due_route_discovery_ring
  → Q0～Q3 调度与背压
  → essential maintenance预算
```

业务数据不是等 Heartbeat 到期才发送；队列通知会唤醒 Owner，`step()` 立即推进。Heartbeat 只是同一维护循环中的一个定时工作。

## 8. Lite 中的 Stub

以下公共能力由 `ucn_profile_stubs.c` 保持链接，但不是 Lite 功能：

- Node Snapshot、Path Trace、Policy Diagnostic Authorizer/Request；
- Path Control Authorizer；
- `ucn_path_*`；
- `ucn_policy_*`；
- `ucn_node_install/revoke/send_path*`；
- Route Policy、Policy Path、Q1 Flow、Policy Quality/Stats。

调用这些函数时，修改型 API 返回 `UCN_ERR_CONFIG`，查找型 API 返回 `NULL`，布尔更新返回 false。Stub 不会写表或偷偷改用普通 Route。

## 9. 推荐阅读顺序

```text
1. ucn_node_init + 配置setter
2. Link/Neighbor数据结构
3. observe/admit/HELLO/Heartbeat
4. learn_route + Route失效
5. RREQ/RREP/RERR Payload helper
6. ucn_node_send_endpoint
7. ucn_node_receive
8. ucn_node_step
9. Security helper
10. ucn_profile_stubs.c
```

## 10. Lite 审计重点

| 风险 | 审计问题 |
| --- | --- |
| 未准入流量 | 非 HELLO 是否在修改 Neighbor/Route/Replay 前拒绝 |
| 多 Bearer 切换 | Route 是否按逻辑 Peer 重映射；旧 Link 的其他 Route 是否误删 |
| RREQ 泛洪 | Token、Ring Hop、重复/Better 分类顺序是否正确 |
| RERR | 是否只删除真正依赖断链的 Route，传播方向是否正确 |
| Route Epoch | 旧 RREP/RERR 能否复活或删除新 Route |
| Security | Required 未准备好时是否无 Decode、转发和业务投递 |
| Step 回绕 | 所有 deadline 是否使用 `ucn_deadline_*`，而不是普通 `now >= deadline` |
| Stub | Full-only API 是否始终明确失败且不写状态 |

主要对照测试：`test_node.c`、`test_route.c`、`test_candidate_route.c` 中 Lite 可用部分、`test_adapter_hello.c`、`test_security.c`、Profile/公共头链接测试。
