# UCN NuttX 快速使用

> `ARCHIVED / NOT CURRENT`：本文仅保留 v5 NuttX 接入记录；v6 请从[当前用户手册](../../用户手册/README.md)开始。

> 适用：NuttX 应用或 PX4 风格的 NuttX 板级产品。当前仓库已有独立、SDK 无关的 `ucn_port_nuttx` C99 外壳；产品仍需映射真实 NuttX Task/同步/驱动。本页不替代你的 NuttX 版本、BSP 或驱动配置。

> 当前 API：V5-62 Port API V2 要求所有 `ucn_port_ops_t` 具名填写 `struct_size/api_version`；旧位置初始化与旧对象不兼容。Transfer 还必须配置权威时钟并使用无时间参数的 `ucn_transfer_step()`。迁移见[总览](README.md)。

## 1. 放置方式

将 UCN 当作产品 App 的私有静态库或源码组：按照[总览的 Profile 源文件矩阵](README.md#共同的构建输入)选择 Nano/Lite/Full 与可选 Service 源文件，加入 `include/ucn/`，再依照当前 NuttX 树的 App 构建规范注册。所有 UCN 与产品编译单元必须共享相同的 `UCN_PROFILE`、`UCN_FEATURE_SERVICE` 和配置头。不同 NuttX 版本和项目可能使用 `Make.defs`、`CMakeLists.txt` 或已有统一 App 规则；应跟随目标树中已有 App 的方式，而不是从其他版本复制构建文件。

建议在产品 Kconfig 定义以下**产品**选项，然后转成 UCN 编译宏：

```text
CONFIG_PRODUCT_UCN=y
CONFIG_PRODUCT_UCN_PROTOCOL_STACKSIZE=2048
CONFIG_PRODUCT_UCN_MAX_FRAME_BYTES=256
CONFIG_PRODUCT_UCN_RX_DEPTH=2
CONFIG_PRODUCT_UCN_SERVICE_Q0_DEPTH=4
```

它们是起始值，不是对所有 NuttX 板子的内存承诺。关闭不需要的 UCN 容量，测量 `.bss/.data`、任务栈余量和驱动 DMA 缓冲后再提升上限。Core 自身没有 `malloc` 依赖；若选择 pthread 创建任务，需要确认该 NuttX 配置不会让协议任务栈变成不可控的运行时堆分配。

## 2. 选择唯一的 Protocol 执行上下文

无论选择 pthread 还是 Work Queue，都在其启动前完成 `ucn_node_init()` → `ucn_node_set_wire_profiles()` → 可选 `ucn_node_set_wire_profile_auto(true)` → 明文 Boot Session 或生产 Security → Link 注册。不要从 shell 命令或第二线程运行时改 Wire 域；需要改变产品域时应安全停网并重建 Node。

二选一，不要混用：

1. **专用 Protocol pthread**：适合有稳定线程资源的板子。由该线程独占 `ucn_node_t`，并周期执行 `ucn_node_step()`。
2. **既有单一 Work Queue**：适合极小系统。所有 UCN RX、Bridge 和定时 Step 都只排入同一个 UCN Work 项；不能让多个 Work Queue 同时碰 Node。

以下是 pthread 逻辑骨架。产品负责按 NuttX 版本设置任务优先级、静态/可测量栈和启动入口：

```c
#include "ucn/ucn_node_storage.h"

static ucn_node_t g_node;

static void *ucn_protocol_main(void *argument)
{
    (void)argument;
    while (!g_stop_requested) {
        size_t pumped = 0U;
        uint8_t bridged = 0U;
        const uint32_t now_ms = product_monotonic_ms();

        nuttx_adapter_drain_rx_frames();
        (void)ucn_adapter_rx_pump(&g_rx_queue, &g_node, 4U, &pumped);
        (void)ucn_service_protocol_bridge_step_at(&g_bridge, now_ms,
                                                   2U, &bridged);
        (void)ucn_node_step(&g_node, now_ms);
        product_wait_rx_or_periodic_tick();
    }
    return NULL;
}
```

等待必须有有限超时；若只被 UART RX 唤醒，Heartbeat、邻居超时、Route 刷新和诊断超时都不会按期运行。

## 3. 驱动到 Adapter 的路径

NuttX 字符设备、CAN 或无线驱动的接收回调/ISR只做以下事情：向固定 DMA ring 或 BSP 的有界 RX 缓冲提交原始数据，并唤醒协议线程。载体解码（例如 UART COBS）和 `ucn_adapter_rx_enqueue()` 都在 Protocol Thread 中执行：

```c
static void nuttx_adapter_drain_rx_frames(void)
{
    uint8_t frame[UCN_MAX_FRAME_BYTES];
    size_t length;

    while (product_take_complete_ucn_frame(frame, sizeof(frame), &length)) {
        const ucn_result_t rc =
            ucn_adapter_rx_enqueue(&g_rx_queue, &g_link, frame, length);
        if (rc != UCN_OK) {
            product_record_rx_drop(rc);
        }
    }
}
```

因为这个 Queue 只由 Protocol Thread 访问，可用 `ucn_adapter_rx_queue_init(&g_rx_queue, NULL, NULL)`。若其他任务提交，必须提供任务临界区；若 ISR 直接提交完整帧，必须提供返回/恢复 token 的 ISR 临界区并使用 `ucn_adapter_rx_enqueue_from_isr()`。不能把可能睡眠的线程锁用于 ISR。

## 4. NuttX 业务任务和 Service Router

多个 NuttX pthread 不直接访问 Node。产品 Port 建立：

- 一个只覆盖 Router 复制/取出的短 mutex；
- 每个固定 Service 的通知 semaphore/轻量事件；
- Service ID 到 pthread ID 的静态绑定表；
- `ucn_service_protocol_bridge_set_inbound_hooks()` 的 lock/unlock/observer。
- 每个要求远端 Q0 Validator 的 Binding 对应一个启动期 Validator 注册；必须早于 Endpoint handler 安装。

业务 pthread 在短锁中调用 `ucn_service_send()` 和 `ucn_service_inbox_take()`；observer 在锁外 `sem_post()` 或等价通知消费者。通知只表示“可能有数据”，消费者必须把自己的 Inbox 读到 `UCN_ERR_NOT_FOUND`。不要用一个全局无界消息队列替换 Router 的 Q0 FIFO/Q1 Latest Value 语义。

对飞控/执行器类 Q0：业务 Task 需要本地 `valid_for_ms` 或 watchdog；`UCN_OK` 只能说明本机 Router/Core 已接收，不能成为输出维持条件。

## 5. Link、身份与安全

- 把 NuttX 设备文件、SocketCAN 或 UART 初始化封装在产品 Link 的 `open/send/poll_rx/get_status/get_metrics/close` 中；Node 注册 Link 时会调用 `open()`，不要预先重复打开。
- `get_status().mtu` 报告当前运行期完整 UCN 帧上限；与静态 `link.mtu` 同时存在时取较小值，两者都为 0 时暂不可发送，运行期恢复后继续使用原注册 Link。
- `network_id` 在同一网内相同，`node_id` 由板级受控配置或持久存储给出，网络内不得重复。不要把 `/dev/ttyS*` 或 CAN ID 当 Node ID。
- NuttX 不会自动提供 UCN 生产安全：若 Endpoint 要求 E2E 保护，产品必须实现并安装 `ucn_security_ops_t`、密钥与单调计数器持久化。

## 6. 验收清单

1. 先用一个轮询 UART Link 运行两 Node Q1 Endpoint；检查所有结构体为静态/固定上限。
2. 验证驱动 RX ring、Adapter Queue、Router Inbox 满时的丢弃/覆盖统计。
3. 加入 pthread Service 后验证线程所有权、远端 Bridge 与中继不投递业务。
4. 量测每个线程的栈、CPU、FD/驱动错误和断链 Q0 本地安全；再接多 Bearer、策略 Path 与实机多跳。

## 7. S16 Protocol Thread 时限

在板级配置中冻结 `UCN_MAX_STEP_INTERVAL_MS`、pthread 优先级/调度策略、最大 poll/sem wait、RX Pump/Bridge 预算和每个设备 `send()` WCET。Protocol Thread 必须有周期超时，不能永久阻塞等待 FD。运行时导出 `max_step_gap_ms` 与 `step_interval_violations`；当前文档是接入模板，不代表已有 NuttX 板级时延证据。

## 8. V5-58/59 NuttX Event Runtime 与 Stream 映射

产品用 ISR-safe semaphore/event/poll wake-up 实现 `notify_owner()`，Protocol Worker 以有限超时实现 `wait_owner()`；Round 上限后只在任务上下文 Yield。`/dev/ttyS*`、CAN、USB 或无线 FD/驱动各对应一个固定 Source，不把 FD 或驱动 Handle写入 Core。若数据先进入 NuttX 驱动缓冲，Source `service()` 在 Worker 内做有界读取；当前仍需 NuttX 目标板验证 ISR 约束和调度时延。

Worker 从 `/dev/ttyS*`/USB CDC 有界读取的字节交给 `ucn_stream_source_write()`；真 ISR 则使用 `write_from_isr()`。公共 Source 只保存调用者数组和 C 状态，不保存 FD/pollfd，也不替代 NuttX 驱动流控。
