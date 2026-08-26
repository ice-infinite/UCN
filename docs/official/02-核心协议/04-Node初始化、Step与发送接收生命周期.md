# Node 初始化、Step 与发送接收生命周期

> 文档级别：`NORMATIVE GUIDE`
> 实现状态：`CURRENT`
> 事实源：`ucn_node.h`、Node 实现和 integration tests
> 最近核对：`a093862`，2026-08-25

## 初始化顺序

```text
1. 选择全局 Profile/配置
2. 静态分配 ucn_node_t storage
3. 填写 network_id、node_id、default_hop_limit
4. ucn_node_init()
5. 配置 Wire Profile、Security、Join/诊断授权
6. 初始化 Link/Adapter/Source
7. ucn_node_register_link()
8. 注册 Endpoint 或通用 RX handler
9. 进入唯一 Owner 的事件循环
```

Network ID、Node ID 必须非零，Node ID 不能为广播，默认 Hop 必须在编译上限内。初始化失败的对象不得继续使用。

## `step(now_ms)`

`ucn_node_step()` 推进：

- 到期 HELLO/Heartbeat/路由发现/重试；
- Neighbor/Route/Candidate/Path 老化；
- Q0/Q1 发送调度；
- 维护帧预算；
- Pending Deadline 和故障恢复。

队列有数据时无需等 HELLO。事件 Runtime 应立即唤醒 Owner；即使无事件，也应在最大 Step 间隔内兜底调用。

## 发送

`ucn_node_send_endpoint()` 面向静态 Endpoint；`ucn_node_send()`/Path API 提供更底层请求。请求包含目标、Traffic Class、Delivery Semantic、Payload 及可选路由/策略语义。

返回 `UCN_OK` 只表示本地 Node 接受该发送请求，不表示远端收到或执行。

## 接收

完整 UCN Frame 经 Adapter 进入 `ucn_node_receive()`。Node 依次处理长度/Profile、Frame Codec、网络域、重复/安全、控制类型、目标/转发和 Endpoint 分发。

接收 API 必须由 Owner 调用；ISR 只写 Source Ring。

## 关闭和重启

Link 的 open/close、Driver 停止和对象内存归属由产品决定。节点重启必须提供新的或持久安全的 Session/Sequence 语义，不能因清零 RAM 重用旧安全域。

## 1. 初始化前需要准备的对象

| 对象/配置 | 谁提供 | 必须保持多久 |
| --- | --- | --- |
| Node storage | 产品唯一 owner | Node 整个生命周期 |
| `network_id/node_id/hop` | 产品配置 | init 时复制或按头合同保持 |
| Port/time ops | Port/BSP | Owner 运行期 |
| Link ops/context | Adapter/Driver | Link 注册期间 |
| Security ops/context | 产品 Provider | 启用安全期间 |
| Endpoint handlers | 应用/Service | 注册期间 |

建议先让 Driver 处于不会继续产生 ISR 的安全状态，再初始化 Source/Ring 和 Node，最后打开 RX 中断。

## 2. 注册顺序为什么重要

Endpoint 在 Link 后注册或 Link 在 Endpoint 后注册都可能由 API 允许，但进入事件循环前必须全部完成。如果 RX 中断过早开启，Owner 可能收到合法业务帧却找不到 Endpoint，或收到 HELLO 时 Link 尚未注册。

## 3. Step 的四类工作

### 维护时间

检查 HELLO、Heartbeat、Neighbor、Route、Candidate、Path 和诊断 transaction 的 deadline。

### 发送调度

在 Q0、Q1 和到期维护帧之间按固定预算选择工作，调用 Link send，并处理同步接受、背压和 Link Down。

### 故障收敛

移除过期 Neighbor/Bearer、生成 RERR、清理依赖 Route/Path/Policy，并唤醒等待路线的请求。

### 统计与门禁

记录 step gap、延迟、失败和容量，执行安全/Feature/状态检查。

`step()` 不是固定周期才“看一眼队列”的低频轮询；事件到来会立即唤醒，周期调用用于定时器和漏通知兜底。

## 4. 发送调用的结果

概念伪代码：

```c
rc = ucn_node_send_endpoint(node, destination, endpoint,
                            traffic_class, payload, length);
if (rc == UCN_OK) {
    /* Node 已接受；若要远端执行结果，继续等待 Service Result。 */
} else if (rc == UCN_ERR_NO_SPACE) {
    /* 按业务 deadline 决定重试或丢弃旧状态。 */
}
```

精确签名以头文件为准。调用者不能在 `UCN_OK` 后立即认定目标舵机已经动作。

## 5. 接收的严格顺序

1. Adapter 提交完整 frame bytes 和 ingress Link；
2. Node 探测 Wire Profile/精确长度；
3. Codec 校验 CRC 和字段；
4. 检查 Network ID、接收 Profile、重复和安全；
5. 如果是控制 Type，进入对应 handler；
6. 如果 Destination 是本地，分发 Endpoint；
7. 否则检查 Hop/Route/Path并转发。

错误发生在哪一步，就增加相应统计；不能在 CRC 未通过前创建 Neighbor 或 Route 状态。

## 6. Link 动态上下线

Driver 报 Link Down 后，Owner 更新 Link/Bearer 状态，Route/Path 模块撤销依赖出口。Link 恢复时重新进入 HELLO/准入或产品固定链接流程。不要在 Driver ISR 中直接删除 Route。

## 7. 安全重启

明文测试可以使用新的随机/启动 Session。生产安全必须从持久化 Provider 获得不会与旧密钥域冲突的 Session/Sequence。若无法证明单调性，节点应保持安全未就绪并拒绝要求保护的 Endpoint。

## 8. 最小启动验证

启动后依次检查：版本/Profile、Node ID、Link 数/MTU、Endpoint 数、Security Ready、Owner step gap，然后进行本地编码、两节点直连和故障重连。不要直接从多跳/Cluster 开始排错。
