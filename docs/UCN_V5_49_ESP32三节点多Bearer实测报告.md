# UCN V5-49 ESP32 三节点多 Bearer 实测报告

> 日期：2026-08-12
> 性质：真实开发板证据，不是 Host 模拟。
> 边界：证明当前 ESP32 测试 Adapter 与 UCN v5 Core 的首轮三节点闭环；不等于生产安全、长期可靠性、功耗、EMC 或零丢帧认证。

## 1. 台架

| 角色 | 开发板/串口 | Node ID | UART | 无线 |
| --- | --- | --- | --- | --- |
| A | ESP32-S3 / COM5 | `0x10000001` | RX15/TX16 ↔ B TX16/RX15 | ESP-NOW |
| B | ESP32-S3 / COM34 | `0x10000002` | RX15/TX16 ↔ A；RX19/TX20 ↔ C | ESP-NOW |
| C | ESP-WROOM-32 / COM35 | `0x10000003` | 显式 RX16/TX17 ↔ B TX20/RX19 | ESP-NOW |

三板共地。测试准入 Provider 强制 A 只接 B、C 只接 B、B 接 A/C；即使三块无线电互相可见，A/C 也不能绕过 B 直接入网。UART 为 115200 8N1 + COBS；基础 Cost 为 UART 34、ESP-NOW 45。

## 2. 本轮实现与软件门禁

- 外部 PlatformIO 工程按 v5 当前 `src/core|node|transport|routing|service` 分层选择权威源码，新增三目标专用固件和诊断输出。
- Adapter 的 Metrics 时间戳改为使用本轮 `pump(now_ms)` 的同一时间基准，消除多 Link 采样跨毫秒后出现“未来时间戳”，从而被误判为 `METRICS_STALE` 的问题。
- RREQ Origin 在首次 Flood 前写入自身去重缓存，避免同一邻居经另一 Bearer 把 RREQ 回送后让 Origin 学到指向自身的错误 Route。
- 新增 RREQ Origin Loop Guard，主库 Full Profile CTest `14/14` 通过。
- 三目标构建尺寸：A RAM/Flash `46,308/590,971 B`，B `48,228/591,207 B`，C `50,776/625,859 B`；三个镜像均完成 Flash Hash 校验。

## 3. 正常三节点结果

- A/C 各注册 2 Link，B 注册 4 Link；邻居准入为 A/C=`1/1`、B=`2/2`。
- 全部 Link 为 `selectable=1`、`exclusion=0`，没有再次出现 `METRICS_STALE`。
- 动态启动惩罚消退后，B 对 A/C 均选择 UART Primary；A↔C 为 `2 hops/cost 68`。
- 未发现 `destination == local Node ID` 的自路由。
- 直连 ping 多为 7～22 ms，两跳多为 15～30 ms；探测或统计窗口出现 65～145 ms 峰值。
- Heap/Stack 在观测窗口稳定；未见发送硬错误、overflow 或 no-space。B 启动期固定出现少量 COBS 错误和一次 Queue Drop，后续不增长，暂列启动瞬态。

## 4. A—B UART 热断与恢复

热断后：

1. 约 5 s：A 观测 RX failure=1000‰，UART effective Cost 34→114，Primary 切到 Wi-Fi，并产生 1 个 RERR。
2. 约 10 s：A/B 撤销 UART Bearer 准入。
3. 约 25 s：A↔C Route 由 UART+UART/cost 68 改为 Wi-Fi+UART/cost 79。
4. 切换窗口有数个 ping 未返回；因此只能表述为自动容错，不能表述为零丢帧无缝切换。

恢复后：

1. 约 23～28 s：双端重新发现并准入 UART Bearer，启动拥塞 Cost 114/74 逐步回落。
2. 约 38 s：A/B 选回 UART Primary。
3. 约 53 s：A↔C Route 回到 UART+UART/cost 68。

## 5. 中间节点 B 离网与返回

- 短复位：B 约 0.2 s READY、约 5 s 恢复四 Bearer；A 约 2 s 临时切 Wi-Fi、约 12 s 选回 UART。Session 更新，Route Epoch 由 37 更新到 38。
- 长按 EN：B 离线约 19 s；A/C 约 6 s 进入失效状态，约 11 s 清为 `links=0`。由于强制 A—B—C，A/C 正确无路由，发送失败累计 17 次，没有非法直连。
- B 返回后约 2～3 s 重新准入，约 12 s 选回 UART；Route 先恢复为 cost 79，完整刷新到 68 慢于 Bearer 恢复。
- 后续无复位快照确认最终完全收敛：A→C 与 C→A 均为 `2 hops/cost 68/epoch 107`，B 到 A/C 均为 `1 hop/cost 34`。

## 6. 确认的问题与后续门禁

| 优先级 | 问题 | 证据 | 处理任务 |
| --- | --- | --- | --- |
| P1 | Route 刷新慢于 Bearer 切换约 15～20 s，期间有少量 ping 丢失。 | UART 已未准入/Primary 已切 Wi-Fi时，诊断 Route 仍短时指向 UART/cost 68。 | V5-50：无效下一跳立即停止发送；同邻居 Backup 快速接管并触发有界 Route/RERR 更新。 |
| P2 | UART RX 悬空产生大量噪声字节和 COBS 解码错误。 | A—B 两根线拔除后错误持续增长，B 未连接 UART TX 仍周期发送 HELLO。 | V5-50：RX 空闲上拉、未接端口显式关闭、错误预算与实机复测。 |
| P2 | 启动期少量解码错误/一次 Queue Drop。 | B 初始 COBS decode 固定为 1/4、UART0 一次 Drop，随后不增长。 | 重复冷启动与上电顺序矩阵；若可复现再修复启动排空/同步。 |

V5-50 完成前，UCN v5 可以宣称“多介质自动容错和恢复已首轮实测”，不能宣称“完全无缝、零丢帧”。
