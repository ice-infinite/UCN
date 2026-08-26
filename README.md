# UniLink / UCN

**UniLink** 是协议品牌名；**UCN（Unified Communication Network）** 是正式协议与架构缩写，代码使用 `ucn_*` 前缀。

UCN 是面向 MCU 自组网的 C99 通信库。它通过统一的 Node、Endpoint、Link、Route、Service 和 Transfer 语义，把 UART、CAN、Wi-Fi、ESP-NOW、BLE、USB、RS-485、LoRa 等承载与业务逻辑解耦。

Linux、ROS 2、MAVLink 和地面站可以作为 Host/Bridge 接入，但不是组网、寻路或转发的前提；没有 Linux 时，MCU 节点仍可独立运行。

## 架构

```text
业务任务
  ├─ Service：任务/服务请求、结果与本机 Fast Path
  ├─ Transfer：T32～T8K 有界消息、分片/重组/ACK
  └─ Core：Node / Endpoint / Route / Path / Policy
          └─ Adapter / Source：Stream、CAN 或产品自定义介质
                  └─ Port / BSP / Driver

可选 Cluster：成员、Head/Backup、Authority、Config、Recovery
可选 Host：Linux、ROS 2、网关、诊断工具
```

## 当前能力

- Core Wire v5，W0～W3 Adaptive Wire Class；
- 静态对象、固定表、有界队列，UCN 核心不依赖动态分配；
- HELLO/Neighbor/Heartbeat、AODV-Lite、RERR、Route/Path；
- Full Profile 的动态 Cost、Pinned/Failover 和 Q1 负载均衡；
- 可选 Service Router/Bridge 与 T32～T8K Transfer；
- 标准 Event Runtime、Port API v2、Stream Source、CAN/CAN-FD Source；
- Security Policy/Provider、E2E 透明密文转发与 ACL 合同；
- 可选 Cluster Current FSM，以及隔离的 Wire v4/Target 实验组件。

## 必须注意的边界

- 仓库提供通用 Adapter/Source 合同，不会自动配置芯片引脚、DMA、Wi-Fi SDK 或 CAN 控制器；
- Security 接口不等于仓库已经提供生产身份、密钥和审计 AEAD；
- 默认 Cluster 使用 v3/32 B Current Wire；v4/40 B encoder 默认关闭，生产 RX/TX/FSM 未放行；
- M10 Takeover、M11 Handover、M13 Rekey Archive 默认关闭；
- Cluster 仍为 `AUDIT HOLD / RELEASE NO-GO`，软件全绿不替代真实 Flash 掉电、多 Bearer、长期功耗和 MCU 资源验收。

详细成熟度见[当前能力、限制与成熟度](docs/official/00-项目总览/02-当前能力、限制与成熟度.md)。

## 构建

```powershell
cmake -S . -B build -DUCN_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

选择 Profile：

```powershell
cmake -S . -B build_nano -DUCN_PROFILE=NANO -DUCN_FEATURE_SERVICE=OFF
cmake -S . -B build_lite -DUCN_PROFILE=LITE
cmake -S . -B build_full -DUCN_PROFILE=FULL
```

## 文档

- [官方文档](docs/official/README.md)
- [源码参考与架构图](docs/reference/README.md)
- [验证证据](docs/evidence/README.md)
- [实验组件边界](docs/experimental/README.md)
- [历史归档](docs/archive/README.md)
- [任务表](docs/00-项目管理/00-任务表.md)
- [项目操作记录](docs/00-项目管理/01-项目操作记录.md)
- [函数调用树](docs/calltree/README.md)

## 源码目录

```text
include/ucn/  公共 C API
src/core/     Wire、配置和基础语义
src/node/     Node、Neighbor、Endpoint 生命周期
src/routing/  Route、Path、Policy、Cost
src/transport/ Link、Adapter、Owner
src/adapters/ Stream、CAN/CAN-FD Source
src/ports/    裸机、RTOS 与 Host Port
src/service/  Service Router/Bridge
src/extended/ Transfer、Cluster、Federation 与实验组件
tests/        单元、集成、负向和虚拟拓扑测试
tools/        规模模拟与文档/资源门禁
```

项目版本为 5.0.0，仍处于发布前优化阶段。兼容、迁移和发布判断请查阅[发布文档](docs/official/13-兼容、迁移与发布/README.md)。
