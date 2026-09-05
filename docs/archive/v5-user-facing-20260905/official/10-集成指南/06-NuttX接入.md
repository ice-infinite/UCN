# NuttX 接入

> 文档级别：`GUIDE`
> 实现状态：`PARTIAL（通用合同已实现，产品 BSP/SDK 需接入）`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：公共 API、Port/Adapter/Source 实现与测试
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：部分 ESP32-S3 UART/ESP-NOW；其他平台按正文标注

NuttX 使用专用 pthread/task 作为 Owner，驱动 poll/IRQ callback 将数据放入 ring，并用 semaphore/mqueue 唤醒。时钟选单调 clock，避免墙钟调整破坏 deadline。

板级 UART/CAN/USB 设备路径和 ioctl 属于产品 glue。若驱动只提供文件描述符，可在 Owner 中以非阻塞 read 作为 Source 输入；仍要限制单轮读取预算。

## 推荐映射

- Owner：专用 `task_create()`/`pthread_create()`；
- 唤醒：`sem_post`/`nxsem_post`、mqueue 或 pollable event；
- 时间：`clock_gettime(CLOCK_MONOTONIC)` 转换到 32-bit ms；
- 串口/USB：`O_NONBLOCK` fd + poll/read → Stream Source；
- CAN：SocketCAN-like/字符设备读取 → CAN Source；
- 临界区：仅在小型共享 ring 元数据上使用 NuttX irq-safe primitive。

## Owner 循环

Owner 可在 `poll()` 中同时等待 UART/CAN fd 和内部 wake fd/semaphore，timeout 不超过最大 step 间隔。每个 fd 每轮读取有限 chunk，随后执行 Runtime/Node step；不要让一个持续可读 fd 形成无限循环。

```text
poll(timeout)
  → 每个 ready fd 最多读 budget
  → Source/Adapter submit
  → Node/Service/Transfer/Cluster step
  → 若 work remaining，零等待继续一轮
```

## 配置与构建

UCN 库和 NuttX app 必须使用相同产品头/Profile。将 Port、Source、Driver glue 放在应用或 board 层，不改 vendor OS tree；可用独立 static library 接入。

## 信号/取消与持久化

线程取消、signal handler 不能直接操作 Node。停机请求写标志并唤醒 Owner，由 Owner 有序关闭。Security counter/Cluster Record 可使用 MTD/文件，但 Provider 必须实现原子双槽/同步语义；普通 `write()` 成功不等于掉电 durable，需依据 FS/MTD 合同。

## 验收

覆盖 fd 短读/EAGAIN、poll timeout、设备热拔、任务取消、文件系统失败、优先级反转和长稳。记录 task stack、mallinfo/heap、CPU 和 driver buffer 峰值。
