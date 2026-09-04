# Route、Path、Policy、Cost API

> 文档级别：`REFERENCE`
> 实现状态：`CURRENT`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：include/ucn 公共头、链接目标与公共 API 测试
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：API 合同不等同于各平台实机驱动完成

Route API 提供 AODV-Lite 发现、缓存和 RERR；Path API 支持安装固定转发段和按需 Trace；Policy API 在候选路径上实现自动、Pinned、Failover 和可选 Q1 负载均衡。

Cost API 接受 Link 指标并计算饱和总 Cost。动态质量变化更新缓存候选，不要求每次发包重新寻路。

Nano/Lite 中被裁剪的高级 API 必须明确返回 `UCN_ERR_CONFIG`，不能假成功。安装 Path 或策略前要验证目标、next hop、Bearer capability、MTU 和固定表容量。

## Route：按 Origin 与目的地自动寻路

应用通常只需要：

```c
rc = ucn_node_send_endpoint(&node, destination, endpoint,
                            UCN_TRAFFIC_Q1_REALTIME,
                            payload, length);
if (rc == UCN_ERR_NOT_FOUND) {
    ucn_node_discover_route(&node, destination, now_ms);
}
```

发现完成后，Route cache 只保存该节点的下一跳/Link/Cost/expiry，不保存完整全网路径。动态键为 `(route_origin,destination)`：本机新发流量使用本机 Node ID，转发流量使用业务帧 Source；静态 Route 以 Origin 0 显式通配。`ucn_node_route_pending()` 可判断本机是否已有发现事务；`refresh_route()` 用于在缓存接近失效时后台刷新，而不是每个包重寻路。

`ucn_node_get_route_quality()` 返回本机作为 Origin 时的 availability、hop、Cost 和可选 verified RTT；它是当前快照，不是远端送达保证。诊断多个转发域时使用 `ucn_node_copy_route_summaries()`，不要直接读取 `node.routes[]`。

## Route constraints

Node 默认 constraints 可限制最大 hop、Cost、verified RTT。Policy 中 0 值可继承默认。设置过严可能让所有候选不可用，正确结果是 `NOT_FOUND/STATE`，而不是悄悄放宽。

## Path：显式可追踪线路

Path 由控制器逐节点安装：每个 relay 只得到本地 `{owner/session/path_id,destination,next_hop,remaining_hops,lease,capability}`。源节点本地安装不代表中继已经准备好。

```c
ucn_node_install_local_path_capable(&node, path_id, destination,
                                    next_hop, remaining_hops,
                                    lease_ms, &bottleneck);
ucn_node_send_path_install_capable(&node, relay, path_id, destination,
                                   relay_next_hop, relay_hops,
                                   lease_ms, &bottleneck);
```

扩展 PATH_INSTALL 只能发给已知支持 capability schema 的 v5 节点；需要兼容旧 v5 时使用 base API。Path Trace 是按需诊断控制帧，返回完整经过节点，但必须有 authorizer、token 和 record limit。

## Policy：每个流量可有不同策略

Policy key 通常包含 destination、endpoint、traffic class，因此同一目标的舵机 Q0 可以固定 CAN，IMU Q1 可以负载均衡 UART/Wi-Fi，日志 Q3 可只走低优先级路径。

| 模式 | 行为 |
| --- | --- |
| `AUTO_BEST` | 从合格候选中选择当前最优，并使用滞回切换 |
| `PINNED_STRICT` | 只走指定 Path，断开即返回失败 |
| `PINNED_FAILOVER` | 指定 Path 正常时固定，断开才回落自动候选 |
| `AUTO_BALANCE` | 对 Q1 flow 进行有界分配，避免逐包乱序 |

Q1 balance 用 `ucn_node_bind_q1_flow()` 建立带 lease 的 flow→path 绑定；同一流保持路径稳定，拥塞连续达到门槛后才迁移。

## Cost API

`ucn_link_cost_resolve()` 输入 base、queue、TX/RX failure、RTT、busy/quality、freshness 和 administrative bias，输出是否 selectable、排除原因、每项 penalty 与最终 Cost。调用者应检查 `exclusion` 和 `base_cost_known`，不能只比较一个整数。

```c
ucn_link_cost_result_t result;
rc = ucn_link_cost_resolve(&input, &result);
if (rc == UCN_OK && result.selectable && result.base_cost_known) {
    use_cost(result.effective_select_cost);
}
```

切换 helper 只有候选至少改善 20% 才返回 true；完整 Policy 还会检查连续样本、probe 和 hold。

## 失败与诊断

- Route `NO_SPACE`：缓存/发现容量不足，保留现有项；`route_instance_table_full` 用于区分 Route Instance 表满；
- Path `ACCESS`：控制帧未授权；
- Path `CONFIG`：当前 Profile 不支持；
- Policy 无候选：不得退回违反 Pinned/安全/MTU 的 Link；
- Cost Unknown/stale：不能排在已知候选前；
- RERR：只通知依赖该下一跳/路由的前驱，不做无关全网广播。

调试时同时查看 route quality、path entry、policy flow、link cost breakdown 和 rejection stats，单看“最终走了哪条路”不足以判断选路是否正确。
