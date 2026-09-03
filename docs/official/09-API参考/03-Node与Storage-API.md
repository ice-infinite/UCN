# Node 与 Storage API

> 文档级别：`REFERENCE`
> 实现状态：`CURRENT`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：include/ucn 公共头、链接目标与公共 API 测试
> 最近核对：当前工作区，2026-09-02
> 硬件状态：API 合同不等同于各平台实机驱动完成

Node API 覆盖初始化、Link/Endpoint 注册、接收提交、发送、`step(now_ms)`、路由/邻居维护和只读状态复制。`ucn_node_t` 的静态存储由调用者持有，初始化前必须使用与当前 API/配置匹配的 storage。

Owner 线程调用绝大多数 Node API。ISR/驱动通过 Adapter Source 或 Event Runtime 投递，不直接执行路由、业务 handler 或发送状态机。

`send` 成功表示消息已被本地协议接受，不一定表示远端业务已执行。Q0～Q3、Transfer 和 Service 的完成语义分别查看对应 API。

快照/诊断复制使用调用者 buffer；容量不足返回明确错误或截断标志，不暴露 Node 私有布局。

## 生命周期与所有权

`ucn_node_t` 是调用者分配、Node Core 管理内容的长期对象。产品可静态分配或放在 RTOS task context 中；不要复制一个运行中的 Node，也不要让不同编译配置访问同一 storage。

推荐顺序：

```text
分配并清零产品对象
  → ucn_validate_config()
  → ucn_node_init()
  → 设置 Wire/Security/Join/诊断策略
  → 初始化并注册每个 Link
  → 注册 Endpoint/Route/Path/Policy
  → 启动驱动与 Owner
  → 循环 receive()/step()
```

安全、Wire 和 Link 本地接收上限应在流量进入前配置。运行中改变会影响路径可表示性，必须通过相应 setter，让 Node 重新验证缓存状态。

## 最小初始化

```c
static ucn_node_t node;

ucn_config_t cfg = {
    .network_id = 1U,
    .node_id = 0x101U,
    .default_hop_limit = 8U,
};

if (ucn_node_init(&node, &cfg) != UCN_OK) {
    product_fail_safe();
}
ucn_node_set_plain_session_id(&node, boot_session);
```

Plain 部署也应提供非零、重启不复用的 session；启用 `security_required` 时，还必须先安装满足合同的 Provider 与策略，否则协议流量会 fail-closed。

## 发送 API 的层次

| API | 用途 |
| --- | --- |
| `ucn_node_send()` | 按 message type 发送 Core 数据/控制语义 |
| `ucn_node_send_endpoint()` | 业务推荐入口，以 Endpoint 区分同节点数据 |
| `ucn_node_send_path()` | 已知 Wire Path ID 的低层/测试发送 |
| `ucn_node_enqueue()` | 完整 `ucn_send_request_t`，控制 delivery/flags 等 |
| `ucn_transfer_send()` | 超过单帧或需要完成确认的大消息 |
| Service Bridge | 命令/结果和本机/远端统一语义 |

`UCN_OK` 通常表示本地接受/入队。若需要知道远端完整接收，用 Transfer completion；若需要知道远端任务执行完成，用 Service Result/应用 ACK。

## 接收与 step

驱动/Adapter 得到完整 Wire frame 后，在 Owner 上调用：

```c
ucn_node_receive(&node, ingress_link, bytes, length);
```

Node 会做 Wire、网络、重复、TTL、安全、路由和 Endpoint 分发。`ingress_link` 必须是已注册 Link，不能传另一个 Node 的 Link 对象。

Owner 还要频繁执行：

```c
ucn_node_step(&node, now_ms);
```

它推进 TX、重试、Heartbeat、路由/邻居过期和维护。队列通知可让数据立即触发 Owner；最大 step 间隔只是保底，不应等待 Heartbeat。

## Storage/API 版本

Node storage 与容量宏强相关。公共 storage helper/版本门禁用于阻止旧布局与新 API 混用；应用不要读取私有数组来“省一次 API”。需要状态时调用 `get_stats()`、Route quality、Neighbor summary、snapshot/diagnostic API。

当前 Layout Version 为 8。`tx_enqueued_by_class[] → tx_scheduled_by_class[] → tx_queue_sent_by_class[]` 是 Node 四级 Queue 的同口径漏斗；`tx_sent_by_class[]` 则统计所有成功 Link 提交，包含即时发送与转发，不能直接和 Queue-only 计数相减。

## 错误与恢复

- init 失败：对象不能投入运行；
- register Link `NO_SPACE`：增加容量或减少实例，不能覆盖旧 Link；
- send `NOT_FOUND`：可触发 route discovery；
- send `NO_SPACE`：Q0 仅在显式 Retry+Deadline 时有界保留；Q1 Latest 可覆盖同 key；Q2/Q3 FIFO 不覆盖；
- receive `MALFORMED/SECURITY/REPLAY`：丢弃并统计，不交业务；
- `LINK_DOWN`：更新 Link 状态并让选路处理，应用不要无限重试同一 Link。
