# UCN V5-14/V5-23 长距离 Cost 与 RREQ/RREP 实现报告

> 日期：2026-08-11
> 范围：累计路由代价、RREQ/RREP 线格式、长链 Host 软件验证；不包含真实介质时延或 MCU 性能结论。
> 当前线格式：V5-23 已在 V5-14 的 32 bit 累计语义上，把 Cost Width 更新为 `3/3/3/4 B`；本文件以下表格均按当前源码描述。
> 当前基线：已对照 `codex/v5-adaptive-wire@f941ae9` 的 Descriptor、RREQ/RREP Codec 与正式测试复核。

## 1. 结果

V5-14 已消除 16 bit 累计代价在长链和高代价介质上的饱和歧义：

- 单跳 `ucn_link_metrics_t.route_cost` 仍为 16 bit，由 Adapter 提供；
- Route、Candidate、RREQ/RREP 累计 Cost 统一使用 `ucn_route_cost_t`（`uint32_t`）；
- `UCN_ROUTE_COST_UNKNOWN=0xFFFFFFFF`，最高有效累计值为 `0xFFFFFFFE`；
- 已知 Cost 的真实 32 bit 加法溢出返回错误，不再静默饱和成 Unknown；任一操作数 Unknown 时结果保持 Unknown。

Known 始终优于 Unknown。单跳指标没有被放大为 32 bit 采样接口，因此 MCU Adapter ABI 仍保持紧凑。

## 2. Profile-aware Cost 编码

Cost 在 W0/W1/W2/W3 中分别占 `3/3/3/4 B`，各档全 1 值保留为 Unknown：

| Wire Profile | Cost 字节 | 最大 Known | Unknown |
| --- | ---: | ---: | ---: |
| W0 | 3 | 16,777,214 | 16,777,215 |
| W1 | 3 | 16,777,214 | 16,777,215 |
| W2 | 3 | 16,777,214 | 16,777,215 |
| W3 | 4 | 4,294,967,294 | 4,294,967,295 |

合法单跳最大 Cost 为 65,534；W0/W1/W2/W3 官方最大 Hop 下的最坏累计值分别是 262,136、1,048,544、4,194,176、16,645,636。3 B 最大 Known 已覆盖全部四档官方 Hop 范围，W3 仍保留 4 B Canonical 域。累计值若不能由当前 Profile 表达，编码失败关闭；不会截断，也不会借 Unknown 哨兵冒充有效值。

## 3. RREQ 与 RREP 新载荷

RREQ：

```text
Target(AddressWidth) | RequestID(4) | Cost(CostWidth) | Hop(1) | Flags(1)
```

RREP：

```text
RequestID(4) | Cost(CostWidth) | Hop(1) | Flags(1) | Epoch(EpochWidth)
```

RREP 的 Origin/Target 不再在 Payload 中重复：Target 使用 Header Source，Origin 使用 Header Destination。Request ID 保持固定 4 B，避免缩窄并发发现标识。

| Profile | RREQ Payload | RREP Payload |
| --- | ---: | ---: |
| W0 | 10 B | 10 B |
| W1 | 11 B | 11 B |
| W2 | 12 B | 11 B |
| W3 | 14 B | 12 B |

协议版本与 Wire Profile 唯一决定格式；旧 18 B RREP、V5-23 前的 W0 8 B/W1 10 B RREQ 和其他错误长度均按 `UCN_ERR_MALFORMED` 拒绝，不做启发式兼容。普通业务帧 Header 没有增加 Cost 字段。

## 4. 软件验证

- 四档均覆盖“最大 Known、Unknown、再加 1 溢出”边界，并断言 `max_hops × 65534` 小于各档 Unknown 哨兵。
- W0 一跳 Cost=300 不再因窄 Cost 字段寻路失败；W0 四跳、W1 十六跳的合法最坏累计值可精确转发。
- A→B→C（每段 50,000，总 Cost 100,000）与 A→D→C（每段 40,000，总 Cost 80,000）同时存在时，选择 80,000 路径，证明不再发生 16 bit 饱和后相等。
- 201 Node、200 Edge 的线形拓扑，每 Edge Cost=1,000，累计 Cost=200,000：`UCN_MAX_HOPS=201` 时 20/20 交付；预算 200 时 0/20，并被模拟器正确判为 FAIL。
- 之所以 200 Edge 需要预算 201，是源节点第一次发出的 RREQ 也消费一个 Hop 预算；这是当前 Hop 语义，不是额外物理跳。
- V5-26 的 Full/Lite Debug/Release `CTest 10/10`、Nano `1/1` 和 WSL ASan+UBSan `13/13` 通过，AODV、Candidate、RERR、端到端 Scale Smoke 和业务转发回归未退化。
- Lite 的动态 Mesh 编译门禁已从旧 18 B RREP 对应的 50 B 修正为当前 W3 RREQ 对应的 46 B；46 B 正向构建通过，45 B 按预期编译拒绝。这里保留 32 B W3 Route Header 的通用 Payload 缓冲契约。

规模模拟器还增加了 `line` 拓扑和 `end-to-end` 流量模式。对无丢包、无断链的端到端场景，必须至少产生、接受并交付业务，且 `accepted == delivered`；因此“路由未建立但没有崩溃”不再被误记为 PASS。

## 5. 边界

这项验证证明线格式、累计代价和 200 Edge Host 状态机可工作，不代表 Wi-Fi、CAN、UART 或 LoRa 上能以相同时间和容量运行。真实栈峰值、CPU、收敛时间、碰撞与丢包仍归 S06/S07 实机测试。
