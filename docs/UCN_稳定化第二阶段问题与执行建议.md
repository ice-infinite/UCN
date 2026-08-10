# UCN 稳定化第二阶段：问题、决策与执行建议

> 建立日期：2026-08-10
> 源码基线：`main@731e37e`
> 适用范围：UCN v4 C99 Core、Service Router/Bridge、Adapter 契约、软件模拟与产品 Port。
> 原则：MCU-first、Linux 可选、固定内存、有界执行；本文只把源码确认的问题转成任务，不把“建议”写成“已经实现”。

## 1. 本轮结论

上一轮已经完成 Deadline 回绕、Session Rotation 接口、控制面基础预算、Service 重启清理、Link `send()` 契约和基础压力测试，但还存在十类需要继续收口的问题：

| 问题 ID | 源码现状 | 是否解决 | 对应任务 |
| --- | --- | --- | --- |
| ST2-01 | Core Q0 和 Service Bridge 在 `UCN_ERR_NO_SPACE` 后会丢失已经出队的消息；发送失败统计还可能重复累计。 | 代码/软件测试已解决；实机待 S06/S07。 | S15 |
| ST2-02 | 调度已经按“业务帧数量”保证维护机会，但产品没有最大 `ucn_node_step()` 调用间隔和最坏维护时延契约。 | 代码/软件测试已解决；实机时序证据待 S06/S07。 | S16 |
| ST2-03 | Core 不负责 HELLO 调度；ESP 参考工程原来只有首次 100～400 ms 抖动，现已补每 Adapter Token、退避、最大尝试、准入后降频，并纳入 S05 动态压力。 | 代码/软件测试已解决；真实规模与参数标定待 S06。 | S17、S05 |
| ST2-04 | 12 B Command Guard 仍是可选格式；高风险远端 Q0 已由 Binding/Bridge 强制产品 Validator 在入队前校验。 | 代码/软件测试已解决；生产 Session/实机待 S02/S07。 | S18 |
| ST2-05 | `PATH_INSTALL/REVOKE` 已要求 Security Provider 和产品 Authorizer，但授权成功后的管理源没有独立入站速率预算。 | 已解决（S19 代码/软件测试）；实机故障注入待 S06。 | S19 |
| ST2-06 | 当前只区分本机投递与远端 Router 入队，缺少 Bridge/Core 后续失败的本机可观测完成事件；也没有通用远端 ACK。 | 软件已解决本机观察与业务结果关联；通用远端 ACK 明确不做，实机待 S07。 | S20、S15、S18 |
| ST2-07 | 生产身份、Join、真实 AEAD、Replay Window、密钥持久化/轮换/撤销仍未闭环。 | 必须解决，P0 发布门禁；不在通用 Core 自造密码算法。 | S02 |
| ST2-08 | Nano/Lite/Full 尚未真正裁剪对象和源码，`ucn_node_t` 仍为 Full 单体。 | 必须解决，P1 MCU 资源门禁。 | S04、S08 |
| ST2-09 | 原 32 节点压力测试仅使用静态环网和同步直投；现已新增动态 HELLO/AODV、丢失/重复/延迟/乱序、Cost/Down、表耗尽和 Service 压力场景。 | 代码/软件测试已解决；真实介质仍归 S06/S07。 | S05 |
| ST2-10 | T22.4 文档仍写成 Cost/RTT/失败率/队列压力混合评分，与当前 Known Cost × Flow、队列压力独立触发的源码不一致。 | 立即修正文档，不改协议。 | DOC-019 |

## 2. 统一决策边界

1. **Q0 仍不是可靠传输。** 只允许对 Adapter 固定 TX Queue 的瞬时 `NO_SPACE` 做有截止时间的本机重试；链路断开、无路由、权限拒绝、过大帧等终止错误立即失败。
2. **Q1 继续保持 Latest Value。** 不为旧 Q1 建重试积压；新值可以覆盖旧值。
3. **Protocol Task 必须有时间契约。** “每四个业务帧给维护一次机会”只有在 `ucn_node_step()` 被足够频繁调用时才成立。
4. **HELLO 风暴控制属于 Adapter。** Core 只处理协议状态；Wi-Fi、CAN、UART、BLE/LoRa 可以使用不同发现策略，静态点对点介质可关闭主动 HELLO。
5. **高风险命令在入 Service Inbox 前做第一道校验，执行 Task 再做第二道校验。** 通用 Core 不理解舵机、电机或传感器 Payload。
6. **管理控制帧必须“认证 + 授权 + 有界预算”后才能改状态。** 授权不代表可以无限频率写 Path 表。
7. **不把本机接受伪造成远端送达。** 通用远端 ACK 会引入新线状态、重发、幂等和 Q2 语义，本阶段不加入；关键命令使用结果 Endpoint 完成闭环。
8. **资源裁剪必须在对象和编译单元层生效。** 仅把数组大小设为 0 或依赖链接器消除，不算完成 Nano/Lite。

## 3. ST2-01：Q0 瞬时背压与消息所有权

### 已确认现状

- `ucn_node_step()` 调用 `ucn_node_send()` 后无论结果如何都会清除 `item->occupied`。
- `send_frame_on_link()` 对失败已经增加 `tx_error_dropped`，`ucn_node_step()` 又会对同一失败增加一次，存在统计重复。
- `ucn_service_protocol_bridge_step()` 先从 Router 取走 Remote TX，再直接调用 `ucn_node_send_endpoint()`；它不会进入 Core Q0 队列，所以 `NO_SPACE` 后同样没有所有权恢复点。

### 解决方案

- 默认保持现有 `BEST_EFFORT` 兼容模式；增加仅供 Q0 选择的 `RETRY_ON_BACKPRESSURE`。
- 固定策略必须同时配置：最大重试次数、短重试间隔和绝对截止时间；三者任一到达即失败。
- 仅重试 `UCN_ERR_NO_SPACE`。`LINK_DOWN/NOT_FOUND/SECURITY/TOO_LARGE/CONFIG` 等立即结束。
- Core Q0 项增加有界重试状态；Service Bridge 增加最多一个固定 Pending Q0 所有权槽，未完成时不继续从 Router 取下一个 Q0。
- 增加 `backpressure_retry_attempted/exhausted/terminal_failed` 等互斥统计，保证一次失败只进入一个最终分类。
- 观察回调在最终接受或最终失败时执行，不在每次内部重试时冒充完成。

### 测试门禁

- Link 前 N 次返回 `NO_SPACE` 后恢复：同一 Q0 只发送一次、顺序不变。
- 一直 `NO_SPACE`：在截止时间/次数到达后释放槽，不无限占用。
- `LINK_DOWN` 等终止错误：零重试。
- Q1 仍是 Latest Value，无旧值积压。
- Core 与 Bridge 的统计不重复；持续背压时维护控制面仍有机会。

## 4. ST2-02：Protocol Task 最大调度间隔

### 已确认现状

S01 已保证默认每 4 个业务发送后检查一次必要维护，但如果产品 Task 50 ms 才调用一次 `ucn_node_step()`，邻居和多 Bearer 越多，所有到期维护被轮转完的时间仍可能超过 SUSPECT 门限。

### 解决方案

- 新增产品 Profile 常量 `UCN_MAX_STEP_INTERVAL_MS`，建议 Full/Lite 默认上限 10 ms；裸机和 RTOS Port 都必须声明。
- Node 记录 `last_step_ms`、`max_step_gap_ms`、`step_interval_violations`；第一次调用不计违规。
- 用保守上界做配置检查：

```text
maintenance_service_bound_ms =
  (UCN_BUSINESS_TX_BURST_BEFORE_MAINTENANCE + 1)
  × UCN_MAX_STEP_INTERVAL_MS
  × UCN_MAX_NEIGHBORS
  × UCN_MAX_BEARERS_PER_NEIGHBOR
```

- 默认 `4、10 ms、8 Neighbor、2 Bearer` 的保守上界为 800 ms；叠加 1 s Heartbeat 后仍应小于 3 s SUSPECT 门限。若 Step 为 50 ms，上界达到 4 s，应由配置断言或产品检查拒绝。
- Port 文档同时冻结 Protocol Task 优先级、最大 Sleep/Block、Adapter Pump 预算、Bridge 预算和每个 Link `send()` 的 WCET。

### 测试门禁

- 连续业务、多 Neighbor、多 Bearer 下测量维护最大间隔。
- 注入超时 Step，确认违规统计准确且不修改线协议。
- 对合法与非法配置做编译/初始化测试；ESP32 实机记录真实最大 Gap。

### 2026-08-10 实现与反向核对

- **问题 → 配置：**`include/ucn/ucn_node.h` 新增 `UCN_MAX_STEP_INTERVAL_MS`（默认 10 ms）以及按 Burst、Neighbor、Bearer 上限计算的 `UCN_MAINTENANCE_SERVICE_BOUND_MS`；默认为 800 ms。编译期要求 `Heartbeat Interval + Maintenance Bound < Suspect Timeout`，50 ms 的故意非法档案已确认编译拒绝。
- **配置 → 运行时：**`ucn_node_step()` 首次调用只建立基线；后续使用无符号时间差记录 `last_step_ms`、`max_step_gap_ms` 和 `step_interval_violations`，跨 `uint32_t` 回绕有效；Heartbeat 与 Bearer/Path Probe 还记录相对到期时刻的最大服务延迟。运行时超限只记诊断而不中止已经迟到的 Node 调度。
- **运行时 → Port：**ESP32 参考端显式冻结 10 ms Step、1 ms 最大 Block、Wi-Fi/UART Pump 各 4 帧、Bridge 每轮 2 条、Protocol Task 最低优先级 1；启动日志输出这些值和 800 ms 上界，Link `send()` WCET 明确标为 `unmeasured`，未伪造实测值。`STAT` 输出 Step 最大 Gap/违规和 Heartbeat/Probe 最大服务延迟。
- **代码 → 测试：**`test_time.c` 覆盖首次、等于上限、超过上限和时间回绕；`test_neighbor_heartbeat.c` 在持续 Q0 下按当前 Profile 填充多 Neighbor/Bearer，验证所有到期 Heartbeat 在保守上界内完成且无误 Suspect/Remove。Debug、Release、64 B MTU、Bearer=1 均为 `1/1`；S3 A/B 与 WROOM 仅完成构建。
- **仍未闭环：**没有烧录或读取 COM；真实调度抢占、Wi-Fi/UART `send()` WCET、Heartbeat/Probe P50/P95/Max、栈/Heap 峰值继续由 S06/S07 提供。故本项只标“代码/软件测试完成”。

## 5. ST2-03：HELLO 启动风暴控制

### 已确认现状

ESP32 参考工程已将首次 HELLO 随机到上电后 100～400 ms，但周期发送是固定间隔；没有每 Adapter Token、指数退避、最大快速尝试，也没有在邻居准入后停止或降频。Core 本身没有主动 HELLO 调度器。

### 解决方案

- 在 Adapter 契约中增加发现状态机：`INITIAL_JITTER → FAST_RETRY → BACKOFF → ADMITTED_SLOW/STOP`。
- 每个 Adapter 独立固定 Token；加入初始/重试抖动、指数退避上限、最大快速尝试次数。
- 已准入邻居改由 Heartbeat 保活，HELLO 停止或进入低频恢复模式；静态 CAN/UART 点对点 Profile 可直接关闭主动 HELLO。
- 提供平台无关的小型调度 helper 或参考实现，随机数仍由 Port 提供，Core 不增加介质状态。

### 测试门禁

- 100 节点同一虚拟时刻启动，统计每 10 ms/100 ms 窗口的 HELLO 峰值、总量与入网 P50/P95。
- 丢 HELLO、拒绝准入、Adapter 重启、邻居掉线再发现、Token 恢复均可重复。
- 对比启用/关闭抖动的峰值，固定 Seed 可重放失败。

## 6. ST2-04：高风险远端 Q0 强制校验

### 已确认现状

Command Guard 当前只提供 12 B 可选 Payload 前缀的 encode/decode/validate。Binding 只有 `accept_remote` 等开关，Bridge 会先把帧交给 Router，再由产品 Task 决定是否调用 helper，因此不能保证“远端高风险 Q0 入队前必须校验”。部分产品 Payload 已自带命令序号和有效期，不能再强迫所有业务使用同一 12 B 格式。

### 解决方案

- Bridge 增加按 Endpoint 注册的产品 Validator；回调接收完整 `frame/source/session/endpoint/payload/now_ms`，不在 Core 中硬编码业务格式。
- Binding 增加“远端 Q0 必须校验”属性；若启用但没有注册 Validator，Bridge 安装 handler 或初始化必须失败关闭。
- Validator 在 `ucn_service_deliver_remote()` 前执行，拒绝帧不得进入 Router Inbox。
- 为采用 Command Guard 的产品提供固定 Replay 状态，键为 `(source Node, source Session, Endpoint)`；Session 变化允许命令序列重新开始。无共享时钟的产品用 Session/Generation + Command ID + 本地 Watchdog，不假定跨节点毫秒绝对同步。
- 执行器 Task 仍校验命令 ID、有效期和本地安全状态，Bridge 校验不能替代执行前检查。

### 测试门禁

- 高风险 Binding 无 Validator 时初始化失败；普通遥测 Binding 不受影响。
- 重复 ID、旧 Session、过期、非法长度、Validator 拒绝均不进入 Inbox。
- Session 切换、新命令、本机 Fast Path 和合法远端结果往返正常。
- 固定 Replay 表满时失败关闭，不动态分配内存。

### 当前落实（S18）

- `ucn_service_binding_t::require_remote_q0_validator` 只允许用于可接收远端的 Q0 Binding；Bridge 安装 Endpoint handler 前会核对所有强制项，缺任一个 Validator 都不会产生部分安装。
- `ucn_service_protocol_bridge_set_validator()` 在安装前按 Endpoint 注册固定槽回调。回调收到完整 Frame 及拆出的 Source/Session/Endpoint/Payload/Now，执行位置在 Router 锁与 `ucn_service_deliver_remote()` 之前；拒绝只更新 Bridge 统计/Observer，不进入 Inbox。
- `ucn_service_bridge_replay_state_t` 使用编译期固定深度。首次 `(source, session, endpoint)` 可建立槽；同源同 Endpoint 的其他 Session 默认拒绝，只有产品 Security 已认证后调用 `rotate_session()` 才能重置命令序号，旧 Session 随后仍被拒绝；表满返回 `UCN_ERR_NO_SPACE`。
- 12 B Command Guard 仍只是可选业务格式。虚拟测试同时覆盖 Guard 格式和 1 B 自定义格式；ESP32 R1 的 `0x60/0x61` 保持原 16 B ABI，先检查长度/模式/标志/范围，再在非零认证 Session 下使用 Replay。当前明文测试档案 Session=0 只做格式校验，不能作为生产防重放证据。
- 执行 Task 保留第二次产品检查，本机 Fast Path 不经过 Bridge Validator；普通 Q1 遥测完全不增加 Validator/Guard 开销。四个 C99 Profile 与 S3 A/B、Service A/B、WROOM 构建已通过，未烧录、未读取串口，真实命令/结果和安全 Session 留 S07/S02。

## 7. ST2-05：Path 管理控制预算

### 已确认现状

`PATH_INSTALL/REVOKE` 已要求 Security Provider 和 `path_control_authorize`，但授权通过后会直接修改固定 Path 状态；现有 RREQ/Heartbeat/Trace 入站预算没有覆盖这两类管理帧。

### 解决方案

- 在 Security `authorize_rx` 和产品 `path_control_authorize` 成功之后、Path 表发生变化之前，增加独立固定 Token Bucket。
- 预算键使用认证后的管理源 `(source Node, source Session)`，而不是仅按入站 Link；同一管理源换 Bearer 不能绕开限流。
- INSTALL 与 REVOKE 分类型统计；表满、授权拒绝、预算拒绝分别计数。
- 预算只限制写状态频率，不改变静态内存上限，也不阻塞正常业务/Heartbeat/RREQ 预算。

### 测试门禁

- 合法管理源洪泛、Token 恢复、INSTALL/REVOKE 隔离、不同管理源隔离。
- 同一源切换 Bearer 仍共用预算；Session 轮换后的处理遵守产品策略。
- 授权/预算拒绝前后 Path 表完全不变。

### S19 实施结果

- Core 顺序固定为 `frame decode → Security authorize_rx → 产品 path_control_authorize → 认证管理源预算 → Path 表修改`；Security 或产品授权拒绝都不创建预算槽。
- 默认 `UCN_PATH_CONTROL_RX_SOURCE_DEPTH=4`，每个活动管理源槽保存一个 `(source Node, source Session)`，INSTALL 与 REVOKE 各自拥有 4 Token，默认每 1000 ms 补 1 Token。来源键不含 Link，因此同一来源切换 Wi-Fi/UART/CAN Bearer 仍共用预算。
- 同一来源的新 Session 只有在 Security 与产品 Authorizer 已经接受后才替换旧预算代际并获得新突发额度；旧 Session 是否仍可用必须由产品 Security 拒绝，Core Token Bucket 不承担身份吊销或 Replay Window。不同来源独立占槽，60 s 不活跃来源在下一次管理请求时惰性回收，不增加周期扫描。
- `ucn_node_stats_t` 分开记录 INSTALL/REVOKE 授权拒绝、速率拒绝、管理源表满、Session 更新、惰性回收和 Path 表满。默认 4 槽的预算状态为 `4 × 24 B = 96 B`，8 个新增计数器为 32 B；均为 `ucn_node_t` 固定静态内存，无堆分配、等待或正常业务帧开销。
- `test_path_management_budget.c` 覆盖安全/产品授权顺序、同源交替 Bearer 洪泛、两种操作隔离、Token 恢复、不同来源、Session 更新、旧 Session 拒绝、源表满/回收和 Path 表满；所有拒绝均比较完整 `ucn_path_state_t` 前后不变。既有 `test_path_control.c` 四节点管理链继续回归透明中继和逐跳安装。
- Debug、Release、64 B MTU、Bearer=1 四个 CTest Profile 均为 `1/1`，`UCN_PATH_CONTROL_RX_SOURCE_DEPTH=1` 的最小来源档案也通过；ESP 参考 `STAT` 新增 `PATHCTL` 行，S3 A/B、legacy dual、WiFi-only、Service A、WROOM 六个环境构建成功。没有上传、复位或串口读取，真实控制洪泛对 Heartbeat/RREQ/业务的影响仍归 S06。

## 8. ST2-06：异步阶段与回执边界

统一使用以下阶段名称：

```text
LOCAL_INBOXED
REMOTE_ROUTER_QUEUED
LINK_QUEUE_ACCEPTED
REMOTE_INBOXED
REMOTE_EXECUTED
```

- 当前 `ucn_service_send_ex()` 只能证明前两项。
- S15/S20 增加 Bridge 本机 outbound observer，报告提交接受、背压耗尽、终止失败或过期；这仍不是远端确认。
- `REMOTE_INBOXED` 和 `REMOTE_EXECUTED` 由业务结果 Endpoint 返回，命令 ID 用于关联。
- 本阶段不新增通用 ACK、自动重传或“可靠 Q0”。未来若新增 Q2，必须单独设计幂等、窗口、重传、ACK 丢失和资源上限。

### S20 实施结果

- `ucn_service_async_stage_t` 冻结五个阶段；`ucn_service_acceptance_stage()` 只把同步 Acceptance 映射到前两项。
- 兼容旧 final-only Observer；新增结构化 Event Observer，显式区分 Link Queue 接受、未启用重试的立即背压拒绝、重试耗尽、Pending 过期和终止失败。失败事件的 Stage 为 `NONE`，不会伪造远端状态。
- 业务 Result Endpoint 可选使用 8 B 大端头：`command_id/stage/status/detail_code`。只有 `REMOTE_INBOXED+ACCEPTED` 与 `REMOTE_EXECUTED+终态` 合法；匹配 helper 同时核对 Command ID 与实际 Endpoint。
- Result Header 不进入普通帧或 Router 固定结构；Bridge 只增加一个 Observer 函数指针与 context。命令等待表、Source/Session 校验和 Deadline 继续由产品固定资源管理。
- 两节点模拟证明目标 Validator 拒绝时源端仍可能看到本机 `LINK_QUEUE_ACCEPTED`，但不会收到伪远端 ACK，最终由产品等待 Deadline 判超时。

## 9. ST2-07：生产安全工作流

S02 继续作为 P0 产品发布门禁，并与通用 Core 稳定化并行：

1. 产品威胁模型、设备身份和 Join/准入凭据。
2. 采用受审计库实现 AEAD Provider，Core 只调用 `seal/open/authorize/rotate_session`。
3. 固定 Replay Window 与 Counter 块预留/掉电持久化，禁止序号回退。
4. Session/Key 轮换、撤销、失联降级和恢复。
5. 控制面逐跳认证，保护 Hop Limit/Route Epoch 等 E2E AAD 未覆盖的可变字段。
6. 断电、旧包、伪造控制帧、坏 Tag、Key 撤销和资源耗尽测试。

不在 UCN 通用仓库自写未经审计的加密算法，也不把测试 Provider 当作生产安全。

## 10. ST2-08：Nano / Lite / Full 真裁剪

| Profile | 必备能力 | 明确不包含 |
| --- | --- | --- |
| Nano | Frame、Link、Endpoint、Q0/Q1、静态直连/静态路由。 | 自动发现、AODV、Path、Policy、Trace、Snapshot；它不是自动 Mesh。 |
| Lite | Nano + HELLO/Heartbeat/Neighbor/AODV-Lite + 最小 Security 接口。 | Candidate 先测后切、高级 Path/Policy/Balance、诊断快照。 |
| Full | Lite + Candidate/Path/Policy/Balance/Trace/Snapshot/Diagnostic。 | 生产密钥/算法仍由产品提供。 |

Service 是正交 Feature，可按产品是否需要节点内 Task 统一通信独立开启。实施要求：

- 拆分 public/internal 头和单体 Node 模块；未启用状态从 `ucn_node_t` 真正消失。
- CMake 不编译被关闭的模块；非法 Feature 依赖静态拒绝。
- CI 构建 Nano/Lite/Full 与关键组合，报告 `sizeof(ucn_node_t)`、静态 RAM、Flash 和最小 MTU。
- 默认 Full API 行为保持兼容；关闭功能时返回明确的编译或配置错误。

当前执行结果：S04 已按上述边界完成代码和 Host 软件验证。Nano 使用独立静态 Node，Lite 在预处理/构建层移除 Candidate、Path、Policy 和 Diagnostic，Service 可独立关闭；三档功能矩阵、最小帧 33/50/64 B、Full 回归及 GCC 严格构建均已通过。S08 又将 `ucn_node.h` 收口为 API/不完整类型，以 `ucn_node_storage.h` 让唯一 owner 显式取得静态布局，并明确 Seen Cache 不等价于生产 Replay Window。详细证据见 [S04 Feature Profile 与资源报告](UCN_S04_Feature_Profile与资源报告.md) 和 [S08 公共 API 与静态存储边界](UCN_S08_公共API与静态存储边界.md)。Host 结果不能替代 ESP32/STM32 ELF、栈/Heap 或真实介质测试，后者继续归 S06/S07。

## 11. ST2-09：动态网络压力场景

现有 `test_stress.c` 保留为快速静态路由环网回归，再新增确定性事件引擎：

- 动态 HELLO/准入/AODV，未知路由首包和 RERR 恢复。
- 丢包、重复、延迟、乱序、Cost 波动、Bearer Down/恢复。
- Neighbor/Route/Candidate/Path/Flow/Seen/Service/Bridge 固定表耗尽。
- Q0 背压重试、Q1 Latest 覆盖、维护公平和 HELLO 启动风暴。
- CI 短跑 + 手动/夜间百万级事件；失败必须输出 Seed 和最小事件轨迹。

核心不变量：无路由环、无无限重试、无状态泄漏、无重复业务投递、资源占用不超过编译上限、故障解除后在有界时间恢复。

## 12. ST2-10：文档漂移处理

当前实现的 T22.4 选择规则统一写成：

```text
已知基础 Cost ×（当前活动 Flow 数 + 1）
```

Unknown 不优于 Known；RTT 和失败率只保留为独立观测量，当前不进入裸加权评分；持续 Adapter Queue Pressure 只作为拥塞重绑触发条件。项目任务表和知识库专题必须同步修正，历史操作记录保留原文并由新记录注明已被当前实现取代。

## 13. 建议到任务的覆盖核对

| 建议 | 主任务 | 配套任务/文档 | 覆盖结论 |
| --- | --- | --- | --- |
| ST2-01 | S15 | S13、S20 | 代码/软件测试已闭环；实机待 S06/S07 |
| ST2-02 | S16 | S01、S06、S07 | 代码/软件测试闭环；真实时序与 WCET 待实机 |
| ST2-03 | S17 | S05、T18 | 代码/软件测试闭环；真实规模参数待 S06 |
| ST2-04 | S18 | S14、S02、S07 | 已建任务 |
| ST2-05 | S19 | S11、T22.1 | 代码/软件测试闭环；真实控制洪泛待 S06 |
| ST2-06 | S20 | S15、S18、S07 | 阶段、结构化本机最终 Observer 与 8 B 业务结果关联已完成软件闭环；实机待 S07，通用远端 ACK 明确不做 |
| ST2-07 | S02 | S06 | 复用 P0 任务并补充产品门禁 |
| ST2-08 | S04 | S08、S09 | 复用任务并冻结 Profile 边界 |
| ST2-09 | S05 | S09 | 动态事件网、固定资源耗尽、长跑入口与 sanitizer 已完成；远端 Actions/实机不在 S05 伪造 |
| ST2-10 | DOC-019 | 项目/知识库 T22.4 | 本轮立即修正 |

结论：十条建议均有唯一主任务或明确文档任务，没有遗漏，也没有把同一问题重复建立成互相竞争的实现。

## 14. 执行顺序

1. **S15（代码/软件已完成）**：Q0 背压所有权、Bridge Pending、最终 Observer 和重复统计已闭环，实机留 S06/S07。
2. **S16（代码/软件已完成）**：已冻结 Step 最大间隔、维护上界与运行时观测，实机时序留 S06/S07。
3. **S18（代码/软件已完成）**：高风险远端 Q0 强制 Validator 与固定 Replay 已闭环，生产 Session/实机留 S02/S07。
4. **S17（代码/软件已完成）**：Adapter HELLO helper、固定 Seed 状态测试、100 节点模拟和 ESP 参考接入已完成，真实规模参数留 S06。
5. **S19（代码/软件已完成）**：认证管理源固定预算、分类统计和拒绝不改表已闭环，实机留 S06。
6. **S20（代码/软件已完成）**：阶段枚举、结构化本机最终事件、Command ID/Result Endpoint 关联和无伪 ACK 边界已闭环，实机留 S07。
7. **S05（下一项）**：把上述能力合并到动态事件压力引擎。
8. **S04/S08/S09**：真裁剪 Nano/Lite/Full 并进入 CI/资源报告。
9. **S02** 与上述任务并行推进，但只有产品身份、密钥和硬件条件确定后才能完成。
10. 最后执行 **S06/S07** 的多板和 Task/Service 实机验收。

## 15. 每个代码任务的完成核对

每项任务只有同时满足以下条件才允许把“代码/软件测试”标为完成：

- [ ] 实现逐条对应任务表的“实现范围”，未偷偷扩大线协议。
- [ ] 新增单元测试覆盖成功、边界、表满、超时和错误分类。
- [ ] 新增模拟测试覆盖多节点/多 Link/调度交互。
- [ ] Debug、Release、64 B、Bearer=1 以及受影响的 Feature Profile 通过。
- [ ] `git diff --check`、文档链接和调用关系树通过。
- [ ] 操作记录写明源码、测试命令、真实结果和未验证边界。
- [ ] 再做一次“问题 → 任务 → 代码 → 测试”反向核对；缺任何一层不得关闭任务。
- [ ] 需要开发板的数据仍标为“实机待验证”，不能用虚拟测试替代。

## 16. S15 完成后的反向核对（2026-08-10）

### 16.1 问题 → 任务 → 代码 → 测试

| 原问题/约束 | 实现位置 | 软件证据 | 核对结论 |
| --- | --- | --- | --- |
| 默认行为必须兼容，不让所有 Q0 自动重试 | `ucn_types.h` 新增显式 `UCN_DELIVERY_RETRY_ON_BACKPRESSURE`；`ucn_node_enqueue()` 仅允许 Q0 且必须有非零绝对 Deadline | `test_link_contract.c` 覆盖默认 `BEST_EFFORT` 单次失败、Q1/无 Deadline 配置拒绝 | 已闭环 |
| Core 在瞬时 `NO_SPACE` 后不能丢失已排队 Q0 | `ucn_tx_item_t` 保存重试次数和下一尝试时间；`ucn_node_step()` 在固定次数、间隔、Deadline 内保留原 FIFO 槽 | 前两次 `NO_SPACE`、等待间隔不重复调用、恢复后成功；一直背压至 3 次重试耗尽 | 已闭环 |
| Bridge 已从 Router 取走的 Q0 必须有所有权恢复点 | `ucn_service_protocol_bridge_t` 增加一个固定 Pending Q0；`step_at()` 未终结前不取下一条 Router Q0/Q1 | 两条 Q0 验证前一条背压时后一条不能越序；恢复后 FIFO 投递 | 已闭环 |
| 只允许重试瞬时本机队列背压 | Core/Bridge 都只对 `UCN_ERR_NO_SPACE` 重试；`LINK_DOWN` 等终止错误立即完成 | Core 与 Bridge 均覆盖 `LINK_DOWN` 零重试 | 已闭环 |
| Q1 保持 Latest Value，不积压旧值 | 新语义在 Core 入口拒绝 Q1；Bridge Policy 只接管 Q0 | 非法 Q1 Retry 配置拒绝，既有 Service Q1 Latest 回归随全套 CTest 通过 | 已闭环 |
| 一条消息的最终失败不能重复计数 | 中间 `NO_SPACE` 仍有所有者时撤销临时 Drop；最终耗尽/终止/过期使用互斥统计 | Core/Bridge 检查最终 Drop、Observer 次数和精确统计值 | 已闭环 |
| 持续背压不能饿死必要维护 | Q0 等待下一尝试期间仍调用 `send_due_essential_maintenance()` | `test_neighbor_heartbeat.c` 在 5000 ms 持续 Q0 背压下仍发送不少于 5 次 Heartbeat，Neighbor 不误离网 | 已闭环 |
| 本机提交最终状态需要可观察，但不能伪造远端 ACK | Bridge 增加 final-only Outbound Observer；中间重试不回调；`UCN_OK` 仅为本机提交接受 | 恢复、耗尽、终止、过期分别只产生一次最终观察 | S20 已补结构化最终事件与业务结果关联；远端 ACK 仍明确不做 |

### 16.2 资源、构建与未验证边界

- Debug、Release、`UCN_MAX_FRAME_BYTES=64`、`UCN_MAX_BEARERS_PER_NEIGHBOR=1` 四个独立构建目录的 CTest 均为 `1/1` 通过。
- ESP32-S3 Node A/B 与 ESP-WROOM-32 只执行 PlatformIO 构建，三目标均成功；当前完整测试固件分别为 S3 RAM `47,692 / 327,680 B`、Flash `596,463 / 6,553,600 B`，WROOM RAM `49,832 / 327,680 B`、Flash `622,943 / 1,310,720 B`。
- Bridge 固定结构始终包含一个 Pending 消息槽，但默认 Policy 关闭，不产生自动重试；这是可计算的静态 RAM，不是动态积压。
- 调用树 11 个实际 YAML、106 节点、151 调用目标闭环；项目 Markdown 与 UCN 知识库内链缺失均为 0；`git diff --check` 通过。
- 本轮没有烧录、复位、监听或访问开发板 COM 口。真实 Wi-Fi/UART Queue 满/恢复、`send()` WCET、重试间隔精度、端到端 P50/P95/Max 和任务栈水位仍属于 S06/S07，不能由软件模拟替代。

### 16.3 当前结论

S15 的代码和软件门禁已完成，且没有修改线格式、路由协议或 Q1 语义。Q0 现在可以按产品显式选择“本机 Adapter 队列瞬时背压的有限重试”，但仍不是可靠传输；Link 接受后的丢包、远端拒绝和远端执行结果必须继续由产品结果 Endpoint 或未来独立可靠语义处理。下一项为 S16：Protocol Task 最大 Step 间隔和维护时延契约。

## 17. S17 完成后的反向核对（2026-08-10）

| 原问题/约束 | 实现位置 | 软件证据 | 核对结论 |
| --- | --- | --- | --- |
| Core 不应承担介质发现调度 | `ucn_adapter.h/.c` 提供 Adapter-owned Scheduler；Node/帧/路由线格式未变 | 既有 Core 全量回归 + `test_adapter_hello.c`；当前 C99 构建对象为 76 B/Adapter | 已闭环 |
| 多介质不能共用同一固定发送相位 | 每 Scheduler 保存非零 `adapter_token`，Port Seed 经 Token 加盐后生成独立固定序列 | 相同 Seed/Token 可重放，不同 Token 状态不同 | 已闭环 |
| 首次同步广播与固定周期重试会形成峰值 | `INITIAL_JITTER → FAST_RETRY → BACKOFF`，重试次数和指数上限均固定 | 100 节点 Seed `0x5EED1234`：10 ms 峰值 `100→9`，100 ms 峰值 `175→52` | 软件模拟闭环 |
| 准入后不应继续高频 HELLO | 支持 `ADMITTED_SLOW/STOP`；Slow 先发一次快速互认，再低频；Heartbeat 负责存活 | Stop、Slow、掉线重启状态测试 | 已闭环 |
| 静态点对点应能关闭主动发现 | `enabled=false` 允许其余时间字段全零，Step 永不返回到期 | 静态禁用与 restart 测试 | 已闭环；产品仍须静态注册/准入 Link |
| 失败必须可重放且不能伪造实机结论 | 模拟固定 Seed，丢弃前 0..3 次 HELLO，统计总量与准入分位 | 总量 250，P50/P95 `608/1177 ms`；相同 Seed 指标逐字节相同 | 仅调度模型证据，非无线性能 |

ESP 参考工程为 ESP-NOW 与独立 UART 各持有一个 Scheduler；ESP-NOW 准入后 5 s 低频发现，UART 30 s，初始 100～400 ms、250 ms 快重试最多 3 次、1～8 s Backoff、150‰ 重试抖动。启动日志报告对象字节数/参数，`STAT` 报各 Adapter 状态、到期数、重启和 Slow/Stop 转换；`UCN_TEST_*_HELLO_ENABLED=0` 只用于产品已静态配置 Link 的 Profile。

S17 该轮四个 CMake Profile 和 S3 Node A/B、legacy dual、WiFi-only、Service A、WROOM 六个参考镜像只完成软件构建。没有烧录、复位或读取 COM；真实 10/100 ms 空口峰值、三板/更多节点入网 P50/P95、碰撞/丢包、RAM/栈/功耗与参数标定继续属于 S06。S17 完成后进入的 S19 已在下一节闭环。

## 18. S19 完成后的反向核对（2026-08-10）

| 原问题/约束 | 实现位置 | 软件证据 | 核对结论 |
| --- | --- | --- | --- |
| 授权成功的管理 Node 可用唯一 Sequence 持续改写 Path 表 | `take_path_control_source_token()` 位于产品 Authorizer 与 `ucn_path_install/revoke()` 之间 | 合法源突发用尽后返回 `UCN_ERR_NO_SPACE`，1 s 后只恢复 1 Token | 已闭环 |
| 换 Wi-Fi/UART/CAN Bearer 不能刷新额度 | 固定槽键保存 `(source, session)`，不保存 ingress Link | 同一 Source/Session 交替两个注册 Link 只建立一个槽并共同耗尽 INSTALL Token | 已闭环 |
| INSTALL 洪泛不能阻止紧急 REVOKE | 每个管理源有两个独立 Token Bucket | INSTALL 为 0 时 REVOKE 仍成功；随后单独耗尽并记录 REVOKE 拒绝 | 已闭环 |
| 不同管理源必须隔离且内存不能增长 | 默认 4 个固定槽，不使用 malloc；60 s 不活跃惰性回收 | 第二来源有独立额度；第 5 个活动来源失败关闭，超时后新源可复用槽 | 已闭环 |
| Session 轮换不能永久泄漏槽或让旧 Session 自动回来 | 只有 Security+Authorizer 接受的新 Session 才重置同源槽；旧 Session 仍由 Security 拒绝 | 新 Session 复用原来源槽并计轮换；旧 Session 在 Security 处拒绝且预算/Path 不变 | 已闭环；生产吊销仍属 S02 |
| 拒绝原因必须可区分且绝不能改 Path | `ucn_node_stats_t` 增加按操作授权/速率拒绝、源表满、Path 表满、轮换/回收计数 | Security 拒绝、产品拒绝、Token 耗尽、源表满、Path 表满均比较完整 `ucn_path_state_t` 不变 | 已闭环 |
| 不能影响正常 Heartbeat/RREQ/业务预算 | Path 管理使用独立状态与计数，只在目标 Node 的 INSTALL/REVOKE 分支执行 | 完整四 Profile CTest（含既有控制预算、四节点 Path、中继与业务回归）均通过 | 软件闭环；真实干扰待 S06 |

默认管理预算状态为 96 B，新增统计为 32 B，总计为每个 Full `ucn_node_t` 增加 128 B 固定状态；没有改变 v4 帧格式、正常数据帧大小、AODV-Lite、Heartbeat/RREQ Token 或 Adapter Cost。ESP `PATHCTL` 行已经能输出实机验收需要的分类计数，但本轮没有烧录或读取串口，因此硬件结论仍保持待验证。S20 已在下一节闭环。

## 19. S20 完成后的反向核对（2026-08-10）

| 原问题/约束 | 实现位置 | 软件证据 | 核对结论 |
| --- | --- | --- | --- |
| `send_ex()`、Bridge 接受和远端执行容易混为一个 `UCN_OK` | `ucn_service_async_stage_t` 冻结五阶段；`ucn_service_acceptance_stage()` 只映射 `LOCAL_INBOXED/REMOTE_ROUTER_QUEUED` | `test_service.c` 覆盖 Acceptance→Stage 与非法/None | 已闭环 |
| 旧 Observer 不能直接区分立即背压拒绝与重试耗尽 | 保留兼容 Observer，新增 `set_outbound_event_observer()` 和五类本机 Outcome | `test_service_bridge.c` 覆盖 Link 接受、立即拒绝、耗尽、过期、终止，旧/新 Observer 次数一致 | 已闭环 |
| S15 中间 `NO_SPACE` 不能冒充最终完成 | 只有 `complete_outbound()` 同时调用两个 Observer，中间重试直接保留 Pending 返回 | 两次瞬时背压期间 Observer 为 0，恢复后各只回调 1 次 | 已闭环 |
| 远端结果必须按命令和真实 Endpoint 关联 | 可选 8 B `ucn_service_result_header_t`；`result_matches_command()` 核对 Command ID、Result Endpoint 与合法阶段/状态 | 大端编码/解码、附加正文、错 Endpoint、错 ID、非法阶段/状态组合 | 已闭环 |
| 目标 Inbox/执行不能由本机 Link Queue 接受推断 | Result Header 只允许 `REMOTE_INBOXED+ACCEPTED` 或 `REMOTE_EXECUTED+终态`，由业务反向发送 | 两节点 Command Guard→Inbox→Result Endpoint 的 Inboxed/Executed 两阶段往返 | 软件闭环 |
| 目标 Validator 拒绝不能自动产生伪 ACK | Bridge/Validator 路径未增加自动结果；产品自己持有等待 Deadline | 重复命令被目标拒绝；源 Observer 仍为 Link 接受，但 Result Inbox 为空并到产品 Deadline | 边界闭环 |
| 不给普通消息增加 RAM/线开销 | Stage 是 API 术语；8 B 头仅存在于产品选择发送的 Result Payload；无通用 Pending/ACK 表 | 64 B Profile 回归，既有 Router/Frame 测试全通过 | 已闭环 |

Debug、Release、`UCN_MAX_FRAME_BYTES=64`、`UCN_MAX_BEARERS_PER_NEIGHBOR=1` 四个 CTest Profile 均为 `1/1`。ESP32-S3 Service A/B 只构建成功，当前完整镜像均为 RAM `49,284 / 327,680 B`、Flash `599,095 / 6,553,600 B`；ESP-WROOM-32 为 RAM `50,184 / 327,680 B`、Flash `626,623 / 1,310,720 B`。这些是完整测试固件绝对值，不是 S20 独立增量。

本轮没有上传、复位、监听或访问 COM。真实 Wi-Fi/UART 结果往返时延、Validator 拒绝后的业务超时、断链本地安全和 Source/Session 产品等待表仍归 S07；S20 未增加通用 ACK、自动重传、可靠 Q0/Q2 或新的 v4 帧字段。S20 之后进入的 S05 已在下一节闭环。

## 20. S05 完成后的反向核对（2026-08-10）

| 原问题/约束 | 实现位置 | 软件证据 | 核对结论 |
| --- | --- | --- | --- |
| 旧压力测试只有静态 32 Node 环网 | 保留 `test_stress.c`；新增 `test_dynamic_stress.c` 的 32 Node×4 Link 固定事件网 | 物理 Link 先以未知 Peer 经双向 HELLO/Open Join 准入，再由首包 Q1 触发 AODV | 已闭环 |
| 无丢失/重复/延迟/乱序和 Cost/Down | 1024 槽测试事件队列，固定 PRNG Seed；Link 注入丢失、重复、0～7 ms 延迟、乱序、双向 Down/恢复和 Cost 波动 | 默认 Seed 2000 轮与 5 Seed×1000 轮通过；业务 Payload ID 未重复交付 | 已闭环（虚拟模型） |
| 无动态恢复和无环判定 | 强制切断首条 AODV 出口，固定 5 次×1.5 s Q1 Latest 产品重试；转发链按 Current/Previous `route_epoch` 检查 | 恢复后重新送达；跨 Epoch 表项不误报为可执行环，同 Epoch 最多 32 跳且无重复节点 | 已闭环 |
| RREP 沿途复制同一总 Cost/Hop | 目标 RREP 从 Cost/Hop=0 开始，每个返回节点按 ingress Link 累加本节点到目标的 Cost/Hop | `test_link_metrics.c` 断言 A→D→C 为 4000/2 Hop，D→C 为 2000/1 Hop；全量路由回归通过 | 已修复，不增加帧字节 |
| Neighbor/Route/Path/Flow/Service 表满和回收未聚合 | S05 固定资源子测试 + 既有 Candidate/Bridge/时间专项测试 | 表满返回 `UCN_ERR_NO_SPACE`，Candidate/Path/Flow/Service 出队或到期后可复用；Route/Candidate 清理后可重新寻路 | 已闭环 |
| 缺少长跑与 sanitizer | `UCN_STRESS_SEED`、`UCN_STRESS_EVENTS`（1～1,000,000） | 固定 Seed×100,000：614,713 入队、14,766 丢失、45,578 重复注入、事件高水位 147/1024；四 Profile 与 WSL GCC ASan/UBSan 均 `1/1` | 软件门禁闭环；百万轮未执行 |

S05 不修改 MCU 对象、队列容量或线格式；1024 槽事件队列和观测表只存在于 Host 测试程序。虚拟结果不能替代 ESP-NOW/UART/CAN 的真实碰撞、时延、栈/Heap、功耗或三板多跳，后者继续属于 S06/S07。复现命令与完整边界见 [S05 动态综合压力测试](UCN_S05_动态综合压力测试.md)。
