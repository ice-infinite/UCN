# UCN 标准 Port / Adapter 封装与默认 Link Cost 基线方案

> 状态：V5-34 设计、V5-35 静态 Preset Resolver、V5-36/V5-44 动态 Cost/选路与 V5-46 分目录独立 Platform Port 已完成；真实 Port SDK 对象/Adapter与实机标定仍未完成。
> 对应后续任务：V5-38～V5-42。
> 适用基线：`codex/v5-adaptive-wire` 当前工作树，UCN Core 5.0.0 / 线协议 v5。
> 目标：让产品只填写 MCU、引脚、外设端口、速率和少量产品边界，即可选用标准 Port/Adapter 配置；同时为每种 Bearer 提供可解释的基础 Cost 与统一的动态质量惩罚规则。

## 1. 先给结论

后续不应把 Wi-Fi、UART、CAN、USB 的驱动代码塞进 `ucn_core`，也不应让业务代码自行填写一堆 `route_cost` 数字。推荐增加一层 **标准 Port + 标准 Adapter 配置层**：

```text
产品配置（板型、引脚、外设、速率、Node ID）
    ↓ 选择 Preset 并覆盖少量参数
标准 Port（裸机 / FreeRTOS / Zephyr / NuttX）
    ↓ 提供时间、临界区、线程/事件、固定队列
标准 Adapter（UART / CAN-FD / Wi-Fi / USB CDC）
    ↓ 形成 ucn_link_t、RX/TX 有界队列、状态和通用指标
UCN Core（Neighbor / Route / Path / Policy / Service）
```

产品在 V5-35 已可先用 SDK 无关的 Resolver 得到一份经过边界检查的静态 Link 配置；它不初始化硬件，也不会直接写 Core 内部字段：

```c
#include <string.h>
#include "ucn/ucn_standard_adapter.h"

ucn_standard_link_config_t config;
ucn_standard_resolved_link_config_t resolved;

(void)memset(&config, 0, sizeof(config));
config.local_link_id = 0x70U;
config.peer_node_id = UINT32_C(0x0000000B);
config.preset = UCN_STANDARD_PRESET_UART_115200_8N1;
config.required_logical_mtu = 128U;

result = ucn_standard_link_config_resolve(&config, &resolved);
/* result == UCN_OK 后，V5-38 的 UART Adapter 才把 resolved 的
 * logical_mtu/base_cost 等事实绑定到产品 HAL 和 ucn_link_t。 */
```

`ucn_standard_preset_resolve()` 和 `ucn_standard_link_config_resolve()` 已是公开 C99 API；它们只返回 Preset 事实与产品显式覆盖后的结果。标准 Adapter 生成静态 `ucn_link_t`、队列和 HAL 回调绑定仍属于 V5-38～V5-42；产品仍必须提供真实 BSP/HAL、引脚、端口、Node ID、Network ID、安全 Provider 与 Endpoint ABI。

## 2. 必须保持的当前边界

当前 `ucn_link_metrics_t` 已冻结如下责任：

| 字段 | 当前含义 | 本方案是否改变 |
| --- | --- | --- |
| `route_cost` | 稳定、可加的单跳**基础**代价；越小越好。 | 不改变。由标准 Preset 或产品显式覆盖提供。 |
| `rtt_ms` | 直连一跳 RTT；可未知。 | 不改变。用于后续动态选择分。 |
| `tx_failure_per_mille` | Adapter 自己的发送失败比例。 | 不改变。不得填端到端业务丢包。 |
| `queue_pressure_per_mille` | Adapter 自己固定 TX Queue 的占用。 | 不改变。不得填 Core/Service 队列。 |

因此分两个概念：

```text
base_cost              = 稳定的介质 + 速率 + 固定 Carrier 开销
effective_select_cost  = base_cost + 已平滑的队列/失败/RTT 动态惩罚
```

`base_cost` 可写入 RREQ/RREP 的累计 Route Cost，并在多个节点间保持可解释；`effective_select_cost` 只在本节点根据本地质量快照做 Bearer、Candidate 和 Q1 Flow 的选择/重绑，**不能直接写回网络帧**。这样既不把各节点的局部拥塞误传播成全网真相，也不会把 RTT/失败/队列在 Adapter 与 Core 重复相加。

V5-44 已实现 `effective_select_cost` 的统一计算，V5-36 已将它接入 Full 的 Bearer、Candidate 本地出口与 Q1 `AUTO_BALANCE`。V5-35 Resolver 仍只负责解析静态 `base_cost`、RTT 参考、最大 MTU 和管理偏置；真实 Adapter 必须显式把这些值和运行指标写入 Link 合同，Core 不会按接口名字猜测。

## 3. 统一 Cost 单位和等级

### 3.1 Cost 不是“纯速率倒数”

UCN Cost 是无单位、正整数、逐跳可加的选择分。它同时表达：

- 介质的稳定时延/调度特性；
- 配置速率下的短 UCN 帧传输能力；
- 固定 Carrier 开销，例如经典 CAN 的有界分段；
- 无法由速率体现的共享无线竞争、半双工切换和 USB Host 调度。

它**不是**毫秒、dBm、百分比，也不是保证吞吐。`1` 只表示“在本产品 Cost 域中最优的一跳”，不表示 1 ms。

| 等级 | Cost 范围 | 意义 | 示例 |
| --- | ---: | --- | --- |
| A | 6～12 | 高带宽、低固定开销、专用/稳定链路。 | USB High-Speed、CAN-FD 1M/4M、UART ≥ 921600。 |
| B | 13～29 | 常规高速链路。 | USB Full-Speed、CAN-FD 500K/2M、UART 230400～460800。 |
| C | 30～59 | 常规控制/遥测链路。 | CAN 500K～1M、UART 57600～115200、ESP-NOW 默认。 |
| D | 60～99 | 低速或明显受限链路。 | CAN 250K、UART 19200～38400。 |
| E | 100～160 | 只适合低频配置、状态或备份的慢链路。 | CAN 125K、UART 9600。 |

等级只用于人阅读和默认 Preset 名称；Core 只比较整数 Cost。它不是 Authorized Class、安全等级或 QoS 等级。

### 3.2 固定规则

1. 有明确产品标定值时，产品值优先于本表。
2. 有明确 Preset 时，使用 Preset 的 `base_cost`；不能因为短暂拥塞改写它。
3. 没有 Preset、没有产品值时，`route_cost_valid=false`，即 Unknown；不能擅自把未知介质当作高速链路。
4. 每一跳有效基础 Cost 必须为 `1..65534`；`0` 和 `65535` 保留为无效/Unknown。
5. 多跳累计值仍使用当前 v5 的 32 bit Route Cost 和既有 3/3/3/4 B 控制载荷编码，不能因本表重新缩窄。

## 4. 默认基础 Cost 表

以下值是假设链路已建立、物理层正常、Adapter TX 队列空闲的**起始值**。它们用于第一次发现、没有足够质量样本、或产品未另行标定时的初始选择。实际板卡部署后应记录日志并可通过产品配置覆盖。

### 4.1 UART / TTL 串口（8N1、全双工）

8N1 每字节通常占 10 bit，因此表中的“理论字节流上限”按 `baud × 8 / 10` 计算；它尚未扣除 COBS/SLIP、CRC、应用帧间隔和 MCU 调度开销。

| Preset | 波特率 | 8N1 理论字节流上限 | 默认 `base_cost` | 等级 |
| --- | ---: | ---: | ---: | --- |
| `UART_9600_8N1` | 9,600 | 7.68 kbit/s | 140 | E |
| `UART_19200_8N1` | 19,200 | 15.36 kbit/s | 92 | D |
| `UART_38400_8N1` | 38,400 | 30.72 kbit/s | 62 | D |
| `UART_57600_8N1` | 57,600 | 46.08 kbit/s | 50 | C |
| `UART_115200_8N1` | 115,200 | 92.16 kbit/s | 34 | C |
| `UART_230400_8N1` | 230,400 | 184.32 kbit/s | 24 | B |
| `UART_460800_8N1` | 460,800 | 368.64 kbit/s | 17 | B |
| `UART_921600_8N1` | 921,600 | 737.28 kbit/s | 12 | A |
| `UART_1M_8N1` | 1,000,000 | 800 kbit/s | 11 | A |
| `UART_2M_8N1` | 2,000,000 | 1.60 Mbit/s | 8 | A |
| `UART_3M_8N1` | 3,000,000 | 2.40 Mbit/s | 7 | A |
| `UART_4M_8N1` | 4,000,000 | 3.20 Mbit/s | 6 | A |

RS-485 复用相同速率表，但默认额外加 `12` Cost：`base_cost = UART 表值 + 12`。原因是半双工方向切换、总线仲裁和轮询时隙；若产品有硬件自动方向控制、固定点对点或已经测得极低时隙，可显式覆盖。UART/RS-485 不应只凭“串口打开”报告低 Cost；字节流 Carrier 必须先正确完成定界、长度校验和有界 TX/RX 队列。

### 4.2 经典 CAN（Carrier 已实现后才可启用）

经典 CAN 单帧最多 8 B，低于 UCN W0 的 17 B 基础头。**在有界 Carrier 分段/重组完成前，任何经典 CAN Preset 都必须返回 `UCN_ERR_CONFIG`，不能假装已经可用。**

下表假设 Carrier 已实现、总线在空闲/轻载状态，并已把分段、仲裁和重组的固定开销计入 Cost。

| Preset | 仲裁/数据速率 | 默认 `base_cost` | 等级 | 说明 |
| --- | ---: | ---: | --- | --- |
| `CAN_CLASSIC_125K` | 125 kbit/s | 110 | E | 适合低频状态、配置或备份。 |
| `CAN_CLASSIC_250K` | 250 kbit/s | 72 | D | 常见低速车载/工业配置。 |
| `CAN_CLASSIC_500K` | 500 kbit/s | 45 | C | 常规控制网络。 |
| `CAN_CLASSIC_1M` | 1 Mbit/s | 30 | C | 经典 CAN 上限；仍有 8 B Carrier 成本。 |

经典 CAN 的硬 Bus-Off、错误被动、控制器停机必须由 `get_status().is_up=false` 表示，直接排除，而不是用一个极大 Cost 掩盖故障。

### 4.3 CAN-FD（64 B 逻辑帧上限）

CAN-FD 的数据段可提高速率且单帧最多 64 B；在 `UCN_MAX_FRAME_BYTES ≤ 64`、Filter、TX Queue 和 Bus 状态都满足时，可不经过经典 CAN Carrier 直接承载一个 UCN 逻辑帧。表中的“仲裁/数据”分别是 Nominal Bit Rate / Data Bit Rate。

| Preset | 仲裁 / 数据速率 | 默认 `base_cost` | 等级 |
| --- | ---: | ---: | --- |
| `CANFD_500K_2M` | 500 k / 2 Mbit/s | 22 | B |
| `CANFD_500K_4M` | 500 k / 4 Mbit/s | 15 | B |
| `CANFD_1M_2M` | 1 M / 2 Mbit/s | 18 | B |
| `CANFD_1M_4M` | 1 M / 4 Mbit/s | 12 | A |
| `CANFD_1M_8M` | 1 M / 8 Mbit/s | 9 | A |

CAN-FD 的实际可用数据速率受控制器、收发器、线缆、拓扑和位时序限制；没有确认硬件支持时不得自动选择 8 Mbit/s Preset。经典 CAN 1 Mbit/s 与 CAN-FD 的 2/4/8 Mbit/s 能力边界参考 Bosch 的协议说明；具体产品仍以板级时钟和收发器数据手册为准。[Bosch CAN 协议概览](https://www.bosch-semiconductors.com/products/ip-modules/can-protocols/)

### 4.4 Wi-Fi / ESP-NOW

无线 Cost 不按标称 PHY rate 单独决定，因为信道竞争、重传、RSSI、干扰、睡眠和 AP 共信道都会影响实际服务能力。表只给出“无线空闲、直接邻居、无历史质量”的保守初值；实时质量必须依赖失败率、Adapter TX Queue 压力和经验证的 RTT。

| Preset | 速率/模式 | 默认 `base_cost` | 等级 | 适用边界 |
| --- | ---: | ---: | --- | --- |
| `ESPNOW_DEFAULT_1M` | ESP-NOW 默认 1 Mbit/s | 45 | C | 当前 ESP32 首选起始 Preset。 |
| `WIFI_80211_1M` | 已锁定 1 Mbit/s | 52 | C | 仅适用于明确的固定速率配置。 |
| `WIFI_80211_2M` | 已锁定 2 Mbit/s | 42 | C | 同上。 |
| `WIFI_80211_6M` | 已锁定 6 Mbit/s | 30 | C | 同上。 |
| `WIFI_80211_12M` | 已锁定 12 Mbit/s | 24 | B | 同上。 |
| `WIFI_80211_24M` | 已锁定 24 Mbit/s | 18 | B | 同上。 |
| `WIFI_80211_54M` | 已锁定 54 Mbit/s | 14 | B | 同上；仍须承受共享信道竞争。 |
| `WIFI_RATE_UNKNOWN` | 自动协商/未知 | 45 | C | 推荐默认；等待真实样本，不以 MCS 名称猜测。 |

ESP-NOW 的默认比特率确为 1 Mbit/s，但 Espressif 的公开测试也显示实际一对一吞吐会随环境显著变化，因此本表把它作为 Cost 45 的保守无线起点，而非 1 Mbit/s 的吞吐承诺。[ESP-NOW 文档](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32/api-reference/network/esp_now.html) / [ESP-FAQ 实测说明](https://docs.espressif.com/projects/esp-faq/en/latest/application-solution/esp-now.html)

Wi-Fi Adapter 必须把发送回调、重传/失败统计、自己的 TX Queue 占用和可选 RSSI EWMA 转换为通用指标；RSSI 不能直接进入 Core。Wi-Fi Mesh 不会被启用：UCN 只借用 Wi-Fi/ESP-NOW 的一跳收发能力，自组网、寻路和多跳仍由 UCN 负责。

### 4.5 USB（仅 CDC ACM / 固定点对点 Link）

USB 不是天然的多点广播 Mesh。首版标准 USB Adapter 只支持已配置的 CDC ACM Host↔Device 或 Device↔Host 点对点 Link；它不负责 USB 枚举、Hub 路由或自动发现远端 UCN Node。产品必须明确 USB 控制器角色、CDC 端点、VID/PID/接口号和物理对端绑定。

| Preset | USB 信令速率 | 默认 `base_cost` | 等级 | 备注 |
| --- | ---: | ---: | --- | --- |
| `USB_CDC_FS` | Full-Speed 12 Mbit/s | 14 | B | 加入 USB 帧调度/CDC 缓冲的固定保守成本。 |
| `USB_CDC_HS` | High-Speed 480 Mbit/s | 6 | A | 仅在 MCU、PHY 和 Device/Host 均实际支持时选用。 |

USB 2.0 的 Full-Speed 为 12 Mbit/s、High-Speed 为 480 Mbit/s；这只是信令速率，CDC 可用吞吐还受端点、Host 调度、驱动缓冲和 CPU 影响。[USB-IF 速率说明](https://www.usb.org/sites/default/files/CabConn20.pdf)

## 5. 动态有效代价：规范引用与资源边界

动态 Cost 不再以“建议阈值”留在本设计文档中。采样窗口、指标口径、EWMA、加减分表、Invalid/陈旧处理、状态门、Unknown、20%/3 样本/2 Probe/保持期、Q1 重绑和测试向量，均以 **[Link Cost 计算规范（LC-1）](../04-路由与链路/UCN_Link_Cost计算规范.md)** 为唯一准则。

该规范规定：

- `effective_select_cost` 只在本节点计算，不能回写 `route_cost` 或任何线上控制帧；硬 Down、MTU/安全门失败直接排除，不用“大 Cost”伪装。
- 所有动态质量项默认只增加惩罚；唯一可减项是产品静态配置的 `administrative_bias`（范围 `-32..+64`），用于明确且可审计的产品偏好。
- 当前 v5 已实现统一 Resolver；RX 失败率、介质占用/质量和快照时间戳均为可选输入，缺失时按 Invalid 零惩罚处理，不能由 Core 伪造。

### 5.1 Build Profile 资源边界

当前质量快照与 Policy 模块只在 Full Profile 编译。后续不能为了“所有节点自动评分”把同一批质量表强行塞进 Nano：

| Build Profile | V5-35 Preset | V5-36 动态选择分 |
| --- | --- | --- |
| Nano | 支持静态 Preset/产品覆盖，直接 Link 使用基础 Cost。 | 不新增常驻质量快照；保持基础 Cost 和既有固定资源边界。 |
| Lite | 支持静态 Preset/产品覆盖。 | 当前保持静态基础 Cost，不新增质量快照或 Path/Policy 表。 |
| Full | 支持静态 Preset/产品覆盖。 | 已使用固定质量快照完成 Bearer、Candidate、Q1 Flow 的动态选择分与本地诊断。 |

所以“标准 Adapter 有默认 Cost”适用于全部 Profile；“动态自动选路”按已有 Feature/Profile 裁剪，不应成为小 MCU 的隐藏 RAM 成本。

## 6. 标准封装 API 设计

### 6.1 共同配置对象

建议新增一个不依赖具体 MCU SDK 的公开配置头，例如 `ucn_standard_adapter.h`：

```c
typedef enum ucn_bearer_kind {
    UCN_BEARER_UART,
    UCN_BEARER_RS485,
    UCN_BEARER_CAN_CLASSIC,
    UCN_BEARER_CAN_FD,
    UCN_BEARER_WIFI,
    UCN_BEARER_USB_CDC
} ucn_bearer_kind_t;

typedef struct ucn_link_default_profile {
    ucn_bearer_kind_t bearer_kind;
    uint32_t configured_rate_bps;
    uint16_t base_cost;
    uint16_t rtt_reference_ms;
    size_t logical_mtu;
    bool requires_carrier;
} ucn_link_default_profile_t;

typedef struct ucn_standard_link_config {
    uint8_t local_link_id;
    ucn_node_id_t peer_node_id;
    ucn_adapter_address_t peer_address;
    ucn_link_default_profile_t profile;
    bool override_base_cost;
    uint16_t base_cost_override;
} ucn_standard_link_config_t;
```

所有字段由产品在启动前静态配置，初始化后只读。`peer_node_id=0` 可用于动态发现的 Candidate Link；物理地址仍只在 Adapter 本地使用，不是 UCN 身份。

### 6.2 Port 层

| Port | 标准层应封装 | 产品仍必须提供 |
| --- | --- | --- |
| Bare metal | Super Loop 的 `poll_rx → pump → bridge_step_at → node_step` 顺序、临界区钩子、静态对象。 | `now_ms`、临界区、IRQ/DMA、所有 HAL。 |
| FreeRTOS | 静态 Protocol Task、`Queue`/通知、ISR 安全入队、短临界区 Hook。 | Task 优先级/栈、UART/CAN/Wi-Fi/USB 驱动句柄和 ISR。 |
| Zephyr | Kconfig 默认、专用 Protocol Thread、`k_msgq`/work 通知、设备就绪检查。 | Devicetree alias、设备节点、线程优先级与驱动配置。 |
| NuttX | pthread 或 Work Queue 外壳、poll/信号量唤醒、静态/受控队列。 | `/dev/*`、SocketCAN、USB CDC 设备、调度策略。 |

四类 Port 都只能创建一个 Node Owner。业务 Task/线程经 Service Router/产品封装发送，不能绕过 Port 直接并发调用 `ucn_node_*`。Port 层不包含任何 Wi-Fi、CAN、UART、USB 驱动实现，因此可保持 MCU-first、固定资源和跨 RTOS 可移植。

V5-46 已将这里的 Owner 顺序落为 SDK 无关公共 API，并按平台拆分文件：公共 `ucn_protocol_owner_rx_enqueue()` 只把完整帧复制到已有 Adapter RX Queue；各平台 `ucn_<platform>_port_rx_enqueue()` 成功入队后才通知自己的 Task/Thread；公共 `ucn_protocol_owner_step()` 按 `RX Pump -> 可选 Bridge step_at -> node_step` 运行，Bridge 与 Node 共享单次采样的 `now_ms`。裸机、FreeRTOS、Zephyr、NuttX、RT-Thread、Host Fake 都有自己的头/源，不会因新增一种 RTOS 修改其它 Port。它仍不替产品创建任何真实线程、Queue、信号量或 BSP 对象；具体映射与软件证据见 [V5-46 平台 Port 解耦重构报告](../08-实现与验证/版本演进/UCN_V5_46_平台Port解耦重构报告.md)。

### 6.3 UART / RS-485 Adapter

产品配置只需填写：外设实例、TX/RX 引脚、可选 RTS/CTS、波特率、格式、RS-485 方向控制、最大逻辑 MTU、Link ID、对端地址/Node ID 与 Preset。Adapter 内部负责字节流定界、完整 UCN 帧收发、有界 TX/RX 队列、CRC/定界错误统计、`get_status()` 和 `get_metrics()`。

首版固定使用一种明确 Carrier（建议 COBS + 长度检查）；不能让不同板子一边裸写 UCN 帧、一边 COBS 却仍宣称互通。Carrier 的实际字节开销进入吞吐实测，但不改变本表的初始 Cost，除非产品经标定显式覆盖。

### 6.4 CAN / CAN-FD Adapter

产品配置只需填写：CAN 控制器、RX/TX 引脚、Nominal/Data Bit Rate、Filter、TX Queue 深度、Node 地址映射、Link ID、Preset 和是否允许动态发现。

- CAN-FD：检查 `UCN_MAX_FRAME_BYTES ≤ 64` 后可直接承载一帧 UCN 逻辑帧。
- 经典 CAN：必须选择已实现并明确版本的 `ucn_can_classic_carrier`；未启用时初始化失败关闭。
- CAN Filter 只做物理接收过滤，不能把 CAN ID 当 UCN Node 身份或安全授权。
- Bus-Off/控制器 Stop 是硬 Down；错误帧、发送等待和总线占用用于更新 Adapter 指标。

### 6.5 Wi-Fi / ESP-NOW Adapter

产品配置只需填写：Wi-Fi interface、模式（ESP-NOW / 已配置 802.11 数据通道）、Channel、PHY/Peer Rate 或 Unknown、MAC Peer/广播地址、TX Queue 深度、Link ID 和 Preset。

Wi-Fi 回调只做“回调数据 → 固定队列”和发送结果计数；Protocol Task 再 Pump。ESP-NOW 默认选择 `ESPNOW_DEFAULT_1M`；当产品显式调用底层 API 改变 Peer Rate 后，必须同步切换对应 Preset，不能继续报告旧 1 Mbit/s 基线。

### 6.6 USB CDC Adapter

产品配置只需填写：USB 控制器角色、CDC ACM interface/端点、Device 或 Host 句柄、静态物理对端、Link ID、Full/High-Speed Preset 和队列深度。

USB Adapter 只在枚举完成且 CDC 通道可用时报告 Up；断开、重新枚举或端点错误报告 Down。USB 作为固定点对点 Bearer 可与 UART/Wi-Fi 组成同 Node ID 的多 Bearer，但不能自行把多个 USB Device 拼成 Mesh。

### 6.7 建议纳入标准 Adapter 路线的其他 Bearer

下表新增的是 UCN 后续建议支持的**一跳承载**。是否支持某个 Bearer，不改变“UCN 自己负责多跳发现、路由、主备和负载均衡”的边界。

| 优先级 | Bearer | 建议方式 | 关键边界 |
| --- | --- | --- | --- |
| P0 | 以太网 10/100M | 以 UDP 或受控二层封装一个点对点 UCN Link；静态或产品控制的对端发现。 | 适合稳定有线 Backbone；IP/MAC 只是 Adapter 地址，不是 Node ID。 |
| P0 | RS-485 | 复用 UART Carrier，增加半双工方向控制、站号/仲裁和总线状态。 | 使用 UART 表 `+12` Cost；多点总线的冲突/超时必须在 Adapter 内有界处理。 |
| P1 | BLE LE | 已连接的 BLE GATT/L2CAP 通道对应一个点对点 Link。 | 使用 BLE LE 连接，不启用 BLE Mesh 路由；连接间隔、MTU、重传和睡眠影响动态质量。 |
| P1 | IEEE 802.15.4 | 直接把一个原始 802.15.4 收发通道作为 Link。 | 不再叠 Zigbee/Thread Mesh 路由；产品负责信道、PAN/短地址到 Adapter 地址的映射。 |
| P1 | NRF24 / 私有 2.4G | 用已确认的单跳 ACK/重传机制承载完整 UCN 逻辑帧。 | 需要明确最大 Payload 与固定 Carrier；无线质量转换为失败/队列/RTT，不把 RF 私有字段泄漏给 Core。 |
| P1 | LoRa P2P / Sub-GHz FSK | 作为低速、远距离的单跳 Link，使用严格的空口时间/发送配额。 | 不用于高频 Q0 或大数据；高 Cost、低 MTU、长 RTT 必须在产品策略中明确。 |
| P2 | UWB | UWB 数据通道作为 Link，测距结果可留在 Adapter 私有质量模型。 | 距离不是 Route Cost 的直接替代；需要单独标定时延、成功率和功耗。 |
| P2 | SPI / I²C | 仅用于板内或短距离、角色固定的 MCU 间点对点 Link。 | 产品必须处理主从、仲裁、复位和共享总线，不把它当自由发现 Mesh。 |
| P2 | IP Tunnel（Wi-Fi UDP / 4G/5G / Internet） | 用受控 UDP/TCP 隧道提供一个逻辑点对点 Link。 | 属于 Host/网关扩展；公网身份、安全、NAT 和重连必须由产品负责，不是 Core 自动入网。 |

这些扩展 Bearer 的条件 Preset 与唯一默认 `base_cost` 已在 [LC-1 Link Cost 计算规范](../04-路由与链路/UCN_Link_Cost计算规范.md) 第 8.3 节冻结；在本表对应 Adapter 通过第 6.9 节准入门槛前，当前代码不得自动采用它们。公网/蜂窝/未知多层 Tunnel 的默认值明确为 Unknown，不得伪造低 Cost。

### 6.8 不与 UCN 路由层直接叠加的网络协议

下面协议本身已含网状或网关路由层。首版不能把它们作为“UCN Link 后再同时启用其原生 Mesh”，否则一个业务包会经过两套独立寻路、重试和拥塞控制，路径、Cost 和故障归因都会失真：

| 协议 | 推荐关系 |
| --- | --- |
| Wi-Fi Mesh | 使用普通 Wi-Fi/ESP-NOW 的一跳能力，由 UCN 做 Mesh；若产品必须使用 Wi-Fi Mesh，则把其视为一个固定 IP Tunnel，而非暴露每个内部无线跳。 |
| BLE Mesh | 使用 BLE LE 点对点连接；不同时启用 BLE Mesh Relay。 |
| Zigbee Mesh / Thread Mesh | 使用原始 IEEE 802.15.4 Link；不再叠 Zigbee/Thread 的网络层路由。 |
| LoRaWAN | 作为云/网关业务通道，不作为 UCN 本地自组网的逐跳 Bearer；远距离本地组网使用 LoRa P2P/FSK。 |

### 6.9 扩展 Bearer 的统一准入门槛

任何新增 Bearer 都必须先回答以下问题，满足后才能获得标准 Preset：

1. 一跳最大逻辑 MTU 是多少；小于最小 UCN 帧时是否已有有界 Carrier。
2. 是否能提供有界 `send()`、固定 RX/TX Queue、明确 Up/Down 和错误计数。
3. 如何把该介质的私有指标归一为基础 Cost、直连 RTT、TX 失败率和 Adapter Queue 压力。
4. 是否存在本身的 Mesh/重试/加密网络层；若存在，UCN 与它只保留一层路由责任。
5. 在目标 MCU 上的 RAM、Flash、Task Stack、CPU、功耗和最坏 `send()` 时间是否实测通过。

## 7. 配置优先级与默认配置文件

配置优先级从高到低：

```text
单条 Link 的 `base_cost` / `rtt_reference_ms` 优先级是：

```text
per-Link 显式 override
    > 标准 Preset 固定值
```

`administrative_bias` 不是自动评分结果，而是单条 Link 的显式静态配置，必须落在 `-32..+64`。`required_logical_mtu=0` 表示请求当前 `UCN_MAX_FRAME_BYTES`；若该值大于 Preset 最大逻辑 MTU，Resolver 必须返回 `UCN_ERR_CONFIG`，不得静默截断。例如 CAN-FD 必须显式请求不大于 `64 B`，ESP-NOW 默认 Preset 必须显式请求不大于 `250 B`。

`UCN_USER_CONFIG_HEADER` 只参与公共编译期容量/默认值（例如 `UCN_MAX_FRAME_BYTES`）；它不改写 Preset 表，也不能绕过 Preset 的最大 MTU、经典 CAN Carrier 或地址校验。
```

建议每个产品只有一个集中配置文件，例如：

```text
ucn_product_config.h          # Core 固定容量、Profile、安全开关
ucn_product_links.c           # Node、Link、引脚、端口、Preset、显式覆盖
ucn_product_endpoints.c       # Endpoint / Service ABI
```

这样修改 UART 波特率、CAN-FD 数据率、Wi-Fi Peer Rate 或 USB FS/HS 时，只需改产品配置和重新编译；不需要到 `ucn_node.c`、业务 Task 或路由策略里寻找 Cost 常量。

## 8. 后续任务拆分

| 任务 | 内容 | 代码状态 |
| --- | --- | --- |
| V5-34 | 冻结本文的 Port/Adapter 分层、Cost 单位、Preset 表和动态评分合同。 | 本文完成；不改代码。 |
| V5-35 | 新增 `ucn_standard_adapter.h/.c`、Preset Resolver、产品覆盖优先级与纯 C 单元测试；不接硬件驱动。 | 已完成（软件）：所有静态 Preset、Product Override、MTU/Carrier/地址/偏置拒绝、Full/Lite/Nano 和 128 B 产品头专用链接测试通过。 |
| V5-36 | 将 `effective_select_cost` 接入 Full 的 Bearer/Candidate/Q1 策略；Lite/Nano 保持基础 Cost，线上 `route_cost` 不变。 | 已完成（Host 软件）；软切换/硬失效/Flow 租约门禁均有回归。 |
| V5-37 | 建立 Bare metal / FreeRTOS / Zephyr / NuttX 的 Port 外壳、编译模板和 Host Fake HAL 测试。 | 历史阶段：集中式 mode API 未发布，已由 V5-46 替换。 |
| V5-46 | 平台 Port 分目录解耦。 | 已完成（软件）：公共 ucn_protocol_owner 与裸机、FreeRTOS、Zephyr、NuttX、RT-Thread、Host Fake 独立 Port 提供唯一 Owner、固定 RX/Bridge 预算、同一时钟、等待上限与 Service OFF 裁剪；真实 RTOS SDK 对象仍由产品实现。 |
| V5-38 | 将 ESP32-S3 工程迁移到 v5，先实现 UART 与 ESP-NOW 标准 Adapter、默认 Preset 和两板实测。 | 待开始。 |
| V5-39 | 实现 CAN-FD 与 USB CDC 标准 Adapter，并做 Host/目标构建和点对点实测。 | SDK 无关 Source 已由 V5-59/60 完成；真实驱动与点对点实测归 V5-61。 |
| V5-40 | 单独实现经典 CAN 有界 Carrier 分段/重组、Bus-Off 与拥塞测试。 | 软件 Carrier/Bus State 已并入 V5-60；真实总线拥塞/Bus-Off 待 V5-61。 |
| V5-41 | 真实多板标定：记录每种 Preset 的 RTT、失败率、队列压力、P50/P95、吞吐、功耗和切换收益；据此调整产品覆盖值。 | 待硬件条件。 |
| V5-42 | 扩展 Bearer 路线 | 按 P0/P1/P2 依次评审并实现以太网、RS-485、BLE LE、802.15.4、NRF24/私有 2.4G、LoRa P2P/FSK、UWB、SPI/I²C 和受控 IP Tunnel；每项先通过统一准入门槛再新增 Adapter。 | 待开始；V5-35 已冻结其 `CONDITIONAL` Preset 元数据，但没有任何驱动、`ucn_link_t` 注册或自动选路。原生 Mesh/LoRaWAN 仅可作为受控 Tunnel/网关，不与 UCN 路由层双重叠加。 |
| V5-43 | 冻结 [LC-1 Link Cost 计算规范](../04-路由与链路/UCN_Link_Cost计算规范.md)，将动态计算的所有采样、取整、加减、状态门和切换规则从建议变为实现合同。 | 已完成（设计/任务）；未改代码。 |
| V5-44 | 扩展可选 Metrics 并实现 LC-1 Resolver、诊断和 C01～C10 向量；保持 Wire `route_cost` 和 Profile 固定资源边界不变。 | 已完成（Host 软件）；三 Profile、产品头、Service OFF、Sanitizer 和静态分析通过。 |
| V5-45 | 修正测试对默认 Frame 容量的隐含假设，完成 128 B 产品头下的全量回归。 | 已完成（软件）：Full+128 B 完整 CTest `2/2` 通过；不改变协议或 Preset 语义。 |

## 9. 验收标准

V5-35～V5-40、V5-44 每项完成前，至少满足：

1. 每个 Preset 的速率、MTU、基础 Cost、RTT 参考值与覆盖优先级有单元测试；非法/未知 Preset 失败关闭。
2. 基础 Cost 不包含 RTT、失败率、队列压力；动态选择分只由 Core 使用且每一动态项只计算一次。
3. LC-1 的 1000 ms 窗口、500 ms 采样、整数 EWMA、固定惩罚、陈旧/Unknown、20% 滞回、连续 3 样本、2 次 Probe ACK 和 3000 ms 保持期都有边界测试。
4. Q0、`PINNED_STRICT`、`PINNED_FAILOVER`、`AUTO_BALANCE` 的行为分别回归，不能因加入动态分数而变成逐帧切换。
5. Nano/Lite/Full 的裁剪边界明确：Nano 至少可使用静态 Preset 基础 Cost；需要质量快照、Candidate/Policy 的动态选择仅进入已有相应 Feature/Profile。
6. 任何 Port/Adapter 的 ISR/回调、`send()`、RX/TX 队列保持有界，不引入 malloc、阻塞等待或把介质私有字段泄漏给 Core。
7. V5-41 才能把这张默认表改称为“目标板标定值”；在此之前它只是保守的官方初始 Preset。

## 10. 资料依据与现实边界

- [Espressif ESP-NOW API](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32/api-reference/network/esp_now.html)：默认 ESP-NOW bit rate 为 1 Mbit/s，且可按接口/Peer 配置速率。
- [Espressif ESP-NOW FAQ](https://docs.espressif.com/projects/esp-faq/en/latest/application-solution/esp-now.html)：公开环境测试吞吐明显低于标称 PHY，证明 Wi-Fi Cost 不能只由标称速率决定。
- [Bosch CAN Protocols](https://www.bosch-semiconductors.com/products/ip-modules/can-protocols/)：经典 CAN 上限为 1 Mbit/s，CAN-FD 扩展每帧数据长度和数据相位能力；最终配置仍取决于控制器、收发器和总线拓扑。
- [USB-IF USB 2.0 速率说明](https://www.usb.org/sites/default/files/CabConn20.pdf)：Full-Speed 为 12 Mbit/s、High-Speed 为 480 Mbit/s；CDC 实际吞吐仍由端点和 Host 调度决定。

本表的整数 Cost 不是这些标准直接规定的数值，而是基于它们的速率边界、UCN 当前固定资源约束和“有线稳定优先、无线可用但需质量惩罚”的产品策略给出的可执行初值。只有 V5-41 的真实目标板数据才能决定最终默认值是否需要调整。
