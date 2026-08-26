# UCN Link Cost 计算规范

> 文档编号：DOC-039 / 规范版本：LC-1。
> 状态：**LC-1 设计与 Host 软件实现已闭环**。V5-44 完成 Resolver/Metrics，V5-36 完成 Full 选路接入；真实 Adapter 标定仍待 V5-38～V5-42。
> 适用范围：标准 Port / Adapter、同一 Neighbor 的多 Bearer 选择、`AUTO_BEST` 本节点候选排序、Q1 `AUTO_BALANCE` 的后续动态评分。
> 权威性：本文件冻结动态 Cost 的计算口径；[标准 Port、Adapter 与默认 Cost 基线方案](../06-平台与适配/UCN_标准Port_Adapter封装与默认Cost基线方案.md)中的旧版第 5 节只保留架构说明，以本规范为准。

## 1. 目的和生效边界

本规范解决“链路质量变差时 Cost 到底加多少、质量恢复时何时减回、缺少数据怎么办”的歧义。所有数值、边界、取整、时间窗口和切换条件均明确，产品不应自行猜测。

它**不改变 v5 已实现的线上语义**：

- `ucn_link_metrics_t.route_cost` 仍是稳定、可加的基础 Cost；它用于 RREQ/RREP、Route 和 Path 的线上累计，动态分数不覆盖该字段。
- Full Profile 每 `500 ms` 采样 RTT、TX/RX 失败率、Adapter TX Queue、介质占用与质量，以 `25%` 定点 EWMA 生成统一 `effective_select_cost`；Lite/Nano 保持基础 Cost，不含常驻动态质量表。
- 本规范中的动态分数只在本节点内存中使用，**不得**写回 `route_cost`、RREQ、RREP、RERR、PATH_INSTALL 或业务帧。
- Full 使用动态分数选择同 Neighbor Bearer、比较 Candidate 的本地出口贡献，以及为新建/到期 Q1 Flow 评分；Q0、Pinned 和未到期 Flow 的既有语义不变。

以下术语采用规范性含义：**必须**表示不满足即为实现错误；**应当**表示默认要求；**可以**表示可选能力。

## 2. 两类 Cost 与数值范围

| 名称 | 类型/范围 | 谁提供 | 用途 | 能否上线 |
| --- | --- | --- | --- | --- |
| `base_cost` / `route_cost` | `uint16_t`，Known 为 `1..65534` | Preset 或产品静态覆盖 | 稳定、可加的路径基础偏好 | 可以，累计时遵循现有宽度/Unknown 哨兵规则。 |
| `effective_select_cost` | 本地计算的 `uint16_t`，`1..65534` | Core Cost Resolver | 本节点的 Link、Candidate、Q1 Flow 选择 | 不可以。 |
| 不可选用 | 布尔状态，不用数值伪装 | 状态门/安全门/MTU 门 | 自动选择时排除该 Link | 不可以。 |
| Unknown 基础 Cost | 现有 `UINT16_MAX` 语义 | Adapter | 基础偏好未知 | 不得当作普通大数参与计算。 |

`0` 不是合法 Known Cost；`65535` 仍保留给现有 Unknown 语义。因此“不可选用”必须以 `selectable=false` 表示，**不得**把 `65535` 当作一个极大 Cost 后继续比较。

动态项只会在基础值上增加，唯一允许的减法是产品明确配置的 `administrative_bias`。因此链路质量恢复时是撤销之前的动态惩罚，而不是因一次“看起来很好”的样本低于基础 Cost。

```text
effective_select_cost = clamp_1_65534(
    base_cost
  + administrative_bias
  + queue_penalty
  + tx_failure_penalty
  + rx_failure_penalty
  + rtt_penalty
  + medium_busy_penalty
  + medium_quality_penalty
  + freshness_penalty)
```

其中每一项均为整数；加法使用至少 `uint32_t` 中间值，再夹紧到 `1..65534`。`administrative_bias` 为有符号整数，其他惩罚均为非负整数。

## 3. 先过状态门，再计算分数

Cost 不是“坏链路的替代状态”。每次 `500 ms` 评分周期，Core 必须按下列顺序处理某条 Link：

1. `get_status()` 不存在、返回错误或 `is_up=false`：`selectable=false`。按既有 Neighbor/Bearer Down、RERR、恢复逻辑处理，不等待动态 Cost。
2. 动态有效 MTU 小于本次完整 UCN 帧的实际长度：`selectable=false`。不能因 Cost 低而发送会被截断的帧。
3. Wire Class、接收上限、安全模式或产品 ACL 不满足本次发送要求：`selectable=false`。
4. `base_cost` 为 Unknown：不得与 Known Cost 混合评分。只有没有任一可选 Known 基础 Cost 时，才可按第 9.5 节的 Unknown 回退规则保留/选用。
5. 最近一次完整 `get_metrics()` 快照年龄超过 `5000 ms`：在自动动态选择中 `selectable=false`；`PINNED_STRICT` 可继续按其既有强制路线规则尝试发送，但诊断必须显示“metrics stale”。
6. 其余 Link 进入第 4～8 节计算。

任何状态门失败都不增加一个“大惩罚”来模拟；它直接排除候选。这样硬 Down、MTU 不足和安全拒绝不会被另一条更差但数值上“更大”的链路误掩盖。

## 4. Adapter 采样口径

### 4.1 固定时间基准

标准 Adapter 必须使用单调时钟，并保存 `metrics_timestamp_ms`。若平台只能在协议任务轮询，时间戳也必须来自该任务使用的同一单调时钟。

| 参数 | 固定值 | 说明 |
| --- | ---: | --- |
| Adapter 统计窗口 `W` | `1000 ms` | TX/RX 计数按最近 1 秒滚动窗口计算；实现可用两个固定桶轮换，但结果必须等价于最近 1000 ms。 |
| Core 采样周期 `Tsample` | `500 ms` | 与现有 `ucn_node_step()` / Policy 质量采样节奏一致。 |
| EWMA 系数 `α` | `25%` | 每个有效指标独立平滑。 |
| 新鲜 | `age ≤ 1500 ms` | 无新鲜度惩罚。 |
| 轻度陈旧 | `1500 < age ≤ 3000 ms` | 加 `+20`。 |
| 重度陈旧 | `3000 < age ≤ 5000 ms` | 加 `+60`。 |
| 失效 | `age > 5000 ms` | 自动动态选择直接排除。 |

Core 每 `500 ms` 读取一次 Adapter 的最近快照；Adapter 不得因为 Core 调用而在 ISR、`get_metrics()` 或 `send()` 中执行探测、动态分配、等待硬件发送或访问 Core 队列。

### 4.2 TX Queue 压力 `queue_pressure_per_mille`

该值表示**Adapter 自己到物理驱动之前的固定 TX Queue**占用，不包含 Core 的 Q0/Q1、Service Router、应用任务或未交给该 Adapter 的数据。

```text
queue_pressure_per_mille = ceil(1000 × queued_frames / queue_capacity_frames)
```

- 无缓冲的同步 Adapter 必须报告有效值 `0`，不能报告 Unknown。
- 计算发生在统计快照时；`queued_frames` 取该时刻队列中的已入队、未被驱动确认消费的帧数。
- `queue_capacity_frames` 必须大于 `0`，且是编译期/初始化时固定值。
- 结果夹紧为 `0..1000`。若驱动内部还有不可见队列，Adapter 必须把它们计入容量/占用，或将该指标设为无效，不能只报告局部空闲队列。

### 4.3 TX 失败率 `tx_failure_per_mille`

在最近窗口 `W` 内：

```text
attempted_tx = accepted_by_driver + failed_before_or_after_accept
tx_failure_per_mille = floor(1000 × failed_before_or_after_accept / attempted_tx)
```

- `accepted_by_driver` 是驱动已接受发送请求的次数；最终硬件 ACK、超时或 MAC 重传失败只要 Adapter 能观察到，都计入 `failed_before_or_after_accept`。
- `UCN_ERR_NO_SPACE` 是背压而不是物理失败，不计入本指标；它已由 Queue 压力反映。
- `attempted_tx=0` 时必须报告 `tx_failure_rate_valid=false`，不得伪造 `0‰`。
- 计数器必须是饱和或足够宽的无符号计数；窗口滚动时清零/换桶，不得溢出回绕造成低失败率。

### 4.4 RX 失败率 `rx_failure_per_mille`（V5-44 新增）

公共 `ucn_link_metrics_t` 已由 V5-44 追加该可选字段，按以下口径实现：

```text
rx_observed = valid_complete_ucn_frames + corrupted_or_dropped_by_link_frames
rx_failure_per_mille = floor(1000 × corrupted_or_dropped_by_link_frames / rx_observed)
```

- 仅统计 Adapter 已识别到的物理层/Carrier/定界/CRC/重组失败；应用拒绝、ACL 拒绝、Endpoint 不存在和 Core 重复抑制不属于 RX 失败。
- `rx_observed=0` 时为 Invalid。
- 若硬件不能提供可靠 RX 错误计数，必须一直 Invalid；不得用 RSSI、重传次数或 TX 失败率冒充。

### 4.5 RTT `rtt_ms`

RTT 只能来自同一 Link 的 UCN Probe/ACK 或等价的、可证明往返同一 Bearer 的测量：

```text
rtt_ms = clamp_0_65535(receive_monotonic_ms - probe_send_monotonic_ms)
```

- 单向本地耗时、Wi-Fi 回调耗时、应用层回复和跨 Bearer 回复不得填入 RTT。
- 未有成功匹配 Probe/ACK 时 `rtt_valid=false`。
- 仅最后一次成功测量的 RTT 进入 EWMA；RTT 样本的时间戳仍以完整 `get_metrics()` 快照时间为准。

### 4.6 介质占用 `medium_busy_per_mille`（V5-44 新增）

该值表示本 Link 所在物理介质在最近窗口内实际不可服务的时间比例，`0..1000`，越大越忙。它补充 Adapter TX Queue：前者反映“介质本身是否拥塞”，后者反映“本节点是否已积压”。

```text
medium_busy_per_mille = floor(1000 × measured_busy_time_us / W_us)
```

- CAN/CAN-FD 必须在控制器或精确定时器能给出 Bus Load 时填入该值；没有可靠 Bus Load 时 Invalid。
- Wi-Fi、ESP-NOW、以太网、BLE、802.15.4、私有无线仅在驱动能提供真实 channel/airtime/MAC busy 计数时填入；不能由 RSSI、单帧重试数、TX Queue 或 CPU 忙碌时间猜测。
- LoRa/FSK 只有在共享空口调度器能统计实际占用空口时间时才有效；产品给业务预留的配额不是“已测得 busy time”。
- UART、RS-485、USB、SPI、I²C、无法取得真实载波占用的 Tunnel 一律 Invalid；RS-485 不能把本节点方向控制等待重复算成 Queue。
- 同一个原始量只能进入一个动态项。具体地，CAN Bus Load 进入本项时，`medium_quality` 必须 Invalid，避免既以“忙”又以“质量差”重复加分。

### 4.7 介质质量 `medium_quality_per_mille`（V5-44 新增）

此指标统一为“越大越好”的 `0..1000`，只允许表示 Adapter 能可靠取得且经本节映射的**本地单跳物理质量**。它不是 Cost、不是吞吐率，也不得直接写入网络帧。

| Bearer | 采样源 | 映射为 `medium_quality_per_mille` |
| --- | --- | --- |
| UART、RS-485、USB、以太网、SPI、I²C、IP Tunnel | 无统一可比物理质量 | 必须 Invalid。可靠性由 Queue、失败率、RTT 和状态门反映。 |
| Wi-Fi、ESP-NOW、BLE LE、私有 2.4G（能拿到 RSSI） | 最近窗口内接收 RSSI 的中位数，单位 dBm | `≥-55→1000`；`-65..-56→850`；`-72..-66→700`；`-78..-73→500`；`-84..-79→300`；`≤-85→100`。 |
| IEEE 802.15.4（有 LQI） | 最近窗口 LQI 平均值 `0..255` | `floor(1000 × LQI / 255)`。只有 RSSI 而无可靠 LQI 时按 RSSI 表。 |
| LoRa P2P/FSK（有 SNR） | 最近窗口 SNR 的中位数，单位 dB | `≥10→1000`；`5..9→850`；`0..4→700`；`-5..-1→500`；`-10..-6→300`；`≤-11→100`。 |
| CAN/CAN-FD | 无与 Bus Load 独立、可比的物理质量输入 | 必须 Invalid；Bus Load 仅按第 4.6 节进入 `medium_busy`。 |
| UWB、硬件私有无线 | 未完成目标板标定 | 必须 Invalid；产品完成单独标定和测试向量前不得自定义隐式映射。 |

无线 RSSI/SNR 只能用已通过 CRC/硬件校验的接收帧更新；没有接收样本时该指标 Invalid。对于只有“最后一次 RSSI”且无窗口能力的驱动，也必须 Invalid，避免单帧偶然值触发切换。

## 5. EWMA 与失效规则

每个有效指标独立维护一个 `uint16_t` EWMA 槽。V5-44 必须使用下列整数量化，不能按平台随意改为浮点或不同舍入：

```text
首次有效样本：E0 = sample
后续有效样本：En = floor((3 × E(n-1) + sample) / 4)
```

- `queue_pressure`、`tx_failure`、`rx_failure`、`rtt`、`medium_busy` 与 `medium_quality` 各自独立计算。
- `*_valid=false` 时，该指标本轮不产生惩罚；此前 EWMA 不得用于后续评分。下一次有效样本按“首次有效样本”重新开始。
- 完整 `get_metrics()` 快照的时间戳每次成功返回时更新；因此“某一项 Invalid”和“整个快照陈旧”是两个不同概念。
- 值超出法定范围（千分比 `>1000`）必须视为该项 Invalid，并递增 Adapter/诊断的 bad-metric 计数；不得夹紧后静默使用。

恢复示例：Queue EWMA 原为 `900‰`，随后连续每 `500 ms` 得到 `0‰`，数列为 `675、506、379、284、213`。按第 6 节表，Queue 惩罚依次为 `+25、+25、+10、+10、+0`；即惩罚在 2.5 秒内逐级撤销，而不是一次好样本立刻清零。

## 6. 固定惩罚表

区间两端均包含；表外值在第 5 节已被判 Invalid。某项 Invalid 的惩罚固定为 `0`，并由诊断显示 Invalid，不能被解释为“质量很好”。

### 6.1 Queue 压力惩罚

| EWMA `queue_pressure_per_mille` | 惩罚 |
| ---: | ---: |
| `0..249` | `+0` |
| `250..499` | `+10` |
| `500..699` | `+25` |
| `700..849` | `+50` |
| `850..1000` | `+80` |

### 6.2 TX 失败率惩罚

| EWMA `tx_failure_per_mille` | 惩罚 |
| ---: | ---: |
| `0..4` | `+0` |
| `5..19` | `+8` |
| `20..49` | `+20` |
| `50..99` | `+40` |
| `100..199` | `+80` |
| `200..1000` | `+160` |

### 6.3 RX 失败率惩罚

| EWMA `rx_failure_per_mille` | 惩罚 |
| ---: | ---: |
| `0..4` | `+0` |
| `5..19` | `+4` |
| `20..49` | `+10` |
| `50..99` | `+20` |
| `100..199` | `+40` |
| `200..1000` | `+80` |

RX 惩罚小于 TX 惩罚，因为本节点能直接通过本地发送失败判断“此 Bearer 不可服务”的确定性更强；RX 失败仍会影响反向可达性和 Probe/ACK 成功率，故不忽略。

### 6.4 RTT 惩罚

| Preset 类别 | `rtt_reference_ms` |
| --- | ---: |
| UART、RS-485 | `5` |
| 经典 CAN、CAN-FD | `3` |
| ESP-NOW、Wi-Fi、BLE LE、802.15.4、NRF24/私有 2.4G | `12` |
| USB CDC、以太网、SPI、I²C、IP Tunnel | `4` |
| LoRa P2P/FSK | `80` |
| UWB | `10` |

```text
excess_ms  = max(0, rtt_ewma_ms - rtt_reference_ms)
rtt_penalty = min(80, ceil(excess_ms / 2))
            = min(80, (excess_ms + 1) / 2)   // 整数除法
```

例如参考值 `5 ms`、EWMA RTT `13 ms`，`excess=8`，惩罚为 `+4`；RTT 为 `6 ms` 时为 `+1`；不超过参考值时为 `+0`。

### 6.5 介质占用惩罚

| EWMA `medium_busy_per_mille` | 惩罚 |
| ---: | ---: |
| `0..249` | `+0` |
| `250..499` | `+5` |
| `500..699` | `+15` |
| `700..849` | `+35` |
| `850..1000` | `+70` |

### 6.6 介质质量惩罚

| EWMA `medium_quality_per_mille` | 惩罚 |
| ---: | ---: |
| `850..1000` | `+0` |
| `700..849` | `+5` |
| `500..699` | `+15` |
| `300..499` | `+35` |
| `0..299` | `+70` |

### 6.7 新鲜度惩罚

使用第 4.1 节的完整快照年龄。`age` 为本次 Core 单调时间减 `metrics_timestamp_ms`；不得使用 RTC/墙钟。

| `age` | `freshness_penalty` | 自动动态选择 |
| ---: | ---: | --- |
| `0..1500 ms` | `+0` | 可选。 |
| `1501..3000 ms` | `+20` | 可选。 |
| `3001..5000 ms` | `+60` | 可选。 |
| `>5000 ms` | 不计算 | 排除。 |

### 6.8 管理偏置：唯一可配置的减法

产品可以为每条 Link/Preset 配置有符号 `administrative_bias`，用于明确的产品策略，例如“已验证的控制 CAN-FD 物理线优先”或“电池模式下回避高功耗 Wi-Fi”。默认必须为 `0`。

| 参数 | 合法范围 | 默认 | 规则 |
| --- | ---: | ---: | --- |
| `administrative_bias` | `-32..+64` | `0` | 只来自静态产品配置；任何实时采样不得修改它。 |

- 负值是**唯一**能使有效分低于 `base_cost` 的来源；最终仍夹紧为至少 `1`。
- 产品不得用大负值补偿已知拥塞/失败；运行时质量惩罚始终照表叠加。
- 用于功耗、费用、许可证、隔离优先级等没有统一传感器定义的因素时，必须通过这个明确偏置表达；V1 不引入不透明“能耗自动分”。

## 7. 缺失数据、同分和 Unknown 的确定性处理

1. 有效但某项指标 Invalid：该项 `penalty=0`，其他有效项照算；诊断中必须显示该项 Invalid。
2. 任何 Known 基础 Cost 候选始终优于 Unknown 基础 Cost 候选，无论 Known 的动态总分多大。
3. 若全部候选均为 Unknown 基础 Cost：不执行动态评分、不会因为 RTT/占用/质量值而切换；保留当前可用 Bearer，若没有当前 Bearer 则选择最少活动 Flow 的 Link，再按 `link_id` 升序打破平局。
4. Known 候选总分相同：保留当前活动 Link；没有活动 Link 时选择 `link_id` 最小者。不得因调用顺序、数组地址或哈希顺序改变结果。
5. `PINNED_STRICT` 不因任何软 Cost、陈旧指标或质量下降而换路；只有其既有的发送失败/安全策略决定结果。
6. `PINNED_FAILOVER` 只在活动 Path 硬 Down、Path 不存在、MTU/安全门失败时启用已验证 Backup，不使用软 Cost 提前切换。
7. Q0 不使用动态 Cost 做逐帧负载均衡、逐帧重新寻路或条带化；Q1 的动态处理遵循第 9 节。

## 8. 基础 Cost 与标准 Preset

基础值是本规范的输入，仍取自[标准 Port、Adapter 与默认 Cost 基线方案](../06-平台与适配/UCN_标准Port_Adapter封装与默认Cost基线方案.md)。为使实现有唯一默认值，首版关键表在此重列；产品覆盖必须在构建/启动日志中打印“Preset、覆盖值、偏置”。

### 8.1 UART / RS-485

| UART 8N1 波特率 | `base_cost` |
| ---: | ---: |
| 9600 | 140 |
| 19200 | 92 |
| 38400 | 62 |
| 57600 | 50 |
| 115200 | 34 |
| 230400 | 24 |
| 460800 | 17 |
| 921600 | 12 |
| 1M | 11 |
| 2M | 8 |
| 3M | 7 |
| 4M | 6 |

RS-485 使用同速率 UART 值再固定加 `12`；例如 RS-485 `115200` 的 `base_cost=46`。这是稳定半双工开销，不是动态 Queue 惩罚的替代。

### 8.2 CAN、CAN-FD、Wi-Fi、USB

| 类别 | Preset / 速率 | `base_cost` |
| --- | --- | ---: |
| 经典 CAN Carrier（完成有界分段/重组后） | 125K / 250K / 500K / 1M | 110 / 72 / 45 / 30 |
| CAN-FD | 500K/2M / 500K/4M / 1M/2M / 1M/4M / 1M/8M | 22 / 15 / 18 / 12 / 9 |
| ESP-NOW | 默认 1M | 45 |
| Wi-Fi 一跳 | 1M / 2M / 6M / 12M / 24M / 54M / 未知 | 52 / 42 / 30 / 24 / 18 / 14 / 45 |
| USB CDC | Full-Speed / High-Speed | 14 / 6 |

表中是“物理层已正常、Adapter 队列空闲、没有额外有效质量惩罚”时的保守初值；不是吞吐、延迟、功耗或可靠性的承诺。

### 8.3 扩展 Bearer 的条件 Preset

下表同样冻结为 LC-1 的**条件初始值**。它们在 V5-42 对应 Adapter 尚未实现前不能被任何当前 Core 自动采用；Adapter 完成 MTU、固定队列、状态、指标、Host 测试和目标 Profile 构建门槛后，才能作为可选 Preset 编码。产品可在 V5-41 的实机标定后覆盖，但不能私自改写本规范默认值而不记录原因。

| 类别 | Preset / 明确条件 | `base_cost` |
| --- | --- | ---: |
| 以太网 | 10M / 100M / 1G，全双工、点对点或受控 LAN | 10 / 8 / 5 |
| BLE LE | 1M / 2M / Coded S=2 / Coded S=8，已连接单跳通道 | 65 / 48 / 160 / 360 |
| 原始 IEEE 802.15.4 | 250K、单跳 ACK 可用 | 75 |
| NRF24 / 私有 2.4G | 250K / 1M / 2M，单跳 ACK 可用 | 120 / 70 / 50 |
| FSK Sub-GHz | 50K / 100K / 250K，点对点 ACK 可用 | 260 / 170 / 95 |
| LoRa P2P | SF7/BW250K / SF7/BW125K / SF9/BW125K / SF12/BW125K | 180 / 240 / 420 / 820 |
| UWB 数据通道 | 850K / 6.8M，已完成时隙和 MTU 配置 | 120 / 55 |
| SPI | 1M / 4M / 10M，板内固定角色点对点 | 20 / 12 / 8 |
| I²C | 100K / 400K / 1M，板内固定角色点对点 | 120 / 70 / 45 |
| IP Tunnel | 受控同一 LAN 的 UDP 单跳 Tunnel | 25 |
| IP Tunnel | 公网、蜂窝、未知 NAT/重连策略或多层转发 | Unknown；`route_cost_valid=false` |

这些值是 UCN 的**产品策略排序**，并非各标准规定的性能换算结果。尤其 LoRa、BLE Coded、UWB 和公网 Tunnel 的实际服务能力受空口时间、连接间隔、时隙、功耗、网关和运营商条件影响很大；没有精确匹配到表中“速率 + 单跳条件”的配置必须报告 Unknown，而不是挑一个看起来较低的值。

## 9. 选择、加减与切换机制

### 9.1 每 `500 ms` 的计算顺序

```text
for each Link:
    执行状态门（第 3 节）
    读取/更新有效指标 EWMA（第 5 节）
    从第 6 节查出每项惩罚；Invalid 项为 0
    effective_select_cost = 第 2 节公式
按第 7 节排序候选
按本节滞回、探测和 QoS 规则决定是否真正切换
```

这不是“每发送一帧重新寻路”。现有已验证 Route/Path 保持可用；Cost 只在固定周期评估同一 Neighbor 的 Bearer、本节点候选和符合条件的 Q1 Flow。

### 9.2 软切换的精确阈值

活动 Link 的分数为 `A`，候选 Link 的分数为 `C`。候选只有满足以下**全部**条件才计为一次“改善样本”：

```text
C × 100 ≤ A × 80
```

即候选至少低 `20%`，等号也允许。为防止乘法溢出，比较必须使用至少 `uint32_t`。同一候选连续获得 `3` 个改善样本（总计至少 `1500 ms`）后，必须完成 `2` 次该候选 Bearer 上的成功 Probe/ACK，才允许把活动 Bearer 切换为候选。

- 任一周期候选不可选、条件不满足、Probe 超时或 ACK 错误，候选的连续计数归零。
- 每个邻居只维护一个“当前待验证候选”；分数最低的候选替换旧待验证候选时，新候选从 `0` 开始计数。
- 切换后对该邻居启动 `3000 ms` 保持期：保持期内不进行新的软 Cost 切换，除非当前 Link 触发硬失效。
- 前三次 Probe 的业务数据仍走旧活动 Link；Probe 是控制维护帧，仍受既有“连续 4 个业务后给到期维护一个槽位”的调度契约限制。

### 9.3 硬失效与恢复

`is_up=false`、有效 MTU 不足、安全/Wire Class 门失败、活动 Link 变为不可选或底层明确发送 `UCN_ERR_LINK_DOWN` 时属于硬失效：

- 不等待 `3` 个样本，不等待 `20%` 条件，不保留 3000 ms 保持期。
- 若已有满足状态门的已验证 Backup/替代 Bearer，立即使用；否则执行既有 RERR、Route Invalid、Discovery/恢复路径。
- 硬失效恢复成 `is_up=true` 后，必须重新经历第 9.2 节的 3 样本和 2 Probe/ACK，不能因旧 EWMA 或旧 Primary 身份立即抢回。

### 9.4 Q1 `AUTO_BALANCE` 的确定规则

本规范不改变现有“Q1 Flow 租约固定、不会逐帧条带化”的原则。V5-44 生效后：

- 新建 Q1 Flow 或租约到期的 Flow：在 Policy 指定、已验证且可选的 Path 中比较出口 Link 的 `effective_select_cost × (active_flow_count + 1)`，取最小者。
- 相同分数：保留既有 Flow 绑定；新 Flow 选择 `path_handle` 最小者。
- 现有 Flow：只在硬失效、连续 `3` 个评分周期 Queue EWMA `≥800‰`，或其租约到期时重绑；软 Cost 变好本身不能打断未到期 Flow。
- Q0、禁止 Discovery 的 Policy、没有 Primary 的 Policy、`PINNED_STRICT`、`PINNED_FAILOVER` 的限制保持现有契约，不因本规范放宽。

### 9.5 多跳 Route 边界

多跳 RREQ/RREP 的累计 Cost 只加每跳 `base_cost`；不得把某节点私有的 Queue、RSSI、RTT、失败率或新鲜度传播给其他节点。`effective_select_cost` 可以影响本节点在多个本地出口间选哪个 Bearer，但不能改变已发现 Route 的线上累计数，也不能把局部短暂拥塞伪装成全网更差的路径。

## 10. 必须通过的测试向量

V5-44 的单元测试必须至少逐项断言以下向量；其中“Invalid”表示该项惩罚为零但诊断有效位为 false。

| 编号 | 输入 | 期望结果 |
| --- | --- | --- |
| C01 | UART 115200：`base=34`，Queue=`760`，TX fail=`25`，RX=Invalid，RTT=`13`、ref=`5`，quality=Invalid，fresh，bias=`0` | `34+50+20+0+4+0+0=108`。 |
| C02 | ESP-NOW：`base=45`，Queue=`100`，TX fail=`0`，RTT=`10`、ref=`12`，quality=`700`，fresh，bias=`0` | `45+0+0+0+0+5+0=50`。 |
| C03 | CAN-FD 1M/4M：`base=12`，Queue=`500`，TX fail=`100`，RX=`50`，RTT=`10`、ref=`3`，bus load=`750` 即 busy=`750`、quality=Invalid，fresh | `12+25+80+20+4+35+0=176`。 |
| C04 | USB FS：`base=14`，所有动态项 Invalid，fresh，`bias=-10` | `4`。 |
| C05 | `base=65534`，任一正惩罚 | 结果夹紧为 `65534`，不回绕。 |
| C06 | metrics age=`1500/1501/3000/3001/5000/5001 ms` | freshness 分别为 `0/20/20/60/60/不可选`。 |
| C07 | 活动 `A=100`，候选 `C=80` 连续 3 次，2 次 Probe ACK 成功 | 允许软切换；`C=81` 不满足，不累计。 |
| C08 | Known `base=2000` 对 Unknown `base` | Known 必须优先；不把 Unknown 当 `65535` 后比较。 |
| C09 | 全部基础 Cost Unknown | 不读取动态分数决定切换；保留活动，否则最少 Flow 再 `link_id` 升序。 |
| C10 | 活动 Bearer `is_up=false` | 立即硬失效处理，不等待 3 样本/Probe。 |

除上述单元测试外，必须有 Full/Lite/Nano 构建测试：Nano 不分配动态快照且保持静态基础 Cost；Lite 只有启用且资源预算通过时才含受限同 Neighbor 槽；Full 覆盖全部计算、诊断、软切换和 Q1 重绑。真实介质标定仍归 V5-41，不得把 Host 虚拟结果写成硬件性能结论。

## 11. 实现任务与验收

| 任务 | 范围 | 完成判据 |
| --- | --- | --- |
| V5-43 | 冻结本 LC-1 规范并将旧模糊动态表迁移为引用。 | 已完成（设计）。 |
| V5-44 | 扩展可选 Metrics（RX failure、medium busy、medium quality、timestamp/bad metric），实现定点 EWMA、Resolver、状态门、诊断及第 10 节向量。 | 已完成（Host 软件）；Full/Lite/Nano 公共 API/链接通过，无动态分配，Wire 不变。 |
| V5-36 | 将 V5-44 的 Resolver 接入同 Neighbor Bearer、Candidate 和 Q1 Flow 策略。 | 已完成（Host 软件）；20%/3 样本/2 Probe/3000 ms、硬失效、Unknown、Q0/Pinned/Flow 租约边界均保留。 |
| V5-41 | 在真实目标板标定每种正式 Preset。 | 记录 RTT、失败率、Queue、吞吐、P50/P95、功耗和切换收益；若修改表值，递增本规范版本并保留原始数据。 |

## 12. 明确不纳入 LC-1 的因素

下列因素已被考虑，但由于没有跨 MCU/驱动可重复的统一测量定义，LC-1 不做自动隐式计算：电池剩余容量、瞬时功耗、付费链路费用、用户业务优先级、GPS 距离、单次 RSSI、厂商私有重传等级、CPU 负载、Core Q0/Q1/Service 队列。产品若确有稳定策略，必须用第 6.8 节的静态 `administrative_bias` 或独立 Policy 表明确表达，并在产品文档中说明。

这条边界是为了保证“所有节点都能解释为什么选了某条链路”，而不是把难以复现的启发式逻辑藏进一个 Cost 数字。
