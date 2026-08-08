# UCN v4 协议核心说明：主要做什么、怎么做

> 状态：**基于 UCN v4 C99 Core 源码快照（2026-08-08）**。本文重点解释协议目标与运行机制；已实现的 C99/虚拟拓扑能力、真实硬件验证和后续设计严格分开。  
> 关联：[整体架构设计](UCN_整体架构设计.md) · [协议分层与配置档案](UCN_协议分层与配置档案.md) · [Adapter 契约](UCN_Adapter_契约.md) · [v4 节点快照诊断](UCN_v4_节点快照诊断.md) · [任务表](00-任务表.md)

## 1. 一句话定位

UCN v4 是一个以 **MCU 为中心** 的轻量通信与自组网 Core。它的目标不是重写 WiFi、CAN、Linux 或 ROS2，而是在这些不同底层之上，让 MCU 节点能用同一套规则完成：

```text
我是谁 → 谁和我直连 → 目标在哪里 → 下一跳交给谁 → 断开后如何恢复 →
同一目标中的哪类数据 → 是否需要端到端保护 → 如何按需诊断网络
```

因此，多个 MCU 即使没有 Linux，也能组成有限多跳网络；Linux/ROS2/MAVLink 只是可选的业务接入端或网关，不是路由中心，也不是网络存在的前提。

## 2. UCN 解决的核心问题

| 问题 | UCN v4 的做法 | 不做什么 |
| --- | --- | --- |
| 不同底层怎样统一通信 | 应用只面对 Node ID、Endpoint、QoS 和发送 API；ESP-NOW、UART、CAN 等由 Link/Adapter 接入。 | 不实现 WiFi MAC、CAN 控制器、TCP/IP 或厂商无线协议。 |
| 没有 Linux 怎样自组网 | MCU 中的 Core 发现一跳邻居、维护路由缓存、按需寻路并转发。 | 不依赖固定 Linux 主机、云端或中心服务器。 |
| 数据怎样去目标节点 | 直连优先；无路由时受限 `ROUTE_REQ/REPLY` 建立 AODV-Lite Route Cache；之后业务帧只走当前下一跳。 | 不让每个节点常驻保存全网完整拓扑，也不让每帧重新泛洪寻路。 |
| 目标节点有多类数据怎样区分 | `Endpoint` 区分 IMU、温度、命令等业务入口；一条 Node 间路径可承载多个 Endpoint。 | 不把业务分类与物理链路、MAC 地址或 CAN ID 绑定。 |
| 链路质量变化怎么办 | Adapter 把本介质指标归一成 `route_cost`；Core 在寻路和候选路径流程中比较 Cost。 | Core 不直接读取 RSSI、SNR、Bus-Off 等介质专用字段。 |
| 需要保密但中继不应看到内容 | 业务帧可端到端受保护；中继按路由头透明转发密文，目标节点才解密。 | 当前 Core 不自带生产 AEAD、密钥发放或证书体系。 |

## 3. 协议的最小运行闭环

```mermaid
flowchart LR
    APP["MCU 应用\n传感器、飞控、电机等"] --> API["Endpoint / Q0-Q1 API"]
    API --> CORE["UCN-Core\n身份、邻居、路由、转发、健康、安全边界"]
    CORE --> LINK["统一 Link"]
    LINK --> MEDIA["ESP-NOW / UART / CAN-FD / RS485 / BLE / LoRa"]
    HOST["可选 Linux / ROS2 / MAVLink Host"] --- LINK
```

Core 只依赖抽象的 `Link`、时间/随机数/临界区等 Port 接口；它不依赖 ESP32 SDK、Linux Socket 或某一种无线技术。这样同一份协议逻辑可以先在虚拟 Link 中测试，再接到不同硬件 Adapter。

一个典型 MCU 网络的实际逻辑如下：

1. 应用在启动前提供稳定的 `Node ID`、Network ID 和策略配置。Node ID 的来源可以是编译期固定值、Flash/NVS、出厂分配或身份 Provider；这属于产品启动层，不应写死在 Core 内。
2. Adapter 发现物理对端后建立 Candidate Link，Core 通过一跳 `HELLO` 绑定对端 Node ID，并按 Manual/Open/Provider 策略决定是否准入。
3. 已准入的直连邻居进入 Neighbor Table。业务有直连出口或有效 Route Cache 时，直接向选定下一跳发送。
4. 目标未知时，Q1 Endpoint 可在固定等待槽内受限发起 `ROUTE_REQ`；`ROUTE_REPLY` 沿反向路径返回并建立缓存。Q0 不等待寻路，避免控制面阻塞紧急业务。
5. 中继节点只需知道“该帧的下一跳”，而不必保存源到目标的完整路径。转发时 TTL 递减；链路失效时清理相关路由并按需回送 `ROUTE_ERROR`。
6. 后续业务复用 Route Cache，而不是每次传输都寻路。缓存/候选路径维护与业务转发分开进行。

## 4. Core 内部每一块做什么

| Core 模块 | 主要职责 | 当前 v4 实现边界 |
| --- | --- | --- |
| Frame | 固定头、版本、Network ID、源/目的 Node ID、TTL、消息类型、CRC 和 Flag 编解码。 | 协议版本为 4；基础头 32 B，带 Route Extension 为 36 B；受保护业务额外带 16 B Tag；旧 v3 帧会拒绝。 |
| Identity / Neighbor | 用 HELLO 发现并绑定一跳对端，维护 Candidate、Admitted、Suspect、Removed 等邻居状态。 | HELLO 不转发；生产 `JOIN_REQ/CHALLENGE/ACCEPT` 状态机和出厂身份格式仍未实现。 |
| Health | 用一跳 Heartbeat 和有效业务帧刷新邻居存活；超时后局部回收 Link 和动态路由。 | Heartbeat 只在直连邻居之间收发，不做全网心跳广播。 |
| Route / Forward | Route Cache、受限 AODV-Lite、`ROUTE_REQ`、`ROUTE_REPLY`、`ROUTE_ERROR`、TTL 与去重。 | 固定 Active/Candidate 表；不是全网拓扑数据库，也没有无限制泛洪。 |
| Path switch | 在旧 Active 路径仍传业务时，探测 Candidate，确认后 Activate。 | 使用 Candidate/Probe/Activate、Cost 比较、Route Epoch 与 Current/Previous 短暂 grace，降低切换期间在途帧混淆；真实空口无缝指标尚未测量。 |
| Endpoint / QoS | 把“发给哪个 Node”与“Node 内哪类数据”分开；调度 Q0/Q1。 | 静态业务 Endpoint 为 `0x40..0xBF`；Q0 为普通尽力而为，Q1 为最新值优先。Q2/Q3、可靠确认和大包分片尚未实现。 |
| Security boundary | 发送/接收策略、Session/Counter、授权与可插拔 `seal/open` Provider。 | 支持按 Node/Endpoint 选择明文或端到端受保护、目标解密和中继透明转发；生产密钥、审计过的 AEAD、轮换/撤销和产品 ACL 表仍待接入。 |
| Diagnostics | 只在需要时查询网络状态。 | 已有按需路径追踪 `PATH_TRACE_REQ/REPLY` 和按需节点快照 `NODE_SNAPSHOT_REQ/REPLY`；二者独立限流、固定表项，默认不维护永久全网地图。 |

## 5. 路由、质量与切换是怎样协同的

UCN 不会在每次业务发送前重新寻找完整路径。正常流程是“**缓存发送，后台受限维护，故障优先恢复**”：

```text
已有 Active 路由
  → 业务帧只沿 Active 的下一跳转发
  → Adapter 持续提供已平滑的 route_cost / Link 状态
  → Core 在允许的刷新窗口发现或检查 Candidate
  → Candidate 先 Probe，确认后 Activate
  → 新旧 Route Epoch 短暂并存，之后回收旧路径

链路硬故障 / 邻居离网
  → 立即使本地相关出口失效
  → 业务命中失效路径时按需 RERR 与重新发现
```

`route_cost` 是跨介质的统一抽象：WiFi Adapter 可以综合 RSSI、丢包、重试；CAN Adapter 可以综合错误、拥塞或 Bus-Off；UART 可以综合 CRC、超时和队列压力。采样、平滑和防抖属于 Adapter，Core 只比较最终 Cost，因此不会把 WiFi 特有逻辑带入 CAN/UART。

当前已实现的是固定容量的 Active/Candidate 控制闭环和虚拟拓扑验证；Cost 的实机采样周期、抖动窗口、吞吐与切换丢包上限必须在具体 Adapter 和多板环境中测量后冻结。

## 6. 对外接口与对内接口的边界

### 对应用：统一业务接口

应用应关注 Node ID、Endpoint、QoS 和安全策略，而不是“这次从 WiFi 还是 UART 到达”。主要接口围绕：

- `ucn_node_send_endpoint()`：向指定 Node 的指定 Endpoint 发送业务数据；
- `ucn_node_discover_route()`：显式发起按需寻路；
- `ucn_node_request_path_trace()`：按需查询一条到目标的实际经过节点；
- `ucn_node_request_node_snapshot()`：按需收集当前网络可见节点的诊断快照。

### 对介质：统一 Adapter/Link 接口

物理驱动负责把“收到的字节和对端物理地址”转换成 Candidate Link，并放入有界 RX 队列；协议任务再通过 `ucn_adapter_rx_pump()` 将帧交给 Core。WiFi/串口/CAN 的中断或回调不应直接执行路由和应用回调。

这种分层的价值是：增加一个新介质时，通常只需要写该介质的 Adapter、地址映射和 Cost 归一化，不需要把应用业务或路由算法分别重写一遍。

## 7. V4 相比 V3 新增的重点

V4 不是推翻 V3 的重新设计，而是在既有“邻居—寻路—转发—安全边界”闭环上，升级协议版本并新增 **按需节点快照诊断**：

- 管理节点显式发起一次受限 Snapshot 请求；
- 节点只在短窗口内处理/转发一次，并沿临时 Reverse 回送自己的简要状态；
- 源节点在窗口结束时汇总固定容量结果；
- 默认拒绝远端请求，且独立限流；
- 不建立永久全网拓扑表，不给正常 Q0/Q1 业务帧增加字段。

它适合多板调试、安装验收和网络诊断；它不是常驻路由协议，也不是业务转发依赖。

## 8. 资源与适用范围

UCN Core 的设计原则接近 FreeRTOS 风格的小内核：固定帧、固定数组、编译期容量、无动态拓扑表、无隐藏后台线程。RAM/Flash 预算不先承诺一个统一数字，而由目标 MCU 的 RAM、节点数量、最大 Link/Neighbor/Route、队列深度、是否启用安全和诊断功能共同决定。

适合的场景包括：多 MCU 传感器/执行器网络、飞行器或机器人内部网络、ESP32 多板实验、WiFi/UART/CAN 混合接入，以及需要以后接入 Linux 监控或 ROS2 的设备网络。

不应把 UCN 误认为以下系统的直接替代品：

- 它不替代 WiFi Mesh/802.11s 的 WiFi MAC 与空口调度；WiFi 只是一种可接入的 Bearer。
- 它不替代 DroneCAN 的具体 CAN 生态和设备数据类型；它可在上层统一不同介质，但经典 CAN 仍需要 Carrier 分段。
- 它不替代 Linux 网络栈、DDS、ROS2 或 MAVLink；这些更适合作为 Host/业务层接入。
- 它不是文件传输、OTA 或高吞吐大数据协议；这些需要后续 Extended 的可靠与分片能力。

## 9. 当前已经证明与尚未证明的边界

| 范围 | 当前结论 |
| --- | --- |
| 已由代码与虚拟拓扑验证 | v4 帧与版本拒绝、HELLO/邻居、Heartbeat、AODV-Lite、Q0/Q1、Endpoint、Active/Candidate 切换、Route Epoch、透明密文转发边界、路径追踪、节点快照。 |
| 已构建但仍需实机验证 | ESP32 测试工程可构建；真实 ESP-NOW 多跳、无线覆盖、断链收敛、吞吐、功耗、RAM/Flash/栈、多个介质并行表现仍需多板测试。 |
| 后续实现 | 生产 JOIN/身份、经审计 AEAD 和密钥生命周期、经典 CAN Carrier 分段、同对端多 Bearer 主备（T21）、指定路径/策略路由和可选负载均衡（T22）、Q2/Q3、可靠传输、分片与服务目录。 |

所以，V4 当前最准确的描述是：**一个已经具备 MCU 独立组网核心闭环、固定资源约束和虚拟测试证据的 C99 Core；真实介质与生产安全正在通过后续 Adapter 和多板测试逐步落地。**

