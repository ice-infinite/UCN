# UCN 内部实现模块

src/ 只放 UCN 的内部实现；产品代码不应直接包含这里的私有文件。稳定的公开入口始终是 [../include/ucn/](../include/ucn/)。

| 目录 | 责任 | 允许依赖 | 禁止依赖 |
| --- | --- | --- | --- |
| core/ | 配置校验、Frame Codec、Endpoint 基础语义、纯定点 LC-1 Cost Resolver | 公共头、C99 标准库 | Node 状态、路由策略、Port、SDK/OS |
| node/ | Node 生命周期、Neighbor、HELLO、重复抑制、Profile Stub | core/ 的公开语义、公共头 | Platform Port、SDK/OS |
| transport/ | Link/Adapter 队列、标准 Preset 解析、Protocol Owner | Core/Node 公共 API | 具体 RTOS/驱动 |
| routing/ | AODV 路径、Candidate、Policy/负载均衡 | Node/Frame 公共语义 | Port、SDK/OS |
| service/ | 本机 Service Router 与跨 MCU Service Bridge | Node/Endpoint/Frame 公共语义 | Port、SDK/OS |
| ports/ | 裸机、各 RTOS、Host Fake 的独立运行外壳 | include/ucn/... 公共 API | 其它 Port 私有实现 |

真实 UART、CAN、Wi-Fi、USB 等 Adapter 将在实际实现时放入独立的 src/adapters/<bearer>/；在此之前不创建空目录或伪驱动。

详细规则见 [../docs/架构/02-代码模块与依赖规则.md](../docs/架构/02-代码模块与依赖规则.md)。
