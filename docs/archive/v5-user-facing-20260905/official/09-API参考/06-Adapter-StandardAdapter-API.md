# Adapter 与 Standard Adapter API

> 文档级别：`REFERENCE`
> 实现状态：`CURRENT`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：include/ucn 公共头、链接目标与公共 API 测试
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：API 合同不等同于各平台实机驱动完成

Adapter Queue 统一承接来自 Stream、CAN、Wi-Fi 或自定义驱动的完整 Core frame；Owner 从队列取出并调用 Node RX。队列是有界的，满载返回背压而不是覆盖未处理帧。

Standard Adapter 提供介质 preset、速率、基础 Cost、动态指标和 HELLO 调度的通用配置。Preset 只产生默认参数，产品可以覆盖，但必须保持单位和饱和规则一致。

Adapter 不拥有硬件驱动、引脚或 RTOS 对象；这些由 BSP/Port 配置持有。

## Adapter Queue 的职责

Adapter Queue 的输入必须是一条完整 UCN Wire frame 和对应 ingress Link。字节流去帧、CAN 重组、无线包校验应在 Source/Driver 层完成；Core 不负责猜测物理 framing。

```text
Driver/ISR
  → Source ring/packet queue
  → Source service 生成完整 frame
  → ucn_adapter_rx_enqueue[_from_isr]()
  → Owner 调用 ucn_adapter_rx_pump()
  → ucn_node_receive()
```

`ucn_adapter_rx_queue_init()` 绑定固定 storage、临界区和 Node；enqueue 成功后队列拥有 frame 副本，调用者可以复用输入 buffer。队列满返回 `UCN_ERR_NO_SPACE` 并增加统计，不覆盖最旧 frame。

`enqueue_from_isr()` 必须使用 Port v2 的 ISR 专用临界区；如果 RTOS 锁无法从 ISR 安全调用，应让 ISR 只写 BSP ring，再在 Owner/Source service 中普通 enqueue。

## Peer address binding

Adapter address 用于把 MAC、CAN ID、串口逻辑地址等物理身份映射到 UCN peer。`ucn_adapter_bind_peer()` 写入固定 binding 表，重复/冲突/容量满必须明确拒绝。它不等于安全认证；无线 MAC 与 Node ID 的绑定仍应由 Join/Security Policy 验证。

## HELLO Scheduler

Scheduler 负责每个直连 Adapter 的 HELLO 时序和 admitted policy，不做全网广播。典型流程为 init→restart→周期 step；是否发送、何时重启由 Link up/down 和 Owner 时间决定。快速 liveness profile 必须和 suspect/remove 参数共同验证。

## Standard Adapter Resolver

产品先选 preset，再可覆盖 base cost、RTT reference、MTU 等：

```c
ucn_standard_link_config_t cfg = {0};
ucn_standard_resolved_link_config_t resolved;

cfg.preset = UCN_STANDARD_PRESET_UART_921600_8N1;
/* 可选：cfg.override_base_cost / override_mtu ... */
rc = ucn_standard_link_config_resolve(&cfg, &resolved);
```

Resolver 验证 preset 与 override 的合法域，并给出 Bearer kind、bitrate、base cost、RTT reference、logical MTU 和 flags。Classic CAN preset 带 `REQUIRES_CARRIER`；Conditional preset 表示驱动合同还需产品确认。

## 接线边界

- Adapter/Resolver 不初始化 UART/CAN/Wi-Fi 外设；
- 不选择 GPIO、DMA channel、IRQ priority；
- 不创建 FreeRTOS queue/task；
- 不把物理 bitrate 当实际吞吐；
- 不自行声明 Link up，必须从 Driver status 获取。

## 最低测试

Queue 应覆盖空/满/回绕/ISR/失败不覆盖；binding 覆盖冲突与表满；preset 覆盖每个枚举、非法 override、MTU 下限；真实 Driver 还要做突发 RX、断链、重连和 metrics 时间戳测试。
