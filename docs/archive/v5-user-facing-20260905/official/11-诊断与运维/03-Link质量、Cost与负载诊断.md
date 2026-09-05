# Link 质量、Cost 与负载诊断

> 文档级别：`GUIDE`
> 实现状态：`CURRENT（诊断方法）`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：状态视图、统计 API、测试与现有实测记录
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：部分实测；未测项不得推断

查看基础 Cost、动态增量、RSSI/丢包、延迟、队列利用率、liveness 状态和最后采样时间。最小总 Cost 路径不一定有最大瞬时吞吐，Pinned/Policy bias 也可能有意改变选择。

排查抖动时比较候选差值、滞回阈值、最小驻留、probe 结果和切换次数。采样过慢会滞后，过快会增加控制开销和误判。

## 一条Link要同时看五类信息

1. **静态**：Bearer、bitrate、base cost、administrative bias、MTU；
2. **状态**：up/down、peer、liveness、last RX/TX；
3. **动态**：queue、TX/RX failure、RTT、busy/quality；
4. **新鲜度**：metrics timestamp/age；
5. **选择结果**：exclusion、每项penalty、effective Cost。

只打印“Cost=52”无法判断是Wi-Fi base 52，还是UART base12加了40失败惩罚。

## LC-1诊断表

建议输出：

```text
link=3 base=12 bias=0 queue=50 txfail=20 rxfail=0
rtt=8 busy=15 quality=0 stale=0 total=105 selectable=yes
```

输入千分比也应保留，例如queue=820‰。非法指标不应参与计算，但`invalid_metric_mask`必须显示；超过5000ms stale时Link被排除而不是给超大但仍可选的Cost。

## 路径为什么没有切换

依次检查：候选是否selectable→满足安全/MTU/capability/constraints→Cost是否至少改善20%→连续stable samples→probe ACK数→switch hold/min residence→Pinned/flow lease是否阻止。

```text
active=60, candidate=50
50×100 <= 60×80 ? false
```

候选虽低10，但未改善20%，不切换是正确的。

## 路径为什么频繁切换

- metrics窗口太短，failure/queue一两个样本跳变；
- busy和quality来自同一源却重复计费；
- Link time domain错误，指标反复stale/恢复；
- base cost过近、没有hold；
- Q1 flow lease太短；
- Driver queue只报0/1000而非平滑比例。

先修采样/来源，再调整滞回；不要用极大administrative bias掩盖错误指标。

## 负载均衡诊断

按flow输出selected path、lease expiry、queue pressure、congested sample count和迁移reason。总字节分布不一定50/50：目标是避免拥塞且保持flow顺序，不是按包平均。

## 采样周期

默认500ms适合一般状态；100ms probe用于候选验证。高速控制链可缩短，但要评估CPU/控制流量；LoRa等慢链需要更长窗口。所有链路用同一个周期不一定合理，仍要保持Cost单位一致。

## A/B测试

固定拓扑/包长/负载，先记录preset，再只改一个base/bias或采样参数，比较goodput、p99、切换次数、丢包和Q0延迟。没有A/B证据的“感觉Wi-Fi更快”不能作为默认Cost变更依据。
