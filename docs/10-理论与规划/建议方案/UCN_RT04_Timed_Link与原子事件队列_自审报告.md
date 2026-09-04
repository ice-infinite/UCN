# UCN RT-04 Timed Link 与原子事件队列自审报告

> 日期：2026-09-04
> 状态：`DONE / EXTERNAL REVIEW GO（受限实验软件范围）`

## 1. 实现结果

- 新增带 `struct_size/api_version` 的独立 Time Link Driver 扩展，未修改现有 `ucn_link_ops_t`；
- event key 固定绑定 `{link_id, direction, link_instance_generation, event_token}`，TX/RX token 使用不同命名空间；
- token 与 Link generation 只使用 `1..0x7FFFFFFF`，到顶进入 FAULT，不自然回绕；
- TX 采用“先保留唯一 key，再把完整 frame+key 原子交给 Driver”的接口；
- Link reopen 必须先调用 Driver `quiesce()`，随后 generation+1 并使旧 key 失效；
- TX timestamp event 与 Timed RX Item 分成两个固定 Ring，Timed RX Item 一次性复制 ingress Link、完整 frame、key、timestamp 与 quality；
- Task 与 ISR 使用独立临界区回调；ISR 入队没有专用回调时返回 `UCN_ERR_CONFIG`；
- Driver `reserve/submit/cancel/quiesce` 回调期间建立重入门，不能递归分配或再次推进 Link 状态。

## 2. 阶段自审整改

首轮测试及 RT-07 交叉自审后又收紧七项：

1. 配置 ISR 锁但没有任务锁时，ISR 生产者与 Owner 消费者仍不安全；现禁止该半配置；
2. `io_active` 必须在任务临界区内发布与清除，避免 Driver 回调与 ISR 之间的重入窗口；
3. TX event 现在拒绝正数或超出 UCN 错误域的 completion，Timed RX key 必须与 ingress Link ID 一致。
4. `reserve/submit/cancel/quiesce` 进入外部 Driver 前建立回调 Fence，递归 `init()` 或控制 API 不得覆盖活动对象；
5. Link reopen、提交和取消在任务临界区内发布 `io_active`，避免 quiesce 与新提交交叉；
6. 两条 Link 交叉回调时，失败的 TX 分配不会先消耗 token；
7. TX/RX Ring 增加初始化与结构合法性校验，损坏的 `head/tail/count` 不索引数组且不写 output。

## 3. 验证与资源

| 门禁 | 结果 |
| --- | --- |
| Windows GCC Full Debug，RT-01～RT-04 | 4/4 PASS |
| Windows GCC Full Release，RT-01～RT-04 | 4/4 PASS |
| reserve 回调递归分配 | `UCN_ERR_STATE`，无二次 token |
| reopen 后旧 key | 拒绝 |
| token no-wrap | `UCN_ERR_EXHAUSTED` + FAULT，输出不写回 |
| TX Ring 满 | 不覆盖，`UCN_ERR_NO_SPACE` |
| Timed RX 40 B | frame/key/timestamp 精确一致 |
| Host Timed Link / Shared Gate / TX Queue / RX Queue | 184 / 32 / 184 / 648 B |
| Release 对象 | `text=12232 B, data=0 B, bss=0 B` |

## 4. 边界

当前仍是 Host/Driver 合同层：没有任何 UART/CAN/USB/ESP-NOW BSP 接线，没有在 ISR 解码 UCN Frame，也没有把 Timed RX 混入普通 Adapter Queue。实际控制器捕获位置、缓存一致性和 ISR 延迟必须由 RT-08/09 分介质验证。

## 5. 外审 RT-A04/A05 整改

- RT-A04：Timed Link 内部新增固定 reservation 表，以完整 Event Key 为主键跟踪
  `RESERVED` 与 `SUBMITTED`。只有 live `RESERVED/TX` 可 submit；成功后转为
  `SUBMITTED`，重复 submit 和 submit 后的普通 cancel 均失败。TX completion、RX
  completion 与 Owner retire 都恰好消费一次 reservation，重复完成/退休不再触发
  Driver 或改变统计。
- RT-A05：Driver callback Fence 改为跨 Link 的控制域门禁。Link A 的任一 Driver
  callback 执行期间，Link B 的任务 TX/RX 分配和 ISR RX 分配均返回
  `UCN_ERR_STATE`，且 `next_tx_token/next_rx_token` 不变化。
- 新增 `ucn_timed_link_complete_event()` 与
  `ucn_timed_link_retire_event()`。前者用于正常完成；后者用于 Sync Owner 在超时、
  替换或切路后回收 reservation。reopen 仍必须先 quiesce。
- `-O3 -Werror` 自审额外发现 RX reservation 指针证明不足，已改为先取得并检查
  固定槽再发布；GCC Release 门禁通过。

## 6. 外审 RT-A09 整改

- 删除原先无同步的进程静态 callback-owner 指针。应用现在显式创建
  `ucn_timed_link_callback_gate_t`，同一任务/ISR/SMP 执行域内所有 Timed Link
  必须引用同一个 gate。
- gate 使用调用者提供且底层一致的 task/ISR 临界区；控制 API 在检查 active、
  修改单 Link 状态和建立回调标记期间遵循 `gate → Link` 锁顺序。调用 Driver 时
  释放锁但保持受锁保护的 active 标记，其他 Link/ISR 获取 gate 后会失败关闭。
- gate 只能在并发开始前初始化；任一 Link 引用后不得重新初始化或移动。若 gate
  与 Link 锁复用同一物理原语，调用者必须保证该原语支持固定嵌套。
- 新增 POSIX 双线程、双 Link、不同 per-Link 锁回归：Link A Driver callback 阻塞
  时，Link B 的 ISR 与任务分配均返回 `UCN_ERR_STATE`，对象不变。WSL GCC TSan
  使用 `-fno-pie/-no-pie` 独立构建，并以 `setarch x86_64 -R` 关闭进程 ASLR 后运行，
  无 data-race 报告；默认 ASLR 下当前 WSL 会触发 TSan runtime 的
  `unexpected memory mapping`，未把该环境故障当作通过。
