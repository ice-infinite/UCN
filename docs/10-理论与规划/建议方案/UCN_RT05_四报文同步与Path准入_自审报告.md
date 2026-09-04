# UCN RT-05 四报文同步与 Path 准入自审报告

> 日期：2026-09-04
> 状态：`DONE / EXTERNAL REVIEW GO（受限实验软件范围）`
> 范围：实验控制 Payload 与 Host FSM，尚未接生产 Endpoint

## 1. 实现结果

- 实现实验 Endpoint `0xBC..0xBF` 对应的 `SYNC / FOLLOW_UP / DELAY_REQ / DELAY_RESP`；
- 角色 Payload 固定为 31/19/11/19 B，严格携带 Version、Domain、generation、sequence；只有 SYNC 建立完整 `WireTimeTxnKey` 并回显 reverse Path；
- 外层认证上下文提供当前方向的 Source、Session、Destination 和 Path；未通过 E2E 的控制报文不能形成可交给 FSM 的 authenticated message；
- Master 最多保存 4 个 Peer pending，Member 只保存 1 个 pending，全部调用者拥有、无动态内存；
- T1/T4 只在 Master 保存，T2/T3 只在 Member 保存；本地 event key 从不上 Wire；
- 每个公开推进入口都先执行当前 `now_us` 的 deadline preflight，exact deadline 即失效；
- 四时间戳计算产生 `mean_path_delay` 与“Domain-master time - Member local time”的 offset；所有差值、和、符号与 uncertainty 相加均检查溢出；
- 固定且事务内不可变、具有可信 asymmetry 上界的 Path 产生有效样本；缺上界的固定 Path 只产生诊断样本；普通动态 Route 不创建 pending；
- 新 SYNC、完全重复、旧 sequence、乱序、迟到 event 和超时均有固定处理。

## 2. 阶段自审整改

首轮测试后自审修正：

1. 无效 role 加零长度可能绕过长度判断并读取 `payload[0]`，现先验证 role 的合法长度；
2. 所有中间推进 API 增加 `now_us`，不再依赖调用者“记得先 step”；
3. 接收 FSM 要求消息来自严格 Codec 的 authenticated outer；直接伪造语义对象不能推进；
4. `local_sample_us` 必须位于 Member 的 T2～T3 区间，不能传入任意时间；
5. Master begin 和 Member receive SYNC 也先清理到期 pending，避免过期槽继续占用或被重放延长；
6. 超时回归改用真实迟到 FOLLOW_UP，并加入完整 31 B SYNC Golden Vector 与容量失败零写回。

## 3. 验证与资源

| 门禁 | 结果 |
| --- | --- |
| Windows GCC Full Debug，RT-01～RT-05 | 5/5 PASS |
| Windows GCC Full Release，RT-01～RT-05 | 5/5 PASS |
| 四报文 Wire→Typed→FSM 完整流程 | PASS |
| 100 us 对称延迟、Member 快 1000 us | delay=100 us，offset=-1000 us |
| 有 asymmetry / 无 asymmetry / dynamic Route | 有效样本 / 仅诊断 / 零 pending |
| 相同 SYNC 重放 | 幂等且不延长 deadline |
| exact deadline 后迟到 FOLLOW_UP/T1 | 拒绝 |
| Host Master/Member/TxnKey | 584 / 240 / 60 B |
| Release 对象 | `text=16452 B, data=0 B, bss=0 B` |

## 4. 边界

这套 Codec/FSM 仍是 `EXCLUDE_FROM_ALL` 实验 archive。它没有修改 Core Message Type、Frame Header、生产 Service Binding 或 Q1 队列，也未宣称真实硬件 timestamp 精度。RT-06 继续补 capability、Session lease 和 STATIC_MASTER 持久化启动门；RT-08/09 才接物理 Driver。

## 5. 外审 RT-A02/A06 整改

- RT-A02：Member 配置不再接受一个笼统的 `timestamp_uncertainty_us`，而是显式
  保存 timer resolution、Link capture、filter residual、rounding 四个固定分量和
  known mask；每个有效样本再与本事务 Path asymmetry checked-add。任一 unknown
  只能形成 diagnostic sample，不能形成有效同步样本。
- RT-A06：Master/Member Owner 各自增加固定 release-obligation 队列。超时、较高
  sequence 替换和显式 Abort 遇到尚未完成的本地 T1/T3 时，先把完整 Event Key
  放入 release 队列，再清 pending；队列没有空间时保持原事务并失败关闭。
- Owner 通过 `ucn_time_sync_*_peek_released_event()` 查看 obligation，调用
  `ucn_timed_link_retire_event()` 完成 Driver reservation 取消/退休，再以匹配的
  `*_ack_released_event()` 移除队首。正常 timestamp completion 则先
  `ucn_timed_link_complete_event()`，再把时间交给 Sync FSM。
- 定向测试覆盖 Master T1 超时、Member T3 替换/超时/Abort、Master Abort、队列
  取尽后的无写回，以及 release 后真实 Timed Link cancel。
- 第二轮全体自审把新事务 deadline 的 checked-add 提前到 replacement release 前；
  `now_us + timeout` 溢出时旧 pending 与 release 队列逐字节不变，不会形成同一 T3
  key 的双重所有权。

## 6. 外审 RT-A10 整改

- 原 destructive take API 已删除，release obligation 改为两阶段确认。peek 不改变
  队列；ack 必须精确匹配当前队首 Event Key，错误 key 返回 `UCN_ERR_STATE` 且整个
  Master/Member 对象逐字节不变。
- RT-07 Fake Driver 增加一次性 cancel 失败注入。首次 retire 返回
  `UCN_ERR_LINK_DOWN` 时 release count 和队首 key 保持；下一轮再次 peek 同一 key，
  Driver 成功退休后才 ack/pop。
- 该规则区分“事务 pending 已清理”和“硬件 timestamp reservation 已释放”两个完成
  层级，确保暂态 Driver 故障不会永久丢失回收责任。
