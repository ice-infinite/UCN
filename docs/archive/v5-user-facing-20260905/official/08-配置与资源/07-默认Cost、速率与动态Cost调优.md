# 默认 Cost、速率与动态 Cost 调优

> 文档级别：`REFERENCE`
> 实现状态：`CURRENT`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：ucn_config.h、ucn_profile.h、CMake 与资源门禁
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：Host 资源可用；目标 MCU 资源需按产品配置实测

## 组成

实际 Link Cost 由静态基础 Cost 和动态增量组成：

```text
effective = base_medium_cost
          + serialization_cost(rate, frame_bytes)
          + latency_penalty
          + loss_retry_penalty
          + queue_utilization_penalty
          + liveness_penalty
          + policy_bias
```

计算使用饱和整数，越界取最大 Cost。不可用、过期或未验证链路不得通过一个很小的静态值进入最优路径。

## Preset 的意义

UART 9600/115200/921600、CAN 500K/1M、CAN-FD、USB、Wi-Fi/ESP-NOW、BLE、LoRa 等 preset 提供可比较的初始量级；它不是永久排名。串口高速且空闲时可能优于拥塞 Wi-Fi，丢包或队列饱和后动态 Cost 会反转。

### 当前典型 Base Cost

| Bearer/Preset | Base Cost | 参考 RTT ms | 说明 |
| --- | ---: | ---: | --- |
| UART 9600 / 115200 / 921600 | 140 / 34 / 12 | 5 | 点对点，全双工能力取决于驱动 |
| UART 1M / 2M / 3M / 4M | 11 / 8 / 7 / 6 | 5 | 实际上限取决于 MCU 时钟、DMA 和接线 |
| RS485 9600 / 115200 / 921600 | 152 / 46 / 24 | 5 | 额外考虑半双工/方向控制 |
| Classic CAN 125K / 250K / 500K / 1M | 110 / 72 / 45 / 30 | 3 | 需要 Carrier 分片 |
| CAN-FD 500K/2M、500K/4M | 22 / 15 | 3 | logical MTU 64 B |
| ESP-NOW 1M / Wi-Fi 1M / 6M / 54M | 45 / 52 / 30 / 14 | 12 | 无线速率不等于稳定应用吞吐 |
| USB CDC FS / HS | 14 / 6 | 4 | 调度、端点和主机栈仍影响延迟 |

完整 preset 以 `ucn_standard_adapter.c` 为事实源。标记为 conditional 的 Ethernet、BLE、802.15.4、私有 2.4G、FSK、LoRa 必须由产品确认驱动语义后才能使用。

Base Cost 是无量纲、可加的路由量，不是微秒或 bit/s。两跳路径通常累加两条 Link Cost，因此低速多跳会自然变贵。

## 更新

质量采样默认 500 ms，快速 probe 可为 100 ms。每次发送使用缓存的当前 Cost，不逐包全网寻路；后台更新候选，Policy 通过滞回、验证和最小驻留减少抖动。

具体公式、权重和 preset 表以 Standard Adapter 与 Cost 合同为准。产品调参必须保留同一流量模型的 A/B 结果，避免只凭理论 bitrate 设置路径优先级。

### LC-1 当前明确惩罚

动态输入按千分比离散分档：队列压力最高加 80，TX failure 最高加 160，RX failure 最高加 80，medium busy/quality 各最高加 70，RTT 超参考值按一半增量且最高 80；指标轻度过期加 20/60，超过 5000 ms 直接排除。行政 bias 合法范围是 -32..64。

计算结果使用饱和加法。Base 未知时保持 Unknown，链路 down、MTU 不足、capability 不允许、指标过期或配置非法时不可选。Wi-Fi RSSI 和 busy 若来自同一统计源，不应重复计入两个 penalty。

### 切换滞回

候选只有满足：

```text
candidate_cost × 100 <= active_cost × 80
```

才算至少改善 20%。随后还需通过连续样本、probe ACK 和最小驻留/hold 等状态机条件，避免一两个瞬时样本导致路径抖动。

### 调优方法

1. 以 preset 作为未标定起点；
2. 在固定包长、方向、跳数和负载下采集 RTT、失败率、queue/busy；
3. 对比选择结果是否符合业务目标，而不只看物理 bitrate；
4. 用拥塞、断链、弱信号和恢复场景验证 Cost 能上升、排除并回落；
5. 只有稳定偏差才调整 base/administrative bias，短期波动交给动态 penalty。
