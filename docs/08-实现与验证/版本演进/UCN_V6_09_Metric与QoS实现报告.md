# UCN V6-09 Metric 与 QoS 实现报告

## 1. 目标与边界

V6-09 在 V6-08 的身份绑定 RouteSet 上增加两层彼此分离的决策：Metric 负责把链路与路径
观测转换成同算法可比较的 Cost；QoS 负责在已经准入的 Frame 和调用方 Buffer Token 上执行
固定容量调度。Metric 不授权 Route，QoS 不验证业务 ACL，也不解析 E2E Realtime Envelope。
所有输入必须先经过 V6-07 Hop Security；本阶段仍由 `UCN_BUILD_V6_EXPERIMENTAL=ON`
显式启用，不接 v5 Node/Adapter。

## 2. Metric 的唯一单位和 Unknown

| 字段 | 单位 | Known 位 | 合法值和保守方向 |
|---|---|---:|---|
| `administrative_cost` | 产品无量纲成本 | bit 0 | 0～65535，越大越差 |
| `latency_us` | 微秒 | bit 1 | 0～2^32-1，越大越差 |
| `jitter_us` | 微秒 | bit 2 | 0～2^32-1，越大越差 |
| `loss_ppm` | 百万分率 | bit 3 | 0～1,000,000，越大越差 |
| `available_bitrate_bps` | bit/s | bit 4 | Known 时必须非零，越大越好 |
| `queue_occupancy_permille` | 千分率 | bit 5 | 0～1000，越大越差 |
| `energy_cost` | 产品无量纲成本 | bit 6 | 0～65535，越大越差 |
| `stability_score_permille` | 千分率 | bit 7 | 0～1000，越大越好 |

Unknown 不是零值。字段 Unknown 时必须同时清除 Known 位并把存储值规范为零；Known 的
latency/loss/admin 等允许真实零值，Known bitrate 不允许零。每次样本还绑定完整 Route Domain、
Route/Path Generation、Path ID、采样时刻与窗口。相同时间戳、完全相同原始样本是幂等重放；
相同时间戳不同内容或更旧时间戳返回 `REPLAY`，不改变滤波状态。

## 3. EWMA、陈旧和 Cost

默认 `alpha=250/1000`。成本型指标在整数除法时向上舍入；收益型 bitrate/stability 向下
舍入，避免因量化把路径估计得比证据更好。新样本将自己的 Known Mask 作为当前有效集合：
本次 Unknown 的旧字段不会继续冒充新观测。

默认样本在 `now - measured_at >= 1,000,000 us` 时陈旧，边界为半开区间。时间倒退也按
不可用处理。显式 `expire()` 才释放槽；敌对 score 请求不能借 lazy cleanup 驱逐其他 Path。

默认 Algorithm ID 为 `0x00060001`。Q0～Q3 各自拥有八项固定权重，归一化使用 10 us
latency/jitter、100 ppm loss、`ceil(1e9/bitrate)` inverse bitrate、以及原始 permille/
cost。某项权重非零而样本 Unknown 时整个 score 失败关闭。跨跳累加只允许 Algorithm ID
一致，Hop Count 与 Cost 均 checked-add；不同算法的 RREQ/RREP Cost 不可比较。

## 4. QoS 固定资源和身份归属

默认容量全部进入 Feature Manifest/Layout Hash：Q0=16、Q1=16、Q2=32、Q3=32、
Flow=32、Inflight=32。表满不驱逐其他 Source 或 Flow。

Security open result 明确携带 `ingress_peer_session`。它是本机真正认证的邻居 Principal、
Binding 与 Session Generation，和 Frame 中可能跨多跳保留的 Origin Source 分开。Token
Bucket 与最大 Flow 数以 Ingress Peer Session + canonical Flow 为归属；Flow hash 另外绑定
Origin/Destination 地址及 Binding、Endpoint、Opcode、Frame Type 和 Traffic Class。因此：

- 中继不能把未认证的远端 Source 当成本地配额主体；
- 邻居 Reauth/撤销时能精确回收其 Queue、Flow 和 Inflight Buffer；
- 同地址换 Binding、同 Peer 换 Session、同 Endpoint 换 Opcode 都不能继承旧 Flow 状态。

默认每 Session 最多 8 个 Flow。每个 Flow、每个 Class 使用独立 Token Bucket；回填只由
本地单调 `now_us` 和固定周期计算，时间倒退拒绝。

## 5. Traffic、Delivery 与调度

Traffic Class 与 Delivery Guarantee 保持正交：

- Q0：有界 FIFO，并在相同本地优先级内按 Hop Budget 绝对到期点做 EDF；
- Q1：跨 Flow round-robin；如果 Delivery 是 Reliable，多个消息仍按 FIFO 保留；
- Q2/Q3：跨 Flow 的 Deficit Round Robin，默认 quantum 分别为 512/256 B；
- 任意 Class 的 `LATEST` Delivery：只替换同 Flow 已排队的上一条 Latest，并把旧 Buffer
  Token 返回调用方；不会覆盖同 Flow 的 Reliable 消息。

Class 轮转表有 12 个固定槽，权重为 Q0:Q1:Q2:Q3=`6:3:2:1`。权重表示有竞争时的服务
机会，不承诺固定带宽；空 Class 不占用物理发送。Flow cursor 防止固定从 slot 0 扫描造成
热点 Flow 饥饿。Q2/Q3 的 deficit 不足时只积累 quantum，不借用 Q0 预留。

## 6. Hop Scheduling Budget

Budget 只接受 V6-07 已 Hop-authenticated 的外层扩展，固定校验：

```text
0 < remaining_budget_us <= initial_budget_us <= policy_max[class]
```

它不能修改 Traffic Class，也不能提高本地优先级。转发时以 checked arithmetic 扣除本跳
驻留上界和发送上界；`debit >= remaining` 时唯一结果是 `DROP_EXPIRED/TIMEOUT`。Scheduler
到达同一半开边界也只产生 DROP 决策；调用方不能把该选择标为 RETRY 或删除扩展后重新排队。
E2E Deadline 仍只由源和目标解析，中继不读取加密 Payload。

## 7. Buffer 与 Completion 所有权

Queue 只保存 caller-owned `buffer_token` 和元数据，不复制可变 Payload 指针。一次选择遵循：

```text
QUEUED --select/peek--> SELECTED
SELECTED --RETRY--> QUEUED
SELECTED --LINK_SUBMITTED--> INFLIGHT
SELECTED expired --DROP_RETIRED--> caller owns token again
```

被选中的 Latest 项不能在未完成选择时被更新。Inflight 分别记录 Link Submitted、Physical
Completed、Remote ACKed、Application Result，阶段只能精确前进或 exact replay；跳级失败。
统计漏斗因此能区分调度队列损失、Driver/物理层损失、远端接收损失和业务结果缺失。

Session invalidation 先统计 Queue+Inflight 全部 Token 数并核对调用方输出容量；容量不足时
对象和输出都不写。容量足够才一次性返回 Token、清 Queue/Inflight/Flow 及未决 selection。

## 8. 分项自审

| 小节 | 自审结论 |
|---|---|
| 09-01 Metric 合同 | 单位、Known/Unknown、范围、原始 replay 与 Path 代际均固定 |
| 09-02 滤波/Cost | 成本向上、收益向下，陈旧半开，同 Algorithm checked 累加 |
| 09-03 身份/配额 | Ingress Peer Session 与多跳 Origin 分离，固定 per-session/flow 门禁 |
| 09-04 调度 | `6:3:2:1`、Flow RR、Q0 EDF、Q2/Q3 DRR 均有确定性测试 |
| 09-05 Delivery | Reliable 与 Latest 不再由 Q1 隐式决定，跨 Class Latest 回归通过 |
| 09-06 Budget | 越界、溢出、精确耗尽和禁止 RETRY 均失败关闭 |
| 09-07 Completion | peek/commit、阶段跳变、幂等、Session 原子回收均覆盖 |

自审期间发现并关闭：重复输入误与滤波值比较、32-bit 归一化加法溢出、收益型 EWMA
向上舍入、Latest 再次耦合 Q1、过期 Budget 可 RETRY、以及 Ingress Peer Principal 与多跳
Origin Binding 错误拼接六类问题。

## 9. 验证和未完成项

| 门禁 | 结果 |
|---|---|
| Windows GCC Full | 68/68 |
| MSVC Release Config/Security/QoS | 3/3 |
| WSL ASan/UBSan QoS | 1/1 |
| WSL `-fanalyzer -Werror` QoS | 1/1 |
| default-OFF `ucn_core` v6 symbol | 0 |
| `git diff --check` | 无空白错误，仅行尾转换提示 |

当前没有生产 Adapter/Node 接线，也没有真实 CAN arbitration、UART DMA、ESP-NOW 队列、
硬件 timestamp 或最坏调度延迟证据。Transfer Window/Credit 与多跳流水属于 V6-10，实际
优先级和负载门禁属于 V6-13/14。

当前状态：`V6-09 软件实现与分项自审完成 / FINAL EXTERNAL REVIEW DEFERRED`。
