# CAN 与 CAN-FD 接入

> 文档级别：`GUIDE`
> 实现状态：`PARTIAL（通用合同已实现，产品 BSP/SDK 需接入）`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：公共 API、Port/Adapter/Source 实现与测试
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：部分 ESP32-S3 UART/ESP-NOW；其他平台按正文标注

为每个 CAN 控制器建立独立 Link/Source。CAN-FD 可承载较长 carrier；Classic CAN 使用 START/CONTINUE 分段并按 ID/sequence/timeout 重组。

产品配置 bitrate、sample point、filter、CAN ID 分配、FIFO、Bus-Off 恢复和收发优先级。Source 只处理 carrier，不替代控制器错误状态机。

测试必须覆盖连续 carrier、完成槽提交、乱序/丢段、padding、Bus-Off、恢复和多总线同时在线。

## CAN ID 规划

UCN frame 的 Source/Destination 不应直接塞满 29-bit CAN ID。CAN ID 更适合表示 Carrier 类型、优先级、发送接口/peer binding；完整网络地址仍在 UCN frame 内。产品必须冻结 Standard/Extended ID、过滤规则、是否允许第三方报文共总线，以及 Q0 到 CAN arbitration priority 的映射。

## 每控制器一个实例

```text
CAN1 Driver ↔ CAN Source 1 ↔ Link 1
CAN2 Driver ↔ CAN Source 2 ↔ Link 2
```

两个控制器不能共享 reassembly slots、bus state 或 stats。`UCN_MAX_LINKS`、Event source 数和 RAM 需按实例总数计算。

## Classic CAN TX/RX

Link send 收到完整 UCN frame 后计算 segment count，依次生成 START/CONTINUE 物理帧并入 Driver TX queue。只有整条 carrier 全部被本地队列接受才能按产品定义报告成功；中途队列满需保持可恢复状态或整条失败，不能悄悄丢尾段。

RX ISR 把 `{CAN ID,DLC,data,timestamp}` 复制进固定 ring；Owner Source 按 identity/sequence/offset 重组。完成槽立即停止继续读取下一 START，先把完整 UCN frame 提交到 Adapter Queue，再继续处理 ring。

## CAN-FD

CAN-FD data phase bitrate 和 arbitration bitrate分别配置。64 B physical payload 可装入当前短 UCN frame/carrier，但 Core 最大 256 B 时仍可能需要上层/Carrier策略。DLC rounding padding 必须为零；BRS/ESI 由 Driver状态管理。

## Link status 与指标

Controller error-active/warning/passive/bus-off 映射为 Source health 和 Link可用性。base cost preset只反映标称速率；实际仲裁 busy、TX failure、RX carrier failure、queue pressure 进入动态 Cost。不要用应用 ACK loss 填 Link failure。

## Bus-Off 恢复

```text
检测 Bus-Off
  → set_bus_state(BUS_OFF)
  → Link down / 触发 Route failover
  → 清或超时 reassembly
  → Driver 按产品策略恢复控制器
  → 重新配置 filter/bit timing
  → set active，重新 HELLO/邻居验证
```

恢复不能保留半条旧 Carrier 与新 session 数据拼接。

## 验收

使用总线分析仪记录 ID/DLC/时序；注入 arbitration load、丢帧、重复、乱序、Bus-Off 和恢复。验证 Q0 在高负载下的尾延迟、Classic Carrier 完整性、CAN-FD padding、两个 CAN 同时工作和 Route/Cost 切换。
