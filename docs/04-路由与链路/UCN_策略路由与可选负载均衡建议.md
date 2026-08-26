# UCN 策略路由与可选负载均衡建议

> 状态：**T22.1 已实现固定数据模型和质量快照，T22.2 已实现 Path ID 与安全控制面，T22.3 已实现固定严格/主备，T22.4 已实现 Q1 流亲和均衡**。未配置或 `AUTO_BEST` 的 Endpoint 仍保持当前自动转发语义。
> 关联任务：[T21 同对端多介质主备 Link](../00-项目管理/00-任务表.md#任务清单) → T22 策略路由与可选负载均衡。
> 设计目标：让不同业务可选择固定端到端路径、固定主备路径或可选自动分担，同时保持 MCU-first、固定内存、无 Linux 依赖和默认简单行为。
> 执行顺序、容量边界、验证门禁与正式子任务以[路由策略与负载均衡执行建议](UCN_路由策略与负载均衡执行建议.md)为准；本文件保留策略语义与协议约束。

## 1. 结论与当前边界

用户提出的需求分成两件事：

1. 某一类业务固定走指定路径，除非该路径确实断开才允许切换。
2. 在明确开启后，让不同业务流分散到多条已验证路径，避免全部挤到瞬时 Cost 最低的一条。

这两件事都合理，但**当前 v4 尚未完整实现**：

| 当前能力 | 当前真实行为 | 能否满足上述需求 |
| --- | --- | --- |
| Active / Candidate 路由 | 每个目标 Node 只保留一条当前动态业务出口和一条候选；候选经 Probe/Activate 验证后替换当前路径。 | 不能并发承载两条业务策略；它是换路，不是分流。 |
| `route_epoch` | 区分切换中的 Current / Previous 路径，默认仅保留短暂 grace。 | 不能当作业务 `Path ID` 或策略编号使用。 |
| `ucn_node_add_route(destination, egress_link)` | 本节点对目标登记一个静态下一跳；完整 A→B→C 路线需每个中继分别配置。 | 不能由源节点一次指定整条路径，也不能按 Endpoint/业务流区分。 |
| 静态路由失效 | 动态路由可由 Link/RERR 清理；静态路由不参加动态失效和重新发现闭环。 | 不能保证“静态路径断开后自动改找替代路径”。 |
| Link Cost | RREQ/RREP 与直连 Link 选择使用通用 Cost。 | 只能帮助择优，不能进行受控负载均衡。 |

另外，当前直连目标会优先于 Route Cache。因此静态配置也不能隐式覆盖“已直连目标”的选路规则。任何改变该优先级的需求都必须作为显式策略，而不能在 T22 中悄悄改变既有行为。

## 2. 与 T21 的边界

T21 和本建议解决的不是同一层问题：

| 层 | 要解决的问题 | 示例 |
| --- | --- | --- |
| T21：Bearer 主备 | 发给**同一下一跳 B**时，选 B 的 WiFi、UART 还是 CAN；正常只使用一个 Primary。 | `A -- WiFi/UART -- B`。 |
| T22：策略路由 | 发往**远端 C**的不同业务，选 `A→B→C`、`A→D→C` 或哪些路径参与分担。 | IMU 固定经 B，日志可经 B/D。 |

推荐先完成 T21：它先使一跳 Bearer 健康、身份和主备切换可靠。T22 再把多个不同下一跳组成的端到端路径作为有限的、可选择的 Path Set。T21 不做负载均衡；T22 也不把单个 Bearer 的逐帧复制当作负载均衡。

## 3. 建议的策略模型

默认行为保持当前自动路由。仅当产品配置为某个 `(destination_node_id, endpoint)` 建立策略时，才启用下列模式：

| 模式 | 业务行为 | 路径断开后的行为 | 适用例子 |
| --- | --- | --- | --- |
| `AUTO_BEST` | 使用当前已验证的最低综合 Cost 路径。 | 沿既有 RERR / 按需寻路恢复。 | 普通遥测。 |
| `PINNED_STRICT` | 只走指定 Path。 | 不改走其他路径；向本地业务报告失败或执行安全动作。 | 时序/合规要求严格的专用链路。 |
| `PINNED_FAILOVER` | 正常固定 Primary Path。 | **仅在硬故障确认后**切换指定 Backup；没有 Backup 时可按配置自动寻路。 | 控制/IMU 走固定低时延路径，但保留断链恢复。 |
| `AUTO_BALANCE` | 仅在若干已验证 Path 间，按业务流分配。 | 故障 Path 从集合剔除，受影响流重新绑定健康 Path。 | 日志、非关键遥测、多源状态。 |

初版策略表必须是固定数组。建议产品先从“每节点最多少量策略、每策略最多 Primary + Backup 两条 Path”开始；具体上限由目标 MCU 的 RAM、目标节点数和业务数量在 T22.1 前冻结，而不是写死为所有产品通用数值。

### 3.1 为什么策略键必须包含 Endpoint

只按目标 Node ID 配置路线不够：同一个 C 可能同时提供 IMU、气压计、温度和日志。建议最小匹配键为：

```text
(destination_node_id, endpoint) → route_policy
```

若将来同一 Endpoint 内还要拆分多个独立、长期并发的业务流，才引入可选 `stream_id`。`stream_id` 应由 Extended/产品 Payload ABI 定义，Core 不应在没有真实需求时扩展每一帧。

## 4. Path ID 与控制面

仅靠源节点本地 API 不能实现真正的“指定完整路径”：中继 B 若同时拥有到 C 的多条出口，必须知道此帧属于哪条策略路径。为避免每帧携带完整 `A→B→C→…` 节点列表，建议使用短小的 `Path ID`：

```text
业务策略： (C, Endpoint 0x40) → PINNED_FAILOVER → Path P1 / Backup P2
业务帧：   Destination=C, Endpoint=0x40, Path ID=P1
中继 B：   (P1, C) → 下一跳 C
```

这样中继只按 Path ID 转发，不需要理解业务 Payload，也不需要解密端到端密文。Path 由受限控制面安装、探测和确认，而不是让每个业务帧携带完整源路由。

### 4.1 已冻结的协议兼容边界

v4 已新增独立 `Path ID`：普通头为 32 B，Route Extension 为 36 B，带 Path ID 的业务帧为 40 B。Path 头必须同时带 Route Extension 与 Path Flag，`path_id` 为非零；旧实现会拒绝未知 Flag/头尺寸，不能把 Path 帧误作普通路由帧。`route_epoch` 继续只标记 Active/Previous 切换期，绝不复用为 Path 身份。

路径安装、更新和撤销由 `PATH_INSTALL/PATH_REVOKE` 控制帧完成。接收方既需已有 Security Provider 授权，又需显式 Path 控制面授权回调；没有回调即默认拒绝，不能让任意节点伪造控制帧修改路线。受保护业务把 Path ID 放入 30 B 不可变 AAD，继电器不解密 Payload，但也不能改写 Path ID。

### 4.2 建议的有限状态

每条 Path 保存固定且最小的信息：

| 字段 | 用途 |
| --- | --- |
| `path_id` | 业务帧和中继转发表使用的短标识。 |
| `destination`、`next_hop`、`egress_bearer` | 指定到目标的本跳转发关系；Bearer 仍由 T21 选择。 |
| `state` | `VERIFIED`、`SUSPECT`、`DOWN`、`RETIRING` 等受限状态。 |
| `cost`、Probe/ACK 统计、过期时间 | 判断是否仍可作为候选或负载均衡成员。 |
| `policy_ref` | 仅供本地固定策略表关联；不能扩大为无限全网拓扑。 |

`ROUTE_ERROR` 需要携带受影响的 `Path ID` 或等效策略范围：Path P1 故障时只清除 P1，P2 和同目标 C 的其他策略不得被一并清掉。

## 5. 固定路径的切换规则

`PINNED_FAILOVER` 是“固定路线，只有断了才找替代”的直接实现方式：

```text
IMU → C：Primary P1 = A→B→C，Backup P2 = A→D→C

P1 正常或仅 Cost 抖动：继续 P1，不因另一条路径分数更低而切换
P1 Driver Down / 明确发送失败 / RERR / 已验证失联：标记 P1 DOWN
    ↓
P2 已 VERIFIED：下一帧切 P2
P2 不存在或不可用：若策略允许，受限 RREQ 建立新路径；否则报告本地失败
```

- `PINNED_STRICT` 不执行自动发现，避免“固定”的语义被悄悄破坏。
- `PINNED_FAILOVER` 的质量变差不是断链。Cost 只用于维护健康/探测和选择备用，不应因短时抖动移动正在固定发送的流。
- 已交给旧驱动的帧不能撤回；Q1 Payload 仍建议带序号/时间戳，Q0 必须预建 Path 或有本地失效安全动作。
- 任何路径恢复、探测和切换仍受现有控制 Token 与 Q0/Q1 调度约束，不能因策略功能挤占控制业务。

### 5.1 当前 T22.3 的精确触发条件

Policy 通过 `primary_local_path_id` 和可选 `backup_local_path_id` 指向本地固定表；该表以 `wire_path_id` 连接已经安装的 T22.2 Path。`PINNED_STRICT` 只调用 Primary；`PINNED_FAILOVER` 只有收到 `UCN_ERR_LINK_DOWN` 或 `UCN_ERR_NOT_FOUND` 时才将该 Path 视为硬失效并尝试 Backup。`NO_SPACE`、Security/ACL、配置错误和其它发送失败不会悄悄切换。

若 Primary/Backup 都不可用，`allow_discovery_on_hard_failure` 只对 Q1 有效：它会进入既有受限 RREQ/固定等待槽，成功后允许该次业务按普通自动 Route 发送。此选择必须由产品显式开启，且不再是固定路线；Q0 始终只接受预安装 Path 或本地失败。Strict 策略禁止设置此选项。Cost、RTT、队列压力只供后续健康/均衡判断，本阶段不会让已固定 Primary 因分数波动切换。

## 6. 可选自动负载均衡

自动均衡必须默认关闭，且只从通过 Probe/ACK、没有处于 `SUSPECT` 的 Path 中选择。它不应把所有帧按轮询方式交替发送：逐帧条带化会带来乱序、抖动和中继排队放大。

T22.4 已将这个边界写入 Core，V5-44/V5-36 又完成 LC-1 接入：`AUTO_BALANCE` 只接受精确 Q1 Policy，Primary 必填、Backup 可选且均须是已验证 Path；Flow 键为当前最小 `(destination, endpoint, Q1)`，零 `balance_flow_lease_ms` 使用默认 2 s。首次/租约到期以 Full 本地 `effective_select_cost ×（当前 Flow 数 + 1）` 选择成员，Known 基础 Cost 仍优先于 Unknown。单一 Flow 不会逐帧轮换。默认连续 3 个 500 ms 队列压力快照达到 800‰，或 Path Down 时，才将未到期受影响 Flow 重绑。Q0、自动发现、帧复制和带宽聚合均未实现。

初版建议如下：

1. **按流绑定而不是按帧分散**：同一 `(source, destination, endpoint[, stream_id])` 在一个租约窗口内固定使用一条 Path；不同流才可分配到不同 Path。
2. **Q0 初版不参与自动均衡**：Q0 只能使用显式 `PINNED_STRICT`/`PINNED_FAILOVER` 或已冻结的安全路径。
3. **Q1 才可选择参加均衡**：日志、非关键遥测等先进入；每条流的业务序号仍由产品 Payload 提供。
4. **使用 LC-1 归一后的同量纲整数分**：Full 以 `effective_select_cost ×（活动 Flow 数 + 1）` 选择；RTT/失败率/Queue/介质项先经固定表归一，绝不裸相加。持续 Queue 仍可独立触发未到期 Flow 重绑。Adapter 负责把介质专有指标归一为通用字段；Core 不读取 RSSI、Bus-Off 等私有字段。
5. **只在边界重新绑定**：流启动、租约到期、Path Down 或明显持续拥塞时重选，不因单次指标波动迁移。
6. **不做隐式复制或带宽聚合**：同一帧不复制到多条 Path；大包/分片仍属于 Extended/Carrier 的独立问题。

这使“最优”不再等于“所有业务都挤到同一条路”，但仍保留 MCU 可预测的内存、顺序和控制面上限。

## 7. 建议 API 与产品配置边界

当前 `ucn_node_send_endpoint()` 不应暴露 WiFi、CAN、UART 或裸 `ucn_link_t *`。后续 API 应由应用选择**策略**，而不是选择物理介质：

```c
/* 示意：接口名称和枚举在 T22.1 冻结前不作为已实现 API。 */
ucn_node_set_route_policy(&node, destination, endpoint, &policy);
ucn_node_send_endpoint(&node, destination, endpoint, UCN_TRAFFIC_Q1_REALTIME,
                       payload, length);
```

其中 `policy` 指向预配置的 Mode、允许的 Path Set、是否允许断链自动发现、可否参加均衡等固定数据。应用正常发送时仍只写目标和 Endpoint；它不用知道帧最后经过 WiFi、UART、CAN 或哪个中继。

产品层需要冻结并审计：

- 哪些 Node/Endpoint 可使用固定或均衡策略；
- 哪些 Q0 业务只允许指定 Path；
- Path 安装/修改的授权主体；
- `PINNED_FAILOVER` 在无 Backup 时是否允许自动发现；
- 每条 Path、每个策略、每个 Flow 的固定容量和过期策略。

## 8. T22 实施与验收门禁

| 子任务 | 实现内容 | 单元测试 | 虚拟/实机验收 |
| --- | --- | --- | --- |
| T22.1 | 已实现固定容量 Policy/当地 Path/Q1 Flow/Link 质量快照、模式和编译期上限；未配置业务保持 `AUTO_BEST`。`local_path_id` 未上帧。 | `test_policy.c` 覆盖表满、重复、非法模式、Endpoint 匹配、质量窗口/EWMA 与旧 API；四 CTest Profile 全绿。 | S3 A/WROOM 已构建；真实 RTT/Cost 标定和硬件资源峰值待实测。 |
| T22.2 | 已完成 40 B Path Header、固定逐跳转发表、受认证 `PATH_INSTALL/PATH_REVOKE`、Path AAD 和 Path 范围 RERR；T22.3 已把已验证 Path 接入发送 API。 | `test_path_control.c`：编解码、默认拒绝/显式授权、伪造 Path AAD 拒绝、重复/过期/撤销、P1 RERR 不误清 P2；四 CTest Profile 全绿。 | 虚拟 A→B→D(P1) 和 A→C→D(P2) 已验证透明中继；真实多板留 T22.7。 |
| T22.3 | 已完成 `PINNED_STRICT` 与 `PINNED_FAILOVER`；只在 `LINK_DOWN`/Path 不存在时失效 P1，按策略走 P2；仅 Q1 无 Backup 时可显式受限寻路。 | `test_path_control.c` 覆盖 Strict 不回退、Failover P1→P2、Q0 预装 Backup/Q0 不发现、Q1 无 Backup RREQ 与静态/AutoBest 回归；四 CTest Profile 全绿。 | 虚拟双中继完成；真实拔掉 P1、切换时延/丢失/乱序和本地安全留 T22.7。 |
| T22.4 | 已完成可关闭的 Q1 流亲和自动均衡、租约和有界重绑定；不做逐帧轮询。 | `test_policy.c` 的配置拒绝、`test_path_control.c` 的同流固定/两流分散/持续拥塞与 Down 重绑/租约过期、四 CTest Profile。 | 虚拟双中继已通过；多块板、多 Path 压力下的负载比例、P50/P95、乱序、丢包仍待 T22.7。 |
| T22.5 | 已让 T21 Bearer 选择与 T22 Path 选择联动，并完成 Route/安全/Endpoint/64 B Profile 回归。 | 单 Bearer 切换保持 Path ID；全 Bearer Down 仅撤销受影响 Path、同步本地 Policy Down，且中继经上游 Bearer 回 Path RERR。 | C99 虚拟 A⇄B⇄D 双 Bearer + 独立 P2 已通过；WiFi+UART（或 CAN-FD）实机留 T22.7。 |
| T22.6 | 已完成受授权、限频的单 Node 策略诊断：`POLICY_DIAGNOSTIC_REQ/REPLY`、三页 Summary、固定 Policy/Path/Flow/质量槽位和超时回收。 | `test_policy_diagnostic.c` 覆盖权限、令牌、表满、空槽、Q0 优先、Path/Bearer/Flow/质量查询与超时；四 CTest Profile。 | C99 虚拟管理 Node→目标 Node 已通过；真实 Adapter、多板资源、故障注入和控制面开销测量留 T22.7。 |

T22.1～T22.6 的 C99 单测、虚拟双中继/管理查询和四套构建已全绿，因此可表述为“已实现受限的 Q1 流亲和均衡、Path/Bearer 联动和受授权按需诊断”；但在 T22.7 的真实多板压力验收前，不能承诺实际 ESP-NOW/WiFi/CAN/UART 的吞吐、无缝切换、负载比例、时延、乱序或最优分担效果。
