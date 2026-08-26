# UCN v5 审计遗留问题实际修复方案

> 日期：2026-08-11
> 基线：`codex/v5-adaptive-wire@84880ce`
> 状态：V5-17～V5-20 已实现并已在远端基线中发布；V5-21 继续阻塞于 S02。后续 V5-22～V5-26 见[最新审计交叉问题修复建议](UCN_V5_最新审计交叉问题修复建议.md)。
> 当前一致性说明：V5-22～V5-26 已完成并发布到 `f941ae9`；V5-27～V5-30 已完成异构 Bearer、动态 MTU 与 Policy 修复，V5-31～V5-33 已完成 PATH_INSTALL/API 发布修复，均纳入当前 `codex/v5-adaptive-wire` 分支。本文第 4～10 节保留修复前设计过程，第 11 节是 V5-17～V5-20 当时结果，当前资源与回归以 [V5-27～V5-30 修复报告](../08-实现与验证/版本演进/UCN_V5_27_异构Bearer与动态MTU修复报告.md)和[V5-31～V5-33 修复报告](../08-实现与验证/版本演进/UCN_V5_31_PATH_INSTALL兼容与API符号修复报告.md)为准。

## 1. 目的

本方案把最新审计意见与当前源码逐项对照，只保留仍真实存在且符合 UCN MCU-first 边界的问题。修复目标不是继续堆叠功能，而是让路由满足以下原则：

> **协议能够表示一条路径，不等于产品应当使用这条路径。**

UCN 后续必须同时约束 Wire 表达范围、产品运行范围、业务实时性和安全授权，不能因为 W2/W3 能编码 64/254 Hop 就让普通 MCU 网络自动搜索到该范围。

## 2. 审计项最终分类

| 审计项 | 当前源码结论 | 最终处理 |
| --- | --- | --- |
| HELLO/RREQ/RREP 固定使用 Node TX 档 | 原问题已修复；HELLO/Heartbeat 可自动最小选档，RREQ 选择完整搜索域最小档，RREP 保持请求档 | 不重复修改；补做有界逐级扩散 |
| 没有 Expanding Ring | 属实；当前 RREQ 一次选择覆盖 `default_hop_limit` 的档位 | V5-19 实现 |
| 累计 Route Cost 只有 16 bit | V5-14 已修复内部 `uint32_t`；V5-23 又将 Wire Cost 修正为 3/3/3/4 B | 不再修改累计位宽；当前格式以 V5-23 为准 |
| Path 可安装当前固定 TX 域不能表达的字段 | 已修复；本地、远端发送和远端接收均在写表前校验 | 保持现状 |
| Pinned Path 使用 Node 默认 Hop | 属实；普通 Route 已用 `route->hop_count`，Path 发送仍用 `default_hop_limit` | V5-18 实现 |
| RREP 重复 Origin/Target、缺一致性检查 | 已修复；重复字段已删除，Header 是唯一语义来源 | 保持现状 |
| RERR/Path/Trace/Snapshot 固定 32 bit | 已修复为 Profile-aware；Candidate/Policy 独立 Schema 有意保持固定 | 保持现状 |
| 只有 Node 级 RX Ceiling | 已修复；Link 可独立继承或收窄本地 RX Ceiling | 保持现状 |
| RX Ceiling 在完整 Decode/CRC 后检查 | 属实；不会越界，但会浪费小 MCU CPU | V5-20 实现 |
| Protocol Version 缺 6 bit 断言 | 已修复并有 Version=64 负向构建 | 保持现状 |
| Authorized Class 未实现 | 属实；当前只有不依赖伪身份的设计冻结 | V5-21，严格依赖 S02 |
| 生产安全未闭环 | 属实；仍是发布 P0 | 继续执行 S02，不由 v5 软件模拟冒充完成 |
| W0/W1 要求真实小 Network ID | 属实但属于官方 Wire Domain 边界 | 不修改 Core；跨域继续归 `ucn_gateway_ext` |

## 3. 路由约束的四层模型

### 3.1 Wire 表达上限

W0/W1/W2/W3 的 4/16/64/254 Hop 只表示帧字段能够编码的范围，不是默认搜索范围，也不是产品授权范围。

### 3.2 产品运行上限

`UCN_MAX_HOPS` 继续作为编译期硬上限，当前默认 16。Node 的 `default_hop_limit` 必须小于等于该上限。普通 MCU 产品建议根据实机结果配置为 4、8 或 16，不允许 Auto 模式仅因为使用 W3 就扩大到 254 Hop。

### 3.3 业务可用性上限

新增路由准入约束，至少包含：

```c
typedef struct ucn_route_constraints {
    uint8_t max_hops;
    ucn_route_cost_t max_route_cost;
    uint16_t max_verified_rtt_ms;
    bool require_verified_rtt;
} ucn_route_constraints_t;
```

具体 API 名称在实现阶段冻结，但语义必须满足：

- `max_hops` 不能超过 Node 产品上限；
- `max_route_cost` 是可配置的路线可用门槛，不能把 65,534 当成天然延迟阈值；
- `max_verified_rtt_ms` 只使用探测得到的端到端 RTT，不用单跳 RSSI 或静态介质等级冒充；
- `require_verified_rtt=true` 时，未完成探测的路线只能是“可达”，不能承载要求该门禁的实时业务；
- 未设置业务覆盖时继承 Node 默认约束，保持现有应用的显式迁移路径。

初步产品建议只作为标定起点，不写死进协议：控制/舵机 1～3 Hop，高频实时数据 1～4 Hop，普通遥测 4～8 Hop，配置与诊断最多 8～16 Hop。最终数值归 S06/S07 实机日志。

### 3.4 安全授权上限

Authorized Class 以后还要限制允许的最大 Hop、控制预算和 Fanout，但它必须绑定 S02 已认证 Principal/Session Generation。Wire Profile、Node ID 或新 Bearer 不能自行提升授权。

## 4. V5-17：路由可用性与延迟门禁

### 4.1 第一阶段：Hop/Cost 硬准入

路线进入普通发送、候选切换或负载均衡集合前，必须按以下顺序判断：

```text
Wire 可表达
  → Hop 不超过 Node 上限
  → Hop 不超过业务上限
  → Route Cost 不超过业务上限
  → 需要 RTT 时检查已验证 RTT
  → 在合格路线中比较 Cost/负载
```

低 Cost 但超 Hop、低 Hop 但超 Cost 的路线都不能进入可用集合。200 Edge Host 模拟继续作为线格式/状态机极限证据，但默认 16 Hop 产品策略必须拒绝把它作为业务路线。

### 4.2 延迟策略

本阶段不在每个 RREQ/RREP 中直接增加独立延迟字段，避免先增加 Wire 开销再等待硬件证明用途。延迟先通过两条路径进入决策：

1. Adapter 的单跳 `route_cost` 只提供稳定、可加的介质/产品基础 Cost，不重复揉入本次 RTT、失败率和队列压力；
2. RTT、发送失败率与 Adapter 队列压力继续作为独立有效位指标，由 Policy 在需要时组合一次；
3. Candidate/Path Probe 增加发送时刻和 ACK RTT EWMA，作为端到端已验证 RTT。

如实机证明 Cost+Probe 仍无法满足业务 Deadline，再单独评审 Profile-aware 累计延迟字段，不能在本轮顺带扩展 RREQ/RREP。

### 4.3 失败语义

- 无路线满足约束时返回明确的无可用路线结果，不能退回超范围路线；
- Q0/Q1 的既有 Deadline 继续生效，排队超时不得发送；
- 自动负载均衡只能在同一业务约束下的合格路线之间分流；
- 指定 Path 不满足业务约束时默认拒绝，只有产品显式使用诊断/维护策略才能放宽。

## 5. V5-18：Pinned Path Remaining Hops

### 5.1 状态与线格式

在 `ucn_path_forward_config_t` 和 `ucn_path_forward_entry_t` 中增加：

```c
uint8_t remaining_hops;
```

PATH_INSTALL 新布局为：

```text
PathID(P) | Destination(A) | NextHop(A) | Lease(4) | RemainingHops(1)
```

| Profile | 当前长度 | 新长度 |
| --- | ---: | ---: |
| W0 | 7 B | 8 B |
| W1 | 10 B | 11 B |
| W2 | 13 B | 14 B |
| W3 | 16 B | 17 B |

### 5.2 运行规则

以 A→B→C 为例：A 保存 2，B 保存 1，C 保存 0。源节点使用 2 作为业务帧 Hop Limit；中继在转发前验证收到值与本地 Remaining Hops 一致并递减；目标只接受终端 Path 状态。

旧的固定长度 PATH_INSTALL 必须按坏长度拒绝，不做启发式兼容。当前 v5 尚未正式冻结，应该现在完成该线格式修正。

## 6. V5-19：有界 Expanding Ring

Profile 与搜索半径必须解耦。建议默认 Ring 序列为 2、4、8、16，并裁剪到 Node 产品上限：

```text
Ring 1: Hop 2
Ring 2: Hop 4
Ring 3: Hop 8
Ring 4: Hop 16
```

每一轮再选择能够表达 Network/Source/Destination/Session/当前 Hop 的最小 Wire Profile。因此大 Node ID 可以使用 W3+2 Hop，而不是 W3+254 Hop。

每个 Discovery 固定保存当前 Profile、当前 Hop、Ring 次数和 Deadline；每轮使用新 Request ID，受现有 Token/Fanout/固定表深度约束。找到路线立即停止；旧轮迟到 RREP 不得覆盖当前状态；达到 Node 上限后结束，不无限重试。固定 Profile 模式保持当前单次搜索语义。

## 7. V5-20：Ingress Profile 早期预过滤

新增只读取前三个字节的 Profile Peek：

```c
ucn_frame_peek_wire_profile(data, length, &profile)
```

Ingress 顺序调整为：

```text
解析 Node/Link 本地 RX Ceiling
  → Peek Magic/Version/Profile
  → Profile 超限立即拒绝
  → 完整 Decode/长度/CRC
  → Security/ACL/状态机
```

Peek 只是资源预过滤，不是认证。需要冻结错误优先级：超本地档返回 Unsupported；允许档内坏 CRC 返回 CRC 错误；Magic/Version 错返回格式错误。Full/Lite 与 Nano 必须使用同一行为。

## 8. V5-21：Authorized Class 执行层

V5-21 只有在 S02 提供生产身份、逐跳控制认证、密钥/Session Generation、吊销和持久 Replay 后才能进入代码阶段。实现时把 C0～C3 绑定到认证 Principal，并同时检查消息 ACL、按身份/控制类型 Token、Fanout 和表容量。任何帧自报 W3 都不能取得更高权限。

## 9. 执行顺序

```text
V5-17 路由可用性与延迟门禁
  → V5-18 Pinned Path Remaining Hops
  → V5-19 有界 Expanding Ring
  → V5-20 Ingress Profile Peek
  → 全量单测/模拟/资源回归
  → 冻结 v5 Wire Schema
  → S02 生产安全闭环
  → V5-21 Authorized Class 执行层
  → S06/S07 多板、多介质、攻击注入实机验证
```

V5-18 在 V5-19 前完成，是为了让指定 Path 和自动 Route 使用同一套真实 Hop 语义；Wire/路由结构稳定后再完成生产 AAD/身份绑定，避免密码实现后再次修改受保护格式。

## 10. 软件验收矩阵

| 任务 | 单元测试 | 模拟/集成测试 | 资源门禁 |
| --- | --- | --- | --- |
| V5-17 | Hop/Cost/RTT 未知与边界、默认继承、非法组合 | 低 Cost 超 Hop、低 Hop 超 Cost、实时业务拒绝未验证路线 | 三 Build Profile 固定内存；关闭能力不得增 RAM |
| V5-18 | 四档 PATH_INSTALL 新长度、Remaining Hops 边界、旧长度拒绝 | A→B→C 精确递减、错误 Scope 拒绝、W3 ID+2 Hop | Path 表增加量和最小 MTU重新报告 |
| V5-19 | Ring 状态、Request ID、超时、Token、迟到 RREP | W0 窄链局部成功、5 Hop 升级、16 Hop 截止、断链/重复/高负载 | Discovery 表增加量、控制帧总量和 Host 工作量 |
| V5-20 | Peek 短帧/Magic/Version/Profile、错误优先级 | W0 Link 注入合法/坏 CRC W3、混合 Link 独立上限 | 对比完整 Decode 与早拒绝工作量；目标 CPU 留实机 |
| V5-21 | Class/ACL/Token/Fanout、轮换/吊销/重启 | 未认证/W3伪报/旧 Session/表满全部失败关闭 | 依赖 S02 生产 Provider 与真实 MCU 报告 |

完成每项后继续执行 Windows Debug/Release Full/Lite、Nano、配置负向构建、WSL ASan+UBSan、单档/混档和线形端到端模拟。软件结果不能替代真实 Wi-Fi/CAN/UART 的时延、碰撞、CPU、栈和功耗测试。

## 11. 实际执行结果（2026-08-11）

- V5-17 已实现 Node 默认/Policy 路由约束、路由质量查询、直接/动态/指定 Path 的 Hop/Cost/RTT 门禁，以及 Candidate Probe ACK 的端到端 RTT EWMA；动态 Route 实质更新会清除旧 RTT，不在 RREQ/RREP 里增加延迟字段。
- V5-18 已实现 Path `remaining_hops`，PATH_INSTALL 四档载荷为 8/11/14/17 B，旧长度失败关闭；源、中继、终端按真实剩余跳数校验和递减。公开 Path 安装 API 因此发生 v5 阶段允许的签名更新，当时 Node Storage Layout 升级到版本 3；V5-24 因 Candidate Profile 连续性继续升到版本 4。
- V5-19 已实现 2→4→8→16 有界 Expanding Ring；单 Ring 默认 250 ms，总预算仍为 1000 ms，每轮新 Request ID、重新选择可表达 Profile。自动首包不会重复重启活动 Discovery，避免 Pending 饥饿。
- V5-20 已实现公开 3 B Prefix Peek；Full/Lite/Nano 都在完整 Decode/CRC 前执行 per-Link 本地 RX Ceiling 门禁。损坏 W3 帧在 W0 Link 上先返回不支持，在 W3 Link 上继续返回 CRC 错误。
- V5-21 未实现且仍阻塞于 S02。没有生产 Principal、逐跳控制认证、Session Generation、吊销与持久 Replay 时，不允许把 Wire Profile、Node ID 或 Bearer 当权限凭据。

V5-17～V5-20 当时 Windows Debug/Release 的 Full/Lite 均为 CTest `10/10`，Nano 为 `1/1`，独立配置契约为 `4/4`，`git diff --check` 无空白错误。当时 GCC 14.2 Release/Service OFF 的 Nano/Lite/Full Node 为 `2648/5944/9728 B`，Link 均为 40 B，Archive `.text` 为 `19692/65720/123876 B`。V5-26 后阶段值为 Node `2648/5960/9744 B`、Archive `.text` `19724/67316/125448 B`、Link `40 B`、Storage Layout Version=4；V5-30 阶段值为 Node `2648/5960/9752 B`、Archive `.text` `19820/68180/129124 B`；V5-33 阶段值为 Node `2648/5960/9752 B`、Archive `.text` `19884/68244/127792 B`。V5-44/V5-36 当前值为 Node `2648/6024/10080 B`、Archive `.text` `27662/73735/139017 B`、Link `40 B`、Storage Layout Version=5。各组都只是 Host ABI/裁剪证据。当前 Host 回归见 DOC-047；未访问硬件/COM，真实介质 RTT、CPU、栈、功耗和多板切换继续归 S06/S07。

## 12. 本轮边界

本阶段完成 V5-17～V5-20 的源码、公开接口、单元/模拟回归和文档闭环，相关提交已进入远端 `codex/v5-adaptive-wire` 基线；本文件不把后续 V5-22～V5-26 或硬件结果倒写成当时结论。生产安全与 Authorized Class 执行层继续不越过 S02 阻塞条件。
