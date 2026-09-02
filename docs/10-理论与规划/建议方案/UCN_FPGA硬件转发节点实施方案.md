# UCN FPGA 硬件转发节点实施方案

> 文档状态：设计建议（DESIGN PROPOSAL），尚未实现。
> 适用基线：当前 UCN v5 开发架构。
> 编写日期：2026-09-02。
> 使用边界：本文规定未来需要 FPGA 加速时“具体怎样实施”，不表示当前源码已经包含 RTL、FPGA BSP、硬件转发表或实机验证结果。
> 核心原则：UCN 仍然以 MCU 自组网为主体；FPGA 是可选的高速转发器、汇聚节点或骨干节点，不是普通节点运行 UCN 的前置条件。

## 1. 为什么需要这份方案

当前 UCN 的普通中间节点不是简单地把收到的字节原样复制到另一个接口。它至少要完成：

1. 从 UART、CAN、USB、无线等物理载体中恢复出完整 UCN 线帧；
2. 校验长度、Wire Profile、Network ID、CRC、Hop Limit 和必要的安全状态；
3. 根据 Destination、Path ID、Route Epoch、策略和当前 Link 状态选择出口；
4. 将 Hop Limit 减一；
5. 按目标 Link 的 Wire Profile、MTU 和载体格式重新编码；
6. 重新计算 CRC，进入对应流量等级队列并发送。

这些动作保证了跨 UART、CAN、USB、Wi-Fi 等异构链路时仍具有统一语义，但也带来软件调度、内存复制、编解码和队列等待开销。跳数增加后，物理串行时间与每跳软件处理时间都会累加。

FPGA 方案的目标不是“让协议不再解析”，而是把普通数据帧的重复性处理做成并行流水线，使中间节点在保持 UCN 语义的前提下减少 CPU 唤醒、任务调度和软件复制抖动。

## 2. 最终结论

建议采用下列混合架构：

```text
                         配置、发现、选路、Cluster、安全策略
                     ┌────────────────────────────────────┐
                     │          MCU / CPU 控制平面         │
                     │ UCN Node + Route/Path + Diagnostics │
                     └───────────────┬────────────────────┘
                                     │ 表项、Fence、事件、统计
                                     │ AXI/APB/SPI/共享内存
┌──────────┐     ┌───────────────────▼────────────────────┐     ┌──────────┐
│ UART/CAN │────▶│                 FPGA 数据平面            │────▶│ UART/CAN │
│ USB/ETH  │◀────│ RX→成帧→校验→查表→减 Hop→CRC→QoS→TX     │◀────│ USB/ETH  │
└──────────┘     └────────────────────────────────────────┘     └──────────┘
                              │
                              └── 本机目的、路由缺失、控制帧和异常帧
                                  上送 MCU/CPU 慢路径
```

分工原则是：

- MCU/CPU 决定“网络应该怎样工作”；
- FPGA 加速“已经决定好的普通数据怎样转发”；
- FPGA 不自行发明路由、不自行选簇头、不自行更换安全策略；
- 任何无法证明安全或语义完整的帧都走慢路径或失败关闭。

这是比“用 FPGA 完整重写 UCN”更稳妥的实现方式，也能保留 MCU-only 节点和现有软件实现。

## 3. 目标与非目标

### 3.1 目标

FPGA 转发节点应实现：

- 多个物理接口可以同时接收和发送，而不由单个 Owner 任务串行轮询；
- 普通业务帧在已有 Route/Path 的情况下走硬件快速路径；
- 保持 UCN Wire Profile、Traffic Class、Hop Limit、CRC 和 E2E 密文语义；
- 由 MCU/CPU 原子更新转发表，避免半更新表项参与转发；
- Q0/Q1/Q2/Q3 分级调度和有界背压；
- 每端口、每队列、每丢弃原因都有可读取统计；
- FPGA、CPU、端口或表项异常时能明确 Fence，并可回退到软件路径；
- 同一个 UCN 网络可混合使用普通 MCU 节点与 FPGA 加速节点。

### 3.2 非目标

第一阶段不追求：

- 在 FPGA 内实现完整 AODV-Lite 路由发现；
- 在 FPGA 内实现 Cluster 选举、Joint Config、Takeover 或 Federation；
- 在 FPGA 内管理身份密钥、证书或复杂安全策略；
- 默认采用收到前几个字节就立即发出的 cut-through 转发；
- 让一个损坏、未知或未认证控制帧进入快速路径；
- 用 FPGA 替代所有 MCU 节点；
- 仅凭仿真数据宣称真实板级吞吐、功耗或生产安全已经完成。

## 4. 与当前 UCN 架构的关系

本文不改变 UCN 的三平面边界：

- 数据平面：业务帧、转发、队列和物理发送；
- 控制平面：Neighbor、Route、Path、Cost、Policy、Cluster 和安全决策；
- 诊断平面：路径查询、全网节点查询、统计与故障证据。

FPGA 只下沉数据平面的确定性子集。现有架构依据仍以以下官方文档为准：

- [数据平面、控制平面与诊断平面](../../official/01-总体架构/03-数据平面、控制平面与诊断平面.md)
- [Protocol Owner、并发与 ISR 模型](../../official/01-总体架构/04-Protocol-Owner、并发与ISR模型.md)
- [一帧数据从任务到物理接口的完整路径](../../official/01-总体架构/07-一帧数据从任务到物理接口的完整路径.md)
- [Wire v5 W0 至 W3 帧格式](../../official/02-核心协议/02-Wire-v5-W0至W3帧格式.md)
- [Link、Bearer、Neighbor、Route 与 Path 关系](../../official/03-路由与链路/01-Link、Bearer、Neighbor、Route与Path关系.md)
- [Link 与 Adapter 公共契约](../../official/05-Adapter与平台/01-Link与Adapter公共契约.md)

必须始终满足：关闭 FPGA 加速后，现有软件 Node、Adapter 和 Protocol Owner 仍能独立工作。

## 5. FPGA 与 MCU/CPU 的详细职责

| 能力 | FPGA 数据平面 | MCU/CPU 控制平面 |
| --- | --- | --- |
| 物理接收 | 接收字节/物理帧、跨时钟域、进入硬件 FIFO | 初始化接口，处理复杂驱动故障 |
| Carrier | COBS 解帧、CAN Carrier 重组、USB/以太网封装入口 | 配置模式与参数，处理未支持格式 |
| 基础校验 | 长度、版本、Profile、Network、CRC、Hop Limit | 决定允许的能力与兼容策略 |
| 路由 | 查询已安装 Route/Path | Neighbor、RREQ/RREP/RERR、Cost、Policy、Path 安装 |
| 转发 | 减 Hop、重算 CRC、按出口 Carrier 编码 | 处理路由缺失、失效和重新发现 |
| 调度 | 每端口 Q0～Q3 队列、信用与仲裁 | 下发限额、优先级和负载策略 |
| 安全 | 保持 E2E 密文不变；可选固定硬件校验 | 身份、密钥、策略、解密终点、审计 |
| Cluster | 不参与决策 | 完整 Cluster FSM 与持久化 |
| 诊断 | 计数器、时间戳、丢弃原因、事件 | 汇总、解释、查询和导出 |
| 故障 | Fence 端口/表项，停止不安全转发 | 决定恢复、重新安装或软件接管 |

## 6. 快速路径、慢路径和丢弃路径

### 6.1 快速路径

同时满足下列条件的普通帧才允许硬件转发：

- Wire 长度与 Profile 合法；
- Network ID 与本节点配置匹配；
- CRC 正确；
- 不是本机最终目的；
- Hop Limit 大于 1；
- 该类型被当前硬件能力白名单允许；
- 命中有效、当前 generation 的 Route 或 Path；
- 出口 Link 为 Up，MTU/Profile/安全属性满足；
- 对应流量队列存在容量或允许执行规定的背压策略。

快速路径适合已有稳定路由的普通单播业务数据，包括经过中间节点的 E2E 密文。

### 6.2 慢路径

下列情况应上送 MCU/CPU：

- 目的地是本机 Endpoint 或 Service；
- Route/Path 未命中或表项已经过期；
- HELLO、RREQ、RREP、RERR、Path 安装/撤销等控制帧；
- Cluster、诊断或需要 CPU 权限判断的消息；
- 广播、泛洪或尚未冻结硬件语义的多播；
- Wire Profile/扩展字段合法但当前 FPGA 版本不支持；
- 需要终点解密、重组 Transfer 或交给业务任务的帧；
- 硬件事件需要软件决定是否重试、改路或撤销链路。

### 6.3 直接丢弃

下列输入不应消耗慢路径队列：

- 长度越界；
- CRC 错误；
- Hop Limit 为 0 或 1 且当前节点不是终点；
- 非法 Wire Profile、保留位或 Network ID；
- 明确命中硬件重复/重放拒绝规则；
- 输入端口被 Fence；
- 描述符、Buffer 所有权或内部状态损坏。

每次丢弃必须增加确定的 reason counter，不能静默消失。

## 7. 为什么第一版必须使用 Store-and-Forward

第一版应先接收完整帧、完成校验，再决定转发，即 Store-and-Forward。

理由如下：

1. UCN 的 CRC 覆盖 Header、Payload 和可选 Auth Tag；没有收到尾部就不能确认完整帧有效。
2. 中间节点需要修改 Hop Limit，修改后必须按当前 Wire 规则重新计算 CRC。
3. 出口可能使用不同 Carrier 或不同 Wire Profile，不能保证入口字节能直接复制。
4. 路由表可能在收帧期间发生 generation 切换，完整缓存后更容易做一致性判断。
5. 出口队列满、Link Down 或 Fence 时，完整帧可以按确定规则上送或丢弃。
6. 与软件 Node 做逐帧差分验证时，完整帧边界最清晰。

Cut-through 可以作为后续独立优化，但默认关闭，只允许用于满足以下全部条件的受控链路：

- 入口和出口格式完全相同；
- 可以边收边计算并在结尾验证 CRC；
- 产品接受坏帧前缀已经进入下一段链路的风险；
- Hop/CRC 修改可以流式完成；
- 出口提前获得信用，不会在中途被背压打断；
- 安全审计明确允许。

对普通混合 MCU 网络，不建议以 cut-through 作为首版要求。

## 8. 硬件接收流水线

每个物理入口建议按下列顺序实现：

```text
PHY/外设 RX
  → 异步 FIFO / CDC
  → Carrier Decoder
  → 长度与边界检查
  → Frame Buffer 写入
  → 流式 CRC
  → Header Parser
  → Admission Gate
  → Route/Path Lookup
  → Fast / Slow / Drop 分类
```

### 8.1 PHY 与跨时钟域

- UART、CAN、USB、Ethernet MAC 可以运行在不同 clock domain；
- 每个入口使用独立异步 FIFO，禁止直接跨域采样多位状态；
- FIFO overflow 必须锁存事件和计数；
- 复位释放必须同步到各时钟域；
- 端口 disabled/fenced 时不接受新帧，已经接收中的帧按规定完成或丢弃。

### 8.2 Carrier Decoder

Carrier 层只负责恢复“一个完整 UCN Wire Frame”，不解释业务 Payload：

- Stream/UART/RS-485：识别 COBS 分隔符，解码并检查最大长度；
- CAN-FD：验证物理帧长度和 padding，再提取 UCN Frame；
- Classic CAN：按 Carrier 序号和长度重组，拒绝冲突 START、缺片和超时；
- USB：根据选定的 bulk packet 或 stream framing 恢复边界；
- Ethernet/UDP：先验证外层长度和端口，再提取 UCN Frame。

### 8.3 Frame Buffer

建议采用固定 BRAM Buffer Pool，不使用动态分配：

- Buffer 大小至少覆盖最大受支持 Wire Frame；
- Buffer 数量在综合时固定；
- free/ingress/lookup/egress/slow-path 各状态只能有一个 Owner；
- 描述符和 Buffer 通过 index 关联，不跨 CPU/FPGA 传裸指针；
- 所有状态转换使用单向握手或有效位，避免双重释放。

## 9. Header 解析与 Admission Gate

硬件至少要解析当前转发决策所需字段：

```text
Wire Profile
Message Type
Traffic Class
Flags
Hop Limit
Network ID
Source Node ID
Destination Node ID
Sequence
Session ID
Route Epoch（若 Profile 携带）
Path ID（若 Profile 携带）
Payload Length
Auth Tag Length（若启用）
CRC
```

注意：以上是“语义字段列表”，不是新的 Wire 布局。实际 bit/byte offset 必须从当前 C Codec 生成单一冻结合同，禁止 HDL 根据本文自行猜测位置。

Admission Gate 必须在查表前完成：

- 精确 Wire 版本和长度分派；
- 保留位必须为零；
- Node ID、Network ID、Payload Length 合法；
- Profile 能力在本硬件声明范围内；
- 完整 CRC 验证通过；
- 安全标志与端口策略一致；
- 帧类型在快速路径白名单内。

解析失败时不得留下部分有效的 metadata。

## 10. Route 与 Path 硬件表

### 10.1 权威来源

硬件表永远是 MCU/CPU 当前已提交状态的镜像。路由发现、Cost 比较、负载策略和 Path 安装仍由软件完成。

### 10.2 建议的 Route 表项语义

首版 Route 表可包含：

```text
valid
network_id
destination_node_id
route_epoch
egress_port_id
next_hop_node_id
wire_profile
minimum_mtu
security_policy_id
traffic_class_mask
table_generation
lease_deadline_or_owner_epoch
```

这只是逻辑字段，真正位宽要在 F0 阶段根据 Node ID 宽度、Profile 和 FPGA 资源冻结。

### 10.3 建议的 Path 表项语义

Path 表应至少能表达：

```text
valid
network_id
path_id
source_or_owner
destination_node_id
route_epoch
egress_port_id
expected_next_hop
wire_profile
minimum_mtu
path_generation
lease_deadline
```

### 10.4 查表优先级

建议固定为：

1. 合法 Path ID 且 Path 表命中；
2. 普通 Destination Route 命中；
3. 慢路径上送；
4. 慢路径也不可用时失败关闭并计数。

不得因为 Path 未命中就静默改走普通 Route，除非软件下发的表项或帧策略明确允许回退。

### 10.5 原子更新

不允许 CPU 在 FPGA 正在查询时逐字段修改可见表项。可选择以下一种机制：

**双 Bank 机制：**

1. CPU 写 inactive bank；
2. FPGA 校验全部命令和摘要；
3. CPU 提交新的 generation；
4. FPGA 在帧边界原子切换 active bank；
5. 旧 bank 等待所有引用描述符释放后才可重用。

**单表 Shadow Commit 机制：**

1. CPU 写 shadow entry；
2. 写入完整 valid/generation；
3. FPGA 单周期替换 active entry；
4. 失败时旧 entry 保持不变。

无论采用哪种机制，描述符都必须记录查表时的 generation。出口发送前若 generation 已失效，应重新查表或上送慢路径，不能使用半旧半新的结果。

## 11. MCU/CPU 与 FPGA 控制接口

### 11.1 总线选择

| 平台 | 推荐控制接口 | 适用场景 |
| --- | --- | --- |
| SoC FPGA | AXI4-Lite + AXI DMA/共享 SRAM | Zynq、Cyclone SoC 等高吞吐方案 |
| FPGA + 高性能 MCU | FMC/EBI/QSPI + 中断 | 有并行外部总线或高速串行总线 |
| FPGA + 小型 MCU | SPI + IRQ | 低成本控制，数据面不经过 SPI |
| Host PCIe 平台 | BAR 寄存器 + DMA Ring | Linux 网关或高端骨干节点 |

控制接口带宽可以较低，因为普通帧不经控制接口转发；它只传表项、异常事件、本机帧和统计。

### 11.2 最小寄存器组

建议至少定义：

| 寄存器 | 作用 |
| --- | --- |
| `HW_ID/HW_VERSION` | RTL 版本和兼容标识 |
| `CAPABILITY` | 支持的 Wire Profile、端口、表深、Carrier、可选安全能力 |
| `CONTROL` | 全局启用、软件旁路、软复位、统计快照 |
| `STATUS` | Ready、Fault、Fence、active generation、Ring 状态 |
| `PORT_ENABLE` | 每端口启用位 |
| `PORT_FENCE` | 每端口失败关闭位 |
| `TABLE_GENERATION` | CPU 请求与 FPGA 当前表代数 |
| `IRQ_STATUS/IRQ_MASK` | 路由缺失、慢路径帧、Link Down、overflow、fault |
| `CMD_RING_HEAD/TAIL` | CPU→FPGA 命令 Ring |
| `EVENT_RING_HEAD/TAIL` | FPGA→CPU 事件 Ring |
| `COUNTER_SNAPSHOT` | 统计快照代数/地址 |

### 11.3 命令 Ring

CPU 下发的命令建议包括：

```text
ROUTE_UPSERT
ROUTE_DELETE
PATH_UPSERT
PATH_DELETE
PORT_CONFIG
PORT_ENABLE
PORT_DISABLE
PORT_FENCE_SET
PORT_FENCE_CLEAR
TABLE_PREPARE
TABLE_COMMIT
TABLE_ABORT
STATS_SNAPSHOT
SLOW_FRAME_RELEASE
```

每条命令至少携带：opcode、length、transaction ID、target generation、entry index/key 和 payload checksum。FPGA 只在命令完整合法时更新 shadow 状态。

### 11.4 事件 Ring

FPGA 上报事件建议包括：

```text
ROUTE_MISS
PATH_MISS
PROFILE_MISMATCH
MTU_MISMATCH
LINK_DOWN
INGRESS_OVERFLOW
EGRESS_BACKPRESSURE
CRC_DROP
TTL_DROP
MALFORMED_DROP
SLOW_FRAME_READY
TABLE_COMMIT_DONE
INTERNAL_FAULT
```

事件必须有端口、时间戳、Frame/Descriptor ID、丢弃原因和相关表 generation。事件 Ring 满时，关键 Fault 还要锁存到寄存器，不能只依赖 Ring。

## 12. 内部描述符

一个硬件描述符可包含：

```text
buffer_index
wire_length
ingress_port
ingress_timestamp
wire_profile
message_type
traffic_class
flags
source
destination
session_id
route_epoch
path_id
lookup_generation
egress_port
forward_action
drop_reason
```

描述符字段同样必须在 F0 阶段确定固定宽度。进入某一级流水线时，只允许该级拥有写权限；其他级只能读取，避免多个状态机同时修改 action 或 buffer owner。

## 13. 转发修改与 CRC

快速路径命中后按下列顺序处理：

1. 重新确认出口未 Fence、表 generation 仍有效；
2. 检查 Hop Limit 大于 1；
3. 将 Hop Limit 减一；
4. 保持 Source、Destination、Session、Sequence、Payload 与 Auth Tag 语义不变；
5. 若出口 Wire Profile 不同，按已冻结的合法转换规则重编码；
6. 对新 Header、Payload 和 Auth Tag 重新计算 CRC16；
7. 进入出口 Traffic Class 队列；
8. 由出口 Carrier Encoder 生成物理传输单元。

不能把旧 CRC 沿用到修改后的 Header。CRC 只能检出传输错误，不提供身份认证或抗篡改能力。

## 14. Q0～Q3 硬件队列与仲裁

FPGA 必须保留 UCN 的四级业务意图：

| 等级 | 典型用途 | 首版硬件策略 |
| --- | --- | --- |
| Q0 Critical | 紧急控制、失效保护 | 最高优先，但设置最大连续 burst，避免永久饿死其他队列 |
| Q1 Realtime | IMU、控制状态、实时遥测 | 低等待，按流或入口公平轮转 |
| Q2 Normal | 普通状态、命令结果 | 加权轮转或信用调度 |
| Q3 Bulk | 日志、大块数据、Transfer | 有带宽上限，只使用剩余信用 |

建议的仲裁顺序不是简单永久严格优先，而是“优先级 + 有界 burst + 最低服务信用”：

```text
Q0：每轮最多 B0 帧，然后必须重新仲裁
Q1：预留 R1 比例的发送信用
Q2：保证最低 R2 信用
Q3：仅在不破坏 Q0/Q1 deadline 的剩余窗口发送
```

参数必须由产品配置下发并有安全上下限。

第一版不要急于在硬件实现复杂的 Q1 latest-value 合并。可以先使用有界 FIFO；若后续需要覆盖旧传感器样本，应由 CPU 为 Flow 分配稳定的 hardware flow slot，再实现“同 Flow 未发帧只保留最新值”，不能仅凭 Source/Destination 粗暴覆盖。

## 15. 各类 Bearer 的具体接入

### 15.1 UART / RS-485

需要实现：

- 可配置波特率、数据位、停止位和校验；
- RX/TX FIFO；
- COBS 编解码与分隔符检测；
- 帧超时和超长帧恢复；
- RS-485 DE/RE 时序；
- 可选硬件流控；
- break、framing、parity、overflow 独立计数。

在 1～3 Mbaud 时，线上的串行时间通常仍是主要部分。FPGA 能减少软件调度和复制开销，但不能消除每字节物理传输时间。

### 15.2 CAN-FD

需要实现：

- CAN 控制器或外部控制器接口；
- DLC 与真实长度转换；
- UCN Frame 长度和 padding 验证；
- non-zero padding 失败关闭；
- Arbitration ID 到 ingress port/policy 的映射；
- bus-off、error passive、重传统计上报。

若当前 UCN Frame 能放入一个 CAN-FD 物理帧，优先使用单物理帧交付。

### 15.3 Classic CAN

需要复用现有 Carrier 语义：

- START/CONTINUE/END 或当前冻结的片段格式；
- 片段序号、总长度、Carrier ID 校验；
- 每个并发重组槽固定 Buffer；
- 超时与新 START 冲突处理；
- 完成槽必须先提交，不能被下一条 START 覆盖。

Classic CAN 的物理载荷很小，Carrier 重组延迟通常明显高于 FPGA 内查表延迟。

### 15.4 USB

优先顺序建议为：

1. USB Device Bulk，FPGA 作为复合设备中的数据接口；
2. 外部 USB Controller + FIFO 总线；
3. 具有原生 USB 硬核的 SoC FPGA。

USB 收到的是 packet，不代表自动等于一个 UCN Frame；仍要规定 stream framing、短包、ZLP、断线和重新枚举后的状态恢复。

### 15.5 Ethernet / UDP / IP

建议分两阶段：

- 第一阶段由 MCU/Linux 网关把 UCN 封装到 UDP/TCP/QUIC 等隧道，FPGA 只处理本地 UCN 端口；
- 第二阶段才考虑在 FPGA 内加入 MAC、IP/UDP parser 和 UCN tunnel endpoint。

跨互联网时，路由器只转发外层 IP 包，并不理解 UCN 内部 Path。UCN 节点负责隧道两端的封装和解封。硬件路由器能减少中间互联网部分的 UCN 软件跳数，但不能替代两端 UCN 的身份、路由域和安全处理。

## 16. 安全边界

### 16.1 E2E 密文

中间 FPGA 节点可以转发 E2E 加密帧，因为转发只依赖 Header，Payload 和 Auth Tag 对它是不透明数据。必须保证：

- 不解密业务 Payload；
- 不修改 Payload/Auth Tag；
- 只修改允许逐跳变化的字段；
- CRC 按修改后的完整线帧重算；
- 终点仍由软件安全 Provider 解密和认证。

### 16.2 控制帧

首版将所有能改变 Route、Path、Cluster、身份、密钥或权限的控制消息送 MCU/CPU。没有完成抗重放、密钥隔离和硬件认证审计前，不能让 FPGA 因收到网络帧而直接写转发表。

### 16.3 控制接口

- 表写权限只属于可信 CPU；
- SPI/AXI 命令要有 transaction/generation 检查；
- 外部可访问总线需要认证或物理信任边界；
- FPGA bitstream、CPU firmware 和配置应纳入 Secure Boot/版本签名；
- Debug 口在产品模式必须有明确关闭策略。

## 17. 故障、Fence 与恢复

| 故障 | FPGA 动作 | MCU/CPU 动作 |
| --- | --- | --- |
| Route/Path miss | 上报慢路径，不自行猜测出口 | 发起发现或安装新表项 |
| Link Down | 立即 Fence 对应出口，保留统计 | 撤销路由、发 RERR、选择新路径 |
| 出口队列满 | 按 Q 等级执行有界背压/丢弃并计数 | 调整 Cost、流控或业务速率 |
| CRC/格式错误 | 丢弃，不进入 CPU 普通队列 | 读取统计，判断线路质量/攻击 |
| FPGA 内部 fault | 全局 Fence，停止快速转发 | 复位、重新加载表或软件旁路 |
| CPU 复位 | 表项租约到期后自动 Fence，或立即全局 Fence | 启动后重新初始化和提交 generation |
| FPGA 复位 | 默认所有端口/表项无效 | CPU 完整重放配置后显式启用 |
| Event Ring 满 | 锁存 critical fault；普通事件饱和计数 | 尽快排空并判断是否降级 |

最重要的原则是：Link Down 后 FPGA 不应自行选择“看起来能通”的另一个端口。新出口必须由 CPU 当前 Route/Path/Policy 决策明确安装。

## 18. CPU 与 FPGA 双重转发的防护

同一帧不得同时由 FPGA 快速路径和 CPU 慢路径各转发一次。建议使用严格的 action ownership：

```text
FAST_FORWARD：Buffer 只属于 FPGA，CPU 只接收统计
LOCAL_DELIVER：Buffer 交给 CPU，本帧 FPGA 不转发
SLOW_ROUTE：Buffer 交给 CPU，释放前 FPGA 不再使用
DROP：FPGA 释放 Buffer，只上报元数据
```

handoff 描述符必须含唯一 Frame ID。CPU 完成慢路径处理后，通过 `SLOW_FRAME_RELEASE` 归还 Buffer。超时归还只能触发 fault/Fence，不能无声复用仍被 CPU 引用的 Buffer。

## 19. 性能模型与合理预期

单跳延迟可以拆成：

```text
T_hop = T_ingress_serial
      + T_carrier_decode
      + T_full_frame_validate
      + T_lookup
      + T_hop_crc_rewrite
      + T_egress_queue
      + T_egress_serial
```

FPGA 主要降低：

- `T_carrier_decode` 的软件开销和抖动；
- `T_lookup`；
- `T_hop_crc_rewrite`；
- CPU 任务通知、调度和内存复制；
- 多端口之间由单任务串行服务造成的等待。

FPGA 不能消除：

- UART/CAN/无线的物理传输时间；
- 半双工总线争用；
- 出口队列等待；
- 每跳必须完整收帧后校验的等待；
- 多个无线节点共享同一信道导致的容量下降。

因此 FPGA 对“多个独立高速有线端口并行转发”收益最大；对“所有跳都共享同一低速无线信道”的提升有限。

第一版可把内部目标设为：在 100 MHz 或更高时钟下，完整帧到齐后的 parser + lookup + descriptor 决策延迟达到确定性的微秒级以内。但这只是设计目标，必须经过综合时序、板级测量和压力测试后才能写入官方性能指标。

### 19.1 多跳吞吐

- 若每一段 Link 相互独立且支持全双工，流水线充满后可以多段同时传输不同帧；吞吐不必按跳数线性除法下降；
- 若多跳共享同一无线频道或同一半双工总线，每转发一跳都会再次占用共同介质，吞吐会明显下降；
- Store-and-Forward 增加单帧端到端延迟，但允许相邻链路对不同帧形成流水线；
- FPGA 能降低中间处理空隙，却不能突破最慢 Link 的物理容量。

## 20. 资源预算与可裁剪档位

### 20.1 计算方法

BRAM 的主要来源为：

```text
Frame Pool ≈ Buffer_Count × Aligned_Max_Wire_Bytes
Ingress FIFO ≈ Σ(每端口 RX FIFO)
Egress FIFO ≈ Σ(每端口 × 每 Q 等级队列深度)
Route Table ≈ Route_Count × Route_Entry_Bits
Path Table ≈ Path_Count × Path_Entry_Bits
Command/Event Ring ≈ Ring_Count × Descriptor_Bits
```

LUT/FF 的主要来源为：Carrier、并行端口、CRC 单元、查表结构、仲裁器和控制总线。

### 20.2 建议的起始档位

| 档位 | 端口 | Route | Path | Frame Buffer | 用途 |
| --- | ---: | ---: | ---: | ---: | --- |
| FPGA-Lite | 2～4 | 32～64 | 16～32 | 8～16 | 两段/小型汇聚转发 |
| FPGA-Gateway | 4～8 | 256 | 128 | 32～64 | 多 Bearer 网关、测试骨干 |
| FPGA-Backbone | 8～16+ | 1024+ | 512+ | 128+ | 高端骨干或集中汇聚 |

这些数字只是综合前的起始参数，不是当前 UCN 的硬性容量承诺。实际值要按器件 BRAM、并行端口数、最大 Wire Frame 和产品拓扑重新测算。

## 21. FPGA 器件与系统形态选择

### 21.1 FPGA + 独立 MCU

优点：

- MCU 延续现有 UCN C 代码；
- FPGA 只做数据面，职责清楚；
- 小型设备可选择低成本 FPGA。

缺点：

- 需要额外控制总线和复位协同；
- 双芯片供电、PCB 和升级流程更复杂。

### 21.2 SoC FPGA

优点：

- CPU 和 FPGA 共享内存/AXI；
- 适合 Ethernet、DMA 和大表；
- 软件/硬件协同效率最高。

缺点：

- 成本、功耗和软件栈复杂度较高；
- 不适合替代资源极小的普通 MCU 节点。

### 21.3 FPGA + Soft Core

优点：单芯片、控制面可裁剪。
缺点：Soft Core 性能和工具链成本需要单独评估，且现有 MCU BSP 不能直接复用。

选择器件时至少核对：逻辑单元、BRAM、PLL/时钟域、原生 SerDes/MAC/CAN/USB 能力、I/O 电压、温度、功耗、bitstream 安全和长期供货。

## 22. 建议的软件与 RTL 目录

真正开工时建议新增独立目录，不把 FPGA 逻辑塞入现有 Adapter 文件：

```text
include/ucn/accelerators/
  ucn_hw_forward.h              # CPU 侧抽象接口
  ucn_hw_forward_contract.h     # 描述符、命令、事件和能力版本

src/accelerators/
  ucn_hw_forward.c              # 与 Node/Route/Path 的软件桥
  ucn_hw_forward_table.c        # 表项镜像与原子提交
  ucn_hw_forward_fallback.c     # 软件旁路/慢路径

src/adapters/fpga/
  ucn_fpga_control_spi.c
  ucn_fpga_control_mmio.c
  ucn_fpga_slow_source.c

rtl/ucn_forward/
  ingress/
  carrier/
  codec/
  table/
  qos/
  egress/
  control/
  assertions/

tests/hw_forward/
  reference_model/
  vectors/
  rtl/
  differential/
  fault_injection/
```

以上目录当前不应提前创建空壳；它表示未来实现时的模块边界。

## 23. CPU 侧抽象接口建议

公共 API 不应暴露 AXI/SPI 细节，可抽象为：

```c
typedef struct ucn_hw_forward ucn_hw_forward_t;

ucn_error_t ucn_hw_forward_init(
    ucn_hw_forward_t *forward,
    const ucn_hw_forward_config_t *config);

ucn_error_t ucn_hw_forward_get_capabilities(
    const ucn_hw_forward_t *forward,
    ucn_hw_forward_capabilities_t *capabilities);

ucn_error_t ucn_hw_forward_prepare_generation(
    ucn_hw_forward_t *forward,
    uint32_t generation);

ucn_error_t ucn_hw_forward_route_upsert(
    ucn_hw_forward_t *forward,
    uint32_t generation,
    const ucn_hw_route_entry_t *entry);

ucn_error_t ucn_hw_forward_path_upsert(
    ucn_hw_forward_t *forward,
    uint32_t generation,
    const ucn_hw_path_entry_t *entry);

ucn_error_t ucn_hw_forward_commit_generation(
    ucn_hw_forward_t *forward,
    uint32_t generation);

ucn_error_t ucn_hw_forward_service(
    ucn_hw_forward_t *forward,
    uint32_t now_ms,
    uint32_t event_budget);
```

这只是 API 形态建议。正式头文件必须在 C 参考模型、描述符 ABI 和重入/Owner 契约冻结后再创建。

## 24. 实施阶段

### F0：冻结硬件转发合同

工作内容：

- 从当前 C Codec 自动导出各 Wire Profile 的 offset、mask、长度和 CRC 向量；
- 冻结 fast/slow/drop 分类表；
- 冻结 Route/Path 表项语义；
- 冻结 descriptor、command、event 和 generation 协议；
- 冻结 CPU/FPGA 复位、Fence 和 Owner 规则；
- 明确第一版支持的 Bearer 和最大 Frame。

交付物：合同头文件、机器可读 schema、golden vectors、异常矩阵和架构评审记录。

完成标准：软件、RTL 和测试使用同一份生成源，不存在人工维护的第二份 Wire offset。

### F1：建立 C 语言参考快速转发模型

不要先写 HDL。先写一个不依赖 Node 私有状态的纯 C 模型：

```text
输入：完整 wire bytes + ingress metadata + frozen table snapshot
输出：FAST / SLOW / DROP + egress metadata + 新 wire bytes
```

模型必须复用或逐位对照现有 Codec，覆盖：CRC、Hop、Profile、Path/Route、E2E 密文、错误不写回和 generation 切换。

完成标准：同一批输入分别经过现有软件 Node 转发语义和参考模型，结果逐字节一致；所有非法输入失败关闭。

### F2：两端口 UART 原型

实现最小 RTL：

- 2 个 UART；
- Stream/COBS Carrier；
- 单一 Wire Profile；
- 32～64 条静态 Route；
- Store-and-Forward；
- Hop Limit 和 CRC 重写；
- Q0～Q3 基础队列；
- SPI/AXI-Lite 控制；
- 软件旁路。

完成标准：FPGA 与 C 模型逐帧一致；1/2/3 跳相同波特率下统计吞吐、P50/P95/P99 延迟和丢弃原因。

### F3：动态 Route/Path 与原子表更新

实现双 Bank 或 Shadow Commit、Route/Path 优先级、generation fence、route miss 慢路径和 Link Down 事件。

完成标准：持续流量中反复安装/撤销路由，不出现旧新表混用、重复转发或越权回退。

### F4：多端口与 QoS

扩展到 4～8 端口，加入每端口四级队列、信用仲裁、Q0 burst 上限、Q1 延迟测量和 Q3 限速。

完成标准：Q3 满载下 Q0/Q1 仍满足冻结的延迟上限；任一端口阻塞不拖死无关端口。

### F5：CAN-FD、Classic CAN 与 USB

逐个增加 Carrier；每增加一种都必须复用统一 Frame Buffer/Descriptor/Route/QoS 管线，不能形成另一套协议实现。

完成标准：每个 Carrier 的 golden、截断、padding、重组冲突、超时、bus reset 和多端口并发全部通过。

### F6：IP/Ethernet 骨干

先实现软件 tunnel endpoint，再评估是否把 MAC/UDP 封装下沉 FPGA。公网使用时必须增加身份认证、防重放、MTU/分片和拥塞边界。

完成标准：UCN 路由域与外层 IP 路由域分离清楚；隧道断开不会产生幽灵 Route。

### F7：可选安全硬件加速

只有在软件安全合同冻结后才评估 AES/GCM/ChaCha20-Poly1305、密钥槽和 anti-replay 下沉。默认仍让中继只转发 E2E 密文。

完成标准：密钥不可由普通控制命令读出；nonce/replay/power-loss 合同完成专门审计。

### F8：产品化与发布

包含：目标器件时序收敛、资源报告、功耗、温度、EMC、长稳、故障注入、升级/回滚、bitstream 签名和生产测试。

完成标准：软件旁路和 FPGA 快速路径在同一测试矩阵中语义一致；实机证据可追溯到 firmware、bitstream、接线和配置 hash。

## 25. 详细测试体系

### 25.1 单元测试

- 每个 Carrier 的 encode/decode；
- CRC golden 与单 bit 错误；
- Header 各字段上下界；
- Route/Path hit/miss/conflict；
- Hop Limit 1、2、最大值；
- generation commit/abort；
- Buffer 分配/释放与所有权；
- Q0～Q3 仲裁；
- Event/Cmd Ring 回绕；
- 计数器饱和。

### 25.2 差分测试

用同一输入同时运行：

```text
现有 UCN C Codec/转发语义
C 硬件参考模型
RTL 仿真模型
FPGA 实机
```

比较 fast/slow/drop 决策、出口、Hop、完整 Wire bytes、CRC 和统计。随机 fuzz 只能补充，不能替代确定性的字段矩阵。

### 25.3 形式验证/断言

至少加入下列断言：

- 一个 Buffer 同时只有一个 Owner；
- 无 valid route 不产生 egress enqueue；
- CRC 未通过不产生 fast-forward；
- Hop Limit ≤ 1 不产生 forward；
- Fence 端口不产生发送；
- generation 未提交不对查询可见；
- FIFO 不读空、不写满；
- slow-path frame 不会又被 fast-path 转发；
- reset 后所有 authority-like 状态默认无效。

### 25.4 压力与故障注入

- 所有入口同时满速；
- 单出口拥塞、多入口竞争；
- Q0/Q1/Q2/Q3 混合；
- Route 表反复切换；
- CPU 在 submit/poll 中断或复位；
- FPGA 各 clock domain 独立复位；
- Link 在帧中途断开；
- Event Ring/Cmd Ring/Buffer Pool 满；
- CRC、长度、序列、Carrier 片段随机损坏；
- 1/2/3/更多跳流水线和共享介质对照。

### 25.5 实机指标

至少记录：

```text
有效 Payload 吞吐 B/s
Wire 吞吐 B/s
P50/P95/P99/最大单跳与端到端延迟
每 Traffic Class 排队时间
CPU 占用与中断率
FPGA LUT/FF/BRAM/DSP 使用率
时钟频率与 worst negative slack
每端口 overflow/backpressure/drop
CRC/Carrier/Link 错误
功耗和温升
```

不能只记录平均吞吐；实时性必须同时看尾延迟和队列水位。

## 26. 必须长期保持的安全不变量

1. CRC 未验证成功的帧绝不快速转发。
2. 中间节点转发时 Hop Limit 必须严格减一。
3. Hop Limit 不足的帧不能继续转发。
4. E2E Payload 与 Auth Tag 在中继处逐字节不变。
5. FPGA 只使用已经原子提交的 Route/Path generation。
6. Link/端口/Fault Fence 生效后不能发送新帧。
7. 同一 Frame 只能由 Fast、Slow、Local、Drop 中的一个动作拥有。
8. 控制帧不能直接修改硬件 Route/Path 表。
9. 不支持的 Profile/类型必须慢路径或拒绝，不能降级猜测。
10. Buffer、Descriptor、Ring 满时不能覆盖仍在使用的数据。
11. CPU/FPGA 任一方复位后默认无有效转发权限。
12. 普通 MCU-only 网络不依赖 FPGA 才能发现、选路和恢复。

## 27. 发布、灰度与回退

建议采用四个运行模式：

| 模式 | 行为 | 用途 |
| --- | --- | --- |
| OFF | FPGA 不参与，全部走现有软件 | 默认与安全回退 |
| MIRROR | FPGA 只计算结果，不实际发送；与软件结果比较 | 早期实机差分 |
| SELECTIVE | 仅白名单端口/类型/Route 走硬件 | 灰度验证 |
| FULL | 已支持的普通数据走硬件，其他走慢路径 | 完成验收后的产品模式 |

从 MIRROR 升级到 SELECTIVE/ FULL 必须由配置明确开启，不能因为检测到 FPGA 存在就自动启用。

回退时：

1. 先全局 Fence FPGA 新入口；
2. 等待或丢弃在途 Buffer，并记录结果；
3. CPU 恢复软件 Link/Adapter；
4. 重新建立 Neighbor/Route/Path 所需状态；
5. 确认软件路径可用后才释放业务；
6. 保留 FPGA fault snapshot 供诊断。

## 28. 开工前决策清单

真正实施前必须先回答：

- 第一块 FPGA/SoC 的具体型号是什么？
- 第一版支持几个端口，分别是什么 Bearer？
- 最大 Wire Frame 和支持哪些 Wire Profile？
- 只做中继，还是也做本机 Endpoint？
- Route/Path 表各需要多少条？
- CPU 与 FPGA 采用哪种总线？
- 是否要求跨时钟域、多电压域或热插拔？
- Q0/Q1 的最大允许 P99 延迟是多少？
- Link Down 后允许保留多少在途帧？
- 是否允许软件慢路径背压物理入口？
- 是否存在公网/不可信物理访问？
- 产品是否要求 Secure Boot 和加密 bitstream？
- 必须支持怎样的在线升级与回滚？

这些问题未确定前，不应冻结 RTL 位宽和资源参数。

## 29. 推荐的第一步

如果以后决定实施，不要直接从 Verilog/VHDL 开始。正确顺序是：

1. 选定“两 UART、Store-and-Forward、单一 Wire Profile”的最小场景；
2. 从当前 C Codec 生成 bit-exact golden vectors；
3. 写 F1 纯 C 硬件参考模型；
4. 用现有 UCN 软件转发与参考模型做差分；
5. 冻结 descriptor、Route entry、command/event ABI；
6. 再实现 RTL；
7. 先跑仿真和形式断言，再上板；
8. 上板先用 MIRROR 模式，最后才允许实际转发。

这样可以先证明“语义一致”，再证明“速度更快”，避免做出一个吞吐很高但与 UCN 路由、安全、Hop 或故障语义不一致的独立协议。

## 30. 最终定位

FPGA 加速节点适合成为：

- 多 UART/CAN/USB 的集中汇聚器；
- 高速有线骨干转发器；
- 无人系统内部的确定性低抖动交换节点；
- UCN 与 Ethernet/IP 骨干之间的网关数据面；
- 多 MCU 网络中的高性能中继站。

它不改变 UCN 的核心定位：普通 MCU 仍能使用相同协议完成发现、寻址、自动选路、跨 Bearer 转发和故障恢复。FPGA 只是把已确定的转发工作硬件化，使端口更多、并行度更高、处理时间更确定。

在没有真实 RTL、目标器件综合结果、差分测试和实机多跳证据之前，只能称为“可实施的架构方案”，不能称为 UCN 当前已经具备的能力。
