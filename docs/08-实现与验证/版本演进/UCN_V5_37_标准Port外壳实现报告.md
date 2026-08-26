# UCN V5-37 标准 Port 外壳实现报告

> 当前迁移说明：集中式实现已由 V5-46 拆分，底层 Port Ops 又由 V5-62 升级为 API V2。本文只保留阶段实现证据；当前产品必须填写 `struct_size/api_version` 并全量重编译，详见 [V5-62 修复报告](UCN_V5_62_Port_API_V2与审计缺陷修复报告.md)。

> 文档编号：DOC-043。
> 状态：历史软件验证；集中式 API 未发布，当前架构请以 DOC-044 / V5-46 为准。
> 分支：`codex/v5-adaptive-wire`；不包含真实 MCU SDK、驱动或实板测试。

## 1. 历史交付内容

- 曾新增集中式 ucn_standard_port.h/.c 与 test_standard_port.c，验证唯一 Owner 的基本运行顺序。
- V5-46 已删除这些未发布文件，改为 ucn_protocol_owner.* 与 include/ucn/ports、src/ports 下的独立平台 Port；当前细节见 [DOC-044](UCN_V5_46_平台Port解耦重构报告.md)。

## 2. 已冻结运行合同

```text
完整帧 ISR / 驱动回调
  -> owner_rx_enqueue()：复制到 Adapter 固定 RX 队列，成功后可通知
  -> 唯一 Protocol Owner
       -> adapter_rx_pump(固定数量)
       -> service_protocol_bridge_step_at(可选、固定数量)
       -> node_step(同一个 now_ms)
```

- ISR/回调不得直接运行 `ucn_node_receive()`、`ucn_node_step()` 或 Bridge。
- 每轮 Owner 从 `ucn_port_ops_t.now_ms` 采样一次；Bridge 与 Node 使用同一值。
- Owner 等待会裁剪到 `UCN_MAX_STEP_INTERVAL_MS`；Bare metal 不允许走等待 API，应由 Super Loop 定期执行 `step()`。
- `ucn_node_step()` 的 `UCN_ERR_NOT_FOUND` 表示无待处理工作。Owner 保留原始值到 `last_node_step_result`，同时向周期调度者返回 `UCN_OK`，避免空闲循环误判失败。
- Service OFF 时 Bridge 字段、包含和执行路径都会由预处理裁剪；Nano/Lite/Full 都可链接公共 Owner API。

## 3. Host Fake 覆盖

`test_standard_port.c` 使用纯 C99 Fake 时钟、临界区、通知、等待和 Link：

1. 验证五种模式的初始化规则，且 FreeRTOS/Zephyr/NuttX/Host Fake 缺通知或等待 Hook 时失败关闭。
2. 动态 Mesh 先经 HELLO 准入；随后两帧数据从 ISR/普通回调进入队列，单轮最多 Pump 一帧，业务回调只在 Owner 上下文触发。
3. 验证成功入队才通知、过大帧被拒绝、临界区成对、等待上限被裁剪。
4. Service ON 时建立固定 Router/Bridge，确认 Remote TX 只由 Owner 的 Bridge 阶段发送，Bridge 与 Node 都使用同一 `now_ms`。

## 4. 软件验证结果

| 配置 | 结果 |
| --- | --- |
| Windows MSVC Debug Full + 配置契约 | CTest `4/4` 通过。 |
| Windows MSVC Debug Lite | CTest `1/1` 通过。 |
| Windows MSVC Debug Nano | CTest `1/1` 通过。 |
| Windows MSVC Debug Full, `UCN_FEATURE_SERVICE=OFF` | CTest `1/1` 通过。 |
| Windows MSVC Debug Full + 128 B 产品配置 | CTest `2/2` 通过。 |
| WSL GCC 严格告警 Full + 配置契约 | CTest `4/4` 通过。 |

MSVC 输出继续存在既有 `ucn_endpoint.h`、`ucn_security.h` 的 C4819 本地代码页警告；本任务未新增该问题。

## 5. 明确未完成项

- 没有创建 FreeRTOS Task、Zephyr Thread、NuttX pthread/Work Queue 或任何实际同步对象；Runtime Hook 由产品映射。
- 没有实现 UART、ESP-NOW/Wi-Fi、CAN/CAN-FD、USB CDC 驱动或 Carrier；后续由 V5-38～V5-42 按平台实现。
- Host Fake 不证明 ISR 并发、任务优先级、WCET、栈/Heap、功耗、无线吞吐或实机多板可靠性。那些仍须目标板与真实介质测试。

## 6. 相关入口

- [V5-37 实施方案](UCN_V5_37_标准Port外壳实施方案.md)
- [标准 Port / Adapter 与默认 Cost 基线](../../06-平台与适配/UCN_标准Port_Adapter封装与默认Cost基线方案.md)
- [Adapter 契约](../../06-平台与适配/UCN_Adapter_契约.md)
- [任务表](../../00-项目管理/00-任务表.md) · [项目操作记录](../../00-项目管理/01-项目操作记录.md)
