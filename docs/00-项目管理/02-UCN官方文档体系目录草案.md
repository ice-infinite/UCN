# UCN 官方文档体系目录草案

> 状态：**目录草案，待确认**
> 基线：2026-08-25 当前工作区源码、公共头文件、CMake 目标与测试
> 本文只冻结“要写哪些官方文档、各自负责什么”，不代表正文已经完成。旧建议、审计记录和历史设计不会被直接当作当前实现。

## 1. 重建目标

新的文档体系必须同时满足以下要求：

1. **源码是事实源**：公开 API 以 `include/ucn/` 为准，模块与构建边界以 `CMakeLists.txt`、`src/` 为准，行为结论必须有测试或实机证据。
2. **现状、实验和规划分离**：默认产品能力、默认关闭的实验组件、尚未实现的建议不得写在同一个“当前能力”口径中。
3. **MCU-first**：以无 Linux 的 MCU 独立运行作为主线；Linux、ROS 2 和地面站只作为 Host/Bridge 集成方式。
4. **一项事实只有一个权威入口**：其他文档通过链接引用，不复制会漂移的版本号、默认值、结构尺寸和状态表。
5. **证据绑定版本**：性能、资源和实机报告必须记录 commit、配置、工具链、硬件、拓扑与限制，不能把 Host 模拟写成 MCU 实测。
6. **保留历史但退出官方口径**：旧建议、聊天整理、阶段自审和过期方案进入归档，不删除追溯证据，也不继续占据官方阅读主线。

## 2. 文档状态标记

每篇新官方文档的页首必须包含状态块：

| 字段 | 含义 |
| --- | --- |
| 文档级别 | `NORMATIVE` 规范、`GUIDE` 指南、`REFERENCE` 参考、`EVIDENCE` 证据、`EXPERIMENTAL` 实验、`ARCHIVED` 归档 |
| 适用版本 | Wire、API、Storage 或项目版本范围 |
| 实现状态 | `CURRENT`、`DEFAULT-OFF`、`PARTIAL`、`PLANNED`、`SUPERSEDED` |
| 事实源 | 对应公共头、源码、CMake 目标、测试或硬件报告 |
| 最近核对 | commit、日期、核对人/流程 |
| 硬件状态 | 未测、部分实测、完整实测；必须写明平台与拓扑 |

只有 `docs/official/` 中标为 `CURRENT` 的内容可以作为“UCN 当前官方能力”引用。`EXPERIMENTAL` 和 `EVIDENCE` 不能自动升级为生产承诺。

## 3. 顶层目录

```text
docs/
├── README.md                         # 唯一总入口：先看什么、当前边界、文档状态
├── 00-项目管理/                      # 内部任务、操作记录、评审流程、重建计划
├── official/                         # 当前官方文档；只写源码和已验证事实
│   ├── 00-项目总览/
│   ├── 01-总体架构/
│   ├── 02-核心协议/
│   ├── 03-路由与链路/
│   ├── 04-传输与服务/
│   ├── 05-Adapter与平台/
│   ├── 06-安全/
│   ├── 07-Cluster簇/
│   ├── 08-配置与资源/
│   ├── 09-API参考/
│   ├── 10-集成指南/
│   ├── 11-诊断与运维/
│   ├── 12-测试与一致性/
│   └── 13-兼容、迁移与发布/
├── reference/                        # 由源码生成或机械核对的表、图、调用关系
├── evidence/                         # 绑定 commit 的测试、资源、模拟和实机证据
├── experimental/                     # 默认关闭组件、尚未生产接线的 RFC/模型
├── archive/                          # 旧建议、过期设计、自审、外审、对话与复盘
├── calltree/                         # 现有机器可读调用树，暂保留稳定路径
└── results/                          # 现有机器生成 CSV/结果，暂保留稳定路径
```

`calltree/` 和 `results/` 暂不迁移，避免破坏工具和历史证据路径；它们分别由 `reference/README.md` 与 `evidence/README.md` 建立导航。

## 4. `official/00-项目总览`

| 文档 | 负责内容 | 主要事实源 |
| --- | --- | --- |
| `README.md` | 官方文档阅读路线；按开发者、集成者、审计者分流 | 本目录全部官方文档 |
| `01-项目定位、目标与非目标.md` | MCU-first、无 Linux 独立组网、统一多 Bearer；明确不替代 Linux 网络栈、不等同 Wi-Fi Mesh/DroneCAN/ROS 2 | `README.md`、模块边界 |
| `02-当前能力、限制与成熟度.md` | 一张表列出 Core、Routing、Service、Transfer、Cluster、Security、Port 的当前状态和未完成项 | CMake 目标、测试、实机证据 |
| `03-术语、缩写与命名规则.md` | UniLink/UCN、Node、Link、Bearer、Endpoint、Route、Path、Cluster、Epoch 等 | 公共类型与源码命名 |
| `04-版本组成与兼容性概览.md` | 项目版本、Core Wire、Cluster Wire、API、Storage Schema 分开编号 | 各公共头版本宏 |
| `05-功能与模块索引.md` | “我想做某事，应看哪个模块/API/指南”的总索引 | 全仓库 |

## 5. `official/01-总体架构`

| 文档 | 负责内容 | 主要事实源 |
| --- | --- | --- |
| `README.md` | 架构阅读入口和总图 | 下列架构文档 |
| `01-UCN总体架构与设计原则.md` | Core、Extended、Adapter、Port、Host 的正式分层；小核心、静态内存、失败关闭 | `src/README.md`、CMake |
| `02-模块边界与依赖规则.md` | `core/node/transport/adapters/routing/service/ports/extended` 的允许依赖和禁止依赖 | `src/`、CMake targets |
| `03-数据平面、控制平面与诊断平面.md` | 业务帧、HELLO/路由控制、Cluster 控制、诊断查询的独立职责 | Node/Cluster/诊断源码 |
| `04-Protocol Owner、并发与ISR模型.md` | 唯一 Owner、ISR→Ring→事件→Owner、轮询兜底、线程安全责任 | Port API V2、Event Runtime |
| `05-对象所有权、生命周期与静态存储.md` | Node/Transfer/Cluster/Adapter/Source 的 owner、借用、初始化、销毁与 opaque storage | 公共头与 storage 头 |
| `06-源码目录、构建产物与链接关系.md` | 每个静态库、可选库、默认关闭实验 Archive、测试目标的关系 | `CMakeLists.txt` |
| `07-一帧数据从任务到物理接口的完整路径.md` | Service/Endpoint→Node→Route/Policy→Link/Adapter→Source/Driver 的调用链 | 源码与 calltree |
| `08-内存、时间与失败关闭三条基础契约.md` | 无动态分配、32 位单调时间/Deadline、非法或不可表达状态拒绝 | 配置、时间函数、测试 |

## 6. `official/02-核心协议`

| 文档 | 负责内容 | 主要事实源 |
| --- | --- | --- |
| `README.md` | Core 协议入口及推荐阅读顺序 | 本目录 |
| `01-基础类型、ID与网络域.md` | Node ID、Network ID、Session、Sequence、Endpoint、Link ID、Path ID 的合法域 | `ucn_types.h` |
| `02-Wire-v5-W0至W3帧格式.md` | 四档头、字段布局、发送档选择、接收上限、长度和编码失败规则 | `ucn_frame.h/.c`、Golden tests |
| `03-帧语义、Payload与开销计算.md` | 每帧字段、Payload 上限、安全 Tag、各档有效载荷与效率计算 | Frame/Security 配置 |
| `04-Node初始化、Step与发送接收生命周期.md` | 初始化、Link 注册、Endpoint 注册、send、receive、step 的正式时序 | `ucn_node.h/.c` |
| `05-Neighbor发现、HELLO、准入与心跳.md` | 一跳发现、直连心跳、状态与 Bearer 生命周期、入离网时序 | Neighbor/HELLO tests |
| `06-Endpoint与业务消息分发.md` | 同一节点多类数据如何用 Endpoint/Service 区分，回调和 ACL 边界 | Endpoint/Node/Service |
| `07-Q0-Q1调度、Deadline与背压.md` | FIFO/Latest、维护帧预算、Q0 重试、队列满语义、不是心跳时才发送 | Node/Service tests |
| `08-Sequence、Session、重复抑制与重放边界.md` | Source Window、RREQ Cache、Session rollover、安全重放边界 | Node/Security tests |
| `09-Hop、TTL、时间与错误模型.md` | Hop Scope、Deadline 回绕安全、错误码和调用者责任 | `ucn_time.h`、Node |
| `10-Profile协商、MTU与能力失败关闭.md` | Wire Profile、Peer capability、动态 MTU、异构 Bearer 的共同能力选择 | Frame/Node/Path tests |

## 7. `official/03-路由与链路`

| 文档 | 负责内容 | 主要事实源 |
| --- | --- | --- |
| `README.md` | 自动路由、固定路径和负载均衡的关系 | 本目录 |
| `01-Link、Bearer、Neighbor、Route与Path关系.md` | 多串口、多 CAN、多无线如何形成独立 Bearer/Link 并参与同一网络 | Link/Neighbor/Path |
| `02-AODV-Lite路由发现与RREQ-RREP-RERR.md` | 按地址寻路、反向路由、下一跳、错误传播、广播范围 | Node/Routing tests |
| `03-Route与Candidate缓存、老化和失效.md` | 缓存用途、何时重找、断链清理、候选替换 | Node route tests |
| `04-Path安装、转发与完整路径诊断.md` | 显式 Path、Path ID、转发表、Path Trace 查询与边界 | `ucn_path.h`、path tests |
| `05-Link Metrics与LC-1动态Cost.md` | 基础 Cost、Queue/失败/RTT/占用/质量/新鲜度、不可重复扣分 | Link Cost/metrics tests |
| `06-自动选路、指定路径与故障回退.md` | Auto、固定路径、固定优先但断线回退、能力不匹配 RERR | Policy/Path/Node |
| `07-Q1负载均衡与Flow粘滞.md` | 自动负载均衡范围、Flow lease、拥塞触发、Q0 不参与的原因 | `ucn_policy.h/.c` |
| `08-多Bearer故障检测与切换时序.md` | 探测、验证、切换、缓存失效和正在发送数据的影响 | Node/Policy tests |
| `09-路由规模、跳数、寻址距离与复杂度.md` | 编译上限、跳数限制、表项/控制开销和延迟边界；不把地址宽度当容量保证 | 配置、规模模拟 |

## 8. `official/04-传输与服务`

| 文档 | 负责内容 | 主要事实源 |
| --- | --- | --- |
| `README.md` | Core 单帧、Transfer 大消息、Service 任务通信的选择关系 | 本目录 |
| `01-消息大小等级T32至T8K.md` | 32/64/128/256/512/1K/2K/4K/8K 的选择、内存代价与能力协商 | `ucn_transfer.h` |
| `02-分片、重组、CRC32与RX-Handle.md` | 分片头、重组槽、完整校验、零拷贝/释放责任 | Transfer 源码/tests |
| `03-ACK、窗口、重试、并发与Deadline.md` | 累计 ACK、Go-Back-N、Window 1～8、Peer 并发和完成状态 | Transfer tests |
| `04-MTU变化、中继与多跳Transfer.md` | 运行期 MTU 收缩、每跳转发、断链/切换语义、当前限制 | Transfer/Node tests |
| `05-Service-Router、Binding与Inbox.md` | 本机任务 Router、Q0 FIFO、Q1 Latest、Binding/ready 生命周期 | `ucn_service.h/.c` |
| `06-Service-Bridge与跨MCU任务通信.md` | 本机 Fast Path 与远端 Bridge 共用语义，Core 接管网络路径 | Service Bridge tests |
| `07-命令、结果与业务确认语义.md` | “发给远端某任务并返回结果”、Command Guard、Result Endpoint；区分入队、传输 ACK、执行结果 | Service API/tests |
| `08-实时传感器流与控制消息使用原则.md` | IMU/气压/温度/舵机消息的 Endpoint、Q0/Q1、频率和丢弃策略 | Service/Node 契约 |
| `09-吞吐、延迟与背压调优.md` | 队列深度、窗口、消息档、Step 预算与 Bearer 速率的关系 | 配置、性能证据 |

## 9. `official/05-Adapter与平台`

| 文档 | 负责内容 | 主要事实源 |
| --- | --- | --- |
| `README.md` | Port、Adapter、Source、BSP Driver 的分工 | 本目录 |
| `01-Link与Adapter公共契约.md` | 物理地址、RX Queue、send 回调、Metrics、生命周期和多实例 | `ucn_link.h`、`ucn_adapter.h` |
| `02-Standard-Adapter预设与默认Cost.md` | 介质/速率 preset、逻辑 MTU、管理 bias、覆盖优先级 | `ucn_standard_adapter.h/.c` |
| `03-Event-Runtime与Protocol-Owner.md` | 多 Source、中断通知、预算、公平性、漏通知兜底 | Event Runtime/Owner |
| `04-Stream-Source与COBS载体.md` | UART/RS-485/USB CDC 可复用的 Ring、COBS+0、完整帧交付 | Stream Source tests |
| `05-CAN-FD与Classic-CAN载体.md` | DLC、Padding、8 B Carrier、重组槽、Bus-Off、过滤器责任 | CAN Source tests |
| `06-裸机Port.md` | superloop/函数式 Owner、临界区、定时和调用模板 | Bare Metal Port |
| `07-FreeRTOS-Port.md` | Owner Task、Task/ISR 临界区、通知与队列边界 | FreeRTOS Port |
| `08-Zephyr-Port.md` | Zephyr 对接对象和调用边界；不虚构未实现 SDK glue | Zephyr Port |
| `09-NuttX-Port.md` | NuttX 对接对象和调用边界 | NuttX Port |
| `10-RT-Thread-Port.md` | RT-Thread 对接对象和调用边界 | RT-Thread Port |
| `11-Host-Fake、Linux与ROS2边界.md` | Host Fake 测试；Linux/ROS 2 是普通 Host/Bridge，不是 MCU 组网依赖 | Host Fake、项目边界 |
| `12-WiFi-ESP-NOW-BLE-LoRa对接规范.md` | 这些介质如何实现产品 Adapter；明确仓库当前没有对应 SDK 驱动 | Adapter 公共契约 |
| `13-自定义Port、Adapter与Source模板.md` | 新 RTOS、新控制器和新介质的最小接口清单 | 公共 Port/Adapter API |
| `14-多串口、多CAN与多Bearer实例化.md` | 每个实例独立 Ring/状态/Link，如何同时挂入一个 Node | Source/Adapter 契约 |

## 10. `official/06-安全`

| 文档 | 负责内容 | 主要事实源 |
| --- | --- | --- |
| `README.md` | 安全文档入口；醒目标出“Core 不内置生产密码算法” | Security API |
| `01-威胁模型与信任边界.md` | 监听、篡改、重放、伪造节点、中继、物理攻击；哪些不在 UCN Core 内解决 | Security/Node 边界 |
| `02-安全Policy与Provider接口.md` | Plain/E2E/Auto、接收和转发模式、seal/open/authorize 合同 | `ucn_security.h` |
| `03-E2E-AAD、透明密文转发与逐跳边界.md` | 中间节点不解密、目标解密、可变路由字段与受保护身份字段 | Frame/Security tests |
| `04-Session、Sequence、持久化与重放窗口.md` | 序列持久化、Session rollover、掉电不得重用的产品责任 | Security Ops/Node |
| `05-Endpoint-ACL、Service命令保护与管理诊断.md` | Endpoint 授权、Q0 Validator、远端快照管理授权 | Node/Service tests |
| `06-生产密钥、AEAD与安全接入清单.md` | 产品必须提供的身份、密钥、审计 AEAD、Flash 安全与密钥轮换 | 当前缺口与门禁 |
| `07-当前安全能力与未完成项.md` | 已实现门禁、测试 Provider 与真实生产安全之间的差距 | 测试/任务状态 |

## 11. `official/07-Cluster簇`

> 本目录必须首先写清：当前默认 `ucn_cluster`、独立 Wire Archive、默认关闭的 M10/M11/M13 实验 Archive，以及尚未完成的生产 v4 RX/TX/FSM 不是同一个成熟度层级。

| 文档 | 负责内容 | 主要事实源 |
| --- | --- | --- |
| `README.md` | Cluster 阅读入口和“默认/实验/阻断”总表 | CMake、Cluster headers |
| `01-Cluster与Core的关系和适用场景.md` | Cluster 是可选单层控制平面，不改变 Core 路由基本职责 | `ucn_cluster` target |
| `02-Role、Phase、Epoch与Authority模型.md` | Current FSM 角色/阶段、Epoch 比较、Authority 与 Fence | Cluster headers/source |
| `03-当前默认Cluster-v3-32B行为.md` | 当前生产兼容 FSM、Type 1～19、选举/Join/租约/恢复边界 | v3 codec/Cluster tests |
| `04-Wire-v4-40B实验规范与双格式边界.md` | Type 1～33、严格分派、encoder 默认关闭、无生产 FSM 接线 | v4 archive/tests |
| `05-Membership、Provisional与Committed-VoterSet.md` | 成员状态、准入、v3 legacy 围栏、投票集合 | Membership model |
| `06-Persistence-Provider、Record与Persist-before-Promise.md` | Record schema、load/submit/poll、重入门、掉电合同 | Persist API/tests |
| `07-Config事务与Joint-Consensus.md` | C_old→C_joint→C_new、Prepare/Commit/Abort、Backup gate | Config components |
| `08-Quorum、Lease、Authority与Fence.md` | Authority preflight、租约、Owner budget、配置切换撤权 | Authority component |
| `09-Backup-Mirror、Snapshot、Coverage与Profile.md` | committed/staging mirror、快照/增量、protected voter coverage | Backup components |
| `10-Majority-Takeover实验模型.md` | 证书、VoteId、持久化和终态；明确默认 OFF 与外审状态 | M10 archive/tests |
| `11-Merge与Handover实验模型.md` | 跨簇/同簇切换、双 Epoch、READY/STEPDOWN/Fence | M11 archive/tests |
| `12-Recovery、Lineage与稳定权威优先.md` | Recovery ID、backoff、lineage adoption、tombstone | Recovery components |
| `13-Rekey、No-wrap与Cluster-ID历史实验模型.md` | serial threshold、Rekey quorum、successor、退役 ID | M13 archive/tests |
| `14-Federation、Locator、Directory与Tunnel.md` | 可选 Federation Archive、小消息 Tunnel、E2E 内层和当前限制 | Federation API/tests |
| `15-不变量、Safety-Liveness与诊断.md` | 运行期 invariant engine、10 条安全条件、已证和未证范围 | Invariant/tests |
| `16-Cluster配置、资源与容量.md` | 固定表、对象大小、控制预算、模拟规模；Host 不替代 MCU | Config/resource reports |
| `17-Cluster发布阻断、硬件验收与回滚.md` | M05 AUDIT HOLD、真实 Flash/掉电、四板、多 Bearer 和发布 NO-GO 条件 | M14 门禁/证据 |

## 12. `official/08-配置与资源`

| 文档 | 负责内容 | 主要事实源 |
| --- | --- | --- |
| `README.md` | 从默认配置到产品覆盖的入口 | Config headers |
| `01-全局配置、回退值与覆盖优先级.md` | `ucn_config.h`、`UCN_USER_CONFIG_HEADER`、原头 fallback 的优先顺序 | CMake/config tests |
| `02-Nano-Lite-Full-Profile功能矩阵.md` | 各档能力、Stub、Wire 接收能力、不可用 API 行为 | `ucn_profile.h`、Profile tests |
| `03-CMake选项、静态库与Feature开关.md` | 所有公开 CMake option、默认值、组合限制和生成 target | `CMakeLists.txt` |
| `04-固定表、队列、槽位与容量参数.md` | Link/Route/Neighbor/Path/Service/Transfer/Cluster 表上限 | Config/public headers |
| `05-RAM-Flash-Stack与动态分配边界.md` | 对象大小、Archive text、静态栈、0 动态分配；区分 Host 和 MCU | 资源门禁/MCU 报告 |
| `06-时间参数、维护周期与Deadline预算.md` | Step、HELLO、Heartbeat、路由、Transfer、Cluster 时间参数 | Config headers |
| `07-默认Cost、速率与动态Cost调优.md` | UART/CAN/Wi-Fi 等 preset 口径、基础 Cost 和动态增减公式 | Standard Adapter/Cost |
| `08-产品裁剪与容量规划方法.md` | 根据 RAM/Flash/节点数/并发选择 Profile、表和组件 | 编译配置/资源证据 |
| `09-配置合法性与编译失败排查.md` | Contract test、非法组合、static assert 和诊断方法 | Config tests |

## 13. `official/09-API参考`

| 文档 | 负责内容 | 主要事实源 |
| --- | --- | --- |
| `README.md` | 公共 API 总索引、版本、线程/Owner 规则 | `include/ucn/` |
| `01-Core-Types-Config-Time-Error.md` | 基础类型、版本、配置校验、Deadline、错误码 | `ucn.h/types/time/config` |
| `02-Frame与Wire-Profile-API.md` | Frame 编解码、Profile 描述和长度探测 | `ucn_frame.h` |
| `03-Node与Storage-API.md` | 初始化、Step、Send、RX、存储、状态快照 | Node headers |
| `04-Link-Neighbor-Endpoint-API.md` | Link ops/metrics、Neighbor、Endpoint 注册 | 对应公共头 |
| `05-Route-Path-Policy-Cost-API.md` | 路由、Path、策略、诊断与 Cost | Routing headers |
| `06-Adapter-StandardAdapter-API.md` | Adapter Queue、HELLO scheduler、preset resolver | Adapter headers |
| `07-Port-EventRuntime-Owner-API.md` | Port API V2、Runtime、Owner、各系统 Port | Port headers |
| `08-Stream与CAN-Source-API.md` | Source 配置、ISR push、service、health/stats | Adapter source headers |
| `09-Service与Bridge-API.md` | Router、Binding、消息、Guard/Result、Bridge | Service headers |
| `10-Transfer-API.md` | Class、Slot、Peer、Endpoint、Send/Step/Release | Transfer header |
| `11-Security-API.md` | Policy、Provider ops、产品实现责任 | Security header |
| `12-Cluster-Current-API.md` | 默认 Cluster、Storage、Epoch、Membership、Persistence | Cluster current headers |
| `13-Cluster-实验组件API.md` | Wire v4、Config、Authority、Backup、Takeover、Handover、Rekey；逐项标默认状态 | Experimental headers/CMake |
| `14-Federation-API.md` | Locator、Directory、Tunnel、Security provider | Federation header |
| `15-API所有权、返回值与失败写回规则.md` | output 不写回、borrowed/owned、同步/异步结果和线程规则 | 公共 API tests |

函数签名、枚举值和宏表尽量由 `reference/generated/` 从公共头生成；本目录主要解释语义，不手工复制易漂移的 900 余个公共声明。

## 14. `official/10-集成指南`

| 文档 | 目标 |
| --- | --- |
| `README.md` | 按场景选择指南 |
| `01-五分钟构建与最小Host示例.md` | CMake 构建、最小 Node、虚拟 Link |
| `02-裸机最小节点.md` | 静态对象、时钟、superloop、Link 回调 |
| `03-通用RTOS对接流程.md` | 唯一 Owner Task、ISR Ring、通知和锁 |
| `04-FreeRTOS接入.md` | FreeRTOS Port 的最小可编译模板 |
| `05-Zephyr接入.md` | Zephyr Port 的最小可编译模板 |
| `06-NuttX接入.md` | NuttX Port 的最小可编译模板 |
| `07-RT-Thread接入.md` | RT-Thread Port 的最小可编译模板 |
| `08-UART-RS485-USB-CDC接入.md` | Stream Source + 产品驱动/引脚配置 |
| `09-CAN与CAN-FD接入.md` | CAN Source + 控制器/过滤器/Bus-Off 处理 |
| `10-WiFi-ESP-NOW-BLE-LoRa接入.md` | 实现自定义 Adapter；明确不是调用仓库内置驱动 |
| `11-多Bearer与多实例节点.md` | 多串口、多 CAN、无线同时在线及 Cost 输入 |
| `12-任务间与跨MCU-Service通信.md` | 同一调用语义、本机 Fast Path、远端请求/结果 |
| `13-32B至8KiB-Transfer通信.md` | 选择 Class、注册 Peer/Endpoint、发送和释放 |
| `14-自动路由、固定路径与负载均衡.md` | 三种策略的配置和故障回退 |
| `15-Cluster受限接入.md` | 只按当前默认/实验边界接入，避免误启 v4/Authority |
| `16-安全Provider接入.md` | AEAD、持久化序列、Session、ACL 的产品清单 |
| `17-自定义Port-Adapter-Source.md` | 新平台和新介质扩展模板 |
| `18-从现有项目迁移到UCN-v5.md` | Port API V2、Cluster API/storage、Wire/Profile 迁移 |

## 15. `official/11-诊断与运维`

| 文档 | 负责内容 |
| --- | --- |
| `README.md` | 诊断入口与安全授权提醒 |
| `01-日志、统计与状态快照.md` | Node/Adapter/Source/Service/Transfer/Cluster stats 的含义 |
| `02-Neighbor、Route、Path与全网节点诊断.md` | 本地邻居、路由、Path Trace、低频 Node Snapshot 的能力边界 |
| `03-Link质量、Cost与负载诊断.md` | 判定哪一路更优、动态 Cost 变化来源与更新周期 |
| `04-入网、离网、断链与恢复排查.md` | 节点接入、失联、缓存清理、重选路和常见故障 |
| `05-队列背压、Transfer超时与吞吐排查.md` | Queue、ACK、窗口、Step、Source Ring 的分层定位 |
| `06-Cluster角色、Authority与Persistence排查.md` | Phase、Fence、Record、Backup、Recovery 的诊断顺序 |
| `07-升级、回滚与持久化介质处理.md` | Wire/API/Storage 升级，何时需要清除旧状态 |
| `08-故障码与排查索引.md` | 从错误码/计数器跳转到具体模块文档 |

## 16. `official/12-测试与一致性`

| 文档 | 负责内容 |
| --- | --- |
| `README.md` | 测试层级、可信度和复现入口 |
| `01-测试策略与证据等级.md` | 单元、集成、虚拟拓扑、规模、静态分析、实机各能证明什么 |
| `02-本地构建、CTest与常用目标.md` | Windows/WSL/GCC/MSVC 的可复现命令 |
| `03-模块到测试的追踪矩阵.md` | 每个公共模块对应哪些 tests 和尚缺哪些测试 |
| `04-Full-Lite-Nano与Feature矩阵.md` | Profile、Service-OFF、产品配置、Core-only 门禁 |
| `05-Sanitizer-Analyzer-Release与Fuzz.md` | ASan/UBSan、`-fanalyzer`、优化级别、确定性 fuzz |
| `06-Wire-Golden-Negative与兼容性测试.md` | v5 Core、Cluster v3/v4、长度/保留位/无写回 |
| `07-规模模拟、流量模型与结果解释.md` | `ucn_scale_sim`、`ucn_cluster_sim` 的参数和局限 |
| `08-硬件实测通用规范.md` | 固件 hash、Node ID、接线、原始日志、重复轮次、环境 |
| `09-UART-CAN-WiFi与多Bearer实测矩阵.md` | 介质专项验收项；未测项必须留空而不是推断 |
| `10-性能、资源、长稳与功耗验收.md` | 吞吐、延迟、丢包、CPU、RAM、Heap、1h/8h/24h |
| `11-安全与掉电恢复验收.md` | 真实 AEAD、Flash 双槽、撕裂写、可控掉电与恢复 |
| `12-发布一致性与回归清单.md` | 每次候选发布必须执行的完整门禁 |

## 17. `official/13-兼容、迁移与发布`

| 文档 | 负责内容 |
| --- | --- |
| `README.md` | 版本和发布入口 |
| `01-Wire-API-ABI-Storage兼容规则.md` | 四种兼容性分别判断，禁止只看项目 v5 |
| `02-Core-Wire-v5与Profile兼容矩阵.md` | W0～W3 收发、MTU、能力协商 |
| `03-Cluster-v3-v4兼容与隔离策略.md` | v3 compat、strict v4、dual stack test archive 和生产边界 |
| `04-Port-API-V2与公共结构迁移.md` | 破坏性升级影响和全量重编译要求 |
| `05-Cluster-API-Storage与Record迁移.md` | opaque storage、Record schema、legacy PREPARED 等迁移 |
| `06-版本升级与回滚步骤.md` | 固件、持久化状态、网络混跑和失败回退 |
| `07-发布门禁与签字清单.md` | 软件、外审、实机、安全、资源和文档门禁 |
| `08-CHANGELOG.md` | 只记录已合入变更、破坏性变化和迁移入口 |
| `09-支持矩阵与已知问题.md` | 平台、Profile、Bearer、组件成熟度、未解决限制 |

## 18. `reference/`、`evidence/`、`experimental/`、`archive/`

### 18.1 `reference/`

```text
reference/
├── README.md
├── generated/
│   ├── 公共API符号表.md
│   ├── 配置宏与默认值表.md
│   ├── 错误码与枚举值表.md
│   ├── Wire字段与长度表.md
│   ├── CMake目标与选项表.md
│   └── 模块-源码-测试映射表.md
└── diagrams/
    ├── 总体架构图.md
    ├── 发送接收时序图.md
    ├── 路由发现状态图.md
    ├── Transfer状态图.md
    └── Cluster-Current与Target状态图.md
```

现有 `calltree/` 继续作为机器可读函数级参考，由这里链接，不手工复制。

### 18.2 `evidence/`

```text
evidence/
├── README.md
├── current/                          # 当前候选 commit 的证据索引
├── software/                         # CTest、Profile、Sanitizer、Analyzer、Fuzz
├── simulation/                       # Scale/Cluster 模拟与参数
├── resources/                        # RAM/Flash/Stack/CPU；区分 Host 和 MCU
├── hardware/                         # 板卡、固件、接线、串口原始日志、实测报告
└── release-candidates/               # 每个候选版本的冻结证据包
```

现有 `results/` 保持机器生成路径，由本目录建立 commit 级索引。

### 18.3 `experimental/`

只保存**已经有源码或已冻结 RFC、但默认产品尚未启用/尚未完成生产接线**的内容：

- Cluster Wire v4 RFC 与语义 Codec；
- M07 Joint Config 受限实验组件；
- M08 Authority/Fence 受限实验组件；
- M09 Backup Mirror/Coverage 模型；
- M10 Majority Takeover 默认关闭 Archive；
- M11 Merge/Handover 默认关闭 Archive；
- M12 Recovery/Lineage 目标模型；
- M13 Rekey/No-wrap 默认关闭 Archive；
- 尚未完成生产接线的 Federation/跨簇能力说明；
- 已有代码原型但缺实机/安全/发布门禁的功能。

每篇必须链接对应官方“当前边界”文档，不得单独声明为当前生产能力。

### 18.4 `archive/`

```text
archive/
├── 建议与设想/
├── 过期架构与旧版本/
├── 阶段任务与实施计划/
├── 自审报告/
├── 外部审计与整改/
├── 实验对话与需求复盘/
└── superseded/                       # 已被新官方文档替代的旧使用/架构文档
```

归档文档保留原日期和上下文，页首统一加 `ARCHIVED / NOT CURRENT`，并链接替代它的官方文档。项目实时任务表和操作记录继续留在 `00-项目管理/`，不进入归档。

## 19. 全项目覆盖核对

| 项目组成 | 官方目录 | API/参考 | 集成 | 验证 | 状态隔离 |
| --- | --- | --- | --- | --- | --- |
| Core、Frame、Endpoint、Node | 01/02 | 09 | 10 | 12 | 当前能力 |
| Neighbor、HELLO、Heartbeat | 02 | 09 | 10/11 | 12 | 当前能力 |
| Route、Path、Policy、Cost | 03 | 09 | 10/11 | 12 | Full 与 Profile 差异明确 |
| Link、Adapter、Source | 05 | 09 | 10 | 12 | BSP 驱动与通用载体分离 |
| Bare metal、五类 Port | 05 | 09 | 10 | 12 | SDK glue 未实现项明确 |
| Service 与任务通信 | 04 | 09 | 10 | 12 | 当前可选组件 |
| Transfer T32～T8K | 04 | 09 | 10 | 12 | 独立 Archive |
| Security | 06 | 09 | 10 | 12 | Provider 契约与生产密码实现分离 |
| Cluster Current v3 | 07 | 09 | 10/11 | 12 | 当前默认与限制明确 |
| Cluster Wire v4/M07～M13 | 07 | 09 | 受限指南 | 12 | `experimental/`、默认关闭/AUDIT HOLD |
| Federation/Directory/Tunnel | 07 | 09 | 受限指南 | 12 | 可选 Archive 与未完成项明确 |
| 配置、Profile、资源 | 08 | reference | 10 | 12 | Host 与 MCU 证据分离 |
| 诊断、运维、升级 | 11/13 | reference | 10 | 12 | 管理授权与回滚明确 |
| 源码、构建、工具和测试 | 01/08/12 | reference | 10 | evidence | commit 绑定 |
| 历史建议、审计和对话 | 不进入 official | archive | 不适用 | 可追溯 | 明确非现状 |

该矩阵覆盖当前 `ucn_core`、`ucn_transfer`、`ucn_cluster`、Wire v3/v4、三个默认关闭 Cluster 实验 Archive、`ucn_cluster_federation`、六个 Port target、Stream/CAN Source、两个 Host simulator、公共 API、配置、测试、诊断、资源、安全和发布流程。

## 20. 现有文档迁移规则

| 现有文档类型 | 处理方式 |
| --- | --- |
| 能与当前源码逐项核对的说明 | 提取事实重写进 `official/`；旧文移入 `archive/superseded/` |
| “建议、方案、可行性、后续设计” | 有当前原型的移入 `experimental/`；纯设想移入 `archive/建议与设想/` |
| M03～M14 分项自审和外审 | 移入 `archive/自审报告/` 或 `archive/外部审计与整改/`；最终证据摘要进入 `evidence/` |
| 实机和压力测试报告 | 核对固件/commit/拓扑后进入 `evidence/hardware/`；缺关键信息的保留归档状态 |
| 旧快速手册和架构文档 | 新官方文档完成后标 `SUPERSEDED`，不并行维护两套事实 |
| CSV、调用树和机器输出 | 保留稳定路径，通过 reference/evidence 索引，不手工改写数据 |
| 任务表和操作记录 | 继续作为内部实时台账，不作为协议规范 |

第一阶段只移动和标记，不删除历史文档。只有新官方文档完成源码核对、链接替换和评审后，旧文才能标记 `SUPERSEDED`。

## 21. 建议编写顺序

```text
P0  00 项目总览
    → 01 总体架构
    → 02 核心协议
    → 03 路由与链路
    → 04 传输与服务
    → 05 Adapter 与平台

P1  06 安全
    → 07 Cluster（先写默认/实验边界，再写各组件）
    → 08 配置与资源

P2  09 API 参考
    → 10 集成指南
    → 11 诊断与运维
    → 12 测试与一致性
    → 13 兼容、迁移与发布

收尾 reference 自动表
    → evidence 当前证据索引
    → experimental/archived 迁移
    → 全仓库坏链、源码契约与文档一致性门禁
```

每完成一个一级目录，必须执行：源码核对、公共头核对、相关测试复跑、链接扫描、术语检查和状态边界检查；不能等全部写完再统一发现偏差。

## 22. 待确认事项

开始正文前需要确认以下目录原则：

1. 是否同意使用 `official/` 作为唯一当前官方文档区；
2. 是否同意将现有建议、自审、外审和过期方案保留但移入 `archive/`；
3. 是否同意默认关闭或未生产接线的功能统一进入 `experimental/`，同时在官方文档只描述它们的真实边界；
4. 是否同意 `calltree/`、`results/` 暂时保留原机器路径；
5. 是否同意按第 21 节顺序逐目录重写、核对并测试，而不是一次性机械改名。

目录获确认后，下一步先建立模板和状态规则，再从 `official/00-项目总览` 开始，不直接修改协议代码。
