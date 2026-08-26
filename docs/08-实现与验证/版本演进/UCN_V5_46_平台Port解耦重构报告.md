# UCN V5-46 平台 Port 解耦重构报告

> 文档编号：DOC-044。
> 状态：已完成（软件验证）。
> 目的：替代 V5-37 集中式运行模型枚举；不接入 MCU SDK、物理驱动或实板。

## 1. 为什么要重构

V5-37 用一个 mode 枚举承载裸机、FreeRTOS、Zephyr、NuttX 和 Host Fake。虽然没有包含 SDK，但公共头会随着平台数量不断膨胀，新增 RT-Thread 也必须修改所有用户共享的集中入口。这不符合 Port 应独立演进的边界。

V5-46 改为“公共 Protocol Owner 契约 + 独立平台 Port”：

    产品 Adapter / ISR
      -> 选定平台的 ucn_<platform>_port_rx_enqueue()
      -> 公共 ucn_protocol_owner_rx_enqueue()
      -> Adapter 固定 RX Queue

    选定平台的主循环 / Task / Thread
      -> ucn_<platform>_port_*_step()
      -> 公共 ucn_protocol_owner_step()
      -> RX Pump -> 可选 Service Bridge -> Node Step

公共 Owner 不再有运行平台枚举、通知回调或等待回调；它只定义唯一 Node Owner、固定 RX 队列、预算、统一时钟和 Bridge 顺序。通知、等待和平台命名留在各自 Port 文件。

## 2. 目录与职责

    include/ucn/ports/
    ├── ucn_protocol_owner.h        公共、SDK 无关的运行顺序和统计
    ├── ucn_port_runtime.h          独立 Port 共用的运行统计结构
    ├── ucn_port_bare_metal.h       Super Loop / Timer Owner
    ├── ucn_port_freertos.h         FreeRTOS Task 对接点
    ├── ucn_port_zephyr.h           Zephyr Thread 对接点
    ├── ucn_port_nuttx.h            NuttX Worker 对接点
    ├── ucn_port_rtthread.h         RT-Thread Thread 对接点
    └── ucn_port_host_fake.h        Host 单元/模拟 Port

    src/
    ├── ucn_protocol_owner.c
    └── ports/
        ├── ucn_port_bare_metal.c
        ├── ucn_port_freertos.c
        ├── ucn_port_zephyr.c
        ├── ucn_port_nuttx.c
        ├── ucn_port_rtthread.c
        └── ucn_port_host_fake.c

| 层 | 可以知道 | 不可以知道 |
| --- | --- | --- |
| 公共 Owner | Node、Adapter RX Queue、时钟、RX/Bridge 预算。 | RTOS 类型、任务句柄、等待原语、GPIO、SDK、驱动。 |
| 单个平台 Port | 自己的 notify/wait 回调、自己的 API 命名、自己的运行统计。 | 其它 RTOS 的类型、枚举或头文件。 |
| 产品 BSP/Adapter | 真正的 RTOS 通知、DMA/IRQ、引脚、外设句柄。 | Core 内部路由表和多个平台 Port 的实现细节。 |

## 3. 选择一个 Port 后如何调用

裸机只包含 ucn/ports/ucn_port_bare_metal.h，在 Super Loop 调用 init、rx_enqueue 和 poll。

FreeRTOS 只包含 ucn/ports/ucn_port_freertos.h。产品将 notify_protocol_task(context, from_isr) 映射到自己静态创建的 Task Notification/静态队列，将 wait_for_work(context, max_wait_ms) 映射到受限等待；Task 循环调用 task_wait 与 task_step。

Zephyr、NuttX、RT-Thread 分别只包含对应头文件，并使用自己的 thread 或 worker API。行为完全一致：成功入队后才通知；ISR 不 Pump；等待不超过 UCN_MAX_STEP_INTERVAL_MS；只有一个执行上下文 Step。

Host Fake 仅用于纯 C99 测试，不能成为产品 Port。现阶段这些 Port 都是 SDK 无关的回调边界：UCN 不替产品创建 Task、Thread、Semaphore 或 Queue；真实 SDK 封装仍属于后续目标工程/Adapter 任务。

CMake 中公共 Core 是 `ucn_core`，每个平台又是独立的 EXCLUDE_FROM_ALL 静态库目标：`ucn_port_bare_metal`、`ucn_port_freertos`、`ucn_port_zephyr`、`ucn_port_nuttx`、`ucn_port_rtthread`、`ucn_port_host_fake`。产品只链接自己的一个 Port；未选择的 Port 不会成为产品链接依赖。

## 4. 新增一种 RTOS 的固定步骤

例如后续接入 MyRTOS，只新增以下独立文件：

1. include/ucn/ports/ucn_port_myrtos.h：定义 MyRTOS 的 Ops、Config、Port 和公开 API。
2. src/ports/ucn_port_myrtos.c：只调用公共 Owner 的 init、rx_enqueue、step，并处理 MyRTOS 的通知/等待回调。
3. tests/test_protocol_owner.c：增加该 Port 的 Host Fake/API 链接案例。
4. CMakeLists.txt：添加这一对源文件；更新使用手册、调用树、任务表和操作记录。

不得向公共 Owner 增加 MyRTOS 枚举、条件编译或 SDK 类型；不得修改 FreeRTOS、Zephyr、NuttX、RT-Thread Port 的头或源，除非公共行为本身改变。

## 5. V5-37 的处理

集中式 ucn_standard_port.h、ucn_standard_port.c 和 test_standard_port.c 已删除，因为它们尚未作为稳定发布 API。V5-37 的 DOC-042/043 作为历史设计证据保留，但当前实现以本报告、ucn_protocol_owner.* 和 ucn/ports/ 为准。产品应只引用选定平台的独立头文件。

## 6. 测试覆盖与边界

V5-48 后，`tests/test_protocol_owner.c` 还覆盖公共 Owner 的 Task/ISR 入队分流，以及 FreeRTOS、Zephyr、NuttX、RT-Thread、Host Fake 五个 `from_isr=true` Port 均实际进入可观察的 ISR token 锁路径；`tests/test_adapter.c` 覆盖缺失 ISR 锁时失败关闭。真实 SDK/BSP ISR 并发仍不由 Host Fake 推定。

| 配置 | 实际结果 |
| --- | --- |
| Windows MSVC Debug Full + 配置契约 | CTest 4/4 通过。 |
| Windows MSVC Debug Lite | CTest 1/1 通过。 |
| Windows MSVC Debug Nano | CTest 1/1 通过。 |
| Windows MSVC Debug Full，Service OFF | CTest 1/1 通过。 |
| Windows MSVC Debug Full + 128 B 产品配置 | CTest 2/2 通过。 |
| WSL GCC 严格告警 Full + 配置契约 | CTest 4/4 通过。 |
| Windows MSVC Lite，Tests/Scale 关闭 | 默认构建只生成 ucn_core；未选择的 Platform Port 不参与 Core 构建。 |

V5-48 的最新全量 Host 证据见 [ISR 队列与容量合同修复报告](UCN_V5_48_ISR队列与容量合同修复报告.md)：Full+Scale `14/14`、128 B/3-Link `15/15`、严格 WSL GCC+Scale `14/14`。本表保留 V5-46 阶段的历史计数，不以旧计数覆盖新报告。

MSVC 仍显示既有 ucn_endpoint.h、ucn_security.h 的 C4819 本地代码页警告；本次未新增该问题。以上只证明 C99 接口、裁剪与 Host 行为；不证明真实 ISR 并发、RTOS 调度、任务栈、驱动、无线吞吐、功耗或实板可靠性。

## 7. 后续

下一项仍是 V5-38：在 ESP32 工程实际选择 FreeRTOS Port，再把 UART/ESP-NOW Adapter 对接到对应 RX 入队入口。届时只改 FreeRTOS Port glue 与 ESP32 Adapter，不影响 Zephyr、NuttX、RT-Thread 和裸机文件。
