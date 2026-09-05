# Service 与 Bridge API

> 文档级别：`REFERENCE`
> 实现状态：`CURRENT`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：include/ucn 公共头、链接目标与公共 API 测试
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：API 合同不等同于各平台实机驱动完成

Service Router 用 Service ID/Operation 区分同一节点上的多类数据和命令；Binding 将服务绑定本地 handler、任务队列或远端 Endpoint。Service 是可选 Feature，与 Nano/Lite/Full 正交。

请求、结果和 Guard 明确区分“协议送达”与“业务执行完成”。Bridge 允许本机任务和跨 MCU 使用相同语义：本机可走 Fast Path，远端仍经 Core/Transfer。

Binding 默认借用调用者提供的静态对象。注销前必须停止新请求并排空在途引用，避免悬挂 handler。

## Binding 设计

每条 `ucn_service_binding_t` 冻结：Endpoint、owner service/task、最大 payload、允许 traffic mask、Q0 FIFO 或 Q1 Latest、允许的本地 source、是否接受远端、启动 ready 和远端 Q0 validator 要求。

```c
static const ucn_service_binding_t bindings[] = {
    {
        .endpoint = IMU_ENDPOINT,
        .owner_service_id = SERVICE_SENSOR,
        .max_payload_length = sizeof(imu_sample_t),
        .allowed_traffic_mask = 1U << UCN_TRAFFIC_Q1_REALTIME,
        .delivery_mode = UCN_SERVICE_DELIVERY_Q1_LATEST,
        .allowed_local_source_mask = 1U << SERVICE_ESTIMATOR,
        .accept_remote = true,
        .enabled_at_boot = true,
    },
};
```

Router 借用该 `static const` 表，运行期间不能修改或释放。

## 本机 Fast Path

`ucn_service_send[_ex]()` 发现 destination 是本节点时，复制 payload 到目标 inbox，不经过 Frame/Route/Link。Q0 保留 FIFO 顺序，满时返回背压；Q1 只保留最新值并统计 overwrite，适合 IMU 等实时样本。

消费者调用 `ucn_service_inbox_take()`，按 owner service+endpoint 验证所有权。设置 `ready=false` 会清空该 binding inbox，确保任务重启后不执行旧命令。

## 远端 Bridge

远端发送先复制到 Router remote TX queue。Protocol Bridge step 取出消息，编码 Endpoint frame 并调用 Node；接收端的 Endpoint handler 经 Security/validator 后调用 `ucn_service_deliver_remote()` 进入本机 inbox。

```text
Task A send
  → Router owns copy (REMOTE_ROUTER_QUEUED)
  → Bridge/Node accepts (LINK_QUEUE_ACCEPTED)
  → Remote Bridge/Router inbox (REMOTE_INBOXED)
  → Remote task executes (REMOTE_EXECUTED Result)
```

前两级不证明远端收到，第三极不证明业务执行。需要执行结果时，命令携带 Guard/command_id/result endpoint，由远端返回 Result header。

## Command Guard 与 Result

Guard 包含 command ID、issued time、有效期、Result Endpoint 和 flags。接收任务调用 validator 拒绝过期/重复命令；如果节点时钟不共享，产品应改用 generation/lease，不得假设各 MCU uptime 相同。

Result 的 `REMOTE_INBOXED+ACCEPTED` 表示已入任务队列；`REMOTE_EXECUTED` 必须配合 SUCCEEDED/REJECTED/FAILED/EXPIRED 等终态。`ucn_service_result_matches_command()` 用于绑定原命令。

## 远端 Q0 安全

远端关键命令可要求 Bridge validator；没有对应 validator 时 handler 安装 fail-closed。Bridge replay state 按 source/session/command 阻止重放，session 轮换必须明确执行。Core Security/Endpoint ACL 仍是来源认证边界，Service 本地 mask 不能替代远端身份验证。

## 生命周期与并发

Router 访问由 Port/产品串行化；Bridge 在 Protocol Owner 上运行；业务任务只通过 inbox API。若要更换 binding，先拒绝新请求、等待 remote queue/inbox 清空、停止 handler，再销毁旧对象。

## 失败处理

- `NOT_FOUND/ACCESS`：Endpoint、source service 或 remote policy 不允许；
- `TOO_LARGE`：超过 binding/Service 固定 payload，改用 Transfer；
- `NO_SPACE`：Q0/remote queue 满；Q1 可能覆盖旧值而成功；
- Bridge Node send 失败：保留/重试策略必须有界，观察 outbound outcome；
- Result timeout：只说明没有收到业务结果，不能推断命令一定未执行，产品需要幂等 command ID。
