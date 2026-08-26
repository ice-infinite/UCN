# UCN 总体架构与设计原则

> 文档级别：`NORMATIVE`
> 实现状态：`CURRENT`
> 事实源：`CMakeLists.txt`、`src/README.md`、`include/ucn/`
> 最近核对：`a093862`，2026-08-25

## 分层

```text
业务任务 / 传感器 / 控制逻辑
              │
       Endpoint / Service
              │
      UCN Core / Node / Route
              │
       Link / Adapter / Source
              │
 产品 Driver / BSP / DMA / ISR
              │
 UART / CAN / Wi-Fi / BLE / LoRa ...
```

旁路扩展：

- `ucn_transfer` 在 Endpoint 之上提供大消息；
- `ucn_cluster` 在一跳邻居摘要之上提供单层控制面；
- `ucn_cluster_federation` 在 Cluster 之上提供 Locator/Directory/Tunnel；
- Port 负责把裸机或 RTOS 的调度、通知和临界区接入唯一 Protocol Owner；
- Linux/ROS 2/地面站位于 Host/Bridge 侧，不是 Core 依赖。

## 设计原则

### MCU-first

Core 只依赖 C99 和调用者提供的对象/回调。没有 Linux、Socket、线程库或动态内存时仍可运行。RTOS Port 和 Host 都是独立 target。

### 小核心、按需链接

`ucn_core` 包含基础 Frame、Node、Adapter、Source、可选 Routing/Service；Transfer、Cluster、Federation 和实验模型是独立静态库。产品不链接就不承担对应对象 RAM 和代码。

### 多介质统一但不抹平物理事实

Core 统一 Node ID、Endpoint、Route、Path、Cost 和 QoS；Adapter 仍必须报告 MTU、队列、失败、RTT、质量和介质状态。Wi-Fi 与 CAN 不会被错误假设为同一种 Driver。

### 有界与确定性

邻居、Link、Route、Candidate、Path、队列、Transfer Slot 和 Cluster 表都使用编译期上限。表满明确返回错误或按冻结策略替换，不动态扩容。

### 失败关闭

版本、长度、CRC、权限、Feature、MTU、Hop、State 或持久化证据不满足时拒绝，不静默截断、不假装成功、不降级授予权限。

### 证据分层

软件回归证明协议状态机；Host 模拟证明给定模型；实机报告只证明指定固件、板卡、拓扑和环境。三者不能相互冒充。

## 1. 每一层具体解决什么问题

### 1.1 业务与 Service 层

业务任务只处理传感器、控制和应用状态。它不应读取 Route Cache、操作 UART FIFO 或判断哪条 Bearer 更好。简单数据注册 Endpoint handler；需要请求/结果、任务 Inbox 或本机 Fast Path 时使用 Service。

这一层的核心问题是“数据属于哪个业务”，而不是“数据从哪条线过来”。

### 1.2 Node 与 Core 层

Node 持有一个逻辑节点的协议状态：Link、Neighbor、Route、发送队列、重复窗口、Endpoint 和统计。Core Frame 把源、目标、Endpoint、Traffic Class、Sequence、Hop 和 Payload 编码为稳定线上格式。

这一层决定“数据应到哪个 Node、是否重复、能否转发、下一跳是谁”。

### 1.3 Routing/Policy 层

Lite/Full 的 Dynamic Mesh 在没有预配置路线时建立 Route。Full 的 Path/Policy 在多个候选上做固定路径、失败回退和 Q1 负载均衡。它只使用通用 Cost/MTU/状态，不直接理解 RSSI 或 CAN Bus-Off 的厂商定义。

### 1.4 Adapter 与 Source 层

Adapter 把 Core 的 Link send 调用映射到实际 peer/物理通道，并把接收的完整 Frame 放入固定队列。Source 位于更靠近驱动的位置：Stream Source 从字节 Ring 解出 COBS Frame，CAN Source 从 CAN/CAN-FD 物理帧重组 Carrier。

### 1.5 Port 与 BSP 层

Port 提供单调时间、task/ISR 临界区、通知和 Owner 调度壳。BSP/Driver 配置寄存器、引脚、DMA、Filter、无线 peer 和收发器。这一层是平台相关代码唯一应集中的地方。

### 1.6 Extended 层

- Transfer：解决一个 Core Frame 放不下的有界大消息；
- Cluster：解决成员控制域、Head/Backup、Authority 和恢复；
- Federation：解决跨 Cluster Locator/Directory/Tunnel；
- 它们通过 Core 公开接口通信，不把私有字段塞回 `ucn_node_t`。

## 2. 为什么架构不直接模仿 TCP/IP

TCP/IP 面向更通用的主机网络，通常假设较大的缓冲、动态内存、线程/Socket 和成熟链路层。UCN 借鉴分层、地址、路由、缓存和可靠传输思想，但做了 MCU 约束下的取舍：

| 问题 | 通用主机常见做法 | UCN 做法 |
| --- | --- | --- |
| 内存 | 动态 socket buffer | 编译期固定表和 slot |
| 传输 | 所有数据可走流/数据报 | 小 Frame + 可选 Transfer |
| 路由 | 大路由表/中心基础设施 | 受限 AODV-Lite 和局部缓存 |
| 线程 | 多线程网络栈 | 唯一 Protocol Owner |
| 接口 | 统一 IP MTU 抽象 | 保留每 Link 实际 MTU/Cost |
| 功能 | 主机通常全量启用 | Profile 和独立 Archive 裁剪 |

这不是为了比 TCP/UDP 更全面，而是为了让 MCU 在小资源、异构链路和无 Linux 条件下获得统一网络语义。

## 3. 为什么 Link 与 Driver 必须分开

如果 Core 直接调用 `uart_write()`、`esp_now_send()` 或 `HAL_FDCAN_AddMessageToTxFifoQ()`：

- Core 会依赖具体 SDK；
- 同一协议无法在裸机、FreeRTOS 和 Zephyr 复用；
- 多个 UART/CAN 实例难以统一管理；
- 单元测试必须依赖真实硬件。

拆分后，Core 只调用类似“向这个 Link 发送完整 Frame”的回调。产品 Adapter 再根据 Link context 选择 UART1、CAN2 或无线 peer。Host Fake Link 也可以用同一合同进行多节点模拟。

## 4. 设计原则如何影响失败行为

### 表满

固定表达到上限时返回 `UCN_ERR_NO_SPACE`，或执行模块明确冻结的替换策略。不能临时 `malloc`，也不能覆盖仍活跃的 Route/Transfer/Persistence transaction。

### MTU 不足

Core 选择能表达字段的最低 Wire Class，再与 Link/Path MTU 求交。无法承载时返回能力/配置错误；大消息由 Transfer 分片，不能截断 Payload。

### Feature 缺失

Lite 调用 Full-only Path API 会返回 `UCN_ERR_CONFIG`。这比假装安装成功更安全，因为应用可以在启动阶段发现产品配置错误。

### Authority 不成立

Cluster Role 即使显示 HEAD，只要 quorum、lease、Config、Persistence 或 Fence 不满足，也必须禁止权威发送。架构把“身份显示”和“产生副作用资格”分开。

## 5. 典型部署形态

### 5.1 最小两节点

两个 Nano 节点通过 UART，使用静态 Route 和 Endpoint。没有 HELLO/AODV/Cluster，资源最小。

### 5.2 多介质动态网络

多个 Full 节点同时注册 UART、CAN 和无线 Link。Neighbor 记录同一 peer 的多个 Bearer，Route/Policy 选择当前可用路径。应用始终使用 Node ID。

### 5.3 MCU 网络加 Linux 网关

MCU 自行组网。Linux 网关作为一个 UCN Node，把 Service/Endpoint 映射到 ROS 2、MAVLink 或运维系统。Linux 断开时 MCU 网络仍继续工作。

### 5.4 带 Cluster 的大网络

Core 继续负责普通业务逐跳转发；Cluster 只组织成员和 Authority。即使 Cluster 被禁用，Core Route/Transfer 不应停止工作。

## 6. 架构验收问题

一个新模块合入前至少回答：

1. 它属于哪一层，是否重复了相邻层职责；
2. 是否让 Core 依赖具体 SDK、RTOS 或动态内存；
3. 是否有固定容量、明确所有权和失败返回；
4. 是否能在默认关闭时不增加 Core-only RAM；
5. ISR 是否只投递，状态机是否仍由 Owner 串行推进；
6. 软件证据与硬件证据是否分开陈述。

无法回答这些问题的功能不应直接塞进 `ucn_node_t` 或生产 FSM。
