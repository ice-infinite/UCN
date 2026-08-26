# NuttX Port

> 文档级别：`NORMATIVE GUIDE`
> 实现状态：通用 wrapper `CURRENT`；具体 NuttX primitive/Board glue 由产品提供
> 最近核对：`a093862`，2026-08-25

NuttX Port 提供独立 worker wrapper，不选择 semaphore、poll、work queue 或 task 方案。产品将 notify/wait 映射到当前 NuttX 配置允许的静态同步原语。

推荐保持：

- 一个 UCN worker 是唯一 Node Owner；
- Driver ISR/回调只写 Source Ring 并通知；
- worker wait 受协议 Deadline 限制；
- 多 Bearer 使用 Event Runtime；
- 业务应用通过 Service/Endpoint 交互。

`ucn_nuttx_port_worker_step()` 和 `worker_wait()` 只包装公共 Owner 合同。Board bring-up、UART/CAN character driver、work queue priority、stack、SMP affinity 和电源管理由产品验证。

当前仓库不是 NuttX apps tree，也没有替产品选择 `CONFIG_*`。

## 两种常见接入形态

1. **独立 pthread/task**：Driver/ISR 通知专用 UCN worker，最接近通用 Owner 模型；
2. **专用 work queue**：只有在 work queue 优先级、不可阻塞和 Deadline 上界可证明时使用，避免与慢文件系统/网络工作共享队列。

无论选择哪种，Node 只能有一个 Owner。

## notify/wait 的实现责任

产品可选 semaphore、eventfd/poll、message queue 等当前 NuttX 配置支持的静态原语。`notify_protocol_worker(context, from_isr)` 必须区分 ISR-safe 路径；`wait_for_work(max_wait_ms)` 最多等待传入时长，并正确处理 EINTR/超时。

不要把 POSIX API 可用等同于允许在 ISR 调用。具体 primitive 的中断上下文合同要按目标 NuttX 版本核对。

## Driver 接入

- UART character driver read/poll 可由独立接收回调/线程写 Stream Source；
- 低层 ISR 路径只写 Ring；
- CAN socket/character/下层 callback 选择取决于板级配置，但最终都交 CAN Source；
- Driver reopen、device removal、Bus-Off 应映射 Link Down；
- 不在 UCN worker 中做无限阻塞 `read/write`。

## 时间、信号和电源

`now_ms` 使用单调 clock，不能使用会被校时跳变的 wall clock。`worker_wait()` 要正确处理 signal 提前返回并重新检查 pending/deadline。进入低功耗前保留 timer/driver 唤醒条件。

## NuttX apps/Board 边界

产品需自行提供：

- `Kconfig/Make.defs/CMakeLists` 或当前 apps 构建 glue；
- board pinmux/clock/UART/CAN bring-up；
- worker priority/stack；
- `CONFIG_*` 驱动、poll、SMP、workqueue 配置；
- shell/日志 UART 与 UCN 通道隔离。

UCN wrapper 本身不会把这些文件生成出来。

## 验证清单

- [ ] 选定 primitive 在 task/ISR 上下文均合法；
- [ ] EINTR/timeout 不会丢失 protocol step；
- [ ] worker 不执行阻塞 Driver I/O；
- [ ] SMP 下共享 Ring/critical 正确；
- [ ] NuttX target map、stack、CPU 和真实 UART/CAN 已测；
- [ ] apps/board 配置与官方通用 wrapper 的边界在产品文档中写清。
