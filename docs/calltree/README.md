# UCN 调用关系树（Call Tree）

> 数据依据：`E:\File\MESH\UCN` 当前 v4 Core 的 `include/ucn/`、`src/` 与测试入口。
> 目的：回答“一个 API 被谁调用、它继续调用什么、在哪个上下文运行、会经过哪些固定队列/回调”。源码是最终事实；本文档不替代源码或测试。

本目录参考 `E:\File\PlatformIO\F405_Zephyr_Parachute\docs\calltree` 的组织方式：以 YAML 为调用关系源数据，按模块拆分，节点使用唯一 ID，关系只记录真实的直接调用、回调或固定队列边界。

## 1. 先从三条主链读起

```text
物理 RX 回调
  → adapter.ucn_adapter_rx_enqueue
  → adapter.ucn_adapter_rx_pump
  → node_runtime.ucn_node_receive
  → frame.ucn_frame_decode / 安全校验 / 控制处理 / 转发 / Endpoint 分发
  → service.ucn_service_protocol_bridge_endpoint_rx
  → service.ucn_service_deliver_remote
  → 目标 Service/Task Inbox

业务 Task 远端发送
  → service.ucn_service_send 或 ESP FreeRTOS Port::send
  → service.ucn_service_protocol_bridge_step
  → route.ucn_node_send_endpoint
  → auto / pinned / AUTO_BALANCE 选路
  → route.ucn_node_send 或 route.ucn_node_send_path
  → security.protect_outbound_business
  → frame.ucn_frame_encode
  → Link ops->send

唯一 Protocol Task 周期调度
  → adapter.ucn_adapter_rx_pump（有限帧数）
  → service.ucn_service_protocol_bridge_step（有限请求数）
  → node_runtime.ucn_node_step
  → Q0/Q1/Pending Q1；连续业务达到上限且必要维护到期时插入一个维护槽；最后才是诊断
```

“唯一 Protocol Task”是最重要的并发边界：业务 Task、驱动回调和 ISR 不直接并发访问 `ucn_node_t`。业务 Task 只调用 Service/Port；驱动 RX 回调只入 Adapter 固定队列。

## 2. 目录和阅读顺序

```text
docs/calltree/
├── README.md                         本文件：范围、主链、阅读方法
├── index.yaml                        模块索引和入口节点
├── _template.calltree.yaml           新节点模板
├── core.calltree.yaml                配置和 Endpoint 编号边界
├── frame.calltree.yaml               编解码、CRC、E2E AAD
├── adapter.calltree.yaml             物理地址、固定 RX Queue、Pump
├── node_runtime.calltree.yaml        Node 初始化、收包总入口、step 调度
├── neighbor.calltree.yaml            HELLO、准入、Heartbeat、Bearer 主备
├── route.calltree.yaml               自动 Route、RREQ/RREP/RERR、业务发送
├── policy_path.calltree.yaml         Policy、受控 Path、Strict/Failover/Balance
├── security.calltree.yaml            Provider、ACL、端到端保护/透明转发
├── service.calltree.yaml             Router、Bridge、任务 Inbox/Remote TX
└── diagnostic.calltree.yaml          Path Trace、Node Snapshot、Policy Diagnostic
```

建议阅读顺序：`index.yaml` → `node_runtime` → `adapter`/`frame` → `route`/`neighbor` → `service`。只有产品要启用固定路径、均衡、加密或低频管理查询时，再进入 `policy_path`、`security`、`diagnostic`。

## 3. YAML 节点语义

每个文件都含 `nodes:`。节点 ID 格式为 `模块.符号或概念`，例如 `route.ucn_node_send_endpoint`。

| 字段 | 含义 |
| --- | --- |
| `type` | `function`、`callback`、`queue`、`scheduler`、`security_provider` 或 `module`。 |
| `layer` | `application`、`adapter`、`core`、`routing`、`security`、`service`、`diagnostic`、`external`。 |
| `called_by` | 已知上游入口；`external.*` 表示由产品/驱动/业务调用。 |
| `calls` | 下游节点，`type` 说明是直接调用、回调、队列或外部操作。 |
| `notes` | Q0/Q1、并发、固定容量、权限或验证边界。 |

不是每一个 `static` 字节序/数组辅助函数都单独建节点；它们会写入对应主函数的 `calls` 或 `notes`。这样树重点呈现协议行为，而不是 C 语法细节。

## 4. 当前范围和边界

- 覆盖当前 C99 Core 和其公开 API 的主要可达路径；ESP32 FreeRTOS Port 是 Core 外产品层，在 `service.calltree.yaml` 中只标明边界，不把板级 C++ 代码伪装成 Core。
- `ucn_node_receive()` 的控制帧分支很大，按“接收总入口 → 邻居/路由/Path/诊断/业务转发”拆到不同模块，以避免重复写同一函数。
- `ucn_security_ops_t` 是产品实现的函数指针边界。树记录 Core 何时调用它，但不会虚构 AEAD、密钥库或 Flash Provider 的内部调用关系。
- 本树描述调用关系，不等价于成功送达、可靠 RPC 或真实无线时延；验证结论仍以 `tests/`、`docs/00-任务表.md` 与 `docs/01-项目操作记录.md` 为准。

## 5. 更新规则

新增或调整 Core 的公开 API、主状态机、回调、队列或调度顺序时：

1. 更新受影响的 `*.calltree.yaml`；
2. 更新 `index.yaml` 的模块入口或状态；
3. 在项目任务表、操作记录和 UCN 知识库留下同步记录；
4. 检查节点 ID 唯一、文件路径存在、`calls.target` 有明确模块归属。
