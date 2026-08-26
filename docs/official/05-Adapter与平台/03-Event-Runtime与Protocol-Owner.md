# Event Runtime 与 Protocol Owner

> 文档级别：`NORMATIVE`
> 实现状态：`CURRENT`
> 事实源：`ucn_event_runtime.h/.c`、`ucn_protocol_owner.h/.c`
> 最近核对：`a093862`，2026-08-25

## Protocol Owner

Owner 组合 Node、Adapter Queue、可选 Service Bridge 和权威时钟，并在一个执行上下文中完成 RX pump、Bridge pump 和 `ucn_node_step()`。

## Event Runtime

Event Runtime 可静态注册最多配置上限的 Source。每个 Source 提供 `service()`，Runtime 维护 pending bit、Source budget、Round budget、调度通知和统计。

典型使用：

```text
UART ISR ─┐
CAN ISR  ─┼→ signal source → notify owner task
USB ISR  ─┘
owner wake → runtime_run → 每 Source 有界 service → node step
```

## 公平和兜底

Runtime 不能在一次唤醒中无限服务一个 Source；达到预算后保留 pending，下一 Round 继续。Owner wait 的最长时间还受下一个协议 Deadline 和最大 Step 间隔约束。

## 单 Queue 兼容 Port

各 Port 仍提供兼容 single-Queue wrapper。新的多 Bearer 产品优先使用 Event Runtime，把多个 Source 合并到同一 Owner。

## ISR 规则

ISR push 前必须配置 ISR 专用 critical callbacks；否则 `from_isr` 输入失败关闭。通知回调只唤醒，不执行 Core。

## Owner 每个周期的工作顺序

一次典型 Owner cycle 包含：

1. 读取同一权威单调时钟；
2. 按 Round/Source budget drain 已 pending 的 Source；
3. 将完整 Frame 交 Adapter/Node；
4. pump 单 Queue 兼容输入；
5. 推进可选 Service Bridge 出站；
6. 调用 `ucn_node_step(now_ms)` 处理 TX、Heartbeat、Route、Deadline；
7. 计算是否仍 pending 和下一个最晚唤醒时间；
8. 有 pending 则继续/重新调度，无 pending 才有界 wait。

顺序由实现合同冻结，产品不能同时启动第二个 Task 调用 Node step 来“提高吞吐”。

## Event Runtime 的 Source 模型

每个 Source 注册固定 ID、`service()` 回调、context 和每轮预算。ISR/Driver 只设置 pending bit 并通知 Owner。Runtime 使用 round-robin/有界 budget，避免高频 UART 永久饿死 CAN 或 USB。

Source `service()` 每次应：

- 从自己的 Ring 取有限 Carrier；
- 还原零个或多个完整 Frame；
- 通过 `ucn_event_runtime_submit_frame()` 交付；
- 尚有数据时保持 pending；
- malformed/overflow 只更新本 Source stats。

## 为什么需要最大 Step 间隔

即使没有 RX，Node 仍有 Heartbeat、Route TTL、Transfer/Policy Deadline。Owner wait 不能无限睡眠。实际等待应取：

```text
min(产品请求等待, 下一个协议Deadline剩余时间, max_step_interval)
```

ISR 通知可提前唤醒。丢通知时 max step interval 是保底，不是正常轮询频率。

## Task 与 ISR 临界区

许多 RTOS 的 ISR critical 需要保存/恢复 mask token，不能复用无 token 的 Task mutex。Port API V2 因此要求独立的 Task enter/exit 与 ISR enter/exit 成对回调。若调用 `submit_frame_from_isr()` 但没有 ISR pair，返回错误而不是退化到不安全锁。

更推荐 Driver ISR 写 BSP Ring，Owner Task 再提交 Adapter；只有 Carrier 很小且 Port 合同完整时才直接用 Runtime ISR submit。

## 背压和预算

Runtime submit 失败时 Source 必须遵守所有权合同：如果 Adapter Queue 没接管，就保留/丢弃并统计，不能复用一份已接管副本。Source budget、Round budget 和 Node TX budget 是不同层级，调参要看哪一层 pending。

## 单 Queue wrapper 的适用范围

单 UART/简单产品可以使用 `ucn_protocol_owner_rx_enqueue()` 和各 Port wrapper。多 UART/CAN/Wi-Fi 产品应使用 Event Runtime，因为一个共享 FIFO 会丢失 Source 公平和每介质独立统计，也难以在 Carrier 层有界 drain。

## 典型主循环/任务伪代码

```c
for (;;) {
    uint32_t now = product_now_ms();
    ucn_event_runtime_task_cycle(&runtime, now, &work);
    if (!ucn_event_runtime_has_pending(&runtime)) {
        product_wait_bounded(work.next_wait_ms);
    }
}
```

实际签名以公共头为准；伪代码强调一个 Owner、同一时钟和有界等待。

## 验证清单

- [ ] 两个 Source 同时满载时都有前进；
- [ ] 单 Source 达预算后仍 pending，不无限 drain；
- [ ] ISR 无合法 critical pair 时 fail-closed；
- [ ] notify 不调用 Core、不发生递归 step；
- [ ] 无 RX 时协议 Deadline 仍按时处理；
- [ ] 丢通知由最大 step 间隔恢复；
- [ ] stats 能区分 Source、Runtime、Owner 和 Node 背压。
