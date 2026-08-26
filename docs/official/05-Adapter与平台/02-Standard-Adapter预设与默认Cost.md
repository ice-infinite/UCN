# Standard Adapter 预设与默认 Cost

> 文档级别：`NORMATIVE REFERENCE`
> 实现状态：Resolver `CURRENT`；绝大多数真实 Driver 未内置
> 事实源：`ucn_standard_adapter.h/.c`
> 最近核对：`a093862`，2026-08-25

Standard Adapter 定义 SDK 无关的介质/速率 vocabulary，覆盖 UART、RS-485、Classic CAN、CAN-FD、ESP-NOW/Wi-Fi、USB CDC、Ethernet、BLE、802.15.4、私有 2.4G、FSK、LoRa P2P、UWB、SPI、I2C 和 IP Tunnel。

每个 preset 解析为：

- bearer kind；
- configured rate；
- base cost；
- RTT reference；
- maximum logical MTU；
- `REQUIRES_CARRIER`/`CONDITIONAL` flags。

## 优先级

```text
preset 默认 base/RTT/MTU
    ↓
每 Link 显式 override
    ↓
运行时 metrics 只生成本地动态 effective cost
```

Override 必须在合法范围，logical MTU 不能超过 preset capability，也不能小到放不下最小 W0 帧。

## 重要边界

Resolver 只计算配置，不打开 Driver、不注册 Link、不选择 GPIO。`ESP-NOW 1M` preset 存在并不表示仓库内有 ESP-IDF ESP-NOW Adapter。

预设是统一初始基线，不是所有硬件的最终真值。产品应以真实吞吐、RTT、失败率和稳定性标定 override。

## Resolver 实际做什么

产品给出 Bearer kind、配置速率和可选 override，Resolver 校验组合并生成一份 resolved config。它不会访问硬件，因此相同输入在 Host 和 MCU 上应得到相同结果。

```text
Bearer/Rate preset
  ↓ 校验该速率是否属于该介质
默认 base cost / RTT reference / maximum logical MTU
  ↓ 应用合法的 per-Link override
Resolved config
  ↓ 产品据此创建 Driver/Source/Link
```

Preset 标记 `REQUIRES_CARRIER` 时，说明物理单元小于普通逻辑 Frame，需要 Stream/CAN 等 Carrier；`CONDITIONAL` 表示最终能力取决于产品 Driver/SDK 模式，不能仅凭枚举宣称可用。

## 为什么默认 Cost 不能只按名义速率倒数

115200 UART、500 kbit/s CAN、Wi-Fi 的实际代价还包含半双工、仲裁、RTT、失败率、包长和功耗。Preset 给出的 Base Cost 是统一起点，使未标定产品能稳定比较；运行 Metrics 再在本地添加动态 penalty。

产品若希望控制命令偏好 CAN，可以用每 Link administrative/base override 或 Pinned Policy，而不是伪造 Wi-Fi RSSI。

## Override 优先级和约束

- Base Cost 必须是合法正值且不使用 Unknown 保留值；
- RTT reference 不能为 0；
- logical MTU 不能超过 preset 最大能力；
- logical MTU 必须能容纳最低合法 UCN Frame/Carrier 合同；
- override 只作用于该 Link 实例，不改变全局 preset；
- 运行期 Metrics 不覆盖 Base Route Cost，只产生本地 Effective Cost。

配置非法时 Resolver 返回 `CONFIG/ARGUMENT` 并不生成半有效结果。

## 产品使用示例

两条 UART 都是 3 Mbit/s，但 UART1 经板内短线，UART2 经长线 RS-485：可以使用同一 Rate Preset，再分别 override RTT/base/liveness。Preset 相同不表示两条 Link 运行质量相同；每实例 Metrics 和 Driver 状态仍独立。

## 新介质何时需要新增 preset

只有当项目需要一个跨产品稳定名称、速率等级和默认能力时才新增。一次性私有 Radio 可以先使用自定义 preset/override。新增官方项必须同时补：

- enum/descriptor；
- 合法速率与 MTU；
- base cost/RTT 的依据；
- Carrier/conditional flags；
- resolver 正负向测试；
- 官方规范和目标硬件标定计划。

## 验证清单

- [ ] 所有 preset/速率组合的合法与非法矩阵通过；
- [ ] override 越界不写 resolved output；
- [ ] 同输入跨编译器结果一致；
- [ ] preset 不打开 Driver、不选择 GPIO；
- [ ] 产品 evidence 记录 override 的真实测量依据；
- [ ] 动态 Metrics 没有写回线上 Base Cost。
