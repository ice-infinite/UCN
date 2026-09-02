# 定义 Endpoint 与收发消息

UCN 用 Node ID 决定消息去哪个设备，用 Endpoint 决定消息交给设备里的哪一类业务。多个传感器可以共享同一条 Route：IMU、气压计和温度只需要不同 Endpoint，不需要各自成为一个 Node。

## 1. 先定义产品消息 ABI

业务 Endpoint 范围是 `0x40..0xBF`。建议建立一份产品公共头：

```c
typedef enum product_endpoint {
    PRODUCT_EP_IMU_SAMPLE      = 0x40,
    PRODUCT_EP_BARO_SAMPLE     = 0x41,
    PRODUCT_EP_TEMPERATURE     = 0x42,
    PRODUCT_EP_SERVO_COMMAND   = 0x50,
    PRODUCT_EP_COMMAND_RESULT  = 0x51,
    PRODUCT_EP_PARAMETER_BLOCK = 0x60
} product_endpoint_t;
```

每个 Endpoint 至少冻结：

| 项目 | 示例 |
| --- | --- |
| Endpoint | `0x40` |
| 版本 | `1` |
| Payload长度 | 固定16 B或长度范围 |
| 字节序 | 大端 |
| 单位 | 加速度 mm/s² |
| 更新语义 | Q1 Latest |
| 安全要求 | 必须E2E或允许明文 |
| 发送者/接收者 | Sensor→Estimator |

不要直接发送编译器结构体布局，除非显式控制 padding、对齐、字节序和版本。跨不同 MCU/编译器时推荐逐字段编码。

## 2. 注册 Endpoint Handler

```c
static void servo_command_received(
    void *context,
    const ucn_frame_t *frame)
{
    servo_task_queue_t *queue = context;
    servo_command_t command;

    if (frame->payload_length != PRODUCT_SERVO_COMMAND_BYTES) {
        product_count_bad_command();
        return;
    }
    if (!product_decode_servo_command(
            frame->payload, frame->payload_length, &command)) {
        product_count_bad_command();
        return;
    }
    command.source_node = frame->source;
    command.source_session = frame->session_id;
    product_servo_queue_try_push(queue, &command);
}

check(ucn_node_set_endpoint_handler(
    node,
    PRODUCT_EP_SERVO_COMMAND,
    servo_command_received,
    &servo_queue));
```

Handler 运行在 Protocol Owner 上下文，必须快速完成。它适合校验、复制和投递任务队列，不适合直接控制电机、写 Flash 或等待互斥锁。

若没有匹配 Endpoint handler，可使用通用 RX handler 接收其他非控制消息：

```c
ucn_node_set_rx_handler(node, generic_receive, context);
```

优先为正式业务注册具体 Endpoint；通用 handler 更适合兼容和诊断，不应变成所有业务的巨型分发函数。

## 3. 立即发送

```c
ucn_result_t result = ucn_node_send_endpoint(
    node,
    destination_node,
    PRODUCT_EP_IMU_SAMPLE,
    UCN_TRAFFIC_Q1_REALTIME,
    encoded_sample,
    encoded_length);
```

适合：

- 当前就在 Owner 上下文；
- Payload 生命周期清楚；
- 希望立即知道本地路由/链路是否接受。

业务任务通常不要直接调用。它应把发送请求投递给 Owner，由 Owner 调用此 API。

## 4. 使用有界发送队列

```c
ucn_send_request_t request = {
    .destination = destination_node,
    .message_type = PRODUCT_EP_IMU_SAMPLE,
    .traffic_class = UCN_TRAFFIC_Q1_REALTIME,
    .delivery = UCN_DELIVERY_LATEST_VALUE,
    .deadline_ms = product_monotonic_ms() + 50U,
    .payload = encoded_sample,
    .payload_length = encoded_length
};

ucn_result_t result = ucn_node_enqueue(node, &request);
```

Node 成功入队后拥有 Payload 的固定副本，调用者可以复用原 Buffer。队列内容由后续 `ucn_node_step()` 发送。

## 5. 选择 Q0～Q3 和 Delivery

当前 Node、Service 与 Transfer 使用统一的四级语义：

| 场景 | Traffic | Delivery | 行为 |
| --- | --- | --- | --- |
| 舵机/急停命令 | Q0 Critical | Retry on backpressure | 有界保留，不能无限重试 |
| IMU/温度实时样本 | Q1 Realtime | Latest value | 同一目标/Endpoint保留最新值 |
| 参数、查询结果、普通状态 | Q2 Normal | Best effort | 独立 FIFO，不覆盖、不自动重试 |
| 文件/日志/大块消息 | Q3 Bulk 或 Transfer | Best effort/Transfer | 普通 Q3 是小容量 FIFO；大消息由 Transfer 分片、ACK 和限流 |

`Q0` 表示本地调度优先，不等于端到端可靠、远端执行或物理安全。危险命令仍需 Command ID、Deadline、去重和业务 Result。

四级队列持续满载时，Node 以 `6:3:2:1` 服务 Q0/Q1/Q2/Q3。该比例防止 Q2/Q3 永久饥饿，但不是物理链路带宽保证；Owner、路由维护、Driver 和真实介质仍会增加等待。

### Latest Value

适合连续传感器流。如果旧 IMU 样本还没发送，新样本可以覆盖旧值，避免网络恢复后把过期数据全部补发。

### Retry on backpressure

只针对本地 `UCN_ERR_NO_SPACE` 做有界 admission retry，不会因为远端未执行而自动重发。

### Q2/Q3 FIFO

Q2 与 Q3 都不会像 Q1 Latest 一样覆盖旧值，也不能选择 Q0 Retry。队列满时调用方收到 `UCN_ERR_NO_SPACE`，应按产品语义限流、丢弃可丢日志或改用 Transfer；不要无限重试堵住业务任务。

## 6. Payload 大小

单个 Core Frame 的 Payload 上限由：

```text
UCN_MAX_FRAME_BYTES
  - 当前Wire头
  - Route/Path扩展
  - 可选E2E Tag
  - 当前Link MTU
```

共同决定。不要只看 `UCN_MAX_PAYLOAD_BYTES` 就假设所有链路都能发送该长度。超过当前单帧能力时使用 Transfer。

## 7. 正确理解成功层级

```text
send/enqueue 返回 UCN_OK
  ≠ 物理发送完成
  ≠ 远端 Core 收到
  ≠ 远端任务 Inbox 接收
  ≠ 远端任务执行成功
```

需要远端执行结果时，定义结果 Endpoint：

```text
Command(command_id, deadline, args)
  → Remote Inbox
  → Execute
  → Result(command_id, stage, status, detail)
```

发送方用 `command_id` 关联结果，超时后查询状态或重发同一个幂等 ID，不能假设“没收到 Result 就一定没执行”。

## 8. 常见错误

| 错误 | 常见原因 | 处理 |
| --- | --- | --- |
| `ARGUMENT` | Endpoint不在静态范围、NULL/长度非法 | 修正ABI和调用参数 |
| `TOO_LARGE` | Frame或Link MTU不足 | 缩小消息或用Transfer |
| `NOT_FOUND` | 无Route/Path | 固定Route或动态discover |
| `NO_SPACE` | Q0～Q3、Service Inbox或Driver队列满 | 按业务语义丢弃/重试/告警 |
| `TTL` | 请求已过Deadline或Hop耗尽 | 不发送过期命令，检查路由环 |
| `SECURITY` | required但Provider/Policy未就绪 | 保持失败关闭，不降级明文 |
| `ACCESS` | ACL、Join或管理授权拒绝 | 检查身份和产品策略 |

## 9. Endpoint 验收

- 每个 Endpoint 的编号和 Payload 有正式版本文档；
- 所有长度、枚举、单位和字节序做负向测试；
- Handler 返回后不再引用 Frame/Payload；
- Q1 Latest 确实覆盖旧样本；
- Q0 满载时有明确降级/告警；
- Q2/Q3 FIFO 满载时不覆盖其他 Class，持续混合负载下不饥饿；
- 命令重复、过期、Result丢失均有业务策略；
- 本机 `UCN_OK` 未被误写成远端执行成功。

下一步阅读：[接入通信介质](04-接入UART-CAN-WiFi等通信介质.md)。
