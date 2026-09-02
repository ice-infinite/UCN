# UCN v5 Q2/Q3 四级调度软件闭环报告

> 状态：`SOFTWARE DONE / HARDWARE PENDING`
> 日期：2026-09-02
> 适用范围：UCN Core Wire v5、Nano/Lite/Full Node、可选 Service、可选 Transfer
> 不包含：ESP32 烧录、真实 UART/CAN/USB/Wi-Fi 优先级映射、目标 MCU 时延/吞吐/功耗结论

## 1. 为什么要补 Q2/Q3

Wire 从早期就能编码 Q0～Q3，但此前 Node 的业务队列与 Service 只真正实现 Q0/Q1。结果是：

- 普通参数、状态与日志没有独立的低优先级容量；
- 大消息 Transfer 虽有 ACK/分片，却只能借用已有 Traffic Class；
- 用户在 API 中看到 Q2/Q3 枚举，却无法得到端到端一致的 Node/Service 行为；
- 将所有流量塞入 Q0/Q1，会混淆“关键顺序”“实时新鲜度”“普通 FIFO”“批量吞吐”四种不同需求。

本次把 Q2/Q3 从线枚举补成软件可执行能力，同时继续遵守 MCU-first：所有队列、Inbox、调度状态和统计均为编译期固定容量，不使用堆内存，不依赖 Linux。

## 2. 完成后的四级合同

| Class | 名称 | 队列语义 | 允许的 Delivery | 典型数据 | 不保证什么 |
| --- | --- | --- | --- | --- | --- |
| Q0 | Critical | FIFO | Best Effort；显式有 Deadline 的 Retry on Backpressure | 急停、舵机命令、关键事件 | 不自动获得端到端 ACK 或远端执行确认 |
| Q1 | Realtime | Latest 或普通单帧发送 | Latest Value、Best Effort | IMU、姿态、温度、链路状态 | Latest 会覆盖尚未发送/消费的旧值 |
| Q2 | Normal | FIFO | Best Effort | 参数、查询结果、普通状态、T32/T64 Direct | 不重试、不持久化、不等于可靠传输 |
| Q3 | Bulk | FIFO | Best Effort | Transfer Fragment、日志、批量数据 | 低优先级不等于可以无界占用 RAM |

Traffic Class 只描述排队和仲裁意图。可靠性、安全、路由策略和业务完成分别由 Transfer、Security、Policy 与 Result Endpoint 负责，不能从 Q2/Q3 名称推导出来。

## 3. 发送路径

### 3.1 立即发送

以下接口现在接受 Q0～Q3：

```c
ucn_node_send(node, destination, message_type, traffic_class,
              payload, payload_length);

ucn_node_send_endpoint(node, destination, endpoint, traffic_class,
                       payload, payload_length);

ucn_node_send_path(node, destination, path_id, message_type,
                   traffic_class, payload, payload_length);
```

立即发送不会先进入 Node 的四级队列；它在唯一 Protocol Owner 上下文中同步完成本地 Route/Path、编码和 Link 提交。因此 Class 会进入 Wire 和后续 Link/Policy 判断，但不会凭空建立一个等待队列。

### 3.2 排队发送

业务任务通过 `ucn_node_enqueue()` 交给 Node 固定存储：

```c
ucn_send_request_t request = {
    .destination = peer,
    .message_type = PRODUCT_EP_PARAMETER,
    .traffic_class = UCN_TRAFFIC_Q2_NORMAL,
    .delivery = UCN_DELIVERY_BEST_EFFORT,
    .payload = bytes,
    .payload_length = length
};

ucn_result_t result = ucn_node_enqueue(node, &request);
```

成功后 Node 拥有 Payload 副本。四个队列互相独立：Q2 满不会覆盖 Q3，Q3 满也不会占用 Q0 的槽。队列满返回 `UCN_ERR_NO_SPACE`。

为了避免语义错配，入队门禁为：

- `LATEST_VALUE` 只允许 Q1；
- `RETRY_ON_BACKPRESSURE` 只允许 Q0，且必须提供非零绝对 Deadline；
- Q2/Q3 使用 FIFO Best Effort；
- 非 Q0～Q3 枚举返回 `UCN_ERR_UNSUPPORTED`；
- 控制消息仍不能通过普通业务 API 伪造。

## 4. 有界 6:3:2:1 调度

Node 与 Service Remote TX 使用同一份逻辑顺序：

```text
Q0, Q1, Q0, Q2, Q0, Q1, Q0, Q3, Q0, Q1, Q0, Q2
```

一个完整周期包含 12 次成功业务选择：

| Class | 每周期机会 | 满载占比 | 满载最长业务间隔 |
| --- | ---: | ---: | ---: |
| Q0 | 6 | 50.00% | 最多隔 1 个其他业务项 |
| Q1 | 3 | 25.00% | 最多隔 3 个业务项 |
| Q2 | 2 | 16.67% | 最多隔 5 个业务项 |
| Q3 | 1 | 8.33% | 最多隔 11 个业务项 |

选择器从当前 cursor 向后寻找第一个非空队列，因此某一级为空时不会浪费发送机会，剩余队列会使用空缺。cursor 只在消息成功、过期或终态释放后前进；临时保留的 Q0 背压/route-wait 项继续拥有 FIFO 头，避免旧关键命令被低级业务越过。

这里的“间隔”是业务调度决策数，不是固定毫秒。实际时间仍取决于 Owner 唤醒、维护帧、Link Queue、物理传输和多跳。

## 5. Service Router/Bridge

Service 增加两个显式模式：

```c
UCN_SERVICE_DELIVERY_Q2_FIFO
UCN_SERVICE_DELIVERY_Q3_FIFO
```

每个 Binding 必须只接受与其 Delivery 对应的单一 Class mask：

- Q0 Binding → Q0 FIFO；
- Q1 Binding → Q1 Latest；
- Q2 Binding → Q2 FIFO；
- Q3 Binding → Q3 FIFO。

Q2/Q3 各有独立的本机 Inbox 和远端 TX FIFO。`ucn_service_set_ready(..., false)` 会清空该 Binding 实际拥有的 Inbox；重新 ready 后不会交付失效前残留的消息。跨 MCU Bridge 取出 Remote TX 时也使用 6:3:2:1，因此 Service 到 Node 的交接顺序与 Node 自身调度方向一致。

Service 的 Q2/Q3 成功仍只说明对应本地层取得所有权：

```text
Service remote queue accepted
  != Node/Link accepted
  != remote Service inboxed
  != remote task executed
```

需要远端执行结果时仍使用业务 Result Endpoint。

## 6. Transfer 映射

本次冻结以下映射：

| Transfer 数据 | Core Traffic Class | 原因 |
| --- | --- | --- |
| T32/T64 Direct | Q2 Normal | 普通单帧业务，不应冒充实时样本或关键命令 |
| T128～T8K Fragment | Q3 Bulk | 有界批量流量，避免挤占 Q0/Q1 |
| Transfer ACK | Q1 Realtime | ACK 新鲜度影响窗口推进，但负载很小 |

接收端不仅检查 Message Type 和结构，还检查 Class：Fragment 必须是 Q3，ACK 必须是 Q1；错 Class 会增加 `rx_rejected` 且不改变重组/发送事务。

Q3 只是 Fragment 的调度级别。当前 `ucn_transfer_step()` 通过立即发送 API 提交一个片，不会先占用 Node Q3 FIFO，因此产品 Owner 仍必须给 Core 与 Transfer 分配有界推进预算；Wire 上的 Q3 标记不能替代 Owner 公平。Transfer 的完整交付仍由整条消息 CRC32、累计 ACK、窗口、重试和 Completion 状态证明。

## 7. 配置与静态资源

### 7.1 Node 默认值

```c
#define UCN_TX_Q0_DEPTH ((size_t)4U)
#define UCN_TX_Q1_DEPTH ((size_t)4U)
#define UCN_TX_Q2_DEPTH ((size_t)2U)
#define UCN_TX_Q3_DEPTH ((size_t)1U)
```

产品可以通过统一配置头覆盖。四个深度都必须大于零。Q2/Q3 默认比 Q0/Q1 小，是为了让小 MCU 获得完整语义但不为低优先级流量预留过多 RAM。

### 7.2 Service 默认值

| 参数 | 默认 |
| --- | ---: |
| `UCN_SERVICE_REMOTE_TX_Q2_DEPTH` | 2 |
| `UCN_SERVICE_REMOTE_TX_Q3_DEPTH` | 1 |
| `UCN_SERVICE_MAX_Q2_BINDINGS` | 1 |
| `UCN_SERVICE_MAX_Q3_BINDINGS` | 1 |
| `UCN_SERVICE_Q2_INBOX_DEPTH` | 2 |
| `UCN_SERVICE_Q3_INBOX_DEPTH` | 1 |

所有深度和 Binding 上限均在编译期验证为非零且不超过总 Binding 容量。未覆盖的值继续使用库 fallback。

### 7.3 Storage ABI

Node Storage Layout 从 6 升到 7，因为 `ucn_node_t` 增加 Q2/Q3 数组与调度 cursor。库和应用必须使用同一配置头并全量重编，不能把旧 Layout 对象与新库混用。

Host x64 Debug 当前绝对值为：

| Profile | `sizeof(ucn_node_t)` |
| --- | ---: |
| Nano | 3480 B |
| Lite | 6872 B |
| Full | 10920 B |

这些数值只用于版本趋势。真实 ESP32/STM32 RAM、对齐、Flash、Task Stack 与 CPU 必须由目标工具链和固件 Map/高水位实测。

## 8. 诊断统计

Node 新增四级数组：

```c
tx_enqueued_by_class[4]
tx_scheduled_by_class[4]
tx_sent_by_class[4]
```

三者分别回答“Node 接收了多少”“调度器选中了多少”“Link 成功接受了多少”。它们之间的差值可帮助区分过期、背压、路由等待和终态错误。

Service 增加：

- `q2_inbox_full`、`q3_inbox_full`；
- `remote_q2_full`、`remote_q3_full`；
- `remote_tx_reads_by_class[4]`。

统计使用固定宽度计数，不引入历史日志或动态对象。产品长稳测试应处理计数回绕或周期快照。

## 9. 软件测试证据

### 9.1 定向行为

- Q2/Q3 立即发送、入队、Endpoint 和 Path 交付；
- Q2/Q3 Node 队列选择和非法 Class 拒绝；
- Service Q2/Q3 FIFO 顺序、满载、purge 和三节点 Bridge；
- Transfer Direct/Fragment/ACK 精确 Class 映射与错 Class 拒绝；
- Node 与 Service 各自持续 12,000 次四队列满载，精确得到 `6000/3000/2000/1000`；
- Full/Lite/Nano 都实际运行同一 QoS 测试，不只编译符号。

### 9.2 构建矩阵

| 环境 | 结果 |
| --- | --- |
| Windows GCC Full Debug | 50/50 CTest |
| Windows GCC Lite Debug | 50/50 CTest |
| Windows GCC Nano Debug | 40/40 CTest |
| Windows GCC Full Release | 50/50 CTest |
| Windows GCC Full / Service OFF | 50/50 CTest |
| WSL GCC ASan + UBSan | 50/50 CTest |
| WSL GCC `-fanalyzer -Wall -Wextra -Werror` | 50/50 CTest |

软件矩阵证明当前 C 实现、裁剪组合、优化构建与已定义的内存安全门禁通过；它不提供物理链路时延、带宽、公平、ISR 抖动或无线拥塞证据。

## 10. 待 ESP32/目标板测试

开发板接入后按 QOS4-08 执行：

1. 使用同一固件和可追溯产品配置，打印 Node ID、Profile、队列深度和固件 Hash；
2. 单 Bearer 分别测试 UART、CAN/CAN-FD、USB、Wi-Fi；
3. 每级单独满载，记录 Payload/s、P50/P95/P99、队列峰值、拒绝和 CPU；
4. 四级同时持续满载，验证 Q0 延迟上限和 Q2/Q3 不饥饿；
5. 多跳与多 Bearer 下重复相同矩阵，记录每增加一跳的序列化损失；
6. 注入 Link Down、队列满、Owner 延迟、路由重建与 MTU 变化；
7. 核对硬件能力：Classic CAN 可按 CAN ID 仲裁，部分 Wi-Fi/USB/UART 只有软件队列，不能伪造不存在的硬件优先级；
8. 使用目标 ELF/Map、Task Stack high-water、Heap minimum、CPU 和功耗形成正式实机报告。

在这些测试完成前，允许写“Q2/Q3 软件队列与调度完成”，不允许写“ESP32 四级实时性/吞吐已验证”或“所有 Bearer 均提供硬件优先级”。

## 11. 最终边界

本阶段没有修改 Core Wire 字段或版本；Q0～Q3 本来就在 Wire 中，变化位于 Node/Service 的本地存储、调度、Transfer 映射和统计。Linux 不是依赖，Nano/Lite/Full 都可运行。没有增加动态内存，没有把 Q3 做成无限文件队列，也没有把 Traffic Class 与可靠性混为一体。

软件阶段完成条件已经满足；产品阶段仍由 QOS4-08 持续跟踪。
