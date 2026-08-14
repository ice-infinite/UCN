# UCN V5-58 标准事件 Runtime 与多 Bearer 接入方案

> 日期：2026-08-14
> 状态：公共 Runtime 与 Host 软件门禁已完成；真实 Carrier/SDK/实机继续 V5-59～61。
> 目标：把 ESP32 三板已验证的“外设事件唤醒唯一 Owner”收回 UCN 主库，形成裸机、FreeRTOS、Zephyr、NuttX、RT-Thread 与任意 Bearer 共用的固定资源接口。
> 当前迁移说明：V5-62 已在预发布阶段把底层 `ucn_port_ops_t` 破坏性升级为 Port API V2。Runtime 调度模型不变，但所有产品必须增加 `struct_size/api_version` 并全量重编译；详见 [V5-62 修复报告](UCN_V5_62_Port_API_V2与审计缺陷修复报告.md)。

## 1. 当前缺口

主库已有 `ucn_protocol_owner_t`、完整帧 RX Queue、Task/ISR 两种入队接口，以及各 RTOS 的通知/等待外壳；但仍缺少统一的多 Bearer 事件源注册、Carrier Ring Drain、事件合并、预算耗尽续跑和轮询兜底。ESP32 `WorkNotifier` 仍属于外部测试工程，不能作为主库标准接口。

V5-58 只增加 SDK 无关 Runtime；不把 FreeRTOS/Zephyr 类型、UART Handle、CAN ID、USB Endpoint 或 Wi-Fi MAC 放入 Core，也不修改 v5 Wire。

## 2. 标准运行链

```text
UART/CAN/USB/Wi-Fi ISR 或驱动回调
  -> 各 Bearer 自己的固定 Driver Ring / DMA 描述符
  -> ucn_event_runtime_signal_source[_from_isr]()
  -> 一个事件唤醒唯一 UCN Owner
  -> 按 Source ID 和固定预算调用 service()
  -> Carrier 解帧/重组后提交完整 UCN Frame
  -> Protocol Owner Pump / Node / Routing / Service / 可选 Extended
```

多个中断可以同时设置不同 Source 的 Pending Flag；事件通知允许合并，因为真正数据仍保留在各自 Ring 中。ISR 只搬运数据、更新固定索引并通知，不运行 Frame Decode、路由、解密、Transfer、Endpoint 或同步日志。

## 3. 固定 Source 契约

每个 UART、CAN 控制器、USB Endpoint、ESP-NOW/UDP Adapter 是一个静态 Source。Source 只暴露一个 SDK 无关 `service()`：

- 输入：本轮 RX/TX/STATUS/FALLBACK 事件和最大工作预算；
- 输出：实际工作量和仍待处理的事件；
- UART/RS-485/USB CDC 在 Owner 上下文从 Byte Ring 解 Carrier；
- CAN/CAN-FD 从 Frame Ring 执行有界 Carrier 重组；
- ESP-NOW/UDP 若回调已获得完整 UCN Frame，可直接使用标准 Task/ISR Frame Submit；
- Source 不选择路由，不直接调用业务任务。

Runtime 默认最多 8 个 Source，产品可通过全局配置收窄；不动态分配、不运行时扩容。

## 4. Runtime 调度契约

- 每轮先原子取得全部 Source/Owner Pending Flag，再按固定 Source 顺序 Drain；
- 每个 Source 每轮有固定 Work Budget；整个 Runtime 有固定最大 Drain Round；
- Source 返回 `pending_events` 时继续下一 Round；新中断并发到达只做 OR，不覆盖旧事件；
- Source 完成后运行一次公共 Protocol Owner；只要 RX/Bridge/Node 仍有工作就在预算内继续；
- 达到 Round 上限返回 `work_remaining=true`，平台先 Yield 再继续，不能睡眠；
- 正常通知立即解除等待；等待时间裁剪到 `UCN_MAX_STEP_INTERVAL_MS`；
- 等待超时执行一次 FALLBACK Scan，用于漏通知、无中断驱动和协议维护，不把它作为正常 RX 路径。

## 5. 平台映射

| 平台 | `notify_owner` | `wait_owner` | Owner 形态 |
| --- | --- | --- | --- |
| FreeRTOS | Task Notification Give/FromISR | `ulTaskNotifyTake` | 单 Protocol Task |
| Zephyr | `k_sem_give`/`k_event_post` | 有界 `k_sem_take`/Event Wait | 单 Thread/Work |
| NuttX | Semaphore/Event + Pending | 有界 Wait/Poll | 单 Worker |
| RT-Thread | Event/Semaphore Send | Event/Semaphore Receive | 单 Thread |
| 裸机有中断 | 只设置 Runtime Pending；可选 SEV | 主循环检查，可选 WFI | 普通函数 |
| 无中断平台 | 无通知 Hook | Super Loop 定时调用 FALLBACK | 普通函数 |

V5-58 当时保留了各平台 Port 入口；当前这些入口和 Event Runtime 都统一要求 Port API V2。单 Queue Port 与多 Source Event Runtime 仍是两种可选 Owner 形态，但旧编译对象和旧位置初始化必须迁移，不能与当前头文件混用。

## 6. 验收

软件必须覆盖：两 Source 同时通知、Task/ISR 分流、事件合并、并发新事件不丢、直接完整帧 Submit、Source Budget、Round Budget、Yield、等待裁剪、超时 FALLBACK、裸机无 Hook、Queue 满、Source 错误和固定容量。Full/Lite/Nano、Service OFF、低资源产品头、ASan/UBSan 与 `-fanalyzer` 全部通过后，才标记 V5-58 软件完成。

真实 UART/CAN/USB Driver Ring 和目标 RTOS 调度属于后续 V5-59～V5-61；V5-58 不用 Host Fake 冒充所有实机已经验证。

## 7. 已实现接口与证据

公共头为 `include/ucn/ports/ucn_event_runtime.h`，实现为 `src/transport/ucn_event_runtime.c`，并作为所有 Profile 的 transport 源进入 `ucn_core`。已实现：

- 固定 Source ID 绑定，默认 8、允许产品全局收窄；
- Task/ISR Source 与 Owner 事件置位，ISR 缺 token 临界区时失败关闭；
- 事件按位合并、并发新事件不覆盖、Source/Round 固定预算；
- Source `service()` Owner-only 回调和 `pending_events` 续跑；
- 完整帧 Task/ISR Submit、公共 RX Queue 背压统计；
- RX/Bridge/Node 预算饱和重新置位，不让剩余工作等待定时器；
- 有 Pending 立即 Run，否则有界 Wait；超时执行全部 Source FALLBACK；
- 裸机无 Scheduler Hook 路径、运行统计和 Owner 统计读取。

`tests/test_event_runtime.c` 覆盖两 Source 同时通知、同 Source 合并、Source 回调中并发投递另一 Source、Task/ISR token、Queue 按配置深度填满、Source/Round Budget、Yield、等待裁剪、超时 FALLBACK、裸机、非法 Source/事件、Source 错误和畸形返回。配置默认/回退/产品覆盖及公共头链接测试同步完成。

2026-08-14 软件结果：Windows Full/Lite/Nano 各 1/1、128 B 产品配置 5/5、Full Service OFF 1/1；WSL ASan+UBSan 1/1、GCC `-fanalyzer` 1/1。Host x64 `sizeof(ucn_event_runtime_t)`：默认 8 Source 时 Full/Lite 432 B、Nano 408 B；3 Source 产品配置 304 B。这些数值不包含 Node、公共 Adapter RX Queue 或各 Bearer Driver Ring，目标 MCU 仍须用目标 ABI 实测。

V5-58 没有修改 v5 Wire、Frame Header、路由或消息编号。下一步 V5-59 才实现公共 UART/RS-485/USB Stream Byte Ring/Carrier Source；V5-60 实现 CAN/CAN-FD Frame Ring/Carrier；V5-61 才能在实际 RTOS/多中断硬件上关闭 ISR、栈、CPU、功耗和长稳门禁。
