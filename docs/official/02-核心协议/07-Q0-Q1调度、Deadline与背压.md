# Q0～Q3 调度、Deadline 与背压

> 文档级别：`NORMATIVE`
> 实现状态：`CURRENT`
> 事实源：`include/ucn/ucn_types.h`、`include/ucn/ucn_node.h`、`include/ucn/ucn_service.h`、Node/Service/QoS tests
> 最近核对：当前工作区，2026-08-31

## 1. 为什么不能把所有数据放进一个 FIFO

MCU 网络里，舵机急停命令、1 kHz IMU 样本和日志块的价值完全不同。如果只使用一个 FIFO，大日志可以让急停在队尾等待；如果所有消息都设成最高优先级，又会让路由维护和心跳没有执行机会。

UCN 当前实现四类独立业务队列：

| Class | 业务意图 | 典型数据 | 当前实现边界 |
| --- | --- | --- | --- |
| Q0 Critical | 控制与关键事件，顺序和明确失败重要 | 舵机命令、模式切换、紧急状态 | 固定 FIFO，可选择有界本地背压重试 |
| Q1 Realtime | 新鲜度优先的连续状态 | IMU、温度、姿态、链路状态 | 固定队列；Service 支持 Latest；Full 支持 Flow 粘滞/均衡 |
| Q2 Normal | 普通 FIFO | 参数、查询结果、一般消息、T32/T64 Direct | Best Effort；不覆盖、不自动重试 |
| Q3 Bulk | 批量 FIFO | Transfer Fragment、日志、低优先级块 | Best Effort；固定小队列、不得挤占其他级别 |

Traffic Class 只决定本地排队和仲裁意图。它不自动增加 ACK、重传、持久化、安全或远端执行确认；这些分别属于 Transfer、产品存储、Security 和 Result Endpoint。

## 2. 数据何时开始发送

消息成功进入 Node/Service 待发队列后，Port 应通知唯一 Protocol Owner。Owner 被调度后立即尝试推进，不等待 HELLO、Heartbeat 或路由维护周期。

端到端可见延迟近似由以下部分组成：

```text
任务提交
+ Owner 唤醒延迟
+ 前序高优先级队列时间
+ 路由/策略选择
+ Link 排队
+ 物理发送
+ 每个中继的接收、Owner 唤醒与再发送
+ 目标任务唤醒
```

所以“Heartbeat 1 秒一次”不意味着业务只能 1 秒发一次。Heartbeat 只是维护邻居活性的控制业务。

## 3. Q0 的顺序和重试语义

Q0 FIFO 适合不能被后消息覆盖的命令。默认发送失败会把错误返回给调用链；当调用方显式选择 `UCN_DELIVERY_RETRY_ON_BACKPRESSURE` 并提供非零绝对 Deadline 时，Node 保留该本地 FIFO 项，并可对两类可恢复状态进行有界推进：本地 Link TX Queue 暂时满，以及 Full/Lite 动态 Mesh 中路由刷新/发现造成的临时 `UCN_ERR_NOT_FOUND`。

两类等待不能混为一个计数：Link `NO_SPACE` 使用固定次数和短间隔的 backpressure retry；动态路由空窗保留原消息 Deadline 和 FIFO Order，合并同目的地 Route Discovery，并按 `UCN_Q0_ROUTE_WAIT_RETRY_INTERVAL_MS` 再试。路由恢复后仍可能立即遇到 Link 背压，此时从 route-wait 转入普通 backpressure，而不是复制消息或越过 FIFO。

当前默认边界由编译配置限定，例如最大重试次数和重试间隔均为静态参数。该机制不是：

- 端到端 ACK；
- 无限重试；
- Link Down 后自动保证送达；
- 远端执行确认。

Nano 没有动态 Mesh/Route Discovery，因此不存在 route-wait 分支；静态路由缺失仍明确失败。Service Bridge 的独立 Pending Q0 也仍只处理它自己观察到的本地 `NO_SPACE`，不能把 Core route-wait 误套到 Bridge。

如果本地队列恢复前 Deadline 到期，消息必须失败，不能在业务已经过期后突然发出旧控制命令。

## 4. Q1 Latest 为什么是有意覆盖

连续传感器数据通常满足“当前值比历史积压更有价值”。Service 的 `UCN_SERVICE_DELIVERY_Q1_LATEST` 为每个绑定保存一个当前样本：新样本到来时，如果旧样本尚未消费，新样本替换旧样本并增加 overwrite 统计。

假设温度任务 10 ms 产生一次数据，消费者暂停 100 ms：

- FIFO 会积压 10 个历史样本，恢复后仍要读取过时数据；
- Latest 只保留恢复时最新样本，内存上限固定，延迟不随暂停时间增长。

如果业务需要每一个样本，例如计费脉冲或事件日志，就不能选择 Latest；应使用 Q0 FIFO、Transfer 或产品专用持久队列。

## 5. Q2 Normal 与 Q3 Bulk

Q2 适合必须保持 FIFO、但不应占用 Q0/Q1 预算的普通消息。Q3 适合批量或可延后的流量；Transfer 的 T128～T8K Fragment 固定使用 Q3，T32/T64 Direct 使用 Q2，Transfer ACK 使用 Q1。接收端会拒绝 Message Type 与 Class 不匹配的 Fragment/ACK。

Q2/Q3 都是独立固定 FIFO：一个队列满时返回 `UCN_ERR_NO_SPACE`，不会覆盖另一级队列，也不会借用 Q0 的重试语义。需要逐条可靠送达时应使用 Transfer，不能把普通 Q2/Q3 队列误写成可靠通道。

`ucn_transfer_step()` 当前每次通过立即发送 API 提交一个 Fragment，因此 Fragment 虽在 Wire 上标为 Q3，却不先进入 Node Q3 FIFO。产品 Protocol Owner 仍要明确限制 Transfer 每轮预算；不能只依靠 Traffic Class 就假设本机 Transfer 与排队业务已经自动公平。

## 6. 背压到底发生在哪一层

`UCN_ERR_NO_SPACE` 不是“Wi-Fi 很慢”的同义词，而是某个固定容量边界暂时没有可用槽。应结合调用点和统计判断具体层级：

| 层级 | 可能满的对象 | 典型处理 |
| --- | --- | --- |
| Service 本地 Inbox | 目标任务没有及时消费 Q0/Q2/Q3 | 唤醒/修复任务，或让发送方明确失败 |
| Service Remote TX | Protocol Owner 没有及时取走 | 调整 Owner 调度或队列容量 |
| Node TX | 前序帧尚未提交 Link | 有界重试、降低生产速率或分流 |
| Adapter RX | Driver 产生快于 Owner 消费 | 增大静态 Ring、缩短通知延迟或 DMA 批量提交 |
| Link Driver | UART/CAN/Wi-Fi 硬件/驱动繁忙 | 等待可写事件、提高带宽、调整 Carrier 批量 |
| Transfer Slot | 并发大消息或重组过多 | 限制并发、完成/超时后释放 |

定位时应分别看每层高水位、drop/backpressure 计数和 Owner 周期，不能只看最终吞吐就猜“协议慢”。

## 7. Deadline 的正确含义

Deadline 是绝对的 32-bit 单调毫秒时刻，值 0 保留为“无 Deadline”哨兵。相对时长必须在 `1..INT32_MAX` 内，通过 `ucn_deadline_from_now()` 生成，比较使用 `ucn_deadline_expired()`。

```c
uint32_t deadline = ucn_deadline_from_now(now_ms, 20U);
if (deadline == 0U) {
    return UCN_ERR_ARGUMENT;
}
```

Deadline 不是定时发送时刻，而是“这条工作最晚到什么时候仍有意义”。Owner 在每次推进时应先剔除已过期工作，再把容量给仍有效的数据。

## 8. 四级调度公平与控制面维护

持续业务负载下，协议仍要发送 Heartbeat、Route、Path 和错误控制帧，否则网络会因为维护帧饥饿而误判链路断开。Node 与 Service Remote TX 使用相同的固定 12 槽顺序：

```text
Q0,Q1,Q0,Q2,Q0,Q1,Q0,Q3,Q0,Q1,Q0,Q2
```

四级持续满载时，每 12 次成功业务选择分别服务 Q0/Q1/Q2/Q3 `6/3/2/1` 次。某队列为空时选择器继续寻找其他非空队列，不浪费 Link 机会；Q3 满载时最迟也会在一个完整周期获得一次机会。这个比例只约束业务选择，不包含到期维护帧、Driver 排队和物理发送时间。

正确目标是：

- Q0 关键业务获得低等待；
- Q1/Q2/Q3 不永久饥饿；
- 到期维护帧能在有界时间内执行；
- 诊断流量不能挤占正常业务；
- 所有 burst 与队列上限可配置、可测量。

## 9. 多路径与负载均衡对队列的影响

Q1 Flow 可以在 Full Profile 下使用粘滞或自动均衡策略。均衡不是把同一 Flow 的每个包随意轮转到不同路径；协议需要考虑路径验证、Flow Lease、顺序和当前动态 Cost。

Q0 通常更重视确定性和固定/故障切换策略。产品可以为同一目标的不同 Endpoint 配置不同 Route Policy，例如舵机控制固定 UART，IMU 流在 UART 与 Wi-Fi 间均衡，Q3 日志优先 Wi-Fi。调度类和路由策略是两个维度：Q0～Q3 决定排队意图，Policy 决定走哪条可用路径。

## 10. 常见失败场景

### 10.1 生产速度高于链路速度

队列水位持续上升，最终触发背压。扩大队列只会延后问题并增加延迟；根本修复是降低产生速率、增加带宽、合并小帧或分担路径。

### 10.2 Owner 只按固定长周期轮询

即使链路空闲，消息也要等待下一次轮询，造成额外延迟。支持中断/事件的 Port 应由 ISR 写驱动 Ring 并通知 Owner；轮询只作为不支持通知的平台或保底机制。

### 10.3 把 Q1 用于必须逐条执行的命令

Latest 覆盖会改变业务语义。这不是协议丢包，而是错误的 Binding 配置。

### 10.4 把本地接受当作远端成功

队列接受只证明本地拥有数据。需要远端执行结果时必须设计 Result Endpoint。

## 11. 验证指标

产品测试至少记录：

- Q0～Q3 提交到 Link 的 P50/P95/P99 延迟；
- 各层队列高水位、背压和 overwrite 次数；
- Q0 route-wait started/retried/recovered/expired/terminal 与 Link backpressure 是否分开；
- 持续 Q0 压力下 Heartbeat/Route 是否仍按预算推进；
- Deadline 到期后旧消息是否确实不再发送；
- Q1 Latest 是否只保留最新值；
- 四级满载时是否维持 `6:3:2:1`，Q2/Q3 是否无饥饿；
- `tx_enqueued_by_class`、`tx_scheduled_by_class`、`tx_sent_by_class` 差值是否可解释；
- 两条不相干 Link/路径是否能由各自驱动并行推进；
- Owner CPU 占用与每次 step 的工作预算。

这些指标比单一“串口波特率”更能解释 UCN 的真实调度效率。
