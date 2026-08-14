# UCN 内部实现模块

src/ 只放 UCN 的内部实现；产品代码不应直接包含这里的私有文件。稳定的公开入口始终是 [../include/ucn/](../include/ucn/)。

| 目录 | 责任 | 允许依赖 | 禁止依赖 |
| --- | --- | --- | --- |
| core/ | 配置校验、Frame Codec、Endpoint 基础语义、纯定点 LC-1 Cost Resolver | 公共头、C99 标准库 | Node 状态、路由策略、Port、SDK/OS |
| node/ | Node 生命周期、Neighbor、HELLO、重复抑制、Profile Stub | core/ 的公开语义、公共头 | Platform Port、SDK/OS |
| transport/ | Link/Adapter 队列、标准 Preset 解析、Protocol Owner、固定多 Source Event Runtime | Core/Node 公共 API | 具体 RTOS/驱动、UART/CAN/USB SDK 类型 |
| adapters/stream/ | 调用者存储的 Byte Ring、COBS+0 Stream Carrier、Owner-only Source | Event Runtime、Frame 上限、C99 标准库 | UART/USB SDK、GPIO、DMA Handle、RTOS 对象 |
| adapters/can/ | 调用者存储的 CAN Frame Ring、CAN-FD DLC 补齐、经典 CAN 有界 Carrier/重组、Bus State | Event Runtime、Frame 长度探测、C99 标准库 | CAN SDK、控制器寄存器、收发器/引脚、自动硬件恢复 |
| routing/ | AODV 路径、Candidate、Policy/负载均衡 | Node/Frame 公共语义 | Port、SDK/OS |
| service/ | 本机 Service Router 与跨 MCU Service Bridge | Node/Endpoint/Frame 公共语义 | Port、SDK/OS |
| ports/ | 裸机、各 RTOS、Host Fake 的独立运行外壳 | include/ucn/... 公共 API | 其它 Port 私有实现 |
| extended/ | 按需链接的 T32～T8K Transfer、固定分片/重组与 ACK | Node/Frame/Endpoint 公共 API | 动态内存、Port、SDK/OS、在中继重组 |

真实 UART、CAN、Wi-Fi、USB 等 BSP/SDK Adapter 放入产品工程。当前 `stream/` 和 `can/` 是可直接复用的 SDK 无关载体，不会自行打开外设、选择引脚、设置过滤器或创建任务；两种 Source 不共享 Ring 或 Carrier 状态。

V5-62 起所有 transport/adapter/port 初始化统一要求 Port API V2 的 `struct_size/api_version`，Transfer 统一从配置回调取权威时间。该项允许预发布源码/ABI 破坏但不改变 Wire；任何产品必须清理旧对象并全量重编译。经典 CAN Source 在完成 Carrier 提交前不再消费下一条物理帧。

详细规则见 [../docs/架构/02-代码模块与依赖规则.md](../docs/架构/02-代码模块与依赖规则.md)。
