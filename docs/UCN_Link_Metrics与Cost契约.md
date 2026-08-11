# UCN Link Metrics 与 Cost 契约

> 对应稳定化任务：S03/V5-14。本文冻结 Adapter 的**单跳输入语义**和 Core 的累计 Cost 语义；不声称当前 `AUTO_BALANCE` 已经使用四项加权评分。

## 1. 结论

`ucn_link_metrics_t` 是 Adapter 向 Core 报告的**单跳 Link 指标**。Core 不读取 RSSI、SNR、CAN TEC/REC、串口错误码或 Wi-Fi 重试次数；Adapter 必须先把介质私有信息转换为下表所列的通用指标。

当前代码的责任边界是：

| 指标 | 生产者 | 单位/范围 | 当前 Core 用途 |
| --- | --- | --- | --- |
| `route_cost` | Adapter | `1..UINT16_MAX-1` 的 UCN Cost，越小越好；`0`、`UINT16_MAX` 或 `route_cost_valid=false` 为未知 | AODV-Lite 的逐跳可加基础代价与直连 Link 选择。 |
| `rtt_ms` | Adapter | 单跳往返 ms；`0..65535`，单独 `rtt_valid` | 以 500 ms 默认周期采样并 EWMA，仅供诊断和未来显式策略。 |
| `tx_failure_per_mille` | Adapter | 本 Link 发送失败比，`0..1000`；超范围视为未知 | 采样/EWMA/诊断；当前不参与自动重绑。 |
| `queue_pressure_per_mille` | Adapter | **Adapter 自己**的发送队列占用比，`0..1000`；超范围视为未知 | 采样/EWMA；当前 `AUTO_BALANCE` 仅用它的连续拥塞样本决定 Q1 Flow 是否重绑。 |

指标接口中的“未知”由对应 `*_valid=false` 表示。单跳 Link 输入仍以 `UINT16_MAX` 为 Unknown 哨兵，正常有效单跳 Cost 不得使用这个值；Route/Candidate/RREQ/RREP 的**累计值**已在 V5-14 升级为 `uint32_t`，其中 `0xFFFFFFFF` 为 Unknown、`0xFFFFFFFE` 为最高 Known。线上累计 Cost 按 W0/W1/W2/W3 使用 1/2/3/4 B，并在各宽度保留全 1 为 Unknown；不可表达或真实加法溢出时失败关闭，不能截断。Known Cost 永远优先于 Unknown，即使 Known=2000 也不会被 Unknown 错误压过；只有全部候选都 Unknown 时，才按活动 Flow 数和稳定顺序回退。调用方应在调用旧 Adapter 前清零整个指标结构。Core 不把 `0` RTT、`0‰` 失败率或 `0‰` 队列压力误认为未知。

## 2. 防止重复惩罚的规则

`route_cost` 是**基础、可加**代价：可表示产品给不同介质的稳定偏好、每跳基本开销或经长期标定的等级，但不得再次揉入当前导出的 RTT、失败率、队列压力三个瞬时样本。

因此未来若新增多指标选路，只允许二选一：

```text
模式 A：score = base_cost + normalize(RTT) + normalize(failure) + normalize(queue)
模式 B：Adapter 提供 composite_cost，Policy 不再读取被它已经包含的动态项
```

v4 当前处于两者之前：没有把四项直接相加。`AUTO_BALANCE` 只用 Known 基础 Cost 与活动 Flow 数评分，Unknown 单独排序；质量快照保留给诊断，队列压力只作 Q1 Flow 的“持续拥塞”触发条件。任何后续加权实现都必须先选择模式 A 或 B、写明权重/归一化和测试向量，不能隐式混用。

## 3. Adapter 的实现要求

1. 一个 `ucn_link_t` 只报告它直连对端的一跳指标，不报告整条 UCN Route 或端到端业务成功率。
2. RTT、失败率与队列压力必须来自 Adapter 自己声明的滑动窗口；建议窗口不短于一次驱动突发，且在 Adapter 文档中写明窗口时长。
3. `queue_pressure_per_mille` 不得引用 Core 的 Q0/Q1 或 Service Router 队列。它只表示该 Adapter 继续提交到物理驱动的阻塞程度。
4. 无法可靠采样时返回 `valid=false`，不能伪造“良好”的零值；Provider 返回错误时 Core 也会将快照清为未知。
5. `UCN_LINK_METRIC_PER_MILLE_MAX` 是唯一的千分比上限。Core 对大于该值的失败率/队列压力拒绝采样，避免单位误填为百分比或原始计数。

## 4. 采样、EWMA 与调度

`ucn_node_step()` 默认每 500 ms 调用 `get_status()` 和 `get_metrics()`，并以 25% EWMA 缓存可选指标。调度周期比 500 ms 快并不等于 Adapter 必须每次重算质量；Adapter 可以返回最近的固定快照。

质量采样不直接占用业务帧。需要发送的 Heartbeat、Bearer Quality Probe、Path Probe、Route Refresh 则属于必要维护：连续 `UCN_BUSINESS_TX_BURST_BEFORE_MAINTENANCE`（默认 4）个业务发送后，若它们已经到期，可使用一个槽位。Snapshot 和 Policy Diagnostic 仍是后台诊断，不参与该例外。

## 5. 当前验证与后续实机标定

软件测试已覆盖：Known 2000 优于 Unknown、四档累计 Cost 的最高 Known/Unknown/溢出边界、80,000 对 100,000 的双路径选择、200,000 的 200 Edge 长链、极端 RTT/失败率不被裸加进 `AUTO_BALANCE`、可选指标 EWMA、采样间隔、超过 `1000‰` 的非法值拒绝，以及 Link Down 导致 Policy Path Down。Route Refresh 也覆盖了持续 Q1 背景下取得维护槽。

尚未用真实 Wi-Fi/UART/CAN/LoRa 标定以下数据：不同介质窗口长度、`route_cost` 基础等级、真实 RTT/丢包/队列的相关性、拥塞阈值和 Flow 重绑收益。这些属于 S03/S06 的实机阶段；在日志和复现实验之前，不能把任何推荐 Cost 数值当作通用默认值。
