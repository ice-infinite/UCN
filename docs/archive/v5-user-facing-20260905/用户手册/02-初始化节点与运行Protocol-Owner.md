# 初始化节点与运行 Protocol Owner

用户接入 UCN 最重要的运行规则是：**每个 `ucn_node_t` 只有一个上下文能够推进协议状态。** 这个上下文称为 Protocol Owner，可以是裸机主循环、RTOS 任务或 Host 线程。

## 1. 对象和所有权

| 对象 | 谁分配 | 谁能修改 | 生命周期 |
| --- | --- | --- | --- |
| `ucn_node_t` | 产品静态分配 | Protocol Owner | 节点整个运行期 |
| `ucn_link_t` | 产品/Adapter静态分配 | 初始化期和对应Driver | 注册前建立，停机后释放 |
| `ucn_adapter_rx_queue_t` | 产品静态分配 | Driver生产、Owner消费 | Driver回调存在期间 |
| `ucn_event_runtime_t` | 产品静态分配 | Owner与受限signal入口 | Owner任务整个生命周期 |
| Endpoint payload view | UCN临时持有 | 回调只读 | 回调返回前 |
| Stats 指针 | UCN对象内部 | 调用者只读 | 下一次状态推进前读取 |

业务任务不要直接读写 `ucn_node_t` 字段，也不要用一个大 mutex 让多个任务轮流调用 Node。顺序、回调和状态机仍可能被破坏。

## 2. 推荐初始化顺序

```text
1. 时钟、随机源、持久计数器
2. GPIO/UART/CAN/Wi-Fi/USB Driver，但先不开放RX
3. Port临界区和调度通知
4. ucn_node_init
5. Wire/Profile/Session/Security配置
6. Endpoint handler、Service、Transfer
7. Link、Adapter Queue、Source、Event Runtime
8. 注册Link和固定Route/发现策略
9. 清Driver pending状态
10. 最后打开IRQ/DMA/无线回调
```

先开中断再初始化 Ring/Queue 会让第一批数据进入未初始化对象。

## 3. Node 初始化

```c
ucn_config_t config = {
    .network_id = PRODUCT_NETWORK_ID,
    .node_id = PRODUCT_NODE_ID,
    .default_hop_limit = 8U
};

ucn_result_t result = ucn_node_init(&g_node, &config);
if (result != UCN_OK) {
    product_fatal("ucn_node_init", result);
}
```

配置规则：

- Network ID、Node ID 不得为保留值；
- Hop Limit 为 1..`UCN_MAX_HOPS`；
- 同一运行网络的 Node ID 唯一；
- 初始化失败后不要继续注册 Link。

随后在网络流量开始前配置 Wire 和 Session：

```c
check(ucn_node_set_wire_profiles(
    &g_node, PRODUCT_TX_WIRE, PRODUCT_MAX_RX_WIRE));
check(ucn_node_set_plain_session_id(
    &g_node, product_next_boot_session()));
```

## 4. 最小 Adapter RX Queue

完整 Packet 驱动可以直接把一帧复制进 Adapter Queue：

```c
ucn_adapter_rx_queue_t g_rx_queue;

check(ucn_adapter_rx_queue_init(
    &g_rx_queue, &g_port_ops, &g_port_context));
```

任务上下文收到完整帧：

```c
check_or_count(ucn_adapter_rx_enqueue(
    &g_rx_queue, ingress_link, data, length));
```

ISR 中收到完整帧：

```c
check_or_count(ucn_adapter_rx_enqueue_from_isr(
    &g_rx_queue, ingress_link, data, length));
```

ISR 版本要求 `ucn_port_ops_t` 提供成对的 ISR 临界区回调；不会自动退回任务锁。

## 5. Protocol Owner 配置

```c
ucn_protocol_owner_t g_owner;

ucn_protocol_owner_config_t owner_config = {
    .node = &g_node,
    .rx_queue = &g_rx_queue,
    .port_ops = &g_port_ops,
    .port_context = &g_port_context,
    .max_rx_frames_per_step = 4U,
#if UCN_FEATURE_SERVICE
    .bridge = &g_bridge,
    .max_bridge_requests_per_step = 2U,
#endif
};

check(ucn_protocol_owner_init(&g_owner, &owner_config));
```

一次 Owner Step 会在同一时间基准下：

1. 从 RX Queue pump 有界数量的完整 Frame；
2. 推进可选 Service Bridge；
3. 调用 Node 维护；
4. 更新 Owner Stats。

```c
size_t pumped = 0U;
uint8_t bridged = 0U;
ucn_result_t result = ucn_protocol_owner_step(
    &g_owner, &pumped, &bridged);
```

## 6. 多 Source 使用 Event Runtime

有多个 UART/CAN/Wi-Fi 实例时，不要给每个接口创建一个 Node。一个 Node 注册多条 Link，每个物理输入绑定一个 Source：

```c
ucn_event_runtime_config_t runtime_config = {
    .owner = owner_config,
    .scheduler_ops = &g_scheduler_ops,
    .scheduler_context = &g_scheduler_context,
    .max_drain_rounds = 8U,
    .max_source_work_per_round = 4U
};

check(ucn_event_runtime_init(&g_runtime, &runtime_config));
check(ucn_event_runtime_bind_source(
    &g_runtime, UART1_SOURCE_ID, &uart1_source_config));
check(ucn_event_runtime_bind_source(
    &g_runtime, CAN1_SOURCE_ID, &can1_source_config));
```

Driver 回调只 signal：

```c
(void)ucn_event_runtime_signal_source_from_isr(
    &g_runtime,
    UART1_SOURCE_ID,
    UCN_EVENT_SOURCE_RX_READY);
```

Owner Task：

```c
for (;;) {
    ucn_event_runtime_run_result_t run;
    ucn_result_t result = ucn_event_runtime_task_cycle(
        &g_runtime, PRODUCT_OWNER_MAX_WAIT_MS, &run);
    product_record_runtime_result(result, &run);
    product_step_optional_transfer();
}
```

`task_cycle()` 有 pending 时不等待；无 pending 时最多等待请求时间与协议最大维护间隔中的较小值，超时会做一次 fallback scan。

## 7. 时间合同

所有模块使用同一个单调 32 位毫秒时钟：

- 允许自然回绕；
- 不允许倒退、暂停很久后伪造小值；
- timeout 必须小于等于 `UCN_MAX_SAFE_DURATION_MS`；
- Node、Owner、Transfer、Service deadline 不要使用不同的“当前时间缓存”。

裸机即使没有 RX，也必须在配置的最大间隔内推进。RTOS 任务通知丢失时，超时唤醒/fallback 是保底，不是主要接收机制。

## 8. ISR、Owner 和业务任务边界

| 上下文 | 可以做 | 不可以做 |
| --- | --- | --- |
| ISR/DMA callback | 写BSP Ring、`*_from_isr`、通知Owner | 解码Frame、调用Node、执行Endpoint业务 |
| Protocol Owner | Source service、Node RX/Step、Bridge、Transfer Step | 长时间阻塞、等待物理发送完成 |
| 业务任务 | 读自身Inbox、产生发送请求、执行算法 | 并发修改Node/Route/Path表 |
| Endpoint callback | 校验并复制消息、快速投递任务 | 保存临时payload指针、执行长耗时动作 |

## 9. 停机顺序

```text
停止应用产生新请求
  → 标记Service not-ready
  → 禁用Driver RX/IRQ/DMA callback
  → 唤醒Owner并有界drain/结束事务
  → Link close/down
  → 导出Stats
  → 停止Owner Task
  → 最后复用或释放对象
```

不要直接 kill Owner Task，否则 Driver callback 可能继续访问旧 Ring 或 Node。

## 10. 验收

- 同一 Node 从未被两个任务同时调用；
- ISR 只使用明确的 ISR API；
- Owner 最大唤醒延迟低于维护和业务 deadline；
- RX 风暴时每个 Source 都能获得预算；
- Queue满会返回错误且不覆盖旧数据；
- 通知丢失后 fallback 能恢复；
- Tick 回绕测试通过；
- 停机后没有 callback 访问旧对象。

下一步阅读：[定义 Endpoint 与收发消息](03-定义Endpoint与收发消息.md)。
