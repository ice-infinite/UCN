# 发送、接收与 Step 完整调用链

本章把“函数字典”重新拼成运行顺序。理解 UCN Core 至少要掌握 TX、RX、Step 和 Driver→Owner 四条链。

## 1. 系统启动顺序

推荐产品初始化顺序：

```text
1. 准备全局/产品配置宏
2. 静态分配 Node、Link、Adapter Queue、Runtime、Source、Service存储
3. ucn_node_init
4. 配置 Wire TX/RX 与自动选档
5. 设置 Plain Session 或安装 Security Provider/Policy
6. 设置 Join/诊断/Path授权策略
7. 初始化产品驱动和每个 Link Ops
8. 设置单Link本地RX上限
9. ucn_node_register_link
10. 安装静态Route/Endpoint/Service/Policy/Path
11. 初始化 Adapter Queue / Owner / Event Runtime
12. 绑定 Stream/CAN/Packet Source
13. 开中断或启动驱动
14. Owner循环 task_cycle()/run()/step()
```

关键原则：本地 RX ceiling、安全门和授权策略应在网络流量进入前完成；不要先开中断再补配置。

## 2. Driver 到 Node 的接收链

### 2.1 Packet 型介质

Wi-Fi、ESP-NOW、USB packet 或能直接给出完整 UCN Frame 的驱动：

```text
Driver ISR/Callback
  → ucn_event_runtime_submit_frame_from_isr
      → ucn_protocol_owner_rx_enqueue_from_isr
          → ucn_adapter_rx_enqueue_from_isr
              → 复制完整Frame到固定Queue
      → signal Owner

Owner醒来
  → ucn_event_runtime_task_cycle
  → ucn_event_runtime_run
  → ucn_protocol_owner_step
  → ucn_adapter_rx_pump
  → ucn_node_receive
```

### 2.2 Stream 型介质

UART、RS-485、USB CDC：

```text
UART RX ISR/DMA回调
  → ucn_stream_source_write_from_isr(bytes)
      → 写Byte Ring
      → signal_source(RX_READY)

Owner
  → Event Runtime调用Source私有service()
      → 找0分隔符
      → COBS解码
      → resolve ingress Link
      → ucn_event_runtime_submit_frame
  → Adapter pump
  → ucn_node_receive
```

### 2.3 CAN

```text
CAN ISR
  → SDK frame归一化为ucn_can_frame_t
  → ucn_can_source_write_from_isr
      → 写物理Frame Ring
      → signal_source(RX_READY)

Owner Source service
  ├─ CAN-FD：解析真实UCN长度 + 检查零Padding
  └─ Classic CAN：START/CONTINUE固定槽重组
  → resolve ingress Link
  → submit_frame
  → Adapter pump
  → ucn_node_receive
```

## 3. Node RX 的阶段顺序

Full/Lite 的总体顺序：

```text
ucn_node_receive(node, ingress_link, data, length)
  1. 参数与Security Ready总门
  2. ingress Link归属检查
  3. ucn_frame_peek_wire_profile
  4. Node/Link本地RX ceiling
  5. ucn_frame_decode + CRC
  6. Network ID
  7. validate_inbound_hop_scope
  8. 入站Security/解密/授权
  9. Duplicate或RREQ专用分类
 10. HELLO未准入特例 / 已准入Link检查
 11. 刷新Neighbor活动
 12. 控制帧handler
 13. 非本机帧转发
 14. 本机Endpoint或通用handler分发
```

审计时最重要的是检查 1～10 是否在任何危险状态写入前完成。例如 CRC 失败、超接收档、错误 Network、超 Hop、安全失败和 Replay 都不应学习 Route 或刷新未经授权的存活状态。

## 4. 业务发送链

### 4.1 立即发送

```text
Application
  → ucn_node_send_endpoint
      → Endpoint和参数
      → Profile相关选路
         Nano: 直连/静态Route
         Lite: 活动Route/当前Primary Bearer
         Full: Policy→Path/Route/Balance
      → 构造ucn_frame_t
      → 分配Sequence/Session
      → protect_outbound_business（Lite/Full）
      → prepare_outbound_wire_profile
      → send_frame_on_link
          → link->ops->get_status
          → effective MTU
          → ucn_frame_encode
          → link->ops->send
```

`UCN_OK` 表示本地发送调用成功，不代表远端任务已经执行。远端完整大消息要看 Transfer completion；远端业务执行要看 Service Result/应用 ACK。

### 4.2 Queue 发送

```text
Application/Service Bridge
  → ucn_node_enqueue(request)
      → 校验delivery/QoS/deadline
      → payload 复制进 Q0～Q3 对应固定 Item
      → 通知Owner（由外层Runtime/Port负责）

Owner
  → ucn_node_step(now_ms)
      → Q0优先
      → Q1
      → deadline / next_attempt
      → 选路并发送
      → 背压重试或释放Item
```

数据不等待 Heartbeat 周期；有事件时 Owner 应立即醒来。周期扫描只是丢中断、计时器和维护工作的保底。

## 5. Route Discovery 调用链

```text
send时无Route / 显式discover_route
  → begin_route_discovery
  → initial_route_discovery_hop_limit
  → send_route_discovery_ring
      → 构造RREQ
      → 每个合格Link发送

中继收到RREQ
  → validate_route_request_frame
  → classify_route_request(New/Better/Replay)
  → 控制预算
  → commit_route_request
  → learn_route(source的反向Route)
  ├─ 自己是目标：发送RREP
  └─ 不是目标：forward_route_request

源收到RREP
  → Lite: learn_route
  → Full: 直接学习或learn_candidate_route
  → Route可用后发送pending Q1/应用重试
```

RERR 链：

```text
Link Down / Route不可达
  → invalidate_routes_by_link或invalidate_route_to
  → 找依赖方向
  → send_route_error
  → 上游收到后只失效匹配Route/Epoch
```

## 6. Full Path 调用链

### 6.1 Provisioning

```text
Controller
  → ucn_node_send_path_install[_capable]
  → send_path_install_internal
  → send_control_to_node_profile

目标/中继RX
  → handle_path_install
      → Payload/Identity/remaining-hops
      → authorize_path_control
      → source/session Token
      → next-hop与Bearer能力
      → install_path_forward_entry
      → ucn_path_install_capable
```

### 6.2 Path 业务转发

```text
ucn_node_send_path
  → 查源侧Path
  → validate_frame_for_path_capability
  → send_frame_on_path_egress

中继ucn_node_receive
  → frame.has_path_id
  → ucn_path_find(owner, session, path, destination)
  → remaining hops / next hop / MTU / Wire capability
  → Hop递减
  → send_frame_on_path_egress
```

## 7. Full Policy 调用链

```text
ucn_node_send_endpoint
  → send_endpoint_internal
  → find_route_policy(key)
  ├─ PINNED_STRICT
  │    → send_endpoint_pinned
  │    → Path失败直接返回
  ├─ PINNED_FAILOVER
  │    → Path失败判定pinned_path_has_hard_failure
  │    → 允许时send_endpoint_auto_best
  ├─ AUTO_BEST
  │    → send_endpoint_auto_best
  └─ AUTO_BALANCE
       → send_endpoint_auto_balance
       → 查Q1 Flow
       → auto_balance_select_path
       → auto_balance_bind_path
       → send_endpoint_on_policy_path
```

## 8. Protocol Owner 的 Step 顺序

公共 Owner 每轮只读取一次平台时间：

```text
ucn_protocol_owner_step
  → now_ms = port_ops->now_ms(context)
  → ucn_adapter_rx_pump(max_rx_frames)
  → 可选 ucn_service_protocol_bridge_step_at(now_ms, budget)
  → ucn_node_step(now_ms)
```

这样 RX、Service 和 Node Maintenance 使用同一时基，不会因为三个组件分别读时钟而产生同轮不同 deadline 判断。

## 9. Node Step 的逻辑顺序

Full 大致包含：

```text
ucn_node_step
  → observe_step_interval
  → 保存node->now_ms
  → expire_dynamic_state
  → maintain_neighbor_liveness
  → Bearer质量评估/Probe/切换
  → Candidate Probe/Activate
  → Route Discovery Ring
  → Heartbeat
  → Path expire
  → Policy quality/path/flow维护
  → Diagnostic pending/reply维护
  → Q0～Q3 业务 Queue
  → 有界Essential Maintenance
```

具体源码顺序以当前 `ucn_node_step()` 为准。审计时要记录每一项的固定预算以及某项返回错误后后续维护是否仍会执行。

## 10. 回调边界

以下回调会把控制权交回产品代码：

- `link->ops->open/get_status/send`；
- Security Provider；
- Join/Path/Diagnostic Authorizer；
- Endpoint/RX handler；
- Service validator/observer；
- Event Runtime scheduler hooks；
- Source ingress resolver。

每到一个回调都应问：调用前是否已建立重入门、对象是否处于可观察的中间态、回调失败后能否原子回退、ISR 是否误调用了只允许任务上下文的回调。
