# 任务间与跨 MCU Service 通信

Service 让业务用统一方式表达“把这条消息交给哪个任务/服务”。目标在本机时走固定Inbox Fast Path；目标在远端时由Bridge封装成UCN Endpoint消息。

任务不是虚拟Node：路由只寻址MCU/设备，Endpoint和Service ID区分设备内部业务。

## 1. 什么时候使用 Service

适合：

- 本机传感器任务→控制任务；
- Node A任务→Node C执行器任务；
- IMU/温度等Latest Value；
- 关键命令Q0 FIFO；
- 需要统一ACL、ready和业务Result语义。

不适合：

- 4KiB文件直接塞Service Inbox，应使用Transfer；
- ISR直接调用Service，应先投递Owner/任务队列；
- 把每个任务伪装成独立网络Node。

## 2. 定义 Service ID 和 Binding

```c
enum {
    SERVICE_SENSOR = 1,
    SERVICE_CONTROL = 2,
    SERVICE_ACTUATOR = 3
};

#define MASK_SENSOR  UCN_SERVICE_SOURCE_MASK(SERVICE_SENSOR)
#define MASK_CONTROL UCN_SERVICE_SOURCE_MASK(SERVICE_CONTROL)
#define MASK_Q0 UCN_SERVICE_TRAFFIC_MASK(UCN_TRAFFIC_Q0_CRITICAL)
#define MASK_Q1 UCN_SERVICE_TRAFFIC_MASK(UCN_TRAFFIC_Q1_REALTIME)

static const ucn_service_binding_t PRODUCT_BINDINGS[] = {
    {
        .endpoint = PRODUCT_EP_IMU_SAMPLE,
        .owner_service_id = SERVICE_CONTROL,
        .max_payload_length = 24U,
        .allowed_traffic_mask = MASK_Q1,
        .delivery_mode = UCN_SERVICE_DELIVERY_Q1_LATEST,
        .allowed_local_source_mask = MASK_SENSOR,
        .accept_remote = true,
        .enabled_at_boot = true,
        .require_remote_q0_validator = false
    },
    {
        .endpoint = PRODUCT_EP_SERVO_COMMAND,
        .owner_service_id = SERVICE_ACTUATOR,
        .max_payload_length = 16U,
        .allowed_traffic_mask = MASK_Q0,
        .delivery_mode = UCN_SERVICE_DELIVERY_Q0_FIFO,
        .allowed_local_source_mask = MASK_CONTROL,
        .accept_remote = true,
        .enabled_at_boot = false,
        .require_remote_q0_validator = true
    }
};
```

Binding表是长期借用的只读数据，应放在 `static const` 存储中。

## 3. 初始化 Router

```c
ucn_service_router_t g_router;

const ucn_service_router_config_t router_config = {
    .local_node_id = PRODUCT_NODE_ID,
    .bindings = PRODUCT_BINDINGS,
    .binding_count = (uint8_t)(sizeof(PRODUCT_BINDINGS) /
                               sizeof(PRODUCT_BINDINGS[0]))
};

check(ucn_service_router_init(&g_router, &router_config));
```

执行器任务真正准备好以后再开放：

```c
check(ucn_service_set_ready(
    &g_router, PRODUCT_EP_SERVO_COMMAND, true));
```

任务重启前：

```c
check(ucn_service_set_ready(
    &g_router, PRODUCT_EP_SERVO_COMMAND, false));
```

设为false会清理该Binding的旧Inbox，防止重启后的任务执行重启前命令。

## 4. 本机任务通信

```c
ucn_service_acceptance_t acceptance;
ucn_result_t result = ucn_service_send_ex(
    &g_router,
    PRODUCT_NODE_ID,
    SERVICE_SENSOR,
    PRODUCT_EP_IMU_SAMPLE,
    UCN_TRAFFIC_Q1_REALTIME,
    payload,
    payload_length,
    &acceptance);
```

成功时 `acceptance == LOCAL_DELIVERED`，表示消息已复制到本机Inbox。

消费者任务读取：

```c
ucn_service_message_t message;
ucn_result_t result = ucn_service_inbox_take(
    &g_router,
    SERVICE_CONTROL,
    PRODUCT_EP_IMU_SAMPLE,
    &message);
```

Q1只取当前Latest；Q0按FIFO逐条取。读取和发送跨任务时，产品必须用短临界区或由唯一Router Owner串行化。

## 5. 跨 MCU 通信

初始化Bridge：

```c
ucn_service_protocol_bridge_t g_bridge;

check(ucn_service_protocol_bridge_init(
    &g_bridge, &g_router, &g_node));
```

高风险远端Q0必须先装Validator：

```c
check(ucn_service_protocol_bridge_set_validator(
    &g_bridge,
    PRODUCT_EP_SERVO_COMMAND,
    validate_remote_servo_command,
    &validator_context));
```

再让Bridge占用Binding对应的Endpoint handler：

```c
check(ucn_service_protocol_bridge_install_endpoint_handlers(&g_bridge));
```

远端发送仍使用同一Service API：

```c
ucn_service_acceptance_t acceptance;
check(ucn_service_send_ex(
    &g_router,
    remote_node_id,
    SERVICE_CONTROL,
    PRODUCT_EP_SERVO_COMMAND,
    UCN_TRAFFIC_Q0_CRITICAL,
    command_payload,
    command_length,
    &acceptance));
```

此时 `REMOTE_ENQUEUED` 只表示Router取得固定副本。Protocol Owner后续调用Bridge Step：

```c
uint8_t processed = 0U;
ucn_result_t result = ucn_service_protocol_bridge_step_at(
    &g_bridge, now_ms, 2U, &processed);
```

若Protocol Owner配置中已传入Bridge，Owner Step会按预算推进，无需业务重复调用。

## 6. Q0 Backpressure

Bridge可为一个固定Q0 pending槽配置有界重试：

```c
const ucn_service_bridge_q0_backpressure_policy_t retry = {
    .max_retries = 3U,
    .retry_interval_ms = 5U,
    .timeout_ms = 30U
};

check(ucn_service_protocol_bridge_set_q0_backpressure_policy(
    &g_bridge, &retry));
```

它只重试本地 `UCN_ERR_NO_SPACE`，不是远端ACK，也不会重传已被Link接受的Frame。

## 7. 命令 Guard 和 Result

危险命令建议在Payload前加Guard：

```c
ucn_service_command_guard_t guard = {
    .command_id = next_command_id,
    .issued_at_ms = shared_time_ms,
    .valid_for_ms = 100U,
    .result_endpoint = PRODUCT_EP_COMMAND_RESULT,
    .flags = 0U
};

check(ucn_service_command_guard_encode(&guard, payload));
```

远端Validator/任务调用：

```c
check(ucn_service_command_guard_decode(
    payload, payload_length, &guard));
check(ucn_service_command_guard_validate(
    &guard, now_ms, has_last_command_id, last_command_id));
```

只有两个节点共享可信毫秒时间域时才使用 `issued_at_ms`。否则定义产品generation/lease机制，不要假装两块MCU的本地uptime同步。

执行后发送Result：

```c
ucn_service_result_header_t result_header = {
    .command_id = guard.command_id,
    .stage = UCN_SERVICE_STAGE_REMOTE_EXECUTED,
    .status = UCN_SERVICE_RESULT_SUCCEEDED,
    .detail_code = 0U
};
```

`REMOTE_EXECUTED`必须由远端任务显式发出，不能由Transport ACK或Link completion推断。

## 8. 推荐任务模式

```text
Sensor Task
  → Service Q1 Latest
  → Local/Remote Control Inbox

Control Task
  → Service Q0 Command + Guard
  → Remote Actuator Inbox
  → Execute
  → Result Endpoint
```

## 9. 常见错误

| 错误 | 原因 |
| --- | --- |
| `ACCESS` | 本机source service不在mask，或远端ACL拒绝 |
| `NOT_FOUND` | Binding not-ready、Endpoint不存在或Inbox空 |
| `NO_SPACE` | Q0 Inbox/Remote Queue满 |
| `TOO_LARGE` | 超过Binding或Service最大Payload |
| `CONFIG` | Binding重复、Q0/Q1数量超容量、缺Validator |

## 10. 验收

- 本机和远端相同消息产生相同业务结果；
- Q1覆盖旧样本，Q0保持FIFO；
- Task not-ready会清旧Inbox；
- 远端Q0缺Validator时Bridge安装失败；
- 重复/过期Command被拒绝；
- Result丢失、重复Command和任务重启有策略；
- `REMOTE_ENQUEUED`从未被当作执行成功。

大于Service Payload的数据阅读 [Transfer](08-32B到8KiB-Transfer.md)。
