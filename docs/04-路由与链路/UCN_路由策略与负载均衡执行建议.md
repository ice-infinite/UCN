# UCN 路由策略与负载均衡执行建议

> 状态：**T22.1 数据模型、T22.2 Path ID/安全控制面、T22.3 固定严格/主备、T22.4 Q1 流亲和均衡、T22.5 Path/Bearer 联动与 T22.6 限频策略诊断已实现；真实多板验收尚未实现**。
> 适用版本：UCN Core v4；以当前 `include/ucn/ucn_node.h`、`src/ucn_node.c` 和 ESP32 双介质测试工程为准。
> 关联：[T21 同对端多介质主备 Link](../00-项目管理/00-任务表.md#任务清单) → [T22 策略路由与可选负载均衡](../00-项目管理/00-任务表.md#任务清单)。
> 本文是 [策略路由与可选负载均衡建议](UCN_策略路由与可选负载均衡建议.md) 的执行版；若两文档的任务拆分有差异，以本文和任务表中的 `T22.0～T22.7` 为准。

## 1. 目标与当前起点

本轮要实现的不是“每发一帧就重新全网选一次路径”，而是在固定容量内让 UCN 达到以下能力：

1. 自动业务使用已验证的较优路径；链路质量波动不导致频繁换路。
2. 指定业务可固定走某条完整路径；只有明确配置允许时才在硬故障后切备用路径或重新寻路。
3. 可选的负载均衡只在多条已验证路径之间分配**不同业务流**，不把同一实时流逐帧交替发送。
4. 应用继续只调用 `ucn_node_send_endpoint()`；不直接选择 UART、ESP-NOW、CAN 或下一跳。

当前 v4 Core 已有 AODV-Lite、通用 `route_cost`、单目标 Active/Candidate/Previous、Candidate Probe/Activate、20% Cost 滞回和 `route_epoch` 切换期保护。这些是“后台候选换路”能力，不是多活动 Path、指定完整路径或负载均衡。

当前 ESP32 测试工程的 `DualMediaLink` 也只是同一对端的 UART 优先、ESP-NOW 回退。它还不是通用多 Bearer 集，也不是流级均衡器；T21 必须先完成其 MCU Core 版本。

## 2. 必须保持的边界

- **MCU-first**：Linux/Host 不参与路由决策，也不保存中央全网路由表。
- **固定容量**：Policy、Path、Flow、统计、候选与控制消息状态均为编译期上限；具体上限按 RAM Profile 冻结，禁止隐式 `malloc`。
- **按需控制面**：业务命中缓存后只做 O(1) 或有界表查找；RREQ、Probe、Activate、Path 安装和诊断均受现有 Token/频率限制。
- **不破坏控制业务**：Q0 初版不进入自动均衡；它只走预建主/备 Path，失败交给本地安全动作。
- **不逐帧条带化**：同一业务流不在 P1/P2 间轮流发帧，避免路径时延差造成乱序和抖动。
- **不把 `route_epoch` 当作 `Path ID`**：前者只标记 Active/Previous 切换期；策略 Path 需独立、受认证的身份。
- **不把 source-local 下一跳偏好称为端到端固定路径**：若中继也需遵守路线，必须经过 Path ID 控制面安装相应转发表。

## 3. 分层模型

```text
应用：目标 Node + Endpoint + QoS + Payload
             │
             ▼
策略匹配：Policy（AUTO / PINNED / BALANCE）
             │
             ▼
端到端 Path Set：Primary / Backup / 可均衡成员
             │
             ▼
下一跳 Bearer 选择：同一邻居的 UART / ESP-NOW / CAN（T21）
             │
             ▼
Adapter：驱动、队列、RSSI/错误/拥塞采样、完整 UCN 帧收发
```

`Path` 与 `Bearer` 不可混淆：

| 层 | 解决的问题 | 示例 |
| --- | --- | --- |
| Bearer | A 发给同一下一跳 B 时使用 UART、ESP-NOW 还是 CAN。 | `A -- UART/ESP-NOW -- B` |
| Path | A 发给远端 D 时经过 B 还是 C。 | `A→B→D`、`A→C→D` |
| Flow | D 的哪一种业务使用 Path P1 或 P2。 | IMU 固定 P1，日志分给 P2 |

## 4. 统一质量与 Cost 规则

Core 继续只消费通用 `route_cost` 和 Link 状态；Adapter 不把 RSSI、Bus-Off、串口号等介质细节泄漏到 Core。

每个 Adapter 应维护固定窗口/EWMA 后的本地质量快照，再由 `get_metrics()` 返回综合 Cost。综合 Cost 至少考虑：

| 指标 | ESP-NOW/WiFi 来源 | UART/CAN/RS485 来源 | 用途 |
| --- | --- | --- | --- |
| 基础代价 | 产品配置 | 产品配置 | 体现介质的固定偏好/能耗。 |
| 可达性 | 驱动状态、近期有效帧 | Driver Down、近期有效帧 | 硬 Down 立即排除。 |
| 丢失/失败 | 发送回调、连续失败、接收拒绝 | CRC/COBS 错误、发送错误 | 持续失败增加惩罚。 |
| 拥塞 | 驱动提交背压、RX/TX 队列占用 | `no_space`、队列占用 | 防止所有流量挤向看似最优的链路。 |
| 时延 | Probe/ACK RTT 的 EWMA | Probe/ACK RTT 的 EWMA | 为实时业务提供稳定参考。 |

规则：瞬时样本只更新 Adapter 本地 EWMA；Core 读取的是缓存结果，不能在 `get_metrics()` 中触发探测、阻塞或分配内存。质量下降先形成软惩罚，只有 Driver Down、明确发送失败、RERR 或达到失联判据才是硬故障。

### 4.1 T22.1 当前实现

`include/ucn/ucn_link.h` 的 `ucn_link_metrics_t` 已在原有 `route_cost` 后增加可选 `rtt_ms`、`tx_failure_per_mille` 与 `queue_pressure_per_mille`。调用方先清零结构体，因此旧 Adapter 只填写原有两个字段仍安全；未填写的新增字段不会被误读成真实测量。

`ucn_policy_refresh_link_quality()` 在 `ucn_node_step()` 内以默认 500 ms 周期读取已注册 Link 的状态和指标，并用默认 25% EWMA 保存到固定 Link 质量快照。该快照只供后续策略/诊断读取，不在 T22.1 改写现有 Active/Candidate 或 Bearer 的自动选路逻辑。

ESP32 测试 Adapter 已提供当前可得输入：ESP-NOW 的 RSSI EWMA、提交/回调失败率和固定 RX Queue 压力；UART 的发送失败率和固定 RX Queue 压力。ESP-NOW 与 UART 都没有可靠 RTT 测量，因此明确返回 `rtt_valid=false`，不得把 Ping 或单次回调时间伪装成 RTT。真实阈值和长期 EWMA 参数仍需后续板级标定。

## 5. 路由策略和流亲和

策略最小匹配键为：

```text
(destination_node_id, endpoint[, traffic_class])
```

同一 Endpoint 内确实有多个长期独立流时，再由产品 Payload ABI 引入可选 `stream_id`；Core 不为未知需求给普通帧增加字段。

| 模式 | 正常行为 | 故障行为 |
| --- | --- | --- |
| `AUTO_BEST` | 使用当前已验证的自动路径。 | 复用既有 RERR/按需寻路闭环。 |
| `PINNED_STRICT` | 只走指定 Path。 | 本地返回失败，不自动改路。 |
| `PINNED_FAILOVER` | 固定 Primary。 | 仅硬故障后切已验证 Backup；是否允许再寻路由策略决定。 |
| `AUTO_BALANCE` | 仅 Q1、仅已验证 Path；按流绑定后分配。 | 受影响流重新绑定到健康成员。 |

自动均衡采用“流亲和 + 租约 + 有界重绑定”：同一 `(source, destination, endpoint[, stream_id])` 在租约内固定 Path；仅在流创建、租约到期、Path Down 或持续拥塞时重新选择。V5-44/V5-36 后，Full 以 LC-1 `effective_select_cost ×（活动 Flow 数 + 1）` 选择成员，所有动态指标先经固定表归一；持续队列压力仍可独立触发重绑。它不复制同一帧，也不做带宽聚合。

### 5.1 T22.4 当前实现

当前 Core 的最小 Flow 键为 `(destination, endpoint, Q1)`；同一 Endpoint 真正需要多条独立流时，仍须由产品 Payload ABI 冻结 `stream_id` 后再扩展，而不是给所有帧增加字段。`AUTO_BALANCE` 仅接受精确 Q1 Policy，Primary 必填、Backup 可选，且禁止 `allow_discovery_on_hard_failure`。Primary/Backup 组成最多两条候选 Path，二者都必须是本节点已验证、未到期且 egress 一致的线上 Path。

首次发送或租约到期时，Core 用固定 Flow 表绑定一条成员 Path；`balance_flow_lease_ms=0` 使用默认 2 s。Full 分数为出口 Link 的 `effective_select_cost ×（该 Path 当前活跃 Flow 数 + 1）`；Known 基础 Cost 永远优于 Unknown。相同分数的新 Flow 按较小 `local_path_id` 确定性选择，活动 Flow 乘数会让后续 Flow 分散。RTT/失败率只通过 LC-1 固定归一表影响本地分数，业务帧本身不添加字段。

同一 Flow 在租约内继续使用原 Path。只有 Path 被硬 Down/不存在，或默认连续 3 个 500 ms 快照的队列压力达到 800‰ 时，才重绑到另一健康成员；当前帧只会在新的成员上发送一次，不复制、不条带化。没有健康成员时返回本地 `LINK_DOWN`/配置错误，不隐式 RREQ；Q0、自动 Discovery 和带宽聚合都被排除。三个宏和租约可按 MCU Profile 覆写，实际门限尚未完成硬件标定。

## 6. Path ID、控制面与故障范围

完整固定路径需要中继也能识别路径，因此应增加短 `Path ID`，而不是在每一帧携带完整 Node 列表。

```text
业务帧：destination=D, endpoint=IMU, path_id=P1
中继 B： (P1, D) → 下一跳 D
中继 C： (P2, D) → 下一跳 D
```

Path 安装、更新与撤销必须由受认证的控制面执行；若业务启用端到端保护，Path ID 要纳入相应 AAD 完整性契约。`ROUTE_ERROR` 需要包含 Path 范围或等效标识，P1 故障只能使 P1 失效，不能误清同一目标的 P2。

每条固定 Path 至少保存：`path_id`、目标、下一跳/egress、状态、Cost、Probe/ACK 统计、过期时间、策略引用。每条 Flow Binding 至少保存：匹配键、当前 Path、租约截止、最后活动时间和重绑定原因。所有表均为固定数组。

### 6.1 T22.2 已实现的线格式与控制面边界

业务帧若带独立 Path，使用 40 B 头：它保留 36 B Route Extension 的 `route_epoch`，并在偏移 34～37 写入非零 `path_id`；偏移 38～39 为 CRC。`route_epoch` 仍只服务 Active/Previous grace，不能当作 Path 身份。旧实现会因未知 Path Flag/头尺寸而拒绝该帧，不会静默按普通路由转发。

每个节点的 `ucn_path_state_t` 是固定数组，默认 `UCN_MAX_PATH_FORWARD_ENTRIES=8`。一项的真实身份是 `(owner_node_id, owner_session_id, path_id, destination)`，而不是仅有一个可能碰撞的整数；它只保存**本跳**的下一跳、egress Link、是否终端和租约，不保存无限长度源路由。源、每个中继和终端分别安装自己的表项，因此中继可按 Path ID 转发端到端密文，却不需要解密业务 Payload。

远程安装/更新帧 `PATH_INSTALL` 的固定 Payload 为 `{Path ID, Destination, Next Hop, Lease}`，撤销帧 `PATH_REVOKE` 为 `{Path ID, Destination}`。两类控制帧既要经过已有 Security Provider 的接收授权，也必须通过 `ucn_node_set_path_control_authorizer()` 设置的显式控制面回调；未设置回调时默认拒绝。控制帧本身不得标记 E2E。业务使用 E2E 时，Path ID 写入 30 B 不可变 AAD，合法中继只可改 Hop Limit 和 Route Epoch。

Path 范围 `ROUTE_ERROR` 使用 `{unreachable, destination, owner_session, path_id}`，只撤销该 Path；保留旧 4 B RERR 的普通 Route 语义。`ucn_node_send_path()` 与安装/撤销 API 仍可用于受控配置、诊断和验证；T22.3 还将已验证 Path 接入普通 `ucn_node_send_endpoint()`。

### 6.2 T22.3 已实现的 Primary/Backup 策略执行

一个 Policy 现在引用一个 Primary 和一个可选 Backup 的**本地句柄**；每个句柄在固定 Policy Path 表中再绑定到线上 `wire_path_id`。这样产品可以替换或重新安装线上 Path，而不需要逐条改写所有 Endpoint Policy。要成为可发送的 PINNED Path，该表项必须是 `VERIFIED`，且其 Path ID、目标和 egress 与本节点已安装、未到期的 T22.2 源端表项一致；零 Path ID 只保留 T22.1 元数据用途，不能发送。

`PINNED_STRICT` 始终只发 Primary。Primary 不存在、过期、RERR、Driver Down 或发送返回硬失败时，API 将失败直接返回给本机，不转成普通 Route、RREQ 或 Backup。设置 Strict 时若开启“硬失败后发现”会被拒绝，避免配置名称与实际行为矛盾。

`PINNED_FAILOVER` 先发 Primary；只有 `UCN_ERR_LINK_DOWN` 或 `UCN_ERR_NOT_FOUND` 才视为硬故障、将相应本地 Policy Path 记为 Down，再尝试已验证 Backup。背压、Security/ACL、参数或任意非硬失败都不会切换，因此 Cost/队列短时变化也不会移动当前固定业务。Primary/Backup 都硬失败时，只有 `allow_discovery_on_hard_failure=true` 的 Q1 才会复用受限 RREQ 和固定等待槽；这是一项显式的“放弃固定路线”策略，Q0 永不进入该分支。

### 6.3 T22.4 已实现的 Q1 流亲和均衡

`AUTO_BALANCE` 重用 Policy 的 Primary/Backup 本地句柄作为候选集，但它们的含义不是“固定主备”：两个成员都可以承载不同 Flow。第一次命中 `(destination, Endpoint, Q1)` 时创建固定 Flow Binding；同一 Flow 在租约内固定，第二个独立 Endpoint Flow 才可能落到另一成员。Core 只读取已缓存的通用质量快照和固定 Flow 表，不对 Adapter 触发额外探测，也不为普通帧加入 `stream_id`、权重或诊断字段。

`send_endpoint_on_policy_path()` 的硬失败会撤销故障 Path 并使当前 Flow 只尝试另一成员一次。队列压力必须连续达到 Profile 阈值才会触发重绑；单次 Cost、背压、Security/ACL 或参数错误均不会移动 Flow。Flow 表满或没有健康成员返回本地错误，绝不回落为逐帧 `AUTO_BEST` 或自动发现。

### 6.4 T22.5 已实现的 Path/Bearer 联动

每个逐跳 Path 表项仍保存它被安装时的 egress Link 指针，但发送和中继转发都会把该指针解析为**同一 Neighbor 当前健康的 Bearer**。因此 UART 主 Bearer Down、ESP-NOW Backup 仍健康时，业务帧仍使用原有 `Path ID`、原有逐跳表和原有端到端 AAD；不会把一次一跳切换误判为端到端 Path 故障，也不修改其它 Path。

只有该 egress Neighbor 的全部 Bearer 都不可用时，Core 才撤销使用该下一跳的非终端 Path 表项；若该 Path 属于本节点，所有匹配 `wire_path_id + destination` 的本地 Policy Path 同时标为 `DOWN`。中继在下一次收到该 Path 业务帧时会经健康上游 Bearer 发送 Path 范围 RERR；源收到后只撤销该 P1，独立 P2 保持可用。没有反向控制路径的本地源端只返回硬失败，不凭空广播 RERR。

这层不改 40 B Path 头、不增加业务帧字段、不复制业务帧，也不把下游转发失败伪装成当前一跳 Bearer 状态。实际 Driver 状态、全 Bearer 失效由 Adapter `get_status()`/保活维护确认；真实切换时延、丢失、乱序和功耗仍需 T22.7 实机测量。

### 6.5 T22.6 已实现的按需策略诊断

管理 Node 使用 `ucn_node_request_policy_diagnostic()` 向一个已经有 Route/直连 Link 的目标发起单播查询。请求 `POLICY_DIAGNOSTIC_REQ (0x1E)` 固定为 8 B `{request_id, section, index, reserved}`；回复 `POLICY_DIAGNOSTIC_REPLY (0x1F)` 固定为 32 B `{request_id, section, index, status, reserved, 24 B record}`。两者都是 `DIAGNOSTIC=0x04` 的 Q1 控制帧，不能携带 Path ID 或 E2E；32 B 回复加基础 32 B 头正好适配 64 B Profile。

查询不传输业务 Payload，也不泛洪或保存完整全网图。`section` 只允许三页六计数器 Summary，或固定表的一个 Policy、Policy Path、Q1 Flow、Link-quality 槽位：Policy 显示精确规则与该规则命中次数；Path 显示线上 Path ID、逻辑 egress、当前解析 Bearer 和已缓存 Cost/RTT/失败率/队列压力；Flow 显示 `(destination, Endpoint, Q1)→local_path_id`、剩余租约和当前 Bearer；Summary 分页给出 Strict/Failover/AUTO_BALANCE 的发送、硬失败、拥塞/Down 重绑等原因计数。

远端默认拒绝，必须用 `ucn_node_set_policy_diagnostic_authorizer()` 显式允许请求 Node；原有 Security Provider 的 RX 授权仍先执行。请求方和响应方各自使用“突发 1、1 s 补 1”的独立 Token，源端 Pending 和目标 Reply Queue 默认各 2 项，均超时回收。诊断不进入业务 Q0/Q1 队列；`ucn_node_step()` 仅在普通 Q0/Q1、保活、Probe 和既有 Snapshot Reply 之后才发送它，因此不会挤占 Q0。

## 7. 稳定切换流程

```text
Active/Primary 持续承载业务
  → 后台刷新或低频候选探测
  → Candidate 通过 Probe/ACK
  → 写入 Backup 或待激活成员
  → 满足策略条件后切换/重绑定
  → 保留 Previous grace，使在途帧正常完成
```

- `AUTO_BEST`：候选达到改进门限并通过探测后才激活；切换后需要冷却时间，防止反复横跳。
- `PINNED_FAILOVER`：Cost 变好/变差不改变 Primary；只有硬故障才使用 Backup。
- `AUTO_BALANCE`：仅影响新流或允许重绑定的流；现有流在租约内保持原 Path。
- 任一 Path Down：立即从可均衡集合移除，相关 Flow 重新绑定；无法替代时按 QoS 返回错误或走本地安全逻辑。

## 8. 执行任务与依赖

| ID | 前置 | 实现内容 | 单元/模拟验收 | 状态 |
| --- | --- | --- | --- | --- |
| T22.0 | 当前 v4 | 冻结 Profile 容量预算、硬/软故障定义、Cost 输入、Q0/Q1 规则、兼容性与 API 边界；不改线格式。 | 配置校验、文档与旧 API 回归。 | 已完成（本文；未改代码） |
| T22.1 | T22.0 | 已新增固定 Policy/Path/Flow/Link 质量快照、可选质量字段和 Node API；未配置业务保持 `AUTO_BEST`。本地 `local_path_id` 仅是表句柄，未上帧。 | `test_policy.c` 覆盖表满、重复、非法组合、精确/通配匹配、EWMA、Flow 租约与旧 API；Debug/Release/64 B/Bearer=1 全绿。 | 已完成（C99/虚拟；ESP32-S3/WROOM 已构建，未做本轮实机）。 |
| T22.2 | T21、T22.1 | 已实现 40 B Path Header、固定逐跳转发表、`PATH_INSTALL/PATH_REVOKE`、Provider+显式授权回调、Path AAD 与 Path 范围 RERR；T22.3 已将已验证线上 Path 接入普通 Endpoint。 | `test_path_control.c` 覆盖双路径、E2E 透明中继、授权、更新/过期/撤销和 P1/P2 隔离；Debug/Release/64 B/Bearer=1 CTest 全绿。 | 已完成（C99/虚拟；S3/WROOM 仅构建，真实多板留 T22.7）。 |
| T22.3 | T22.2 | 已实现 `PINNED_STRICT` / `PINNED_FAILOVER`：Policy Primary/Backup 本地句柄绑定已验证线上 Path；仅 `LINK_DOWN`/Path 不存在切换。无 Backup 的 Q1 可按配置受限 RREQ，Q0 绝不寻路。 | `test_path_control.c` 覆盖 Strict P1/不回退、Failover P1→P2、P1 RERR、Q0 Backup/Q0 无发现、Q1 无 Backup 经 RREQ 回退与 AUTO_BEST 回归；四 CTest Profile 全绿。 | 已完成（C99/虚拟；S3/WROOM 仅构建，实机切换留 T22.7）。 |
| T22.4 | T22.3 | 已实现默认关闭的 Q1 `AUTO_BALANCE`：固定 Flow 表、默认 2 s 租约、Primary/Backup 两成员评分选择、持续队列压力/Down 重绑；禁止 Discovery、逐帧轮询、复制和 Q0 均衡。 | `test_policy.c` 覆盖非法 Q0/Primary/Discovery/租约；`test_path_control.c` 覆盖同流固定、两 Flow 分散、3 次压力样本重绑、Down 重绑和租约到期；四 CTest Profile 全绿。 | 已完成（C99/虚拟；S3/WROOM 仅构建，真实比例/时延/乱序留 T22.7）。 |
| T22.5 | T21、T22.4 | 已让 Path 的 egress 在发送/中继转发时解析同 Neighbor 当前健康 Bearer；全 Bearer Down 才撤销受影响 Path，并同步本机 Policy Path Down。 | `test_path_control.c` 覆盖 A⇄B/B⇄D 双 Bearer 的单 Down 保持 Path ID、全 Down、Path 范围 RERR、透明 E2E 和独立 P2 保留；四 CTest Profile 全绿。 | 已完成（C99/虚拟；S3/WROOM 仅构建，真实多板留 T22.7）。 |
| T22.6 | T22.1～T22.5 | 已实现受授权的单 Node `POLICY_DIAGNOSTIC_REQ/REPLY`：8 B 请求/32 B 回复，三页 Summary 与固定 Policy/Path/Flow/quality 槽位，普通业务零额外字段。 | `test_policy_diagnostic.c` 覆盖权限、限频、Pending 满、空槽、Q0 优先、Path/Bearer、Flow、质量、Summary 与超时；四 CTest Profile 全绿。 | 已完成（C99/虚拟；S3/WROOM 仅构建，远程 ACL/时延/控制开销实机留 T22.7）。 |
| T22.7 | T22.6 | 验收父任务：T22.7.1 已冻结可重复的 C99/目标构建回归，T22.7.2 再以分阶段实板给出性能结论。 | 不以自动构建替代实机；所有统计按实际日志记录。 | 进行中（等待 T22.7.2）。 |
| T22.7.1 | T22.6 | `tools/run_t22_7_core_regression.ps1` 已重配并运行 Debug、Release、64 B、Bearer=1 CTest，构建 S3 Node A/B、WROOM；全程不上传、不监听、不复位。 | `test_neighbor_bearer`、`test_path_control`、`test_policy`、`test_policy_diagnostic` 与完整 CTest 均回归，CMake Flags 不串档；四次 CTest 均 1/1 通过。 | 已完成（仅自动 Core/构建证据）。 |
| T22.7.2 | T22.7.1 | 两板双 Bearer → 三板 A→B→C/RERR → 四/五板 P1/P2/策略/均衡实机验收。 | 每步留原始日志、Node ID、摆位、介质与统计；测 P50/P95、丢失、乱序、收敛、控制面、资源/功耗。 | 待开始（需要实板条件）。 |

T21 是 T22.2/T22.5 的硬前置：没有同一 Neighbor 的多 Bearer 身份和状态模型时，不能把 ESP-NOW 与 UART 的一次回退误称为通用多介质负载均衡。

## 9. 测试路线

先使用虚拟 Link 构造受控拓扑，再上真实板：

```text
        B
      /   \
    A       D
      \   /
        C
```

虚拟测试可精确注入延迟、丢失、队列满、RERR 与 Driver Down。实机使用两块 S3 验证同对端 UART+ESP-NOW Bearer 行为；再使用五块板构造 `A→B→D` 与 `A→C→D` 两条平行中继路径。必须通过屏蔽、距离、准入白名单或测试 Profile 阻止 A 与 D 直接相连，否则不能把“直连优先”误判为多跳策略成功。

每一阶段至少记录：Path/Bearer 状态、Cost、Probe 次数与开销、Flow 绑定、切换原因、P50/P95 时延、丢失、乱序、队列丢弃、RAM/Flash/栈和功耗。真实实机未测前，不承诺无缝、最优或自动均衡效果。

## 10. 推荐实施顺序

先完成 T21 的同对端主备，再按 `T22.0 → T22.1 → T22.2 → T22.3 → T22.4 → T22.5 → T22.6 → T22.7` 推进。不要跳过 Path ID/故障范围直接写“按 Endpoint 强制路径”，也不要在没有 Flow Binding 的情况下实现逐帧轮询均衡。
