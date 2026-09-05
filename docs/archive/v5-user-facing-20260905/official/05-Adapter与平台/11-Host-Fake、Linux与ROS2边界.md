# Host Fake、Linux 与 ROS 2 边界

> 文档级别：`NORMATIVE`
> 实现状态：Host Fake `CURRENT`；Linux/ROS 2 生产桥未作为本仓库完整组件发布
> 最近核对：`a093862`，2026-08-25

## Host Fake

`ucn_port_host_fake` 是确定性测试 Port，复用与产品 Port 相同的 notify/wait/Owner 边界。它不代表 POSIX Socket、网卡或真实 Linux 调度性能。

## Linux

Linux 可以：

- 作为普通 UCN Node 接入串口/CAN/UDP tunnel；
- 承担日志、配置、升级或 Directory 服务；
- 桥接 UCN Endpoint 到 Socket、数据库或 UI。

Linux 不是 MCU Neighbor、Route、Transfer 或 Cluster Current FSM 的必需中心。

## ROS 2

ROS 2 Bridge 应在 Host 应用层做 Topic/Service/Action 与 UCN Endpoint/Service 的映射。UCN 不实现 DDS discovery/QoS，也不建议 MCU 为兼容 ROS 2 而承担完整 DDS 栈。

## 边界

当前仓库没有完整 POSIX Port、ROS 2 package 或 MAVLink gateway target。官方文档只能定义集成架构，不能声称已提供可直接运行的 Host 产品。

## Host Fake 能证明什么

Host Fake 用确定性时钟、通知和固定队列验证 Port/Owner 的公共合同：初始化、RX enqueue、step/wait、Deadline、公平和统计。它适合单元/状态机/压力模拟，因为测试可精确控制时间和事件顺序。

它不能证明：

- Linux pthread 实际调度延迟；
- Socket/CAN/UART Driver 行为；
- MCU ISR、DMA、Cache 和功耗；
- 真实网络拥塞和 RF；
- 产品 daemon 的权限/升级/服务管理。

## Linux Node 的推荐分层

```text
systemd/service process
├─ POSIX Port: clock, poll/epoll, owner thread
├─ Adapter: SocketCAN / termios / UDP tunnel
├─ UCN Node/Service/Transfer
└─ application bridge: CLI/UI/database/ROS2
```

Linux 端仍应遵守一个 Node 一个 Protocol Owner。epoll 多 fd 可以并行产生事件，但由一个 owner loop 串行提交 Core；重 CPU 业务交给其他 worker，通过 Service/Queue 回传。

## Linux 不是网络中心

没有 Linux 时，MCU 仍能通过 HELLO、Route、Path 和 Transfer 自组网。加入 Linux 只是增加一个普通或资源更强的 Node，可承担观测、配置、网关和持久化服务。系统不应把每次 MCU→MCU 数据都强制绕 Linux，除非产品拓扑明确如此。

## ROS 2 映射建议

| ROS 2 | UCN 映射 |
| --- | --- |
| Topic sample | Q1 Endpoint/Service，Payload ABI 显式转换 |
| Service request/response | Q0 Command ID + Result Endpoint |
| Action | 产品状态机 + 多阶段 Result/Feedback |
| DDS QoS | 在 Bridge 中映射可实现子集，不能声称一一等价 |
| DDS discovery | Linux Bridge 负责，不灌入 MCU Core |

ROS 2 消息可能动态/大端/对齐复杂，应在 Bridge 复制并转换为冻结的 MCU Payload，不把 CDR/DDS 类型直接塞进小 MCU。

## 故障和背压

ROS subscriber 慢不能阻塞 UCN Owner；使用有界 queue/Latest 并记录 drop。Linux process 重启时 Session/Service Ready/Route 要重新建立，不能复用旧裸指针或假装远端命令仍 pending。

## 真正发布 Linux/ROS 2 组件前需要

- POSIX Port 与 poll/wakeup/reentrancy 测试；
- termios/SocketCAN/UDP Adapter；
- daemon 生命周期、权限、日志和配置；
- ROS 2 package、消息映射和 QoS 限制；
- 端到端回环/断连/重启/高负载；
- 版本兼容和部署文档。

在这些产物进入仓库并验证前，Host Fake 绿色只能写成“公共 Port 合同有软件证据”。
