# UCN V5-35 标准 Preset Resolver 实现报告

> 文档编号：DOC-040。
> 状态：代码与 Host 软件验证完成；驱动、实机和动态 Cost 接入未完成。
> 分支：`codex/v5-adaptive-wire`，线协议版本仍为 v5；本任务不修改任何线上帧。

## 1. 本任务交付

V5-35 新增公开、SDK 无关的静态配置层：

- [`include/ucn/ucn_standard_adapter.h`](../../../include/ucn/ucn_standard_adapter.h)：Bearer 类型、标准 Preset、静态配置/解析结果和两个公开 Resolver API。
- [`src/transport/ucn_standard_adapter.c`](../../../src/transport/ucn_standard_adapter.c)：只读 Preset 表和确定性边界检查；无动态分配、无 GPIO、无 RTOS/SDK 类型、无驱动回调。
- [`tests/test_standard_adapter.c`](../../../tests/test_standard_adapter.c)：覆盖所有 Preset、关键默认值、覆盖顺序、MTU/Carrier/地址/偏置/非法枚举拒绝。

它解决“产品不再手写零散基础 Cost”的配置问题；它**不**创建 `ucn_link_t`、不开 UART/CAN/Wi-Fi/USB，不改变现有 `route_cost`、Route、Path、Policy 或 Wire 载荷。

## 2. 公开 API 与产品调用顺序

```text
ucn_standard_preset_resolve(preset, &profile)
    -> 查询固定 bearer / 速率 / 基础 Cost / RTT 参考 / 最大逻辑 MTU

ucn_standard_link_config_resolve(&product_config, &resolved)
    -> 校验并应用单条 Link 覆盖
    -> 后续标准 Adapter 才可把 resolved 绑定到 BSP/HAL、固定队列和 ucn_link_t
```

解析优先级固定为：`Preset 默认 base_cost/RTT/最大 MTU` → `单条 Link 显式 base_cost/RTT 覆盖与所请求 MTU`。`administrative_bias` 仅原样保存为静态产品策略输入；LC-1 动态评分尚未读取它。

`required_logical_mtu=0` 不表示“任意长度”，而是请求当前 `UCN_MAX_FRAME_BYTES`。若该值超过 Preset 的能力，返回 `UCN_ERR_CONFIG`，防止隐式截断：CAN-FD 需显式 `<=64 B`，ESP-NOW 默认 Preset 需显式 `<=250 B`。

## 3. Preset 分类和硬门

| 分类 | 当前 Resolver 状态 | 关键门禁 |
| --- | --- | --- |
| UART / RS-485 | 已解析静态表 | RS-485 固定包含半双工基础偏好；尚无串口 Adapter。 |
| CAN-FD | 已解析静态表 | 最大逻辑 MTU 固定为 64 B。 |
| 经典 CAN | 已解析静态表 | 必须明确 `carrier_enabled=true`，否则返回 `UCN_ERR_CONFIG`；真正 Carrier 仍是 V5-40。 |
| ESP-NOW / Wi-Fi / USB CDC | 已解析静态表 | Preset 只给静态事实；没有 ESP-IDF 或 USB 驱动。 |
| Ethernet、BLE、802.15.4、私有 2.4G、FSK、LoRa、UWB、SPI、I2C、受控 LAN Tunnel | 已解析为 `CONDITIONAL` 元数据 | 不代表已有 Adapter、自动注册或自动选路；具体实现仍属于 V5-42。 |

共同拒绝条件包括：零 `local_link_id`、广播 Peer Node、非法物理地址、未知 Preset、超 Preset MTU、无 Carrier 的经典 CAN、无效基础 Cost/RTT 覆盖和越界管理偏置。

## 4. 验证证据

| 构建/测试 | 结果 |
| --- | --- |
| Full Debug + Config Contract | CTest `4/4` 通过。 |
| Lite Debug | CTest `1/1` 通过。 |
| Nano Debug | CTest `1/1` 通过。 |
| Full + `tests/config/ucn_test_user_config.h`（`UCN_MAX_FRAME_BYTES=128`） | Resolver 专用可执行文件编译、链接并 CTest `1/1` 通过。 |
| WSL GCC 13.3 Full Debug + Config Contract | CTest `4/4` 通过，含标准 Resolver 主测试。 |

MSVC 在既有 `ucn_endpoint.h`、`ucn_security.h` 仍报告源文件代码页 C4819；本任务没有新增该类警告。所有验证均为 Host 软件证据，不代表 MCU Flash/RAM、实际总线吞吐或无线可靠性。

## 5. V5-45 后续问题已闭环

首次用 128 B 产品头运行完整历史 `ucn_tests` 时，`test_frame`、Wire/Profile、AODV、动态压力和 Service 的少量用例仍隐含默认 `UCN_MAX_FRAME_BYTES=256`。Core 与本 Resolver 已能在该配置下编译，专用 Resolver 链接测试也通过；失败原因是旧测试常量没有从当前配置派生。

该问题已由 **V5-45** 闭环：Frame 最大 Payload 现在同时受当前帧头/Tag、`UCN_MAX_FRAME_BYTES` 与 `UCN_MAX_PAYLOAD_BYTES` 约束；Wire/AODV 测试从当前 Hop 配置推导；Service 测试按当前 Binding/Validator 容量选择可用子集。默认 Full/Lite/Nano 回归不变，Full + 128 B 产品头完整 CTest `2/2` 通过。

动态 32 Node×4 Link 压力的前提被显式检查。代表性产品头只有 `UCN_MAX_LINKS=3`，因此该场景会清楚报告跳过，仍执行固定资源边界测试；这不是把 32×4 压力误记为低资源产品已经验证。V5-45 没有修改 Core、MTU 门禁、Preset 数值或 Wire 格式。详见 [V5-45 产品配置全量回归报告](UCN_V5_45_产品配置全量回归报告.md)。

## 6. 后续顺序

1. V5-37：四类 MCU/RTOS Protocol Owner 与 Host Fake HAL 外壳。
2. V5-38 / V5-39 / V5-40：按实际硬件先后实现 UART/ESP-NOW、CAN-FD/USB 和经典 CAN Carrier。
3. V5-44 后再做 V5-36：动态指标/评分与策略接入；不能把本任务的静态 Preset 误称为动态自动选路。
