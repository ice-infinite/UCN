# 接入 UART、CAN、Wi-Fi 与 USB

## 1. 共用流程

每条物理接口创建独立 Link config 和 Driver context，注册到 Adapter。RX ISR/DMA 完成时发布
完整 item 并通知 Owner；TX 由 Adapter 调 Driver，最终 completion 再归还 token。不得在 ISR
直接解析协议或调用业务回调。

一个节点可以有多个 UART、多个 CAN 或混合 Bearer。每个实例必须有不同 Link ID 和独立
Instance Generation；断开重开时推进 Generation，防止迟到 completion 命中新实例。

## 2. UART/RS-485

先调用 `ucn_v6_uart_link_config_init()` 生成协议配置，再实现实际串口 open/submit/cancel。
产品决定 UART 号、TX/RX 引脚、波特率、DMA、RS-485 DE、frame delimiter 和缓存。接收器必须
处理粘包、拆包、坏长度和 line noise；不能假设一次 DMA completion 正好是一帧。

## 3. Wi-Fi/ESP-NOW

`ucn_v6_esp_now_link_config_init()` 只描述 MTU/代价/能力。产品设置 Wi-Fi channel、peer、
加密关系和 SDK callback。ESP-NOW completion 是异步的；不要在 send 返回后立即复用 buffer。

## 4. CAN/CAN-FD

`ucn_v6_can_link_config_init()` 区分 Classic CAN 与 CAN-FD。Classic CAN 必须 Carrier 分片重组，
连续 START、slot 满、超时和丢帧均要 fail-closed；CAN-FD 对 padding 做零值检查。CAN 仲裁
优先级可映射 Q0～Q3，但不能绕过协议配额或安全门禁。

## 5. USB

`ucn_v6_usb_link_config_init()` 生成 Link 参数，实际 CDC/vendor endpoint、枚举、短包、ZLP、
断连和 host reset 由产品 Driver 处理。USB ready 变化必须通知 Adapter，不得继续向失效 endpoint
提交。

## 6. 上板前

先用 Fake Driver 覆盖同步/异步完成、取消失败、队列满和 reopen；再上板测逻辑分析仪波形、
真实 MTU、吞吐、尾延迟、CPU/栈和错误恢复。参考配置不能替代板级原理图与 SDK 验证。
