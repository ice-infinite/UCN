# RAM、Flash、Stack 与动态分配边界

> 文档级别：`REFERENCE`
> 实现状态：`CURRENT`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：ucn_config.h、ucn_profile.h、CMake 与资源门禁
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：Host 资源可用；目标 MCU 资源需按产品配置实测

## 分别测量

- RAM：公共对象、静态表、队列、RX/TX buffer、RTOS task stack；
- Flash/Text：实际链接进产品的 archive 与优化级别；
- Stack：最大静态 frame 和运行时 high-water；
- Heap：UCN 核心目标是零动态分配，但 BSP/SDK/Wi-Fi 栈可能使用 heap。

四项不能互相替代。静态对象进入 `.bss` 会占 RAM 但不一定增加镜像文件同等字节；`const` 表通常占 Flash；函数局部数组影响 stack；驱动 DMA descriptor、Wi-Fi buffer 或 RTOS queue 可能来自 SDK heap，不能归入“UCN 核心零堆”。

### RAM 账目建议

```text
total_product_ram
  = .data + .bss
  + all_task_stacks
  + interrupt_stack
  + driver/DMA/ring buffers
  + SDK heap high-water
  + application buffers
  + safety margin
```

UCN 需要单独列出 Node、每个 Link、Adapter Queue、Event Runtime、Transfer、Service、Security、Cluster/Federation 对象，以及产品配置创建的实例数量。

## 当前证据口径

Cluster M14 的 Host 回归观测为对象 1608 B、archive text 133559 B、最大静态栈帧 1840 B、UCN 动态分配 0。它们只能作为当前 commit 的 Host 趋势证据。

Core、Transfer、Service 和各 Adapter 应从对应构建的 map/size 输出单独统计；不能将某个 archive 的 text 大小等同于最终固件增量，因为链接器可能裁剪未引用对象。

资源报告应绑定 Git commit、编译器版本、`Debug/Release`、LTO、Profile 和容量宏。任何条件变化都可能使数字失效。Host `sizeof` 对相对趋势有价值，但指针宽度、对齐和 ABI 与 32-bit MCU 不同。

## MCU 验收

目标板上至少记录：编译配置、链接 map、固件 hash、FreeRTOS/RTOS stack high-water、运行峰值 heap、PSRAM 使用、ISR ring 峰值和持续压力下的最小余量。没有这些证据时只写“Host 已测”，不能写“MCU 占用已确认”。

### 测量场景

至少覆盖：空闲联网、持续小包、最大 Transfer、路由风暴、链路断开/重连、多个 Bearer 同时 RX、Cluster Snapshot/Recovery、诊断查询与加密开启。每个场景记录峰值而非只记录启动后的静态值。

### 栈验收

编译器 `.su` 文件只给出静态 frame；运行时还要考虑调用深度、库函数、日志格式化和中断嵌套。RTOS 应分别读取每个 UCN Owner/驱动任务的 high-water，裸机则使用栈填充法或链接器边界测量。对不可测的中断路径保留更高余量。

### 建议门槛

项目可根据风险制定余量，但必须写成产品规范。常见起点是：长期压力后 task stack 仍保留至少 25%，可用 heap/静态 RAM 保留至少 20%，ISR ring 峰值不超过容量的 75%，并且没有丢控制帧。这里只是规划起点，不是 UCN 已验证的通用承诺。
