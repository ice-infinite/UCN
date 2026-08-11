# UCN 网络容量与关键参数总览

> 版本口径：UCN v5 V5-10 软件状态，分支 `codex/v5-adaptive-wire`，2026-08-11。
> 目的：集中说明当前网络规模、直连能力、跳数、寻址范围、寻路效率、帧效率、资源占用和关键默认参数。
> 边界：源码与 Host 模拟是当前事实；ESP32 两板结果只代表对应测试镜像，不能外推为多板、多跳或其他介质性能。

## 1. 先给结论

当前 UCN 没有一个脱离场景的“全网最多 N 个节点”数字，必须分四个口径看：

1. **地址空间**：API 保持 32 bit；实际可表示范围由 W0/W1/W2/W3 决定，分别适合 8/16/24/32 bit 域。
2. **单节点直接连接**：默认最多注册 4 个逻辑 Link；最多保存 8 个 Neighbor 状态，每个 Neighbor 最多 2 个 Bearer，但所有已注册 Link 仍受总数 4 限制。
3. **单节点远端工作集**：默认同时缓存 8 个远端 Route，最多并发发现 4 个未知目的地。
4. **已验证规模**：Host 稀疏 Tree 的直连业务中，W0/mixed 在单域上限 254 Node、W1/W2/W3 在 Harness 上限 4096 Node 均为 100%；远端高并发工作集会出现固定表/建路工作集饱和。实机目前只有两块 ESP32-S3 的一跳台架证据。

因此当前最准确的产品口径是：

> UCN API 使用 32 位 Node ID，默认固定 Wire W3，也可显式配置固定域或自动选最小官方档位。当前默认每节点支持 4 个逻辑 Link、8 个 Neighbor 状态、每 Neighbor 2 个 Bearer、8 条远端 Route、4 个并发寻路和 Build 上限 16 跳；总网节点数由 Wire 域、拓扑、活跃目的地、共享介质与负载共同决定。

## 2. 节点数量上限

| 口径 | 当前默认/上限 | 说明 |
| --- | ---: | --- |
| Node ID 类型 | 32 bit | `ucn_node_id_t = uint32_t`。 |
| 单 Network 可用 Node ID | 4,294,967,294 | `0` 非法，`0xFFFFFFFF` 为广播，合法范围 `1..0xFFFFFFFE`。 |
| 单节点注册 Link | 4 | `UCN_MAX_LINKS`；一个 Link 是 Core 看见的一条逻辑承载。 |
| Neighbor 状态 | 8 | `UCN_MAX_NEIGHBORS`；保存候选、准入、可疑、移除等状态。 |
| 每 Neighbor Bearer | 2 | `UCN_MAX_BEARERS_PER_NEIGHBOR`；如同一对端 UART Primary + ESP-NOW Backup。 |
| 远端 Route | 8 | `UCN_MAX_ROUTES`；直连查找不要求占用远端 Route 槽。 |
| 并发 Route Discovery | 4 | `UCN_MAX_ROUTE_DISCOVERIES`。 |
| 未知路由 Q1 Pending | 4 | `UCN_PENDING_Q1_DEPTH`。 |
| Candidate Route | 8 | Full Profile 才启用。 |
| Path Forward Entry | 8 | Full Profile 的逐跳显式 Path 转发表。 |

这几个上限互相约束。例如一个节点与两个邻居分别使用 UART+Wi-Fi，会占满 4 个 Link；此时 Neighbor 表即使还有空槽，也不能再准入新的 Link，除非重新编译更大的 `UCN_MAX_LINKS` 并重新评估 RAM、维护时延和 Adapter 资源。

UCN 不在每个节点保存“全网完整成员表”。总网络可以比 8 Node 大很多，但单节点默认只能高效维护少量直连对端和最多 8 个活跃远端目的地。

Wire Profile 还会收窄单个部署域的可表示范围。W0/W1/W2/W3 地址字段为 1/2/3/4 B；各档最大普通地址为对应全 1 广播保留值减一。自动模式会按本帧的 Network/Source/Destination/Session/Path/Hop 与出口能力选择能完整表达它的最小档，不能表达时明确失败。

同一个扁平域只要包含 W0 节点，全网一跳控制和经由该节点的业务地址就必须能被 W0 表达，因此 mixed（W0～W3）同样以 254 个单播节点为域内上限。跨越该边界必须使用 V5-06 规划的 Domain/Gateway/Alias，不能静默截断高位地址。

源码依据：[ucn_types.h](../include/ucn/ucn_types.h)、[ucn_node.h](../include/ucn/ucn_node.h)、[ucn_neighbor.h](../include/ucn/ucn_neighbor.h)、[ucn_path.h](../include/ucn/ucn_path.h)。

## 3. 跳数与寻址距离

### 3.1 逻辑跳数

当前 Build 默认 `UCN_MAX_HOPS=16`，实际单帧上限取 Build 与 Wire Profile 两者较小值：

| Wire Profile | 协议档最大 Hop | 默认 Build 下实际最大 |
| --- | ---: | ---: |
| W0 | 4 | 4 |
| W1 | 16 | 16 |
| W2 | 64 | 16 |
| W3 | 254 | 16 |

- 每个产品在 `ucn_config_t.default_hop_limit` 中配置实际值，并且不得超过其固定发送档；自动业务仍会按已知 Route Hop 选择足够档位。
- 最长业务路径可经过 16 条 Link。
- 对应路径最多包含 17 个节点位置（源节点到目标节点）。
- 中继收到非目标帧且 `hop_limit<=1` 时返回 `UCN_ERR_TTL`，不会继续扩散。
- 如果希望网络中任意两点互通，实际网络拓扑直径必须不大于产品配置的 Hop Limit。

### 3.2 寻址范围

逻辑上，只要满足以下条件，节点可向同一 Network ID 内任意合法 Node ID 寻址：

1. 目标在 Hop Limit 内；
2. 沿途 Link/Neighbor 已准入并健康；
3. 沿途节点的 Route/Path/控制表没有耗尽；
4. 安全 Provider 与产品 ACL 允许该业务；
5. 目标 Endpoint 已注册且 Payload 合法。

物理距离不由 UCN 固定：Wi-Fi/ESP-NOW、UART/RS485、CAN、BLE、LoRa 的距离、速率、冲突和重传由 Adapter 与硬件决定。UCN 负责通过中继组合这些物理链路，但当前最多 16 跳。

### 3.3 路径与全网诊断

- 默认 256 B 帧下，按需 Path Trace 最多记录 17 个 Node ID；64 B Profile 最多记录约 6 个，超出会截断。
- Node Snapshot 单次最多保存 8 个结果，可标记截断；它是低频诊断，不是常驻全网拓扑表。
- Path Trace、Snapshot 和 Policy Diagnostic 不增加普通业务帧字段。

## 4. 寻路方式与效率

### 4.1 已有 Route 时

正常数据不会每帧重新寻路。节点只在固定数组中查找：

```text
Destination Node ID
    → 直连 Link（最多 4）
    → Active Route（最多 8）
    → 可选 Policy / Path（Full）
    → 下一跳 Link.send()
```

默认查找上界很小且固定，没有动态内存、全网 Dijkstra 或 Linux 服务依赖。每个中继只知道该目的地的下一跳，不需要知道完整路径。

### 4.2 首次访问未知目标

Q1 在无 Route 时使用受限 AODV-Lite：

```text
Q1进入固定 Pending
  → RREQ受限广播
  → 中间节点建立反向状态
  → 目标返回RREP
  → 沿途学习下一跳/Cost/Hop
  → 原Q1继续发送
```

关键参数：

- 并发未知目标：4；
- Pending Q1：4；
- RREQ 超时：1000 ms；
- 同目标最短 RREQ 间隔：100 ms；
- v5 RREQ 删除重复 Origin，Payload 为 `地址宽度 + 8 B`，W0～W3 整帧分别为 26/31/37/42 B；
- 当前 RREP Payload 仍为 18 B，W0～W3 基础头下整帧分别为 35/39/44/48 B；
- Q0 永不等待或主动发起 RREQ，必须预先具备直连、静态 Route、缓存 Route 或安装好的 Path。

一次冷寻路的 UCN 线字节可粗略表示为：

```text
RREQ档位帧长 × 实际扩散次数 + RREP档位帧长 × 返回跳数
```

它还没有包含 Wi-Fi/CAN/UART 自己的帧头、ACK、仲裁和重传。因此稳定链路应尽量复用 Route，不应每帧或高频定时全网寻路。

### 4.3 Route 缓存与换路

| 参数 | 默认值 | 行为 |
| --- | ---: | --- |
| 动态 Route 寿命 | 30 s | 到期后释放固定槽。 |
| 提前刷新窗口 | 6 s | 接近到期才允许候选刷新。 |
| 同目标刷新最短间隔 | 5 s | 防止重复全网 RREQ。 |
| Candidate 超时 | 3 s | 验证失败后释放。 |
| 切换改善门槛 | 20% | 候选 Cost 至少低 20%。 |
| Candidate Probe | 3 ACK | 每 100 ms 一次。 |
| Route Epoch Grace | 1 s | 切换时短期接受前一 Epoch，减少在途帧被误拒绝。 |

当前 Route 表满时不会无条件淘汰仍有效的 Route；新目的地可能得到 `UCN_ERR_NO_SPACE`，直到旧 Route 到期、失效或被产品清理。这是全对全场景下降的重要原因之一。

### 4.4 Cost 与负载均衡

- Link `route_cost` 合法范围为 `1..65534`，越小越优；`65535` 是 Unknown 保留值。
- 多跳 Cost 使用逐跳加和并饱和到 65534；Known Cost 永远优于 Unknown。
- Full 的 `AUTO_BALANCE` 只支持 Q1，默认最多 8 个 Flow。
- 同一 `(Destination, Endpoint, Q1)` 在 2 s 租期内保持 Path 亲和，避免逐帧乱序。
- 选择分数使用基础 Cost 与活动 Flow 数；Queue Pressure 达 800‰ 且连续 3 个 500 ms 样本后可触发重绑。
- Q0 不参与自动均衡，不因瞬时拥塞自动换到未经产品确认的路径。

## 5. 当前规模模拟结果

### 5.1 稀疏直连规模

Full、Tree Local、每节点每 10 ms 发送 1 条 16 B Q1：

| Nodes | Generated / Accepted / Delivered | 交付率 | Wire效率 |
| ---: | ---: | ---: | ---: |
| 8 | 800 / 800 / 800 | 100% | 31.496% |
| 64 | 6,400 / 6,400 / 6,400 | 100% | 31.281% |
| 256 | 25,600 / 25,600 / 25,600 | 100% | 31.258% |
| 1024 | 102,400 / 102,400 / 102,400 | 100% | 31.252% |
| 2048 | 204,800 / 204,800 / 204,800 | 100% | 31.251% |
| 4096 | 409,600 / 409,600 / 409,600 | 100% | 31.250% |

4096 是当前 Host Harness 主动上限下的稀疏独立 Link 场景，不代表单 Wi-Fi 信道、单 MCU 或全对全可承载 4096 Node。

### 5.2 两跳工作集

| Nodes | Generated / Accepted / Delivered | Accepted→Delivered |
| ---: | ---: | ---: |
| 8 | 800 / 800 / 800 | 100% |
| 16 | 1,600 / 1,600 / 1,100 | 68.750% |
| 64 | 6,400 / 6,400 / 1,800 | 28.125% |
| 256 | 25,600 / 25,600 / 3,900 | 15.234% |
| 1024 | 102,400 / 102,400 / 12,500 | 12.207% |

这里的下降来自同时冷启动建路、固定 Route/Discovery 工作集和共享转发节点，不是所有节点退出网络或程序崩溃。

### 5.3 全对全工作集

| Nodes | Generated / Accepted / Delivered | Generated→Delivered |
| ---: | ---: | ---: |
| 8 | 800 / 800 / 800 | 100% |
| 16 | 1,600 / 752 / 514 | 32.125% |
| 32 | 3,200 / 782 / 481 | 15.031% |

因此默认 UCN 更适合“网络中节点很多，但每个节点只和少量稳定目标通信”，不适合不调整表深度就让所有节点同时轮转访问所有目标。

完整证据：[S21测试方案](UCN_S21_极限规模模拟测试方案.md)、[S21测试结果](UCN_S21_极限规模模拟测试结果.md)、[逐节点与汇总CSV](results/S21/)。

### 5.4 V5-10 Wire Profile 单档与混档极限

Full Release 的本地高负载结果为：W0/254 Node、mixed/254 Node、W1/W2/W3 各 4096 Node 全部 100% 交付，且无重复业务、Route Loop 或 Harness 背压。W0～W3 的 32 B Payload 线效率随 17/21/26/30 B 基础头递减；mixed 的逐档结果也符合相同趋势。

两跳同时冷启动时，W0/mixed 的 254 Node 交付率为 15.354%，W1/W2/W3 的 4096 Node 都为 11.377%，不同档位的交付数完全一致，瓶颈来自默认 Route/Discovery/Pending 固定工作集而不是 Codec。丢包、重复、延迟、断链和 Q0/Q1 组合下五种布局均通过正确性门禁。完整证据见 [V5-10 单档与混档极限模拟报告](UCN_V5_10_单档与混档极限模拟报告.md) 和 [V5-10 汇总 CSV](results/V5_10/V5_10_extreme_summary.csv)。

## 6. 帧效率与多跳成本

默认最大帧 256 B，CRC 已包含在头部；静态 Payload Buffer 仍保守限制为 224 B：

| Profile | 普通头 | Route 头 | Path 头 | 16 B 明文单跳理论效率 |
| --- | ---: | ---: | ---: | ---: |
| W0 | 17 B | 18 B | 19 B | 48.485% |
| W1 | 21 B | 23 B | 25 B | 43.243% |
| W2 | 26 B | 28 B | 31 B | 38.095% |
| W3 | 30 B | 32 B | 36 B | 34.783% |

受保护业务额外携带固定 16 B Tag。自动选档在判断 MTU 时已经计入 Tag，随后才调用 Provider Seal，防止“先选档、加 Tag 后超 MTU”。

典型小帧：

- 32 B 普通明文单跳理论效率：W0 为 65.306%，W1 为 60.377%，W3 为 51.613%。
- V5-07 的 256 Node、M4、32 B Payload、80‰ 重复模拟中，固定 W3 为 50.383%，自动选档为 63.310%，两者均 100% 交付、重复 0、Loop 0、背压 0。
- 16 B动态Route帧为52 B；两跳忽略控制面时理论端到端效率约15.38%，S21八节点两跳为14.60%。

多跳会在每一跳重复发送完整UCN帧：

```text
端到端效率 ≈ Payload /（单帧长度 × 实际跳数 + 寻路/保活控制量）
```

小型周期传感器可以通过合并Payload、降低发送频率或使用一个Endpoint承载同一采样周期的多个字段提高效率；控制消息则优先保证时限和明确失败，不应只追求字节利用率。

## 7. RAM、Flash和队列占用

Windows x64、GCC 14.2、Release、Service OFF的当前静态证据：

| Profile | `sizeof(ucn_node_t)` | Core静态库`.text` |
| --- | ---: | ---: |
| Nano | 2,648 B | 19,116 B |
| Lite | 5,888 B | 57,688 B |
| Full | 9,400 B | 117,164 B |

这是 V5-07 的 Host x64 GCC 14.2 Release/Service OFF 结果；`ucn_link_t=40 B`。它只证明固定对象与代码裁剪，不等于目标 MCU ELF/栈/功耗；详见[V5-07 报告](UCN_V5_07_发布门禁与软件验证报告.md)。

特性：

- 每个MCU只承担自己的Node固定对象，不保存整个网络全部状态。
- 全网从100 Node增加到1000 Node，不会直接让每个MCU的Node RAM线性增加。
- 活跃邻居、Route和Flow超过固定表后会明确返回`NO_SPACE`，而不是继续动态分配。
- `UCN_MAX_FRAME_BYTES`会显著影响Q0/Q1/Pending/Adapter每槽缓冲大小。

默认核心与外围队列：

| 队列/表 | 默认容量 |
| --- | ---: |
| Core Q0 FIFO | 4 |
| Core Q1 Latest队列 | 4 |
| Pending Q1 | 4 |
| Adapter RX Queue | 2帧/Adapter |
| Endpoint Handler | 8 |
| Endpoint Security Policy | 8 |
| Service Binding | 6 |
| Service Remote TX Q0/Q1 | 各4 |
| Service本机Q0 Inbox | 每Binding 4 |
| Service本机Q1 Inbox | 每Binding 1个Latest槽 |

默认Adapter RX Queue的两个槽都能保存完整256 B帧，仅帧数组约512 B，另有Ingress Link指针、长度、索引和统计。Service Router/Bridge、RTOS Task栈、驱动TX/RX队列、DMA、Wi-Fi系统任务和Security Provider都在Node对象之外。

最近完整目标构建证据：

- ESP32-S3 Full/Service ON测试固件：RAM 48,124 B，Flash 600,819 B；
- ESP-WROOM-32 Full/Service ON测试固件：RAM 50,184 B，Flash 626,803 B。

它们包含Arduino、Wi-Fi/ESP-NOW、UART、日志和测试业务，不是纯UCN Core资源。Host对象也不能直接当作STM32/ESP32 ABI数据。

## 8. 保活、故障和调度参数

| 参数 | 默认值 | 说明 |
| --- | ---: | --- |
| Protocol Step最大间隔 | 10 ms | 唯一Protocol Task必须周期调用。 |
| 业务维护Burst | 4帧 | 连续业务后给到期保活/Probe/Refresh一个维护机会。 |
| 保守维护上界 | 800 ms | 由Burst、Step、Neighbor、Bearer上限计算。 |
| Heartbeat | 1 s | 仅对已准入直连Neighbor，不全网转发。 |
| Neighbor Suspect | 3 s | 最后有效帧后进入可疑。 |
| Neighbor Remove | 4 s | 回收Neighbor并清相关动态状态。 |
| Link质量采样 | 500 ms | Core缓存通用质量快照。 |
| Bearer稳定样本 | 3次 | 候选需要持续明显更优。 |
| Bearer Probe | 2/最多3次ACK | 防止只凭瞬时Cost切换。 |
| Q0背压重试 | 最多3次、间隔5 ms | 仅产品显式开启且只处理`NO_SPACE`；不是端到端可靠重传。 |

Driver明确报告Link Down时可立即处理，不必等4秒。Heartbeat也不是唯一活性证据：来自该Neighbor的合法业务或控制帧同样刷新`last_seen`。

## 9. Endpoint、Service和安全边界

- 静态Endpoint编号范围为`0x40..0xBF`，共有128个协议编号；默认每Node同时注册8个Handler。
- Endpoint直接复用`message_type`，正常业务帧不额外增加Endpoint字段。
- 当前Core只真正发送Q0/Q1；Q2/Q3枚举存在但发送API返回不支持。
- Q0是有界关键FIFO；Q1偏向实时Latest Value。
- `UCN_OK`只说明本机对应层接受了副本，不自动表示远端Inbox或执行成功；远端结果需要业务Result Endpoint。
- 受保护帧增加16 B E2E Tag；加密算法、密钥、Session持久化和生产Replay Window由Security Provider负责。

## 10. 当前已知限制

### 10.1 重复抑制当前边界

S21 的旧 8 项逐帧环曾在 256 Node、80‰重复、M4 下产生 1070 次重复业务投递。S22 已改为按 `(Source, Session)+highest_sequence+bitmap` 的固定滑动窗口，并把 RREQ Best Cost 状态拆出；同一 M1/M2/M4 场景当前均为 100% 原始业务交付且重复业务为 0。

这不等于无限容量或生产安全 Replay：

- Nano/Lite/Full 默认只同时维护 4/16/32 个 Source/Session 窗口；
- 全部窗口被活跃来源占用时明确返回`UCN_ERR_NO_SPACE`；
- `Session=0` 的对端重启并复用 Sequence，在窗口过期前无法与延迟副本区分；
- 易失位图不认证来源、不持久化，也不替代 Security Provider Replay Window或业务 Command Replay。

完整实现与验证见[S22 稳定化修复报告](UCN_S22_重复抑制与稳定化修复报告.md)。

### 10.2 尚未取得的证据

- 三板以上ESP32真实多跳、最大节点数和同时入网风暴；
- Wi-Fi共享空口下多路径并行吞吐；
- UART/CAN/Wi-Fi混合Adapter的真实WCET、队列与仲裁；
- 真实物理断链的P50/P95、丢失和乱序；
- 各Profile在目标MCU上的纯Core RAM/Flash、Task栈、CPU、功耗和温升；
- 生产身份、AEAD、密钥轮换与防重放审计。

## 11. 如何选择实际产品容量

产品不应直接照抄默认表。建议按以下顺序冻结：

1. 选择Nano/Lite/Full；需要无Linux自动组网时至少选择Lite。
2. 列出每Node最大直连对端、每对端Bearer数和活跃远端目的地数。
3. 将网络直径控制在产品`default_hop_limit`内，通常先从4～8跳实测，不直接使用16跳作为目标。
4. 根据RAM缩放Frame、Q0/Q1、Pending、Neighbor、Route、Path、Flow和Adapter队列。
5. 使用S21工具做对应拓扑/负载模拟。
6. 在真实目标板记录ELF/Map、栈高水位、最低Heap、CPU、功耗、入网/断链时间和业务P50/P95/Max。
7. 只有硬件门禁通过后，才将“最多节点数、吞吐和时延”写成产品规格。

## 12. 当前推荐的对外规格表述

在S22和多板实机验收完成前，推荐使用以下表述：

> UCN v4是面向MCU的固定资源自组网Core。协议采用32位Node ID，默认每Node支持4个逻辑Link、8个Neighbor状态、每Neighbor最多2个Bearer、8条远端Route、4个并发寻路、8个Endpoint Handler和最多16跳。Host模拟已验证4096 Node稀疏直连运行；默认配置面向少量活跃目的地，不承诺大规模全对全通信。真实无线/有线多板节点上限、吞吐、时延和资源以目标产品实测为准。
