# 实验与未放行能力

当前发布树只有 v6 源码；“实验”不再表示另有一套可编译旧协议。以下边界仍未获得生产证据：

- Realtime 的真实硬件时间戳、asymmetry 和 uncertainty；
- Cluster 的真实 Flash 掉电、分区与多簇长稳；
- ESP32-S3、CAN/CAN-FD、USB、混合 Bearer 的 v6 驱动实测；
- 目标 MCU 的资源、P99/P999、功耗与 24 小时稳定性；
- MSVC 与可运行 TSan 工具链。

模块源码和 Host 测试可以存在，但上述能力在证据完成前仍是产品验证 HOLD。旧 v5 实验组件
列表已归档到 `docs/archive/v5-user-facing-20260905/experimental/`。
