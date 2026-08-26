# 自定义 Port、Adapter、Source

> 文档级别：`GUIDE`
> 实现状态：`PARTIAL（通用合同已实现，产品 BSP/SDK 需接入）`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：公共 API、Port/Adapter/Source 实现与测试
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：部分 ESP32-S3 UART/ESP-NOW；其他平台按正文标注

新增平台时按三层拆分：

- Port：时间、临界区、通知和 task/ISR 语义；
- Source：驱动数据、ring、carrier/reassembly、health；
- Adapter/Link：完整 UCN frame、MTU、发送、指标和 Cost。

每层单独文件和 context，不在通用 Runtime 中写厂商 SDK。至少测试分块、并发 ISR、满队列、断链、重连、无写回和多实例。

## 先选择Source类型

| 驱动输出 | 推荐做法 |
| --- | --- |
| 完整packet且保留边界 | 直接Runtime submit frame/packet Source |
| UART/USB/TCP字节流 | Stream Source + COBS |
| CAN-FD frame | CAN-FD Source |
| Classic CAN 8 B | Classic Carrier重组 |
| 特殊SPI/共享内存 | 自定义Source，最终提交完整UCN frame |

不要在Adapter Queue之后再做Carrier重组；那时Core期望已经是完整frame。

## Port文件

新建平台专属头/源，例如`include/ucn/ports/ucn_port_xxx.h`与`src/ports/ucn_port_xxx.c`，只实现时间、随机/持久counter、task/ISR临界区、Owner通知/等待。Port不include厂商UART/CAN业务driver。

API v2使用具名初始化和ISR token。为Host写fake scheduler验证notify/wait/yield语义，再接真实RTOS。

## Source文件

Source context拥有driver ring/reassembly状态、health/stats和Runtime引用。接口至少包括init、task/ISR write或signal、service、reset/get_health。service必须接受max_work并在有剩余数据时返回pending事件。

## Link/Adapter

Link ops实现open、bounded send enqueue、poll_rx(可选)、get_status、close、get_metrics。设置独立link ID、peer、static/dynamic MTU、liveness和standard preset/override。

## 集成骨架

```text
bsp_xxx_driver.c       厂商SDK/IRQ/DMA
ucn_xxx_source.c       framing/ring/health
ucn_port_myos.c        scheduler/time/critical
product_ucn_links.c    实例、引脚、preset、peer binding
product_ucn_owner.c    Node/Runtime/Service/Transfer组装
```

这种拆分让后续增加第二个RTOS或第二路同类外设时复用Source/Link，而不复制整个协议。

## 必测合同

- init非法配置完全不写对象；
- task/ISR同时生产不破坏ring；
- budget=1逐步drain与pending重置；
- full queue/ring返回NO_SPACE且旧数据不覆盖；
- Driver临时buffer在callback后不被引用；
- Link down/MTU变化立即反映选择；
- 多实例context、stats和peer不串线；
- Release/ASan/静态分析与目标板压力。

## 上游化要求

通用且不依赖厂商的Source可进入主库；产品引脚、设备句柄和SDK初始化留在BSP。文档写清已验证平台/SDK版本，不能因为抽象接口存在就列为官方实机支持。
