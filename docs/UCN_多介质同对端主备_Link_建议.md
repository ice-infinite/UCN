# UCN 多介质同对端主备 Link 建议

> 状态：**T21.1～T21.4 已完成 C99/虚拟闭环；T21.6.1 已完成两块 ESP32-S3 的真实独立双 Link 接入与准入/恢复控制实验**；目标板物理拔线时延、持续负载和三板多跳仍待实施。
> 日期：2026-08-09
> 适用范围：同一对 UCN 节点同时拥有 WiFi/ESP-NOW、UART/RS485、CAN-FD、BLE、LoRa 等两个或更多物理 Bearer 时的固定资源主备；保持 MCU-first、无动态内存、应用不感知介质。

## 1. 为什么需要这一层

跨介质转发与“同一对节点多条直连”是两个不同问题：

```text
跨介质路径：A -- WiFi -- B -- UART -- C -- CAN-FD -- D
同对端主备：A -- WiFi -- B
                    └ UART -- B
```

当前 v4 已能把每种介质抽象为 `Link`，在多跳 RREQ 中累计统一 `route_cost`，使业务可以跨 WiFi、UART、CAN-FD 等不同 Link 转发。但同一对节点 A/B 的多条**自动准入** Link 尚未被建模为一个可切换的邻居集合。

| 当前事实 | 含义 |
| --- | --- |
| 同一 Node ID 通过多条 `HELLO` Link 出现时，Core 将它们绑定进同一个固定 Bearer 集。 | 默认每 Neighbor 两条；第三条返回 `UCN_ERR_NO_SPACE`，不会占用第二个 Neighbor 身份槽。 |
| Join Provider 会对每条新 Bearer 调用一次。 | 第二条物理链路不能借第一条已入网身份绕过 ACL。 |
| 当前 Primary 在健康时保持；明显更优的 Backup 需连续采样和一跳 ACK 才切换。 | 已有受控质量切换，不把单次 Cost 波动写成“自动最优”或“无缝”。 |
| 每个远端目的地的正常业务只有一条 Active 路径；可保留 Candidate 与 Previous/grace。 | 当前不做业务复制、负载均衡或帧级多链路聚合。 |
| Core 只使用 Adapter 提供的通用 `route_cost`。 | WiFi RSSI、UART 超时、CAN Bus-Off 等不得泄漏到路由 Core。 |

因此 T21 的目标不是“把所有帧同时发三份”，而是让一个 Node ID 在固定上限内拥有多个可验证 Bearer，并在**不改变业务 API、不过度占用 RAM、不过度抖动**的前提下选择主链和备链。

## 2. 目标与明确边界

### 2.1 目标

1. 一个逻辑 Neighbor（一个 Node ID）最多拥有固定数量的 Bearer Link；建议默认 `UCN_MAX_BEARERS_PER_NEIGHBOR=2`，可编译为 1 以完全关闭该能力。
2. 每条 Bearer 独立维护 Link 状态、最近有效帧、心跳、平滑 Cost 和错误计数。
3. 正常业务只选一条 Primary Bearer；Backup 保持已验证但不复制普通业务。
4. Primary 明确 Down 时，下一帧可立即改走健康 Backup；质量恶化时采用滞回/短 Probe，避免 WiFi 与 UART/CAN 来回抖动。
5. 仅当一个 Neighbor 的全部 Bearer 都不可用时，才进入现有 `SUSPECT → REMOVED`、清路由和 RERR 恢复流程。
6. 应用仍只提供目标 Node ID、Endpoint、QoS 和 Payload；不得要求应用选择 WiFi/UART/CAN。

### 2.2 不在 T21 内

- 不做逐帧复制、帧级条带化、负载均衡或带宽聚合。
- 不把 WiFi 的 RSSI、CAN 的 Bus-Off 等介质字段加入 UCN 业务帧。
- 不把经典 CAN 8 B 的 Carrier 分段混入本任务；经典 CAN 仍归 T16。
- 不替代现有“不同中继的多跳 Candidate 路径”机制；T21 解决的是**同一下一跳 Node ID**的 Bearer 选择。
- 不因多介质引入 Linux、中心协调器或动态内存。

## 3. 建议的固定资源模型

建议把“Node 身份”和“物理承载”拆开，而不是让一个 Neighbor 永远只持有一个 `link`：

```text
Neighbor（Node ID = B）
 ├─ identity / ACL / Neighbor state
 ├─ primary_bearer_id
 ├─ bearer[0] = WiFi Link，Cost、状态、last_seen
 └─ bearer[1] = UART Link，Cost、状态、last_seen
```

| 对象 | 建议保存的信息 | 资源规则 |
| --- | --- | --- |
| Neighbor | Node ID、授权状态、当前 Primary、Bearer 数量。 | 每 Node ID 一项；不重复保存身份。 |
| Bearer | `ucn_link_t *`、本地 Bearer ID、健康状态、`last_seen`、平滑 `route_cost`、错误/切换计数。 | 固定数组；最大数由编译期宏限制。 |
| Route | 目标、下一跳 Node ID、当前 egress Bearer、Cost、Hop、Epoch/Previous。 | 仍只保存一个业务出口；不扩展为多份业务副本。 |

`UCN_MAX_LINKS` 仍是节点总物理 Link 上限，`UCN_MAX_BEARERS_PER_NEIGHBOR` 只是其中同一 Node ID 可占用的子上限。产品必须同时核算两者，不能只增大 Bearer 数而忽略总 Link、队列和 RX 缓冲。

## 4. 建议的行为流程

### 4.1 发现与准入

1. Adapter 为每种物理端点建立 Candidate Link：例如 WiFi MAC 与 UART 端口各有一个 Link。
2. 两条 Link 都通过 `HELLO` 得到同一 Node ID B。
3. Core 不再因为“B 已存在且 Link 不同”直接拒绝，而是要求 Security Provider/ACL 在**每条 Bearer**上确认对端都是同一合法身份。
4. 身份、Network ID、策略和容量均通过时，将新 Link 加入 B 的 Bearer 数组；满时拒绝新 Bearer，不挤掉当前业务链。

不能只因 MAC、CAN ID、串口号相同就绑定为同一身份；这些只是 Adapter 地址，生产网必须由 Provider 证明两条 Bearer 的对端确为同一 Node ID/权限实体。

### 4.2 正常发送与切换

```text
发送给 B / 经 B 转发
        ↓
Primary Bearer 健康？
        ├─ 是：只经 Primary 发送
        └─ 否：存在已验证 Backup？
                 ├─ 是：从下一帧开始切到 Backup
                 └─ 否：按现有 Link Down / RERR / 寻路流程处理
```

- **硬故障切换**：Driver Down、Bus-Off、明确发送失败等事件出现时，立即禁止故障 Bearer；若同一 Neighbor 还有健康 Backup，不移除 Neighbor、不清经该 Neighbor 的整条逻辑路径，下一帧改走 Backup。
- **质量切换**：当前默认 Candidate Bearer 必须比 Primary 低至少 20%，连续 3 个 500 ms 采样窗口成立，再在候选 Bearer 上得到 2 次一跳 `HEARTBEAT` ACK（最多 3 次、间隔 100 ms）才切换。宏均可按产品 Profile 覆盖；采样的 Cost 仍由 Adapter 归一并可自行平滑。
- **切换期**：已经交给旧驱动的帧不撤回；Q1 应带业务序号/时间戳并允许少量旧帧/乱序，Q0 必须预建路径或使用本地失效安全。
- **心跳**：每条 Bearer 独立一跳 Heartbeat。一个 Bearer 的心跳失败不应使整个 Neighbor 离网，只改变该 Bearer 健康状态；全部 Bearer 失效才触发现有 Neighbor 移除。

### 4.3 与多跳路由的关系

T21 与现有 Candidate 路由分两层工作：

| 层级 | 解决的问题 | 示例 |
| --- | --- | --- |
| Bearer 选择（T21） | 到同一个下一跳 B 时，使用 B 的 WiFi 还是 UART。 | `A -- WiFi/UART -- B`。 |
| Route 选择（现有 T17/v4） | 到远端 C 时，经 B 还是经 D。 | `A→B→C` 与 `A→D→C`。 |

推荐顺序是“先为每个下一跳选择健康 Bearer，再比较不同下一跳组成的路径 Cost”。RREQ 继续对所有可用 Link 受限扩散；同一下一跳的多 Bearer 不应被当成多个独立身份或无限泛洪分支。

当前“直连 Link 优先于多跳 Route”的规则保持不变：若 A 直连 C，则先在 A→C 的 Bearer 集内选择 Primary，而不因某条远端多跳路径 Cost 更低就自动绕行。是否允许“较差直连让位给显著更优多跳”应作为独立策略项，不能在 T21 中隐式改变。

## 5. Cost 与介质 Adapter 的职责

Core 不判断哪种介质“天生更好”。每个 Adapter 在后台采样、平滑、防抖后给出相同语义的 Cost：数值越小，表示该 Bearer 当前越适合作为路由出口。

| Bearer | 可输入 Cost 的信息 | 不应直接写入 Core |
| --- | --- | --- |
| WiFi / ESP-NOW | RSSI、重传、发送回调失败、RX 丢帧、队列压力。 | 原始 RSSI 阈值和 WiFi 专有状态。 |
| UART / RS485 | CRC/帧错误、超时、半双工等待、队列压力。 | 串口号、波特率寄存器细节。 |
| CAN / CAN-FD | Bus-Off、错误帧、仲裁等待、总线负载。 | CAN 控制器寄存器和特定错误码。 |

未知或暂时无法测量的 Bearer 仍应使用保守 Cost，不能因为“尚未测量”压过已验证的健康链路。不同介质的 Cost 量纲、权重、采样窗口和切换门限必须在目标板实机上标定，不能直接用 WiFi RSSI 推导 UART/CAN 的优先级。

## 6. 安全与故障边界

- 每条 Bearer 都要经过 Network ID、身份、ACL 和安全策略校验；禁止用“第二条 Link”绕过首条 Link 的入网授权。
- 端到端受保护 Payload 的目标身份不因 Bearer 切换而变化；中继仍可按现有透明转发规则处理密文。
- Bearer 切换不是可靠传输。需要一次执行语义的未来 Q2 命令仍需 ACK、请求号和去重；普通 Q1 传感器流应按最新值/时间戳处理。
- Adapter 必须把 Link Down 与瞬态发送失败区分开，避免一次 WiFi 回调失败就错误移除健康 Neighbor。

## 7. T21 实施与验证门禁

| 子任务 | 实现内容 | 单元测试 | 虚拟/实机验收 |
| --- | --- | --- | --- |
| T21.1 | 将 Neighbor 从“单 Link”扩展为固定 Bearer 集；新增编译期上限和容量拒绝。 | 同 Node ID 的两 Link 准入、第三条超限、Provider 第二 Bearer 拒绝。 | **已完成（C99）**：无动态内存，`Bearer=1` Profile CTest 仍全绿。 |
| T21.2 | 每 Bearer 的 Heartbeat、状态、`last_seen` 与健康统计；全部失效才移除 Neighbor。 | 单 Bearer Down 不移除、两 Bearer 均 Down 后移除、单 Link SUSPECT 恢复。 | **已完成（C99）**：真实 WiFi/UART 拔链仍待验收。 |
| T21.3 | Primary/Backup 选择、硬故障立即切换、Cost 滞回和固定 Probe。 | 覆盖主/备初选、硬 Down 下一帧切备、5% 抖动保持、Probe ACK 丢失保持，以及成功 2 ACK 后切换和统计。 | **已完成（C99/虚拟）**：Probe 复用一跳 Heartbeat，不新增线格式；真实切换时延、丢失/乱序与功耗待实测。 |
| T21.4 | 与 Route/Candidate/RERR 的联动。 | A→B→C 覆盖 Active/Candidate 的 Probe/Activate 经 Backup、下游 RERR 经 Backup 清理动态状态、全 Down 回收和静态 Route 保留边界。 | **已完成（C99/虚拟）**：不新增线格式；真实多板断链收敛待验收。 |
| T21.5 | 完整 C99 虚拟 Link 回归、64 B Profile、资源报告。 | 原有单 Link、路由、安全、Endpoint 测试不回退。 | **进行中**：四个 CTest Profile 已绿；静态资源与实机空闲 Heap/栈已有记录，峰值/功耗仍待补。 |
| T21.6 | 实际 Adapter 与多板测试。 | Adapter 地址/身份绑定、队列满、Down 事件。 | **进行中**：两块 S3 已确认 ESP-NOW Peer Link + UART Link 各自进入 Core、合并为一个 Neighbor 的两个 Bearer；远端 Wi-Fi-only 控制实验保持 Neighbor 并在恢复后自动重新合并。物理拔线、P50/P95、丢失/乱序、功耗与三板多跳待测。 |

实施前必须先冻结：目标板每节点最大物理 Link 数、同对端 Bearer 上限、可接受切换时间、Q0/Q1 行为、介质组合、生产身份绑定方式和 RAM 预算。未通过 T21.5/T21.6 前，不得把该建议表述为已实现的链路聚合或无缝冗余。

## 8. 当前代码与验证证据（2026-08-09）

- `include/ucn/ucn_neighbor.h` 新增 `UCN_MAX_BEARERS_PER_NEIGHBOR`（默认 2）、`ucn_neighbor_bearer_t` 与 `primary_bearer_index`；只增加固定数组，不引入堆或动态表。
- `src/ucn_node.c` 以 `(Neighbor, Bearer)` 维护自动准入、Provider 授权、Heartbeat、`last_seen`、Down、移除和 egress 解析。Active/Previous Route 与 Candidate 在业务、Probe、Activate 或中继转发时，都会将学习时保存的物理 egress Link 解析为同一 Neighbor 当前 Primary。单 Bearer Down 时仍保留同一 Node ID 的健康 Bearer；全部 Down 才回收 Neighbor、注册 Link 和相关动态 Route/Candidate。
- 正常业务只走 Primary。首次建立时从健康、已准入 Bearer 中选较低 Cost；Primary 保持健康时不因瞬时 Cost 改变。Driver 明确 Down 后，下一次常规选路改用健康 Backup；质量切换只有满足滞回和 Probe ACK 后才发生。
- 为 Bearer 增加固定的质量采样/Probe 状态和三个统计：Probe 已发送、匹配 ACK、质量切换次数。Probe 直接复用现有 8 B、一跳 `HEARTBEAT` request/ACK，按候选 Link 定向收发；中继不参与，业务帧和线格式不增加字段。`ucn_node_step()` 仍先处理 Q0/Q1 待发业务，空闲时才处理 Heartbeat/质量 Probe，因此 Probe 不抢占 Q0。
- `tests/test_neighbor_bearer.c` 现覆盖双 Bearer 自动入网、容量拒绝、逐 Bearer Provider 拒绝、Primary Down 后经 Backup 收发、全 Down 后回收、5% 抖动保持、3 次 ACK 丢失保持、恢复后 2 ACK 切换，以及 A→B→C 的动态 Route/Candidate Probe/Activate、下游 RERR 回传和静态 Route 边界。`build`、`build-release`、`build-mtu64` 和 `build-bearer1` 的 CTest 均通过。
- ESP32 测试工程的默认诊断 Profile 已把 ESP-NOW Peer Slot Link 与 UART Link（ID `0x70`）分别排入 Core；`DualMediaLink` 仅保留为 `legacy_dual` 对照。两块 S3 同时复位后均记录到 `links=2`、`admitted=1`、`count=2`，UART Cost 5 为 Primary、ESP-NOW Cost 10 为 Backup，空闲 Queue 丢弃为 0。B 临时刷同 Node ID 的 Wi-Fi-only 镜像时，A 保留 `admitted=1/count=1` 且 Ping 持续；恢复 B 独立镜像后 A 回到 `count=2`。
- 未完成：Adapter 端 Cost 的 EWMA/RTT/拥塞映射和实机标定、**物理** ESP-NOW+UART/CAN-FD 拔链切换 P50/P95、持续业务丢失/乱序、功耗、Wi-Fi 反向故障和三板 RERR。当前不承诺业务零丢包、零乱序或“无缝”切换。
