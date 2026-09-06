# UCN 协议分层与配置档案

> `ARCHIVED / NOT CURRENT`：本文描述 v5 分层与配置；当前 v6 架构请见 [v6 官方文档](../official/README.md)。

> 状态：**UCN v5 V5-01～V5-20、V5-22～V5-36、V5-44～V5-60、V5-62 软件闭环（2026-08-14）**；V5-16 为设计冻结，V5-21 阻塞于 S02。Nano/Lite/Full 与 W0～W3 正交，三种 Build Profile 均使用四档 Decoder；当前还包含 3 B Ingress Peek、运行期 Hop Scope、3/3/3/4 B Wire Cost、Candidate Profile 连续性、Q1 绝对 Deadline、有界 Expanding Ring、动态 MTU、异构 Bearer Path 能力、PATH_INSTALL 双格式兼容、Full LC-1 本地动态选路、Port API V2、权威 Transfer 时钟和经典 CAN 完成优先提交。真实 Adapter、生产 AEAD/身份与目标板资源仍待接入。
> 日期：2026-08-14
> 关联文档：[UCN 整体架构设计](../02-总体架构/UCN_整体架构设计.md) · [工程架构索引](../02-总体架构/项目结构/README.md)

## 1. 拆分目标

UCN 的第一目标是让资源受限 MCU 独立完成安全自组网，而不是让每个节点默认携带服务目录、文件传输、时间同步和 Linux 桥接。

因此协议固定拆为三层：

```text
UCN-Core      所有参与自组网的 MCU 必须具备
    ↑
UCN-Extended  只有协调器、网关或资源充足节点按需增加
    ↑
UCN-Host      只有 Linux、地面站、ROS2 等外部主机使用
```

上层只能依赖下层，不能反向依赖：关闭 `Extended` 或完全不部署 `Host` 时，`Core` 的认证、发现、路由、转发和失联恢复仍完整可用。

当前仓库已实现 v5 W0～W3 Codec、所有 Build Profile 四档接收、Node 固定 TX/最大 RX 档、per-Link 本地 RX 上限、1 B RX Ceiling HELLO、Profile-aware 控制载荷、32 bit 累计 Cost、3/3/3/4 B Wire Cost、3 B Ingress Peek、运行期 Hop Scope、Profile 绑定安全、显式自动最小档、Candidate Profile 连续性、2→4→8→16 有界发现和 Q1 绝对 Deadline，以及从 v4 继承并演进的邻居保活/回收、Route Epoch/grace、受限 Candidate 选路、静态 Endpoint、Q0～Q3 固定队列与 `6:3:2:1` 调度、透明密文中继、受认证 Path ID 逐跳表、按需诊断、Adapter 模拟和 Full LC-1 Bearer/Candidate/Q1 本地评分。公开编译参数集中在 `ucn_config.h`，产品头可覆盖且原文件默认保留。默认固定 W3；`Extended Gateway`、真实 Linux Host 产品实现、生产密码库、Authorized Class 执行层和真实介质驱动仍未实现。

## 2. 三个档案的边界

| 档案 | 典型节点 | 必须解决的问题 | 明确不包含 |
| --- | --- | --- | --- |
| `UCN-Core` | 所有 MCU 自组网节点、MCU 中继、MCU Coordinator。 | 安全入网、可信邻居、有限多跳、逐跳转发、Q0～Q3 固定队列与有界调度、失联安全。 | 服务目录、文件分片、全网日志、ROS2、视频、Linux IPC。 |
| `UCN-Extended` | Coordinator、资源更充足的网关、少数主控 MCU。 | 服务发现、组播、时间同步、Q2 可靠传输、可控分片和诊断。 | Linux 特有 API、ROS2、图形化界面和无限大缓存。 |
| `UCN-Host` | Linux、地面站、ROS2/AI 计算机。 | 将现有主机业务受控映射到 UCN，提供运维、日志和大数据能力。 | 作为 MCU 网络的必经路由、唯一身份中心或唯一 Coordinator。 |

### 2.1 Core 内部的编译期 Feature Profile

Core/Extended/Host 是系统分层；Nano/Lite/Full 是 **Core 本身在某个节点上的编译档案**，两者不能混为一谈。

| `UCN_PROFILE` | Core 能力 | 典型节点 | 自动 Mesh |
| --- | --- | --- | --- |
| `NANO` | Frame、Link、Adapter RX Queue、Endpoint、Q0～Q3 固定队列与 `6:3:2:1` 调度、静态直连和静态 Route。 | 资源很小且拓扑预配置的传感器/执行器。 | 否 |
| `LITE` | Nano + HELLO/准入、Neighbor/Heartbeat、多 Bearer、AODV-Lite/RERR、最小 Security Provider。 | 普通 MCU Mesh 节点与中继。 | 是 |
| `FULL` | Lite + Candidate、显式 Path、Policy/Balance、Path Trace、Node Snapshot、Policy Diagnostic。 | 主控、复杂中继、诊断或多路径节点。 | 是 |

`UCN_FEATURE_SERVICE` 独立控制节点内 Service Router/Bridge。Nano 使用独立 `ucn_node_nano.c`；Lite/Full 使用动态 Node；非 Full 不编译 Path/Policy 源文件，高级 API 由固定 Stub 返回 `UCN_ERR_CONFIG`。因此这是对象字段和源文件级裁剪，不是仅关闭运行时分支。当前最小帧上限分别为 Nano 33 B、Lite 46 B、Full 64 B；默认 Service 开启时最低为 64 B。详见 [S04 Feature Profile 与资源报告](../08-实现与验证/版本演进/UCN_S04_Feature_Profile与资源报告.md)。

## 3. UCN-Core：MCU 自组网最小闭环

### 3.1 Core 模块与当前状态

| 模块 | 当前状态 | 后续范围 |
| --- | --- | --- |
| Fixed Frame | 已实现版本 5 的 W0/W1/W2/W3 官方 Codec；基础头 17/21/26/30 B，Route/Path 长度由 Profile+Flags 推导；Node 支持固定域和显式自动最小档，保留 CRC、16 B 可选 Tag、完整范围校验并拒绝 v4。可选 `ucn_transfer` 复用这些正常帧承载 T32～T8K，默认 Fragment 窗口与 Peer 消息并发均为 1；显式窗口 2～8 和静态有界多消息并发均不改变 Wire。 | Core 自身仍不分片；Transfer 的完整 Path MTU、动态窗口/并发能力协商和其他 Bearer 性能待验收。 |
| Identity & Join | 已实现最小 HELLO、邻居状态机和三种准入策略。 | JOIN 挑战/接受、设备证书/身份格式。 |
| Session & Replay | 已实现 Provider 回调、会话 ID、持久化发送序号、入站去重、`seal/open`、固定 AAD 和 Node/Endpoint 安全策略。 | 生产 AEAD、密钥轮换、完整重放窗口。 |
| Trusted Neighbor | 已实现固定 Candidate/Admitted/Suspect/Removed/Rejected/Expired 表、Heartbeat 和已接纳节点撤销/Link 槽复用。 | 随机退避、入网令牌桶与实机在线时间标定。 |
| AODV-Lite Route | 已实现 RREQ/RREP/RERR、Active/Candidate 固定表、刷新、Probe/Activate、Route Epoch/grace、老化、保守未知 Cost、源端控制 Token，以及 Full 本地 LC-1 出口贡献比较。 | 真实介质质量标定。 |
| Explicit Path | 已实现默认 8 项的逐跳 Path 表；v5 Path Header 为 W0～W3 的 19/25/31/36 B，固定/自动模式均可使用。受认证安装/撤销、透明 E2E 中继、Path RERR、`PINNED_*` 与基于 LC-1 有效分的 Q1 Flow 亲和 `AUTO_BALANCE` 保持。 | 真实多板验收。 |
| Forwarding / QoS | 已实现 TTL、下一跳、Q0～Q3 独立固定队列、`6:3:2:1` 调度、deadline、Q1 latest-value、静态 Endpoint 分发；Endpoint Q1 未知路由固定等待/自动 RREQ。 | 普通消息无端到端可靠确认；硬件优先队列映射待产品 BSP。 |
| Node 内 Service / Task | Endpoint 目前分发到协议任务中的固定回调，不增加帧字节。 | T25：Port 层的静态 Endpoint→任务队列映射、统一任务发送 API、本机直投和任务收发统计；不把 FreeRTOS 写入 Core。 |
| Health | 已实现候选/路由老化、Heartbeat、Link down 路径清理和动态 Link 回收。 | 统一 Link 状态消息、应用失联事件。 |

### 3.2 Core 的报文类型与实现状态

| 类别 | 报文 | 当前状态 |
| --- | --- | --- |
| 邻居 | `HELLO` | 已处理，一跳绑定 Node ID 并执行准入；不转发、不交给应用。 |
| 入网 | `JOIN_REQ`、`JOIN_CHALLENGE`、`JOIN_ACCEPT` | 枚举值已保留，尚未实现状态机。 |
| 路由 | `ROUTE_REQ`、`ROUTE_REPLY`、`ROUTE_ERROR` | 已处理。 |
| 健康 | `HEARTBEAT` | 已处理：一跳 8 B 请求/ACK 和邻居状态机；`LINK_STATE` 尚未定义为线协议消息。 |
| 路径验证 | `PATH_PROBE`、`PATH_PROBE_ACK`、`PATH_ACTIVATE`、`PATH_ACTIVATE_ACK` | v5 继续使用现有控制语义；Activate/ACK 载荷为 Candidate ID + Route Epoch，业务按 Epoch 查 Current/Previous。 |
| Path 控制 | `PATH_INSTALL`、`PATH_REVOKE` | 已处理：字段按 Wire Profile 宽度编码；PATH_INSTALL 基础格式为 8/11/14/17 B，扩展能力格式为 11/14/17/20 B，REVOKE 为 2/4/6/8 B；旧 API 固定发送基础格式，capability API 显式发送扩展格式；依次需要 Provider、显式控制面授权和按 `(Source, Session)` 的固定管理预算，同源换 Bearer 不会重置预算。 |
| 按需诊断 | `PATH_TRACE_REQ`、`PATH_TRACE_REPLY` | 已处理：`DIAGNOSTIC=0x04`、逐跳 Node ID、固定 Pending/Reverse 表和回调结果；只查当前 Cache，不触发 RREQ，也不锁定业务路径。 |
| 按需诊断 | `NODE_SNAPSHOT_REQ`、`NODE_SNAPSHOT_REPLY` | 已处理：受限泛洪、短期 Reverse、随机短延迟 Reply、源端固定结果表与默认拒绝 ACL；不常驻保存全网节点或拓扑。 |
| 按需诊断 | `POLICY_DIAGNOSTIC_REQ`、`POLICY_DIAGNOSTIC_REPLY` | 已处理：受授权单 Node 查询、三页 Summary 或一个固定 Policy/Path/Flow/quality 槽位、8 B 请求/32 B 回复、独立 Token 与 Pending/Reply 固定表；普通业务帧零额外字段。 |
| 数据 | `DATA_Q0`、`DATA_Q1`、静态 Endpoint `0x40～0xBF` | 已处理；Endpoint 可固定分发，不增加帧字节。 |

Core 只需要单播、受限广播和有限多跳。组播、服务调用、文件传输和任意长度分片不是 Core 的前置条件。

`ROUTE_REQ`/`ROUTE_REPLY` 的控制载荷携带 32 bit 语义的累计 `route_cost` 与 hop 数；Cost 在线上按 W0/W1/W2/W3 使用 `3/3/3/4 B`，Route Epoch 使用 1/2/2/2 B。Link 未上报质量时先以 16 bit 单跳 Unknown 输入，累计 Unknown 为 `0xFFFFFFFF`；因此 WiFi、BLE、LoRa、CAN、UART 的质量指标必须先由各自 Adapter 归一化，不能把 RSSI 等无线字段写入共同线协议。

### 3.3 Core 的资源纪律

Core 必须满足“所有可增长状态均有编译期上限”：

```c
UCN_MAX_NEIGHBORS
UCN_MAX_LINKS
UCN_MAX_ROUTES
UCN_MAX_ROUTE_DISCOVERIES
UCN_DUPLICATE_SOURCE_WINDOWS
UCN_DUPLICATE_WINDOW_BITS
UCN_DUPLICATE_SOURCE_TIMEOUT_MS
UCN_RREQ_CACHE_SIZE
UCN_MAX_FRAME_BYTES
UCN_TX_Q0_DEPTH
UCN_TX_Q1_DEPTH
UCN_PENDING_Q1_DEPTH
UCN_MAX_CANDIDATE_ROUTES
UCN_MAX_ENDPOINT_SECURITY_POLICIES
UCN_PATH_TRACE_PENDING_DEPTH
UCN_PATH_TRACE_REVERSE_DEPTH
UCN_PATH_CONTROL_RX_SOURCE_DEPTH
UCN_PATH_CONTROL_RX_TOKEN_BURST
UCN_PATH_CONTROL_RX_TOKEN_REFILL_MS
UCN_PATH_CONTROL_RX_SOURCE_IDLE_MS
UCN_PATH_TRACE_TIMEOUT_MS
UCN_NODE_SNAPSHOT_MAX_RESULTS
UCN_NODE_SNAPSHOT_PENDING_DEPTH
UCN_NODE_SNAPSHOT_REVERSE_DEPTH
UCN_NODE_SNAPSHOT_REPLY_QUEUE_DEPTH
UCN_NODE_SNAPSHOT_TIMEOUT_MS
UCN_NODE_SNAPSHOT_TOKEN_REFILL_MS
UCN_POLICY_DIAGNOSTIC_PENDING_DEPTH
UCN_POLICY_DIAGNOSTIC_REPLY_QUEUE_DEPTH
UCN_POLICY_DIAGNOSTIC_TIMEOUT_MS
UCN_POLICY_DIAGNOSTIC_TOKEN_REFILL_MS
UCN_ADAPTER_RX_QUEUE_DEPTH
UCN_MAX_STEP_INTERVAL_MS
```

上述宏当前定义在公共头文件中，可由构建参数覆盖；`UCN_MAX_HOPS=16` 是默认编译期上限，Node 的 `default_hop_limit` 是不得超过它的运行期参与范围。Ingress、RREQ/RREP 学习和 Path 安装都执行运行期上限；实际产品需按 MCU 的 RAM、Flash、Link MTU、节点数和安全算法测量后冻结。

`UCN_MAX_STEP_INTERVAL_MS` 不是协议线字段，而是 Product Port 的调度 Profile。Core 将它与业务 Burst、最大 Neighbor、每 Neighbor Bearer 数换算成保守维护服务上界，并要求 Heartbeat 间隔加该上界严格早于 Suspect 门限。默认 10 ms 档案得到 800 ms；将 Step 放宽到 50 ms 会在当前默认资源档案下编译失败。运行时只用固定计数记录最大 Gap/违规，实际任务抢占和 Link `send()` WCET 仍需目标板测量。

Core 禁止以下实现方式：

- 运行时无限动态内存分配。
- 无上限的路由泛洪、重传和广播缓存。
- 每个报文携带昂贵的数字签名。
- 让 Q0 等待路由发现，或让 Q1 重传过期旧状态。
- 把 Host 是否在线作为节点转发条件。

任务间消息同样采用固定表/固定队列：一个 MCU 只维护一个 Node/邻居/路由实例；本机目标由后续 T25 Service/Task Adapter 直接投递，不生成 UCN 帧。它是产品 Port 层，不是额外的路由协议或 Node 身份；详见[节点内任务通信建议](../05-传输与服务/UCN_节点内任务通信建议.md)。

远端高风险 Q0 使用 `UCN_SERVICE_BRIDGE_MAX_VALIDATORS` 个固定 Validator 槽；可选防重放状态使用 `UCN_SERVICE_BRIDGE_REPLAY_DEPTH` 个固定槽。两者均由产品编译档案确定、无动态扩容，也不改变 v5 线格式或普通 Payload。Binding 要求强制 Validator 而产品未注册时，Bridge handler 安装失败关闭。

### 3.4 Core 在没有 Linux 时怎样运行

1. MCU 从产品配置读取 Node ID、Network ID；启用安全时由 Provider 读取会话与持久化计数器。
2. Adapter 将无线/总线物理端点绑定为 `peer_node_id=0` 的 Candidate Link，并仅把完整帧放入有界收包队列。
3. 协议任务 Pump 收包；双方用物理广播 `HELLO` 绑定一跳 Node ID。最小 HELLO 不转发、不进入应用。
4. `Manual`、`Open` 或 `Provider` 决定是否记录已准入邻居；生产网络必须由 Provider/Coordinator 身份策略授权，中继不能擅自授权。
5. Endpoint Q1 目标不在直连/Route Cache 时，`ucn_node_send_endpoint()` 通过受限 `ROUTE_REQ` 建立 Route Cache，并按 `(destination, Endpoint)` 在固定等待表中覆盖旧值；Q0 和兼容的原始 `ucn_node_send()` 仍显式寻路/不等待。
6. Q0～Q3 通过缓存路径发送；路径断开时清理缓存并产生 `ROUTE_ERROR`。
7. 若找不到安全路径，应用收到不可达事件，飞控/执行器执行本地失联安全策略。

这套流程只要求 MCU 能使用至少一种 Link。STM32 没有无线收发器时，可以通过 CAN-FD/RS485 成网，或外挂无线模块；协议本身不要求 Linux。

Adapter 不把 MAC、CAN ID 或蓝牙地址当作 UCN 身份。其统一接口、固定队列和物理地址映射见 [UCN Adapter 契约](../06-平台与适配/UCN_Adapter_契约.md)。Core 不做逻辑消息分片；可选 `ucn_transfer` 在 Core 上层切片，但每个实际 UCN 帧仍必须小于对应 Path/Bearer 的有效 MTU。`link->mtu=0` 表示静态值未知，由 `get_status().mtu` 动态提供；两者非零取较小值，两者都为零时 Link 暂不可发送。经典 CAN 已由独立 Adapter Carrier 有界分段/重组，这与 Transfer 的业务消息分片不是同一层；真实 CAN 控制器驱动仍归产品。

## 4. UCN-Extended：按需增加而非默认常驻

| 可选模块 | 解决的问题 | 关闭后的行为 |
| --- | --- | --- |
| Service Discovery | 发布 `motor.control`、`imu.data` 等服务及版本。 | 应用以预配置 Node ID + Message Type 通信。 |
| Group / Multicast | 面向编队或同类传感器的受权限组消息。 | 使用受限广播或多次单播。 |
| Reliable Transfer | 参数写入、配置和需确认的普通服务。 | Core Q2 只提供 Normal FIFO；逐条可靠交付必须使用 Transfer/业务 Result。 |
| Fragmentation | 在小 MTU Link 上传输较大消息。 | 超过 Core 最大载荷直接拒绝，不隐式切片。 |
| Single-level Cluster | 一跳邻居范围内选 Head、受容量加入、租约保活和失效重选。 | 节点继续使用 Core 的扁平按需 Route/Path，不产生簇级语义。 |
| Q3 Bulk + Transfer | 日志、OTA、文件与低优先级限速。 | Core Q3 只有小型固定 FIFO；不链接 Transfer 时不能发送超单帧大消息。 |
| Time Sync | 日志关联、编队与传感融合时间基准。 | 使用本地单调时间，不提供跨节点精密时间。 |
| Diagnostics | 详细统计、抓包镜像、长期质量历史。 | Core 只保留必要的错误计数和健康状态。 |

`Extended` 节点仍必须服从 Core 的固定内存原则。当前 `ucn_transfer` 使用独立、可裁剪的 TX/RX/Endpoint/Peer 固定表；`ucn_cluster` 使用独立 Peer/Candidate/Member 固定表；两者均为 `EXCLUDE_FROM_ALL` Target，没有链接/创建对象的节点不支付对应 RAM。Cluster 首阶段不含簇间 Locator/Directory/Tunnel，不能把单层收敛测试当作万级数据面。Q3 已有独立固定小队列和 `6:3:2:1` 调度配额，绝不能借用或吞掉 Q0/Q1 的队列、会话或路由内存。

## 5. UCN-Host：兼容层，不是协议中心

`UCN-Host` 的目标是复用与 MCU 相同的线协议、身份和 ACL，但当前仓库只验证了“Host 作为普通逻辑节点”的虚拟 Link 边界，尚未提供 UDP/Ethernet/SocketCAN 或 ROS2/MAVLink 实现。未来 Host 增加的能力为：

- Linux UDP/Ethernet/SocketCAN 适配。
- ROS2、MAVLink/PX4、地面站与用户应用的白名单 Bridge。
- 日志、抓包、可视化配网、性能测试和大数据处理。
- 可作为已授权的时间源或管理操作发起端。

它不能获得协议特权：

- 无权绕过 MCU Coordinator 批准节点加入。
- 无权修改 MCU 的 Route Cache 或让其强制经由 Host 转发。
- Host 断开时，不影响已建立 MCU 会话、邻居表和 MCU 间数据转发。

## 6. 节点配置组合

| 节点类型 | 编译档案 | 典型用途 |
| --- | --- | --- |
| Sensor / Actuator Node | `Core` | 只发送实时状态或接收受限控制；必要时可转发。 |
| Flight / Control Node | `Core` | 飞控、姿态控制、关键执行器；Q0/Q1 和本地失联安全优先。 |
| MCU Relay Node | `Core` | 参与邻居、路由和多跳转发，连接不同 Link。 |
| MCU Coordinator | `Core + Extended(Enrollment/Time 可选)` | 授权入网、网络策略、可选时间源；不做业务中心。 |
| MCU Service Gateway | `Core + Extended` | 服务目录、参数、分片或受控 Q2/Q3。 |
| Linux / Ground Station | `Host`，按需叠加 `Extended` | 观察、运维、ROS2/PX4、AI、日志和大数据。 |

## 7. V1 的实施顺序

### Phase A：只实现 Core

当前已通过内存虚拟 Link 验证多跳业务、`ROUTE_ERROR`、重新寻路和 Q0～Q3 队列隔离；真实目标板四级延迟、吞吐、硬件优先级映射和生产安全仍待实机门禁。

### Phase B：只给必要 MCU 增加 Extended

有界 Transfer 已实现默认单消息、可选静态 Peer 有界多消息并发；Node/Service 的通用 Q2/Q3 软件队列也已完成。后续按实际需要增加动态能力协商、服务发现和时间同步，并把四级策略接入具体 Bearer 的硬件能力。每增加一项都要记录新增 Flash、静态 RAM、峰值 RAM、CPU 和空口占用。

### Phase C：最后接入 Host

再实现 Linux/ROS2/MAVLink Bridge。验收点不是“Host 能控制网络”，而是 Host 接入、异常退出和断链都不影响 MCU Mesh 的基本通信。

## 8. 对“简洁、完整、低消耗”的最终判断

拆分后，`UCN-Core` 的功能是完整的最小自组网闭环，但刻意不承担服务目录、大包、文件和主机生态；因此它适合长期运行在 MCU 上。

`UCN-Extended` 与 `UCN-Host` 让系统在需要时变完整，而不把所有复杂度复制到每个节点。运行消耗仍不能凭架构文字宣称“很低”，但拆分后的资源边界清晰：低资源节点只为 Core 付费，额外能力必须单独编译、单独测量、单独验收。
