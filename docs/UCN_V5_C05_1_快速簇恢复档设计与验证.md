# UCN V5 C05.1 快速簇恢复档设计与验证

## 1. 目标与边界

C05 首轮四板实测使用保守时序：`8 s` 的 Head 租约失效、`5 s` 观察、`3 s` 选举。因此 Head 突然离线后，成员的最坏恢复量级约为 `16 s`。这适合未知无线、启动阶段和优先防抖的产品，但不适合已知低丢包、固定有线链路。

C05.1 新增一个**显式选择**的快速固定链路档：缩短故障检测和重选时间，同时不改 UCN Core Wire v5、Cluster 控制帧格式、Node ID、评分规则或数据面路由。默认档不变；没有主动调用新 API 的已有产品仍使用原时序。

该档只适合：固定 UART/CAN/RS-485/以太网等低丢包介质，且产品可接受更高 Cluster 控制频率。它不是 Wi-Fi、ESP-NOW、LoRa 或持续移动网络的默认档，更不是 C07 的备用 Head、拆并、跨簇目录或多级 Cluster 实现。

## 2. 固定时序

| 字段 | 默认稳定档 | `FAST_FIXED` | 作用 |
|---|---:|---:|---|
| `observation_ms` | 5000 ms | 1000 ms | 冷启动/普通重新观察窗口 |
| `recovery_observation_ms` | 5000 ms | 500 ms | 已确认当前 Head 租约失效后的再次观察窗口 |
| `election_window_ms` | 3000 ms | 1000 ms | Candidate 收集并比较候选者的窗口 |
| `advertise_interval_ms` | 1000 ms | 250 ms | Candidate/Head 完成一轮邻居广告的目标周期 |
| `join_retry_ms` | 1000 ms | 250 ms | Join Pending 的重试间隔 |
| `keepalive_interval_ms` | 2000 ms | 500 ms | Member 向 Head 的保活间隔 |
| `lease_ms` | 8000 ms | 2000 ms | Head 与 Member 租约 |
| `head_min_tenure_ms` | 30000 ms | 10000 ms | 同组重复 Head 消解前的最短任期 |

快速档的故障恢复量级为：`≤ 2 s` 发现租约失效 + `0.5 s` 恢复观察 + `1 s` 选举 + 调度/报文传播余量，目标为约 `3.5～4 s`。它不是每次控制帧丢失就重选：`2 s` 租约容纳多个 `500 ms` Keepalive 周期。

## 3. 状态机变化

原先 `MEMBER → DETACHED` 无论原因均重新等待 `observation_ms`。现在状态机把“当前 Head 租约确实到期”单独识别：

```text
MEMBER --Head lease expired--> DETACHED(recovery_observation_ms)
       --普通拒绝/Head stepdown--> DETACHED(observation_ms)
DETACHED --观察截止--> CANDIDATE(election_window_ms)
CANDIDATE --本机最优--> HEAD
HEAD/CANDIDATE --广告--> Member Join
```

因此只有已建立 Member 关系后、确认 Head 不再续租的故障恢复走快速观察；启动、Join 拒绝、候选落选和收到明确 Stepdown 仍走完整观察窗口，避免把控制面抖动误判为故障。

## 4. 配置与调用

`ucn_cluster_config_t` 在末尾新增 `recovery_observation_ms`，不打乱已有字段的位置初始化；零值由全局默认 `UCN_CLUSTER_RECOVERY_OBSERVATION_MS` 填充。所有 Cluster 时间参数均可由 `include/ucn/ucn_config.h`、产品用户配置头或编译 `-D` 覆盖。

推荐直接使用命名档，而不是在业务代码里分散填写 8 个毫秒值：

```c
ucn_cluster_config_t config = {0};

config.local_node_id = local_node_id;
config.enabled = true;
config.head_capable = true;
config.head_score = filtered_head_score;
config.member_capacity = 8U;
ucn_cluster_config_apply_timing_profile(
    &config, UCN_CLUSTER_TIMING_PROFILE_FAST_FIXED);
config.now_ms = product_now_ms;
config.send = product_cluster_send;
ucn_cluster_init(&cluster, &config);
```

`ucn_cluster_config_apply_timing_profile()` **仅填写时间字段**；不会覆盖 Node ID、安全开关、评分、成员容量、时钟回调或发送回调。`DEFAULT` 显式写入全局默认值，`FAST_FIXED` 写入上表快速值。未知枚举或空配置返回 `UCN_ERR_ARGUMENT`。

初始化会拒绝不安全的租约配置：Advertisement 与 Keepalive 周期必须不大于租约的三分之一，保证至少约三个周期的失联容忍度；不合法返回 `UCN_ERR_CONFIG`。产品若手动调参，必须同时审查此关系和实际 Owner 调度间隔。

## 5. 代价与选择

快速档不会改变数据帧、路由 Cost、业务 QoS 或 Transfer 吞吐上限；代价只在 Cluster 控制面：广告频率约为默认档四倍，Keepalive 与 Join 重试约为四倍。64 节点、每 8 节点一组的 Host 受损模拟中，默认档 1 s 控制峰值门限为 `32`，快速档独立门限为 `40`；不能混用一个数字判断两种设计。

选择建议：

- 固定 UART/CAN 链路，需要缩短 Head 故障恢复：选择 `FAST_FIXED`，并实测控制总线占用。
- Wi-Fi/ESP-NOW、有较强竞争或移动：先使用 `DEFAULT`；只有实测丢包、抖动和误切换都满足门限后才可评估快速档。
- 没有明确实时恢复指标：保持默认档。

## 6. 验证证据

本次 Host 软件门禁：

- Full、Lite、Nano Cluster 套件均 `12/12` 通过；全局默认/回退/产品覆盖和 Scale 合并门禁为 `25/25`；WSL ASan/UBSan 与 GCC `-fanalyzer` 各 `22/22`。其中覆盖配置 API、非法租约拒绝、64/256/1000 默认模拟、64 节点快速故障恢复与快速受损链路。
- 64 节点快速 Head Failover：`initial_ms=2250`、`recovery_ms=3680`、`max_tx_1s=32`，满足小于等于 `4 s` 的恢复门限。
- 64 节点快速受损条件（15% 丢包、0～40 ms 抖动、5% 重复、短时单向阻断）：`converged_ms=3610`、`max_tx_1s=35`，在快速档 `40` 条/秒控制门限内收敛。
- 四块 ESP32-S3 N16R8 的 A/B/C/D 已自动烧录并核验 `profile=fast-fixed`。在 3 Mbaud UART A—B—C—D 台架上，四轮合格 B-Head 复位中，A 在 `3.500–3.516 s` 接任 Head，C 在 `2.062–2.063 s` 加入 D，B 释放后在 `1.140–1.156 s` 加入 A；最终 Cluster `api_fail/malformed/security/stale=0`。这是固定有线 MCU 证据，不能把 Host `3.68 s` 或该台架结论外推为无线实测。

## 7. 后续门禁

- C05.1 四板故障重选的最低四轮门禁已通过；继续 1 h/8 h/24 h 运行，观察首个预诊断轮出现但未复现的 B `api_fail.rx=1`；
- 后续与默认档同拓扑做完整控制计数、CPU、Heap、业务 Ping 丢失的统计比较；当前只给出快速档快照，不把一次短窗外推为长稳；
- 再决定是否为 Wi-Fi/ESP-NOW 定义独立的“快速无线档”；不得直接复用固定有线参数；
- C07 的 Backup Head/恢复后优选 Head 迁移仍需独立设计，不能由本档代替。
