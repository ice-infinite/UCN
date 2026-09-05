# UART、CAN、Wi-Fi 与多 Bearer 实测矩阵

> 文档级别：`NORMATIVE`
> 实现状态：`CURRENT（测试规范）`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：CMake、tests、tools、results 与审计门禁
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：实机证据按具体报告；规范本身不代表已验收

| 项目 | UART/USB | CAN/CAN-FD | Wi-Fi/ESP-NOW | 多 Bearer |
| --- | --- | --- | --- | --- |
| 基础收发 | baud/分块 | bitrate/filter | channel/peer | 同时在线 |
| 压力 | ring/队列 | FIFO/Bus load | RF/队列 | 公平性 |
| 故障 | 拔线/重连 | Bus-Off | 掉线/重连 | 路径切换 |
| 完整性 | COBS/CRC | carrier/padding | datagram 长度 | MTU 一致 |
| 性能 | payload/s/RTT | payload/s/RTT | payload/s/RTT | 每路负载 |

经典 CAN、真实 CAN-FD、ESP-NOW 四节点、多源并发和各 RTOS SDK 未完成项必须继续标为未测。

## UART/USB

速率至少覆盖低/常用/最高稳定档；每档测试T32～T8K、1/2/3跳、window 1/4/8（若支持）、随机chunk和持续流。注入拔线、交叉线错误、RX ring满、USB重枚举。报告raw carrier和payload goodput。

## CAN/CAN-FD

Classic 125K/250K/500K/1M（按硬件支持）测试完整Carrier，CAN-FD分别记录arbitration/data bitrate和DLC。注入50/80% bus load、丢段/乱序、Bus-Off、filter错误、两总线并发；验证padding与完成槽不覆盖。

## Wi-Fi/ESP-NOW

记录芯片/SDK、channel、peer数、距离/RSSI、Wi-Fi共存和payload MTU。测试单peer、多peer、四节点、遮挡/干扰、断连重连、队列满、channel变化。PHY rate与业务goodput分别记录。

## 多Bearer策略

至少执行：

1. UART+Wi-Fi同时up，静态Cost选择；
2. 制造Wi-Fi拥塞，动态Cost上升；
3. 候选probe后切UART；
4. 正在Transfer时切路；
5. Pinned Strict断路失败；
6. Pinned Failover切备用；
7. Q1多flow分散，Q0保持固定；
8. 恢复链路不立即抖回。

## 多路径并行

建立两对不相干source/destination，验证不同物理控制器可同时收发而非全局串行；若共享Wi-Fi radio/CAN总线，记录共享介质限制。

## 结果表字段

`bearer/profile/rate/MTU/hop/class/window/concurrency/offered load/goodput/p99/loss/retry/queue peak/CPU/power/switch count`。缺少任一关键条件的速度数字不可比较。

## 当前边界

已有ESP32-S3 UART/ESP-NOW部分证据只能引用对应报告。未完成项保持“未测”，本矩阵是验收规范，不是完成状态声明。
