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

仓库同时发布 Core 源码、单元/虚拟拓扑测试、CMake 配置，以及 `docs/` 下的架构、协议设计、任务表和项目操作记录。建议先阅读 [UCN v4 协议核心说明](docs/UCN_v4_协议核心说明.md)，再按需要进入完整架构和专题设计文档。

## 目录

```text
include/ucn/  公共 C API 与固定资源配置
src/          C99 Core 实现
tests/        单元测试与虚拟 Link 集成测试
docs/         架构、协议设计、任务表与操作记录
CMakeLists.txt
```

## 构建与测试

需要 CMake 和一个支持 C99 的 C 编译器：

```powershell
cmake -S . -B build
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

这些测试验证 C99 Core 和虚拟 Link 拓扑；它们不等同于 ESP-NOW、WiFi、CAN、BLE、LoRa 或 UART 的真实硬件性能，也不等同于生产密钥/AEAD 安全验证。

## 使用边界

UCN-Core 只依赖抽象 `Link` 和通用 `route_cost`。各 Adapter 自行处理 RSSI、SNR、丢包、Bus-Off、CRC 错误、超时等介质指标，并将平滑后的结果提供给 Core。真实驱动、产品身份、密钥管理和硬件资源评估由具体产品工程完成。
