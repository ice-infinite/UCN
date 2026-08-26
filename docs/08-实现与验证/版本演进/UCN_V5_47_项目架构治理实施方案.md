# UCN V5-47 项目架构治理实施方案

> 文档编号：DOC-045。
> 状态：已完成（软件/工程结构）。
> 范围：工程结构、依赖边界、构建目标、测试地图与文档导航；不改变 Wire、路由算法或运行功能。

## 1. 问题

UCN 已有 MCU-first 系统架构和协议分层，但源码大多位于 src 根目录，测试也主要平铺。Port 已完成独立目录拆分，Core、Node、Transport、Routing、Service 仍缺少对应的工程目录、依赖规则和统一入口。

这会导致后续新增 UART、WiFi、CAN、RTOS 或 Host 对接时，容易把平台逻辑写入 Core，或让测试、构建目标与真实模块边界失去对应关系。

## 2. 冻结的三条分类轴

| 分类轴 | 分类 | 作用 |
| --- | --- | --- |
| 系统职责 | UCN-Core / Product Port+Adapter / UCN-Host | 明确 MCU 独立运行，Linux 只是可选外接端。 |
| 工程模块 | core / node / transport / routing / service / ports | 决定源码目录、CMake 分组、调用树和测试归属。 |
| 编译档案 | Nano / Lite / Full，Service ON/OFF | 决定某一节点实际编译的能力，不能与目录或系统职责混同。 |

UCN-Extended 与 UCN-Host 当前都是系统边界和后续路线，不建立空源码目录假装已实现；只有实际实现时才以独立模块进入仓库。

## 3. 目标目录

    include/ucn/
    ├── 现有公开头：保持路径兼容
    └── ports/：独立裸机、FreeRTOS、Zephyr、NuttX、RT-Thread、Host Fake Port

    src/
    ├── core/：配置、Frame、Endpoint 与内部基础工具
    ├── node/：Node 生命周期、Nano 实现、Profile Stub、去重内部状态
    ├── transport/：Link Adapter、Preset Resolver、Protocol Owner
    ├── routing/：Path 与 Policy
    ├── service/：本机 Service Router 与 Protocol Bridge
    └── ports/：各平台独立运行 Port

    tests/
    ├── 保持既有路径兼容
    ├── README：按单元、模块、集成、Profile、配置、规模模拟分类
    └── config/：产品配置测试头

    tools/
    ├── scale：Host-only 规模模拟器
    └── regression：可重复测试脚本

    docs/02-总体架构/项目结构/
    ├── README：架构入口
    ├── 01-系统边界
    ├── 02-代码模块与依赖规则
    ├── 03-目录迁移与兼容策略
    └── 04-构建目标与测试地图

## 4. 不可破坏的依赖规则

    Product Adapter / Platform Port
             ↓
    transport -> node -> core
             ↓        ↑
    service ---+--------+
    routing ---+--------+

- core 不依赖 node、routing、service、ports、RTOS SDK、Linux 或具体介质驱动。
- node 只能依赖 core 和公开 transport 契约，不知道某个 RTOS、GPIO、UART 或 WiFi SDK。
- transport 承担 Link、RX Queue、Protocol Owner 和静态 Preset；不实现路由策略和业务任务。
- routing 只能通过 Node 公开/内部边界维护 Path 与 Policy；不直接驱动 Link。
- service 通过唯一 Protocol Owner/Bridge 使用 Node；业务 Task 不直接并发访问 Node。
- ports 只调用公共 Protocol Owner；一个产品只链接一个平台 Port。
- 真实 Bearer Adapter 未来位于 transport/adapters/<bearer>/<platform>，不得进入 core；在没有实现前不创建空目录。

## 5. 执行步骤

1. 建立 `docs/02-总体架构/项目结构` 导航、职责、依赖、迁移和构建测试文档；登记 V5-47/DOC-045。
2. 迁移内部 src 文件到六个模块目录，并将 CMake 从单一平铺列表改为模块源集。
3. 更新 README、调用树、源码地图与测试逻辑分类；公开 include/ucn 的既有路径保持不变。
4. 执行 Full/Lite/Nano、Service OFF、128 B 产品配置、WSL GCC 和 Core-only 构建；检查目录、CMake、调用树和文档链接。

## 6. 验收

- src 根目录不再保留业务 C 源文件，只保留模块目录。
- CMake 源集按模块命名，Profile/Feature 条件只追加对应模块。
- 公开头路径、函数名、Wire、Node 内存布局和运行行为不因目录整理改变。
- 目标 CMake 产品只链接 ucn_core 与一个选择的 ucn_port_<platform>。
- 全部软件回归通过；未将 Host/Extended、真实 Adapter、SDK 或实机验收伪造为已完成。

## 7. 实施与验证结果

- 实施：内部 C99 实现已移动到 `src/core`、`src/node`、`src/transport`、`src/routing`、`src/service`、`src/ports`。公开 `include/ucn/...` 路径、函数签名、Wire、Profile 和 Node 存储布局没有变化。
- 构建：CMake 用 Foundation、Transport、Node、Routing、Service 五个源集组合 `ucn_core`；Platform Port 保持独立的 `EXCLUDE_FROM_ALL` 静态目标，未选择 Port 的 Core-only Lite 构建不生成任何 `ucn_port_*` 库。
- 同步：根 README、快速手册源矩阵、Zephyr 示例、S04 Profile 报告、Service 方案、调用树、tests/README、docs/架构 与 UCN 知识库均已更新。调用树非模板源码路径、CMake 源路径和项目 Markdown 相对链接均为 0 缺失；`src` 根目录没有 C/H 业务文件。
- 回归：Windows MSVC Debug Full+配置契约 CTest 4/4、Lite 1/1、Nano 1/1、Full Service OFF 1/1、Full+128 B 产品配置 2/2；WSL GCC Full+配置契约 4/4。MSVC 仅保留既有 `ucn_endpoint.h` / `ucn_security.h` C4819 代码页警告。
- 边界：本轮没有实现或伪造 UART/CAN/Wi-Fi/USB Adapter、RTOS SDK/BSP、UCN-Host/Extended、生产安全或任何实机测试；这些仍由 V5-38～V5-42、S02、S06/S07 等任务负责。
