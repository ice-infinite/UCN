# Service Router、Binding 与 Inbox

> 文档级别：`NORMATIVE`
> 实现状态：`CURRENT`；`UCN_FEATURE_SERVICE` 可独立关闭
> 事实源：`ucn_service.h/.c`、Service tests
> 最近核对：当前工作区，2026-09-02

## Service 是什么

Service 把节点内任务视为 Endpoint 的本地 owner，使同一套消息语义既能在 MCU 内部传递，也能经 UCN 发到另一 MCU。

Service 不把每个任务伪装成网络 Node。Node ID 仍代表协议节点；Service ID 和 Endpoint 表示节点内部责任。

## Binding

Binding 冻结：

- Endpoint；
- owner Service ID；
- 最大 Payload；
- 允许的单一 Q0～Q3 Class；
- 本地 Source ACL；
- 是否接受远端；
- Q0/Q2/Q3 FIFO 或 Q1 Latest；
- 远端 Q0 是否要求 Validator。

Binding table 是借用的 immutable 配置，整个 Router 生命周期保持有效且不修改。

## Inbox

- Q0：固定 FIFO，满时明确 NO_SPACE；
- Q1：每 Binding 一个 Latest Slot，新值覆盖未消费旧值；
- Q2：固定 Normal FIFO，满时明确 NO_SPACE；
- Q3：固定 Bulk FIFO，满时明确 NO_SPACE；
- Router 成功投递后拥有 Payload 固定副本，调用者可复用输入。

## Ready 生命周期

任务启动后显式设 ready。设为 false 会清空该 Binding 的旧 Inbox，防止重启任务消费停机前残留命令。

调用者/Port 必须串行 Router 访问；Service 本身不创建 RTOS Queue/Mutex/Task。

## 接受状态

`LOCAL_DELIVERED` 表示进入本机 Inbox；`REMOTE_ENQUEUED` 表示进入远端发送固定队列。二者都是本地所有权结果，不是端到端业务确认。

## 为什么 Service 不等于 Node

Node ID 是网络可路由身份。如果每个 RTOS Task 都伪装成 Node，同一 MCU 内任务通信也要维护 Neighbor/Route/Session，并显著扩大地址和状态表。Service 保留任务级寻址，但共享一个真实 Node 的网络身份。

典型映射：

```text
Node 10
├─ Service 1: IMU producer, owns Endpoint 0x40
├─ Service 2: Servo controller, owns Endpoint 0x60
└─ Service 3: Parameter manager, owns Endpoint 0x70
```

远端只路由到 Node 10，到达后由 Endpoint/Binding 分发到正确任务。

## Router 初始化和 Binding 校验

`ucn_service_router_init()` 在写入有效 Router 前检查：

- local Node ID 合法；
- Binding 数不超过编译上限；
- Endpoint 位于静态业务范围且不重复；
- Service ID、Payload、Traffic Mask、Delivery Mode 合法；
- Q0/Q1/Q2/Q3 Binding 数分别不超过对应固定 Inbox/Slot 数；
- 使用 `uint8_t` 计数或索引的 Binding、Remote Queue 和 Inbox 深度都在 `1..255`；
- ACL/远端 Q0 Validator 要求可满足。

Binding 表是借用的 immutable 存储。应用若把它放在栈上，函数返回后 Router 就会悬空；正确做法是 `static const` 或寿命覆盖 Router 的产品配置对象。

## 本机发送完整流程

任务 A 调用 `ucn_service_send_ex()`，目标 Node 等于本机：

1. 查找 Endpoint Binding；
2. 检查源 Service ACL、Traffic Class、Payload 长度和目标 Ready；
3. Q0/Q2/Q3 复制进各自目标 FIFO，满则 NO_SPACE；或 Q1 覆盖 Latest；
4. Router 成为 Payload 固定副本所有者；
5. 返回 `LOCAL_DELIVERED`；
6. 目标任务被 Port/产品事件唤醒，调用 `ucn_service_inbox_take()`；
7. 取出后由任务验证并执行。

步骤 5 和步骤 7 不是同一时刻，所以本地投递成功也不能自动等于业务执行成功。

## 远端发送完整流程

目标 Node 不等于本机时，Router 不调用 Link：

1. 完成相同 Binding/ACL/长度检查；
2. Q0/Q2/Q3 进入各自固定 Remote FIFO，Q1 进入 Remote Latest Slot；
3. 返回 `REMOTE_ENQUEUED`；
4. 后续 Bridge/Protocol Owner 从 Remote Queue 取消息并调用 Node；
5. 每一后续阶段可能独立失败并产生事件/统计。

这种分层使业务任务不需要持有 Node/Link 锁，也让 RTOS Port 可以统一通知 Protocol Owner。

## Ready=false 为什么要清 Inbox

假设舵机任务崩溃，旧队列里有“转到 90°”。任务重启后设备状态可能已经改变；若继续消费旧命令，可能产生危险动作。`ready=false` 清理旧 Inbox，要求新实例只接收重新验证的新消息。

Ready 不是网络可达状态，也不是权限。它只表示当前本地 Owner 是否准备接管新业务。

## Q0～Q3 的容量与公平语义

- Q0 每条消息占一个完整固定 `ucn_service_message_t`；增加深度会按最大 Payload 成倍增加 RAM；
- Q1 每 Binding 一个 Latest Slot，内存固定且不因产生速率增长；
- Q2/Q3 各自使用独立 FIFO，满时不覆盖、不借用 Q0/Q1 的槽；
- Remote Q0～Q3 与本地 Inbox 是不同容量，不能只调一个宏；
- Remote TX 在 Class 之间按固定 12 槽 `6:3:2:1` 调度；同一 Q1 Class 内从上次成功槽的下一槽开始扫描，热点 Latest key 持续更新也不能让其他 Q1 key 永久饥饿；
- 当前内部索引与计数是 `uint8_t`，所有对应配置的最大合法深度是 255，256 会在编译配置阶段失败；
- stats 中的 full/overwrite 是调优证据，不应静默清零。

## 调用上下文

Service Router 不内置 Mutex/RTOS Queue。所有访问必须由产品串行化：常见做法是业务任务通过 Port 固定请求队列提交，由 Owner/Router Task 统一调用；或在裸机主循环中禁止 ISR 直接操作 Router。

ISR 应只写驱动/请求 Ring 并通知，不能在 Handler 中直接 `ucn_service_send()` 后又和任务并发 `inbox_take()`。

## 失败处理

| 失败 | 状态是否改变 | 调用方动作 |
| --- | --- | --- |
| 未知 Endpoint | 不写 Inbox | 修正 ABI/统计 |
| ACL/Traffic 拒绝 | 不写 Inbox | 不自动降级 |
| Not Ready | 不写 Inbox | 等任务 ready 或报告失败 |
| Payload 过长 | 不写 Inbox | 用更小 Payload/Transfer |
| Q0/Q2/Q3 满 | 不覆盖旧消息 | 按 Class 语义有界重试或业务失败 |
| Q1 已占用 | 覆盖旧样本 | 正常 Latest 语义并计数 |

## 验证清单

- [ ] 重复 Endpoint/非法 Binding 初始化原子失败；
- [ ] Binding 配置寿命覆盖 Router；
- [ ] 本机 Fast Path 不进入 Frame/Route/Link；
- [ ] Q0/Q2/Q3 满不覆盖，Q1 覆盖只发生在 Latest；
- [ ] 多个 Q1 key 在热点持续更新时仍按 slot round-robin 有界获得发送机会；
- [ ] Service 深度 255 可编译，256 被编译期合同拒绝；
- [ ] ready=false 清除旧消息，重启后不可消费；
- [ ] acceptance 只描述本地所有权，不被报告为远端执行。
