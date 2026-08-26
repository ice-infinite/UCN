# 接入 UART、CAN、Wi-Fi 等通信介质

UCN 不直接配置引脚和厂商外设。用户要把一个物理接口包装为 Link，并根据 Driver 输出形态选择 Source。

## 1. 先判断 Driver 输出类型

| Driver输出 | 推荐接法 |
| --- | --- |
| 完整Packet，边界可靠 | 直接 `ucn_event_runtime_submit_frame*()` 或自定义Packet Source |
| UART、RS-485、USB CDC、TCP字节流 | Stream Source + COBS Carrier |
| CAN-FD Frame | CAN-FD Source |
| Classic CAN 8 B | Classic CAN Carrier 重组 |
| Wi-Fi UDP/ESP-NOW Packet | Packet Queue/直接Runtime提交 |
| BLE小MTU、特殊SPI/I2C | 自定义Carrier/Source，最终提交完整UCN Frame |

统一目标是：**进入 Adapter Queue 以前，必须已经恢复成一条完整 UCN Frame。**

## 2. 每条物理通道一个 Link

一个 Node 可以注册多条同类或不同类 Link：

```text
UART1 → Link 1
UART2 → Link 2
CAN1  → Link 3
CAN2  → Link 4
Wi-Fi Peer B → Link 5
USB CDC → Link 6
```

每条 Link 有自己的：

- `link_id`；
- Driver context；
- Peer Node ID或动态候选；
- MTU；
- liveness profile；
- status/metrics；
- TX/RX queue和故障域。

## 3. 使用标准 Preset 得到默认 Cost

Preset 只解析标称介质参数，不初始化 Driver：

```c
ucn_standard_link_config_t input = {
    .local_link_id = 1U,
    .peer_node_id = UINT32_C(2),
    .preset = UCN_STANDARD_PRESET_UART_921600_8N1,
    .required_logical_mtu = UCN_STANDARD_LOGICAL_MTU_AUTO,
    .carrier_enabled = true,
    .override_base_cost = false,
    .override_rtt_reference = false,
    .administrative_bias = 0
};
ucn_standard_resolved_link_config_t resolved;

check(ucn_standard_link_config_resolve(&input, &resolved));
```

然后用 `resolved.logical_mtu`、`base_cost`、`rtt_reference_ms` 配置产品 Link/metrics。运行时实际队列、RTT、失败率和介质质量会形成动态 Cost；不要把动态罚分预先重复塞进 base cost。

## 4. UART、RS-485、USB CDC

共同链路：

```text
UCN完整Frame
 → COBS编码+分隔符
 → Driver TX Queue/DMA
 → 字节流
 → RX DMA/Ring
 → Stream Source
 → COBS解码
 → 完整UCN Frame
```

### UART 接入步骤

1. BSP配置UART实例、TX/RX、baud、8N1和可选流控；
2. 建立Driver RX ring与TX queue；
3. 建立 `ucn_link_t`；
4. 初始化 Stream Source和静态storage；
5. 绑定到Event Runtime source ID；
6. IRQ/DMA callback写chunk并signal source；
7. Owner运行Source service并提交完整Frame；
8. Link `send()`执行COBS编码后入TX queue。

不要把一次DMA callback当作一帧。字节流可能分包、粘包、截断或一次收到多个Carrier。

### RS-485 额外要求

```text
等总线策略允许
 → 拉高DE
 → DMA发送
 → 等UART TC（最后停止位完成）
 → 拉低DE
```

只等DMA completion可能提前释放DE。多点RS-485的物理仲裁、主从或时隙由产品决定，UCN路由不会解决多个收发器同时驱动总线的问题。

### USB CDC

主机未枚举或DTR未就绪时报告Link Down。拔线时丢弃未完成Carrier，清Driver状态；重连后重新建立Session/Neighbor状态。

## 5. CAN 与 CAN-FD

每个控制器一个Source/Link，不能让CAN1/CAN2共享reassembly状态。

### Classic CAN

完整UCN Frame需要START/CONTINUE分段：

```text
Link send
 → 分成多个8B物理帧
 → Driver TX queue

RX ISR
 → 固定CAN frame ring
 → Owner重组Carrier
 → 完整后先提交Adapter Queue
 → 再处理下一条START
```

必须测试丢段、重复、乱序、连续Carrier、slot满和timeout。

### CAN-FD

64 B DLC可以承载较短Carrier，但DLC rounding产生的padding必须为零。仲裁速率、Data Phase速率、BRS/ESI、Filter和Bus-Off仍由Driver管理。

### Bus-Off

```text
检测Bus-Off
 → Source报告BUS_OFF
 → Link Down/Route失效
 → 清半条重组
 → Driver恢复控制器与Filter
 → Source ACTIVE
 → 重新HELLO/验证
```

## 6. Wi-Fi、ESP-NOW、BLE、LoRa

仓库不内置厂商无线Driver。产品完成：

1. 初始化Radio、Channel、Peer/Connection/Socket；
2. 在SDK callback内复制Packet和物理来源；
3. 把MAC/connection handle映射到对应Link；
4. Owner上下文提交完整Frame；
5. `get_status()`报告up/MTU；
6. `get_metrics()`报告真实RTT、重试、队列、RSSI/quality或busy。

### Wi-Fi UDP

UDP有Packet边界，可一包一条Carrier。TCP是字节流，必须使用Stream framing。Wi-Fi关联、IP、DHCP、Socket仍是产品职责。

### ESP-NOW

维护peer table、channel、加密key和send callback。当前SDK/芯片的实际MTU必须由产品确认，不能硬编码历史值。ESP-NOW peer存在不代表已通过UCN Join和安全准入。

### BLE

GATT notification/write的ATT MTU可能小于Core Frame，应选择较窄Wire或增加Carrier分片。Connection handle重连后会变化，必须重新绑定Node/Session。

### LoRa

空口时间和法规占空比严格，必须降低HELLO、诊断和Transfer频率。T8K在部分LoRa配置下虽然格式可表达，但没有实际工程意义。

## 7. Link Metrics

`get_metrics()`可提供：

- 稳定base route cost；
- 一跳RTT；
- TX/RX Carrier失败率；
- Adapter自身TX queue pressure；
- medium busy和quality；
- metrics timestamp；
- product administrative bias。

不要把Core Q0/Q1队列占用写进Adapter queue pressure，也不要将同一个RSSI样本同时重复映射成多个罚分来源。

## 8. 介质验收清单

- Driver临时Buffer在callback后不再被引用；
- RX/TX队列满明确返回并计数；
- Link Down、MTU变化和恢复及时上报；
- 随机分块、粘包、ring wrap、连续Carrier通过；
- 多实例context和stats不串线；
- 关闭日志后重测吞吐、CPU和丢帧；
- 弱信号、Bus-Off、拔线、重连和session变化均覆盖；
- 实测报告区分物理bitrate、Carrier B/s、UCN Frame B/s和业务Payload B/s。

下一步：固定拓扑可直接继续业务；动态网络阅读 [自动入网与自动路由](05-自动入网与自动路由.md)。
