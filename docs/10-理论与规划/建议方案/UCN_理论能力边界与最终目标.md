# UCN 理论能力边界与最终目标

> 状态：面向 UCN v5 预发布工作树的目标定义与能力边界，不是已经发布的产品规格。
> 日期：2026-08-14
> 依据：当前 Core、Extended Transfer、Service、Port/Adapter、Host 模拟、规模模拟和四块 ESP32-S3 UART 实测。
> 原则：本文严格区分“已经实现”“最终应实现”“字段可表示极限”和“真实产品可承诺值”。

## 1. 一句话结论

UCN 的最终目标是成为一个 **以 MCU 为主体、无需 Linux、跨有线与无线介质、资源有界且可裁剪的统一通信网络层与轻量服务层**。

应用最终只需要表达：

```text
把什么数据，以什么实时性/可靠性/安全要求，发给哪个 Node 的哪个 Endpoint
```

至于数据最终经过 UART、CAN、CAN-FD、RS-485、USB、Wi-Fi、ESP-NOW、BLE、LoRa，是否需要中继、当前哪条路径更合适、主链路失效后怎样恢复，应由 UCN 与产品 Adapter 共同完成。

UCN 不应该变成另一个 Linux 网络栈，也不应该把所有功能塞入每块小 MCU。最终形态应一直保持：

- `UCN-Core`：每个需要组网的 MCU 都能承担的最小闭环。
- `UCN-Extended`：大消息、可靠传输、服务发现、组播、时间同步等按需能力。
- `UCN-Host`：Linux、ROS 2、MAVLink、地面站和调试工具的可选接入层。
- Product Port / Adapter：真正连接具体 RTOS、驱动、控制器、引脚和硬件队列。

## 2. UCN 最终要解决什么问题

### 2.1 对产品开发者

完成一次平台对接后，业务不再为每种介质各写一套通信流程：

1. 产品配置本机 Node ID、Network ID、资源档位和安全策略。
2. UART/CAN/Wi-Fi 等驱动通过统一 Adapter/Source 注册为 Link。
3. IMU、气压计、电机、舵机、参数服务等本机任务注册为 Endpoint/Service。
4. 业务按目标 Node + Endpoint 发送，不直接操作路由表和物理地址。
5. UCN 自动使用直连链路或按需寻路，按策略选择 Bearer/Route/Path。
6. 中继只根据协议头转发；端到端密文不要求中继解密。
7. 目标任务处理消息；需要业务结果时，通过独立 Result Endpoint 返回。

### 2.2 对网络本身

UCN 最终应让网络具备以下性质：

- 没有 Linux、云端、路由器或固定中心时，MCU 仍能发现邻居、建立路线和相互通信。
- 新节点经授权后可以动态加入；节点离开或链路失效后，相关状态有界回收。
- 一个节点可以同时拥有多个 UART、多个 CAN 控制器和多个无线接口，每个实例都可成为独立 Link。
- 路由只保存当前活跃工作集，不要求每个节点保存全网完整拓扑。
- 正常业务不逐帧寻路；Route/Path 有效时直接查表转发。
- 关键控制、实时状态、普通消息和大数据使用不同资源与调度规则，互不无限挤占。
- 所有可增长状态都有编译期上限；表满、超时、不可达和安全失败必须显式报告。

## 3. 最终能力全景

下表中的“当前状态”以 2026-09-04 工作树为准；软件测试不自动等于真实硬件与生产安全已通过。

| 能力域 | 最终应具备的能力 | 当前状态 | 仍需完成的关键项 |
| --- | --- | --- | --- |
| MCU 独立组网 | 无 Linux完成一跳发现、有限多跳、转发、失效回收与恢复 | Core 已实现 HELLO、Heartbeat、AODV-Lite、RERR、老化和多跳 | 生产 Join、更多真实 Bearer 与大规模实机 |
| 统一身份 | 物理 MAC/CAN ID 与逻辑 Node ID 解耦；默认生成并允许产品配置 | 32 bit Node ID、Network ID、手动/Provider 准入边界已存在 | 正式出厂身份、地址分配/冲突处理和域间 Alias |
| 多介质统一 | UART/CAN/Wi-Fi 等都映射为 Link，Core 不感知 SDK | Link、Adapter、Event Runtime、Stream Source、CAN Source 已有公共接口 | 各平台真实 BSP、USB/Wi-Fi/BLE/LoRa 等参考 Adapter |
| 多 Bearer | 同一邻居可通过多条物理链路连接，支持主备和动态选择 | Neighbor/Bearer、动态指标、LC-1 本地有效 Cost 已实现 | 各介质默认 Cost 的目标板标定和长期切换实测 |
| 自动寻路 | 未知目的地按需扩圈，沿途只保存下一跳 | 2→4→8→16 AODV-Lite、RREQ/RREP/RERR 与 `(traffic_origin,destination)` Route Instance 已实现并完成 Host 软件矩阵 | 多 Origin 多板/ESP-NOW、广播风暴和故障压力实机 |
| 指定路径 | 业务可固定走指定路径，可严格固定或断路后回退 | Full 的 Path 安装/撤销、`PINNED_STRICT`/`PINNED_FAILOVER` 已实现 | 管理工具、跨 Bearer 安装和真实断链验收 |
| 自动负载分担 | 在满足业务约束的候选中分散 Q1 Flow，而不是逐帧乱跳 | Full 的 `AUTO_BALANCE`、Flow 亲和和动态本地评分已实现 | 多流、多介质、共享空口压力与公平性标定 |
| 无缝换路 | 先验证候选，再切换；旧 Epoch 在短暂 Grace 内继续可用 | Probe/ACK、Activate/ACK、Current/Previous Epoch 已按 Origin 分域实现；同 candidate ID 多域软件回归已覆盖 | 真实丢包/乱序、多 Bearer 和切换中 Transfer 验收 |
| 实时 QoS | 关键控制优先、实时状态保留最新值、过期数据主动丢弃 | Q0 FIFO、Q1 Latest、Deadline、维护公平和有界背压重试已实现 | Q0 端到端时延预算、产品 WCET 与失联安全实测 |
| 大消息 | 小消息直发；大消息按 MTU 分片、重组、校验、ACK 与有界重试 | T32～T8K、CRC32、窗口 1～8、累计 ACK 已实现 | 自动能力协商、多源并发、切换中传输、其他 Bearer |
| 节点内任务通信 | 一个 MCU 通常只有一个 Node；多个任务挂载为 Endpoint/Service | 固定 Router、Inbox、Remote TX、Bridge 和本机 Fast Path 已实现 | 各 RTOS 完整参考接入、任务重启与实时性实机 |
| 跨 MCU 服务调用 | 远端命令送到指定任务，任务可返回明确业务结果 | Endpoint/Service Bridge 和 Result Endpoint 模式可实现 | 标准请求 ID、超时/取消/幂等服务契约可继续封装 |
| 安全 | 认证入网、控制面逐跳认证、业务端到端 AEAD、重放防护和密钥轮换 | Provider、ACL、Session/Sequence、E2E seal/open 与透明密文中继边界已实现 | 受审计 AEAD、真实密钥存储、Join、轮换和攻击注入 |
| 诊断 | 按需查路径、节点快照、策略状态和资源统计；普通帧零额外诊断字段 | Path Trace、Node Snapshot、Policy Diagnostic 已实现 | PC/Host 可视化、抓包、统一运维工具和权限验收 |
| Host 兼容 | Linux/ROS 2/MAVLink/PX4/地面站作为普通节点或受控网关 | Host 非中心化边界与虚拟 Link 已验证 | UDP/SocketCAN/ROS 2/MAVLink 正式 Adapter/Bridge |
| 可裁剪资源 | 小 MCU 只支付需要的功能和固定表内存 | Nano/Lite/Full、Service ON/OFF、Extended 独立链接已实现 | 各 MCU 系列的 Flash/RAM/栈/CPU/功耗基线 |
| 工程可移植 | 裸机和不同 RTOS 使用相同 Core，平台差异只留在 Port | 独立 Port 文件、API V2 和通用事件 Owner 已建立 | FreeRTOS/Zephyr/NuttX/RT-Thread 的真实 SDK Glue 与 CI |

## 4. 最终可以做什么

### 4.1 传感器与执行器网络

- 一个传感节点同时发布 IMU、气压、温度等多个 Endpoint。
- 另一个节点持续订阅实时状态，并向舵机、电机或继电器 Endpoint 发送控制。
- 同一 MCU 内多个任务走本机 Fast Path，跨 MCU 时自动进入 UCN 网络。
- 执行器收到命令后，可将执行结果、拒绝原因或当前状态返回来源节点。

不需要“每个传感器一颗 MCU”。通常是一块 MCU 对应一个 Node，MCU 内的任务/设备由 Endpoint 区分。

### 4.2 混合有线与无线自组网

- 近距离节点走 UART、CAN-FD 或 RS-485。
- 移动节点走 Wi-Fi、ESP-NOW、BLE 或其他无线承载。
- 中继节点可在不同介质之间转发，应用看到的仍是同一个 Node/Endpoint 地址空间。
- 同一对节点可以同时保留有线 Primary 与无线 Backup，或按实时质量动态选择。

### 4.3 飞控、机器人和分布式控制系统

- Q0 承载少量关键控制或安全消息。
- Q1 承载 IMU、姿态、位置、电池等“最新值比旧值更重要”的实时状态。
- 固定 Path 可隔离关键控制流；自动 Route 可服务一般遥测。
- 本地失联动作由执行器节点自己完成，不能依赖 Linux 在线。

UCN 可以负责命令到达目标任务和结果返回，但不替代电流环、姿态内环、PWM/FOC 等本地硬实时闭环。

### 4.4 诊断、配置和维护

- 按需查询 A 到 D 实际经过哪些节点。
- 低频查询当前可见节点快照，不要求全网常驻拓扑。
- 查询某个节点的 Route Policy、Path、Flow 与链路质量结果。
- 通过受权限 Endpoint 修改参数或启动诊断。
- 最终可由 Host 工具绘制网络、查看错误和导出性能记录。

### 4.5 大消息和受控数据传输

- 32 B～8 KiB 逻辑消息按等级选择资源上限。
- 根据当前 Path MTU 自动分片，在目标端校验并重组。
- 中继不缓存完整 8 KiB，只转发一个个普通 UCN Fragment。
- 更大的日志、固件或文件不应扩成“超大单帧”；最终应使用独立的分块流/文件协议、断点状态和 Q3 限速。

## 5. 理论极限：必须分四个口径

### 5.1 地址字段能表示多少节点

| Wire Profile | 地址宽度 | 可用单播 Node ID 数 | Wire 最大 Hop | 当前默认 Build 实际最大 Hop |
| --- | ---: | ---: | ---: | ---: |
| W0 | 1 B | 254 | 4 | 4 |
| W1 | 2 B | 65,534 | 16 | 16 |
| W2 | 3 B | 16,777,214 | 64 | 16 |
| W3 | 4 B | 4,294,967,294 | 254 | 16 |

这里的几十亿只是 **地址空间**，不是并发容量。真实网络还受到以下因素限制：

- 每节点 Link、Neighbor、Route、Discovery、Flow 和去重窗口容量。
- 无线信道、CAN 总线、UART 点对点拓扑等物理带宽与冲突。
- 节点启动风暴、广播范围、活跃源数量和通信模型。
- RAM、CPU、调度周期、功耗和安全算法。

当前默认单节点只注册 4 个 Link、维护 8 个 Neighbor、8 条远端 Route 和 4 个并发 Discovery。这是一种“全网可以较大，但每个节点只关注少量活跃目标”的稀疏网络模型。

同一扁平域只要必须经过 W0 节点，域内地址就要受 W0 的 254 个单播地址限制。最终若要把很多低资源域连在一起，应使用 Domain/Gateway/Alias，而不是强迫每个小节点认识全世界。

### 5.2 网络最终应该支持多少节点

UCN 不应给出脱离拓扑和负载的单一节点数字。建议把最终验证目标分级：

| 产品级别 | 建议目标 | 典型通信模型 | 要求 |
| --- | ---: | --- | --- |
| 小型控制网 | 2～16 Node | 多数节点可直接或少量多跳互通 | Full/Lite 实机全功能、故障和时延门禁 |
| 中型设备网 | 17～128 Node | 稀疏邻居、少量稳定目的地 | 启动风暴、共享介质、表容量和多源压力门禁 |
| 大型稀疏网 | 129～1024 Node | 分区、层次化、每节点小工作集 | Domain/Gateway、管理面和多信道设计，不做全对全 |
| 超大系统 | 超过 1024 Node | 多个 UCN 域由 Host/网关互联 | 不应做单信道、单广播域、全网平铺 Mesh |

这些是 **最终工程目标分级**，不是当前实机承诺。当前 Host 模拟已经覆盖 4096 Node 的稀疏独立 Link 场景，但它不能代表 4096 块 MCU 共用一个无线信道。

### 5.3 跳数应该到什么程度

- W3 字段最多表示 254 Hop；当前 Build 有意限制为 16 Hop。
- 产品建议首先把网络直径控制在 4～8 Hop，并在真实介质上测量。
- 16 Hop 是当前通用软件上限，不代表所有产品都应开放到 16。
- 不建议把 Hop 扩到 65,534。跳数越大，累计序列化、排队、重传、控制面扩散和失效收敛代价越高，已经失去 MCU 实时网络的意义。

最终目标不是追求“跳得最远”，而是在 **有限直径内可预测地通信和恢复**。超过产品直径时，应增加网关、分域、长距离物理链路或重新设计拓扑。

### 5.4 帧和消息能有多大

当前边界：

- Core 默认最大帧为 256 B，静态 Payload Buffer 为 224 B。
- v5 普通头为 W0/W1/W2/W3 的 17/21/26/30 B；Route/Path 和 16 B E2E Tag 会进一步占用空间。
- Extended Transfer 固定支持 T32、T64、T128、T256、T512、T1K、T2K、T4K、T8K。
- 当前 Transfer 的明确编译上限是 8 KiB，不是任意长度。

最终设计应坚持：

1. Core 帧保持小而有界，不能为了文件传输把每个 Node 的队列都放大。
2. 8 KiB 内使用有界 Transfer。
3. 超过 8 KiB 的日志、地图、固件和文件使用可选流式分块协议。
4. 流协议一次只占固定窗口和固定缓冲，不要求接收端一次性分配整个文件大小。
5. Q3 大数据必须限速，并为 Q0/Q1 保留独立预算。

因此，UCN 最终可以传很大的文件，但不是靠“定义一个无限大的帧”，而是靠持续、可恢复、有配额的分块传输。

### 5.5 吞吐极限

UCN 不创造底层带宽。端到端吞吐上限由最慢 Link、介质共享方式、协议头、ACK、重传和 Hop 数共同决定。

粗略上界：

```text
单跳有效吞吐 <= 物理有效吞吐 × Payload / Wire Bytes

同一半双工共享介质的 H 跳线形网：
端到端吞吐通常随 H 增加而明显下降，最坏可接近单跳吞吐 / H

独立全双工 Link 且可流水：
稳态吞吐可接近最慢一段 Link 的有效吞吐，但单消息时延仍累加每一跳开销
```

当前四块 ESP32-S3、三段独立 3 Mbaud UART、T8K 自动路线的中位吞吐为：

| 路径 | 中位吞吐 | 相邻损失 |
| --- | ---: | ---: |
| 1 Hop | 46.109 KiB/s | — |
| 2 Hop | 37.209 KiB/s | 19.303% |
| 3 Hop | 32.587 KiB/s | 12.422% |

三跳相对一跳下降 29.327%。这只是当前 UART Bench、单源、T8K、窗口配置下的证据，不代表 Wi-Fi、CAN-FD 或多源结果。

最终性能目标应该是：

- 短控制消息优先低时延和明确过期，不以大吞吐为第一目标。
- 大消息使用窗口、批处理和事件驱动减少 Stop-and-Wait/轮询损耗。
- 独立路径可以并行；共享 MCU、总线、无线信道和协议任务时必须公平调度。
- Adapter `send()` 只做有界入队，不等待物理发送完成。

### 5.6 时延和实时性极限

端到端最低时延可表示为：

```text
总时延 >= Σ(每跳序列化 + 介质仲裁 + 驱动排队 + 中继处理) + 端点任务调度
```

UCN 最终应提供的是：

- Q0/Q1 优先级与有界队列。
- Deadline、Latest Value、过期丢弃和明确背压。
- Link/Task WCET 与最大 Step 间隔的产品契约。
- 路由建立、断链发现和恢复的 P50/P95/Max 证据。

UCN 不能把非确定性的 Wi-Fi 变成硬实时总线。微秒级电机闭环必须留在本机；跨节点控制应按目标介质评估，并准备本地失联安全动作。

### 5.7 RAM、Flash 和 CPU 极限

UCN 不追求一个固定占用数字，而是追求可证明的线性资源模型：

```text
Node RAM = 固定基础对象
         + Link/Neighbor/Route/Flow 等固定表
         + 队列深度 × 单槽大小
         + 可选 Service/Transfer/Security/Driver Storage
```

当前 Host x64、Release、Service OFF 的 `ucn_node_t` 参考值为 Nano 2,648 B、Lite 6,024 B、Full 10,080 B；它们不是 MCU ABI。目标板还要加入任务栈、DMA、Wi-Fi 系统、驱动队列和密码实现。

最终要求不是“所有 MCU 都跑 Full”，而是：

- 极小 MCU 使用 Nano 静态直连/静态路由。
- 需要自动组网的 MCU 至少使用 Lite。
- 需要 Candidate、Path、Policy、Balance 和诊断的节点使用 Full。
- 大消息只由显式创建 `ucn_transfer_t` 的节点支付缓冲。
- 任意配置超过时间或内存安全边界时，在编译期或初始化期失败关闭。

### 5.8 可靠性和安全极限

- Core Q0/Q1 的 `UCN_OK` 只表示本机层已接受，不等于远端任务已经执行。
- T128～T8K Transfer 的完整 ACK 可证明目标 Transfer 已重组接收，仍不等于业务执行成功。
- 业务执行结果必须由 Result Endpoint 返回，并带请求标识和业务状态。
- CRC16/CRC32 只能发现随机传输错误，不能防篡改或伪造。
- 最终生产安全必须使用认证 Join、逐跳控制面认证、端到端 AEAD、持久化 Sequence/Replay Window、密钥轮换和 Endpoint ACL。
- 即便有重试，物理断路、网络分区、节点掉电和资源耗尽仍可能失败；协议必须显式上报，不能承诺绝对不丢。

## 6. 最终不应该做什么

UCN 的成功不以功能越多越好。以下内容应明确排除或放在可选 Host/Extended：

- 不实现 Wi-Fi MAC、802.11s、CAN 控制器、USB Device Stack 或射频驱动。
- 不替代 TCP/IP、DDS/ROS 2、MAVLink、CANopen、DroneCAN 或 Linux 网络栈。
- 不把每个本机任务伪装成独立路由 Node。
- 不让每个节点保存全网完整路由表或长期全网拓扑。
- 不做无限制广播、无限重试、无限缓存或运行时无界分配。
- 不让普通业务每帧携带完整路径、全网状态或昂贵签名。
- 不把 Linux/网关在线作为 MCU 通信前提。
- 不承诺在任意物理介质上都能硬实时、零丢包或无限扩展。
- 不为了大文件牺牲 Q0/Q1 的控制与实时资源。

## 7. 最终产品形态

### 7.1 开发者看到的接口

理想的最终使用流程应收敛为：

```c
/* 1. 选择产品默认配置并覆盖 Node、引脚、控制器和资源档位。 */
ucn_product_configure(...);

/* 2. 注册一个或多个 UART/CAN/Wi-Fi/USB Link。 */
ucn_product_register_link(...);

/* 3. 把本机任务注册为 Endpoint/Service。 */
ucn_service_bind(...);

/* 4. 按 Node + Endpoint + QoS 发送。 */
ucn_send_to_endpoint(...);
```

这些名称表达的是最终体验，不代表当前已经存在同名的一键 API。当前公开 API 仍由 `ucn_node_*`、`ucn_adapter_*`、`ucn_event_runtime_*`、`ucn_service_*` 和 `ucn_transfer_*` 组成。

### 7.2 仓库最终应交付的内容

- 稳定且版本化的 C99 Wire Codec 与公共 API。
- Nano/Lite/Full 和 Service/Extended 的可复现构建档案。
- 裸机、FreeRTOS、Zephyr、NuttX、RT-Thread 的标准 Port 模板与至少一个真实参考实现。
- UART/RS-485、CAN/CAN-FD、USB、Wi-Fi/ESP-NOW 的参考 Adapter；BLE/LoRa/Ethernet 可按需求增加。
- Host 的 UDP/SocketCAN 接入与 ROS 2/MAVLink Bridge 示例。
- 单元、虚拟拓扑、故障注入、规模模拟、Sanitizer/Analyzer、跨编译和实机测试矩阵。
- 明确的资源报告、Cost 标定表、安全接入指南、快速使用手册和迁移说明。

## 8. 达到“整体完成”的验收标准

UCN 只有同时满足以下条件，才适合从“预发布协议”进入“可对外发布的基础版本”：

### 8.1 协议正确性

- Wire Golden Vector、畸形帧、版本/Profile/长度/CRC/Tag 门禁全部通过。
- 一跳、多跳、扩圈、断链、RERR、重建、Path 和 Epoch 在多 Origin 下闭环。
- 表满、队列满、超时、回绕、重复、乱序和重启都产生确定结果。

### 8.2 实机通信

- 至少 UART、CAN/CAN-FD、Wi-Fi/ESP-NOW 三类真实 Bearer 通过。
- 1～N Hop 的吞吐、时延、丢包、恢复、CPU、RAM、栈和功耗有原始证据。
- 多源并发、双向流、负载均衡、指定路径、链路切换和大消息并发通过。

### 8.3 安全

- 采用受审计密码库，不自研生产 AEAD。
- 身份、Join、密钥存储、轮换、Replay Window 和 ACL 通过攻击注入与掉电测试。
- 中继透明转发密文，未授权节点不能注入控制帧或业务命令。

### 8.4 可移植与资源

- 至少一个小型 STM32/同级 MCU、ESP32 和一个 RTOS 平台完成资源实测。
- Nano/Lite/Full 的功能与资源裁剪真实生效。
- Port/Adapter 不把 SDK 类型反向带入 Core，ISR 不执行协议重活。

### 8.5 发布工程

- 公共 API、ABI/Wire 兼容策略和版本升级规则冻结。
- CI 覆盖所有支持 Profile、严格编译器、Sanitizer 和静态分析。
- 示例工程可以从干净仓库构建，不依赖本机私有路径或密钥。
- 文档中的“已实现/已实测/理论目标”与代码和证据完全一致。

## 9. 从当前 v5 到最终目标的优先顺序

1. **先关闭剩余正确性和发布阻塞项**：V5-64 外审、HELLO Capability/Path MTU、表/回绕/并发边界和现有审计问题。
2. **完成生产安全闭环**：Join、身份、受审计 AEAD、Replay、密钥轮换和安全实机。
3. **扩大真实 Bearer 证据**：CAN/CAN-FD、Wi-Fi/ESP-NOW、UART/RS-485、USB，并补 V5-64 多源实机。
4. **完成多源与故障压力**：双向/多源 Transfer、Q0/Q1 并发、Path 切换、负载均衡和长稳。
5. **补齐平台封装**：FreeRTOS、Zephyr、NuttX、RT-Thread 和裸机参考工程。
6. **再增加可选能力**：服务发现、组播、通用 Q2、Q3 流/文件、时间同步和 Host Bridge。
7. **最后冻结发布规格**：按目标硬件分别给出最大节点工作集、Hop、吞吐、时延、RAM、Flash、CPU 和功耗，不用一个数字覆盖所有产品。

## 10. 最终定义

UCN 最终不应被描述为“比 Wi-Fi Mesh、DroneCAN、TCP/IP 什么都强的万能协议”。更准确的定义是：

> UCN 是一个面向资源受限 MCU 的统一多介质通信骨架。它以固定资源完成逻辑寻址、邻居与有限多跳、路由/指定路径、QoS、安全边界、任务 Endpoint、故障恢复和诊断；再通过可选模块提供大消息、服务和 Host 接入。它让业务使用同一套 Node/Endpoint 语义跨 UART、CAN、Wi-Fi 等承载通信，同时保留底层介质各自的性能与物理边界。

如果最终做到这一点，UCN 的真正价值不是“发明所有底层网络”，而是让 MCU 产品在增加、替换或组合通信介质时，业务层、任务模型、安全规则和寻址方式不必全部重写。

## 11. 关联文档

- [UCN 整体架构设计](../../02-总体架构/UCN_整体架构设计.md)
- [UCN 网络容量与关键参数总览](../../01-入门与使用/UCN_网络容量与关键参数总览.md)
- [UCN 协议分层与配置档案](../../01-入门与使用/UCN_协议分层与配置档案.md)
- [UCN 使用与调用手册](../../01-入门与使用/UCN_使用与调用手册.md)
- [UCN Adapter 契约](../../06-平台与适配/UCN_Adapter_契约.md)
- [UCN Link Metrics 与 Cost 契约](../../04-路由与链路/UCN_Link_Metrics与Cost契约.md)
- [UCN 消息大小等级与有界分片重组建议](../../05-传输与服务/UCN_消息大小等级与有界分片重组建议.md)
- [UCN V5-65 Transfer 冷启动寻路重入缺陷](../../08-实现与验证/版本演进/UCN_V5_65_Transfer冷启动寻路重入缺陷.md)
