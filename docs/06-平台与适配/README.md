# 平台与适配

> `MIGRATION / NOT CURRENT`：本目录保留旧接入材料，当前接口以[官方 Adapter 与平台](../official/05-Adapter与平台/README.md)为准。

这里保存 MCU、RTOS、Port、Adapter 和物理介质对接合同：

- [多介质 Adapter 契约](UCN_Adapter_契约.md)
- [标准 Port/Adapter 与默认 Cost 基线](UCN_标准Port_Adapter封装与默认Cost基线方案.md)
- [各平台快速使用手册](../01-入门与使用/快速使用手册/README.md)

平台实现应只适配队列、临界区、时钟、事件与驱动，不应把某个 RTOS 或 Linux 对象泄漏到 UCN Core。
