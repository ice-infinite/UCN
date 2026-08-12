# UCN V5-50 快速故障收敛与 UART 空闲硬化报告

> 状态：Core、ESP32 Adapter、软件回归和三板 10 轮热断/恢复已完成。Route/Primary 快速同步通过，但零丢帧未通过；物理静默检测窗转 V5-51，不能表述为无缝切换。

## 1. 问题来源

V5-49 三板首轮实测发现两项问题：

1. A—B UART 失效后，Neighbor Primary 约 5 s 内可切到 ESP-NOW，但 Route 的物理 `egress_link` 和基础 Cost 仍短时保留 UART，Route 刷新落后约 15～20 s，并出现少量 ping 丢失。
2. 启用但悬空的 UART RX 会吸收噪声，形成大量假字节和 COBS 解码错误。

## 2. Core 修复

### 2.1 逻辑下一跳与物理 Bearer 同步

Route、Previous Route 和 Candidate Route 仍表示同一个逻辑下一跳 Neighbor；当该 Neighbor 的 Primary Bearer 改变时，Core 立即执行：

- 将上述表项的 `egress_link` 改为新 Primary，不再让诊断和 Route Epoch 发送条件引用已失效 Bearer；
- 按 `新总代价 = 旧总代价 - 旧本地 Bearer 基础 Cost + 新本地 Bearer 基础 Cost` 更新本节点可见的累计 Cost；
- 清除旧端到端 RTT 验证值，因为本地物理承载已改变；
- 保留 Route Epoch 和逻辑节点序列，不把同 Neighbor 的物理切换误作整条多跳路径替换。

基础 Cost 按契约是稳定的产品配置值；动态 RTT、失败率和队列压力仍只参与本节点选择评分，不写入线上累计 Cost。

### 2.2 发送阶段硬失败的一次性接管

Driver 可能在状态查询后、真正发送时才返回 `UCN_ERR_LINK_DOWN`。Core 现在会：

1. 将该物理 Bearer 标记为 Down；
2. 重新解析同 Neighbor 的已准入 Primary；
3. 仅当找到不同的 Backup 时，对同一帧重试一次；
4. 没有 Backup 或 Backup 也失败时停止，不循环重试；中继按既有 RERR 回送，源端 Q1 仍按既有有界发现规则处理。

第一次发送已明确返回 `LINK_DOWN`，因此这次单次重试不会造成“两次都成功”的重复交付。普通 Route 和转发路径使用该规则；指定 Path 继续使用其既有能力交集、撤销和 Path-RERR 规则。Q0 不新增寻路逃生通道，Pinned Strict/Failover 和自动负载均衡语义不变。

## 3. ESP32 UART Adapter 修复

- `begin(..., enabled, rx_idle_pullup)` 新增显式启用参数；关闭的 UART 不启动 Driver、不分配串口 RX/TX Buffer、不 Pump、不发送 HELLO。
- 启用且提供显式 RX 引脚时，先配置 `INPUT_PULLUP`，再调用 `HardwareSerial::begin()` 绑定 UART 外设。Arduino-ESP32 3.x 的 `pinMode()` 会清除引脚已有的 Peripheral Manager 总线，因此禁止在 `begin()` 之后调用，否则会把 RX 从 UART 矩阵静默解除。
- `UCN_TEST_UART0_ENABLED`、`UCN_TEST_UART1_ENABLED` 允许产品/台架逐口关闭。
- `UCN_TEST_UART_LINK_RX_ERROR_BUDGET_PER_PUMP` 默认 4：单次 Pump 达到噪声错误预算后只做有界排空和重同步，避免噪声长期占用协议 Owner 时间片。
- 诊断新增 `rx_noise_budget_exhausted`，不会把错误静默隐藏。

## 4. 软件验证

专项测试覆盖：

- Link 状态选择后 Driver 才返回 `LINK_DOWN`；同一次业务调用通过 Backup 成功；
- Active/Candidate/Static Route 出口和基础 Cost 同步；Route Epoch 在 Backup 帧上保留；
- 下游故障使用 RERR，而不把远端中继失败冒充成本地 Bearer 失败；
- 既有 Candidate Probe、Path、Policy、Q0/Q1 和全量规模测试不退化。

| 配置 | 结果 |
| --- | --- |
| Windows Full | 14/14 |
| Windows Lite | 11/11 |
| Windows Nano | 1/1 |
| Full + Service OFF | 11/11 |
| Full + 128 B/3-Link 产品配置 | 12/12 |
| WSL ASan/UBSan/Leak Detection | 14/14 |
| PlatformIO A/B/C 三目标构建 | 全部成功 |

最终构建尺寸：A S3 `46652 B RAM / 594627 B Flash`；B S3 `48580 / 594975 B`；C WROOM `50784 / 628899 B`。这些是完整测试固件尺寸，不是纯 UCN Core 增量。

### 4.1 三板同源基线

三块板已写入最终固件并通过 Flash Hash 校验。首轮监听曾观察到所有 UART `tx_frames` 增长而 `rx_bytes=0`；对照 Arduino-ESP32 3.3.7 源码确认，原因是 Adapter 在 `HardwareSerial::begin()` 后调用 `pinMode(INPUT_PULLUP)`，触发 Peripheral Manager 解除 UART RX 归属。调整调用顺序并重新烧录后：

- A、C 各发现同一 Neighbor 的 `UART + ESP-NOW` 两个 Bearer，B 发现四条物理 Link；
- A—B 与 B—C 的 UART `rx_bytes/rx_frames` 持续增长，`decode/length/overflow/noise_budget/no_space/partial` 均为 0；
- 两段 Primary 均为 UART，单跳 Cost 34，A↔C 两跳 Route Cost 68；
- 三节点 Ping 持续互通，基线窗口没有发送拒绝；Heap、最小 Heap 与任务栈余量稳定。

该结果证明最终固件的正常双 Bearer 基线成立，但不替代下面的 10 轮故障门禁。

## 5. 十轮热断/恢复结果

第 1 轮先观察 90 s 稳定断开，再恢复；其余 9 轮由 Host 以 A→C Route Cost 自动计数，完整捕获 9 次 `68→79→68`。日志周期为 5 s，因此下表“断开观测窗”包含人工保持时间和采样量化，**不是**物理动作到切换的精确延时。

| 轮次 | Route 回退/恢复 | 断开观测窗 | A→C Ping 序号缺口增量 | 重复/乱序 | A—B UART 错误增量 |
| --- | --- | ---: | ---: | ---: | ---: |
| 1 | 79→68 | 断开稳定 90 s；恢复在一个 5 s 快照内 | 首轮未启用逐序号记录；累计差约增加 1 | 未见 | 0 |
| 2 | 68→79→68 | 15.000 s | 4 | 0/0 | 0 |
| 3 | 68→79→68 | 5.000 s | 2 | 0/0 | 0 |
| 4 | 68→79→68 | 10.000 s | 3 | 0/0 | 0 |
| 5 | 68→79→68 | 10.000 s | 3 | 0/0 | 0 |
| 6 | 68→79→68 | 10.000 s | 3 | 0/0 | 0 |
| 7 | 68→79→68 | 25.000 s | 4 | 0/0 | 1 |
| 8 | 68→79→68 | 14.984 s | 3 | 0/0 | 1 |
| 9 | 68→79→68 | 10.000 s | 2 | 0/0 | 0 |
| 10 | 68→79→68 | 10.000 s | 2 | 0/0 | 0 |

结论：

- 10/10 轮均能自动回退并恢复；9 个自动计数轮次中，Primary 和 A→C Route 出口/Cost 在同一个统计快照内同步，旧版 15～20 s Route 滞后未复现。
- 全部自动轮次 `ping_reject=0`，A→C 共记录 26 个序号缺口，平均 2.9 个/轮；重复 0、乱序 0。首轮未保存逐序号集合，不把它并入精确总数。
- 反复插拔只让 A 端新增 2 次 COBS decode error；B/C 及所有 length/overflow/noise-budget/no-space/partial 均为 0。断开稳定期间悬空 RX 字节不再增长，证明上拉和错误预算有效。
- 最终稳定快照恢复为 A/C 各 2 Link、B 4 Link；两段 UART Primary、单跳 Cost 34，A↔C 两跳 Cost 68；三节点继续通信，Heap/Stack 无趋势性恶化。

## 6. 为什么仍会丢帧

普通 UART 没有“线已拔掉”的硬件载波信号。插头断开后，本机 `HardwareSerial::write()` 仍会把字节成功交给 TX FIFO；在 `UCN_TEST_UART_LINK_HEALTH_TIMEOUT_MS=2500` 到期前，Core 看不到 `LINK_DOWN`，因此不能触发同调用 Backup 重试。上述每轮约 2～4 个 A→C Ping 缺口与这一检测窗一致。

这不是 Route 同步再次失效，而是更前面的“物理静默何时判 Down”问题。不能简单把超时无限缩短，否则高负载、调度延迟或低频业务会制造误切换。V5-51 应在固定资源约束下比较：独立链路探测/ACK、硬件 DCD/CTS/GPIO 断线检测，以及只对配置的关键流在 Suspect 窗做双发并由端点去重。验收必须同时记录假阳性、控制开销、RAM/CPU、最大连续缺口和重复交付。
