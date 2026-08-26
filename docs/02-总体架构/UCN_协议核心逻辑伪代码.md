# UCN 协议核心逻辑伪代码

> 状态：依据 v5 工作树源码编写的**注释级伪代码手册**（2026-08），覆盖 Core 主链与 Extended 可选模块。
> 目的：用一套统一、带解释的伪代码描述"数据从哪进、经过哪些判断、最后从哪出"，
> 让人不用逐行读 C 也能理解协议的真实逻辑。
> 依据：`src/node/ucn_node.c`（主状态机）、`src/core/ucn_frame.c`、`src/transport/*`、
> `src/routing/ucn_policy.c`、`src/service/*`、`src/extended/ucn_transfer.c`、
> `src/extended/ucn_cluster.c` 以及 `docs/` 下各协议文档。
> 边界：**源码是最终事实**，本文不替代源码或测试。伪代码与实现同构，但会省略字节序、
> 偏移量、防御性参数检查等 C 细节；被简化的地方以"简化："标注。Nano 档是本文的子集
> （走 `ucn_node_nano.c`，无自动 Mesh 主链）；Cluster 仅覆盖已实现的首阶段。

---

## 0. 伪代码约定

| 写法 | 含义 |
| --- | --- |
| `UCN_OK / UCN_ERR_*` | 与真实返回码同名：`UCN_ERR_NOT_FOUND`（无路由/无事可做）、`UCN_ERR_NO_SPACE`（固定表满/令牌不足）、`UCN_ERR_TTL`、`UCN_ERR_REPLAY`（重复帧）、`UCN_ERR_ACCESS`（未准入）、`UCN_ERR_MALFORMED`、`UCN_ERR_SECURITY`（安全未就绪/失败关闭）等 |
| `node->xxx` | 单节点静态状态字段（`ucn_node_t` 固定存储，全部编译期定长） |
| `固定表[N]` | 强调"容量是编译期宏"，表满必须显式报错，绝不动态扩容 |
| `// 注释` | 解释"为什么这样写"：并发边界、QoS 语义、安全门禁、容量契约 |
| `简化：` | 伪代码省略的 C 实现细节（字节序、偏移、统计计数等） |
| `owner 上下文` | 唯一 Protocol Owner 线程上下文——所有伪代码中的"收到帧后"处理都发生在这里 |

**最重要的并发模型（贯穿全文）**：一个 Node 只有一个 Protocol Owner。ISR 只搬字节、
更新计数、发通知；业务 Task 只调用发送 API 或本机 Service；解码、路由、解密、
转发、Endpoint 分发全部在 Owner 上下文中串行完成。不存在对 `ucn_node_t` 的并发访问。

---

## 1. 全局三条主链（先看这张总览）

### 1.1 RX 主链（字节 → 业务）

```
物理 RX ISR/回调
  → stream_source_write_from_isr（UART/RS-485/USB：写固定 Byte Ring）
     或 can_source_write_from_isr（CAN/CAN-FD：写固定 Frame Ring）
  → event_runtime_signal_source_from_isr   // 只置位 + 通知，绝不在这里解码
  → event_runtime_task_cycle               // Owner 被唤醒
  → source::service()                      // Owner 上下文：COBS 解帧 / CAN 重组
  → event_runtime_submit_frame             // 完整帧进入 Adapter 固定 RX Queue
  → protocol_owner_step                    // 每轮只采样一次 now_ms
  → adapter_rx_pump                        // 有限帧数批量取出
  → ucn_node_receive                       // ★ 本文第 3 章：全部协议逻辑入口
      ├─ 控制帧 → 各自 handler（HELLO/RREQ/RREP/RERR/Heartbeat/Path/诊断）
      ├─ 需转发 → 选 egress Link 转发（本文第 7 章）
      └─ 目的地是自己 → dispatch_endpoint → Endpoint handler / rx_handler
```

### 1.2 TX 主链（业务 → 字节）

```
业务 Task
  → ucn_service_send（或直接 ucn_node_send_endpoint）
  → service_protocol_bridge_step_at      // 可选桥：Q0 背压重试、Validator
  → ucn_node_send_endpoint               // ★ 本文第 8 章
      ├─ PINNED 策略 → 固定 Path
      ├─ AUTO_BALANCE 策略 → Q1 Flow 亲和分散
      └─ 默认 → 直连/Route Cache，否则 Q1 等待寻路、Q0 立即失败
  → ucn_node_send
      ├─ protect_outbound_business       // 可选 seal()：E2E 加密/认证
      ├─ 自动档模式：选最小可用 Wire Profile（含 16 B Tag 预留）
      ├─ ucn_frame_encode
      └─ link->ops->send                 // 产品 Adapter 落介质
```

### 1.3 调度主链（step：一切定时行为）

```
协议 Task 每轮（有事件立即醒，超时才轮询兜底）
  → ucn_node_step                        // ★ 本文第 9 章
     1. observe_step_interval            // 记录最大 Step 间隔（UCN_MAX_STEP_INTERVAL_MS 契约）
     2. 过期回收（路由/邻居/Path/诊断/Flow）
     3. 邻居保活（Heartbeat/HELLO 到期发送、Suspect/Remove 判定）
     4. Bearer 质量探测
     5. 业务发送：Q0 FIFO 优先 → Q1；有界背压重试；维护公平抢占
     6. Pending Q1 / 必要维护 / 诊断 / 路由刷新
```

---

## 2. 帧编解码（ucn_frame.c）

### 2.1 v5 帧结构（W0~W3 四档，基础头 17/21/26/30 B）

```
┌───────────┬──────────┬────────────┬──────────┬─────────┬──────────┐
│ Magic 2B  │ Ver|档位 │ MessageType│ 流量级+Flag │ HopLimit │ 可变域    │
└───────────┴──────────┴────────────┴──────────┴─────────┴──────────┘
可变域（宽度随档位）：NetworkID、Source、Destination（各 1/2/3/4 B）
                    Sequence(4B)、SessionID、PayloadLength
                    [RouteEpoch]（有 Route 扩展时，1/2/2/2 B）
                    [PathID]（有 Path 时，1/2/2/2 B）
                    CRC16-CCITT（覆盖头到 CRC 位 + Payload + 可选 16 B Tag）
                    Payload、[E2E AuthTag 16B]（受保护帧）
```

| Wire Profile | 地址宽 | 基础头 | 可用单播 | Wire 最大 Hop | 默认最大 Hop |
| --- | --- | --- | --- | --- | --- |
| W0 | 1 B | 17 B | 254 | 4 | 4 |
| W1 | 2 B | 21 B | 65,534 | 16 | 16 |
| W2 | 3 B | 26 B | 16,777,214 | 64 | 16 |
| W3 | 4 B | 30 B | 4,294,967,294 | 254 | 16 |

### 2.2 编码（发送侧收尾）

```
ucn_frame_encode(frame, output):
    // 1) 全字段校验：地址范围、flag 组合、档位能否表达（"任何字段表达不了就失败关闭"）
    descriptor = 档位描述符(frame.wire_profile)
    if 字段非法: return UCN_ERR_MALFORMED

    // 2) 定长写头（简化：按偏移写入）
    output[magic0..magic1] = MAGIC
    output[version|profile] = 版本 5 + 档位编码      // 3 B 前缀，供 Ingress 早拒绝
    output[message_type]   = frame.message_type
    output[traffic|flags]  = (traffic_class << 6) | flags
    output[hop_limit]      = frame.hop_limit
    write_be(network_id); write_be(source)
    write_be(destination == 广播 ? 档位最大可表达值 : destination)   // 广播编码为全 1
    write_u32_be(sequence); write_be(session_id)
    write_be(payload_length)
    if frame.has_route_extension: write_be(route_epoch)
    if frame.has_path_id:         write_be(path_id)

    // 3) 载荷与认证 Tag
    copy(payload)
    if frame 受保护: copy(auth_tag, 16 B)

    // 4) CRC16-CCITT 覆盖头+载荷+Tag
    crc = crc16(header, payload, tag?)
    write_u16_be(crc)
    return UCN_OK
```

### 2.3 解码（接收侧入口）

```
ucn_frame_decode(input):
    // 只校验前缀、档位、flag、可表达长度；CRC/地址/安全由 Node 层负责
    peek_wire_profile(input) → 档位              // 3 B 前缀就能读档位
    if 档位非法: return UCN_ERR_UNSUPPORTED
    read 各域 → frame
    if 长度不一致或字段越界: return UCN_ERR_MALFORMED
    return UCN_OK
```

---

## 3. 接收入口 ucn_node_receive（协议核心，最重要）

> 每个进入 Owner 的完整帧都走这一个函数。它体现三条铁律：
> **先安全再状态、先校验再处理、能转发就转发、错误显式上报。**

```
ucn_node_receive(node, ingress_link, data, length):
    // ── 第 0 道门：安全就绪（fail-closed）──────────────────────────
    // 若产品声明"安全必须存在"而 Provider 未完整配置，这里直接拒绝一切收发。
    if !node_security_ready(node): return UCN_ERR_SECURITY

    // ── 第 1 道门：3 B 前缀早拒绝（省掉整帧解码与 CRC）─────────────
    // 每个 Link 可配置"本地 RX 上限档"，Peer 通过 HELLO 发布自己的接收上限。
    local_profile = resolve_link_local_receive_profile(node, ingress_link)
    incoming_profile = peek_wire_profile(data)          // 只读 Magic+版本+档位
    if incoming_profile > local_profile: return UCN_ERR_UNSUPPORTED

    // ── 第 2 道门：完整解码（含长度/CRC/地址范围校验）───────────────
    if ucn_frame_decode(data, &frame) != UCN_OK: return 解码错误

    // ── 第 3 道门：网络与 Hop Scope ──────────────────────────────────
    if frame.network_id != node.config.network_id: return UCN_ERR_NETWORK
    if 运行期 Hop 门禁拒绝(node, frame): return UCN_ERR_UNSUPPORTED
        // 解释：W 档字段最多能表示 4/16/64/254 跳，但产品可收窄"本节点参与的最大跳数"，
        // 防止超深路径消耗资源；检查发生在安全/状态变更之前。

    // ── 第 4 道门：控制帧形态规则（编译期能力 + 硬规则）────────────
    // 规则 A：控制帧（HELLO/RREQ/...）不允许端到端加密——控制面是逐跳语义。
    if 是控制帧 && frame 带 E2E_PROTECTED flag: return UCN_ERR_MALFORMED
    if 是控制帧 && frame.has_path_id: return UCN_ERR_MALFORMED
    // 规则 B：本节点编译档位关闭了对应能力 → 显式拒绝，而不是静默忽略。
    if !UCN_FEATURE_PATH      && 是 Path 类帧: return UCN_ERR_CONFIG
    if !UCN_FEATURE_CANDIDATE && 是 Probe/Activate 帧: return UCN_ERR_CONFIG
    if !UCN_FEATURE_DIAGNOSTICS && 是诊断帧: return UCN_ERR_CONFIG

    // ── 第 5 道门：安全授权 authorize_rx（所有帧都要过）────────────
    // 产品 Provider 决定：这个 Link 上、这个来源、这个类型的帧是否被信任。
    // 控制面逐跳认证、业务端到端 seal/open 都挂在 Provider 之后。
    if security_ops != NULL:
        if authorize_rx(security_context, ingress_link, &frame) != UCN_OK:
            if frame.destination == 自己:
                note_path_control_authorization_rejected(...)  // 记录 Path 管理被拒
            return UCN_ERR_ACCESS

    // ── 分支 1：HELLO（最特殊：无需 Link 已注册，一跳绑定）─────────
    if frame.message_type == HELLO:
        if duplicate_accept_frame(node, &frame) != UCN_OK: return 重复
        return handle_hello(node, ingress_link, &frame)        // → 见第 4 章

    // ── 第 6 道门：Link 必须已注册（准入后或静态配置）──────────────
    // HELLO 之后的一切流量，只有"已准入邻居所在 Link"或"产品静态注册的 Link"可用。
    if !link_is_registered(node, ingress_link): return UCN_ERR_ACCESS

    // ── 第 7 道门：业务安全校验（E2E open：解密/验签 → plaintext）──
    if validate_inbound_business_security(node, ingress_link, &frame, plaintext) != UCN_OK:
        return 安全失败                          // → 见第 10 章

    // ── 分支 2：RREQ（独立于去重窗口，有自己的 Cache 与令牌）──────
    if frame.message_type == ROUTE_REQ:
        if frame.destination != 广播: return UCN_ERR_MALFORMED
        if payload 长度 != 该档位 RREQ 精确长度: return UCN_ERR_MALFORMED
        if validate_route_request_frame(...) != UCN_OK: return 校验失败
        cost = read_route_cost(payload)
        // RREQ Cache 分类：重放直接丢弃；Cache 满显式拒绝（防广播风暴）。
        if classify_route_request(...) == REPLAY:  return UCN_ERR_REPLAY
        if classify_route_request(...) == FULL:    return UCN_ERR_NO_SPACE
        // 控制面接收令牌：每 Peer 独立预算，防单个故障 Peer 刷爆本节点。
        if !take_control_rx_token(link, ROUTE_REQUEST): return UCN_ERR_NO_SPACE
        commit_route_request(...)                  // 写入 RREQ Cache
        touch_neighbor(node, ingress_link)         // 任何合法帧都刷新邻居存活
        return handle_route_request(...)           // → 见第 6 章

    // ── 第 8 道门：通用去重窗口（业务与其余控制帧）──────────────────
    // 按 (source, session_id) 的滑动位图防重放/重复投递。RREQ 不走这里。
    if duplicate_accept_frame(node, &frame) != UCN_OK: return UCN_ERR_REPLAY

    // ── 分支 3：心跳（一跳，8 B）────────────────────────────────────
    if frame.message_type == HEARTBEAT:
        return handle_heartbeat(node, ingress_link, &frame)   // 见第 4 章
    touch_neighbor(node, ingress_link)

    // ── 分支 4：按需诊断（默认拒绝远端请求，需产品授权）────────────
    if frame.message_type == NODE_SNAPSHOT_REQ:   return handle_node_snapshot_request(...)
    if frame.message_type == NODE_SNAPSHOT_REPLY: return handle_node_snapshot_reply(...)
    if frame.message_type == POLICY_DIAG_REQ 且 dst==自己: return handle_policy_diag_request(...)
    if frame.message_type == POLICY_DIAG_REPLY 且 dst==自己: return handle_policy_diag_reply(...)
    if frame.message_type == PATH_TRACE_REQ:     return handle_path_trace_request(...)
    if frame.message_type == PATH_TRACE_REPLY:   return handle_path_trace_reply(...)

    // ── 分支 5：Path 管理（目标必须是自己；三重门禁）───────────────
    if frame.message_type == PATH_INSTALL 且 dst==自己: return handle_path_install(...)
    if frame.message_type == PATH_REVOKE  且 dst==自己: return handle_path_revoke(...)
    // Candidate 路径验证（Full 档）：
    if frame.message_type == PATH_PROBE 且 dst==自己:    return handle_path_probe(...)
    if frame.message_type == PATH_PROBE_ACK 且 dst==自己: return handle_path_probe_ack(...)
    if frame.message_type == PATH_ACTIVATE:               return handle_path_activate(...)
    if frame.message_type == PATH_ACTIVATE_ACK 且 dst==自己: return handle_path_activate_ack(...)

    // ── 分支 6：路由学习（学习完可能继续转发）──────────────────────
    if frame.message_type == ROUTE_REPLY:
        result = handle_route_reply(node, ingress_link, &frame, &consumed)
        if result != UCN_OK: return result
        if consumed: return UCN_OK                  // 学习完成、本节点是目标/已消费
        // 否则落入下面的转发分支（RREP 沿回程逐跳转发）
    if frame.message_type == ROUTE_ERROR:
        result = handle_route_error(node, &frame, &consumed)
        if result != UCN_OK: return result
        if consumed: return UCN_OK

    // ── 分支 7：Path 业务帧的一致性校验（防 Path 表滥用）───────────
    if 非控制帧 && frame.has_path_id:
        path = find_active_path(source, session_id, path_id, dst)
        if path == NULL 或 terminal/剩余跳数与 HopLimit 不匹配:
            send_path_route_error(...)             // 告知源端 Path 已失效
            return UCN_ERR_NOT_FOUND
    // 普通 Route 业务帧到本机：检查 Route Epoch 是否还在有效窗口内
    if 非控制帧 && !has_path_id && has_route_extension && dst==自己:
        if !route_epoch_is_accepted(source, &frame):   // Current 或 Previous+Grace
            return UCN_ERR_NOT_FOUND               // 旧 Epoch 帧过期丢弃，防环

    // ── 分支 8：转发（目的地不是自己且不是广播）────────────────────
    if frame.destination != 自己 && frame.destination != 广播:
        // 8.1 TTL 门禁
        if frame.hop_limit <= 1: return UCN_ERR_TTL

        // 8.2 选择出口 Link（按帧类型从专用表中查，绝不用"最近学习"糊弄）
        if 是 Candidate RREP:       egress = find_candidate_link(dst, candidate_id)
        elif 是 PATH_PROBE/_ACK:    egress = find_candidate_link(dst, candidate_id)
        elif has_path_id:           egress = resolve_egress_link(path->egress_link)
        elif 非控制帧:              egress = find_link_for_route_epoch(dst, 有扩展, epoch)
        else:                       egress = find_link(dst)

        // 8.3 没有出口或出口等于入口（不能原路发回）→ 回送 RERR
        if egress == NULL || egress == ingress_link:
            send_route_error(或 path_route_error)(ingress, source, dst)   // 显式报不可达
            return UCN_ERR_NOT_FOUND

        // 8.4 RREP 转发时把"入链路 Cost"累加进回程 Cost（Hop +1）
        if 是 ROUTE_REPLY:
            cost = accumulate(cost, link_route_cost(ingress_link)); hop++

        // 8.5 扣 Hop、按 Path 或普通逻辑出口发送（不重新选档、不改密文）
        frame.hop_limit--
        result = has_path_id ? send_frame_on_path_egress(...)
                             : send_frame_on_logical_egress(...)
        if result == UCN_OK:
            mark_route_used(dst)                   // 路由保鲜（有流量即视为活跃）
        if result == UCN_ERR_LINK_DOWN:
            invalidate_routes_by_link(egress)      // 物理断了，清走这条 Link 的路由
            send_route_error / send_path_route_error(...)   // 告诉源端换路
        return result

    // ── 分支 9：目的地是自己 → 投递（静态 Endpoint 优先）───────────
    if !dispatch_endpoint(node, &frame):           // 命中 0x40~0xBF 静态 Endpoint 表
        if node->rx_handler != NULL:
            node->rx_handler(node->rx_context, &frame)   // 兜底：原始回调
    return UCN_OK
```

**这条函数的阅读要点**：
1. 顺序即优先级：安全 → 前缀 → 解码 → 网络 → 形态 → 授权 → 去重 → 具体语义；
2. **HELLO 是唯一绕过"Link 已注册"门的帧**（否则新邻居永远进不来）；
3. **RREQ 独立去重**，且有"每 Peer 令牌 + 全局 Cache"双重风暴抑制；
4. 任何无法处理的帧都**显式返回错误码**，收到不可达信息的节点会产生 RERR——
   协议里没有"静默丢弃"这一档（除非是过期重复帧）。

---

## 4. HELLO、准入与邻居生命周期

### 4.1 handle_hello：一跳绑定

```
handle_hello(node, ingress_link, frame):
    // HELLO 载荷 = 1 字节：Peer 的"最大接收档"（RX Ceiling），用于本端发帧时收敛档位
    if payload 长度 != 1 或 (dst != 自己 && dst != 广播): return UCN_ERR_MALFORMED
    peer_rx_profile = payload[0]
    if peer_rx_profile 不是合法档位 或 peer_rx_profile < frame.wire_profile:
        return UCN_ERR_MALFORMED          // Peer 不能说"我能收 W3"却用 W2 发 HELLO

    // 来源合法性：非 0、非广播、非自己；Link 若已绑定别的 Node ID 则冲突
    if source 非法 或 (link 已绑定 && 绑定不一致): return UCN_ERR_MALFORMED

    ingress_link.peer_node_id = frame.source   // ★ 物理端点 → 逻辑 Node ID 绑定
    observe_neighbor(node, ingress_link)       // 见 4.3
    set_link_wire_profile_limit(ingress_link, peer_rx_profile)   // 记住对端接收上限
    return UCN_OK
```

### 4.2 准入策略（产品在配置时三选一）

```
admit_neighbor_entry(node, entry, policy):
    switch node.config.join_policy:
        MANUAL:   // 产品显式调用 ucn_node_admit_neighbor 才准入
            if !产品已显式授权(entry): return UCN_ERR_ACCESS
        OPEN:     // 开发/演示：HELLO 即候选，直接准入
            entry.state = ADMITTED
        PROVIDER: // 生产：交给安全 Provider 裁决（身份/证书/ACL）
            if !security_provider.authorize_join(entry): return UCN_ERR_ACCESS
    return UCN_OK
```

### 4.3 邻居状态机与保活

```
// 状态机（固定表，容量 UCN_MAX_NEIGHBORS）：
// CANDIDATE → ADMITTED → SUSPECT → REMOVED（另有 REJECTED / EXPIRED 终态）

observe_neighbor(node, ingress_link):
    entry = 按 (node_id, link) 查邻居表
    if entry 不存在:
        if 邻居表满: return UCN_ERR_NO_SPACE        // 显式拒绝，不驱逐旧邻居
        新建 entry，state = CANDIDATE
    if entry.state == CANDIDATE:
        if admit_neighbor_entry(...) == UCN_OK: state = ADMITTED
        else: state = REJECTED
    refresh entry.last_seen = node.now_ms
    return UCN_OK

// Owner 每轮在 step 中执行：
maintain_neighbor_liveness(node, now_ms):
    for entry in 邻居表:
        if entry.state == ADMITTED 且 now_ms - last_seen > SUSPECT 超时:  // 默认 3 s
            entry.state = SUSPECT
        if entry.state == SUSPECT 且 now_ms - last_seen > REMOVE 超时:   // 默认 4 s
            entry.state = REMOVED
            回收 Bearer / 撤销经该邻居的 Route 与 Path / 通知应用失联事件

handle_heartbeat(node, link, frame):
    // 8 B 一跳心跳：请求/ACK 成对。业务帧也会刷新邻居存活。
    refresh neighbor.last_seen
    if frame 是请求 && dst==自己: 回送 HEARTBEAT_ACK（一跳，不转发）
    // 心跳与 HELLO 的差别：HELLO 管"发现与准入"，Heartbeat 管"存活与撤销"；
    // 心跳不会把已 REMOVED 的邻居复活。
```

> 关键语义：**只有 ADMITTED 邻居的 Link 才能承载普通业务与路由**（receive 里第 6 道门）。
> 无线/有线介质都共用同一套状态机；不同介质只是超时参数不同
> （Fast 档：Heartbeat 250 ms / Suspect 1250 ms / Remove 2000 ms）。

---

## 5. 去重窗口（滑动位图）

```
duplicate_accept_frame(node, frame):
    // 固定 UCN_DUPLICATE_SOURCE_WINDOWS 个槽，每个槽：source + session + 最高序号 + 位图
    slot = 查找 (source, session_id)
    if slot 存在:
        delta = frame.sequence - slot.highest_sequence     // 序列号比较（环绕安全）
        if delta > 0:                                      // 更新
            slot.bitmap = (delta >= 窗口位宽) ? 1 : (bitmap << delta) | 1
            slot.highest_sequence = frame.sequence
            return UCN_OK
        if delta == 0: return UCN_ERR_REPLAY                // 精确重复
        age = -delta                                        // 早到的乱序帧
        if age >= 窗口位宽 或 bitmap 已标记: return UCN_ERR_REPLAY
        置位 bitmap[age]; return UCN_OK
    else:
        if 有空槽（含超时未用的旧槽）: 初始化新槽; return UCN_OK
        return UCN_ERR_NO_SPACE                            // 表满：显式拒绝
```

> 用途：防重放/防环/防重复投递。**RREQ 不走这里**——它有独立 Cache（按请求 ID），
> 因为 RREQ 是广播，需要允许同一帧从多个入口到达后再被 Cache 拦截，而不是直接判重。

---

## 6. 路由：受限 AODV-Lite

### 6.1 RREQ 处理（被寻路的中继/目标）

```
handle_route_request(node, ingress_link, frame):
    // 载荷（档位相关）：目标 ID、请求 ID(4B)、累计 Cost、Hop、flags
    origin  = frame.source
    target  = payload.target
    request_id = payload.request_id
    cost    = payload.cost
    hop     = payload.hop
    is_candidate = payload.flags & CANDIDATE

    // 学习"反向路由"：指向 origin 的下一跳 = ingress_link（用于回送 RREP）
    // 反向路由是回程建表的根基；正向路由在 RREP 时学习。
    learn_route(node, origin, ingress_link, cost, hop, epoch)   // 或 learn_candidate_route

    // 情况 A：我就是目标 → 构造 RREP 沿来路单播回 origin
    if target == 自己:
        // RREP 的 Cost 从 0 起，Hop 从 0 起——每个回程中继累加自己出链路的 Cost。
        // 这样存储的 Cost 严格"越靠近目标越小"，是有界 AODV-Lite 无环的不变量。
        reply = { request_id, cost=0, hop=0, flags, epoch=反向路由的 epoch }
        send_control_on_link(ingress_link, origin, ROUTE_REPLY, reply)
        return UCN_OK

    // 情况 B：我不是目标 → 受限泛洪转发（见 6.2）
    return forward_route_request(node, ingress_link, frame)
```

### 6.2 RREQ 转发（广播但受控：每链路累加 Cost、Hop+1、不原路发回）

```
forward_route_request(node, ingress_link, frame):
    if frame.hop_limit <= 1: return UCN_ERR_TTL        // 源端自己不扣 Hop，中继扣 1
    forwarded = *frame; forwarded.hop_limit--

    sent_count = 0
    for link in 注册的所有 Link:
        if link == ingress_link: continue               // 不原路发回（防反射）
        if is_candidate 且 !link 是候选资格链路: continue
        payload2 = payload 拷贝
        payload2.cost += link_route_cost(link)          // 累加出链路 Cost
        payload2.hop++
        if send_frame_on_link(link, &forwarded) == UCN_OK: sent_count++
    return sent_count > 0 ? UCN_OK : 最后一个错误
    // 注意：扩圈值 2 表示"最多经过 2 条链路"（A-B-C），中继转发前先扣 Hop。
```

### 6.3 有界扩圈（2 → 4 → 8 → 16，防广播风暴）

```
begin_route_discovery(node, dst, now, is_candidate, max_hop, ...):
    if 并发发现槽满(UCN_MAX_ROUTE_DISCOVERIES): return UCN_ERR_NO_SPACE
    slot = 空闲发现槽; slot.destination = dst; slot.current_hop = 2
    slot.deadline = now + UCN_ROUTE_RING_TIMEOUT_MS      // 每环 250 ms
    send_route_discovery_ring(slot, hop_limit=2)          // 发第一环

// Owner 每轮检查（step 内）：
send_due_route_discovery_ring(node, now):
    for slot in 发现表:
        if slot 活跃 && 到期 && current_hop < max_hop:    // 上一环没成功，扩圈
            next = next_hop(2→4→8→16)
            send_route_discovery_ring(slot, next)
            return
    // 总预算 1 s（4 环 × 250 ms）；发现槽固定 4 个，满则拒绝新的寻路请求。
```

### 6.4 RREP 处理（回程逐跳学习 + 成本累加）

```
handle_route_reply(node, ingress_link, frame, &consumed):
    // 方向语义：RREP 的 source=目标，destination=origin，沿反向路由逐跳回传
    origin = frame.destination      // 寻路发起者
    target = frame.source           // 寻路目标
    validate(请求ID/epoch/flags 合法)                    // 非候选 RREP 必须带非 0 Epoch

    // 累加"入链路 Cost"：我学到的是"经 ingress_link 到 target 的距离"
    cost += link_route_cost(ingress_link); hop++

    if is_candidate && 目的地 == 自己:
        // 候选路由验收（Full 档）：只有"本地评估更优"或"当前路线缺已验证 RTT
        // 且本候选不更差"才学习；否则拒绝并消费，避免无效候选占表。
        if 不合格: deactivate 发现槽; return (consumed=true, UCN_OK)
        learn_candidate_route(target, ingress_link, cost, hop); return consumed

    learn_route / learn_candidate_route(target, ingress_link, cost, hop, epoch)

    if frame.destination != 自己: return consumed=false      // 继续走转发分支回传
    // 到达寻路发起者：关闭对应发现槽，Pending Q1 会在下一轮 step 被放行
    deactivate 发现槽(dst=target, request_id); return consumed=true
```

### 6.5 RERR（显式断链告警）

```
handle_route_error(node, frame, &consumed):
    unreachable = payload.unreachable
    invalidate_route_to(unreachable)          // 清掉本节点到 unreachable 的活跃路由
    if frame.destination == 自己: return consumed=true    // 我是告警目标
    // 否则沿回程继续转发，让上游源端尽快知道断链（不重新寻路、不缓存）
    return consumed=false
```

### 6.6 Route Cache 与 Epoch

```
// 固定 8 条活跃路由；每条：dst、下一跳 Link、hop、cost、route_epoch、过期时间
// 过期策略：30 s 寿命；提前刷新（到期前 6 s 重新发现）；最短刷新间隔 5 s。

// Epoch 语义（防环 + 无缝换路）：
//  - 每次寻路成功生成新 epoch；业务帧携带 Route 扩展（epoch）。
//  - 接收侧只接受 Current 或 Previous 的 epoch（Previous 带 1 s Grace），
//    彻底解决"旧路径在途帧"与"新路径已切换"之间的环路窗口。
route_epoch_is_accepted(node, source, frame):
    route = find_active_route(source)
    if frame.epoch == route.epoch: return true                // Current
    if frame.epoch == route.previous_epoch 且 在 Grace 期内: return true
    return false
```

---

## 7. 转发决策（receive 分支 8 的展开）

```
forward_frame(node, ingress_link, frame):
    // 出口选择优先级（保证"路由归属"清晰，不允许"随便找条 Link 发"）：
    egress = 按帧类型选择：
        Candidate RREP / Probe / Probe-ACK  → find_candidate_link(dst, candidate_id)
        Path 业务帧                          → path->egress_link（Path 表逐跳安装）
        普通业务帧                           → find_link_for_route_epoch(dst, 有扩展?, epoch)
        控制帧                               → find_link(dst)   // 直连或活跃路由
    if egress == NULL || egress == ingress: → 回送 RERR；return NOT_FOUND

    // 转发前不重新解码/不重新加密：
    //   - 中继只改 HopLimit（-1）与 RREP 的 Cost/Hop，密文 Payload 原样透传；
    //   - 这就是"透明密文中继"：中继不需要（也不应该能）读业务内容。
    frame.hop_limit--
    result = link_ops.send(egress, encode(frame))

    if result == UCN_ERR_LINK_DOWN:
        invalidate_routes_by_link(egress)      // 该 Link 上所有路由全部作废
        send RERR 给 source                       // 上游据此重选或重新寻路
    return result
```

> 转发是**无状态逐跳**的：中继不缓存完整业务消息、不做端到端 ACK、
> 不代答业务结果。可靠交付是大消息 Transfer 或应用层的职责（见第 13 章）。

---

## 8. 发送侧：从业务 API 到 Link 字节

### 8.1 入口：ucn_node_send_endpoint

```
ucn_node_send_endpoint(node, dst, endpoint, class, payload, len):
    if !ucn_endpoint_is_static(endpoint): return UCN_ERR_ARGUMENT  // 只发静态 Endpoint
    // 查策略表（Full 档）：Policy 按 (dst, endpoint) 键绑定
    policy = lookup_policy(dst, endpoint)
    if policy 是 PINNED:     return send_endpoint_pinned(...)      // 固定 Path
    if policy 是 AUTO_BALANCE 且 class==Q1:
                             return send_endpoint_auto_balance(...) // Flow 亲和分散
    return send_endpoint_auto_best(...)                            // 默认自动路由
```

### 8.2 默认路径：auto_best（体现 Q0/Q1 的生死之别）

```
send_endpoint_auto_best(node, dst, endpoint, class, payload, len):
    constraints = 解析默认/特定路由约束（Hop、Cost、已验证 RTT）
    usable = 路由质量满足约束(dst)

    if class == Q1 且 !usable:
        // Q1 允许等待：启动有界发现（扩圈），把最新值放进固定 Pending 表
        begin_route_discovery(dst, 候选=存在活跃非静态路由, max_hop=约束.Hop, ...)
        if 仍然不可用:
            if !允许入 Pending: return UCN_ERR_NOT_FOUND
            return queue_pending_q1(dst, endpoint, payload)  // 合并同 (dst,endpoint)
        // 注意：Q0 永远不进入这个分支——没路由就立即失败（见 8.4）

    if !usable: return UCN_ERR_NOT_FOUND         // Q0 到这里 = 立即失败
    return ucn_node_send(node, dst, endpoint, class, payload, len)
```

### 8.3 实际组帧发送：ucn_node_send

```
ucn_node_send(node, dst, message_type, class, payload, len):
    if !node_security_ready(node): return UCN_ERR_SECURITY   // fail-closed
    link = find_link(node, dst)               // 直连 Link 或活跃路由的下一跳 Link
    if link == NULL: return UCN_ERR_NOT_FOUND
    if !link.status.is_up: return UCN_ERR_LINK_DOWN

    frame = {
        message_type, traffic_class,
        hop_limit = 自动档且路由已知 ? route.hop_count : 默认 Hop 上限,
        network_id, source=自己, destination=dst, session_id
    }
    route = find_active_route(dst)
    if route != NULL 且 egress 一致 且 route.epoch != 0:
        frame.flags |= ROUTE_EXTENSION; frame.route_epoch = route.epoch
    frame.sequence = allocate_sequence(node)   // 单调递增，防重放窗口用

    // 安全保护（可选但建议）：seal → 密文 + 16 B Tag；见第 10 章
    result = protect_outbound_business(node, link, &frame, ciphertext, auth_tag)

    // 出口发送：
    send_frame_on_logical_egress(node, link, &frame):
        if node 开启自动最小档:
            // 结合地址宽度、Hop、Route/Path 字段、对端 RX Ceiling、Link MTU、
            // 以及受保护帧预留的 16 B Tag，选出"能表达的最小档位"。
            // 任何字段表达不了 → 失败关闭，不静默截断。
            profile = select_min_wire_profile(&frame, max_profile, mtu)
        else:
            profile = node.tx_wire_profile          // 默认固定 W3
        if 帧编码长度 > Link/Path 有效 MTU: return UCN_ERR_TOO_LARGE
        return link->ops->send(link, encode(&frame))
    return result
```

### 8.4 Q0 的"三不"原则（与 Q1 的对照表）

| | Q0（关键控制） | Q1（实时状态） |
| --- | --- | --- |
| 队列语义 | FIFO，先进先出 | Latest-value：同 (dst, message_type) 覆盖旧值 |
| 无路由时 | **立即失败**，绝不等待寻路 | 入 Pending 槽并启动寻路，路由就绪后发送 |
| Deadline | 绝对 deadline，过期丢弃（TTL 上报） | 同样绝对 deadline；Pending 期间内部重试**不刷新** deadline |
| 背压（NO_SPACE） | 可选有界重试（次数+时间窗内） | 不重试（旧状态本就该被新值取代） |
| 维护抢占 | 连续突发达到上限后，让位给必要的保活/维护 | 同左 |

---

## 9. ucn_node_step：唯一的定时调度器

```
ucn_node_step(node, now_ms):
    // 本函数由唯一 Owner 周期性调用（默认 ≤10 ms 间隔，有事件立即醒）。
    if !node_security_ready(node): return UCN_ERR_SECURITY

    observe_step_interval(now_ms)        // 记录实际 Step 间隔；超 UCN_MAX_STEP_INTERVAL_MS 记违规

    // ── 阶段 1：过期回收（所有可增长状态都有寿命）────────────────
    expire_dynamic_state()               // 路由、候选、发现槽超时回收
    policy_refresh_link_quality()        // Full：采样 9 项 Link 指标 → LC-1 有效分
    policy_expire_flows()                // 过期 Q1 Flow
    path_expire()                        // 过期 Path
    expire_neighbor_candidates()         // 过期未准入候选
    maintain_neighbor_liveness(now_ms)   // 发到期 Heartbeat；Suspect/Remove 判定
    evaluate_bearer_quality(now_ms)      // 主备 Bearer 质量探测

    // ── 阶段 2：挑一个业务项发送（Q0 FIFO 优先于 Q1）──────────────
    item = q0 队首；若 q0 空则 item = q1 队首

    // 维护公平：业务突发不能饿死邻居保活。
    // 计数到 UCN_BUSINESS_TX_BURST_BEFORE_MAINTENANCE 且确有到期维护 → 先做维护。
    if 连续业务发送计数 >= 突发上限:
        if send_due_essential_maintenance(...) 有产出: 计数清零; return

    if item == NULL:                     // 队列全空 → 只做后台工作
        send_pending_q1_if_ready()       // 路由刚就绪的 Pending Q1 放行
        send_due_essential_maintenance() // 到期 Heartbeat/RREQ 环/Path 探测等
        send_due 诊断请求/回复           // 诊断永远排在业务之后
        start_due_route_refresh()        // 活跃路由到期前提前刷新
        return UCN_ERR_NOT_FOUND         // "无事可做"是正常结果，不是错误

    // ── 阶段 3：deadline 与背压纪律 ─────────────────────────────────
    if deadline 已过(item):
        释放槽位; 统计过期丢弃; return UCN_ERR_TTL        // 过期数据主动丢弃

    if item 带背压重试 且 还没到下次尝试时间:
        // 保留 FIFO 占有权：低优先级业务不得占用这个空档，但必要维护可以
        send_due_essential_maintenance(...); return

    // ── 阶段 4：真正发送 ───────────────────────────────────────────
    result = ucn_node_send(item...)      // 复用第 8 章的完整发送路径
    if result == UCN_OK: 释放槽位; return UCN_OK

    if result == UCN_ERR_NO_SPACE 且 item 是 Q0 背压重试型:
        if 重试次数还有额度 且 deadline 允许再等一个重试间隔:
            item.retries++; item.next_attempt = now + 重试间隔; return result
        // 额度用尽 → 终态失败（精确计数一次）
    释放槽位; 统计终态丢弃; return result
```

### 9.1 Pending Q1 的合并语义

```
queue_pending_q1(node, dst, endpoint, payload):
    slot = 按 (destination, endpoint) 查固定 Pending 表
    if slot 存在: 覆盖 payload（保留原绝对 deadline）   // "最新值比旧值更重要"
    else: 有空槽则新建（deadline = 首次入队的绝对时刻）
    return UCN_OK

send_pending_q1_if_ready(node, now):
    for slot in Pending 表:
        if deadline 已过: 丢弃; continue
        if 现在有可用路由(dst): 发送 slot.payload; 释放槽位
```

---

## 10. 安全边界（框架就位，生产 AEAD 由产品接入）

### 10.1 三道安全门（按 receive 顺序）

```
// 门 1：全局就绪（fail-closed）
node_security_ready(node):
    if !node.security_required: return true          // 产品显式关闭安全要求
    return security_ops != NULL
        && ops.authorize_rx != NULL
        && ops.seal != NULL && ops.open != NULL      // seal/open 必须成对
        && Provider 完整配置                           // 否则一切收发拒绝

// 门 2：逐帧授权（所有帧，含控制帧）
authorize_rx(context, link, frame):
    // 产品实现：按 Link、来源、消息类型、会话状态决定接受/拒绝
    // 控制面逐跳认证通常在这里完成（Hop-by-Hop）

// 门 3：业务端到端（E2E）
validate_inbound_business_security(node, link, frame, plaintext):
    policy = 按 (endpoint/帧类型) 查 RX 安全策略     // PLAIN / PROTECTED / BOTH
    if frame 带 E2E 标志 但策略是 PLAIN: return UCN_ERR_SECURITY
    if frame 不带 E2E 标志 但策略是 PROTECTED: return UCN_ERR_SECURITY
    if frame 带 E2E 标志:
        ops.open(context, frame, payload, plaintext)  // 解密 + 验 16 B Tag
        // AAD = 30 B 不可变字段（档位、地址、Path ID 等）：
        // 中继改过 HopLimit 也不破坏 AAD，但改地址/Path 立即验签失败
    return UCN_OK
```

### 10.2 发送侧 seal

```
protect_outbound_business(node, link, frame, ciphertext, auth_tag):
    if 安全策略要求保护:
        ops.seal(context, frame, payload, plaintext_len,
                 ciphertext, auth_tag)               // 产出密文 + 16 B Tag
        frame.payload = ciphertext; frame.flags |= E2E_PROTECTED
        frame.auth_tag = auth_tag
        // 自动选档时必须为 16 B Tag 预留空间（帧仍要装进 MTU）
    return UCN_OK
```

### 10.3 会话轮换与重放

```
// 发送序号持久化（Provider 存储），重启后不回绕
// 会话轮换：rotate_session → 新的 (session_id, sequence)；旧序列低于阈值才接受
// 去重窗口（第 5 章）承担网络级重放防护；完整会话生命周期仍是待办
```

> 边界诚实声明：以上是**接口与边界**。受审计 AEAD 实现、真实密钥存储、
> JOIN 挑战状态机、生产 ACL 表都不在 Core 里——产品接入 Provider 后
> 还必须完成安全审计才能宣称"生产安全"。

---

## 11. 显式 Path 与 Candidate 换路（Full 档）

### 11.1 安装与撤销（三重门禁）

```
// 源端产品配置 Path 后：
ucn_node_install_local_path(owner, path_id, hops[], dst):
    // 本地写入逐跳 Path 表，然后逐跳发送 PATH_INSTALL（基础 8/11/14/17 B，
    // 能力扩展 11/14/17/20 B；REVOKE 2/4/6/8 B）
    for hop in path: send PATH_INSTALL → hop 节点

// 每跳收到 PATH_INSTALL（receive 分支 5）：
handle_path_install(node, ingress_link, frame):
    // 门禁 1：安全（已在 receive 第 5 道门做过 authorize_rx）
    // 门禁 2：产品 Path Authorizer（管理面授权）
    // 门禁 3：按认证 (source, session) 的固定 Token Bucket（4+1/1000 ms）
    //         ——同源换 Bearer 不会刷新额度，防止绕限
    if 任一门禁失败: 记录拒绝; return UCN_ERR_ACCESS
    if 本跳能力（档位/MTU）不满足 Path 能力交集: 回送 Path-RERR
    写入 Path 转发条目：{path_id, owner, owner_session, remaining_hops, egress_link}
    return UCN_OK
```

### 11.2 固定路径发送语义

```
send_endpoint_pinned(node, policy, payload):
    path = find_active_path(owner, session, path_id, dst)
    if path == NULL 或 约束不满足: 
        if policy.mode == PINNED_STRICT: return UCN_ERR_NOT_FOUND   // 严格：宁可不发
        else: return send_endpoint_auto_best(...)                  // FAILOVER：回退自动路由
    return send_frame_on_path_egress(path, &frame)   // 逐跳按 remaining_hops 转发
```

### 11.3 Candidate 验证与无缝切换

```
// 新候选路由验证（换路前先验证，验证通过才切）：
源端:  send PATH_PROBE(候选路径) → 目标
目标:  回 PATH_PROBE_ACK
源端:  收到 ACK → 更新候选 RTT EWMA → 达标则 send PATH_ACTIVATE(候选 ID + Epoch)
沿途:  激活后按 Current/Previous 双 Epoch 工作（Previous 保留 1 s Grace）
源端:  收到 PATH_ACTIVATE_ACK → 完成切换；旧 Epoch 期满回收
// 好处：切换瞬间在途旧帧仍被接受，不会误判为环/重放，业务不闪断。
```

---

## 12. 节点内任务通信：Service Router 与跨节点 Bridge（可选）

### 12.1 本机 Service Router（不生成帧）

```
ucn_service_router_init(router):
    // 固定绑定表：service_id → {任务 Queue, 类型, Q0/Q1}

ucn_service_send(router, service_id, data):
    // 本机目标：直接投递到绑定任务 Inbox（Q0 FIFO / Q1 Latest），
    // 不经过帧编解码——这是"Fast Path"。
    // 远端目标：消息进入 Remote TX 队列，由 Bridge 每轮提交到 Core。

ucn_service_deliver_remote(router, message):
    // 远端到达本机的服务消息 → 校验 → 投递到对应绑定任务 Inbox
```

### 12.2 跨节点 Bridge（把任务消息接上网络）

```
ucn_service_protocol_bridge_init(bridge, node, router):
    // 在 Core 上安装 0xA0/业务 Endpoint handler，注册收发回调

// 发送侧（Owner 每轮调用，有限请求数）：
ucn_service_protocol_bridge_step_at(bridge, now, max_requests):
    while 处理数 < max_requests:
        if 有 Q0 Pending（背压重试中）:
            if deadline 已过: 释放 + 上报 EXPIRED; continue
            if 未到下次尝试: break                     // 有界等待，不忙等
        else: message = remote_tx_take(router)          // 无消息 → break
        result = ucn_node_send_endpoint(...)            // 复用 Core 发送
        if result == UCN_ERR_NO_SPACE 且 Q0 且 允许重试:
            存入固定 Pending 槽（retries、deadline、next_attempt）
        else: complete_outbound(...)                    // 通知业务最终结果

// 接收侧（Core 分发到 Bridge handler）：
bridge_endpoint_rx(context, frame):
    // 高风险远端 Q0：入队前强制走产品 Validator（未注册 Validator → 安装失败关闭）
    if 绑定要求 Validator: if !validator(frame): 拒绝
    // 可选命令防重放表（固定槽）与 Command Guard 校验
    if 可选防重放开启 && replay_accept_command 判定重复: 拒绝
    ucn_service_deliver_remote(router, message)         // 投递到目标任务 Inbox
```

---

## 13. Extended：Transfer 大消息（T32~T8K，固定资源）

### 13.1 发送：直接档 vs 分片档

```
ucn_transfer_send(transfer, dst, endpoint, class, data, len):
    if 未初始化 / dst 非法 / 非静态 Endpoint / 档位非法: return UCN_ERR_ARGUMENT
    if len > class 上限 或 len > 8 KiB: return UCN_ERR_TOO_LARGE
    peer = find_peer(dst)
    if peer == NULL 或 class > peer 声明的最大档: return UCN_ERR_ACCESS

    // T32/T64：一条普通 Endpoint 帧直达（无端到端确认），完成回调=SENT
    if class <= T64:
        result = ucn_node_send_endpoint(node, dst, endpoint, Q1, data, len)
        回调(SENT); return result

    // T128~T8K：固定 TX Slot（每 Peer 有最大并发数；窗口 = min(本机, Peer 能力)）
    if 该 Peer 活跃 Slot 数 >= Peer 最大并发: return UCN_ERR_NO_SPACE
    slot = 空闲 TX Slot
    slot = { dst, endpoint, class, transfer_id=分配ID, data, total_length=len,
             window_size, message_crc32=crc32(data), deadline=now+档位期限 }
    return UCN_OK          // 只入槽；真正的分片由 step 按节奏发送
```

### 13.2 发送节奏：每轮 step 最多一个分片（天然限速 + 不饿死 Core）

```
ucn_transfer_step(transfer):
    expire_transfer_state()                 // 超时 TX/RX 槽回收
    for slot in TX 槽（活跃的）:
        if 在途未 ACK 字节数 >= window_size * fragment_data_limit: continue
        if ACK 等待超时: begin_window_recovery(slot)
            // Go-Back-N：从 acknowledged_offset 重发；重试次数用尽 → RETRY_EXHAUSTED
        if slot 可发: send_tx_fragment(slot)   // ★ 每轮最多一个分片
        // fragment_data_limit 按当前 Path/Bearer MTU 自适应：
        // 每个分片本身是一条普通 UCN 帧，中继照常转发，不感知"大消息"。
```

### 13.3 接收：重组 + 累计 ACK

```
handle_fragment(transfer, frame):
    decode fragment; if 非法: 拒绝计数; return
    binding = find_endpoint(fragment.target_endpoint)
    if binding == NULL 或 class 超限 或 总长超限 或 (要求 E2E 而帧未保护):
        send_ack(REJECTED); return

    if find_recent(已完成去重) 命中:          // 迟到/重复分片
        send_ack(OK, 已确认长度); return
    slot = find_rx_slot(transfer, frame, fragment)
    if slot == NULL:
        if !(START 且 offset==0): send_ack(OK, 累计缺口=0)   // 丢 START 时引导 Go-Back-N
        else: slot = allocate_rx_slot() 或 send_ack(NO_SLOT) // 固定 RX 槽，满则拒
    if slot 状态与 fragment 元数据不一致: send_ack(BAD_FORMAT); return
    if fragment.offset != slot.received_length:              // 乱序
        send_ack(OK, slot.received_length); return           // 累计 ACK 告诉对端"发到哪了"
    追加 fragment.data; slot.received_length += len; 刷新 RX deadline

    if slot.received_length != slot.total_length:
        send_ack(OK, slot.received_length); return           // 没齐：回累计进度
    if 无 END 标志 或 crc32(全量数据) != message_crc32:
        send_ack(INTEGRITY_FAIL); 清槽; return
    slot.completed = true; remember_completion(slot)          // 已完成去重表
    send_ack(OK, total_length)
    binding.handler(..., rx_handle)                           // 交付业务 + 显式 Handle
```

### 13.4 发送侧收 ACK

```
handle_ack(transfer, frame):
    decode ack: {transfer_id, endpoint, acked_length, code}
    slot = find_tx_slot(transfer_id, endpoint)
    if code == OK:
        slot.acknowledged_offset = max(现值, acked_length)    // 累计确认前移
        if acked_length == total_length: complete(SUCCESS); 回调
    elif code == INTEGRITY_FAIL / REJECTED / NO_SLOT / BAD_FORMAT:
        complete(对应失败); 回调                                 // 终态失败
```

### 13.5 资源契约（为什么说它是"有界"的）

- 固定 TX/RX Slot：**并发消息数是编译期上限**，不是来多少存多少；
- 中继只转发普通帧，**不缓存 8 KiB 完整消息**；
- RX 交付后必须由业务**显式 release**（`ucn_transfer_release_received`），
  完成态在固定 Hold 期后由 step 回收；
- 无端到端确认的 T32/T64 是"快速帧"，不是可靠传输；
- 超过 8 KiB 的文件/固件**不做无限帧**，应使用未来的分块流协议。

---

## 14. Extended：单层 Cluster 首阶段（自动分簇）

> 现状：Host 软件门禁通过（64/256/1000 节点模拟 + 四板 ESP32-S3 容量/故障测试）；
> 簇间互联、多级簇、小时级长稳仍是待办。以下伪代码只描述已实现的首阶段。

### 14.1 角色与主状态

```
角色：DETACHED → CANDIDATE → MEMBER / HEAD / BACKUP
产品提供：set_head_score(滤波后的动态评分)；sync_node_neighbors(一跳已准入邻居)
```

### 14.2 选举与加入

```
// DETACHED/CANDIDATE 观察窗口内收集邻居广告与评分：
observe_candidate(cluster):
    if 收到 HEAD 广告: 记录候选簇头 + 评分
    if 观察窗到期:
        if 本地评分最高（确定性比较，同分用稳定 tie-break）:
            start_election() → 成为 HEAD → 周期性发广告
        else: begin_join(最佳候选头)

// HEAD 侧：
handle_join_request(cluster):
    if 成员数 >= 容量上限: 拒绝（容量拒绝）
    else: 发送 JOIN_ACCEPT → 加入成员表

// MEMBER 保活：
send_keepalive() → HEAD 续租；HEAD 收不到 → 成员过期
MEMBER 租约到期: 进入 Grace（等待 Backup 接管）→ 超时 → DETACHED → 恢复流程
```

### 14.3 备份簇头与接管

```
// HEAD 周期性：给覆盖成员最广的成员分配 BACKUP，并同步成员快照
// BACKUP：跟踪主簇头心跳；missed >= 门限 且 主租约期满 → 发起接管
start_takeover():
    send TAKEOVER_PREPARE → 各成员 → 收集多数 ACK     // 多数派防双头
    send TAKEOVER_ANNOUNCE → 成员切换归属
    complete_takeover() → 自己成为 HEAD

// 有界消解双头：若出现两个 HEAD，按确定性规则有序 Stepdown，
// 落选者退回观察/候选，防止永久分裂。
```

### 14.4 失效恢复

```
// 无 Head 的成员进入恢复流程：
recovery_eligible = true
set_detached(...) → 随机/确定性退避（有界）→ send RECOVERY_DECLARE
    → 收到多数 ACK → declare_recovery_head() → 成为恢复簇头 → 重新广告
// FAST_FIXED 档：固定有线拓扑下使用更短的观察/恢复窗口（快速恢复）
```

### 14.5 step 骨架（对应 ucn_cluster_step 的真实结构）

```
ucn_cluster_step(cluster):
    if !config.enabled: return UCN_OK
    now = 单调时钟
    // 每个角色只处理自己的到期事项（全部 deadline 驱动）：
    if 角色 == MEMBER:
        租约到期 → Grace → 到期则转 DETACHED + 恢复资格
    if 角色 == HEAD:
        expire_members()                // 成员过期
        到期 → 发广告（发现/续租优先于备份复制）     // 防大簇控制预算耗尽
        到期 → 备份分配周期 / 备份心跳 / 接管广播 / 成员快照
        快照丢帧 → 有界重发（backup_resync）
    if 角色 == BACKUP:
        主簇头心跳超时计数；达到门限且主租约期满 → start_takeover()
        接管窗口内未达多数 → 放弃接管 → 回到恢复流程
```

---

## 15. 按需诊断（三件套，全部"默认拒绝 + 独立预算"）

```
// 1) Path Trace：查 A→D 实际经过哪些节点
//    - 只沿当前 Route Cache 查（不触发 RREQ、不锁定业务路径）
//    - 每跳追加自己的 Node ID，目标沿反向路径回 REPLY
//    - 固定 Pending/Reverse 表；默认需要产品 Authorizer 授权

// 2) Node Snapshot：低频查看可见节点
//    - 受限泛洪（有 Hop/预算上限）；回复加随机短延迟防同步风暴
//    - 默认拒绝远端请求：产品必须显式配置管理节点授权

// 3) Policy Diagnostic：单节点策略/Path/Flow/质量快照
//    - 8 B 请求 / 32 B 回复；Summary + 分页查询
//    - 独立 Token Bucket；普通业务帧零额外诊断字段
```

---

## 16. 核心不变量速查（协议"宪法"，违反即 Bug）

1. **唯一 Owner**：只有 Protocol Task 访问 `ucn_node_t`；ISR 不进入 Core。
2. **fail-closed**：安全要求开启而 Provider 不完整 → 拒绝一切收发。
3. **HELLO 一跳**：不转发、不进应用、只绑定 Node ID 与准入。
4. **控制帧不端到端加密**：控制面逐跳语义，业务面才 E2E。
5. **Q0 不等待寻路**：无路由立即失败；只有 Q1 能入 Pending 并触发发现。
6. **Latest-value 是 Q1 的灵魂**：Pending 合并同 (dst, endpoint)；旧值绝不重传。
7. **所有容量编译期固定**：表满/令牌尽 = 显式错误码，无驱逐、无动态扩容。
8. **中继只读头**：转发改 Hop 与 RREP Cost，密文 Payload 原样透传（透明密文中继）。
9. **扩圈有界**：2→4→8→16，每环 250 ms，总预算 1 s，发现槽固定 4 个。
10. **Epoch + Grace 防环**：只接受 Current/Previous Epoch；Previous 有 1 s 宽限。
11. **错误必须显式**：转发失败回 RERR；无"静默吞帧"（过期重复除外）。
12. **业务突发不饿死维护**：连续业务发送有计数上限，到期保活/维护可抢占。
13. **Pending Q1 不刷新绝对 Deadline**：内部重试只消耗剩余时间。
14. **诊断永远在业务之后**，且有独立 Token 预算与授权门禁。

---

## 附录 A：伪代码 ↔ 真实符号对照

| 伪代码章节 | 主要真实符号（文件） |
| --- | --- |
| 2 帧 | `ucn_frame_encode / ucn_frame_decode / ucn_frame_peek_wire_profile / ucn_frame_select_min_wire_profile`（src/core/ucn_frame.c） |
| 3 接收 | `ucn_node_receive`（src/node/ucn_node.c，~9300 行文件的总入口） |
| 4 邻居 | `handle_hello / ucn_node_observe_neighbor / ucn_node_admit_neighbor / handle_heartbeat / maintain_neighbor_liveness` |
| 5 去重 | `ucn_duplicate_accept_frame`（src/node/ucn_duplicate_internal.h） |
| 6 路由 | `handle_route_request / forward_route_request / handle_route_reply / handle_route_error / begin_route_discovery / send_route_discovery_ring` |
| 7 转发 | `ucn_node_receive` 的转发分支、`find_link_for_route_epoch / send_frame_on_logical_egress / send_frame_on_path_egress` |
| 8 发送 | `ucn_node_send_endpoint / send_endpoint_auto_best / send_endpoint_pinned / send_endpoint_auto_balance / ucn_node_send` |
| 9 调度 | `ucn_node_step / send_pending_q1_if_ready / send_due_essential_maintenance` |
| 10 安全 | `ucn_node_set_security / node_security_ready / authorize_rx / protect_outbound_business / validate_inbound_business_security`（`ucn_security_ops_t` 见 include/ucn/ucn_security.h） |
| 11 Path | `handle_path_install / handle_path_revoke / handle_path_probe / handle_path_activate / ucn_path_install`（src/routing/ucn_path.c） |
| 12 Service | `ucn_service_* / ucn_service_protocol_bridge_*`（src/service/） |
| 13 Transfer | `ucn_transfer_send / ucn_transfer_step / handle_fragment / handle_ack / ucn_transfer_release_received`（src/extended/ucn_transfer.c） |
| 14 Cluster | `ucn_cluster_init / ucn_cluster_receive / ucn_cluster_step / start_takeover / declare_recovery_head`（src/extended/ucn_cluster.c） |
| 15 诊断 | `handle_path_trace_* / handle_node_snapshot_* / handle_policy_diagnostic_*` |

## 附录 B：阅读建议

1. 先背下第 1 章三条主链；
2. 精读第 3 章（receive）与第 9 章（step）——协议 80% 的行为在这两个函数；
3. 需要追路径时对照 [调用关系树](../calltree/README.md) 的 YAML 与本文附录 A；
4. 本文不替代源码：任何细节争议以 `src/` 与 `tests/` 为准。
