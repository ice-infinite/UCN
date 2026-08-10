# UCN 节点内任务通信建议

> 状态：**T25.0～T25.3 已完成（T25.3 为 ESP32 构建验证）；T25.4 实机验收待实施**；v4 C99 Core 仍不包含 FreeRTOS 任务消息总线。
> 适用范围：一个 MCU 内有 IMU、控制、舵机、电源、日志等多个任务，同时需要保持“本地与远端调用形式一致”的产品。
> 关联：[整体架构](UCN_整体架构设计.md) · [协议分层与配置档案](UCN_协议分层与配置档案.md) · [更新后设计方案](UCN_更新后设计方案.md) · [详细执行方案](UCN_T25_节点内任务通信详细执行方案.md) · [T25.0 首版 Endpoint 与 Service 契约](UCN_T25_首版Endpoint与Service契约.md) · [任务表](00-任务表.md)。

## 1. 结论与边界

可以让任务通过 UCN 风格的统一接口通信，但**任务不应成为独立 UCN Node**。

- **Node** 代表可独立入网、拥有 Node ID、邻居、路由、安全身份和 Link 的设备实体；通常是一块 MCU。
- **Task / Service** 代表该 Node 内的业务执行单元；一个任务可占用一个或多个静态 Endpoint。
- 跨 Node 时使用 `destination Node ID + Endpoint + QoS + Payload`；同 Node 时按相同目标语义投递到本地固定队列，绝不封装 UCN 帧、绝不占用 Link、绝不寻路。

若把每个 FreeRTOS 任务都当作 Node，会为每个任务重复引入 Node ID、邻居/心跳/路由语义，并会把任务重启误认为设备离网。这既浪费 MCU RAM，也破坏“设备网络”和“本地调度”分层，因此不采用。

当前 v4 已有 `0x40..0xBF` 静态 Endpoint、`ucn_node_send_endpoint()` 和固定 Endpoint 回调表；目标帧到达后回调在 Core 的协议任务上下文执行。T25.1 已在 Core 外实现纯 C `ucn_service`：固定 Binding、本机直投、远端 TX Q0/Q1、Inbox、就绪/ACL 与统计；T25.2 已用独立 Bridge 将远端 TX 有界提交给唯一 Protocol Task，并把 Endpoint callback 接回 Router。T25.3 已在 ESP32 产品工程以静态 Queue、任务通知、短临界区和静态业务 Task 接入它们；这些 FreeRTOS 对象仍完全留在产品 Port，不进入 C99 Core。

## 2. 推荐结构

```text
MCU Node (一个 Node ID)
│
├─ UCN 协议任务：唯一拥有 ucn_node_t
│   ├─ Adapter RX Pump / ucn_node_step()
│   ├─ 处理邻居、路由、Q0/Q1、安全和转发
│   └─ Endpoint 回调只做快速本地投递
│
├─ Service / Task Adapter（固定表、固定队列）
│   ├─ Router Inbox 是唯一 Payload 队列；Endpoint 事件 Queue 仅唤醒目标 Task
│   ├─ 业务任务受短临界区保护地提交 Router，Protocol Task 独占 Bridge/Core
│   └─ 统计：本地投递、队列满、未知 Endpoint、远端发送失败
│
├─ IMU Task       : Endpoint 0x40，Q1，Latest Value Inbox
├─ Control Task   : Endpoint 0x60，Q0，控制状态机
├─ Servo Task     : Endpoint 0x61，Q0，PWM 与失联安全
└─ Power Task     : Endpoint 0x50，Q1，状态/告警
```

协议任务是唯一可以调用 `ucn_node_step()`、`ucn_node_receive()` 和最终 `ucn_node_send_endpoint()` 的上下文。业务任务不能并发修改 `ucn_node_t`、路由表或 Link；它们只在短临界区内操作固定 Router。当前 Adapter 驱动回调不得直接运行 Core 或应用回调的规则继续有效。

## 3. 统一发送与接收流程

建议在 FreeRTOS/产品 Port 层提供（名称可在实施时冻结）：

```c
ucn_result_t ucn_service_send(ucn_service_router_t *router,
                              ucn_node_id_t destination,
                              ucn_endpoint_t endpoint,
                              ucn_traffic_class_t traffic_class,
                              const uint8_t *payload,
                              uint16_t payload_length);
```

```text
业务任务 A
  → ucn_service_send(destination, endpoint, qos, payload)
     ├─ destination == 本机 Node ID
     │   → Endpoint 查固定表 → 投递目标任务 Inbox
     └─ destination != 本机 Node ID
         → 固定 TX Request Queue → UCN 协议任务
         → ucn_node_send_endpoint() → Link / 路由 / 远端 Node

远端 UCN 帧到达本机
  → Adapter RX Queue → 协议任务 → Endpoint handler
  → Service / Task Adapter → 目标任务 Inbox
```

这里的“统一”是调用者使用相同的目标 Node 与 Endpoint 语义，不是要求本地消息绕一圈无线网络。若某个本机业务以后可能移到远端，它使用相同的 API 和 Payload ABI 即可；本机路径仍保持低延迟、零空口开销。

为保证本地/远端可替换，`ucn_service_send()` 的透明业务 Payload 应限制在当前 `UCN_MAX_PAYLOAD_BYTES`。本机专用的大块数据、图像或 DMA Buffer 不应伪装成可远传的 UCN 消息，应使用产品本地 Buffer Pool 或后续 Extended 大数据机制。

## 4. 固定资源与 QoS 规则

Service/Task Adapter 属于 **Port / 产品层**，而不是把 FreeRTOS API 写进 C99 Core。建议所有表和队列在编译期冻结：

| 对象 | 固定内容 | 规则 |
| --- | --- | --- |
| Service 表 | Endpoint、目标 Inbox、最大 Payload、允许 QoS、投递策略。 | 一个任务可注册多个 Endpoint；重复 Endpoint 或超限必须启动失败。 |
| TX Request 队列 | 目标 Node、Endpoint、Q0/Q1、长度、固定 Payload 副本。 | 业务任务只入队；协议任务出队后调用 Core。满时立即失败并计数。 |
| Q0 Inbox | 控制命令的有界 FIFO。 | 保留顺序；不得把旧命令静默覆盖。舵机仍须本地超时/安全状态机。 |
| Q1 Inbox | 传感器/状态的单槽或有界 Latest Value。 | 同一 Endpoint 的新值覆盖旧值，避免 IMU 等高速数据堆积。 |
| 本地投递统计 | 成功、未知 Endpoint、目标未就绪、队列满、过期。 | 不改变 UCN 帧统计；用于诊断任务负载。 |

首版应使用固定长度 Payload **复制**，先保证所有权清晰和跨任务安全。若 RAM 压力证明复制不可接受，后续才增加固定 Buffer Pool + 引用计数；不得改为无边界动态分配或把调用方栈指针异步保存。

## 5. IMU、控制与舵机的推荐用法

| 业务 | Endpoint 示例 | 本机路径 | 远端路径 | 安全/实时边界 |
| --- | --- | --- | --- | --- |
| IMU 数据 | `0x40` | Q1 Latest Value，控制任务只取最新样本。 | Q1；在已有路由上发送，首次远端发送可走现有有界寻路。 | Payload 应带时间戳/序号；不积压旧样本。 |
| 电机控制指令 | `0x60` | 有界控制 Inbox。 | Q0；必须已有直连或预建路由，不能等待寻路。 | 按 Endpoint 配置来源 ACL/端到端保护。 |
| 舵机目标值 | `0x61` | 控制任务投递舵机任务；PWM/限位/超时均在本机完成。 | Q0 到目标 Node，再投递其舵机任务。 | 网络不是 PWM、FOC 或安全闭环；超时必须回落到本地安全状态。 |
| 电源状态 | `0x50` | Q1 状态或独立本地告警。 | Q1；按产品定义可另设 Q0 故障 Endpoint。 | 高风险告警可独立 Endpoint 和权限策略。 |

Endpoint 是业务 ABI，不是“任意函数名”。编号、Payload 布局、字节序、单位、版本、QoS、权限和队列策略必须和产品配置一起冻结。任务重启只影响本机 Service 是否就绪，不改变整个 MCU 的 Node ID、邻居或路由；未就绪 Endpoint 的本地/远端投递应显式拒绝或计数，不能假装成功。

## 6. T25 实施与验证门禁

| 子任务 | 实现内容 | 单元测试 | 虚拟/实机验收 |
| --- | --- | --- | --- |
| T25.1 | 冻结静态 Service 表、Endpoint ABI、队列容量、Payload 所有权和错误码；Core 不引入 RTOS 头文件。 | 重复/非法 Endpoint、表满、长度/QoS 不匹配、启动失败。 | 在不链接 FreeRTOS 的 C99 测试中验证固定 Service 表。 |
| T25.2 | 实现可替换的纯 C Service Router 与本机直投；仅协议任务拥有 Core。 | 本机 A→B 不调用 Link；远端请求只进入 TX Queue；未知/未就绪/满队列可见失败；Q1 覆盖、Q0 FIFO。 | 虚拟 A→B→C：C 的 Endpoint 投递到 C 的本机任务 Inbox；中继不出现业务回调。 |
| T25.3 | 实现 FreeRTOS Port：静态 Queue/Task Handle 绑定、协议任务 Pump、无阻塞投递和统计。 | 并发业务任务入队、队列满、任务未就绪、协议任务所有权。 | 两块 ESP32-S3：本机模拟 IMU→Control→Servo 与远端控制 Endpoint 并发；测队列丢弃、时延、栈/RAM。 |
| T25.4 | 与 T15/T19/T20 联动：Endpoint ACL、安全策略、首包 Q1 与产品 ABI。 | 本机/远端策略边界、Q0 不等待、受保护帧只在目标 Service 投递。 | 持续 IMU Q1 + 舵机 Q0 + 断链/重连；验证本地安全动作不受网络故障影响。 |

实施时不改变 v4 正常业务帧格式，不新增 Node ID，不修改 AODV-Lite 或把任务注册为网络发现流量。只有 Service/Task Adapter 自身、产品 Endpoint 表和对应测试通过后，才把它标为已实现。

详细的接口、固定内存、任务上下文、测试矩阵、实施门禁和后续零拷贝边界，见[《UCN T25 节点内任务通信详细执行方案》](UCN_T25_节点内任务通信详细执行方案.md)；首批 Endpoint 的具体编号和 Payload ABI 见[《T25.0 首版 Endpoint 与 Service 契约》](UCN_T25_首版Endpoint与Service契约.md)。
