# UCN V6-06 认证 HELLO、Capability 与 Path Budget 实现报告

## 1. 目标与边界

V6-06 解决的是：已经完成 V6-07 双向认证并拥有有效 Peer Session 后，双方如何低成本发现
能力变化、交换完整能力，并在发送前算出一条 Path 真正能装下多少数据。它不负责身份发现、
地址分配或准入，尤其不能创建第二套 JOIN 或写入 `ADMITTED`。

本阶段保持以下硬边界：

1. 普通 HELLO、Query、Advertise 必须来自有效 Peer Session 的 Hop authentication；
2. Capability 表示“能做什么”，不表示“被允许做什么”，Endpoint 仍需 V6-07 精确 ACL；
3. Group HELLO 只说明某个 claimed Binding 可能在线，不证明独立 Device Principal；
4. 所有状态均为调用方提供的固定内存，不驱逐旧项、不动态分配；
5. 当前 archive 仍由 `UCN_BUILD_V6_EXPERIMENTAL=ON` 显式启用，未接 v5 生产路径。

## 2. Wire 语义与安全分层

新增三个精确 Control Opcode：

| Opcode | Traffic/Delivery | 认证 | 权限效果 |
|---|---|---|---|
| `PEER_HELLO` | Q1 / LATEST | Peer Hop | 只刷新匹配缓存的发现租约，Digest 不同则要求 Query |
| `CAPABILITY_QUERY` | Q1 / RELIABLE | Peer Hop | 请求指定 Generation/Digest，不写 Capability |
| `CAPABILITY_ADVERTISE` | Q2 / RELIABLE | Peer Hop | 经字段、代际和载荷一致性验证后写固定缓存 |

三者都要求唯一 flags 组合 `PEER_HOP_CONTEXT | PROTOCOL_CONTEXT`、一跳、非零双 Binding
Generation 和 Session Generation。Security 层提供独立
`ucn_v6_security_protect_peer_discovery()`，不复用要求 E2E+ACL 的业务 API；接收结果也用
`hop_authenticated` 与 `endpoint_authorized` 两个位明确区分。这样 Capability 可以消费
逐跳认证结果，但绝不会因为“已经认证邻居”而获得业务 Endpoint 权限。

## 3. Capability Record

完整记录固定为 64 B，字段按大端编码：

- `capability_generation` 与 `link_instance_generation`；
- Carrier MTU 及 Carrier header/padding/CRC/tag/max-fragment；
- Adapter 可完整重组交付的 `link_frame_mtu`；
- Node ingress/egress 的 `processing_frame_mtu`；
- ordered/reliable/broadcast/unicast/security、标称速率与硬件优先级数；
- RX/TX software/hardware timestamp 能力及非零误差上界；
- Peer Feature、Hop/E2E Suite bitmap、最大 Message Class、RX Window 和并发 Transfer。

末尾 4 B 是必须为零的保留区。HELLO Summary 固定 24 B，仅包含 Capability Generation、
Link Generation 和 16 B Digest；Query 固定 20 B，可表达“未知能力”或“已知某一
Generation/Digest”。Digest 用于认证帧内的快速变化检测，不被解释为密码学身份凭据。

## 4. 固定缓存与代际规则

Capability Owner 的容量进入 Manifest/Layout Hash：

| 表 | 默认容量 | 满载行为 |
|---|---:|---|
| Peer Capability | 16 | `NO_SPACE`，不驱逐 |
| Path Capability | 16 | `NO_SPACE`，不驱逐 |
| Group Reauth Hint | 8 | `NO_SPACE`，不驱逐 |
| Group Hint Link Budget | 8 | `NO_SPACE`，不驱逐 |

同一 `{Principal, Binding, Session, Link Generation}` 域中，Capability Generation 只能
精确 checked-next；相同 Generation 只接受相同 Digest 的幂等重放。Principal 移动到新
Binding/Session/Link 域时，旧 Peer 槽和其关联 Path 立即失效。相同域中 Digest 或能力推进
也立即清除关联 Path，避免“新能力配旧预算”。

HELLO 只能刷新已经完全匹配的发现租约；Generation/Digest 不匹配只返回
`QUERY_REQUIRED`，既不覆盖完整能力，也不延长 Capability Lease。

## 5. Group HELLO Hint

V6-07 已验证 Group selector、Tag 和 Replay 后，V6-06 仍只建立：

```text
{ingress link id/generation,
 group id/generation,
 claimed realm/address/binding/session,
 absolute local deadline}
```

它不能写 Peer Capability、ACL、Session、Binding Lease 或 Authority。相同 hint 在未到期时
幂等且不刷新 deadline；到达 deadline 的 hostile input 返回 `TIMEOUT`，只有显式 Owner
expiry 才清槽。分配前同时预检 per-Link 和 per-Link/per-Group 两个 Token Bucket，任一表满
或无 Token 时所有表保持不变。

## 6. Path 能力归约与预算

Path 派生采用以下确定性规则：

```text
path_frame_mtu = min(path policy limit,
                     every link_frame_mtu,
                     every processing_frame_mtu)
feature/suite  = intersection(all hops)
message class  = min(all hops, path policy)
window/concurrency = min(all hops, path policy)
timestamp bits = intersection(all hops)
timestamp uncertainty = checked sum(all participating hop bounds)
```

随后复制调用方给出的精确零载荷 `ucn_v6_frame_t` Contract，调用同一个 Wire Codec 获得
公共头、Address Class、所有 Extension、E2E/Hop Tag 与 CRC 的真实字节数：

```text
payload_budget = path_frame_mtu - exact_encoded_zero_payload_bytes
fragment_data_budget = payload_budget - exact_fragment_header_bytes
```

任何 Unknown、零值、unsupported Feature/Suite、溢出、下溢或结果为零，都在安装 Path/TX/
重组槽之前失败。安装 Path 时还会重新核对目标 Peer 的 Binding、Session、Link Generation
与 Capability Digest，防止调用方把刚过期的派生结果写入表中。

## 7. 分项自审

### 7.1 Record 与 Digest

- 64/24/20 B 严格长度；
- 每个非零、范围、保留位与 Suite bitmap 都有确定性验证；
- decode 只在全部检查完成后写 output；
- Carrier MTU 不与 Link Frame MTU 混为一个含义。

### 7.2 Peer Discovery

- 逐跳认证和 Endpoint ACL 状态分开；
- 未认证、错误 Opcode/flags、错误 Link Generation、Payload 与 typed record 不一致均拒绝；
- no-space 在预留 Sequence 前返回，输出与 Frame 不变。

### 7.3 Capability Cache

- 同域 exact-next、相同记录幂等、冲突 Generation/Digest 拒绝；
- 表满不驱逐，Peer/Path 数量不变；
- Capability/Binding/Session/Link 变化立即失效旧 Path；
- Deadline 使用 64-bit 半开区间，创建时做 overflow 检查。

### 7.4 Group Hint

- Group discovery 不产生 Principal proof、Peer Capability 或 ACL；
- 全局槽、per-Link、per-Group 均固定；
- 预算采用 preview 后原子提交，失败不消耗另一层 Token；
- 输入帧不能清理或续期到期 hint。

### 7.5 Path Budget

- 使用 Wire Codec 计算 exact overhead，不复制一份易漂移常量；
- Feature/Suite/MTU/Window/Concurrency/Timestamp 全部归约；
- 下溢与 unsupported requirement 不写 output；
- 公开结构体的语义幂等使用逐字段比较，不依赖 C padding `memcmp`。

## 8. 验证结果

封存前验证矩阵以实际命令结果为准：

| 门禁 | 结果 |
|---|---|
| Windows GCC 定向 Config/Security/Capability | 3/3 |
| Windows GCC Full | 66/66 |
| MSVC Release v6 | 8/8 |
| WSL ASan/UBSan | Security/Capability 2/2 |
| WSL `-fanalyzer -Werror` | Security/Capability 2/2 |
| v6 default-OFF 生产 `ucn_core` 符号 | 0 |
| `git diff --check` | 无空白错误，仅行尾转换提示 |

## 9. 尚未完成

- 真实 CAN/Stream/USB/Wi-Fi Adapter 尚未填报并实测 Carrier/Frame 上限；
- 硬件 timestamp 类型和误差仍需逐平台标定；
- 生产 Node RX/TX、RouteSet 和 Transfer 尚未接入；
- 当前 Digest 只服务已认证帧内的变化检测，不替代 V6-07 Tag/Proof；
- 本报告是软件实现与自审证据，不是实机、性能、掉电或最终外审签字。

当前状态：`V6-06 软件实现与分项自审完成 / FINAL EXTERNAL REVIEW DEFERRED`。
