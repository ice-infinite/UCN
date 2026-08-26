# CAN-FD 与 Classic CAN 载体

> 文档级别：`NORMATIVE`
> 实现状态：Source/Carrier `CURRENT`；真实控制器 Driver 由产品提供
> 最近核对：`a093862`，2026-08-25

## CAN-FD

CAN-FD 可在一个物理 Frame 中承载较大 UCN Frame。Source 根据 DLC 得到物理长度，再用 UCN 前缀探测真实 encoded length。

DLC 向 12/16/20/24/32/48/64 B 取整带来的 Padding 必须全为 0。非零 Padding 计错并拒绝，避免把脏尾数据静默忽略。

## Classic CAN

8 B 数据不足以放下最小 UCN Frame，因此使用固定、有界 Carrier：START/CONTINUE、Carrier ID/Offset/Length、重组 Slot 和超时。

完成一个 Carrier 后先提交完整 UCN Frame，再消费同 CAN ID 的下一条 START，防止连续消息覆盖已完成但尚未提交的 Slot。

## Bus State

Source 暴露 Active/Warning/Passive/Bus-Off 等健康信息和统计。产品 Driver 负责读取控制器状态、执行硬件恢复并更新 Source/Link。

## 过滤与 ID

CAN ID 分配、硬件 Filter、优先级、仲裁和总线负载由产品规范。UCN Carrier 不自动替代 DroneCAN/CANopen 的标准 ID 规划。

## 多 CAN

每个 CAN 控制器/总线使用独立 Frame Ring、Carrier reassembly 和 Bus State，可同时注册为多个 Link。

## CAN-FD 的精确长度问题

CAN-FD DLC 不是任意 0～64 连续长度；超过 8 B 后映射到 12/16/20/24/32/48/64。一个真实 UCN Frame 57 B 会放进 64 B CAN-FD payload，尾部 7 B 必须由发送端清零。

接收端用 UCN 固定前缀 `peek_encoded_size()` 得到 57 B，验证剩余 7 B 全零后只提交前 57 B。若直接把 64 B 全部交给严格 Frame decode，会长度不匹配；若无条件忽略尾部，又会掩盖 Driver 脏数据/协议混淆。

## Classic CAN Carrier 状态

Classic CAN 将一个逻辑 UCN Frame 拆成 START 和若干 CONTINUE。重组槽绑定物理 CAN ID/Carrier 身份、总长、offset 和 deadline。概念流程：

```text
START(total length, carrier id, initial bytes)
CONTINUE(offset, bytes)
...
complete -> 提交精确完整UCN Frame -> 释放/重置槽
```

新 START 只有在旧槽 `active && !complete` 等合同允许时才能替换不完整事务；完整但尚未提交的槽不能被下一条 START 覆盖。完成后 Source 应停止本轮继续消费该 CAN ID，让 Owner 先提交。

## CAN ID 与 UCN Node ID 不同

CAN ID 是总线仲裁/过滤字段，UCN Node ID 在 Frame 内用于端到端寻址。产品需要冻结映射：

- 哪些 CAN ID 承载 UCN Carrier；
- 多 peer/方向/优先级如何分配；
- 硬件 filter 接收哪些 ID；
- 控制帧和业务是否使用不同仲裁优先级；
- 与现有 DroneCAN/CANopen ID 是否冲突。

不能把 CAN ID 直接当作 UCN Route 表目标而省略 Frame 地址。

## Bus-Off 与恢复

Driver 检测 Bus-Off 后应立即更新 Source/Link hard down，停止选择该 Bearer，并按控制器要求恢复。恢复完成、filter/bit timing 重新建立后再标 up，重新通过 Neighbor/HELLO 收敛；不能仅清一个错误计数就继续使用旧 Path 承诺。

Warning/Passive 可作为 Metrics/诊断，是否硬排除由规范状态门决定。

## 总线负载和优先级

Classic CAN Fragment 会产生多个仲裁帧；大 Transfer 可能显著占用总线。产品应：

- 为 Q0/维护与大数据规划 CAN ID 优先级；
- 限制 Window/并发和 Transfer 档；
- 计算最坏总线占用与 bit stuffing；
- 监控 TX queue、arbitration loss、error passive；
- 不让日志 Transfer 挤占安全控制。

## 验证清单

- [ ] CAN-FD 所有 DLC 边界和真实长度逐字节交付；
- [ ] 57→64 等 padding 全零，任一非零尾部拒绝；
- [ ] Classic START/CONTINUE 的 offset、长度、超时严格检查；
- [ ] 连续 Carrier 不覆盖 completed pending slot；
- [ ] 不同 CAN ID/控制器重组槽隔离；
- [ ] Bus-Off→Link Down→Route撤销→恢复完整实测；
- [ ] 总线利用率和 Q0 最坏时延在大 Transfer 下满足产品预算。
