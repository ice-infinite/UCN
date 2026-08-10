# UCN

UCN（Unified Communication Network）是一个面向 MCU 自组网的 C99 通信核心库。它让应用通过统一 Node/Link 接口通信，而不把 WiFi、UART、CAN、BLE、LoRa 等具体承载方式带入路由核心。

Linux、ROS 2、MAVLink 或地面站可以作为普通 Host/Adapter 接入，但不是组网、路由或设备准入的前提；没有 Linux 时，MCU 节点仍可独立发现、转发和恢复通信。

## 当前能力

- 固定长度上限的帧、固定表和静态内存模型，不依赖动态内存。
- v4 线协议：32 B 基础头、36 B 路由扩展头、CRC，以及可选端到端受保护业务帧 Provider 边界。
- 一跳 HELLO/准入、Heartbeat、受限 AODV-Lite 路由发现、RERR 与路由/邻居老化。
- Q0/Q1 有界发送调度、静态 Endpoint 业务分发和跨介质通用 `route_cost`。
- Adapter 将物理地址和驱动回调转换为有界 RX 队列；协议任务中再执行路由和应用回调。
- 按需路径追踪与低频节点快照诊断。节点快照默认拒绝远端请求，产品必须显式配置管理节点授权。
- 编译期 Nano/Lite/Full Feature Profile；Service Router/Bridge 可独立开启或移除。

仓库同时发布 Core 源码、单元/虚拟拓扑测试、CMake 配置，以及 `docs/` 下的架构、协议设计、任务表和项目操作记录。开始接入时先阅读 [UCN 网络容量与关键参数总览](docs/UCN_网络容量与关键参数总览.md)，再按运行环境阅读 [UCN 快速使用手册](docs/快速使用手册/README.md) 和 [UCN 使用与调用手册](docs/UCN_使用与调用手册.md)；Adapter 实现者还必须遵守 [Link Metrics 与 Cost 契约](docs/UCN_Link_Metrics与Cost契约.md)。需要追踪实际函数路径时进入 [UCN 调用关系树](docs/calltree/README.md)；需要继续开发时先读 [稳定化第二阶段问题与执行建议](docs/UCN_稳定化第二阶段问题与执行建议.md) 和 [任务表](docs/00-任务表.md)。动态网络软件验收与复现命令见 [S05 动态综合压力测试](docs/UCN_S05_动态综合压力测试.md)，极限规模、全节点高负载和逐节点 CSV 见 [S21 方案](docs/UCN_S21_极限规模模拟测试方案.md) 与 [S21 结果](docs/UCN_S21_极限规模模拟测试结果.md)，高并发重复抑制、RREQ 拆分和生产安全激活门禁见 [S22～S26 稳定化修复报告](docs/UCN_S22_重复抑制与稳定化修复报告.md)，小 RAM MCU 的档位选择和资源证据见 [S04 Feature Profile 与资源报告](docs/UCN_S04_Feature_Profile与资源报告.md)，公共 API/静态存储和 Replay 边界见 [S08 公共 API 与静态存储边界](docs/UCN_S08_公共API与静态存储边界.md)，再按需要阅读 [UCN v4 协议核心说明](docs/UCN_v4_协议核心说明.md)、[UCN v5 Adaptive Wire Profile 设计方案](docs/UCN_v5_Adaptive_Wire_Profile设计方案.md)、完整架构和专题设计文档。

## 目录

```text
include/ucn/  公共 C API；Node 静态存储布局由 owner 显式选择
src/          C99 Core 实现
tests/        单元测试与虚拟 Link 集成测试
tools/        Host-only 规模模拟器与可复现阶梯脚本
docs/         架构、协议设计、任务表与操作记录
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
```

选择裁剪档位：

```powershell
cmake -S . -B build_nano -DUCN_PROFILE=NANO -DUCN_FEATURE_SERVICE=OFF
cmake -S . -B build_lite -DUCN_PROFILE=LITE -DUCN_FEATURE_SERVICE=ON
cmake -S . -B build_full -DUCN_PROFILE=FULL -DUCN_FEATURE_SERVICE=ON
```

这些测试验证 C99 Core 和虚拟 Link 拓扑；它们不等同于 ESP-NOW、WiFi、CAN、BLE、LoRa 或 UART 的真实硬件性能，也不等同于生产密钥/AEAD 安全验证。

## 使用边界

UCN-Core 只依赖抽象 `Link` 和通用指标。各 Adapter 自行处理 RSSI、SNR、丢包、Bus-Off、CRC 错误、超时等介质细节，并按契约分别提供基础 `route_cost`、可选 RTT、发送失败率和 Adapter 队列压力。真实驱动、产品身份、密钥管理和硬件资源评估由具体产品工程完成。

只传递 `ucn_node_t *` 的业务、Adapter 和 Port 头文件包含 `ucn_node.h`；只有唯一 Protocol Task 的 Node 所有者在需要静态分配时包含 `ucn_node_storage.h`。存储字段不是应用 ABI，禁止跨任务直接访问。
