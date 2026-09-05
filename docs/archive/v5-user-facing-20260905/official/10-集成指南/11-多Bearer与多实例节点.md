# 多 Bearer 与多实例节点

> 文档级别：`GUIDE`
> 实现状态：`PARTIAL（通用合同已实现，产品 BSP/SDK 需接入）`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：公共 API、Port/Adapter/Source 实现与测试
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：部分 ESP32-S3 UART/ESP-NOW；其他平台按正文标注

一个 Node 可注册多个 Link；每个 UART、CAN 控制器、USB、Wi-Fi 接口都可形成独立路径。Link ID 和 context 区分实例，不限制“每类只能一个”。

为每条 Link 独立配置 MTU、基础 Cost、队列和 liveness。Neighbor 可同时记录多个 Bearer，Policy 选择自动、Pinned、Failover 或 Q1 balance。

多实例带来并行性，也增加 Owner drain、公平性和 RAM 压力。应限制每轮每 Source 预算，避免高速接口饿死低速控制链路。

## 对象规划示例

一个节点含 UART1、UART2、CAN1、CAN2、Wi-Fi、USB 时至少需要：

- 6 个 `ucn_link_t` 与独立 link ID/context；
- 2 个 Stream Source（UART）、2 个 CAN Source、Wi-Fi packet Source、USB Stream Source；
- 6 份 Driver RX/TX ring/queue 与 health；
- Event Runtime 至少 6 个 source slot；
- `UCN_MAX_LINKS >= 6`，Neighbor/Bearers-per-neighbor按实际拓扑配置。

类型相同不合并实例；两路 UART可能连接不同 peer、baud/MTU/Cost和故障域。

## 同一 Neighbor 多 Bearer

Node A 与 B 同时经 CAN 和 Wi-Fi直连时，Neighbor identity是一份，Bearer状态是两份。Heartbeat/HELLO在各 Link上维护 freshness；Route/Policy可选择：

- Q0 舵机严格固定 CAN；
- Q1 IMU 自动选择低延迟链路；
- Q1视频摘要在两条已验证路径间按 flow balance；
- Q3日志优先 Wi-Fi，断开后不占 CAN。

Path安装必须写明每跳 next hop和能力瓶颈，不能只固定“介质类型”。

## 并行与共享介质

不同 UART/CAN控制器可真正并行；同一 Wi-Fi radio上的多个 Link/peer仍共享 airtime。Cost的 medium busy应按共享源处理，Policy不能把共享radio误当两条完全独立带宽。

中继节点若只有一个半双工介质，接收和转发会串行；多 Bearer交叉转发可能提高吞吐，但也要考虑 Owner和内存拷贝。

## 公平性

Runtime按 Source work budget轮转；Core TX按 Q0/Q1与维护burst；Policy flow保持同一Q1流路径稳定。不得让高速Wi-Fi每轮填满RX queue，使CAN Q0 Heartbeat/命令饿死。

## 故障测试

逐条拔掉每个 Bearer，验证只失效依赖路径；恢复时先HELLO/probe，不立即把瞬时低Cost链路切为主路径；两条同时断开时自动寻路/RERR；Pinned Strict与Failover行为分别验证。记录切换期间丢包、乱序和重复。
