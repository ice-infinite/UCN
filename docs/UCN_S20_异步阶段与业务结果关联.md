# UCN S20 异步阶段与业务结果关联

## 1. 目标

S20 解决“`UCN_OK` 到底证明了什么”以及“远端执行结果怎样与原命令关联”两个问题。它不把 Q0 改成可靠传输，也不为普通帧增加 ACK、重传窗口或新的 v4 帧字段。

统一阶段如下：

| 阶段 | 由谁确认 | 准确含义 |
| --- | --- | --- |
| `LOCAL_INBOXED` | `ucn_service_send_ex()` | 目标是本 Node，Router 已把副本放进本机 Inbox。 |
| `REMOTE_ROUTER_QUEUED` | `ucn_service_send_ex()` | 目标是远端，源 Node 的 Router 已拥有 Remote TX 副本。 |
| `LINK_QUEUE_ACCEPTED` | Bridge Outbound Observer | 本机 Core/Link 已接受提交；不证明空口送达、远端入队或执行。 |
| `REMOTE_INBOXED` | 业务 Result Endpoint | 目标产品明确报告命令已进入其业务处理边界。 |
| `REMOTE_EXECUTED` | 业务 Result Endpoint | 目标产品明确报告执行成功、拒绝、失败或过期。 |

## 2. 本机最终提交观察

原有 `ucn_service_protocol_bridge_set_outbound_observer()` 保留兼容，只返回最终 `ucn_result_t`。新增的 `ucn_service_protocol_bridge_set_outbound_event_observer()` 返回结构化事件：

```c
static void on_local_submit(
    void *context,
    const ucn_service_message_t *message,
    const ucn_service_bridge_outbound_event_t *event)
{
    (void)context;
    product_record(message->destination_node_id,
                   message->endpoint,
                   event->stage,
                   event->outcome,
                   event->result);
}
```

`outcome` 可以区分：

- `LINK_QUEUE_ACCEPTED`：本机链路队列接受；`stage=LINK_QUEUE_ACCEPTED`、`result=UCN_OK`。
- `BACKPRESSURE_REJECTED`：未启用重试时，本机队列立即拒绝。
- `BACKPRESSURE_EXHAUSTED`：已启用 S15 有界重试，但次数或剩余 Deadline 不允许再试。
- `EXPIRED`：Bridge 持有的 Pending Q0 已经过期。
- `TERMINAL_FAILED`：Link Down、无路由、安全/参数等不可重试终止错误。

两个 Observer 可同时安装；对同一条消息，各自只在最终状态调用一次。中间 `NO_SPACE` 重试不会触发回调。Message 和 Event 指针只在回调期间有效。

## 3. 可选业务结果头

关键命令可继续使用 12 B `ucn_service_command_guard_t`，其中 `command_id` 用于关联，`result_endpoint` 指定回程 Endpoint。目标产品需要报告远端阶段时，可在结果 Payload 开头放 8 B `ucn_service_result_header_t`：

```text
0..3  command_id   大端 uint32
4     stage        REMOTE_INBOXED 或 REMOTE_EXECUTED
5     status       ACCEPTED/SUCCEEDED/REJECTED/FAILED/EXPIRED
6..7  detail_code  大端 uint16，产品 ABI 自定义
8..N  可选产品结果正文
```

合法组合只有：

- `REMOTE_INBOXED + ACCEPTED`；
- `REMOTE_EXECUTED + SUCCEEDED/REJECTED/FAILED/EXPIRED`。

示例：

```c
uint8_t result_payload[UCN_SERVICE_RESULT_HEADER_BYTES];
const ucn_service_result_header_t result = {
    .command_id = command.command_id,
    .stage = UCN_SERVICE_STAGE_REMOTE_EXECUTED,
    .status = UCN_SERVICE_RESULT_SUCCEEDED,
    .detail_code = 0U,
};

if (ucn_service_result_header_encode(&result, result_payload) == UCN_OK) {
    (void)ucn_service_send(&router,
                           command_source_node,
                           UCN_SERVICE_ID_NONE,
                           command.result_endpoint,
                           UCN_TRAFFIC_Q1_REALTIME,
                           result_payload,
                           sizeof(result_payload));
}
```

源端解码后调用 `ucn_service_result_matches_command()`，同时核对收到消息的 Source Node/产品 Session。Helper 只检查 `command_id + result_endpoint + 结果头合法性`，不会创建 Pending 表、计时器或动态内存。

## 4. 超时与拒绝

命令等待表和超时仍归产品所有。推荐使用固定容量表保存目标 Node、Command ID、Result Endpoint 和绝对 Deadline：

- 本机 Observer 报终止失败时，可立即关闭该等待项；
- 本机 Observer 报 `LINK_QUEUE_ACCEPTED` 后，继续等待业务结果；
- 到 Deadline 仍没有匹配结果，产品报告超时并执行本地安全策略；
- 目标 Validator 在 Inbox 前拒绝时，UCN 不自动生成结果，源端最终表现为“本机提交接受，但业务结果超时”；
- 目标执行 Task 能识别命令但拒绝执行时，应主动返回 `REMOTE_EXECUTED + REJECTED`。

这一区分避免把同步虚拟 Link、Link Queue 接受或目标本机统计误写成端到端成功。

## 5. 资源和线格式

- 普通数据帧、Command Guard 之外的命令和 Router 队列没有新增字节。
- 只有选择业务回执的结果消息增加 8 B Payload；可在其后追加产品正文。
- Bridge 只增加一个结构化 Observer 函数指针和 context；没有堆分配、通用 ACK 表或周期扫描。
- UCN v4 帧头、路由、安全、Q0/Q1 和 S15 Pending 线语义均未改变。

## 6. 软件验证与实机边界

`test_service.c` 覆盖阶段映射、8 B 大端编码、非法阶段/状态组合和 Command ID/Result Endpoint 匹配。`test_service_bridge.c` 覆盖结构化 Observer 的接受、立即背压拒绝、重试耗尽、终止失败、过期且每项只回调一次；两节点模拟覆盖 `REMOTE_INBOXED → REMOTE_EXECUTED` 往返、目标 Validator 拒绝后无伪 ACK、源端超时边界和 Link Down。

Debug、Release、64 B 和 Bearer=1 四个 CTest Profile 均通过；ESP32-S3 Service A/B 与 ESP-WROOM-32 只完成构建。真实 Wi-Fi/UART 上的结果时延、断链超时、执行器安全和两板日志仍属于 S07，不能由本轮软件测试替代。
