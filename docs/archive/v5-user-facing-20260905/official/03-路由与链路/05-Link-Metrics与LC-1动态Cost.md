# Link Metrics 与 LC-1 动态 Cost

> 文档级别：`NORMATIVE`
> 实现状态：Full 动态选择 `CURRENT`；Lite/Nano 使用静态基础 Cost
> 事实源：`ucn_link.h`、`ucn_link_cost.h/.c`、Policy tests
> 最近核对：`a093862`，2026-08-25

## 两种 Cost

| 名称 | 作用 | 是否上线累加 |
| --- | --- | --- |
| `route_cost` / base cost | 介质和链路的稳定基础成本 | 是 |
| `effective_select_cost` | 当前节点基于拥塞/质量计算的本地选路分 | 否，仅本地比较 |

不能把本地瞬时 Queue 压力写入 Wire 累计 Cost，否则每个节点会用不同时间快照污染全网 Route。

## LC-1 输入

- Link up/down；
- MTU 是否足够；
- capability 是否允许；
- base cost；
- administrative bias；
- queue pressure；
- TX/RX failure rate；
- RTT 与 reference；
- medium busy；
- medium quality；
- metrics timestamp/freshness。

动态指标统一归一到固定量纲并使用整数 EWMA：`floor((3*previous + sample)/4)`。缺失指标贡献 0，不伪造为好或坏；非法指标记录 mask/统计。

## 状态门

Link Down、MTU 不足、能力不符、指标过期或配置非法会使 Link 不可选择，而不是仅加一点罚分。

同一物理计数不能同时作为 busy 和 quality 重复扣分。Adapter 必须标识两者是否同源。

## 新鲜度

源码冻结 Fresh、Light Stale 和 Stale Limit 时间域。旧指标先加 freshness penalty，超过上限后不可选择。Policy 默认定期采样，并对各指标做 EWMA。

## 滞回

候选只有“足够更好”才替换当前 Link，避免信号轻微波动导致来回切换。硬 Down/能力失效不等待普通质量滞回。

## 产品标定

Standard Adapter preset 给出一致的初始 base cost/RTT/MTU，但真实 UART、CAN、Wi-Fi、LoRa 仍需在目标硬件上测量并通过每 Link override 校准。

## Effective Cost 的确定性计算

LC-1 先执行硬状态门，再把可用指标转换为整数 penalty：

```text
effective_select_cost = clamp(
    base_cost + administrative_bias
  + queue_penalty
  + tx_failure_penalty
  + rx_failure_penalty
  + rtt_penalty
  + medium_busy_penalty
  + medium_quality_penalty
  + freshness_penalty)
```

最低值被限制为 1，最高值限制在协议 Cost 上限。整个计算不使用浮点，便于不同 MCU 得到确定结果。

## 当前分段规则

以下值均为当前 LC-1 源码合同，per-mille 范围是 0～1000：

| 输入 | 分段 penalty |
| --- | --- |
| Queue `<250/<500/<700/<850/≥850` | `0/10/25/50/80` |
| TX failure `<5/<20/<50/<100/<200/≥200` | `0/8/20/40/80/160` |
| RX failure `<5/<20/<50/<100/<200/≥200` | `0/4/10/20/40/80` |
| Medium busy `<250/<500/<700/<850/≥850` | `0/5/15/35/70` |
| Medium quality `≥850/≥700/≥500/≥300/<300` | `0/5/15/35/70` |

RTT 只惩罚高于 reference 的部分，按 `(excess+1)/2` 计算并封顶 80。Administrative Bias 合法范围为 `-32..64`，用于产品偏好而不是伪造链路测量。

## Metrics 新鲜度

| Age | 当前处理 |
| --- | --- |
| `0..1500 ms` | Fresh，无 freshness penalty |
| `>1500..3000 ms` | +20 |
| `>3000..5000 ms` | +60 |
| `>5000 ms` | 不可选择 |

没有 timestamp 的旧 Adapter 兼容快照按“当前采样”处理，不凭空制造陈旧历史；但产品 Adapter 应尽快提供真实 timestamp，才能在 Driver 停止更新时自动淘汰旧质量。

## 缺失、非法和 Unknown 的区别

- `valid=false`：该指标未提供，贡献 0；
- `valid=true` 但超范围：设置 invalid mask，该项不参与；
- base cost 未知：Link 可以处于结构可用状态，但有效 Cost 为 Unknown，选择器不能假装它优于已知候选；
- Link Down/MTU/capability/stale：直接 `selectable=false`；
- Administrative Bias 越界：配置错误并 fail-closed。

这种区分让诊断能回答“没有传感器”“传感器坏值”“基础配置未知”和“链路实际不可用”四种不同原因。

## 为什么 TX 与 RX 惩罚不对称

本节点选择出口时，TX failure 直接说明从该 Link 发出的成本，因此权重高于 RX failure。具体权重是当前规范选择，不是所有产品的自然定律；若要调整，必须升级规范/测试并重新标定，不能各节点私自改公式后仍声称同一 LC-1。

## 20% 滞回示例

`ucn_link_cost_is_sufficiently_better()` 要求候选不高于活动 Cost 的 80%。活动 UART Cost=100：

- Wi-Fi=81：虽略好，但不切换；
- Wi-Fi=80：达到阈值，可进入后续连续样本/探测门；
- UART 硬 Down：不看 20%，立即排除。

滞回避免在 99/100 之间来回抖动，但不能掩盖硬故障。

## Base Cost 与动态 Cost 为什么不能混写到线上

AODV 累加的基础 Route Cost 要相对稳定，代表每跳长期代价。Queue 和 RSSI 每几十毫秒变化，只适合本地在多个已知出口间选择。如果把它们写入 RREQ/RREP：

- 同一发现的不同时间副本不可比较；
- 瞬时拥塞会在多跳传播中被放大；
- 每个节点的传感器定义不同，Cost 失去统一量纲；
- 网络会高频重发现和振荡。

## 产品标定步骤

1. 用 preset 获得可工作的保守 base cost/RTT reference；
2. 在目标硬件测空载吞吐、RTT、丢包和 CPU；
3. 测业务负载下 queue/busy/quality 与真实延迟的相关性；
4. 为每个具体 Link 实例设置 override，而不是只按“Wi-Fi 类”一刀切；
5. 注入丢包、Bus-Off、peer loss 和 stale metrics 验证状态门；
6. 观察 20%/连续样本条件下是否既不抖动又能及时切换；
7. 把标定 commit、固件、硬件和环境写入 evidence。

## 验证清单

- [ ] LC-1 每个分段边界有测试；
- [ ] EWMA 使用 `floor((3*old+sample)/4)` 且无溢出；
- [ ] shared medium source 不被 busy/quality 双重扣分；
- [ ] 旧指标按 1500/3000/5000 ms 正确处理；
- [ ] Unknown、Invalid 与 Excluded 在诊断中可区分；
- [ ] 20% 滞回边界和硬故障旁路通过；
- [ ] Effective Cost 不写回 Wire Route Cost。
