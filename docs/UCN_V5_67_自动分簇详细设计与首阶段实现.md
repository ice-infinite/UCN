# UCN V5 自动分簇详细设计与首阶段实现

> 状态：首阶段软件实现；Wire v5 Core 不变，Cluster Payload 自带格式版本 1。本文冻结单层选举边界，不把簇间路由或万级能力写成已完成。

## 1. 目标与非目标

首阶段目标：在没有 Linux 和中心控制器的情况下，让 MCU 使用已准入的一跳邻居，确定性选出簇头，受容量约束加入，靠租约检测簇头/成员离开，并在簇头失效后重新选举。

本阶段不负责：全网最优聚类、跨簇数据转发、服务目录、时间同步、生产密钥、备用簇头、簇拆分/合并和多层 Cluster。

## 2. 分层边界

```text
产品/BSP
  ├─计算 head_score
  ├─把 Endpoint 0xA0 绑定到 ucn_cluster_receive()
  └─把 Cluster send 回调接到 ucn_node_send_endpoint()

ucn_cluster（可选 Extended）
  ├─只读 Neighbor Summary
  ├─选举/加入/租约状态机
  └─固定 Peer/Candidate/Member 表

UCN Core
  ├─HELLO、Neighbor、Route、Endpoint、QoS、Security
  └─不知道 Cluster 角色，不依赖 Linux
```

Cluster 对象由唯一 Protocol Owner 调用。ISR、驱动回调和业务 Task 不得并发执行 `ucn_cluster_sync_*()`、`receive()` 或 `step()`。

## 3. 固定资源

| 宏 | 默认 | 用途 |
| --- | ---: | --- |
| `UCN_CLUSTER_MAX_PEERS` | 8 | 参与本地选举的一跳 Peer |
| `UCN_CLUSTER_MAX_CANDIDATES` | 8 | 候选簇头广告缓存 |
| `UCN_CLUSTER_MAX_MEMBERS` | 16 | 本机作为 Head 时的成员租约槽 |
| `UCN_CLUSTER_OBSERVATION_MS` | 5000 | 启动观察期 |
| `UCN_CLUSTER_ELECTION_WINDOW_MS` | 3000 | 候选比较窗口 |
| `UCN_CLUSTER_ADVERTISE_INTERVAL_MS` | 1000 | 轮转广告间隔 |
| `UCN_CLUSTER_KEEPALIVE_INTERVAL_MS` | 2000 | 成员保活 |
| `UCN_CLUSTER_LEASE_MS` | 8000 | Head/Member 租约 |

产品可以通过全局用户配置覆盖宏，也可在 `ucn_cluster_config_t` 中覆盖单实例时间参数。静态数组容量只能编译期设置。

## 4. 评分规则

当前状态机接收 `0..10000` 的 `head_score`，分数高优先，同分时 Node ID 小优先。产品可在唯一 Owner 上下文调用 `ucn_cluster_set_head_score()` 更新经过滤的评分；该调用会更新后续广告，但不会单凭一次变化强迫既有 Head 立即退位。首阶段不在协议内部猜测不同硬件的权重，产品应按固定公式提供，例如：

```text
score = power_class
      + compute_class
      + bearer_capacity
      + stable_neighbor_quality
      - queue_pressure
      - recent_failure_penalty
```

输入必须限幅并低通，不能把瞬时 RSSI 直接映射为 Head 变化。当前已验证选举前评分改变会选出新的最优 Head；运行中跨簇评分迁移、最小任期策略和完整簇合并仍归 C07。

## 5. 控制消息

Cluster 固定 Payload 28 B：

| 偏移 | 长度 | 字段 |
| ---: | ---: | --- |
| 0 | 1 | Cluster Format Version |
| 1 | 1 | Message Type |
| 2 | 1 | Sender Role |
| 3 | 1 | Flags |
| 4 | 4 | Cluster ID |
| 8 | 4 | Term |
| 12 | 4 | Head Node ID |
| 16 | 2 | Head Score |
| 18 | 2 | Available Capacity |
| 20 | 4 | Lease ms |
| 24 | 4 | Nonce |

字段使用大端序。载荷由 Endpoint `0xA0` 承载；它不是新的 Core Frame Type，因此 Core Wire v5 不升级。产品上线时应给该 Endpoint 配安全策略，并将 `require_protected_control` 设为 true。

## 6. 产品接入

初始化阶段：

```c
ucn_cluster_config_t config = {0};
config.local_node_id = local_id;
config.enabled = true;
config.head_capable = true;
config.head_score = 7200U;
config.member_capacity = 8U;
config.require_protected_control = true;
config.now_ms = product_now_ms;
config.send = product_cluster_send;

ucn_cluster_init(&cluster, &config);
```

`product_cluster_send()` 只负责把 `destination/endpoint/payload` 交给正常的 `ucn_node_send_endpoint()`；不得绕过 Core 路由、安全和队列。Endpoint Handler 收到 `0xA0` 后，把来源 Node ID、是否已保护及 Payload 传给 `ucn_cluster_receive()`。

唯一 Owner 循环：

```text
Node/Adapter Pump
  → ucn_cluster_sync_node_neighbors(cluster, node)
  → 处理 Endpoint 0xA0 到达的 Cluster 消息
  → ucn_cluster_step(cluster)
  → 正常 ucn_node_step()/Transfer/Service 调度
```

Neighbor Summary 只接受 `ADMITTED` 和 `SUSPECT` 邻居。Nano 没有动态 Neighbor 生命周期，虽然 API/库可编译，但产品若不给它显式 Summary，就不能自主发现和分簇；自动分簇推荐 Lite/Full。

## 7. 故障与安全规则

- 非已准入 Peer 的 Cluster 消息拒绝；
- 格式版本、长度、枚举和分数非法即拒绝；
- 开启保护门禁后，明文控制消息拒绝；
- Term/Nonce 陈旧消息不回退状态；
- Head 满容量必须 JOIN_REJECT，不能静默覆盖成员；
- Member/Head 租约到期释放状态，再走观察/重选；
- 单次 Step 只向一个轮转 Peer 广告，控制开销有界；当前受损模拟观测到每节点任意 1 s 窗口峰值不超过 14 条。
- 丢包导致相邻候选同时成为 Head 时，较差 Head 在连续优胜样本和最小任期满足后转为加入更优 Head；旧成员通过租约恢复。该逻辑只消解局部重复 Head，不等同于完整拆簇/并簇。

当前 Cluster 还没有独立 Token Bucket；接入 Core 后仍受 Endpoint/QoS 路径约束。C07 应补 Cluster 级预算，防止高密度拓扑或反复拆并形成控制风暴。

### 7.1 默认稳定档与 C05.1 快速恢复档

默认档继续使用 `5 s` 启动/重新观察、`3 s` 选举、`8 s` 租约；它是未知无线和稳定优先产品的默认值。C05.1 增加显式 `UCN_CLUSTER_TIMING_PROFILE_FAST_FIXED`：只对已确认 Head 租约到期后的重新观察使用 `500 ms`，配合 `2 s` 租约、`1 s` 选举，使固定低丢包有线链路的故障恢复目标落在约 `3.5～4 s`。

快速档不改变 Cluster Wire Format、Core 路由或业务数据面，代价是更高控制频率；只适合固定 UART/CAN/RS-485/以太网等经实测确认的低丢包介质。所有时序、租约三分之一安全关系、API 调用和 Host/MCU 验证进度见[快速簇恢复档设计与验证](UCN_V5_C05_1_快速簇恢复档设计与验证.md)。

## 8. 软件验证与资源

- `test_cluster`：Codec、安全、4 节点选举、容量、Head 失效重选、Neighbor Summary；
- `ucn_cluster_sim --nodes 64|256|1000 --scenario clean|impaired`：每 8 节点一组的确定性和受损单层收敛；
- `head-failover`、`mobility`、`score-shift`：验证 Head 断开、成员离开超过租约后返回、选举前评分改变；
- 默认 Host x64 `sizeof(ucn_cluster_t)=880 B`；它不是 MCU 实测值；
- Core-only 不链接 `ucn_cluster` 时，不增加 `ucn_node_t` 和 Core Archive 的 Cluster 状态。

固定 Seed 的受损模型为 15% 随机丢包、0～40 ms 延迟（自然产生乱序）、5% 重复和选举期 2 s 单向阻断。结果为：

| 节点 | 最终 Head/Member | 收敛 | 丢弃/重复 | Switch 事件 | 1 s 单节点控制峰值 |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 64 | 8 / 56 | 39.66 s | 815 / 210 | 10 | 12 |
| 256 | 32 / 224 | 40.70 s | 3157 / 871 | 25 | 13 |
| 1000 | 125 / 875 | 41.84 s | 12282 / 3280 | 64 | 14 |

64 节点 Head 故障场景先在 8.91 s 收敛，首选 Head 于 12 s 离开后 16.16 s 完成恢复；成员离开超过租约并于 23 s 返回后 0.41 s 重入；4 s 改变候选评分后 4.91 s 选出新的最优 Head。这些仍是隔离八节点组的 Host 状态机证据，不包含共享无线信道竞争。

### 8.1 C05 四节点 MCU 首轮证据

四块 ESP32-S3-N16R8 使用 3 Mbaud UART 组成 A—B—C—D 线形拓扑，固定
Node ID=`0x10000001..4`、Score=`7000/9000/8000/6000`。常规容量 2 时，B 在约
8.31 s 成为 A/C 的 Head，D 在约 16.32 s 成为独立 Head；这是 Cluster 只比较一跳
已准入邻居的预期结果，普通数据面 A→D 仍可走三跳 Route。

容量 1 时严格形成 `{B,A}` 和 `{C,D}`，没有超额成员。一次 20 s 和三次独立 12 s
B-Head 复位均恢复；重复三轮的 Head 租约失效约 7.2～7.8 s，释放后首个三跳冷路由
包为 264～266 ms，随后回到 2～3 ms。120 s 稳定窗口内收敛后无角色抖动，A 的
156 个 Ping 全部收到回复。

ESP32 ABI 下 `sizeof(ucn_cluster_t)=860 B`。115 s 快照的 Cluster 累计 CPU 时间折算
约 0.037%～0.069% 单核，单次观测最大 165～225 us；它是当前诊断固件的量级，不是
硬实时 WCET。详细固件 Hash、资源和原始日志索引见
[C05 四节点实测报告](UCN_V5_C05_四节点自动分簇实测报告.md)。

恢复后的高分 B 会加入已经工作的低分 Head A，而不会立即抢占。首阶段保留此稳定
优先行为；C07 必须明确最低任期、优选 Head 恢复和合并策略。独立 Bearer 插拔、小时级
长稳、功耗和受保护控制仍未测试，因此 C05 不能标记全部完成。

## 9. 下一阶段接口方向

C06 不应简单扩大 Route/Path 表，而应引入：

```text
Node ID → Cluster Locator（目录/租约）
Cluster Locator → Gateway/Next Cluster（聚合转发）
Inner UCN frame → Cluster Tunnel Envelope → 目标簇解封装
```

只有完成 Locator、目录失效、隧道 MTU/分片、安全绑定和环路防护后，自动分簇才能实际减少远端逐节点路由状态。两级分簇和万级模拟必须放在这之后。
