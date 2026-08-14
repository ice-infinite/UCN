# UCN

UCN（Unified Communication Network）是一个面向 MCU 自组网的 C99 通信核心库。它让应用通过统一 Node/Link 接口通信，而不把 WiFi、UART、CAN、BLE、LoRa 等具体承载方式带入路由核心。

Linux、ROS 2、MAVLink 或地面站可以作为普通 Host/Adapter 接入，但不是组网、路由或设备准入的前提；没有 Linux 时，MCU 节点仍可独立发现、转发和恢复通信。

## 当前能力

- 固定长度上限的帧、固定表和静态内存模型，不依赖动态内存。
- v5 线协议 Codec：官方 W0/W1/W2/W3 基础头为 17/21/26/30 B；Nano/Lite/Full 共用四档 Decoder，推荐发送采用最低够用档、接收默认开放至 W3。Node 也可按 MTU/安全策略收窄接收上限，或显式开启路由感知的最小档自动选择；任何字段不能表达时失败关闭，不静默截断。
- 全局公共编译配置位于 `include/ucn/ucn_config.h`；产品可用独立 `UCN_USER_CONFIG_HEADER` 只覆盖需要调整的项，未配置项继续使用统一默认和原头文件回退。
- 1 B 最大接收档声明的一跳 HELLO/准入、Heartbeat、压缩 RREQ、受限 AODV-Lite 路由发现、RERR 与路由/邻居老化；发送档与接收上限相互独立。
- Q0/Q1 有界发送调度、Pending Q1 绝对 Deadline、静态 Endpoint 业务分发和跨介质通用 `route_cost`。
- 可选 `ucn_transfer` Extended 库：按需选择 T32/T64/T128/T256/T512/T1K/T2K/T4K/T8K 九档逻辑消息上限；T128～T8K 使用固定 TX/RX Slot、MTU 自适应分片、CRC32、ACK/有界重试和显式 RX Handle 释放。V5-62 起 Transfer 必须配置权威单调时钟，Step 不再接收调用方缓存时间。Core-only 节点不链接该库，不增加 8 KiB 缓冲。当前实机证据包括三板 UART 两跳压力和四块 N16R8 的 UART-only 单源三跳 9/9；ESP-NOW 四节点、多源和其他 Bearer 仍待验收。
- W0/W1/W2/W3=`3/3/3/4 B` 的累计 Cost 控制域、Candidate Wire Profile 连续性，以及 Full/Lite/Nano 一致的运行期 Hop Scope 门禁。
- LC-1 本地动态 Cost：基础 `route_cost` 保持稳定并继续在线上累加；Full 用 Queue、TX/RX 失败、RTT、介质占用/质量和新鲜度生成本地 `effective_select_cost`，用于 Bearer、Candidate 与 Q1 Flow 排序，不把局部拥塞写入 Wire。Lite/Nano 保持静态基础 Cost。
- Adapter 将物理地址和驱动回调转换为有界 RX 队列；公共 `ucn_event_runtime_t` 可静态注册最多 8 个 UART/CAN/USB/Wi-Fi 等事件 Source，合并 Task/ISR 通知并按 Source/Round 预算唤醒唯一 Protocol Owner。UART、RS-485 与 USB CDC 可复用 `ucn_stream_source_t` 的固定 Ring/COBS；CAN-FD 与经典 CAN 可复用 `ucn_can_source_t` 的固定 Frame Ring、DLC 零填充校验、8 B 有界 Carrier、重组超时和 Bus-Off 状态。V5-62 Port API V2 要求 `ucn_port_ops_t` 显式携带结构大小/API 版本，旧对象必须全量重编译；连续经典 CAN Carrier 采用完成优先提交。ISR 不进入 Core，轮询只用于无中断平台、协议定时器或漏通知兜底；真实 BSP 驱动、DMA、控制器过滤器、收发器、引脚和 RTOS SDK glue 仍由产品实现。
- 按需路径追踪与低频节点快照诊断。节点快照默认拒绝远端请求，产品必须显式配置管理节点授权。
- 编译期 Nano/Lite/Full Feature Profile；Service Router/Bridge 可独立开启或移除。

仓库同时发布 Core 源码、单元/虚拟拓扑测试、CMake 配置，以及 `docs/` 下的架构、协议设计、任务表和项目操作记录。要先理解“UCN 最终要做到什么、极限如何计算”，阅读 [UCN 理论能力边界与最终目标](docs/UCN_理论能力边界与最终目标.md)；开始接入时再阅读 [工程架构索引](docs/架构/README.md) 与 [UCN 网络容量与关键参数总览](docs/UCN_网络容量与关键参数总览.md)，并按运行环境阅读 [UCN 快速使用手册](docs/快速使用手册/README.md) 和 [UCN 使用与调用手册](docs/UCN_使用与调用手册.md)。从旧工作区迁移先看 [V5-62 破坏性 API 修复报告](docs/UCN_V5_62_Port_API_V2与审计缺陷修复报告.md)，四节点扩展环问题见 [V5-63 修复与实测](docs/UCN_V5_63_扩展环Epoch一致性与四节点三跳实测.md)。Adapter 实现者还必须遵守 [Link Metrics 与 Cost 契约](docs/UCN_Link_Metrics与Cost契约.md)。需要追踪实际函数路径时进入 [UCN 调用关系树](docs/calltree/README.md)。继续开发时以 [任务表](docs/00-任务表.md) 为准。

## 目录

```text
include/ucn/  公共 C API；Node 静态存储布局由 owner 显式选择
src/core/     配置、Frame Codec、Endpoint 基础语义
src/node/     Node 生命周期、Neighbor、HELLO、Profile Stub
src/transport/ Link/Adapter 队列、Preset Resolver、Protocol Owner
src/adapters/ SDK 无关 Carrier/Source；含 Stream 与 CAN/CAN-FD 独立模块
src/routing/  AODV、Candidate、Path、Policy/负载均衡（Full）
src/service/  本机任务 Service Router/Bridge（可选）
src/ports/    裸机、各 RTOS、Host Fake 的独立 Port 外壳
src/extended/ 按需链接的有界大消息 Transfer；不进入 Core-only 产品
tests/        单元测试与虚拟 Link 集成测试；按逻辑组导航，不强制物理分目录
tools/        Host-only 规模模拟器与可复现阶梯脚本
docs/         架构、协议设计、任务表与操作记录
docs/架构/    系统边界、模块依赖、目录迁移与构建/测试地图
docs/results/ 规模模拟生成的汇总与逐节点 CSV 证据
CMakeLists.txt
```

## 构建与测试

需要 CMake 和一个支持 C99 的 C 编译器：

```powershell
cmake -S . -B build
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

需要在电脑上模拟大量 Node 时，可单独构建并运行 Host-only 目标：

```powershell
cmake --build build --target ucn_scale_sim --parallel
.\tools\run_ucn_scale_ladder.ps1 -BuildDir build -Traffic local
.\tools\run_ucn_scale_ladder.ps1 -BuildDir build -Traffic local -WireProfiles w0,w1,w2,w3,mixed
.\build\ucn_scale_sim.exe --nodes 254 --traffic local --wire-profile mixed --wire-mode fixed --quiet
```

选择裁剪档位：

```powershell
cmake -S . -B build_nano -DUCN_PROFILE=NANO -DUCN_FEATURE_SERVICE=OFF
cmake -S . -B build_lite -DUCN_PROFILE=LITE -DUCN_FEATURE_SERVICE=ON
cmake -S . -B build_full -DUCN_PROFILE=FULL -DUCN_FEATURE_SERVICE=ON
```

这些测试验证 C99 Core 和虚拟 Link 拓扑；它们不等同于 ESP-NOW、WiFi、CAN、BLE、LoRa 或 UART 的真实硬件性能，也不等同于生产密钥/AEAD 安全验证。

## 使用边界

UCN-Core 只依赖抽象 `Link` 和通用指标。各 Adapter 自行处理 RSSI、SNR、丢包、Bus-Off、CRC 错误、超时等介质细节，并按契约分别提供基础 `route_cost`、可选 RTT、TX/RX 失败率、Adapter 队列压力、介质占用/质量和单调时间戳。指标缺失可保持 Invalid，禁止伪造；同一个物理计数不得同时作为 busy 与 quality 重复扣分。真实驱动、产品身份、密钥管理、Cost 标定和硬件资源评估由具体产品工程完成。

只传递 `ucn_node_t *` 的业务、Adapter 和 Port 头文件包含 `ucn_node.h`；只有唯一 Protocol Task 的 Node 所有者在需要静态分配时包含 `ucn_node_storage.h`。存储字段不是应用 ABI，禁止跨任务直接访问。
