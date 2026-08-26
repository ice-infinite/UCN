# 集成指南

> 文档级别：`GUIDE`
> 实现状态：`PARTIAL（通用合同已实现，产品 BSP/SDK 需接入）`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：公共 API、Port/Adapter/Source 实现与测试
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：部分 ESP32-S3 UART/ESP-NOW；其他平台按正文标注

先按平台选 Owner/Port，再接物理 Source/Adapter，最后启用 Service、Transfer、安全或 Cluster。UCN 不内置具体 Wi-Fi SDK、CAN 控制器或板级引脚配置，产品负责把驱动数据转换为标准 frame/event 合同。

最小闭环是：静态对象与配置 → 时钟/临界区 → Link/Adapter → Endpoint → Owner step → 日志与统计。

## 推荐实施顺序

```text
Host双节点
  → 目标平台Owner/Port
  → 单个UART/CAN/无线Link
  → Endpoint收发
  → 自动路由/断链
  → 多Bearer/Policy
  → Service与Transfer
  → Security Provider
  → 可选Cluster
  → 压力、长稳、资源和回滚
```

不要一开始同时打开Wi-Fi、UART、Transfer、Security和Cluster；任一错误都难以定位。

## 导航

- 起步：[五分钟Host](01-五分钟构建与最小Host示例.md)、[裸机](02-裸机最小节点.md)、[通用RTOS](03-通用RTOS对接流程.md)
- OS：[FreeRTOS](04-FreeRTOS接入.md)、[Zephyr](05-Zephyr接入.md)、[NuttX](06-NuttX接入.md)、[RT-Thread](07-RT-Thread接入.md)
- 介质：[UART/RS-485/USB](08-UART-RS485-USB-CDC接入.md)、[CAN/CAN-FD](09-CAN与CAN-FD接入.md)、[无线](10-WiFi-ESP-NOW-BLE-LoRa接入.md)
- 功能：[多Bearer](11-多Bearer与多实例节点.md)、[Service](12-任务间与跨MCU-Service通信.md)、[Transfer](13-32B至8KiB-Transfer通信.md)、[路由Policy](14-自动路由、固定路径与负载均衡.md)
- 高风险：[Cluster](15-Cluster受限接入.md)、[Security](16-安全Provider接入.md)、[自定义三层](17-自定义Port-Adapter-Source.md)、[迁移](18-从现有项目迁移到UCN-v5.md)

## 每次集成的交付物

- 产品配置头和CMake preset；
- Node/Endpoint/Service ID表；
- 引脚、总线、peer binding和Link Cost表；
- Owner/ISR/任务上下文图；
- 固件hash、构建命令、测试日志和资源报告；
- 未验证能力与回滚步骤。

只有代码、接线和证据三者对得上，才算完成一个平台接入。
