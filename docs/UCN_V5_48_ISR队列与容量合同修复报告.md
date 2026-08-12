# UCN V5-48 ISR 队列、Scale 容量与 Path 源兼容修复报告

> 状态：已完成（Host 软件）。对应任务 V5-48 / DOC-046；不替代真实 RTOS ISR、BSP、驱动或实板验收。

## 1. 修复范围

本报告关闭复审中的三项问题：

1. `from_isr` 之前只影响通知，Adapter RX Queue 仍使用任务临界区；
2. 128 B、`UCN_MAX_LINKS=3` 的产品头默认运行需要四条 Link 的 8 节点 tree Scale；
3. `ucn_path_forward_config_t` 扩展 capability 字段破坏既有八字段位置初始化。

没有改 Wire 版本、业务帧、路由算法、真实 Adapter、RTOS SDK 或外部 ESP32 工程。

## 2. ISR Queue 合同

```text
驱动 ISR/回调
  -> ucn_<platform>_port_rx_enqueue(..., from_isr=true)
  -> ucn_protocol_owner_rx_enqueue_from_isr()
  -> ucn_adapter_rx_enqueue_from_isr()
  -> enter_critical_from_isr() -> token -> copy -> exit_critical_from_isr(token)
  -> 成功后才通知 Protocol Task
```

- `ucn_port_ops_t` 保留 Task/Owner 的 `enter_critical()` / `exit_critical()`，并新增独立的 ISR token 对。
- `ucn_adapter_rx_enqueue_from_isr()` 只能使用 ISR token 对；缺失时返回 `UCN_ERR_CONFIG`，不会错误调用任务 mutex 或任务临界区。
- `ucn_adapter_rx_pump()` 只由唯一 Protocol Owner 在 Task/主循环上下文调用，继续使用任务临界区。
- 当前建议优先级不变：ISR 先写 BSP 自己的固定 ring，再让 Protocol Task 解码并调用普通入队；只有驱动已经形成完整帧、ISR 时间预算允许、且产品能正确实现 token 对时，才直接进入 Adapter Queue。

五个 SDK 无关 Port（FreeRTOS、Zephyr、NuttX、RT-Thread、Host Fake）都只负责把 `from_isr` 分流到公共 Owner；它们不创建 SDK Task、Queue 或锁。实际 SDK 映射、嵌套中断和最大 ISR 时长必须在目标产品工程验证。

## 3. Scale 容量合同

| 模式 | `UCN_MAX_LINKS >= 4` | `UCN_MAX_LINKS = 2..3` | 目的 |
| --- | --- | --- | --- |
| `--topology auto` | tree | line | CTest 常规 Smoke 永远只选可构造拓扑。 |
| `--topology tree` | 执行 tree | 返回 `SCALE_TOPOLOGY_UNSUPPORTED` | 人工要求的不满足拓扑必须明确失败。 |
| `--topology tree --topology-capacity-contract` | `SCALE_TOPOLOGY_CAPACITY_SUPPORTED` | `SCALE_TOPOLOGY_CAPACITY_SKIPPED` | CTest 明确记录 tree 是否因容量未运行。 |

8 节点三叉 tree 的内部节点需要“父节点 + 三个子节点”共四条 Link；因此 3-Link 产品改跑 line 是正确的容量选择，不是把 tree 测试伪装成已通过。

## 4. Path 配置的源码兼容

`ucn_path_forward_config_t` 现严格保持历史八字段：

```c
const ucn_path_forward_config_t legacy = {
    owner, session, path_id, destination, next_hop, remaining_hops,
    egress_link, expires_at_ms
};
```

不能只把 capability 追加到末尾：某些产品开启 `-Wmissing-field-initializers -Werror` 时，旧的八项初始化仍会被当作构建失败。最终方案如下：

- 原 `ucn_path_install(state, config)` 保持旧结构、旧语义，能力为空；
- 新 `ucn_path_install_capable(state, config, capability)` 独立接收 `ucn_path_capability_t`；
- Node 的 `ucn_node_install_local_path_capable()` 内部改调用该扩展 API；
- Lite/Nano 提供同名 Stub，`NULL` 状态/config 返回 `UCN_ERR_ARGUMENT`，其余返回 `UCN_ERR_CONFIG`。

这保证源码级旧位置初始化通过严格编译；所有使用该公共结构体的对象仍应整体重编译，不承诺跨旧/新静态库混用的二进制 ABI。

## 5. 验证证据

| 配置 | 结果 |
| --- | --- |
| Windows MSVC Debug Full + Scale + 配置契约 | CTest `14/14` 通过。 |
| Windows MSVC Debug Full + 128 B / 3-Link 产品头 | CTest `15/15` 通过。 |
| 128 B/3-Link 直接 Scale | auto 报告 `topology=line`；tree 容量合同报告 `CAPACITY_SKIPPED`。 |
| WSL GCC 13.3，`-Wall -Wextra -Wpedantic -Werror`，Full + Scale + 配置契约 | CTest `14/14` 通过。 |
| WSL GCC 13.3，ASan + UBSan + Leak Detection，Full + Scale + 配置契约 | CTest `14/14` 通过，无 Sanitizer 报错。 |

MSVC 只保留既有 `ucn_endpoint.h`、`ucn_security.h` 的 C4819 代码页警告；本任务未新增该问题。

## 6. 未覆盖的验收

- FreeRTOS/Zephyr/NuttX/RT-Thread 的真实 ISR 嵌套、通知语义、临界区 token 映射和 Driver DMA/ring；
- UART、Wi-Fi、CAN、USB 等 Adapter 的完整帧边界、吞吐、丢包、ISR WCET、RAM/栈/CPU/功耗；
- 真实多节点 tree/line 的收敛时间与实际介质性能。

后续实际平台接入继续归 V5-38～V5-42；本修复没有进行烧录、串口监测、提交或推送。
