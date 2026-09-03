# Nano 架构、函数与调用顺序

## 1. Nano 是什么

Nano 是 UCN 的固定拓扑最小实现。它保留：

- Wire v5 W0～W3 Frame 编解码；
- 固定 Link 注册；
- 直连 Peer 和静态 Route 查找；
- Q0～Q3 固定深度发送队列；
- Endpoint handler 和通用 RX handler；
- 普通业务帧的中继、Hop Limit 和重复抑制；
- Adapter、Event Runtime、Protocol Owner、Stream/CAN Source。

它不实现动态邻居发现、AODV 路由发现、安全 Provider、Candidate、Path、Policy、负载均衡和高级诊断。Nano 的目标不是“自动 Mesh 的缩水版”，而是资源更小、拓扑由产品预先确定的静态网络节点。

### 本文怎样覆盖 Nano 的全部函数

Service=ON 的当前基线中，Nano 由公共层 **211** 个函数定义、Nano Node **47** 个函数定义和 Profile Stub **39** 个函数定义组成，共 **297** 个函数定义。本文解释 Nano 专属入口、参数、内部 helper 和调用顺序；[公共基础层](01-公共基础层架构与函数.md)解释三档共用 API；[函数签名与源码位置索引](08-函数签名与源码位置索引.md)逐项列出这 297 个定义的完整参数、可见性和行号。这里的数量是函数定义数，不等于可用公共 API 数；Stub 和 `static` helper 也包含在内。

## 2. 实际构建架构

Nano 的差异源文件是：

```text
src/node/ucn_node_nano.c      真实 Nano Node
src/node/ucn_profile_stubs.c  保持高级公共 API 可链接并明确失败
```

它仍然链接公共 Foundation、Transport、Stream/CAN Source；Service 是否链接由 `UCN_FEATURE_SERVICE` 独立决定。

```text
Application / Service
        │
        ├── send_endpoint()/enqueue()
        ↓
  Nano Node
    ├── Link 表
    ├── 静态 Route 表
    ├── Endpoint Handler 表
    ├── Q0～Q3 Queue
    └── Duplicate Window
        ↓
  Frame Codec → Link Ops
```

Nano 没有 Neighbor 状态机。`link->peer_node_id` 是直连目标；静态 Route 将其他目标映射到一个已注册 egress Link。

## 3. 十个内部 helper

`ucn_node_nano.c` 当前有 10 个静态 helper，它们构成 Nano 的全部内部骨架。

| helper 与参数 | 谁调用 | 作用 |
| --- | --- | --- |
| `nano_link_is_registered(node, link)` | Link/Profile/RX API | 在固定 Link 指针表中确认归属 |
| `nano_resolve_link_local_receive_profile(node, link, profile)` | Register/RX | 合并 Node 与单 Link 本地 RX 上限 |
| `nano_link_status(link, status)` | TX | 清零状态后调用 `link->ops->get_status()` |
| `nano_find_link(node, destination)` | Send/Forward | 先查直连 Peer，再查静态 Route |
| `nano_find_endpoint_handler(node, endpoint)` | Handler 注册/分发 | 查固定 Endpoint handler 表 |
| `nano_allocate_sequence(node)` | Send | 分配非零 Sequence 并处理回绕 |
| `nano_send_frame(node, link, frame)` | 新发/转发 | 状态、MTU、Wire 档、Encode、`link->ops->send()` |
| `nano_send_existing_frame(node, link, frame)` | Forward | 检查并递减 Hop 后复用发送函数 |
| `nano_dispatch_endpoint(node, frame)` | RX 本机分发 | 调用静态 Endpoint handler |
| `nano_select_queue_item(queue, depth)` | Step | 选取 Queue 中 order 最早项 |

这十个函数建议先完整读一遍，再进入公共 API。它们短小，而且完整解释了 Nano 为什么不需要复杂的 Route Candidate 或 Policy。

## 4. 初始化和配置 API

### 4.1 Node 初始化

```c
ucn_result_t ucn_node_init(
    ucn_node_t *node,
    const ucn_config_t *config);
```

- `node`：调用者长期持有的静态对象；成功后由唯一 Owner 修改；
- `config`：包含 Network ID、Node ID、默认 Hop；函数复制内容，不保存指针。

调用顺序：

```text
ucn_validate_config(config)
  → memset(node)
  → 保存 config
  → TX/RX 默认 W3
  → next_sequence=1
  → next_queue_order=1
```

### 4.2 Wire 与 Session

| 函数 | 参数 | Nano 行为 |
| --- | --- | --- |
| `ucn_node_set_wire_profiles(node, tx_profile, max_receive_profile)` | Node、固定 TX 档、本地最大 RX 档 | 必须在注册 Link 前配置，并验证本地 ID/Hop/Session 可表达 |
| `ucn_node_get_tx_wire_profile(node)` | Node | 返回固定 TX 档，NULL 返回 UNSPECIFIED |
| `ucn_node_get_max_receive_wire_profile(node)` | Node | 返回 Node RX 上限 |
| `ucn_node_set_wire_profile_auto(node, enabled)` | Node、开关 | 决定新发帧是否选最小可表达档 |
| `ucn_node_wire_profile_auto(node)` | Node | 查询自动选档 |
| `ucn_node_set_link_wire_profile_limit(node, link, maximum_profile)` | 已注册 Link、对端 RX 上限 | 设置 peer ceiling |
| `ucn_node_get_link_wire_profile_limit(node, link)` | Node、Link | 查询 peer ceiling |
| `ucn_node_set_link_local_wire_profile_limit(node, link, maximum_profile)` | Node、Link、本地入口上限 | 可在 Link 注册前调用；同时验证静态 MTU |
| `ucn_node_get_link_local_wire_profile_limit(node, link)` | Node、Link | 查询单 Link 本地入口上限 |
| `ucn_node_set_plain_session_id(node, session_id)` | Node、非零 Boot Session | 防止重启复用 Source/Session/Sequence 域 |

Nano 没有 Security Provider，所以以下接口存在但明确不支持：

| 函数 | 返回规则 |
| --- | --- |
| `ucn_node_set_security_required(node, true)` | `UCN_ERR_CONFIG`；false 可保持普通模式 |
| `ucn_node_security_ready(node)` | 非 NULL Node 返回 true |
| `ucn_node_set_security(node, ops, context)` | 非 NULL Node 返回 `UCN_ERR_CONFIG` |
| `ucn_node_set_security_policy(node, policy)` | 同上 |
| `ucn_node_set_endpoint_security_policy(node, endpoint, policy)` | 同上 |

## 5. Link、Route 与邻居相关 API

### 5.1 原生支持

```c
ucn_result_t ucn_node_register_link(
    ucn_node_t *node,
    ucn_link_t *link);

ucn_result_t ucn_node_add_route(
    ucn_node_t *node,
    ucn_node_id_t destination,
    ucn_link_t *egress_link);
```

`register_link()` 验证 Link Ops、Liveness Profile、MTU、重复 Link ID 和固定容量；若产品提供 `open()`，成功打开后才写入 Node Link 表。

`add_route()` 只接受已注册 Link，写入静态目标→egress 映射。目标已经存在时更新为静态 Route；表满返回 `UCN_ERR_NO_SPACE`。

### 5.2 为统一 API 保留但 Nano 不支持

| 函数 | 参数 | 结果 |
| --- | --- | --- |
| `ucn_node_set_join_policy(node, policy, authorize, context)` | 自动入网策略和回调 | `UCN_ERR_CONFIG` |
| `ucn_node_observe_neighbor(node, link, now_ms)` | Link、当前时间 | `UCN_ERR_CONFIG` |
| `ucn_node_probe_neighbor(node, link, now_ms)` | Link、当前时间 | `UCN_ERR_CONFIG` |
| `ucn_node_broadcast_hello(node, link, now_ms)` | Link、当前时间 | `UCN_ERR_CONFIG` |
| `ucn_node_admit_neighbor(node, peer_node_id)` | Peer Node ID | `UCN_ERR_CONFIG` |
| `ucn_node_reject_neighbor(node, peer_node_id)` | Peer Node ID | `UCN_ERR_CONFIG` |
| `ucn_node_neighbor_count(node, state)` | 状态过滤 | 0 |
| `ucn_node_copy_neighbor_summaries(node, output, capacity)` | 输出数组/容量 | 0，不生成动态摘要 |
| `ucn_node_discover_route(node, destination, now_ms)` | 目标、时间 | `UCN_ERR_CONFIG` |
| `ucn_node_refresh_route(node, destination, now_ms)` | 目标、时间 | `UCN_ERR_CONFIG` |
| `ucn_node_route_pending(node, destination)` | 目标 | false |

默认 Route Constraints/Quality 查询在 Nano 由 `ucn_profile_stubs.c` 明确返回 `UCN_ERR_CONFIG`。

## 6. Handler、发送与队列 API

### 6.1 接收 handler

```c
void ucn_node_set_rx_handler(
    ucn_node_t *node,
    ucn_rx_handler_t handler,
    void *context);

ucn_result_t ucn_node_set_endpoint_handler(
    ucn_node_t *node,
    ucn_endpoint_t endpoint,
    ucn_endpoint_rx_handler_t handler,
    void *context);
```

- Endpoint handler 优先；
- 未命中静态 Endpoint handler 时才调用通用 RX handler；
- 回调运行在 Protocol Owner 上，不能阻塞或递归修改同一个 Node；
- `handler == NULL` 用于清除已有 Endpoint handler。

### 6.2 立即发送

```c
ucn_result_t ucn_node_send(
    ucn_node_t *node,
    ucn_node_id_t destination,
    uint8_t message_type,
    ucn_traffic_class_t traffic_class,
    const uint8_t *payload,
    uint16_t payload_length);

ucn_result_t ucn_node_send_endpoint(
    ucn_node_t *node,
    ucn_node_id_t destination,
    ucn_endpoint_t endpoint,
    ucn_traffic_class_t traffic_class,
    const uint8_t *payload,
    uint16_t payload_length);
```

参数含义：

- `destination`：非零、非广播、不能是本机；
- `message_type`：普通业务类型，不能是控制类型；
- `endpoint`：静态业务 Endpoint，内部直接映射到 message type；
- `traffic_class`：Nano 支持 Q0 Critical 和 Q1 Realtime；
- `payload/payload_length`：调用期间只读，长度必须能装入当前 TX Profile。

### 6.3 有界入队

```c
ucn_result_t ucn_node_enqueue(
    ucn_node_t *node,
    const ucn_send_request_t *request);
```

`request` 还携带 delivery、deadline。Nano 将 payload 复制进对应的固定 Q0～Q3 Item；因此函数成功后调用者可以释放原 Buffer。

支持的 delivery：

- `BEST_EFFORT`：一次出队发送；
- `LATEST_VALUE`：同 destination + message type 覆盖旧值；
- `RETRY_ON_BACKPRESSURE`：仅 Q0，要求非零 deadline，遇 `NO_SPACE` 有界重试。

## 7. Nano 的三条主调用链

### 7.1 直接发送

```text
ucn_node_send_endpoint
  → ucn_endpoint_is_static
  → ucn_node_send
      → 参数/QoS/Payload检查
      → nano_find_link
      → nano_allocate_sequence
      → nano_send_frame
          → nano_link_status
          → ucn_link_effective_mtu
          → 固定或自动Wire档
          → ucn_frame_encode
          → link->ops->send
```

### 7.2 Queue 发送

```text
ucn_node_enqueue
  → 复制到 Q0～Q3 对应固定 Item

后续Owner调用 ucn_node_step(now_ms)
  → 记录Step间隔
  → nano_select_queue_item(Q0)
  → 没有Q0再选Q1
  → deadline/next_attempt检查
  → ucn_node_send
  → 对Q0背压决定重试或删除Item
```

### 7.3 接收和转发

```text
ucn_node_receive
  → 确认 ingress_link 属于本Node
  → 解析本地Link RX上限
  → ucn_frame_peek_wire_profile
  → ucn_frame_decode
  → Network/Hop/Source/Sequence检查
  → 拒绝控制/Route/Path/Security/Diagnostic帧
  → ucn_duplicate_accept_frame
  ├─ 目的为本机
  │    → nano_dispatch_endpoint
  │    → 或通用rx_handler
  └─ 目的为其他Node
       → nano_find_link
       → nano_send_existing_frame
       → Hop减1
       → nano_send_frame
```

## 8. 高级 API Stub

Nano 会链接 `ucn_profile_stubs.c`。以下能力保持符号存在，但不会模拟成功：

- Route Constraints 与 Route Quality；
- Path state、Path install/revoke/send；
- Route Policy、Policy Path、Q1 Flow；
- Link Quality/Policy stats；
- Path Trace、Node Snapshot、Policy Diagnostic；
- 对应 Authorizer。

精确 Stub 函数签名见 [函数签名索引](08-函数签名与源码位置索引.md)。审计时必须验证应用正确处理 `UCN_ERR_CONFIG`、`NULL`、`false`，不能因为头文件中存在声明就继续使用返回对象。

## 9. 推荐源码阅读顺序

```text
ucn_node_nano.c: nano_link_is_registered
  → nano_find_link
  → nano_allocate_sequence
  → nano_send_frame
  → ucn_node_init
  → ucn_node_register_link
  → ucn_node_add_route
  → ucn_node_send
  → ucn_node_enqueue
  → ucn_node_step
  → ucn_node_receive
  → ucn_profile_stubs.c
```

## 10. Nano 审计重点与测试

| 风险 | 应检查什么 | 主要测试入口 |
| --- | --- | --- |
| 静态 Route 错绑 | egress 必须是本 Node 已注册 Link | `test_node.c`、`test_route.c` |
| Queue 所有权 | enqueue 成功后 payload 已复制；满表不覆盖 | `test_node.c` |
| Hop 转发 | `hop_limit <= 1` 不转发；每跳只减一次 | `test_node.c`、Hop 配置测试 |
| 低档误收高级帧 | 控制、Route、Path、安全和诊断均拒绝 | Profile tests |
| 重复业务投递 | Source/Session/Sequence 只交付一次 | duplicate/replay tests |
| Stub 误成功 | 高级 API 返回明确失败且符号存在 | `test_public_headers.c`、Profile tests |
