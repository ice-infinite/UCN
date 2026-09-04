# UCN RT-07 多域漂移与故障集成模拟自审报告

> 日期：2026-09-04
> 状态：`DONE / EXTERNAL REVIEW GO（受限实验软件范围）`
> 范围：确定性 Host 软件模拟；不是 ESP32、CAN、USB 或真实时钟精度结论

## 1. 集成范围

RT-07 把此前保持物理隔离的六个可选组件显式链接到一个测试程序中：

```text
Metadata v1 Codec
    -> Endpoint Policy / 双新鲜度门禁
    -> Time Domain FSM
    -> Timed Link 事件键与原子队列
    -> 四报文 Time Sync
    -> Capability Lease
    -> STATIC_MASTER Authority generation
```

模拟器建立两个互相独立的 Time Domain。每个 Domain 包含一个 Master 和
四个 Member，共 10 个逻辑节点。每个 Member 都持有独立的同步事务、
Time Domain、固定 Path Contract 和 Capability Lease；两个 Master 分别通过
独立 fake persistence Provider 完成 generation 发布。同步控制消息必须先经过
实际 RT-05 payload encode/decode，再交给事务状态机，不能在测试中直接填写
T1～T4 来绕过 Wire 语义。

## 2. 漂移、丢包与乱序矩阵

测试为八个 Member 设置方向不同、数值不同的本地时钟漂移，并连续执行五轮
同步。故障注入包括：

- 丢弃一帧 `FOLLOW_UP`，并让该事务按 deadline 清理；
- 重复注入已经消费的 `DELAY_RESP`；
- 在缺少 `FOLLOW_UP` 时提前注入 `DELAY_RESP`；
- 把 Domain 2 的合法 `SYNC` 投递给 Domain 1 Member；
- 在故障后继续下一轮，验证旧事务不污染新 sequence。

确定性结果为：39 个有效同步样本、8/8 Member 最终进入 `LOCKED`，模型内
最坏换算误差 40 us。该 40 us 只描述测试时钟模型与整数换算实现，不包含
真实晶振、ISR、驱动、DMA、收发器或物理链路误差，不能作为产品精度指标。

## 3. Path 与诊断样本边界

RT-07 同时建立以下三个反例：

1. 固定且事务期间不可变，并具有可信非对称上界的 Path，才产生有效样本；
2. 固定 Path 缺少可信非对称上界时，只能产生 diagnostic sample，Domain 的
   有效样本数不增加，也不能进入 `LOCKED`；
3. ordinary dynamic Route 即使 Requirement 为 `PREFERRED`，也不分配 pending、
   不发送四报文，调用方只能回退到 `LOCAL_STAMP/NONE`。

这证明“允许诊断事务发生”与“允许时间样本影响业务时间”是两条不同门禁。

## 4. 重启、切路和租约故障

集成模拟覆盖以下身份生命周期：

- Master Session 从旧启动值切换到新启动值；
- authority witness 从 generation 1 单调推进到 generation 2；
- 新 generation 尚未完成双重 reload 证明时不发布同步；
- 旧 Session、旧 generation、旧 Path 的控制帧与 Capability Lease 均被拒绝；
- 显式失效旧租约、Timed Link `reopen()`、安装新双向 Path 后才能重新同步；
- Capability Lease 到达精确 deadline 后不能继续准入；
- `HOLDOVER` 超时进入 `UNSYNCED`，不继续生成同步时间 Envelope。

还验证了 Master 默认四个 pending 槽的容量边界：四个并发对端可建立事务，
第五个对端在没有空槽时失败关闭，不覆盖活跃事务。

## 5. 阶段交叉自审整改

在 RT-07 集成过程中，不只检查模拟器本身，还反向审查了 RT-03、RT-04、
RT-06 的公共边界，并关闭以下问题：

1. 修正 Capability Session 不匹配测试夹具，使租约自身合法、只在准入身份上
   不匹配，避免“用非法输入证明拒绝”的弱测试；
2. Capability cache 安装时可惰性回收已经到期的槽，但 live 槽仍禁止隐式
   淘汰；失败前不修改原缓存；
3. Timed Link 的 Driver 回调门在调用外部代码前建立，递归 init/allocate/
   submit/cancel/reopen 均失败关闭；
4. 两条 Timed Link 发生交叉回调时，失败的 TX 分配不会先消耗 event token；
5. Link reopen 与 submit/cancel 的状态切换放在任务临界区内，避免 callback
   quiesce 与新提交交叉；
6. TX/RX Ring 增加初始化与结构合法性校验，损坏的 count/head/tail 不索引
   数组、不写调用方 output；
7. Time Domain 每个公共入口先验证完整状态对象，损坏的 sample cursor、phase
   或 canonical 字段不能造成越界或继续换算；
8. 增加跨 Domain 合法报文注入，验证不仅坏帧会拒绝，身份正确但 Domain
   错误的帧也必须 `UCN_ERR_REPLAY` 且对象逐字节不变。

上述整改均有对应的定向回归，不依赖随机 fuzz 碰撞。

## 6. 阶段验证结果

| 项目 | 结果 |
| --- | --- |
| 逻辑节点 / Time Domain | 10 / 2 |
| 每域 Master / Member | 1 / 4 |
| 有效样本 | 39 |
| 最终 LOCKED | 8/8 Member |
| 模型最坏换算误差 | 40 us |
| 丢包 / 重复 / 乱序 / 跨域 | 全部按预期拒绝或恢复 |
| diagnostic-only Path | 不增加有效样本、不进入 LOCKED |
| dynamic Route | 零 pending、零同步事务 |
| Session/generation/Path 重绑 | 旧身份拒绝，新身份恢复 |
| Host 单 Domain 模拟对象 | 10720 B |
| Host Member 集成对象 | 2152 B |

## 7. 尚未放行的边界

- 所有计时值都来自确定性软件模型，不是物理硬件时间戳；
- 没有 UART、CAN、CAN-FD、USB、Wi-Fi 或 ESP-NOW BSP 接线；
- 没有验证真实 Flash 双槽、eFuse/OTP witness、掉电撕裂写或 anti-rollback；
- 没有把 Realtime 模块接入生产 Node、Service、Adapter RX/TX；
- 未执行 RT-10 Hop-aware Wire v2，也没有把每跳排队时间加入 Envelope；
- 默认产品仍不链接这些 `EXCLUDE_FROM_ALL` archive。

因此 RT-07 的阶段结论是“Host 集成状态机和失败边界自审通过”，不是实时功能
已经可投入生产。RT-01～RT-07 的全体构建矩阵、静态分析、资源与隔离性自审
已经完成，下一步为外部审计。

## 8. 外审 RT-A07 整改

外审确认旧模拟器虽然链接了 RT-04 archive，但实际用测试 helper 直接构造 T1～T4
Event Key，可执行文件中 Timed Link 符号为零，因而不能证明事件生命周期闭环。
整改后的每轮交换执行以下真实路径：

1. TX 由 `ucn_timed_link_allocate_event()` 保留 key，经
   `ucn_timed_link_submit()` 进入 fake Driver，再从 ISR 语义的 TX completion Ring
   取回并调用 `ucn_timed_link_complete_event()`；
2. RX 由 `ucn_timed_link_allocate_rx_event_from_isr()` 分配 key，把完整
   `{frame,key,timestamp}` 原子写入 Timed RX Ring，Owner 取出并完成 reservation；
3. 丢包、超时和显式 Path switch 从 RT-05 release 队列取回 key，调用
   `ucn_timed_link_retire_event()`；reopen 后验证旧 generation key 失效；
4. 模拟可执行文件现检出 10 个 `ucn_timed_link_*` 符号，旧 `event_key()` 伪造 helper
   已删除。

这使 RT-07 可以称为 RT-01～RT-07 的 Host 软件链路集成；仍不能称作真实 ISR、
DMA、控制器时间戳或产品实时性验证。

## 9. 外审 RT-A10 故障重试整改

集成 Fake Driver 新增可确定注入的 `cancel_failures_remaining`。容量测试在四个
Master release obligation 均存在时令第一次 Driver cancel 返回
`UCN_ERR_LINK_DOWN`，然后验证：

1. release count 不减少；
2. 第二次 peek 得到与第一次逐字段相同的 Event Key；
3. 后续 retire 成功后才调用 exact ack；
4. 全部 obligation 最终清空，Timed Link reservation 不泄漏。

因此模拟 Owner 不再因一次暂态 Driver 错误遗失回收责任。
