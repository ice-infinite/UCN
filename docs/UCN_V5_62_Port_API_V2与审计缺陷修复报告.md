# UCN V5-62 Port API V2 与审计缺陷修复报告

> 日期：2026-08-14  
> 状态：源码、Host 单元/模拟、跨 Profile、产品配置、Sanitizer 与静态分析门禁已完成。  
> 发布策略：UCN 尚处于预发布优化期，本项允许源码与 ABI 破坏；Wire Header、消息编号和 `UCN_PROTOCOL_VERSION=5` 不变。

## 1. 决策

本项不继续维持“任何旧初始化方式都能编译”的假兼容。旧产品对象必须用同一套新头文件全部重编译，旧源码必须显式迁移。这样换取三个确定性收益：

1. Port 配置先校验结构大小和 API 版本，再读取任务/ISR 回调，旧对象或错误头文件组合失败关闭；
2. Transfer 的发送、接收、ACK、重试和回收只从一个权威单调时钟取时间，不再接受调用方缓存的第二份 `now_ms`；
3. 经典 CAN 完成一条 Carrier 后先提交完整 UCN 帧，再处理后续物理帧，同 CAN ID 连续 START 不会覆盖已完成槽。

本项没有改变任何线上字节。因此不把 `UCN_PROTOCOL_VERSION` 改为 6；Port API 另以 `UCN_PORT_OPS_API_VERSION=2` 标识。若未来改变 Wire 字段、消息长度或接收解释，才必须升级 Wire 版本或提供显式协商。

## 2. Port API V2

`ucn_port_ops_t` 的前两个字段固定为：

```c
uint16_t struct_size;
uint16_t api_version;
```

所有公共 Owner、Event Runtime、Adapter、Stream Source 和 CAN Source 初始化都会调用 `ucn_port_ops_is_compatible()`。当前要求：

```text
ops != NULL
struct_size >= sizeof(ucn_port_ops_t)
api_version == UCN_PORT_OPS_API_VERSION
```

新产品必须使用 C99 具名初始化：

```c
static const ucn_port_ops_t g_port_ops = {
    .struct_size = (uint16_t)sizeof(ucn_port_ops_t),
    .api_version = UCN_PORT_OPS_API_VERSION,
    .now_ms = product_now_ms,
    .random_bytes = product_random_bytes,
    .enter_critical = product_enter_task,
    .exit_critical = product_exit_task,
    .enter_critical_from_isr = product_enter_isr,
    .exit_critical_from_isr = product_exit_isr,
};
```

旧六字段/八字段位置初始化不再受支持。不能把旧静态库与新头文件混合，也不能只重编译应用而保留旧 Port/Adapter 对象。迁移时必须清理构建目录并重编译 UCN Core、所选 Port、Adapter、Transfer 和产品固件。

## 3. Transfer 权威时钟

`ucn_transfer_config_t` 新增强制字段：

```c
ucn_transfer_now_ms_fn now_ms;
void *now_context;
```

初始化示例：

```c
static uint32_t transfer_now_ms(void *context)
{
    (void)context;
    return product_monotonic_ms();
}

ucn_transfer_config_t cfg = {0};
cfg.node = &g_node;
cfg.now_ms = transfer_now_ms;
cfg.now_context = NULL;

ucn_result_t result = ucn_transfer_init(&g_transfer, &cfg);
```

`now_ms` 为空时初始化返回 `UCN_ERR_ARGUMENT`。回调必须返回单调递增、允许 32 bit 自然回绕的毫秒时钟，并与 Node/Port 观察同一时间域。

Step API 由：

```c
ucn_transfer_step(&g_transfer, cached_now_ms);
```

改为：

```c
ucn_transfer_step(&g_transfer);
```

Send、目标 RX 回调和每次 Step 都重新采样该回调。这样系统已经运行很久后才初始化/发送，或 Transfer 长时间空闲后再次发送时，Deadline 都建立在当前时间，不会使用初始零值或陈旧缓存并立即超时。

## 4. 经典 CAN 连续 Carrier

接收顺序冻结为：

```text
物理段进入 Frame Ring
  -> Owner 严格重组当前 Carrier
  -> 一旦 Slot 完成，停止本轮物理帧消费
  -> 下一轮先把完整 UCN 帧提交 Adapter Queue
  -> 提交成功后才处理下一条物理帧/START
```

只有 `active && !complete` 的旧 Slot 能被同来源新 START 作为显式重启替换。`complete=true` 的 Slot 在提交成功或确定性拒绝前保持所有权；防御分支会保留下一条物理帧并返回 Pending。公共 Adapter Queue 满时仍保留完成 Slot，等待后续 Owner Round 重试。

## 5. 兼容与影响范围

| 范围 | 结果 |
| --- | --- |
| v5 Wire Header/控制帧/Data/Transfer Fragment/ACK | 不变，线上互通语义未改。 |
| `ucn_port_ops_t` 源码/ABI | 破坏；所有初始化必须增加 V2 头，全部对象必须重编译。 |
| `ucn_transfer_config_t` 源码/ABI | 破坏；必须配置权威时钟。 |
| `ucn_transfer_step()` 源码/函数 ABI | 破坏；删除外部 `now_ms` 参数。 |
| CAN Source 内部调度 | 行为修复；不改变 C1/C2 Carrier 格式。 |
| 已烧录旧测试固件 | 不会被远程自动升级；下一次构建必须迁移 API 后再烧录。 |

## 6. 软件验证

新增/更新的关键回归：

- Port 正确 V2、过短 `struct_size`、错误 `api_version` 和公共头链接；
- Transfer 缺时钟拒绝、系统 uptime=100000 ms 初始化后立即发送、长空闲跳到 500000 ms 后再次发送；
- 两条最小经典 CAN Carrier 共 8 个物理帧在一次 Drain 前连续到达、使用同一 CAN ID，验证两条都交付、无静默重启；
- 原有 CAN Queue 背压、乱序、超时、Bus-Off、Transfer 重试/窗口/MTU/两跳用例继续回归。

结果：

| 门禁 | 结果 |
| --- | ---: |
| Windows Full Debug + 配置契约/Scale | 14/14 |
| Windows Lite Debug | 11/11 |
| Windows Nano Debug | 1/1 |
| Windows Full Service OFF | 11/11 |
| Windows 128 B/3-Link 产品配置 + 配置契约 | 5/5 |
| WSL GCC ASan+UBSan | 1/1 |
| WSL GCC `-fanalyzer` | 构建通过，1/1 |

以上是 Host 软件证据，不代表真实 FreeRTOS ISR mask、CAN 仲裁/Bus-Off、收发器电气、目标 MCU 资源或外部 ESP32 Bench 已重新验证。实机迁移和验收继续归 V5-61/EXT-07。

## 7. 发布前迁移清单

1. 搜索全部 `ucn_port_ops_t` 初始化，改为具名 V2 初始化；
2. 搜索全部 `ucn_transfer_config_t`，配置同一产品单调时钟；
3. 将所有 `ucn_transfer_step(x, now)` 改为 `ucn_transfer_step(x)`；
4. 删除旧构建产物并全量重编译，不混用旧 `.a/.lib/.o`；
5. 逐个目标板验证 Task/ISR 临界区、Transfer 高 uptime/长空闲、连续 CAN Carrier 与 Queue 背压；
6. 只有实际板级结果完成后，才更新对应实机报告。
