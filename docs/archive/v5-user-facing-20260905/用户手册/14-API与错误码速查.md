# API 与错误码速查

本页用于集成时快速找到“该调用哪一组接口”。完整语义仍以相应专题手册和公共头文件为准。

## 1. 初始化与生命周期

| 目的 | 主要 API | 调用上下文 |
| --- | --- | --- |
| 初始化 Node | `ucn_node_init()` | 启动阶段，Owner 尚未运行 |
| 配置 Link | 对 `ucn_link_t` 使用清零后的具名字段初始化 | 启动阶段；Link 没有独立 `init()` API |
| 注册 Link | `ucn_node_register_link()` | Protocol Owner |
| 安装静态路由 | `ucn_node_add_route()` | Protocol Owner |
| 推进协议 | `ucn_node_step(node, now_ms)` | 唯一 Protocol Owner |
| 接收入站帧 | `ucn_node_receive()` | 由 Adapter/Owner 调用，不在 ISR |
| 查询统计 | `ucn_node_get_stats()` | Owner 或复制后的只读快照 |

Node、Link、Adapter、Runtime、Service、Transfer 等对象由调用方静态提供。初始化成功后，业务任务不能绕过 Owner 并发修改 Node。

## 2. Endpoint 收发

| 目的 | API | 说明 |
| --- | --- | --- |
| 注册 Endpoint | `ucn_node_set_endpoint_handler()` | 一个 Endpoint 对应一个本地处理入口 |
| 立即提交消息 | `ucn_node_send_endpoint()` | 适合 Owner 上下文或已序列化调用 |
| 有界入队 | `ucn_node_enqueue()` | 业务任务交给 Owner 的常用方式 |
| 低层消息类型发送 | `ucn_node_send()` | 仅在产品明确需要 message type 时使用 |
| 指定 Wire Path 发送 | `ucn_node_send_path()` | 低层 provisioning/测试原语 |

Endpoint 建议只使用应用区间 `0x40..0xBF`。`UCN_OK` 只表示当前层接受，不表示目标业务执行。

## 3. 动态发现与路由

| 目的 | API |
| --- | --- |
| 观察/探测邻居 | `ucn_node_observe_neighbor()`、`ucn_node_probe_neighbor()` |
| 发送直连 HELLO | `ucn_node_broadcast_hello()` |
| 允许/拒绝邻居 | `ucn_node_admit_neighbor()`、`ucn_node_reject_neighbor()` |
| 查邻居数量/摘要 | `ucn_node_neighbor_count()`、`ucn_node_copy_neighbor_summaries()` |
| 主动寻路 | `ucn_node_discover_route()` |
| 刷新路由 | `ucn_node_refresh_route()` |
| 查询是否 pending | `ucn_node_route_pending()` |
| 查询路由质量 | `ucn_node_get_route_quality()` |

Lite/Full 才提供完整动态网络能力；Nano 对不支持的功能按配置合同 fail-closed。

## 4. Full Policy 与 Path

| 目的 | API |
| --- | --- |
| 配置路由策略 | `ucn_node_set_route_policy()` / `ucn_node_clear_route_policy()` |
| 配置本地 Policy Path | `ucn_node_set_policy_path()` / `ucn_node_clear_policy_path()` |
| 绑定 Q1 Flow | `ucn_node_bind_q1_flow()` |
| 查询 Link Quality | `ucn_node_get_link_quality()` |
| 查询 Policy Stats | `ucn_node_get_policy_stats()` |
| 安装本地转发 Path | `ucn_node_install_local_path()` |
| 安装带能力 Path | `ucn_node_install_local_path_capable()` |
| 下发基础 Path | `ucn_node_send_path_install()` |
| 下发带能力 Path | `ucn_node_send_path_install_capable()` |
| 撤销远端 Path | `ucn_node_send_path_revoke()` |

固定路径不是只在源节点写一个 ID。每个中继与终点都必须取得授权且一致的 Path 转发状态，租约到期或撤销后应停止使用。

## 5. Event Runtime 与 Adapter

| 目的 | API/对象 |
| --- | --- |
| 初始化 RX Queue | `ucn_adapter_rx_queue_init()` |
| Task 提交 Carrier | `ucn_adapter_rx_enqueue()` |
| ISR 提交 Carrier | `ucn_adapter_rx_enqueue_from_isr()` |
| 将队列泵入 Node | `ucn_adapter_rx_pump()` |
| 初始化标准事件 Runtime | `ucn_event_runtime_init()` |
| 标记 Source pending | `ucn_event_runtime_signal_source()` / `ucn_event_runtime_signal_source_from_isr()` |
| 运行一轮 | `ucn_event_runtime_run()` 或 `ucn_event_runtime_task_cycle()` |
| 查询统计 | `ucn_adapter_rx_get_stats()`、`ucn_event_runtime_get_stats()` |

具体函数是否可从 ISR 调用，以对应头文件的 `from_isr`、Port V2 临界区和平台 wrapper 合同为准；Node、Service、Transfer 不能在 ISR 中直接调用。

## 6. Service

| 目的 | API |
| --- | --- |
| 初始化 Router | `ucn_service_router_init()` |
| 设置任务 ready | `ucn_service_set_ready()` |
| 本地/远端发送 | `ucn_service_send()`、`ucn_service_send_ex()` |
| 任务取 Inbox | `ucn_service_inbox_take()` |
| 初始化 Bridge | `ucn_service_protocol_bridge_init()` |
| 推进 Bridge | `ucn_service_protocol_bridge_step()` |
| 统计 | `ucn_service_get_stats()`、`ucn_service_protocol_bridge_get_stats()` |

本地目标直接进入 Inbox；远端目标进入 Remote TX Queue，再由 Bridge 交给 Node。两者统一消息语义，但不强迫本机任务走完整网络栈。

## 7. Transfer

| 目的 | API |
| --- | --- |
| 初始化 | `ucn_transfer_init()` |
| 绑定 Endpoint | `ucn_transfer_bind_endpoint()` |
| 配 Peer class | `ucn_transfer_set_peer_capability()` |
| 配 Peer window | `ucn_transfer_set_peer_window_capability()` |
| 配 Peer concurrency | `ucn_transfer_set_peer_concurrency_capability()` |
| 发送 T32～T8K | `ucn_transfer_send()` |
| 推进分片事务 | `ucn_transfer_step()` |
| 释放接收 Slot | `ucn_transfer_release_received()` |
| 统计 | `ucn_transfer_get_stats()` |

T128 及以上发送期间，调用方缓冲必须保持不变直到 completion。Owner 应先推进 Core，Core 当前无工作时再推进 Transfer。

## 8. 安全

| 目的 | API |
| --- | --- |
| 安装 Security Provider | `ucn_node_set_security()` |
| 设置默认策略 | `ucn_node_set_security_policy()` |
| 设置 Endpoint override | `ucn_node_set_endpoint_security_policy()` |
| 启用 fail-closed 产品门禁 | `ucn_node_set_security_required()` |

产品必须实现真实 AEAD、密钥选择、RNG、序列持久化和授权；示例 Provider 不能直接作为量产安全实现。

## 9. 诊断

| 目的 | API |
| --- | --- |
| 路径追踪 | `ucn_node_request_path_trace()` |
| 节点快照 | `ucn_node_request_node_snapshot()` |
| Policy 分页诊断 | `ucn_node_request_policy_diagnostic()` |
| 授权路径追踪 | `ucn_node_set_path_trace_authorizer()` |
| 授权节点快照 | `ucn_node_set_node_snapshot_authorizer()` |
| 授权 Policy 诊断 | `ucn_node_set_policy_diagnostic_authorizer()` |

远程诊断默认拒绝、低频、有界，并且不能被当作永久全网拓扑同步机制。

## 10. 错误码与处理建议

| 返回值 | 含义 | 一般处理 |
| --- | --- | --- |
| `UCN_OK` | 当前层接受或当前动作完成 | 继续等待更高层结果；不要等同远端执行 |
| `UCN_ERR_ARGUMENT` | 空指针、非法 ID/枚举/组合 | 编程错误；修正调用，不盲目重试 |
| `UCN_ERR_CONFIG` | Feature/Profile/容量/配置不支持 | 修改构建或产品配置 |
| `UCN_ERR_NO_SPACE` | 固定队列、表或 Slot 已满 | 有界重试/背压/降频；检查消费端 |
| `UCN_ERR_TOO_LARGE` | 长度超过当前接口或 class | 选更大 Transfer class 或缩小消息 |
| `UCN_ERR_MALFORMED` | 帧结构非法 | 丢弃并统计；检查 Carrier/版本 |
| `UCN_ERR_VERSION` | 版本或 Wire Profile 不兼容 | 协商/升级配置，禁止降级猜测解析 |
| `UCN_ERR_NETWORK` | 网络层失败 | 查看路由、邻居和控制面统计 |
| `UCN_ERR_CRC` | 完整性校验失败 | 丢弃，检查物理误码与实现一致性 |
| `UCN_ERR_TTL` | 跳数耗尽 | 检查环路、最大跳数和拓扑 |
| `UCN_ERR_UNSUPPORTED` | 当前对象/API 不支持该能力 | 选择受支持路径或构建组件 |
| `UCN_ERR_LINK_DOWN` | 选中 Link 不可用 | 触发 Failover/重新寻路或上报业务 |
| `UCN_ERR_NOT_FOUND` | 路由、绑定、记录或当前工作不存在 | 视 API 语义决定发现、配置或正常空闲 |
| `UCN_ERR_SECURITY` | 认证、密钥或安全 Provider 失败 | fail-closed；查 Key/ACL/持久序列 |
| `UCN_ERR_REPLAY` | 重放或序列不前进 | 丢弃并审计，不自动接受旧数据 |
| `UCN_ERR_ACCESS` | Authorizer/权限拒绝 | 不重试轰炸；检查产品授权策略 |
| `UCN_ERR_STATE` | 当前 FSM/生命周期状态禁止操作 | 修正调用顺序；失败应保持对象不变 |
| `UCN_ERR_EXHAUSTED` | 有界 serial 到达 no-wrap 阈值 | 执行规定的 rotate/rekey/新身份流程 |

## 11. 调用上下文速查

| 上下文 | 可以做什么 | 不应做什么 |
| --- | --- | --- |
| ISR/SDK 回调 | 搬运固定数据、写 Ring/Queue、通知 | Node step、路由、安全、业务 handler |
| Protocol Owner | Adapter pump、Node、Service Bridge、Transfer、查询状态 | 阻塞等待业务或长时间打印 |
| 业务任务 | Service send/take、向 Owner 的产品队列提交请求 | 并发遍历或修改 Node 内部表 |
| 诊断任务 | 读取 Owner 复制出的快照 | 直接高频远程扫描全网 |

## 12. 头文件入口

- 总入口：`include/ucn/ucn.h`
- 基础类型/错误码：`include/ucn/ucn_types.h`
- Node/Endpoint/Route：`include/ucn/ucn_node.h`
- Link：`include/ucn/ucn_link.h`
- Adapter：`include/ucn/ucn_adapter.h`
- Full Policy：`include/ucn/ucn_policy.h`
- Service：`include/ucn/ucn_service.h`、`ucn_service_bridge.h`
- Transfer：`include/ucn/ucn_transfer.h`
- Security：`include/ucn/ucn_security.h`
- Runtime/Port：`include/ucn/ports/`
