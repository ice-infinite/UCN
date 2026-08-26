# Core、Types、Config、Time、Error API

> 文档级别：`REFERENCE`
> 实现状态：`CURRENT`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：include/ucn 公共头、链接目标与公共 API 测试
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：API 合同不等同于各平台实机驱动完成

- `ucn_types.h` 定义 Node ID、Endpoint、Link ID、Cost、序列号等基础域；广播和零值是否合法由具体字段合同决定。
- `ucn_config.h` 提供默认配置和产品覆盖入口；改变布局的宏必须全工程一致。
- `ucn_profile.h` 冻结 Nano/Lite/Full 依赖图。
- `ucn_time.h` 提供回绕安全 deadline/duration；duration 不得超过 `INT32_MAX`。
- 错误码区分参数、配置、状态、容量、链路、格式、访问和未找到等原因。调用者应按类别处理，不能把所有失败都当作重试。

公共函数通常返回 `UCN_OK` 或负错误码。除接口明确说明外，错误返回不改变调用者 output。

## 基础类型与保留值

| 类型/常量 | 宽度或含义 | 使用注意 |
| --- | --- | --- |
| `ucn_node_id_t` | 32-bit API 域 | 具体 Wire Class 可能只能表示更窄地址 |
| `UCN_NODE_BROADCAST` | `0xFFFFFFFF` | 只在允许广播的字段中合法，不能作为普通成员/next hop |
| `ucn_network_id_t` | 网络隔离域 | 接收帧必须与本地网络匹配 |
| `ucn_sequence_t` / `ucn_session_id_t` | 会话内重放身份 | 不允许重启后无意复用旧组合 |
| `ucn_route_cost_t` | 32-bit Route 累加域 | `UNKNOWN` 与 `MAX` 是保留边界 |
| `ucn_endpoint_t` | 业务/控制交付标识 | 静态/控制范围由 Endpoint 合同判断 |
| `ucn_traffic_class_t` | Q0～Q3 | 优先级不等于可靠性语义 |

零值是否有效不能靠 C 零初始化推断。例如 `deadline=0` 表示未武装，`duration=0` 非法；某些配置中的 0 表示继承 Node 默认；Node ID/Cluster ID 在多数身份字段中禁止 0，但应以具体 API validator 为准。

## 最小 Core 配置

```c
ucn_config_t cfg = {
    .network_id = 0x1001U,
    .node_id = 0x21U,
    .default_hop_limit = 8U,
};

ucn_result_t rc = ucn_validate_config(&cfg);
if (rc != UCN_OK) {
    /* 不要继续初始化 Node。 */
}
```

`network_id` 决定逻辑网络隔离，`node_id` 必须在产品地址规划中唯一，`default_hop_limit` 必须落在编译期 `UCN_MAX_HOPS` 内。手动 Node ID、Flash 配置或硬件派生都可以，但在入网前必须确定最终值。

## 时间 API 的正确用法

```c
uint32_t deadline = ucn_deadline_from_now(now_ms, 250U);
if (deadline == 0U) {
    return UCN_ERR_CONFIG;
}
if (ucn_deadline_expired(now_ms, deadline)) {
    /* timeout */
}
```

禁止直接使用 `now_ms >= deadline`，因为 32-bit 毫秒时钟会回绕。相对 duration 必须在 `1..INT32_MAX`；`ucn_elapsed_at_least()` 用于周期任务，`ucn_deadline_due_within()` 用于判断是否需要提前刷新。

## 错误码处理策略

| 错误 | 一般含义 | 调用方建议 |
| --- | --- | --- |
| `UCN_ERR_ARGUMENT` | 指针、枚举、字段域非法 | 修复调用，不自动重试 |
| `UCN_ERR_CONFIG` | 编译/产品能力或 Provider 合同不满足 | 记录配置错误，停止相关功能 |
| `UCN_ERR_NO_SPACE` | 固定表/队列满 | 有界背压、降载或稍后重试 |
| `UCN_ERR_TOO_LARGE` | payload/MTU/Class 不容纳 | 选择 Transfer/更小消息或其他路径 |
| `UCN_ERR_MALFORMED` / `VERSION` / `CRC` | Wire 不合法 | 丢弃并计数，不降级猜测 |
| `UCN_ERR_LINK_DOWN` | 当前发送链路不可用 | 触发选路/failover，避免忙等 |
| `UCN_ERR_SECURITY` / `REPLAY` / `ACCESS` | 认证、重放或授权失败 | fail-closed，按安全策略告警 |
| `UCN_ERR_STATE` | 当前 FSM 不允许操作 | 检查调用顺序，不强制覆写状态 |
| `UCN_ERR_EXHAUSTED` | serial 接近 no-wrap 阈值 | 建立新 identity/rekey，不能继续累加 |

## 所有权与线程

这些基础 helper 多数是纯函数或只读，但 Node/Transfer/Cluster 等对象的状态 API仍只允许唯一 Owner 调用。返回的版本字符串为库拥有；调用者提供的 output buffer 由调用者拥有；错误时是否写回以各 API 的明确合同和测试为准。
