# Link、Neighbor、Endpoint API

> 文档级别：`REFERENCE`
> 实现状态：`CURRENT`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：include/ucn 公共头、链接目标与公共 API 测试
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：API 合同不等同于各平台实机驱动完成

Link API 由产品提供发送、MTU、状态和可选指标回调。每个物理/逻辑通道注册为独立 Link，因此一个节点可以同时拥有多 UART、多 CAN 和无线 Bearer。

Neighbor API 维护直连 peer 与每 Bearer 状态；Heartbeat 不做全网洪泛。Neighbor summary 是只读复制，供 Cluster/诊断使用。

Endpoint API 把 `endpoint_id` 映射到 handler 和可选 Security Policy。注册表固定容量；重复 ID、非法范围或满表返回错误。handler 在 Protocol Owner 上下文执行，耗时业务应转交应用任务。

## Link 对象与回调

一个 Link 对应一个可独立发送、报告 MTU/状态/指标的通道实例：UART1 和 UART2 是两个 Link，CAN1 和 CAN2 也是两个 Link，即使它们使用同一驱动类型。

```c
static const ucn_link_ops_t ops = {
    .open = product_open,
    .send = product_enqueue_tx,
    .poll_rx = product_poll_rx,
    .get_status = product_get_status,
    .close = product_close,
    .get_metrics = product_get_metrics,
};

ucn_link_t uart1 = {
    .ops = &ops,
    .context = &uart1_driver,
    .link_id = 1U,
    .mtu = UCN_MAX_FRAME_BYTES,
    .peer_node_id = 0x22U,
};
```

`send()` 只做有界 copy/enqueue，不能阻塞等待物理发送完成；驱动队列满返回 `UCN_ERR_NO_SPACE`，链路断开返回 `UCN_ERR_LINK_DOWN`。`get_status()` 返回动态 up/MTU，effective MTU 取静态与动态的较小值。`get_metrics()` 的 queue/failure/RTT 都是该物理 Link 的窗口指标，不得填 Core 队列或端到端指标。

初始化 Driver/Adapter 后再 `ucn_node_register_link()`。如果 `open()` 由产品单独调用，应把调用顺序固定在集成文档中，避免 Node 已开始选路但硬件尚不可用。

## Neighbor 生命周期

Neighbor 是“某个 Node ID 经某个或多个 Bearer 与本节点直连”的状态。HELLO/观察先建立 candidate，Join Policy/authorize 决定 admit/reject；Heartbeat 只在一跳范围更新 freshness，不做全网转发。

```text
物理帧到达
  → observe/probe
  → JOIN challenge/authorize
  → ADMITTED
  → Heartbeat/Link metrics 续期
  → SUSPECT
  → REMOVE，并使依赖路由失效/RERR
```

产品也可以对固定点对点 Link 调用 `ucn_node_admit_neighbor()`，但仍要保证 peer identity 与物理配置可信。

只读摘要使用两步模式：先以 `NULL,0` 获取非空数量，再提供容量；但两次调用间状态可能变化，产品应提供上限 buffer 或接受有界结果。

## Endpoint 注册与接收

```c
static void imu_rx(void *ctx, const ucn_frame_t *frame)
{
    app_queue_latest((app_queue_t *)ctx,
                     frame->payload,
                     frame->payload_length);
}

ucn_node_set_endpoint_handler(&node, IMU_ENDPOINT, imu_rx, &imu_queue);
```

同一 Node 的 IMU、气压计、温度、舵机命令分别使用不同 Endpoint/Service；它们可走完全相同的 Route/Path。handler 应校验业务长度/版本，再复制到应用队列，不能保存 `frame->payload` 指针供 Owner 返回后使用。

## 安全与并发

Endpoint 可以覆盖 Node 默认 Security Policy。命令/固件升级/诊断通常比传感器遥测要求更严格。注册/注销 handler 与接收分发必须由 Owner 串行化；运行中注销前先停止新流量并排空应用引用。

## 常见错误

- 把介质类型当 Link 唯一实例，导致多个 UART 共用上下文；
- `send()` 内忙等 DMA 完成，拖死 Owner 和其他路径；
- Heartbeat 到期后只删 Neighbor，不撤销依赖 Route；
- Endpoint handler 执行 Flash 写或长算法；
- 使用广播 Node ID 注册普通 Neighbor/next hop；
- 指标时间戳不在 Protocol Owner 的时钟域。
