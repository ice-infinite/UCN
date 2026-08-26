# UART、RS-485、USB CDC 接入

> 文档级别：`GUIDE`
> 实现状态：`PARTIAL（通用合同已实现，产品 BSP/SDK 需接入）`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：公共 API、Port/Adapter/Source 实现与测试
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：部分 ESP32-S3 UART/ESP-NOW；其他平台按正文标注

三者都可使用 Stream Source：驱动字节 → ISR ring → COBS carrier → 完整 Core frame → Adapter Queue。

配置项包括实例、TX/RX 引脚、baud、DMA/ring 大小、半双工方向控制、最大 frame 和 service 预算。RS-485 在发送完成中断后再释放 DE；USB CDC 断连要报告 Link Down 并清理半帧 carrier。

先验证随机分块、粘包、截断、ring overflow 和重新连接，再测吞吐。UART 理论 baud 不等于 UCN payload throughput。

## 共同软件链

```text
Node Link send(full UCN frame)
  → COBS carrier encode
  → Driver TX queue/DMA
  → 物理字节流
  → RX IRQ/DMA chunk
  → Stream Source ring
  → COBS decode/full frame
  → Adapter Queue/Node receive
```

COBS carrier解决字节流没有包边界的问题。不要把一次 DMA callback 当一帧，也不要假设接收端每次 read 恰好返回发送长度。

## UART 配置步骤

1. 为每个实例选择 UART peripheral、TX/RX、可选 RTS/CTS、baud、8N1；
2. 用 Standard preset 解析初始 base cost/RTT/MTU；
3. 配置 RX DMA circular/ring 和 TX queue；
4. 建立独立 `ucn_link_t`、Stream Source/storage、Runtime source ID；
5. Driver callback 只写 chunk/通知；
6. Link `send()` 执行 COBS encode 后入 TX queue；
7. `get_status()` 报告 up/MTU/errors，`get_metrics()` 报告队列和失败率。

两块板交叉连接为 TX→对端 RX、RX←对端 TX，并共地。引脚号必须来自具体 BSP/board variant，不能从通用 UCN 文档写死。

## RS-485

RS-485 还需处理 DE/RE、总线拓扑和地址冲突：

```text
取 TX carrier
  → 等总线/仲裁策略允许
  → 拉高 DE
  → DMA 发送
  → 等 UART TC（最后停止位真正发完）
  → 拉低 DE
```

只等 DMA completion 可能过早释放 DE，截断尾字节。多点 RS-485 还需要产品 MAC/主从/时隙；UCN 路由不自动解决物理总线同时发送冲突。

## USB CDC

USB CDC 的 `send()` 通常写入 endpoint queue；主机未枚举/DTR 未就绪时 Link down。USB packet 仍是字节块，不代表 UCN frame 边界。拔线时丢弃未完成 carrier、清 Driver状态并生成 Link Down；重连后使用新 session/明确邻居恢复。

## 吞吐计算

UART 8N1 理论原始字节率约 `baud/10`，再扣除 COBS、UCN header/tag、ACK、队列和转发。多跳半双工或单 UART relay 还会因为收完再发降低吞吐。测试报告要区分 raw carrier B/s、UCN frame B/s 和业务 payload B/s。

## 测试矩阵

- 9600～目标最高 baud 的误码/稳定性；
- 1 字节 chunk、随机 chunk、多个 carrier 粘连；
- DMA/ring wrap、overflow、TX queue full；
- UART framing/overrun、RS-485 碰撞、USB suspend/unplug；
- 重连后 route/session/duplicate window；
- 单跳/多跳、各 Transfer Class 和 Q0 并发。
