# Cluster Current API

> 文档级别：`REFERENCE`
> 实现状态：`CURRENT`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：include/ucn 公共头、链接目标与公共 API 测试
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：API 合同不等同于各平台实机驱动完成

Current API 覆盖 Cluster v3/32 B 的初始化、step、receive、状态 view、Role/Phase/Epoch、成员/恢复/合并和 Persistence 配置。Cluster storage 与 API 版本必须匹配，避免应用依赖私有结构布局。

初始化的 persistence mode 默认为 REQUIRED；无合法 Provider 时 fail-closed。`VOLATILE_TEST` 必须显式选择，只能用于测试。

生产 v3 Backup/Takeover Authority 帧已有接收围栏。Current API 可用于现有兼容状态机，不能据此启用 Wire v4 或实验 Archive。

## Storage 与初始化

公共 `ucn_cluster_t` 是 opaque handle。静态 MCU owner 只在一个实现文件包含 `ucn_cluster_storage.h` 并分配 storage，其他模块只持有指针和只读 view，避免依赖可变布局。

`ucn_cluster_config_t` 至少配置本地身份、enable/head-capable、控制帧保护、score/capacity、时间参数、`now_ms`、send callback 和 persistence。推荐使用具名初始化；结构末尾会随版本增加字段。

```c
ucn_cluster_config_t cfg = {0};
cfg.local_node_id = local_id;
cfg.enabled = true;
cfg.head_capable = true;
cfg.require_protected_control = true;
cfg.head_score = product_score;
cfg.member_capacity = 8U;
cfg.now_ms = product_now_ms;
cfg.now_context = &clock;
cfg.send = product_cluster_send;
cfg.send_context = &bridge;
cfg.persistence_mode = UCN_CLUSTER_PERSISTENCE_REQUIRED;
cfg.persistence_provider = &provider;

ucn_cluster_config_apply_timing_profile(&cfg,
                                        UCN_CLUSTER_TIMING_DEFAULT);
rc = ucn_cluster_init(cluster, &cfg);
```

默认/零 persistence mode 是 REQUIRED。Provider 不兼容、Record 损坏或启动迁移失败时 init fail-closed 并清空对象。Host 单测要用 volatile 必须显式选择 `VOLATILE_TEST`。

## 运行循环

Cluster 不直接读取 Node 私有表。Owner 周期同步邻居：

```c
ucn_cluster_sync_node_neighbors(cluster, &node);
ucn_cluster_step(cluster);
```

Cluster Endpoint 收到固定 32 B v3 payload 后调用：

```c
ucn_cluster_receive(cluster, source_node_id,
                    frame_was_protected,
                    frame->payload, frame->payload_length);
```

send callback 把 Cluster payload 封装到 Core Endpoint/Q0；它必须有界返回，不能在 Cluster step 中阻塞链路。

## Authority 的唯一判断

应用不得用 `role == HEAD` 推断写权限，必须调用 `ucn_cluster_authority_active()`。`authority_is_managed()` 表示 M08 Authority Owner 是否安装，用于扩展在兼容模式下区分只读行为，不是启用 v4 的开关。

Role、Phase、Epoch view 用于诊断；`active_epoch_get()` 返回原始投影，不验证当前 liveness/Authority。

## 只读状态

- `ucn_cluster_get_view()`：Role/Phase/Epoch/Persistence/Authority/Fence；
- `copy_member_summaries()`：有界复制成员；
- `get_member_summary_at()`：按固定槽查看；
- `get_member_capacity_view()`：runtime/voter capacity；
- `get_stats()`：选举、Join、Lease、持久化和拒绝计数。

这些 API 只在 Owner 上下文读取。远程诊断应复制 snapshot 后再发，不要跨任务遍历对象。

## Persistence 行为

REQUIRED 模式会在 promise 前 submit Record；PENDING 时 Cluster TX/RX/step 被 Fence，Owner 后续 poll；COMMITTED 后 reload/journal 验证再继续。Provider I/O 回调有重入门。持久化成功后的 Link `NO_SPACE/DOWN` 是传输错误，可有界重试，不应被记为 persistence fault。

## 当前边界

- Wire v3 固定 32 B、Type 1～19；
- v3 Backup/Takeover Authority 接收在生产入口拒绝，历史桥仅用于隔离测试；
- Wire v4 40 B encoder/production RX/FSM 未放行；
- Config/Takeover/Handover/Rekey 独立组件不能通过 Current API 自动生效；
- Flash 双槽/掉电和 MCU 完整资源仍待实机验收。
