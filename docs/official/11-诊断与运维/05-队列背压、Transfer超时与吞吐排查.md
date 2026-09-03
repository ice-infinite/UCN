# 队列背压、Transfer 超时与吞吐排查

> 文档级别：`GUIDE`
> 实现状态：`CURRENT（诊断方法）`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：状态视图、统计 API、测试与现有实测记录
> 最近核对：当前工作区，2026-09-02
> 硬件状态：部分实测；未测项不得推断

先定位哪一层满：驱动/DMA ring、Source carrier、Adapter Queue、Node pending、Transfer window 或应用 completion handle。队列满表示生产速度超过消费预算，不等于 Wi-Fi/UART 本身很慢。

Transfer 吞吐排查同时记录 MTU、fragment 数、窗口、ACK RTT、重传、每跳序列化和 Owner step 间隔。接收 handle 未 release 会表现为持续无槽。

## 队列链路图

```text
应用产生
 → Service remote/Q0-Q3
 → Node Q0-Q3/pending
 → Link TX queue/DMA
 → 物理链路
 → Driver RX ring
 → Source carrier/reassembly
 → Adapter RX queue
 → Node receive
 → Transfer RX slot/Service inbox
 → 应用消费
```

每一层都要有depth、current/peak、enqueue/reject/dequeue计数。只看到Node `NO_SPACE`时，先确认它是Node队列、Link callback还是Bridge返回的错误。

## 背压语义

- Q0 `RETRY_ON_BACKPRESSURE`保留本地 FIFO 所有权：Link queue `NO_SPACE`按固定次数有界重试；Full/Lite 的可恢复 route gap 在原 Deadline 内合并发现并等待路由。两组统计必须分开，且都不是远端可靠送达；
- Q1 Latest可覆盖旧值，适合实时状态；
- Q2 Normal、Q3 Bulk 都是独立固定 FIFO，满载返回背压；四级持续满载时 Node/Service Remote TX 的目标比例为 `6:3:2:1`；
- Transfer有自己的窗口/ACK，不应靠无限扩大Node queue；
- Service Q0满应让生产任务感知，不能覆盖未执行命令。

## 快速定位方法

1. 暂停业务生产，看积压是否能排空；
2. 比较相邻层的累计in/out差值；
3. 查Owner wake/step是否满足10ms保底；
4. 关闭日志，排除printf阻塞；
5. 单跳/单Bearer复现，再逐步增加中继与流量。

Node Queue 必须用同一漏斗比较 `tx_enqueued_by_class[]`、`tx_scheduled_by_class[]` 和 `tx_queue_sent_by_class[]`：入队数高但调度数低说明消息仍在四级仲裁前等待；调度数高但 Queue 成功发送数低，再结合 `tx_expired_dropped`、Q0 route-wait/backpressure 与错误统计定位 Route/Link/Deadline。`tx_sent_by_class[]` 是所有成功 Link 提交总量，还包含即时发送、转发和 Transfer/控制路径，不能直接与 Queue 的 enqueued/scheduled 做差。Service 另看 `q2_inbox_full`、`q3_inbox_full`、`remote_q2_full`、`remote_q3_full` 和 `remote_tx_reads_by_class[]`，不要把 Service 满载误判为物理 Wi-Fi 或 UART 限速。

如果源端 `accepted` 大于目标端普通 Q0 `rx`，先确认是否错误地把“本地队列接纳”当成端到端
完成。route-wait 只能防止 Core 在可恢复的无路由窗口提前释放本地项；帧一旦被 Link 接受，
普通 Q0 没有远端 ACK，也不会像 Transfer 一样根据丢失自动重传。必须逐条送达时查看 Transfer
completion；必须确认远端执行时查看 Result Endpoint。

## Transfer timeout

发送accepted后仍可能：首次Fragment无法发送、ACK超时、重试耗尽、远端NO_SLOT、RX重组timeout。分别查看`fragments_sent/retried`、ACK received、remote status、RX slot full/expired和completion。

如果启动后第一次send立即timeout，检查now_ms是否来自强制callback而非init时缓存；当前API在实际推进建立deadline，产品仍需保证时钟一致。

## 吞吐损耗拆分

```text
raw bitrate
 - UART 8N1/CAN arbitration/无线MAC
 - Carrier(COBS/Classic CAN)
 - Core header/tag
 - Transfer 14B fragment头和ACK
 - 每跳store-and-forward
 - Owner/queue等待
 - retry
 = payload goodput
```

中继损耗大时检查是否每片Stop-and-Wait、同一UART收发串行、窗口未协商、每step只发一片和中间节点queue预算。不能把全部损耗归因于“协议头大”。

## 调优顺序

先修Driver DMA/中断和Owner调度，再增加合适window，再考虑并发slots；始终监测Q0 p99。大窗口在高丢包链路可能更差，多并发T8K会占满RAM/队列。

## 结果表

按Class、payload、hop、MTU、window、concurrency列出B/s、fragment/ACK、retry、loss、p50/p99、CPU、queue peak。这样才能比较优化前后，不只报告一个最高速度。
