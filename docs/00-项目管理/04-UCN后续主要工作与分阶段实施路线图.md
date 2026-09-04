# UCN 后续主要工作与分阶段实施路线图

> 文档级别：`PROJECT ROADMAP`
> 当前状态：`CURRENT PLAN / RELEASE NO-GO`
> 适用基线：`main@69901bf`，2026-09-03
> 适用范围：UCN Core v5、可选组件、目标 MCU/RTOS/物理接口与 Cluster 演进
> 事实来源：当前任务表、公共头、生产源码、CMake、软件测试、ESP32 实测记录和发布门禁

## 1. 文档目的

本文回答的不是“UCN 还能增加什么功能”，而是：

1. 当前协议已经完成到什么程度；
2. 距离可冻结的 Core v5 还缺哪些协议工作；
3. 距离某一块真实 MCU 上可长期运行的产品实现还缺哪些平台工作；
4. Cluster Current、Cluster Target 和跨簇网络应怎样继续推进；
5. 每项工作需要怎样实现、测试和验收；
6. 哪些内容当前不应继续扩张，防止项目再次进入“功能越来越多、发布门禁一直不关闭”的状态。

本文是后续执行顺序和完成标准，不取代各模块的 Wire、API、状态机和测试规范。具体完成状态仍以[总任务表](00-任务表.md)、[当前能力与成熟度](../official/00-项目总览/02-当前能力、限制与成熟度.md)和对应测试证据为准。

## 2. 总体结论

UCN 的正常数据面已经基本成型，后续不需要再推翻整体架构。当前 Core 已经具备：

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
协议收口
  ├─ Q0～Q3 外审与接口冻结
  ├─ 多 Origin Route Epoch
  └─ 能力、MTU、旧节点互操作边界

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
  ├─ Wire v4 生产 RX/TX/FSM/Authority 接线
  ├─ 真实 Flash/掉电
  ├─ 跨簇 Transfer 与 MCU 验收
  └─ 两级分簇和 1k/10k 规模门禁
```

因此，后续工作重点应从“继续增加协议功能”转为“关闭边界、接入真实平台、取得可复现证据”。

## 3. 发布线必须拆分

UCN 不应继续用一个发布结论同时覆盖 Core、Cluster Current 和 Cluster Target。建议正式拆成三条成熟度线。

| 发布线 | 内容 | 当前状态 | 放行方式 |
| --- | --- | --- | --- |
| Core v5 | Frame、Node、Mesh、Route、Path、QoS、Adapter Runtime | 软件主体完成，仍有协议与产品门禁 | 优先形成第一个 MCU 参考版本 |
| 可选服务层 | Service、Transfer、Security Provider、Cluster Current、Federation | 按组件分别成熟 | 只有明确链接并完成产品验收才声明支持 |
| Cluster Target | Wire v4、Joint Config、Authority、Backup、Takeover、Handover、Recovery、Rekey | 大量受限模型存在，但生产接线冻结 | 单独形成后续 Cluster Target 候选版本 |

这样可以先发布一个真正可用的 MCU 自组网 Core，而不必等待多级簇和万级网络全部完成。Cluster Target 也不会因为赶 Core 版本而过早打开生产 Authority。

## 4. 执行原则

### 4.1 MCU-first 不变

普通 MCU 必须在没有 Linux 的情况下完成发现、寻路、转发、故障恢复和业务通信。Linux、ROS2、PC 工具只能是 Host、诊断、网关或开发辅助，不能成为网络运行所必需的中心。

### 4.2 不重新设计已经稳定的正常数据面

除非外审或新反例证明存在协议级错误，否则不再随意修改：

- Core Wire v5 基础语义；
- Q0～Q3 的 `6:3:2:1` 有界调度；
- Endpoint/Service/Transfer 的职责分层；
- 单一 Protocol Owner、ISR Ring/Queue 和事件通知模型；
- 静态内存、无协议内动态分配的基础约束。

后续性能优化优先放在 Adapter、DMA、批量提交、Driver Queue、调度通知和可选硬件转发，不通过不断增加 Core 特例解决。

### 4.3 软件证据和实机证据分开

Host 单元测试、模拟、Sanitizer、Analyzer 和 Fuzz 只能证明软件合同。真实 UART、CAN、USB、无线、Flash、掉电、功耗和长期运行必须有目标固件、接线、原始日志和结果报告。

### 4.4 受限实验组件不得直接变成生产能力

Cluster Wire v4 Codec 或 M07～M13 模型通过测试，不代表生产 Node 已经可以接收这些帧并产生 Authority。生产接线必须单独设计、实现和外审。

### 4.5 Bearer 按产品需要增加

不为了“接口种类多”同时实现所有 Bearer。先闭环真实会使用的 UART/RS-485、ESP-NOW/Wi-Fi、CAN/CAN-FD、USB；之后再按项目选择 Ethernet、BLE、802.15.4、LoRa、UWB 或其他介质。

## 5. 第一优先级：关闭 Core v5 的剩余协议工作

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
- 形成冻结点和兼容说明；
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

#### 需要冻结的设计问题

必须在下列方案中做出明确选择，不能继续依赖单源假设：

1. **Origin 分域**：Route Discovery 新鲜度由 `(origin, destination, epoch/request)` 组成，不比较不同 Origin 的 Epoch；
2. **Route Instance**：为并发发现分配有界实例槽，Reverse/RREP 只匹配自己的实例；
3. **显式串行策略**：低资源 Profile 明确只允许一个目的地发现事务，并对第二个 Origin 返回背压/稍后重试；
4. **组合策略**：Full 支持多实例，Lite 保持较小实例数，Nano 不支持动态发现。

推荐采用组合策略：协议身份使用 Origin 分域，资源上使用固定数量 Route Instance；表满时失败关闭或合并同一 Origin/目的地的重复请求，不覆盖其他 Origin 的合法状态。

#### 实现范围

- 冻结 Discovery Key、Reverse Key、RREP 匹配和 Epoch 比较域；
- 检查 Route、Candidate、Reverse Route、RREQ Seen 和 Route Refresh 是否混用了目的地 Epoch；
- 明确同一 Origin 重发、不同 Origin 并发、交叉 RREP 和旧 Epoch Grace；
- 增加表满、重复、乱序、环路和回绕边界；
- 保持 Nano 静态路由不受影响；
- 更新 Route 诊断，让用户可以看到 Origin/实例和拒绝原因。

#### 测试矩阵

- 四节点双 Origin 同时发现同一目标；
- A→C 与 B→C 使用不同 Ring 和不同返回路径；
- 交叉 RREQ/RREP、重复、延迟和乱序；
- 一条发现成功、另一条表满或超时；
- 路由刷新与旧 Epoch Grace 并存；
- 两个 Origin 同时持续业务，确认不会相互撤销有效 Route；
- Full/Lite/Nano/Release/Sanitizer/Analyzer；
- 后续在多板 UART 和 ESP-NOW 上做多源并发实测。

#### 完成标准

不同 Origin 的数值 Epoch 永不直接形成大小关系；任何失败只影响对应 Discovery Domain，不破坏其他 Origin 已安装的合法路由。

### 5.3 HELLO 能力、路径 MTU 与旧节点边界

#### 当前缺口

当前已有静态 Peer Class、窗口能力、Binding 最大消息等级、E2E 策略和多种 MTU 软件测试，但以下内容还没有形成完整产品闭环：

- HELLO 自动交换并缓存能力；
- 整条路径最小 MTU 的主动确认；
- Endpoint 级独立 TX 安全映射；
- Bearer 切换时能力/MTU 的重新验证；
- 新旧固件、不同 Wire Profile 和不同窗口能力的实机互操作。

#### 实施方案

1. 定义 Capability 的来源、租期和失效规则；
2. HELLO 只发布本节点真实能力，远端值不能提升本地授权；
3. Path 能力取各跳交集，MTU 取各跳有效最小值；
4. Route/Path/Bearer 改变时使旧协商结果失效；
5. Transfer 发送前检查等级、窗口、E2E 和 Path MTU；
6. 未知旧节点默认退化到最保守能力或明确拒绝，禁止猜测；
7. 诊断输出协商来源、有效期和失败原因。

#### 完成标准

在能力未知、信息陈旧、路径改变或中间节点不支持时，发送端必须在占用大量队列之前确定性失败或安全降级，不允许传到中途才因不可表达而静默丢失。

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

## 11. 第七优先级：发布工程与兼容性冻结

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

1. **Developer Preview**：软件合同可用，明确不含生产安全和完整硬件门禁；
2. **Core v5 Release Candidate**：Core 协议冻结，一个参考 MCU/RTOS/Bearer 组合完整通过；
3. **Core v5 Stable**：安全、资源、长稳、兼容和回滚签字；
4. **Cluster Target Preview/RC/Stable**：独立于 Core 的后续发布线。

## 12. 分阶段执行顺序

### 阶段 A：Core 软件冻结

```text
A1  QOS-A01～A08 最终外审
A2  V5-64 多 Origin Route Epoch 设计
A3  V5-64 实现与软件矩阵
A4  HELLO Capability / Path MTU / 旧节点边界
A5  Core API/Wire/Storage 兼容复核
```

阶段完成后，Core 数据面原则上不再接受无发布必要的破坏性修改。

### 阶段 B：第一参考平台

```text
B1  ESP32-S3 + FreeRTOS 产品配置
B2  UART/RS-485 生产 Adapter
B3  ESP-NOW 生产 Adapter 与 readiness
B4  多 Source Event Runtime
B5  Nano/Lite/Full 资源和性能
B6  断链、重连、长稳和故障注入
```

### 阶段 C：生产安全

```text
C1  威胁模型和身份生命周期冻结
C2  逐跳控制面认证
C3  E2E AEAD Provider
C4  Replay/Session/Key 持久化
C5  ACL、轮换、撤销和升级回滚
C6  安全外审与实机攻击测试
```

安全接口设计可以与阶段 B 并行，但最终实机签字必须使用真实参考平台。

### 阶段 D：多 Bearer 和发布候选

```text
D1  CAN/CAN-FD
D2  USB CDC
D3  多 Bearer Cost 标定
D4  Transfer 切换、多源并发和 QoS
D5  24 h 长稳、CPU/Stack/RAM/Power
D6  Core v5 RC 全矩阵与回滚演练
```

### 阶段 E：Cluster Target

```text
E1  收敛 M08～M14 外审台账
E2  M05 v4 生产 RX/TX/FSM/Authority 接线
E3  真实 Flash Persistence/掉电
E4  C06 跨簇 Transfer、规模和 MCU
E5  C07.7 主备/恢复实机
E6  Cluster Target RC
E7  C08 两级分簇与 1k/10k 门禁
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

1. 将 `69901bf` 作为 Q0～Q3 外部复审基线；
2. 外审无新 P0/P1 后冻结 Q0～Q3 软件合同；
3. 建立 V5-64 专项设计文档和任务表；
4. 使用 Host 模拟完成多 Origin Route Epoch，不等待硬件；
5. 再进入 HELLO Capability/Path MTU；
6. 同时确定第一个参考产品为哪种 MCU、RTOS 和 Bearer 组合；
7. 在参考产品上开始 S02 和真实 Adapter 闭环。

如果当前只选择一个代码任务，应选择 **V5-64 多 Origin Route Epoch 所有权**。它是现有 Core 路由层明确登记、可以纯软件先完成、又可能影响后续数据结构和兼容冻结的剩余问题。

## 16. 最终完成定义

### 16.1 Core v5 完成

- Q0～Q3、Route、Path、Service、Transfer 和 Capability 合同冻结；
- 多 Origin 路由通过；
- S02 生产安全通过，或者发布范围明确且物理隔离地声明不提供安全边界；
- 至少一个 MCU+RTOS+UART/无线组合生产闭环；
- CAN/USB 若列入支持矩阵则必须实测；
- 目标资源、长稳和回滚通过；
- 外审关闭 P0/P1；
- 文档、固件和测试绑定同一 commit。

### 16.2 Cluster Target 完成

- M05 v4 生产路径放行；
- Persist-before-promise 在真实 Flash/掉电下成立；
- Joint、Authority、Backup、Takeover、Handover、Recovery、Rekey 在生产 Owner 中闭环；
- 跨簇小消息和 Transfer 闭环；
- 主备、分区、合并、旧 Head 回归和 Directory Handover 实机通过；
- 资源、长稳、安全和升级回滚通过；
- M14 从 `RELEASE NO-GO` 转为有明确候选基线的 GO。

### 16.3 大规模能力完成

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
