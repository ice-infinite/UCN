# 时间参数、维护周期与 Deadline 预算

> 文档级别：`REFERENCE`
> 实现状态：`CURRENT`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：ucn_config.h、ucn_profile.h、CMake 与资源门禁
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：Host 资源可用；目标 MCU 资源需按产品配置实测

## 默认示例

| 参数 | 默认值 |
| --- | ---: |
| 最大 Owner step 间隔 | 10 ms |
| Q0 背压重试 | 5 ms |
| Heartbeat | 1000 ms |
| Bearer quality sample | 500 ms |
| RREQ 最小间隔 | 100 ms |
| Route request timeout | 1000 ms |
| Route refresh 最小间隔 | 5000 ms |
| Transfer ACK timeout | 250 ms |
| Transfer RX timeout | 5000 ms |
| Cluster Advertise | 1000 ms |
| Cluster Keepalive | 2000 ms |

快速 liveness profile 可使用更短周期，但必须整体调整 suspect/remove、链路带宽和 Owner 预算，不能只缩短 heartbeat。

### 参数之间的约束

通常需要满足：

```text
owner_step_max << heartbeat_interval < suspect_timeout < remove_timeout
probe_interval × required_attempts < switch_hold_or_route_timeout
ack_timeout > worst_case_queue + serialization + hops + processing
rx_timeout > one_transfer_worst_case_with_retries
```

这不是固定数学常数，而是调参时必须证明的偏序关系。若 `suspect_timeout` 只略大于 Heartbeat，单次调度抖动就会误判；若 ACK timeout 小于一帧在慢速多跳链路上的序列化时间，重传会放大拥塞。

## 时间代数

所有 duration 必须小于等于 `INT32_MAX`，deadline 使用回绕安全比较。`deadline == 0` 只在合同明确时表示未武装；创建 deadline 失败必须原子拒绝，不能留下永不超时事务。

推荐统一使用库提供的 duration/deadline helper，不直接写 `now >= deadline`。32-bit 毫秒计数约 49.7 天回绕，合法 duration 限制在半范围内后，差值比较才能区分“已过期”和“还未到”。

创建事务时应先验证 duration，再计算 deadline，最后一次性写入对象。不能先改变 phase、再因 deadline 无法建立而返回错误。

## 预算方法

端到端时延由排队、序列化、每跳处理、重传和应用调度共同构成。维护周期只负责状态新鲜度，数据队列有数据时由事件/Owner 立即推进，不需要等待 Heartbeat。

### 端到端估算

```text
T_end_to_end
  = T_source_schedule
  + T_queue
  + Σ(T_encode + T_serialization + T_medium + T_rx_owner)
  + T_retry
  + T_destination_dispatch
```

例如 3 Mbit/s UART 的理论字节率还要考虑 8N1 每字节 10 bit、COBS/帧头、Owner 批次、ACK/窗口和中间节点收发串行化；不能仅用 3,000,000/8 作为业务吞吐。

### 数据即时发送与维护定时器

队列从空变为非空时应通知 Owner；Owner 获得运行机会后立即 drain 有界批次。10 ms 最大 step 间隔是保底/维护合同，不表示每条数据固定等待 10 ms，更不表示等待 1 s Heartbeat。中断只收数据并通知，协议解析、路由和发送通常在 Owner 上下文完成。

### 调参证据

记录 p50/p95/p99 延迟、最大连续抖动、timeout/retry 次数、CPU 占用和控制带宽。只展示平均值会掩盖实时控制最关心的尾延迟。
