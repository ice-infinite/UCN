# UCN 整体架构设计：MCU 自组网优先

> 状态：**UCN v4 C99 Core 源码快照（2026-08-08）**；虚拟拓扑已验证，真实 Adapter/生产密码库/实机指标未验证。  
> 日期：2026-08-04  
> 目标：以 MCU 为主体完成安全自组网；Linux 仅作为兼容接入端；协议小而可裁剪；资源占用由目标硬件配置决定。

## 1. 架构结论

UCN 是一个可移植的 C 通信协议栈。它的最小运行形态不是 Linux 守护进程，而是运行在 MCU 上的 `UCN-Core`：多个 MCU 即使没有 Linux，也能完成邻居发现、有限多跳和消息转发。生产认证入网、统一失联事件与本地安全动作是 MCU-first 的架构目标，当前仍需 T15 和实机产品逻辑完成。

Linux、ROS2、PX4/MAVLink、地面站与 AI 系统可以通过 `UCN-Host` 接入同一网络，但它们不拥有路由中心、入网中心或控制中心的地位，更不替代 Linux 自己已有的网络体系。

当前代码已实现 **v4 32/36 B 帧头、Route Epoch/grace、受限 AODV-Lite、Candidate Probe/Activate、Endpoint Q1 首包自动寻路、固定邻居表、最小 HELLO、通用 Link Cost、按 Node/Endpoint 配置的端到端受保护帧与透明中继，以及按需路径追踪和节点快照诊断**。真实 `JOIN_*` 状态机、经审计 AEAD/身份库、真实无线驱动和小 MTU Carrier 分段仍是后续任务；它们不能被虚拟测试替代。

这份架构只围绕四项约束设计：

| 方向 | 架构结论 |
| --- | --- |
| 兼容 Linux 现有体系 | 以 Host Adapter 方式接入 UDP、Ethernet、SocketCAN、ROS2/MAVLink；不重写或替代 Linux 网络栈。 |
| 无 Linux 也能自组网 | `UCN-Core` 在 MCU 上独立完成安全、邻居、AODV-Lite 路由和转发。 |
| 足够简洁 | Core 只保留自组网闭环；服务目录、分片、时间同步、大数据和桥接全部是可选模块。 |
| MCU 资源可控 | 无堆内存、无无限表项、无隐藏后台线程；当前由公共头文件中的编译期宏或构建参数配置，目标板专用 `ucn_config.h` 尚未建立。 |

当系统需要接入 ROS2、PX4/MAVLink、地面站或 AI 时，采用“上层应用 → 受控 Bridge → UCN-Host → MCU Mesh”的关系；详细的系统级边界见 [UCN × ROS2 协同整体架构](UCN_ROS2协同整体架构.md)。

## 2. 系统边界

UCN 负责的是“不同 MCU 节点如何安全地组成一个网络并通信”。它不负责下列事情：

- 不实现 WiFi MAC、CAN 控制器驱动、TCP/IP 或 802.11s。
- 不实现电机 FOC、PWM、电流环、姿态内环等本地硬实时闭环。
- 不替代 ROS2、DDS、MAVLink、PX4、CANopen 或 Linux 网络栈。
- 不要求网络必须存在 Linux、云端、地面站或固定网关。

底层已有的 WiFi、CAN-FD、RS485、UART 或厂商 Mesh 能力只要能收发字节，就可以作为 UCN Link。UCN 在其上增加统一身份、路由、QoS 和安全规则。

## 3. 总体架构

```mermaid
flowchart TB
    subgraph MCU_NET["MCU 自组网：协议默认运行位置"]
        APP["飞控 / 电机 / 传感器 / MCU 应用"]
        API["当前 C API<br/>ucn_node_* / ucn_adapter_*"]
        subgraph CORE["UCN-Core"]
            PKT["Packet<br/>短帧、地址、序号、TTL"]
            SEC["Identity & Security Boundary<br/>HELLO、Provider、会话/防重放接口"]
            NBR["Neighbor<br/>可信邻居、Link 状态"]
            ROUTE["Route<br/>AODV-Lite、下一跳、修复"]
            QOS["Core QoS<br/>Q0/Q1、截止时间、过期丢弃"]
            HEALTH["Health<br/>Heartbeat、邻居/路由/候选老化"]
        end
        LINK["MCU Link<br/>无线 / CAN-FD / RS485 / UART"]
        APP --> API --> CORE --> LINK
    end

    EXT["UCN-Extended（按需）<br/>服务目录 / Q2-Q3 / 分片 / 时间同步"]
    HOST["UCN-Host（可选）<br/>Linux / ROS2 / MAVLink / 地面站"]

    CORE -. "编译期开关" .-> EXT
    LINK <-- "同一 UCN 线协议" --> HOST
```

`UCN-Core` 不依赖 `Extended` 或 `Host`。因此图中的 Host 不存在时，MCU 网络仍是完整网络。

## 4. UCN-Core：所有 MCU 必须具备的最小闭环

### 4.1 模块职责与当前实现状态

| 模块 | 当前已实现 | 明确尚未实现 |
| --- | --- | --- |
| Packet | v4 基础 32 B 头或 Route Extension 36 B 头；长度/CRC/Network ID/TTL/Flag 校验；v3 显式拒绝。受保护业务额外固定 16 B Tag，CRC 覆盖密文和 Tag。 | 跨 Link 分片。 |
| Identity & Join | 最小 `HELLO`、Candidate/Admitted/Suspect/Removed/Rejected/Expired 邻居表、`Manual`/`Open`/`Provider` 准入。 | `JOIN_REQ`/挑战/接受状态机、出厂身份格式。 |
| Session & Replay | 可注入 Provider 的会话 ID、发送序号持久化、TX/RX 授权、固定 `seal/open`、26 B 不可变 AAD、明文/受保护策略和入站去重缓存。 | 生产 AEAD、密钥轮换、完整重放窗口与产品 ACL 表。 |
| Route / Forwarding | 固定 Active/Candidate 表、受限 AODV-Lite、RREQ/RREP/RERR、`PATH_PROBE/ACK`、携带 Candidate ID + Epoch 的 `PATH_ACTIVATE/ACK`、TTL、断链清表、保守 Link Cost；业务按 `(destination, route_epoch)` 区分 Current/Previous，默认 1 s grace。 | 自动业务重发、全网拓扑。 |
| Core QoS | Q0/Q1 固定队列、deadline、`best_effort`、`latest_value`；Endpoint Q1 首包固定 4 槽/1 s 等待并合并同 `(destination, Endpoint)`。 | Q2/Q3、可靠确认、分片。 |
| Health | 8 B 一跳 `HEARTBEAT` 请求/ACK、业务帧刷新存活、`ADMITTED → SUSPECT → REMOVED`、路由/Link 槽回收。 | `LINK_STATE` 线协议消息、对应用的统一失联事件、介质专用 Profile 实机标定。 |

### 4.2 报文类型与处理状态

| 类别 | 报文 | 当前处理状态 |
| --- | --- | --- |
| 邻居与入网 | `HELLO` | 已处理：只在一跳 Link 上绑定 Node ID 并执行准入；不转发、不交给应用。 |
| 邻居与入网 | `JOIN_REQ`、`JOIN_CHALLENGE`、`JOIN_ACCEPT` | 已定义枚举值，尚无 Core 状态机；生产入网属于 T15。 |
| 路由 | `ROUTE_REQ`、`ROUTE_REPLY`、`ROUTE_ERROR` | 已处理：受限发现、回程建表、失效清理；非 Candidate RREP 携带 Route Epoch。 |
| 健康 | `HEARTBEAT` | 已处理：一跳 8 B 请求/ACK、认证后刷新存活、SUSPECT/REMOVED 回收；`LINK_STATE` 尚未定义为线协议消息。 |
| 业务 | `DATA_Q0`、`DATA_Q1`、静态 Endpoint `0x40..0xBF` | 已处理：单跳/多跳转发与 Q0/Q1 API；Endpoint 可配置明文/受保护/接收/透明转发策略。 |

Core 只需支持：单播、受限广播、有限跳转发和小尺寸应用负载。组播、服务调用、文件和大包分片不属于 MCU 自组网成立条件。

### 4.3 MCU 独立自组网流程

```text
启动
  ↓
读取设备身份、网络配置、持久化计数器
  ↓
Adapter 发现物理端点，建立 Candidate Link（Node ID 未知）
  ↓
物理广播 HELLO；Core 绑定 Node ID
  ↓
按 Manual / Open / Provider 策略准入；生产网由 Provider 认证/授权
  ↓
写入可信 Neighbor Table
  ↓
已有路由：直接下一跳发送
未知路由：限速 ROUTE_REQ → ROUTE_REPLY → Route Cache
  ↓
链路失效：ROUTE_ERROR → 清理缓存 → 允许的业务重新寻路
  ↓
无安全路径：通知本地应用，由飞控/执行器执行失联安全动作
```

Coordinator 是可由 MCU 承担的**授权角色**：它批准加入、保存网络策略、可选提供时间源；正常业务转发不应强制经过它。Coordinator 暂时离线时，已有会话和缓存路径可继续使用到各自有效期结束。

当前 Core 的 `HELLO` 只是链路本地发现与 Node ID 绑定：它不转发、不交给业务层，也不会把 MAC/CAN ID 当作身份。`Open` 仅用于实验网络；生产设备必须配置安全 Provider，并由 Provider 验证 HELLO/后续 JOIN 的真实身份与 ACL。

### 4.3.1 控制帧的送达范围：不能把 HELLO、Heartbeat 与寻路混为一谈

UCN 不会为所有控制帧做全网广播。每类帧的范围由目的地址、`hop_limit` 和当前 Link 决定：

| 帧 | 当前发送范围 | 会不会被中继转发 | 用途 |
| --- | --- | --- | --- |
| `HELLO` | Discovery Link 上的一跳物理广播，`hop_limit=1`。处于同一无线/总线一跳范围的候选节点可以接收。 | 不会。 | 发现物理邻居、绑定 Node ID、执行准入。Core 只提供发送 API；周期由 Adapter/Profile 决定。当前 ESP-NOW 测试 Profile 每秒调用一次。 |
| `HEARTBEAT` | 已准入的**一个直连邻居**。请求和 ACK 都在该邻居对应的单播 Link 上发送，`hop_limit=1`。 | 不会。 | 只证明这一跳 Neighbor/Link 仍可用，并刷新该邻居 `last_seen`。 |
| `ROUTE_REQ` | 在受 Hop Limit、去重和 Token 约束下向本地可用邻居扩散。 | 会。 | 仅在未知路径或受限候选刷新时寻找多跳路径。 |
| 业务帧 | 只交给当前选定的下一跳 Link。 | 仅路径上的中继会转发。 | 交付到目标 Node 的 Endpoint/业务回调。 |

例如 `A → B → C` 是一条业务路径时，保活关系实际是两组独立的一跳会话：

```text
A ── HEARTBEAT / ACK ── B ── HEARTBEAT / ACK ── C
A ── 业务帧 ───────────► B ── 转发业务帧 ───────► C
```

B 会收到并处理 A 发给 B 的心跳，也会独立地与 C 保活；但 B **不会**把 A 的心跳转发给 C，C 收不到那一帧 A→B 心跳。换言之，Heartbeat 不能证明 A 到 C 的端到端路径可达；多跳健康仍依赖业务结果、Link Down/RERR、路由刷新和 Candidate Probe 等机制。

### 4.3.2 两条路径的并行性与共享资源边界

UCN 没有全网总发送队列，也没有要求一条路径传完后另一条才可开始。彼此不共享节点的两条路径由各节点自己的协议任务、路由表和发送队列独立推进，属于**逻辑并行**。

但 MCU 网络不是“任何条件下都物理并行”，必须区分以下三层边界：

| 场景 | 当前行为 |
| --- | --- |
| `A→B→C` 与 `D→E→F` 不共享节点、且底层介质独立 | 两条路径可独立收发；不存在 UCN Core 的全局互斥。 |
| 两条路径共享中继 B | B 的协议任务按帧处理接收和转发；本地 Q0 队列优先于 Q1，因此共享 B 时会出现本地排队。 |
| 多块 ESP-NOW 板共用同一 WiFi 信道 | 业务可同时被不同节点提交，但无线空口和每块板的无线电仍是共享资源；实际发送会竞争、退避或重试，高负载时表现为时延上升、队列积压或丢帧，而不是全网协议锁。 |

当前 `ucn_node_step()` 每次只取一个本地 Q0 或 Q1 待发项，Q0 优先；只有业务队列暂时为空时，才依次处理等待路由的 Q1、到期 Heartbeat、Probe 和路由刷新。因而持续满载时 Heartbeat 可能延后；不过来自该邻居的任意有效业务或控制帧都会刷新 `last_seen`，不会只依赖单独的 Heartbeat。实际 ESP-NOW 的并发上限、空口占用和队列深度必须由 T14 实机压力测试测量，不能由该逻辑规则推导为吞吐承诺。

### 4.4 AODV-Lite 的限制

为了保持 MCU 资源可控，路由只采用一套按需、受限的 AODV-Lite：

- 每节点只维护固定数量的邻居、路由、重复请求和等待请求。
- `ROUTE_REQ` 带请求 ID、Hop Limit、累计 `route_cost` 与过期时间；重复请求默认抑制，但累计代价更低的副本可以替换较高代价副本。
- `ROUTE_REPLY` 只建立“目标 → 下一跳”的缓存，不构建完整全网拓扑图；同一目标优先保留较低 `route_cost` 的动态路径。
- `ROUTE_ERROR` 只影响相关缓存路径，不触发无限全网泛洪。
- Q0 不得等待路由发现；Q0 只能使用直连、预建路径或本地安全动作。
- `ucn_node_send_endpoint()` 的 Q1 在未知路径时自动发起一次受限 RREQ，并把最新值放入固定等待槽；旧 `ucn_node_send()` 保持显式寻路兼容。

等待槽仅服务 Endpoint Q1，默认 `UCN_PENDING_Q1_DEPTH=4`、`UCN_PENDING_Q1_TIMEOUT_MS=1000`，同目的同 Endpoint 覆盖旧值；Q0 永不进入等待槽。这保持了 RAM、时延和 RREQ 数量的上限。

### 4.4.1 后续的策略路由与可选负载均衡（T22，未实现）

当前 Active/Candidate 解决的是“同一目标 Node 的自动换路”，不是“不同业务固定走不同路径”或“多路径同时分担”。后续 T22 将在不配置时保持 `AUTO_BEST`，并按 `(destination, Endpoint)` 提供可选的 `PINNED_STRICT`、`PINNED_FAILOVER` 与 `AUTO_BALANCE`：

- `PINNED_FAILOVER` 使业务正常固定在指定 Primary Path；只有 Driver Down、明确发送失败、RERR 或已验证失联等硬故障才切到 Backup 或按策略寻路，Cost 的短时优劣不触发切换。
- `AUTO_BALANCE` 默认关闭，只让通过验证的 Q1 Path 按**业务流**而非按帧分散，避免乱序和抖动；Q0 初版不参与自动均衡。
- 真正的端到端指定路径需要短小 `Path ID` 和受认证的路径安装/撤销控制面，中继仅按 Path ID 转发。现有 `route_epoch` 只用于 Current/Previous 切换，不能充当 Path ID。

该层与 T21 的“到同一下一跳时选择 WiFi/UART/CAN Bearer”不同：T21 处理一跳承载冗余，T22 处理经不同中继的端到端路径策略。完整边界、API 原则、帧兼容要求和测试门禁见[策略路由与可选负载均衡建议](UCN_策略路由与可选负载均衡建议.md)。

### 4.4.2 按需路径追踪（T23，Core 已实现）

应用可调用 `ucn_node_request_path_trace()` 查询**该次** Trace 实际走过的 Node ID。源节点先写入自身，沿途节点追加自身，目标经固定容量 Reverse 表反向回送 Reply；中继不保存完整路径，也不需要目标到源的普通 Route Cache。

- Trace 只使用当前直连/Route Cache，找不到路径立即返回，不触发 RREQ；它不锁定或改变后续业务的选路。
- `PATH_TRACE_REQ/REPLY` 使用 `DIAGNOSTIC=0x04`、Q1 和独立低频 Token；不占 Q0 控制预算，不进入普通 Q0/Q1 队列。
- 每个 Node 都以固定 Pending/Reverse 槽和超时限制状态；结果含 `OK`、`NO_ROUTE`、`TTL_EXCEEDED`、`TRUNCATED` 或源端本地 `TIMEOUT`。
- Trace 是拓扑诊断，控制帧不允许 E2E 保护；生产是否开放该能力仍由身份 Provider/产品 ACL 决定。所有路径中继必须支持该 Flag。

线格式、记录上限和实机门禁见[路径追踪诊断建议](UCN_路径追踪诊断建议.md)。它与 T22 的业务 `Path ID` 无关，不能作为指定业务路径或负载均衡机制。

### 4.4.3 按需节点快照（T24，Core 已实现）

被授权的管理节点可调用 `ucn_node_request_node_snapshot()`，通过受限广播收集当前连通域内回复的 Node ID 和直接 Link 数量。请求只首次泛洪；每个中继只保存短期 `(origin, query_id, ingress Link)` Reverse 项，Reply 沿该项回源。因此它不要求全网 Route Cache，也不让每个 MCU 常驻完整拓扑图。

- Snapshot 使用 `NODE_SNAPSHOT_REQ/REPLY`、`DIAGNOSTIC=0x04`、Q1 与独立的“突发 1、10 s 补 1”Token；正常 Q0/Q1、Heartbeat 和 Path Probe 优先。
- 远端默认拒绝，必须经 `ucn_node_set_node_snapshot_authorizer()` 显式允许管理 Node；既有 Security Provider 的 RX 授权仍先执行。
- 源端仅用固定结果数组收集一次窗口内的唯一回复；没有回复不等于永久离网，也不返回 MAC、密钥或完整邻接图。

完整载荷、资源上限、虚拟测试与五节点实机门禁见 [v4 节点快照诊断](UCN_v4_节点快照诊断.md)。

### 4.5 Link Cost：跨介质质量选路

Core 不直接读取 WiFi RSSI、BLE RSSI、LoRa SNR 或 CAN 专有状态。每个 Link 可选上报一个统一的非零 `route_cost`，值越小表示越适合参与路由；未上报有效指标时 v4 回退为保守 `UCN_UNKNOWN_LINK_ROUTE_COST=1000`，不会压过已测量的低 Cost 链路。

| Link 类型 | Adapter 可映射为 `route_cost` 的输入 | 不能直接写入 Core 的原因 |
| --- | --- | --- |
| WiFi / ESP-NOW | RSSI、重传次数、丢包率、发送队列积压。 | RSSI 只对无线链路有意义。 |
| BLE | RSSI、连接间隔、重传和丢包。 | BLE 的连接时序与 WiFi 不同。 |
| LoRa | RSSI、SNR、空口时间、占空比。 | 低 RSSI 不必然等于不可用。 |
| CAN / CAN-FD | Bus-Off、错误帧、总线负载、发送等待。 | CAN 没有 RSSI，且总线常为单跳广播域。 |
| UART / RS485 | CRC 错误、超时、队列积压。 | 是有线字节流，不存在无线信号质量。 |

这不是动态内存的全网最短路径算法：它只在现有固定路由表和受限 RREQ/RREP 流程中比较候选路径。质量值的采样、平滑和防抖由各 Link Adapter 负责，避免 WiFi RSSI 瞬时波动造成频繁换路。

### 4.6 Adapter：物理介质与 Core 的唯一边界

每种介质都遵循同一条路径：`物理地址 → Candidate Link → 有界收包队列 → 协议任务 pump → Core`。驱动 ISR/WiFi 回调不直接运行路由或应用逻辑；Core 不保存 MAC、CAN ID、串口号或 socket。物理地址到 Link 的静态映射由 Adapter 管理，成功 HELLO 后才由 Core 写入 `peer_node_id`。

当前“跨介质多跳转发”已由 Link + 通用 Cost 支持；但同一 Node ID 的 WiFi/UART/CAN 等多条自动准入 Link 还不能作为一个可切换的 Neighbor 集合。静态多 Link 可择低 Cost，自动邻居模型则会拒绝第二条同对端 Link。该差距和后续固定资源主备设计见 [多介质同对端主备 Link 建议](UCN_多介质同对端主备_Link_建议.md)，对应 T21；在 T21 完成前，不将其称为链路聚合或自动多介质冗余。

即使 T21 完成，它也只是在一个下一跳内部选择 Bearer，不会自动让不同业务在 `A→B→C` 与 `A→D→C` 间分担。后者属于 T22 的显式策略层，默认关闭，见[策略路由与可选负载均衡建议](UCN_策略路由与可选负载均衡建议.md)。

Adapter 的默认收包队列是两个最大帧缓存，可按 MCU RAM 编译期缩小。队列满必须显式丢弃并计数。接口、状态机、各介质映射和实际限制见 [UCN Adapter 契约](UCN_Adapter_契约.md)。

所有处于同一 Core Profile 的 Link 还必须共享可承载的最大帧上限：当前 Core 不做跨 Link 分片/重组，故 `UCN_MAX_FRAME_BYTES` 必须不大于最小有效 MTU。64 B CAN-FD 可使用 64 B Profile；经典 CAN 不能直接承载 32 B UCN 基础头，需后续有界 Carrier 分段/重组，不能宣称已直接支持。

## 5. UCN-Extended：仅给需要它的 MCU 开启

`Extended` 不是第二套协议，而是建立在 Core 之上的编译期模块集合。

| 可选模块 | 用处 | Core 节点关闭后怎样工作 |
| --- | --- | --- |
| Service Discovery | 发布/查询 `motor.control`、`imu.data` 等服务与版本。 | 使用预配置 Node ID + Message Type 通信。 |
| Group / Multicast | 面向编队或同类传感器的受权限组消息。 | 使用受限广播或多次单播。 |
| Q2 Reliable | 参数写入、配置、普通确认。 | Core 不承担可靠参数服务。 |
| Fragmentation | 小 MTU Link 上的大消息重组。 | 超过 Core 最大负载直接拒绝，不隐式分片。 |
| Q3 Bulk | 日志、OTA、文件等限速流量。 | 不在普通 MCU Mesh 上进行大数据传输。 |
| Time Sync | 编队、传感器融合和日志关联。 | 使用本地单调时间完成 Core 超时与防重放。 |
| Diagnostics | 详细统计、抓包镜像、长期质量历史。 | 仅保留必要故障计数。 |

只有 Coordinator、MCU 网关或 RAM/Flash 充足的控制节点才需要开启其中一部分。即使开启，也必须拥有独立的缓冲、队列和上限，绝不能占用 Q0/Q1 或路由表资源。

## 6. UCN-Host：兼容 Linux，但不替代 Linux

Linux 通过 `UCN-Host` 以一个普通受认证节点的身份加入网络。Host 复用与 MCU 相同的帧、身份、会话和 ACL，只额外提供主机侧适配与桥接：

- UDP、Ethernet、SocketCAN 等 Linux Link Adapter。
- ROS2、MAVLink/PX4、地面站和用户应用的白名单 Bridge。
- 日志、抓包、图形化配网、性能测试、AI 和大数据处理。

它不能做的事情：

- 不能绕过 MCU Coordinator 批准新节点加入。
- 不能修改 MCU 的 Route Cache，或强制 MCU 经由 Linux 转发。
- Host 断线、重启、未部署时，不能影响 MCU 间的会话、路由和本地控制。
- 不能把 ROS2 Topic、MAVLink 帧或 Linux UDP 裸包无筛选地写入执行器。

这就是“兼容 Linux 现有体系”的含义：Linux 保留原生能力，UCN 只提供一层受控的节点通信接口，不取代 Linux 的任何网络或机器人生态。

## 7. 像 FreeRTOS 一样保持简洁的实现方式

UCN 不能像 FreeRTOS 内核那样小，因为它还要解决身份与路由；但应采用相同的工程思想：**小核心、静态配置、明确模块、无隐式成本。**

### 7.1 当前 Core 的最小 API

```c
ucn_node_init(&node, &config);
ucn_node_set_join_policy(&node, UCN_JOIN_PROVIDER, authorize, context);
ucn_node_set_security_policy(&node, &node_policy);
ucn_node_set_endpoint_security_policy(&node, endpoint, &endpoint_policy);
ucn_adapter_rx_enqueue(&rx_queue, ingress_link, frame, frame_length);
ucn_adapter_rx_pump(&rx_queue, &node, max_frames, NULL);
ucn_node_send_endpoint(&node, destination, endpoint, UCN_TRAFFIC_Q1_REALTIME, payload, length);
ucn_node_request_path_trace(&node, destination, 0U, trace_callback, callback_context);
ucn_node_enqueue(&node, &request);  /* 或兼容的 ucn_node_send() */
ucn_node_step(&node, now_ms);
```

- `ucn-core` 不创建线程、不调用 `malloc`、不依赖 Linux socket 或特定 RTOS。
- 裸机主循环可周期调用 `ucn_node_step()`；FreeRTOS/Zephyr 可由端口层把 RX、TX、Timer 映射为任务或队列。
- 应用通过 `ucn_node_send()` 或 `ucn_node_enqueue()` 提交 Node ID、消息类型、Q0/Q1 与截止时间；应用不直接指定 WiFi、CAN 或下一跳。

### 7.2 当前代码目录

```text
UCN/
├── include/ucn/       types、frame、link、neighbor、security、adapter、node API
├── src/               ucn_core.c、ucn_frame.c、ucn_adapter.c、ucn_node.c
├── tests/             单元测试、虚拟 Link、路由、Adapter、集成模拟
├── docs/              架构、任务表、操作记录、Adapter 契约
└── CMakeLists.txt     C99 静态库与 CTest 入口
```

当前源码以小型平铺目录组织；未创建 `host/`、`extended/` 或具体厂商 `transport/` 目录。后续新增真实 Adapter 时可按介质拆分目录，但不能使 Host、ROS2 或 Linux 反向成为 Core 的编译依赖。

## 8. RAM 与 Flash：按目标硬件配置，不写死一个数字

UCN 不预设“所有 MCU 必须占用多少 RAM”。资源取决于目标 MCU、链路 MTU、最大节点数、安全算法、是否开启 Extended，以及应用自身剩余空间。

### 8.1 编译期资源配置

当前工程通过公共头文件的 `#ifndef` 宏或 CMake 编译参数裁剪资源；目标板专用 `ucn_config.h`/Kconfig 是后续工程化选项，尚未提供。当前可配置项至少包括：

```c
UCN_MAX_NEIGHBORS       // 可信一跳邻居数量
UCN_MAX_LINKS           // 已注册 Link 数量
UCN_MAX_ROUTES          // Route Cache 条目数
UCN_MAX_ROUTE_DISCOVERIES // 并发路由请求数
UCN_SEEN_CACHE_SIZE     // 入站去重缓存
UCN_MAX_FRAME_BYTES     // Core 最大帧和单帧负载
UCN_TX_Q0_DEPTH         // Q0 队列深度
UCN_TX_Q1_DEPTH         // Q1 队列深度
UCN_PENDING_Q1_DEPTH    // Endpoint Q1 等待寻路槽数
UCN_MAX_CANDIDATE_ROUTES // Candidate 路由表
UCN_MAX_ENDPOINT_SECURITY_POLICIES // Endpoint 安全策略表
UCN_PATH_TRACE_PENDING_DEPTH // 本地并发 Trace 槽
UCN_PATH_TRACE_REVERSE_DEPTH // 中继反向回程槽
UCN_PATH_TRACE_TIMEOUT_MS // Trace 临时状态寿命
UCN_ROUTE_EPOCH_GRACE_MS // 旧 Epoch 保留窗口
UCN_ADAPTER_RX_QUEUE_DEPTH // 公共 Adapter 收包队列深度
```

`UCN_MAX_HOPS=16` 是当前固定协议上限，不是当前可覆盖的资源配置项。

Core RAM 的构成应可计算而非猜测：

```text
Core RAM = 固定上下文
         + 邻居表 × UCN_MAX_NEIGHBORS
         + 路由表 × UCN_MAX_ROUTES
         + RREQ/去重缓存
         + Q0/Q1 队列与帧缓冲
         + Link Adapter 必需缓冲
```

启用 Extended 后，其分片槽、Q2/Q3 队列、服务目录和诊断缓存必须单独计入；不能把它们隐藏在 Core RAM 中。

### 8.2 资源纪律

1. Core 全部使用静态数组、调用方缓冲或初始化时一次性提供的内存池。
2. 不允许运行时按邻居、路由、分片或接收包无限扩容。
3. T15 的每个目标构建必须输出：Core/Extended 各自的 Flash、静态 RAM、峰值 RAM、栈峰值、CPU 占用和空口占用；当前仓库尚未有实机资源报告。
4. 编译期应检查配置是否合法；资源预算不足时构建失败或启动失败，不可悄悄降级为不安全行为。
5. 节点规模变大时优先选择更大 MCU 或调整配置，而不是取消会话校验、防重放或路由限速。

## 9. 节点配置档案

| 节点 | 必选档案 | 可选档案 | 说明 |
| --- | --- | --- | --- |
| 传感器/执行器 MCU | Core | 无 | 能入网、收发 Q0/Q1；是否允许转发由配置决定。 |
| 飞控/控制 MCU | Core | 少量 Extended | 本地闭环优先；网络只给高层命令与状态。 |
| MCU Relay | Core | 无或少量 Extended | 承担有限多跳转发，路由容量按硬件配置。 |
| MCU Coordinator | Core | Enrollment/Time 等 Extended | 只承担授权与策略角色，不承担业务中心。 |
| MCU 网关 | Core | 按需 Extended | 连接不同 Link，必要时提供服务或诊断。 |
| Linux/地面站 | Host | 按需 Extended | 只提供兼容、桥接、日志和大数据能力。 |

更细的档案和可选模块清单见 [UCN 协议分层与配置档案](UCN_协议分层与配置档案.md)。

## 10. V1 的实际实施顺序

### V1-A：MCU Core 单跳

- 固定帧、身份、加入、会话、AEAD、防重放。
- 一个无线或总线 Link。
- 邻居表、Q0/Q1、心跳、单跳通信。

### V1-B：MCU Core 自组网

- AODV-Lite、两跳/多跳转发、Route Cache、路由错误和受限修复。
- 三个 MCU 节点在没有 Linux 时完成认证、两跳通信、断链与恢复。
- 测量节点规模、跳数、帧长和安全算法配置下的资源与时延。

### V1-C：按需扩展与 Linux 兼容

- 只为必要节点开启服务发现、Q2、时间同步或分片。
- 最后实现 Host Adapter 与 ROS2/MAVLink Bridge。
- 验收条件是 Host 退出后 MCU Mesh 仍持续工作。

## 11. 编码前必须冻结的参数

以下参数不能从架构图中猜测，必须按首个实际硬件方案确定：

1. MCU、无线模块、总线控制器、RTOS/裸机环境以及各自可用 RAM/Flash。
2. 网络最大节点数、最大邻居数、最大并发会话数、最大跳数和最差 Link MTU。
3. Core 最大帧、Q0/Q1 队列深度、路由请求频率、缓存老化和心跳策略。
4. 设备身份保存方式、入网授权策略、所选 AEAD 套件和 Counter 持久化方案。
5. 哪些设备可以作为中继、Coordinator，哪些业务允许多跳以及失联时的本地安全动作。
6. 是否、在哪些节点启用 Extended；每个模块允许增加多少 Flash、RAM、CPU 与空口占用。
7. Linux Host 需要接入的实际 Link 与白名单 ROS2/MAVLink 业务，而不是先做全量桥接。

## 12. 最终判断

新的 UCN 架构以 MCU 为协议主体：**没有 Linux 仍能安全自组网；有 Linux 时只是在不改变 Linux 体系的前提下增加一个受控接入端。**

简洁性来自 Core 的严格边界，而不是删除安全和路由：Core 只承担自组网必需能力，所有“好用但重”的能力均后置为 Extended 或 Host。资源占用不再写死一个数字，而是随目标硬件和配置生成可验证的预算与报告。
