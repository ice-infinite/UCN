# UCN 策略路由与可选负载均衡建议

> 状态：**后续设计建议，未实现**。基于 UCN v4 C99 Core 源码快照（2026-08-08）整理；不改变当前 v4 线格式、Core API 或测试结论。  
> 关联任务：[T21 同对端多介质主备 Link](00-任务表.md#任务清单) → T22 策略路由与可选负载均衡。  
> 设计目标：让不同业务可选择固定端到端路径、固定主备路径或可选自动分担，同时保持 MCU-first、固定内存、无 Linux 依赖和默认简单行为。

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

### 4.1 协议兼容边界

当前 v4 基础头/Route Extension 没有独立业务 `Path ID`，`route_epoch` 也不可复用。因此实现 T22 前必须冻结以下之一：

1. 新的受版本保护的路由扩展字段；或
2. 后续协议版本/明确 Profile 的固定 `Path ID` 线格式。

无论选择哪一种，旧节点都必须显式拒绝无法理解的版本/标志/格式，不能把 `Path ID` 偷塞进现有保留含义的字段。路径安装、Path ID 与策略权限也必须接受认证/ACL 约束；不允许网络中的任意节点通过伪造控制帧指定某条业务路线。若帧使用端到端保护，Path ID 的完整性覆盖范围也需要在安全 AAD 契约中冻结，防止被中途篡改。

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

## 6. 可选自动负载均衡

自动均衡必须默认关闭，且只从通过 Probe/ACK、没有处于 `SUSPECT` 的 Path 中选择。它不应把所有帧按轮询方式交替发送：逐帧条带化会带来乱序、抖动和中继排队放大。

初版建议如下：

1. **按流绑定而不是按帧分散**：同一 `(source, destination, endpoint[, stream_id])` 在一个租约窗口内固定使用一条 Path；不同流才可分配到不同 Path。
2. **Q0 初版不参与自动均衡**：Q0 只能使用显式 `PINNED_STRICT`/`PINNED_FAILOVER` 或已冻结的安全路径。
3. **Q1 才可选择参加均衡**：日志、非关键遥测等先进入；每条流的业务序号仍由产品 Payload 提供。
4. **使用加权选择**：以平滑后的总 Cost、近期 ACK/丢包、RTT 或确认时延、**本地队列压力/链路占用**形成权重。Adapter 继续负责把介质专有指标归一为通用 Cost；Core 不读取 RSSI、Bus-Off 等字段。
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
| T22.1 | 冻结固定容量 Policy / Path / Flow 数据模型、模式和编译期上限；保持未配置业务的 `AUTO_BEST` 兼容行为。 | 表满、重复键、非法模式、Endpoint 匹配、旧 API 回归。 | 记录 RAM/Flash/栈增量。 |
| T22.2 | 冻结兼容的 `Path ID` 线格式与受认证的安装/撤销控制面；中继建立有限转发表。 | 编解码、未知格式拒绝、伪造/未授权安装拒绝、TTL/重复/过期。 | A→B→C 和 A→D→C 可独立安装 P1/P2。 |
| T22.3 | 实现 `PINNED_STRICT` 与 `PINNED_FAILOVER`；只在硬故障/RERR 时失效 P1，按策略改走 P2 或受限寻路。 | 直连优先边界、静态路由兼容、单 Path RERR 不误清 P2、禁止自动发现。 | 持续 IMU 流经 P1；拔掉 P1 后验证 P2 与 Q0/Q1 行为。 |
| T22.4 | 实现可关闭的 Q1 流亲和自动均衡、租约和有界重绑定；不做逐帧轮询。 | 多流分散、单流顺序、关闭后不分流、拥塞/Down 重绑、Q0 排除。 | 多块板、多 Path 压力下测每路径负载、P50/P95 时延、乱序、丢包。 |
| T22.5 | 让 T21 Bearer 选择与 T22 Path 选择联动，并补 Route/安全/Endpoint/64 B Profile 回归。 | 单 Bearer 切换不改变 Path ID；全 Bearer Down 的 Path 状态与 RERR；受保护 Path ID 完整性。 | WiFi+UART（或 CAN-FD）与两个中继路径混合拓扑。 |
| T22.6 | 完成真实 Adapter、多板资源和故障注入验证。 | Driver Down、队列满、空口/总线错误、恢复抖动。 | 测 RAM/Flash/CPU、功耗、控制面开销、收敛时间和干扰下稳定性。 |

在 T22.1～T22.5 的 C99 单测、虚拟多拓扑和三档构建全绿前，本建议不得表述为当前 v4 已有“指定完整路径”或“自动负载均衡”。在 T22.6 完成前，也不能承诺实际 ESP-NOW/WiFi/CAN/UART 的吞吐、无缝切换或最优分担效果。
