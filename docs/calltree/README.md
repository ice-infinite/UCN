# UCN 调用关系树（Call Tree）

> 数据依据：`E:\File\MESH\UCN` 当前 v5 V5-33 工作树的 `include/ucn/`、`src/` 与测试入口；`f941ae9` 是本轮修复前审计基线，v4 调用树由 `v4.0.0-final-before-v5` 标签保留。
> 目的：回答“一个 API 被谁调用、它继续调用什么、在哪个上下文运行、会经过哪些固定队列/回调”。源码是最终事实；本文档不替代源码或测试。

本目录参考 `E:\File\PlatformIO\F405_Zephyr_Parachute\docs\calltree` 的组织方式：以 YAML 为调用关系源数据，按模块拆分，节点使用唯一 ID，关系只记录真实的直接调用、回调或固定队列边界。

## 1. 先从三条主链读起

```text
物理 RX 回调
  → adapter.platform_port_rx_enqueue（选择一个平台的独立 Port）
  → adapter.ucn_adapter_rx_enqueue
  → adapter.ucn_protocol_owner_step
  → adapter.ucn_adapter_rx_pump
  → node_runtime.ucn_node_receive
  → frame.ucn_frame_peek_wire_profile / per-Link RX Ceiling
  → frame.ucn_frame_decode / 安全校验 / 控制处理 / 转发 / Endpoint 分发
  → service.ucn_service_protocol_bridge_endpoint_rx
  → external.product_service_validator（高风险远端 Q0，Router 入队前）
  → service.ucn_service_deliver_remote
  → 目标 Service/Task Inbox

业务 Task 远端发送
  → service.ucn_service_send 或 ESP FreeRTOS Port::send
  → service.ucn_service_protocol_bridge_step_at
  → service.bridge_submit_static（可选固定 Pending Q0，只重试 NO_SPACE）
  → route.ucn_node_send_endpoint
  → auto / pinned / AUTO_BALANCE 选路
  → route.ucn_node_send 或 route.ucn_node_send_path
  → security.protect_outbound_business
  → node_runtime.prepare_outbound_wire_profile_static
  → frame.ucn_frame_select_min_wire_profile（仅显式自动模式）
  → frame.ucn_frame_encode
  → Link ops->send

唯一 Protocol Task 周期调度
  → adapter.platform_port_step → adapter.ucn_protocol_owner_step（每轮仅采样一次 now_ms）
  → adapter.ucn_adapter_rx_pump（有限帧数）
  → service.ucn_service_protocol_bridge_step_at（有限请求数，共用本轮 now_ms）
  → node_runtime.ucn_node_step
  → Q0/Q1/Pending Q1；Q0 背压等待时仍可插入必要维护；最后才是诊断
```

“唯一 Protocol Task”是最重要的并发边界：业务 Task、驱动回调和 ISR 不直接并发访问 `ucn_node_t`。业务 Task 只调用 Service/Port；驱动 RX 回调只入 Adapter 固定队列。

这个所有权也体现在头文件中：指针/API 使用者只包含 `ucn_node.h`；实际静态分配 Node 的 Protocol Task owner 才包含 `ucn_node_storage.h`。调用树描述公开函数关系，不把存储头中的内部表项当作应用可调用接口。

S15 的 Bridge Pending 与 Core Q0 Retry 都只处理本机 Adapter TX Queue 的瞬时 `UCN_ERR_NO_SPACE`。它们有固定容量、次数和 Deadline；`UCN_OK` 只代表 Link Queue 接受，不是远端送达或执行确认。

S16 给这条循环增加产品时间契约：`UCN_MAX_STEP_INTERVAL_MS` 默认 10 ms，Node 观测实际最大 Gap/违规；业务 Burst、Neighbor、Bearer 上限会生成保守维护服务上界，不安全的 Profile 编译失败。调用树只描述该边界，真实 Task 抢占和 Link `send()` WCET 仍须目标板日志证明。

S19 给 Path 管理写状态增加独立门禁：目标 Node 只有依次通过 Security、产品 Path Authorizer 和按认证 `(Source, Session)` 的固定 Token Bucket 后才安装/撤销 Path。同一来源改变 Bearer 不会刷新额度；正常业务、Heartbeat/RREQ 使用不同预算。

S04 后，本调用树以默认 `FULL + Service ON` 展示完整可达关系。`LITE` 仍走动态 `ucn_node.c` 的 HELLO/Neighbor/Heartbeat/AODV/Security 主链，但 Candidate、Path、Policy 和 Diagnostic 分支在编译期不存在；`NANO` 改走 `ucn_node_nano.c`，只保留静态 Link/Route、Q0/Q1、转发和 Endpoint 分发，不存在自动 Mesh 主链。Service 树仅在 `UCN_FEATURE_SERVICE=ON` 时成立。关闭能力对应的高级 API 只进入 `ucn_profile_stubs.c` 并返回 `UCN_ERR_CONFIG`。

V5-10 后，默认发送仍固定 W3。产品只有显式调用 `ucn_node_set_wire_profile_auto(true)` 才进入自动最小档路径；HELLO 使用固定 TX 档并以 1 B 发布独立 Peer RX Ceiling，中继保留来源帧档位。业务发送先确定是否带 16 B Tag，再结合地址/Hop/Route/Path、Link MTU 和 Peer RX Ceiling 选档，最后才 Seal/Encode。

V5-17～V5-20、V5-22～V5-33 后，Wire 可表达与业务可用分开判断：Node/Policy 可限制 Hop、32 bit Cost 与已验证 RTT；线上 Cost 为 3/3/3/4 B；Pinned Path 使用逐跳安装的 `remaining_hops` 和共同 Profile/MTU 能力；PATH_INSTALL 旧 API 发送基础 8/11/14/17 B，capability API 发送扩展 11/14/17/20 B，接收端只接受这两组精确 Schema；未知 Q1 路线默认按 2→4→8→16 有界扩圈且 Pending 内部重试不刷新绝对 Deadline；Candidate 验证保持发现时的 Wire Profile；Ingress 在完整 Decode/CRC 前先用 3 B Prefix 执行 per-Link RX Ceiling，完整 Decode/Network 后再在 Security/状态前执行运行期 Hop Scope。动态 MTU 使用静态/状态最小值，Policy 与 AUTO_BALANCE 跟随逻辑 Neighbor 当前 Bearer；后续跳能力失败会撤销 Path 并回送 Path-RERR。V5-21 Authorized Class 仍阻塞于生产安全 S02，不在调用树中伪造执行分支。

V5-46 将“唯一 Protocol Task”拆为公共 `ucn_protocol_owner_*` 与独立 Platform Port：驱动/ISR 先调用当前平台的 `ucn_<platform>_port_rx_enqueue()`，成功入队后才通过该平台自己的 Hook 通知 Owner；平台 Step 再进入公共 Owner，在同一 `now_ms` 下有界 Pump、可选 Bridge、最后 Node Step。V5-48 继续把 `from_isr` 传至 Adapter Queue：任务入口使用任务锁，ISR 入口使用可恢复 token 的 ISR 锁；缺少 token 对即失败关闭，绝不将 ISR 回退到任务锁。裸机、FreeRTOS、Zephyr、NuttX、RT-Thread 与 Host Fake 彼此没有枚举或头文件耦合，Core 不创建 RTOS SDK 对象；空闲 `ucn_node_step()` 的 `UCN_ERR_NOT_FOUND` 只保留在 Owner 统计，不会把周期循环误报失败。

## 2. 目录和阅读顺序

```text
docs/calltree/
├── README.md                         本文件：范围、主链、阅读方法
├── index.yaml                        模块索引和入口节点
├── _template.calltree.yaml           新节点模板
├── core.calltree.yaml                配置和 Endpoint 编号边界
├── frame.calltree.yaml               编解码、CRC、E2E AAD
├── adapter.calltree.yaml             物理地址、固定 RX Queue、标准 Owner、Pump
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
- YAML 主图描述 `FULL + Service ON` 超集；Nano/Lite 的实际可达子集必须结合 `ucn_profile.h` 与 CMake 源文件选择判断，不能把 Full 的 Path/Diagnostic 分支当成低档 Profile 的运行时代码。
- `ucn_node_receive()` 的控制帧分支很大，按“接收总入口 → 邻居/路由/Path/诊断/业务转发”拆到不同模块，以避免重复写同一函数。
- `ucn_security_ops_t` 是产品实现的函数指针边界。树记录 Core 何时调用它，但不会虚构 AEAD、密钥库或 Flash Provider 的内部调用关系。
- 本树描述调用关系，不等价于成功送达、可靠 RPC 或真实无线时延；验证结论仍以 `tests/`、`docs/00-任务表.md` 与 `docs/01-项目操作记录.md` 为准。

## 5. 更新规则

新增或调整 Core 的公开 API、主状态机、回调、队列或调度顺序时：

1. 更新受影响的 `*.calltree.yaml`；
2. 更新 `index.yaml` 的模块入口或状态；
3. 在项目任务表、操作记录和 UCN 知识库留下同步记录；
4. 检查节点 ID 唯一、文件路径存在、`calls.target` 有明确模块归属。
