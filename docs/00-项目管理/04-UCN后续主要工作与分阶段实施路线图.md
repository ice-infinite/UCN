# UCN 后续主要工作与分阶段实施路线图

> 文档级别：`PROJECT ROADMAP`
> 当前状态：`CURRENT PLAN / RELEASE NO-GO`
> 适用基线：当前 `main` 工作区，2026-09-04
> 适用范围：v5 实验快照、UCN v6 最终协议、目标 MCU/RTOS/物理接口与 Cluster 演进
> 事实来源：当前任务表、公共头、生产源码、CMake、软件测试、ESP32 实测记录和发布门禁

## 1. 文档目的

本文回答的不是“UCN 还能增加什么功能”，而是：

1. 当前协议已经完成到什么程度；
2. 如何将 v5 实验实现破坏性收敛为单一 v6 最终协议；
3. 距离某一块真实 MCU 上可长期运行的产品实现还缺哪些平台工作；
4. Cluster Current、Cluster Target 和跨簇网络应怎样继续推进；
5. 每项工作需要怎样实现、测试和验收；
6. 哪些内容当前不应继续扩张，防止项目再次进入“功能越来越多、发布门禁一直不关闭”的状态。

本文是后续执行顺序和完成标准，不取代各模块的 Wire、API、状态机和测试规范。具体完成状态仍以[总任务表](00-任务表.md)、[当前能力与成熟度](../official/00-项目总览/02-当前能力、限制与成熟度.md)和对应测试证据为准。

## 2. 总体结论

UCN 的正常数据面机制已经基本成型，但用户已明确不要求兼容尚未发布的旧实验协议；
因此 MCU-first、固定资源和 Owner 等原则继续保留，Wire、Identity、Security、Message、
Capability、Route/Path、Transfer、API 和 Cluster 过渡结构允许破坏性重构。当前 Core 已经
具备：

- MCU 无 Linux 依赖运行；
- Nano、Lite、Full 三种静态裁剪档位；
- Core Wire v5 与 W0～W3 编码档位；
- Link、Neighbor、HELLO、Heartbeat、AODV-Lite 和 RERR；
- 自动路由、显式 Path、策略路由、Q1 负载均衡和动态 Cost；
- Q0～Q3 四级有界调度；
- Endpoint、Service、任务间和跨 MCU 消息语义；
- T32～T8K Transfer、分片、累计 ACK、重组和 CRC32；
- Stream、CAN-FD、Classic CAN 的 MCU 无关 Source；
- 裸机与多种 RTOS 的 Port 外壳；
- 安全 Policy/Provider 接口；
- 可选的 Cluster Current、Federation 和多个 Cluster Target 实验组件。

剩余工作主要分成四条线：

```text
v5 实验收口
  ├─ Q0～Q3 与 V5-64 外审台账
  └─ 不可变快照、Tag/分支和证据

v6 最终协议
  ├─ Identity/Address/Generation
  ├─ 单一 Wire/Message/Security
  ├─ Capability/Path Frame MTU/Payload Budget/RouteSet/Transfer
  └─ Realtime/Cluster 统一接入

产品化
  ├─ 真实 RTOS/BSP/Driver
  ├─ 真实 Cost 标定
  └─ CPU、栈、RAM、功耗与长稳

生产安全
  ├─ 身份与 Join 引导
  ├─ 逐跳认证与业务 E2E AEAD
  ├─ Replay/Session/Key 持久化
  └─ ACL、轮换、撤销与升级回滚

Cluster Target
  ├─ 删除 v3/v4 双栈并接入统一 v6 Wire
  ├─ 真实 Flash/掉电
  ├─ 跨簇 Transfer 与 MCU 验收
  └─ 两级分簇和 1k/10k 规模门禁
```

因此，后续工作重点不是继续为 v5 增加兼容分支，而是先冻结 v6 最终合同，再按依赖顺序
重构、接入真实平台并取得可复现证据。

## 3. 发布线必须拆分

UCN 不应继续用一个发布结论同时覆盖 v5 实验、v6 Core 和 Cluster Target。建议正式拆成
三条成熟度线。

| 发布线 | 内容 | 当前状态 | 放行方式 |
| --- | --- | --- | --- |
| v5 Experimental | 当前 Frame、Node、Mesh、Route、Path、QoS、Transfer 和实验组件 | 软件证据丰富但不发布 | 只形成不可变历史快照 |
| v6 / UCN 1.0 | 单一 Identity、Wire、Security、Message、Capability、RouteSet、Transfer 与参考平台 | V6-00 架构 RFC 已终审 GO；实现未开始 | 按 V6-01～V6-15 逐项实现和外审 |
| Cluster Target | 复用 v6 的 Joint Config、Authority、Backup、Takeover、Handover、Recovery、Rekey | 模型较完整，统一接线待 v6 基座 | 作为可选模块随 1.0 支持矩阵签字 |

这样可以先保存 v5 试验成果，再构建一个没有兼容债务的 MCU 自组网 Core；Cluster
Target 不需要阻塞没有 Cluster 的 Core，但凡进入 1.0 支持矩阵，就必须复用同一 v6
身份、安全和 Wire。

## 4. 执行原则

### 4.1 MCU-first 不变

普通 MCU 必须在没有 Linux 的情况下完成发现、寻路、转发、故障恢复和业务通信。Linux、ROS2、PC 工具只能是 Host、诊断、网关或开发辅助，不能成为网络运行所必需的中心。

### 4.2 保留正确原则，允许破坏性重构过渡结构

继续保留：

- MCU-first、固定内存和有界失败；
- Q0～Q3 的独立资源与无饥饿目标；
- Endpoint/Service/Transfer 的职责分层；
- 单一 Protocol Owner、ISR Ring/Queue 和事件通知模型；
- 软件/实机/安全/掉电证据分层。

允许重新设计：Core Wire v5、W0～W3 字段宽度、Node ID 身份语义、DATA Type、Route
Epoch、PATH_INSTALL、Go-Back-N、公共对象 ABI、配置默认、Cluster v3/v4 和所有 legacy
bridge。是否保留某段代码只由 v6 最终合同和实测收益决定，不由旧固件兼容决定。

后续性能优化优先放在 Adapter、DMA、批量提交、Driver Queue、调度通知和可选硬件转发，不通过不断增加 Core 特例解决。

### 4.3 软件证据和实机证据分开

Host 单元测试、模拟、Sanitizer、Analyzer 和 Fuzz 只能证明软件合同。真实 UART、CAN、USB、无线、Flash、掉电、功耗和长期运行必须有目标固件、接线、原始日志和结果报告。

### 4.4 受限实验组件不得直接变成生产能力

Cluster Wire v4 Codec 或 M07～M13 模型通过测试，不代表生产 Node 已经可以接收这些帧并产生 Authority。生产接线必须单独设计、实现和外审。

### 4.5 Bearer 按产品需要增加

不为了“接口种类多”同时实现所有 Bearer。先闭环真实会使用的 UART/RS-485、ESP-NOW/Wi-Fi、CAN/CAN-FD、USB；之后再按项目选择 Ethernet、BLE、802.15.4、LoRa、UWB 或其他介质。

## 5. 第一优先级：保存 v5 实验成果并冻结 v6 最终协议

### 5.1 Q0～Q3 最终外部复审和接口冻结

#### 当前事实

Q0～Q3 已经进入 Nano/Lite/Full Node、Service 和 Transfer。QOS-A01～A08 的负枚举、Q1 多流公平、队列深度、统计口径、Windows 路径和文档问题已经整改，并在 `69901bf` 形成可审阅提交。六板 UART 和部分 ESP-NOW 测试也提供了实际证据。

当前仍保持 `AUDIT HOLD / EXTERNAL REVIEW PENDING`，原因是软件接口尚未获得最终外部复审签字，而不是已知代码仍有未修 P0。

#### 后续步骤

1. 以 `69901bf` 为固定审计基线；
2. 复审所有 Traffic Class 公共入口、Policy、Service、Transfer 和统计；
3. 复核 Full/Lite/Nano、Service-OFF、Release、ASan/UBSan 和 Analyzer；
4. 核对官方文档、用户手册和任务表的 Q0～Q3 语义；
5. 若无新 P0/P1，冻结当前枚举、调度权重、队列所有权和 Transfer 映射；
6. 后续 Bearer 硬件优先级只作为映射，不再改变 Core Class 语义。

#### 完成标准

- 外部复审明确签字；
- 所有 P0/P1 关闭；
- 任务表改为 `DONE / EXTERNAL REVIEW GO`；
- 形成 v5 实验冻结点和证据说明；
- 不把 UART 测试结果扩张成 CAN、USB 或硬实时结论。

### 5.2 V5-64 多 Origin Route Epoch 所有权

#### 为什么必须做

当前单源路由发现已经覆盖较充分，但多个 Origin 同时对共享目标发起 RREQ 时，不能直接把不同 Origin 产生的 Epoch 当成同一数值域比较。

例如：

```text
A ─┐
   ├─ R ─ C
B ─┘

A 正在发现到 C 的路径
B 同时也在发现到 C 的路径
```

如果中继 R 只按目标 C 保存一个“最新 Route Epoch”，A 的新 Epoch 可能覆盖 B 的状态，或者 B 的较大数值让 A 的合法 RREP 被误判为旧消息。不同 Origin 的 Epoch 没有天然全局大小关系。

#### 已冻结的软件合同

V5-64 采用 Origin 分域和固定 Route Instance 的组合方案：

1. 动态 Route 的固定键是 `(traffic_origin,destination)`，不同 Origin 的 Epoch 永不直接比较；
2. RREQ `A→T` 在沿途学习 Reverse `(T,A)`，RREP `T→A` 在沿途学习 Forward `(A,T)`；
3. Candidate 的固定键是 `(traffic_origin,destination,candidate_id)`，相同 candidate ID 可在不同 Origin 域同时存在；
4. 静态 Route 以 `route_origin=0` 表示人工配置的 Destination 通配项，并保持 Nano 行为；
5. Full/Lite 使用编译期固定 Route Instance 槽，表满返回 `NO_SPACE` 并保留所有既有实例；Nano 不启用动态发现；
6. RERR 根据 Payload 中的 unreachable target 找到 `target→origin` Reverse Domain，不能把检测故障的中继 Source 当成 Route Origin。
7. Activate ACK 精确绑定已发送 Candidate/Epoch；ACK 丢失按 250 ms 间隔有界重试，1000 ms 或 3 次重试耗尽后清 Candidate、保留旧 Active Route。
8. Probe/RTT/Activate 开始后 Candidate 路径快照不可变；路径或 Bearer 变化必须使用新 Candidate ID 重新发现和证明。

该修改不改变 Core Wire v5；它升级了 Node Storage Layout，并要求所有使用透明 Storage 的产品全量重编译。

#### 当前实现范围

- Discovery、Reverse、RREP、业务转发、Route Refresh 与 Route Epoch 接收验证均按 Origin 分域；
- Candidate Probe/Activate 同样按 Origin 分域，本机主动事务还要求 `route_origin == local node`；
- `ucn_node_copy_route_summaries()` 可导出 Origin、Destination、Link、Cost、Hop 与 Current/Previous Epoch；
- `route_instance_table_full` 单独统计动态 Route Instance 满载；
- Storage Layout Version 已升级为 10；Host x64 Debug Nano/Lite/Full 为 `3496/6952/11152 B`；
- 详细字段、状态和边界见 V5-64 实现与验证文档。

#### 已完成的软件测试

- 五节点 A/B→R/X→T 同时发现共享目标，使用不同 Ring/路径并交叉交付 RREP；
- T 只接受与 Source Domain 匹配的 Epoch，伪造 A Source 携带 B Epoch 会被拒绝；
- Full 中相同 candidate ID 的 A/B Candidate 可并存；
- 错 Epoch/提前 ACK 零写拒绝；ACK 丢失后同 ID/Epoch、新 Sequence 重试并可收敛，耗尽不破坏旧 Route；
- A 路径断开/RERR 只撤销 A Domain，B Domain 继续交付业务；
- Route 表满返回 `NO_SPACE`、统计增加且整表逐字节不变；
- Full/Lite/Nano/Release/Service-OFF/MSVC、ASan/UBSan 与 `-fanalyzer -Werror` 软件矩阵；
- 多板 UART、ESP-NOW、多 Bearer、故障压力与长稳仍待硬件验收。

#### 当前签字状态与完成标准

不同 Origin 的数值 Epoch 分域、定向 RERR、静态 Route fallback、Activate 原子性、ACK
精确绑定和有界重试均已有实现/软件证据；A01/A02/A04/A05 与 QA-A03 有明确外审记录。
但 A06 当前只有 OP-460 的代码整改和全体自审记录，没有可追溯的独立外审签字，因此
V5-64 整体恢复为 `AUDIT HOLD`。取得 A06 独立外审 GO 并同步任务表、操作记录和实现报告
后，才可形成 V6-01 的 v5 实验快照。即使届时获得 Host 软件 GO，也不能表述为多板、
ESP-NOW、多 Bearer 或生产完成。

### 5.3 v6 最终协议收敛、能力与路径 MTU

#### 当前缺口

用户已经明确：UCN 尚未发布，不要求 v4/v5 固件、Wire、API、ABI 或 Storage
兼容；协议应先达到最合理状态，再考虑正式发布。因此本阶段不再把“旧节点互操作”作为
设计目标。当前已有静态 Peer Class、窗口能力、Binding 最大消息等级、E2E 策略和多种
MTU 软件测试，但以下内容还没有形成最终协议闭环：

当前 V6A-01～V6A-25 对应整改已全部获得外部 GO；V6A-24/V6A-25 合并冻结了 Group
Generation 所有权、防 ABA 与固定容量表示。`V6-00` 已在最终架构 RFC/纯文档范围完成
外部终审。该签字不代表 v6 代码、Wire、安全、实机或掉电能力已经完成；`V6-01` 仍等待
V5-64 A06 的可追溯独立外审记录和用户对提交、v5 快照/Tag及 v6 基线的明确授权。

- 未绑定地址节点的一跳双向认证 Bootstrap、已绑定节点新 Link 重认证、单一逻辑 Address
  Authority 的 Lease/Fence/Quorum 与地址 Binding Generation；
- Authority/Binding Lease 的新鲜 Challenge、认证剩余时长和本地保守 Deadline，避免各节点
  从 Final Commit 到达时刻重新获得完整租期；
- Lease 验证端 Timer Resolution、两次读取误差和慢钟裕量的统一扣减，避免量化后的本地
  `now` 尚未跳变时继续使用真实已经过期的租约；
- Bootstrap/无 Session Reauth 在分配 pending 或执行昂贵密码运算前的无状态 Cookie、认证前
  限流、per-Link 固定配额、绝对 timeout 与响应放大限制；
- 安全 Session 建立后的 HELLO/Capability 自动交换与缓存；
- Group HELLO 的独立 Group Security Context、Key selector、保留目的地址、Tag/Replay 和
  “只提示 Reauth、不证明独立 Principal”的权限边界；
- Group Policy/Key Generation 的唯一 Owner、父域、持久 high-water、跨 Authority 换主
  连续性，以及动态 ID 单调分配、静态/Key 固定槽、永久退休和有界耗尽规则；
- Carrier MTU、Link Frame MTU、Path Frame MTU 和实际 Payload/Fragment Budget 的主动确认；
- Endpoint 级独立 TX 安全映射；
- Bearer 切换时能力/MTU 的重新验证；
- Core、Cluster、Realtime、Transfer 的单一 Identity/Wire/Security 合同，以及无歧义的
  Hop/E2E Suite、Key ID/Generation 和 Control/Transfer Opcode；
- Endpoint/Control ACL 精确绑定 Protocol Opcode，禁止把一个 CONTROL 操作的授权扩大为
  整个 Frame Type；
- Durable Operation 的 `EXECUTING/IN_DOUBT` 掉电结果、固定 Journal 表满和 GC 合同；
- Hop Scheduling Budget 在原 Class、per-source/per-flow 配额内的防注入门禁；
- 旧兼容分支删除后的统一 Golden、资源和实机证据。

#### 实施方案

1. 先按[UCN v6 最终协议架构与破坏性重构 RFC](../10-理论与规划/建议方案/UCN_v6_最终协议架构与破坏性重构_RFC.md)
   冻结 Identity、Bootstrap 双向认证与抗资源耗尽、Address Authority/Binding Generation/
   Lease Freshness（含验证端 Timer 量化误差）、Wire、Peer/Group/E2E Security、Group
   Policy/Key Generation 及固定容量分配/槽状态、精确 Opcode ACL 和对象所有权；
2. V6-01 建立逐文件/符号 Compatibility Removal Manifest，再按任务删除旧 Core/Cluster
   Runtime 双格式、legacy bridge 和 downgrade；
3. V6-03 只构建隔离 default-OFF Codec；由 V6-07 建立唯一 JOIN/ADMITTED 状态机、Cookie/
   固定 pending、Lease freshness proof、Peer/Group/E2E selector 和精确 Opcode ACL并完成
   Security 外审后，才允许接入生产 RX/TX。V6-06 不允许形成无认证 admission；
4. 定义 Capability 的来源、租期、Generation、Digest 和失效规则；
5. 普通 HELLO 只在安全 Session 后发布发现摘要，完整 Capability 使用认证的独立
   Query/Advertise；Group HELLO 使用独立 Group selector/Tag/Replay，只能触发有界 Reauth
   提示，不能续期或写 Capability/ACL；
6. Path 能力取各跳交集，分别计算 Path Frame MTU 与精确 Payload/Fragment Budget；
7. Route/Path/Bearer/Session/Address Binding 改变时使旧协商结果失效；
8. Transfer 发送前检查等级、窗口、并发、安全 Suite 和精确 Fragment Budget；
9. 诊断输出协商来源、有效期和失败原因；
10. 旧版本只通过 Git 分支/Tag 运行，不允许加入 v6 网络。

#### 完成标准

在能力未知、信息陈旧、路径改变或中间节点不支持时，发送端必须在占用大量队列之前
确定性失败，不允许猜测能力、运行时降级到旧 Wire，或传到中途才因不可表达而静默丢失。
v6 代码必须只有一个当前 Wire/状态机事实源。

## 6. 第二优先级：完成生产安全 S02

### 6.1 当前已经具备什么

当前代码已经提供：

- Security Policy/Provider 接口；
- E2E AAD 和透明密文中继边界；
- Source/Session/Sequence 去重；
- Provider 不可用时的失败关闭；
- Session Rotation Hook；
- Endpoint/Service ACL 的结构接口；
- 安全未准备时阻止错误激活的门禁。

这些能力说明协议已经为安全留下了正确位置，但不等于生产密码体系已经完成。

### 6.2 必须补齐的生产能力

#### 设备身份与入网

- 定义设备出厂身份、Node ID 与 Principal 的关系；
- 定义首次 Join、重新 Join、设备替换和身份撤销；
- 防止节点仅凭自报 Node ID 或 Wire Profile 获得权限；
- 明确调试固件、救援固件和生产固件的身份边界。

#### 逐跳控制面认证

HELLO、Heartbeat、RREQ、RREP、RERR、Path 和 Cluster 控制帧需要逐跳来源保护。因为 Hop Limit、Route Epoch 等字段必须被中继修改，不能只依赖业务 E2E Tag。

#### 业务 E2E AEAD

- 选择经过审计、适合目标平台的 AEAD 实现；
- 固定 Nonce、AAD、Tag、Key ID 和 Session Generation；
- 中间节点只转发外层，不解密业务内容；
- 目标 Endpoint 解密并执行 ACL；
- 坏 Tag、旧 Session、未知 Key 和策略降级全部失败关闭。

#### Replay 与持久化

- Sequence/Session 不得在重启后回退；
- Replay Window 需要固定内存和明确容量；
- 持久化失败不能继续发送可能复用 Nonce 的密文；
- 接近阈值时必须先完成 Key/Session Rotation 再继续通信。

#### Key 生命周期

- 生产、烧录、轮换、撤销、备份和销毁；
- 丢失控制节点或长时间离线节点的恢复；
- 固件升级、回滚和旧 Key 的拒绝窗口；
- 密钥材料不得写入日志或普通诊断帧。

### 6.3 验收

- 正式测试向量与负向篡改；
- 未认证控制帧注入；
- A→B→C 透明密文中继；
- Session/Key 轮换中断和重启；
- Flash 撕裂写与可控掉电；
- ACL、撤销和旧固件回滚；
- CPU、RAM、Flash、延迟和单帧开销；
- 独立安全审计。

S02 完成前可以使用 UCN 做实验网络，但不得声明“协议已经生产安全”。V5-21 Authorized Class 也必须继续阻塞，不能用自报 Class 代替已认证身份。

## 7. 第三优先级：建立一个真正完整的参考产品

### 7.1 为什么先做一个参考产品

当前仓库有统一 Port、Adapter、Stream Source 和 CAN Source，但不会替用户配置具体 SDK、GPIO、DMA、控制器、收发器或无线 Peer。若同时追求所有 MCU 和 RTOS，最终容易停留在“每个接口都有头文件，没有一个平台真正闭环”。

建议第一个参考产品固定为：

```text
ESP32-S3
  + FreeRTOS
  + UART/RS-485 Stream Source
  + ESP-NOW 或 Wi-Fi 自定义 Bearer
  + Event Runtime / Owner Task
  + Nano/Lite/Full 至少两种产品配置
```

之后再把同一合同移植到 STM32 + FreeRTOS/裸机和 Zephyr/NuttX/RT-Thread。

### 7.2 参考产品需要实现的层次

```text
UCN Core
  ↓ Link send / RX submit / time / critical section
Standard Adapter
  ↓ readiness / queue / metrics / link-state
Platform Driver Glue
  ↓ UART DMA、CAN FIFO、USB endpoint、ESP-NOW callback
芯片 SDK 与真实硬件
```

每种 Driver 必须提供：

- RX 中断或 DMA 到固定 Ring/Queue；
- ISR-safe 通知 Owner；
- `send()` 只做有界入队，不等待物理完成；
- readiness 或可提交容量；
- Link Down、恢复和 Driver 错误；
- 物理完成回调和队列统计；
- RSSI、失败率、RTT、队列压力等可用 Metrics；
- 不支持硬件优先级时的明确软件边界。

### 7.3 多 Source 和多实例

同一节点必须能够同时存在多个独立实例：

```text
UART0 Link
UART1 Link
CAN0 Link
CAN1 Link
USB Link
ESP-NOW Link
```

它们不能共用未经保护的全局 Driver 状态。每个 Source 有独立 Ring/Carrier 状态；统一 Event Runtime 汇聚事件；只有 Protocol Owner 修改 Node、Route、Path、Service 和 Transfer 状态。

### 7.4 平台验收

- ISR priority、FromISR Yield 和 Owner 唤醒延迟；
- RX burst、事件风暴和通知合并；
- Driver Queue 满、恢复、拔插和反复重连；
- 两个以上 Source 同时满载；
- 任务栈高水位、Heap 最小值和静态 RAM；
- Wi-Fi 回调、UART DMA、CAN RX FIFO 与 USB endpoint 并发；
- 1 小时、8 小时、24 小时长稳；
- watchdog、brownout、panic 和重启原因可追踪。

## 8. 第四优先级：真实 Cost 标定和选路闭环

### 8.1 当前 Cost 的定位

当前基础 Cost 和 LC-1 动态项可以工作，但默认值是跨平台初始值，不是所有产品的真实最优值。UART 3 Mbps、CAN 1 Mbps、ESP-NOW、USB 和 LoRa 不能只按名义 bit rate 排序。

实际 Cost 至少受以下因素影响：

- 有效吞吐而非标称速率；
- P50/P95/P99 RTT；
- 失败率、重试和断链频率；
- Driver Queue 压力；
- MTU 和 Transfer 分片数量；
- 半双工、仲裁和共享空口占用；
- 功耗；
- 当前业务的 Hop、Cost 和 RTT 限制。

### 8.2 标定流程

1. 每个 Bearer 在两板直连下测空载基线；
2. 测不同 Payload、Q0～Q3 和 Transfer 等级；
3. 增加反向流量和同介质竞争；
4. 测三板/六板多跳；
5. 注入拥塞、RSSI 恶化、Bus-Off、拔线和恢复；
6. 记录基础 Cost、动态加分和最终 effective cost；
7. 验证选路、保持、探测和切换理由；
8. 给出产品覆盖值，而不是修改协议全局常量满足单一台架。

### 8.3 完成标准

相同固件在相同观测下给出确定性选择；链路变差时可以解释为何切换；短时抖动不会频繁振荡；业务限制不满足时即使 Cost 最低也不会被选中。

## 9. 第五优先级：多 Bearer、性能和长期运行

### 9.1 必须覆盖的组合

- UART-only 单跳/多跳；
- ESP-NOW-only 双向与四节点；
- UART + ESP-NOW 动态切换；
- Classic CAN 与 CAN-FD；
- USB CDC；
- 至少一个节点同时存在两个 UART、一个 CAN 和一个无线 Source；
- Bearer 切换过程中运行小消息与 T32～T8K Transfer；
- 多 Origin、多目的地和双向并发。

### 9.2 QoS 实机边界

Q0～Q3 软件调度已经能够提供有界服务比例，但还需分别判断物理接口是否支持：

- CAN ID/邮箱优先级；
- USB endpoint/queue 优先级；
- UART 是否只能在帧边界调度，不能抢占已发送字节；
- Wi-Fi/ESP-NOW 是否能提供可靠 readiness 和不同业务队列；
- DMA descriptor 是否允许按 Class 分队列。

没有硬件优先级的接口仍可使用 UCN 软件调度，但必须把保证写成“下一帧调度优先”，不能写成“任意时刻抢占”。

### 9.3 六板遗留问题

当前六板 UART 多项门禁已经通过，但六板 UART+ESP-NOW 组合中的特定 COM58/C WDT/Panic 仍是未关闭硬件问题。需要换板或隔离硬件后重新执行：

1. 单板完整组合镜像；
2. 两板多 Bearer；
3. 六板 75 s 功能门禁；
4. 长时间多 Bearer；
5. 保存复位原因、panic、供电、电流和温度。

不得用 UART-only 通过覆盖这一组合失败。

### 9.4 资源和长稳

每个产品配置都要记录：

| 指标 | 测量内容 |
| --- | --- |
| Flash | 实际 APP image、分区占用、可选模块差值 |
| 静态 RAM | `.data/.bss`、Node/Service/Transfer/Cluster 对象 |
| Task Stack | Owner、Driver、业务任务 high-water |
| Heap | 启动后、峰值、长稳最小值和碎片情况 |
| CPU | 空载、持续小包、Transfer、多 Bearer、故障风暴 |
| 延迟 | P50/P95/P99/Max，分 Class、Hop 和 Bearer |
| 功耗 | 空闲、收发、转发、无线拥塞和故障恢复 |
| 稳定性 | watchdog、panic、重启、错误计数和消息守恒 |

## 10. 第六优先级：Cluster Target 产品化

Cluster 后续不能简单理解为“把实验宏打开”。它需要单独完成下面六组工作。

### 10.1 M05：Wire v4 生产接线

当前已经有 v4/40 B strict codec、semantic 对象、golden/negative/fuzz 和双格式测试，但生产默认仍是 v3/32 B。

生产接线必须明确：

- RX Owner 在什么条件接受 v4；
- v3 legacy 只能获得哪些非 voting 能力；
- raw decode、semantic parse、FSM admission 的三层边界；
- Type 1～33 分别允许哪些 Role/Phase/Epoch/Config；
- certificate pending 的 Source、Config 和资源门禁；
- encoder 在什么产品版本打开；
- v3/v4 混跑、升级、回滚和失败关闭策略；
- 所有 Authority 副作用前的 Persistence/Quorum/Fence 证明。

M05 放行前不得让“解析成功”自动等于“获得角色或 Authority”。

### 10.2 真实 Persistence Provider

把 M04/M07/M10/M13 的软件 Provider 合同接到真实 Flash/NVS：

- 双槽或等效原子记录；
- generation、CRC、schema 和 canonical encoding；
- submit/poll/load 与重入门；
- 每个写阶段的可控断电；
- 擦写对齐、erase block、wear leveling 和写寿命；
- Record 升级、旧固件回滚和受控擦除；
- 已 durable 但尚未发 ACK/Commit 的 continuation 恢复。

### 10.3 收敛 M08～M14 外审依赖

任务台账中 M08/M09/M10/M12/M13/M14 仍存在等待外审或上层阻断状态。应以当前代码重新建立一次依赖图，逐项处理：

- 已完成修复是否拥有可审阅提交；
- 受限 GO 是否只属于模型；
- M10 R31～R34 是否最终签字；
- M12 Recovery/Lineage 和唯一 ID 的剩余限制；
- M13 Rekey/No-wrap 的外审；
- M14 14-03、14-07、14-08、14-11、14-12 的 NO-GO 条件。

不能因为后续模块已经实现，就反向假定前置里程碑自动通过。

### 10.4 C06：跨簇 Transfer

C06.0～C06.3 已完成 Locator、Directory 和单帧 Tunnel 的 Host 软件闭环。后续要做：

- C06.4：T32～T8K 穿过 Tunnel 的分片、ACK、重组和完整路径 MTU；
- C06.5：两/三簇规模、目录副本、Cache、Head/Directory 故障和资源；
- C06.6：四板及更多 MCU 的跨簇通信、时延、CPU、RAM 和丢包。

中间 Head 不应完整重组不属于自己的大消息；跨簇重路由时必须保持 Transfer 身份、ACK 返回路径和 E2E 安全边界。

### 10.5 C07.7：主备簇头实机

需要满足 Backup 对 protected voter 的真实一跳覆盖，执行：

- Head 突断和短时抖动；
- Backup 多数接管；
- Head+Backup 双失效；
- 旧 Head 回归；
- Directory Handover；
- 无多数失败关闭；
- 无线/有线分别测试；
- 小时级长稳、功耗和资源。

如果物理拓扑不能让 Backup 覆盖所需 voter，就只能证明“系统正确拒绝接管”，不能证明“快速接管已通过”。

### 10.6 C08：两级分簇和万级规模

只有 C06/C07 闭环后才进入：

- 定义一级簇、二级簇或 Region 的状态上界；
- 普通成员不保存全网逐节点路由；
- Directory/Locator/Tunnel 的层级聚合；
- 局部故障不扩散为全网重选；
- 1k/10k 混合流量、移动、拆分、合并和 Head 故障；
- 控制开销、时延、吞吐和内存上界；
- Host 模拟后仍需目标硬件抽样验证。

完成前只能说“设计目标支持大规模分层”，不能说“已经支持万级节点”。

## 11. 第七优先级：发布工程与单版本冻结

### 11.1 版本坐标

发布不能只写“UCN v5”，至少应同时记录：

```text
Project version
Core Wire version
Public API/ABI version
Storage layout version
Port API version
Cluster Current Wire/API
Cluster Target Wire
Persistence Record schema
Profile/Feature/product config
```

### 11.2 候选版本清单

- 干净 Git commit 和 tag；
- 编译器、SDK、优化级别和配置头；
- 库、固件、分区表和 SHA256；
- Full/Lite/Nano 和 Feature 组合；
- 软件、实机、安全、掉电和资源报告；
- 支持的 MCU、RTOS 和 Bearer；
- 已知问题、回滚最低版本和介质处理；
- 官方文档与候选 commit 一致；
- 外部审计关闭 P0/P1。

### 11.3 发布层级

建议按下列顺序命名，避免一次性宣称全部生产完成：

1. **v5 Experimental Snapshot**：只保存当前测试代码和证据，不承诺继续兼容；
2. **v6 Developer Preview**：最终架构的模块逐项进入软件验证，仍不承诺 Wire 稳定；
3. **UCN 1.0 Release Candidate**：单一 Wire/API/Storage 冻结，一个参考 MCU/RTOS/Bearer
   组合和生产安全完整通过；
4. **UCN 1.0 Stable**：安全、资源、长稳、故障、掉电和支持矩阵签字。

## 12. 分阶段执行顺序

### 阶段 A：v5 快照与 v6 架构冻结

```text
A1  关闭 QOS 与 V5-64 外审台账
A2  V6-00 最终架构 RFC 外审
A3  V6-01 保存 v5 实验 Tag/分支、建立 Compatibility Removal Manifest 与 v6 干净基线
A4  V6-02 Identity/Bootstrap/Address Binding/Generation
A5  V6-03 Core Wire v6
A6  V6-04 Message/Endpoint/Traffic/Guarantee/Interaction
A7  V6-05 C99 Opaque Storage/Config/Owner
```

阶段 A 只允许形成隔离、default-OFF 的 v6 Codec/Golden/Negative target；不得接入生产
Node/Adapter，不得发送真实 v6 Frame。生产 RX/TX/Encoder 必须等待阶段 B 的 V6-07
Security 与唯一 JOIN FSM 获得外部复审 GO；不为 v4/v5 增加 Runtime 兼容分支。

### 阶段 B：能力、安全、路由与传输

```text
B1  V6-07 Production Security + 唯一 JOIN FSM + Cookie/Lease Freshness/Opcode ACL
B2  V6-06 认证 HELLO/Capability/Path Frame MTU/Payload Budget
B3  V6-08 RouteSet/Path/Multipath
B4  V6-09 Metric/QoS
B5  V6-10 Transfer Selective ACK/Credit/Pipeline
```

### 阶段 C：Realtime、Cluster 与统一持久化

```text
C1  共同 Security/Owner/Generation/Persistence 基座门禁
C2  V6-11 Realtime 生产集成 ─┐
C3  V6-12 Cluster Target 统一 ─┴─ 并行、互不链接、互不依赖
C4  两个 Feature 分别完成真实 Flash、掉电和实机门禁
```

安全从阶段 A 的 Identity/Wire 设计起就必须参与，不能等阶段 B 结束后补 Tag。

### 阶段 D：第一参考平台

```text
D1  ESP32-S3 + FreeRTOS 产品配置
D2  UART/RS-485 生产 Adapter
D3  ESP-NOW 生产 Adapter 与 readiness
D4  多 Source Event Runtime 与 Timed Link
D5  Nano/Lite/Full 资源、性能和功耗
D6  断链、重连、故障注入和 24 h 长稳
```

### 阶段 E：扩展 Bearer 与 UCN 1.0 RC

```text
E1  CAN/CAN-FD
E2  USB CDC
E3  多 Bearer Cost/Capability/MTU 标定
E4  Realtime 与 Cluster 实机
E5  1k/10k 模拟、资源和控制峰值
E6  全软件/硬件/安全/掉电审计
E7  UCN 1.0 RC 与支持矩阵
```

## 13. 每项任务的统一执行模板

后续每个任务应采用同一套证据链：

```text
问题与边界核实
  → 形成设计/字段/状态机合同
  → 拆分任务并登记依赖
  → 实现最小闭环
  → 单元与负向测试
  → 多节点/故障模拟
  → Profile/Release/Sanitizer/Analyzer
  → 目标硬件实测（适用时）
  → 资源/长稳/功耗（适用时）
  → 分项自审
  → 全体自审
  → 外部复审
  → 更新官方文档、操作记录和发布状态
```

测试必须包含“为什么旧错误实现会失败”的区分度，不能只证明当前正向样例能运行。

## 14. 当前不建议做的事情

在上述主线收口前，不建议：

- 再增加新的 Wire Profile 或消息大小等级；
- 为 v4/v5 增加 Runtime 双解码、legacy bridge、旧节点能力 fallback 或降级发送；
- 在 V6-00 外审前修改协议版本或 Codec；在 V6-07 外审 GO 前接入生产 v6 RX/TX/Encoder；
- 同时实现所有无线和工业总线；
- 把原生 Wi-Fi Mesh、BLE Mesh、Thread 或 LoRaWAN 的网络层再叠一层 UCN 路由；
- 为单块测试板反复修改 Core 默认行为；
- 在 S02 前实现依赖“已认证身份”的 Authorized Class；
- 在 M05 HOLD 前打开默认 Cluster v4 encoder/Authority；
- 在 C06/C07 未闭环前宣称万级节点能力；
- 用 Host `sizeof` 或短时间串口测试直接给出所有 MCU 的资源和性能上限；
- 为追求平均吞吐取消失败关闭、Deadline、CRC、ACK 或安全门禁。

## 15. 推荐立即开始的下一项

当前最合理的连续动作是：

1. 对 V5-64 A06 形成独立、可追溯的外审结论，并据此关闭或继续整改台账；
2. V6-00 最终架构 RFC 已完成外部终审，不再作为 V6-01 阻塞项；
3. V5-64 A06 获得可追溯独立 GO 且用户明确授权后执行 V6-01：保存当前 v5 实验 Tag/分支、建立 Compatibility Removal Manifest 和
   v6 干净基线；
4. 按 V6-02～V6-05 实现身份/Bootstrap/Binding、Core Wire、Message 和 C99 Opaque
   Storage 基座；
5. 先实现 V6-07 唯一 JOIN/Security，再实现 V6-06 认证 Capability/Path Budget 和后续
   RouteSet/Transfer；
6. 同时确定第一个参考产品的 MCU、RTOS 和 Bearer 组合。

如果当前只选择一个新任务，应选择 **完成 V5-64 A06 的可追溯独立外审**。它与用户对
提交、v5 快照/Tag及 v6 基线的明确授权共同构成 V6-01 的剩余前置；在此之前不直接扩
HELLO 或修改 Frame。完整任务见[总任务表](00-任务表.md)。

## 16. 最终完成定义

### 16.1 v5 实验快照完成

- 当前 Q0～Q3、V5-64 Route、Path、Service、Transfer、Realtime 实验和 Cluster 实验
  状态与证据形成不可变提交；
- 分支/Tag、源码 Hash、软件矩阵和实机报告可复现；
- 明确该快照不是 UCN 1.0，不继续承担 v6 运行时兼容。

### 16.2 UCN v6 / 1.0 Core 完成

- Q0～Q3、Delivery Guarantee、Interaction Role/Operation ID、Route、Path、Service、
  Transfer 和 Capability 合同冻结；
- 多 Origin 路由通过；
- 单一 Wire、Identity、Bootstrap、Address Binding、Session、Route/Path Generation 和
  C99 Opaque Storage API 冻结；
- S02/v6 Production Security 必须通过；Bootstrap/Reauth Cookie 与固定资源、挑战相对的
  Lease Freshness、Peer/Group/E2E selector、Replay 和精确 Opcode ACL 全部闭环，不再把
  无安全边界作为正式 1.0 默认模式；
- 至少一个 MCU+RTOS+UART/无线组合生产闭环；
- CAN/USB 若列入支持矩阵则必须实测；
- 目标资源、长稳和回滚通过；
- 外审关闭 P0/P1；
- 文档、固件和测试绑定同一 commit；
- Compatibility Removal Manifest 全部销账；v6 产品代码中不存在 v4/v5 Runtime
  fallback、Mixed Wire、legacy Storage load 或可编译旧源码。

### 16.3 Cluster Target 完成

- Cluster Target FSM 使用统一 v6 Wire/Identity/Security，不再保留 v3/v4 双栈；
- Cluster 与 Realtime 可分别裁剪和链接，二者互不依赖；
- Persist-before-promise 在真实 Flash/掉电下成立；
- Joint、Authority、Backup、Takeover、Handover、Recovery、Rekey 在生产 Owner 中闭环；
- 跨簇小消息和 Transfer 闭环；
- 主备、分区、合并、旧 Head 回归和 Directory Handover 实机通过；
- 资源、长稳、安全和升级回滚通过；
- M14 从 `RELEASE NO-GO` 转为有明确候选基线的 GO。

### 16.4 大规模能力完成

- 两级或多级状态模型实现；
- 普通节点状态上界不随全网节点数线性增长；
- 1k/10k 模拟覆盖正常、拥塞、移动、分区和故障；
- 控制峰值、延迟、吞吐和资源有明确上界；
- 真实硬件抽样验证模拟中的关键假设；
- 文档只宣称实际通过的规模和配置。

## 17. 路线图维护规则

本路线图描述执行顺序，不重复维护每个子任务的实时状态。状态变化时应：

1. 在[总任务表](00-任务表.md)更新唯一状态；
2. 在[项目操作记录](01-项目操作记录.md)登记实际修改、验证和限制；
3. 将测试结果放入 `docs/results/`、`evidence/` 或对应实测工程；
4. 更新受影响的官方文档；
5. 只有外审和发布门禁明确解除后，才修改成熟度结论。

若本路线图与当前源码、任务表或候选构建冲突，以源码和可复现证据为准，并立即修订路线图，不能让规划文档反向定义不存在的能力。
