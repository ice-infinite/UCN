# UCN V5-37 标准 Port 外壳实施方案

> 当前迁移说明：V5-46 已把集中式外壳拆为独立平台 Port；V5-62 又将底层 `ucn_port_ops_t` 破坏性升级为带大小/版本的 Port API V2。本文保留 V5-37 的 Owner 调度历史，当前初始化以 [V5-62 修复报告](UCN_V5_62_Port_API_V2与审计缺陷修复报告.md) 为准。

> 文档编号：DOC-042。
> 状态：历史阶段记录；集中式设计已由 DOC-044 / V5-46 分目录 Port 架构替代。
> 分支：`codex/v5-adaptive-wire`；不接入任何 MCU SDK、RTOS SDK 或物理 Bearer 驱动。

## 1. 目标

V5-37 曾在既有 ucn_port_ops_t、ucn_adapter_rx_queue_t 和可选 Service Bridge 之上，以一个集中头/源实现纯 C99 的 Protocol Owner 外壳。该运行顺序仍有效，但“多平台由一个 mode 枚举承载”的公开结构已被 V5-46 删除。当前实现请阅读 [V5-46 平台 Port 解耦重构报告](UCN_V5_46_平台Port解耦重构报告.md)。

```text
驱动 ISR / 回调
  -> Adapter 完成完整 UCN 帧
  -> Owner RX 入固定队列 + 可选唤醒通知
  -> 唯一 Protocol Owner: RX Pump -> 可选 Bridge step_at -> node_step
```

它只负责运行时归属、固定预算和时钟一致性；UART/CAN/Wi-Fi/USB 的收发、Carrier、GPIO、线程创建、Devicetree/Kconfig、FreeRTOS Queue、Zephyr `k_msgq`、NuttX `poll()` 都仍由产品或后续 Adapter 任务提供。

## 2. 不可破坏的边界

- 每个 `ucn_node_t` 只有一个 Protocol Owner；ISR、业务 Task 和其他线程不得直接调用 `ucn_node_receive()`、`ucn_node_step()` 或 Service Bridge `step_at()`。
- ISR/回调仅可调用 Owner 的入队 API：复制到既有 Adapter 固定 RX 队列后立即返回；满队列返回 `UCN_ERR_NO_SPACE` 并计数，绝不 Pump 或进入 Core。
- 每次 Owner Step 只读取一次单调 `now_ms`，同一时间值传给 Bridge `step_at()` 和 Node `step()`。
- RX 与 Bridge 都有固定单轮预算；没有动态分配、无限循环、内置线程、睡眠或 SDK 类型。
- RTOS 等待最多到 `UCN_MAX_STEP_INTERVAL_MS`；即使无 RX/业务，Owner 也必须按该上界执行 `ucn_node_step()`。
- Service 是正交开关：Service ON 时可持有 Bridge，Service OFF 时头文件和源仍能编译，且不产生未解析符号。

## 3. 公开对象与调用方式

新增 `include/ucn/ucn_standard_port.h` 和 `src/ucn_standard_port.c`：

| 对象/API | 职责 |
| --- | --- |
| `ucn_standard_port_mode_t` | 标记 Bare metal、FreeRTOS、Zephyr、NuttX、Host Fake 五种运行模型；不引入其 SDK。 |
| `ucn_standard_port_owner_config_t` | 静态绑定 Node、Adapter RX Queue、`ucn_port_ops_t.now_ms`、预算、可选通知/等待回调和可选 Service Bridge。 |
| `ucn_standard_port_owner_init()` | 校验对象、预算、模式、时钟和 Bridge→Node 一致性；不创建线程、不注册 Link。 |
| `ucn_standard_port_owner_rx_enqueue()` | 供驱动回调/ISR 提交完整帧并按平台回调通知 Owner。 |
| `ucn_standard_port_owner_step()` | Owner 上下文唯一调用：Pump RX → Bridge `step_at`（若启用）→ Node `step`。 |
| `ucn_standard_port_owner_wait()` | 可选的 RTOS 适配点；将外部请求等待时间裁剪到 `UCN_MAX_STEP_INTERVAL_MS`。 |

`notify_protocol_owner(context, from_isr)` 的实现属于产品：`from_isr=true` 时必须映射为该 RTOS/裸机可安全调用且不阻塞的通知机制。`wait_for_work(context, max_wait_ms)` 只允许由 Owner 调用。裸机 Super Loop 可以不配置两者，直接周期性调用 `step()`。

## 4. 四个平台映射

| 模式 | ISR/驱动侧 | Owner 侧 | 产品仍负责 |
| --- | --- | --- | --- |
| Bare metal | DMA/轮询收齐帧后入队；可置 PendSV/标志。 | Super Loop 调用 `step()`；不调用等待回调。 | 时基、临界区、中断保存/恢复、DMA。 |
| FreeRTOS | 入队后用 ISR 安全 Task Notification 唤醒静态 Protocol Task。 | 静态 Task 限时等待，再调用 `step()`。 | Task 栈/优先级、通知封装、驱动句柄。 |
| Zephyr | 入队后用 ISR 安全 `k_sem`/work 通知。 | 专用 Thread 受限等待，再调用 `step()`。 | Devicetree、线程优先级、设备状态。 |
| NuttX | 入队后用信号量/工作队列通知。 | pthread 或 Work Queue 受限等待，再调用 `step()`。 | `/dev/*`、调度策略、文件描述符轮询。 |
| Host Fake | 测试回调累加通知与等待参数。 | 单线程显式 `step()`。 | 仅用于单元/模拟，不外推成硬件线程行为。 |

## 5. 测试任务与验收

1. Host Fake HAL：测试四种模式均能初始化；验证通知只发生在入队后，处理回调只发生在 Owner Step。
2. 预算与时钟：多帧入队时每轮仅 Pump 配置数量；验证 Node 与可选 Bridge 使用同一个采样时间。
3. 失败关闭：空 Node/Queue/时钟、零预算、非法模式、Bridge Node 不一致、Owner 未初始化、过大帧、队列满均返回明确错误且不触碰 Node。
4. 等待边界：验证请求 0、短等待和超长等待，超长值被裁剪到 `UCN_MAX_STEP_INTERVAL_MS`；裸机无等待回调保持无副作用。
5. 矩阵：Full、Lite、Nano、Service OFF 和代表性 128 B 产品配置构建/CTest；WSL GCC 回归。所有结果写入 DOC-043 完成报告和操作记录。

## 6. 明确不做

- 不创建任何真实 FreeRTOS/Zephyr/NuttX Task、Queue、Semaphore、Kconfig、Devicetree 或 NuttX 线程。
- 不实现 UART/RS-485 Carrier、ESP-NOW/Wi-Fi、CAN-FD、USB CDC；这些由 V5-38～V5-40 在真实 SDK/板级工程中完成。
- 不增加 Node 常驻表、动态内存、Wire 字段、路由策略或 Link Cost 动态评分。
- 不以 Host Fake HAL 或 CTest 结果宣称目标 MCU 栈、CPU、实时性、功耗或实机多板通信已经验证。

## 7. 实现结果

- 新增公共 C99 API：`ucn_standard_port.h/.c`。Owner 只引用产品静态持有的 Node、Adapter RX Queue、Port 时钟/临界区、可选 Runtime Hook 与可选 Service Bridge；不创建 SDK 对象或动态内存。
- `ucn_standard_port_owner_rx_enqueue()` 是唯一的驱动/ISR 入口：成功复制完整帧后才通知 Owner；它从不进入 Node、Bridge 或业务回调。
- `ucn_standard_port_owner_step()` 以一次 `now_ms` 依次执行有界 RX Pump、可选 Bridge `step_at()`、Node `step()`；Core 的空闲 `UCN_ERR_NOT_FOUND` 只保留在 `last_node_step_result`，对 Owner 循环归一为 `UCN_OK`。
- Host Fake 覆盖模式/初始化、ISR 通知、单轮 RX 预算、等待上限、过大帧拒绝、可选 Bridge 出队和统一时钟；公共头测试直接引用 Owner API，防止裁剪 Profile 缺符号。
- 完整证据见 [V5-37 标准 Port 外壳实现报告](UCN_V5_37_标准Port外壳实现报告.md)。真实 RTOS 线程、BSP、驱动和目标板 WCET/栈/功耗仍不属于本任务。
