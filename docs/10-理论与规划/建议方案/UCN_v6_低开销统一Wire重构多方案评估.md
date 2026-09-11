# UCN v6 低开销统一 Wire 重构多方案评估

> 状态：PROPOSED / 方案评估，未冻结 Wire、ABI 或实现
>
> 日期：2026-09-06
>
> 范围：只讨论如何降低 v6 线上开销并保持统一协议形态；当前不修改 Wire、Security、Runtime 或生产接线
>
> 兼容策略：UCN 尚未发布，本方案不保留 v4/v5/v6 Contract 1 的线上兼容；旧版本只由 Git 历史保存

各 Contract 的字段、长度、允许组合和完整运行流程见：[UCN v6 低开销统一 Wire：各 Contract 字段与运行机制详细设计](UCN_v6_低开销统一Wire各Contract字段与运行机制详细设计.md)。

## 1. 为什么现在必须重新设计

UCN 最初要解决的核心问题很简单：

1. 节点能够自动发现路径；
2. 数据能够经过一个或多个节点送达目标；
3. MCU 使用固定内存、非阻塞接口，不依赖 Linux 或中心服务器。

后来加入的 Cluster、加密、Realtime、Transfer、可靠请求等能力都有实际价值，但它们不应让最普通的“自动寻路后发送一帧数据”永久承担全部功能的字段成本。

当前 v6 Contract 1 将 Realm、Source/Destination Binding Generation、Session Generation、Origin/Hop Sequence、Payload Length、CRC，以及多类安全和消息上下文直接组合在 Frame 中。基础 A0 Frame 已为 41 B；普通 DATA 又需要 Peer Hop Context、16 B Hop Tag 和 Message Context，因此最低开销远高于 41 B。

按当前实现常量计算，以下数字均不含业务 Payload 和 Carrier 自身开销：

| 当前 A0 组合 | 开销 |
| --- | ---: |
| 无扩展基础 Frame | 41 B |
| 普通 Hop-authenticated DATA | 77 B |
| Hop + E2E DATA | 101 B |
| Hop + E2E + Route | 105 B |
| Hop + E2E + Route + Path | 111 B |
| 再加 Hop Budget | 127 B |

其中扩展固定长度为：Peer 7 B、E2E 8 B、Message 13 B、Route 4 B、Path 6 B、Hop Budget 16 B，每个 Hop/E2E Tag 为 16 B。

这说明问题不是“删掉一个 CRC”就能解决，而是当前模型把以下三种生命周期不同的信息混在了一起：

- 每帧都会变化的信息：类型、流量等级、序号、Hop Limit、Payload；
- 一个 Flow/Path 生命周期内稳定的信息：端点、交付方式、Operation 语义、Path、Route、Realtime Policy；
- 一个 Session/Realm 生命周期内稳定的信息：完整身份、Binding Generation、密钥 Suite/ID/Generation、ACL 与 Capability。

正确的压缩方式不是牺牲这些语义，而是只在它们建立或改变时发送完整值，稳定阶段用不可复用的短 Context 标识引用。

## 2. 不可妥协的目标

### 2.1 必须保留

- 单一 v6 协议版本和一套规范解析模型；
- 自动寻路与多跳转发；
- 固定内存、编译期容量、无动态分配；
- 地址复用、Session、Route、Path 的防 ABA 约束；
- 不可信无线链路上的认证、防篡改和重放保护；
- Cluster、Realtime、Transfer、Group 等可选能力；
- Full、Lite、Nano 仍使用同一 Wire 规范，且只表示固定容量档位；能力集合由正交的 Composition 决定；
- 无法满足 Endpoint 安全或功能要求时失败关闭，不静默降级。

### 2.2 必须改变

- Cluster 关闭时，普通 DATA 不携带任何 Cluster 字段；
- Realtime 关闭时，普通 DATA 不携带时间戳、Deadline 或 Budget 字段；
- Transfer 未使用时，普通 DATA 不携带 Fragment/ACK/Credit 字段；
- Request/Result 未使用时，不携带 Operation ID；
- E2E 未要求时，不携带 E2E Context 和 E2E Tag；
- 已建立 Session/Flow/Path 后，不再逐帧重复完整身份、密钥选择、端点和代际；
- 不再以“所有可能字段 + Flags”的最大 Frame 作为最常用 Frame。

### 2.3 “统一形式”的准确定义

统一不等于每帧字段完全相同。本文建议把统一定义为：

1. 所有 Frame 共享同一个 v6 版本入口；
2. 第一个字节无歧义地选择有限个固定 Header Contract；
3. 每个 Contract 的字段位置和长度固定，不使用任意 TLV 链；
4. Contract 之间共享相同的 Traffic、Delivery、Interaction、安全与错误语义；
5. 同一 Context 安装、失效、防 ABA 和失败关闭规则适用于所有 Bearer；
6. Decoder 根据 Contract 一次分派，不通过试探或降级解析猜测格式。

因此，统一协议可以拥有若干固定的短头形态，就像一套指令集可以拥有有限种固定指令格式，而不是把所有操作数放进每条指令。

## 3. 哪些检查应留在节点端

用户提出“能在节点端添加的认证或者校验绝不再协议端添加”，这个方向应转化为明确的分层规则。

### 3.1 可以只保留在节点本地状态中的内容

- Device Principal 与本地短地址的完整映射；
- Address Binding Generation；
- Peer Session Generation；
- Security Suite、Key ID、Key Generation；
- Endpoint ACL 与允许的 Protocol Opcode；
- Flow 的 Source/Destination Endpoint；
- Delivery Guarantee 与 Interaction Role；
- Route/Path 的完整代际和出口；
- Cluster Membership、Config、Epoch；
- Realtime Policy、初始 Deadline、uncertainty policy；
- Capability 与 MTU/Payload Budget。

这些信息在 Context 建立时完成认证并存入固定表，普通 Frame 只携带短 Context ID。

### 3.2 不可信链路上不能省成零字节的内容

以下信息如果完全不在线上出现，接收者就无法判断 Frame 是否被篡改、重放或错配：

- 能选中唯一安全上下文的短 Context ID；
- 进入 anti-replay 域的 Sequence；
- 密码认证 Tag；
- 多跳转发需要的 Hop Limit 或等价受保护预算；
- 无状态/恢复 Frame 所需的明确身份和代际。

节点本地 ACL 可以判断“谁允许做什么”，但不能证明收到的字节真的来自那个人。CRC 只能发现随机错误，也不能替代密码认证。

### 3.3 可以由 Bearer 合同省略的字段

只有 Link 在配置阶段明确证明具备以下能力时，UCN 才能省略重复字段：

- 已认证、不可伪造的 Peer 身份；
- 带重放保护的链路 AEAD/Auth；
- 明确的 Frame 长度边界；
- 足够的随机错误检测；
- 静态且不可由远端 Frame 自行声称的 Link Contract。

例如受控 CAN-FD、已建立加密 Session 的 ESP-NOW/802.15.4 或可靠 Stream Carrier 可以采用不同的 Carrier 优化。是否省略必须由本地 Link 配置决定，不能由收到的 Frame Flags 决定。

## 4. 方案一：精简无状态头（Stateless Compact）

### 4.1 思路

保留接近 v5 的短头，每帧仍携带 Source、Destination 和基础序号，不依赖预安装 Flow。Realm 的地址宽度由 Realm Manifest 固定，不再逐帧编码 Address Class。

建议候选 A1 基础布局：

| 字段 | 建议长度 | 说明 |
| --- | ---: | --- |
| Version + Contract | 1 B | 高 4 bit Version，低 4 bit Contract |
| Type + Traffic + Delivery | 1 B | 按固定 Contract 解释 |
| Source Address | 2 B | A1 Realm；A0 为 1 B |
| Destination Address | 2 B | A1 Realm；A0 为 1 B |
| Hop Limit | 1 B | `1..254`；需要更大范围时使用扩展 Contract |
| Sequence | 4 B | 本 Contract 的唯一重放/去重域 |
| Hop Context ID | 0/2 B | 仅 UCN 自己提供 Hop Auth 时存在 |
| Payload Length/Flags | 1～2 B | 能从 Carrier 得出长度时可省略 |
| CRC | 0/2 B | 有 Tag 或可靠 Carrier 时可省略 |

目标基础开销约为：

- A0 明文：10～13 B；
- A1 明文：12～15 B；
- 不可信链路加短 Hop Auth Tag 后：26～29 B；
- 若需要 E2E，再增加 E2E Context/Tag 或切换到对应固定 Contract。

### 4.2 优点

- 状态最少，节点重启后更容易恢复；
- 第一个业务 Frame 不必先建立 Flow；
- 适合低频、偶发、目标经常变化的数据；
- 自动寻路语义直观；
- 对 Nano 和 Classic CAN 友好；
- 与当前 v5 的低开销经验最接近。

### 4.3 缺点

- 每帧仍重复 Source/Destination；
- 多跳加密需要 Hop 序号与 Tag；
- Endpoint、Operation、Path、Realtime 等仍需要额外 Contract；
- 要彻底防地址复用 ABA，需要依赖邻居/Session 中的 Binding 上下文，而非仅靠 Frame 地址。

### 4.4 适用场景

- 广播或低频遥测；
- 临时目标；
- 路由建立前的首包；
- Context 表紧张的 Nano 节点；
- 不值得为少量数据建立长生命周期 Flow 的业务。

## 5. 方案二：Session/Flow 上下文压缩（Context Compact）

### 5.1 思路

JOIN/Peer Session 阶段建立 Hop Context；首次业务或 Capability/Flow Setup 阶段建立 Flow Context。后续普通 Frame 只携带短 ID 和序号。

本地 Flow Context 至少绑定：

```text
Parent Peer/Realm Session Generation
Source Principal / Source Address Binding
Destination Principal / Destination Address Binding
Source Endpoint / Destination Endpoint
Delivery Guarantee / Interaction Role
Protocol Opcode
Security Policy and Key Context
Route or Path policy
Traffic Class ceiling
Optional Realtime policy
```

建议稳态直接 Frame：

| 字段 | 建议长度 |
| --- | ---: |
| Version + Contract | 1 B |
| Type + Traffic + short flags | 1 B |
| Hop Context ID | 2 B |
| Flow Context ID | 2 B |
| Hop Sequence | 4 B |
| Payload Length | 0～2 B |

基础开销目标为 10～12 B；加 12 B Hop Tag 后约 22～24 B。若 Link 本身已经完成等价认证，则 UCN 逻辑头可接近 10～12 B。

### 5.2 优点

- 高频周期流效率显著提高；
- 完整身份、端点、密钥选择和 Policy 只发送一次；
- Flow ID 能同时替代多个当前扩展；
- 易于为传感器、控制指令、遥测等稳定业务做固定资源预留；
- 不需要在每个 Fragment 重复 13 B Message Context。

### 5.3 缺点

- 首次发送前需要 Setup；
- Context 表满时必须失败关闭或使用无状态 Contract；
- Session 重建、切路或 Policy 变化会使 Context 失效；
- 防 ABA、超时、删除和 ID 耗尽规则必须非常严格；
- 不能把短 ID 当成永久身份。

### 5.4 适用场景

- 周期传感器数据；
- 高频控制流；
- 长连接 Request/Result；
- 稳定的一跳或少量固定 Peer；
- 同一 Endpoint 组合反复发送。

## 6. 方案三：逐跳 Label 转发（Label Switched）

### 6.1 思路

自动寻路阶段不只建立 `Destination → egress`，还在每个中继安装本地 Forwarding Label。稳态多跳 Frame 不再携带完整 Source/Destination/Route Generation，而只携带入站 Label。中继用固定表做 O(1) 查找、替换出站 Label，然后发送。

建议稳态路由 Frame：

| 字段 | 建议长度 |
| --- | ---: |
| Version + Contract | 1 B |
| Type + Traffic + Delivery | 1 B |
| Forwarding Label | 2 B |
| Flow Context ID | 2 B |
| Hop Limit | 1 B |
| Hop Sequence | 4 B |
| Origin Sequence | 0/4 B | 仅 E2E/可靠语义需要 |
| Payload Length | 0～2 B |

基础开销目标：

- Hop-only：11～13 B；
- 需要 Origin Sequence：15～17 B；
- 加 12 B Hop Tag：23～29 B；
- 加 E2E Tag 后的完整多跳安全帧：约 39～45 B。

### 6.2 优点

- 多跳稳态头最短；
- 中继无需解析完整 Endpoint、Operation、Cluster 或 Realtime 内容；
- 查表、改 Label、更新 Hop Limit/Tag 即可转发；
- 很适合以后用 FPGA、交换芯片或专用硬件加速；
- 路径越长不会让单帧头变长。

### 6.3 缺点

- 每个中继要保存 Label 状态；
- Route 建立阶段更复杂；
- 切路必须安装新 Label，再原子切换，最后回收旧 Label；
- Label ID、Parent Session/Route Generation 必须防 ABA；
- 第一包延迟可能高于纯无状态路由；
- 表容量决定同时活跃的路径数量。

### 6.4 适用场景

- 稳定的多跳链路；
- 大量周期流；
- 需要低中继解析开销；
- 将来计划 FPGA/硬件转发的骨干节点；
- 大规模 Cluster 间骨干通信。

## 7. 方案四：Carrier 辅助省略（Bearer Assisted）

### 7.1 思路

UCN 的逻辑 Frame 语义保持统一，但 Adapter 根据静态 Link Contract，不重复携带 Carrier 已经可靠提供的字段。

示例：

- CAN/CAN-FD ID 可承载 Traffic/短 Link Label；
- CAN-FD DLC 已提供长度，不再携带 Payload Length；
- Stream Carrier 自己负责 Magic、长度和边界，UCN 内层不再重复 Magic；
- 已认证无线 Session 可以提供 Peer 身份、Hop anti-replay 和 Tag；
- 点对点 UART Link 的 Peer 可由端口配置固定，不必每帧声明。

### 7.2 优点

- 物理链路效率最高；
- Classic CAN 等小 MTU Bearer 获益明显；
- 可利用 ESP-NOW、802.15.4、BLE、TLS/DTLS 等已有安全能力；
- 不必在所有 Bearer 上重复同一层功能。

### 7.3 缺点

- Adapter 合同更复杂；
- 同一个逻辑 Frame 的 Carrier 表示不同；
- 必须证明 Carrier 提供的是等价能力，不能仅凭“有 CRC/有加密”就省略；
- 抓包与跨 Bearer 测试工具需要先恢复统一逻辑视图；
- 不适合作为唯一核心方案，因为并非所有 Link 都提供这些能力。

### 7.4 适用场景

- 固定 Peer 的点对点 Link；
- CAN/CAN-FD；
- 已有认证 Session 的无线链路；
- 极端重视每字节效率的产品配置。

## 8. 四套方案对比

| 方案 | 稳态基础头目标 | 状态成本 | 首包成本 | 多跳效率 | 硬件转发 | 主要用途 |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| 一：精简无状态 | 12～17 B | 低 | 低 | 中 | 中 | 偶发流、恢复、Nano |
| 二：Session/Flow | 10～12 B | 中 | 中 | 中 | 中 | 高频直接流 |
| 三：逐跳 Label | 11～17 B | 中高 | 高 | 高 | 高 | 高频多跳、骨干 |
| 四：Carrier 辅助 | 可降至 8～12 B | 取决于 Bearer | 取决于 Link | 高 | 高 | 特定链路优化 |

这些方案不是互斥的。若只能选一套：

- 只选方案一，简单但无法达到最优稳态效率；
- 只选方案二，偶发流和重启恢复成本较高；
- 只选方案三，路由状态复杂且不适合所有节点；
- 只选方案四，会让协议过度依赖 Bearer。

## 9. 推荐方案：统一 Contract + 三条数据路径

推荐组合方案一、二、三，并把方案四作为 Adapter 级可选优化。它不是多版本兼容，而是同一个 v6 中有限、固定、可直接分派的 Header Contract 集合。

### 9.1 推荐 Contract 集合

| Contract | 名称 | 用途 | 是否携带完整地址 | 是否依赖上下文 |
| --- | --- | --- | ---: | ---: |
| C0 | ABSOLUTE | Bootstrap、Recovery、Context Setup、Route Discovery | 是 | 否/最少 |
| C1 | STATELESS | 偶发普通数据、无 Flow 回退 | 是 | 仅 Hop Session 可选 |
| C2 | DIRECT | 已建立 Peer/Flow 的直接数据 | 否 | 是 |
| C3 | ROUTED_HOP | 已建立 Route Label 的最短 Hop-only 多跳数据 | 否 | 是 |
| C4 | ROUTED_ORIGIN | 多跳且要求 Latest/Reliable/Origin Replay/E2E | 否 | 是 |
| C5 | GROUP | Group/广播数据 | 按 Group Context | 是 |

建议第一字节固定为：

```text
bits 7..4 : protocol_version
bits 3..0 : header_contract
```

第二字节按所有 Contract 共享的固定规则表达 Frame Type、Traffic Class 和少量存在位。剩余字段由 Contract 固定定义，不允许任意 TLV 排列。

### 9.2 三条数据路径

```text
未知/偶发目标
    C1 STATELESS
        ↓ 发现路径且流量持续
稳定直接 Peer
    C2 DIRECT
        ↓ 多跳路径已安装 Label
稳定多跳
    C3/C4 ROUTED
```

C0 只负责建立或修复上下文，不应成为普通高频业务格式。

### 9.3 统一性为什么没有被破坏

- 所有 Contract 都由同一个 version/contract 字节选择；
- 同一个 Codec 模块进行严格分派；
- 同一套 Result、Traffic、Delivery、Interaction 语义；
- 同一套 Context 生命周期和 anti-ABA 规则；
- 同一个 Capability 过程协商双方可用 Contract；
- 不支持某 Contract 时明确拒绝，不能尝试按更旧格式解析；
- Full/Lite/Nano 不发明自己的 Wire，也不直接决定 Contract 集合；Composition 声明实际编译能力，Profile 只为这些能力提供容量。

## 10. 地址宽度不应逐帧重复声明

当前 A0～A3 表达不同地址宽度是合理的，但 Address Class 不需要每帧选择。

建议由 Realm Manifest 在部署时固定：

| Realm 类型 | 地址宽度 | 普通地址容量 |
| --- | ---: | ---: |
| Small | 1 B | 254 |
| Standard | 2 B | 65,534 |
| Large | 3 B | 16,777,214 |
| Global | 4 B | 4,294,967,294 |

对“几千节点”的目标，2 B 地址已经足够。Cluster ID 不必进入每个普通 DATA Frame；Cluster 是控制面和目录/隧道语义，不是普通节点地址的一部分。

跨 Realm 时由 Gateway/Context Setup 使用 C0/C4 表达完整身份，Realm 内稳态仍使用短地址或 Label。

## 11. 功能选择与最短 Contract 算法

发送端不能让用户手工拼 Flags。应由 Endpoint Policy、Path 和 Link Contract 自动求出最短合法格式：

```text
required_features =
    endpoint_policy
  union frame_semantics
  union route_requirements
  union link_threat_policy

available_features =
    local_capability
  intersect peer_capability
  intersect path_capability

if required_features is not subset of available_features:
    fail closed
else:
    choose shortest fixed Contract satisfying required_features
```

典型选择：

| 业务需求 | 自动选择 |
| --- | --- |
| 首次向未知目标发送 | C1；必要时先 C0 Route Discovery |
| 已建立的一跳周期遥测 | C2 |
| 已安装 Label 的多跳 Best Effort/One Way 数据 | C3 |
| 已安装 Label 且要求 Latest/Reliable/Origin Replay 的数据 | C4 |
| 多跳、目标不信任中继 | C4 + E2E Auth/AEAD |
| Realtime Endpoint | C2/C4 + Flow 中的 Realtime Policy；仅必要时带短剩余预算 |
| Cluster 控制 | C0/C4/C5 的 Cluster Opcode/Payload，不污染普通 DATA |
| 大消息 | C2/C4 + Transfer Flow，不让每个 Fragment 重复完整消息上下文 |

MTU 不足时不得擅自关闭 E2E、Realtime 或可靠性；只能选择等价的更短 Contract、进行 Transfer 分片，或返回明确错误。

## 12. 安全如何减小而不失真

### 12.1 分离 Hop Context 与 E2E Context

Hop Peer Session 只证明当前一跳邻居；E2E Context 证明原始 Source Principal 与最终 Destination Principal。两者不能继续用同一个“Peer”概念替代。

多跳 Frame 的推荐处理：

1. Source 使用 E2E Context 生成 Origin Sequence 和 E2E Tag；
2. 每个 Hop 使用本地 Hop Context 验证前一跳、替换 Hop Sequence/Label 并生成新 Hop Tag；
3. 中继不解密 E2E Payload，也不读取 Cluster/Realtime 业务内容；
4. Destination 同时验证最终 Hop 和 E2E Context。

当前重构继续 Route 生产接线前，必须先把这两个安全域彻底分离。

### 12.2 Tag 长度

当前固定 16 B Tag 安全余量高，但对小包成本很大。候选策略：

- 默认 Hop Auth Tag：12 B；
- E2E Auth/AEAD Tag：12 B 或 16 B，由 Security Suite 固定；
- Authority、Config、Key、Cluster Epoch 等高价值控制：16 B；
- 8 B 仅在明确的报文速率、密钥轮换周期和可接受伪造概率经过量化审计后考虑；
- 同一 Security Context 内 Tag 长度固定，不逐帧携带长度字段。

Tag 截短不是普通“压缩”，必须单独做攻击预算，不能仅为好看而选择更短长度。

### 12.3 CRC

- Frame 已有密码 Tag 时，核心层 CRC 可省略；
- Carrier 已提供满足产品故障模型的 CRC/FCS 时，核心层 CRC 可省略；
- 明文且 Carrier 不可靠时，使用固定 CRC16 或 CRC32C Contract；
- CRC 的存在由本地 Link/Contract 决定，不由远端 Flags 临时声明。

### 12.4 Magic 和 Length

- Stream Carrier 需要 Magic/Length 来恢复边界，应放在 Stream Carrier；
- CAN/CAN-FD、Datagram、ESP-NOW 等天然有帧边界，不应重复 Magic；
- Payload Length 能从 Carrier Frame 长度与固定 Contract 头长推出时不携带；
- 聚合或 Padding 场景必须由对应 Carrier Contract 明确真实长度。

## 13. anti-ABA 与 Context 生命周期

上下文压缩绝不能靠“短 ID 循环复用”。建议统一规则：

```text
ContextKey = ParentGeneration + ContextID
```

- Hop Context 的父代际是 Peer Session Generation；
- Flow Context 的父代际是安全 Session/Binding Generation；
- Label Context 的父代际是 Route/Path Generation；
- Group Context 的父代际是 Group/Key Generation；
- Context ID 在父代际内单调分配，不回绕、不复用；
- 删除 Context 不释放历史 ID；
- ID 耗尽时轮换父代际并重新认证/建路；
- 父代际失效时，递归 Fence 所有子 Context；
- 未知、过期或代际不匹配的 Context Frame 在产生任何业务副作用前拒绝。

完整 Principal、Binding、Key 和 ACL 仍保存在节点本地固定表中。压缩只是不逐帧传输，绝不是删除身份语义。

## 14. Cluster、Realtime、Transfer 的零污染原则

### 14.1 Cluster

- Cluster OFF：零 Cluster 代码、零静态表、零普通 Frame 字段；
- Cluster ON：Cluster Epoch/Config/Membership 只出现在 Cluster 控制 Payload、目录或 Cluster Tunnel Context；
- Cluster 内普通节点数据仍使用 C2/C3/C4；
- 万级规模通过 Realm、Cluster Directory 和骨干 Route/Label 扩展，不通过给每帧添加完整 Cluster 身份实现。

### 14.2 Realtime

- Realtime NONE：零时间字段；
- 时间同步控制使用专用 Opcode/Flow；
- Timed Endpoint 的 capture time/uncertainty/deadline Envelope 只放在该业务 Payload；
- 初始 Deadline、Policy、允许误差保存在 Flow Context；
- 如果中继需要调度预算，只携带 2～3 B 量化后的 remaining budget，并受 Hop Auth；
- 不允许 Realtime 开关改变普通非实时 Frame 的格式。

### 14.3 Transfer

- 小消息直接使用 DATA；
- 大消息建立 Transfer Flow 后，Fragment 只携带短 Transfer ID、Fragment Index 和必要校验；
- Source/Destination Endpoint、Operation ID、总长度、可靠策略不在每片重复；
- ACK/SACK/Credit 是 Transfer 控制 Payload，不变成所有 Frame 的基础字段。

### 14.4 Request/Result

- ONE_WAY 不携带 Operation ID；
- REQUEST/RESULT/ERROR 才建立或引用 Operation Context；
- 高频 Request/Result 可在 Flow Setup 中绑定短 Operation Stream ID；
- Durable at-most-once 的 Journal 是 Endpoint 本地状态，不应让所有普通数据承担其字段。

## 15. 目标开销与效率

以下是设计目标，不是已冻结 Byte Layout。按 32 B 业务 Payload 计算，只用于比较方向：

| 形式 | 估算协议开销 | 32 B Payload 理论占比 |
| --- | ---: | ---: |
| v5 W0 基础明文 | 17 B | 65.3% |
| 当前 v6 A0 Hop DATA | 77 B | 29.4% |
| 当前 v6 A0 Hop + E2E DATA | 101 B | 24.1% |
| 目标 Compact Hop | 21 B | 60.4% |
| 目标 Compact Direct E2E | 29 B | 52.5% |
| 目标 Compact Routed Secure | 44 B | 42.1% |
| 受信 Carrier 的 Compact Direct | 8～12 B | 72.7%～80.0% |

建议冻结前设立硬预算：

| 场景 | 建议上限（不含 Payload/Carrier） |
| --- | ---: |
| 明文/受信 Carrier 一跳普通 DATA | 12 B |
| UCN Hop-authenticated 一跳 DATA | 24 B |
| UCN Hop + E2E 直接 DATA | 32 B |
| UCN Hop + E2E 多跳 Label DATA | 48 B |
| C0 Bootstrap/Recovery | 不追求短，但必须有固定上限 |

任何新字段若使常用路径超过预算，必须证明它为何不能放入 Context、Payload 或本地状态。

## 16. 不推荐的方案

### 16.1 单一最大头 + 可选 Flags

这正是当前问题的来源。即使扩展可选，只要基础头已装入所有代际和序号，普通帧仍然很大。

### 16.2 任意 TLV Header

TLV 看似灵活，但会增加 Type/Length 字节、解析分支、排序歧义、攻击面和 MCU 最坏时间，不符合“最简最终协议”。

### 16.3 把所有安全都交给业务节点但线上无 Tag

这只能在物理链路完全可信时成立。无线或不可信中继上，接收者无法凭本地 ACL 检测线上篡改和伪造。

### 16.4 每个功能定义独立完整协议头

会重新产生多套版本、重复路由和安全字段。功能特有内容应进入专用 Payload/Flow，而公共转发语义留在有限 Contract 中。

### 16.5 继续保留当前 Contract 1 作为兼容模式

项目尚未发布，没有必要承担双栈成本。应在新 Contract 冻结并实现后删除旧 Contract 1，而不是长期同时维护。

## 17. 建议的实施顺序

本轮只出方案。用户选定方向后，按以下顺序实施：

1. 冻结常用场景的字节预算，而不是先写字段；
2. 冻结 C0～C5 的职责，减少到最小必要集合；
3. 冻结 Realm 固定地址宽度；
4. 冻结 Hop Context 与 E2E Context 的独立安全域；
5. 冻结 Context/Label 的父代际、分配、耗尽和 Fence 规则；
6. 为每个 Contract 画出精确 bit/byte offset；
7. 形成 Golden、截断、别名、重放、错误 Context 和 anti-ABA 负向测试；
8. 单独实现 default-OFF Codec，不接生产 Runtime；
9. 对 Codec 做 Full/Lite/Nano、Release、ASan/UBSan、Analyzer 和 fuzz；
10. 接 Security Context 安装与失效；
11. 接 C1 无状态自动寻路；
12. 接 C2 Flow，再接 C3/C4 Label Route；
13. 最后接 Transfer、Realtime、Cluster，逐项验证关闭时普通 DATA 字节完全不变；
14. 删除当前 Contract 1 的源码、测试和 CMake 入口；
15. 再恢复后续生产 Route/Cluster 闭环工作。

迁移期间只允许测试内部并存，最终发布面只保留新的 v6 Contract 集合，不形成旧版本兼容承诺。

## 18. 每阶段必须做的自审

### 18.1 字节与解析

- 每个 Contract 的最短、最长和精确长度；
- 所有保留位必须为零；
- Length、Carrier Length 和 Contract 固定长度完全一致；
- Decoder 不试探、不回退、不部分写输出；
- 每个字段只拥有一个语义和一个 Owner。

### 18.2 安全

- Context ID 无法跨 Parent Generation 重放；
- Hop 与 E2E AAD 覆盖各自所有可变/不可变域；
- 中继不能提升 Traffic、延长 Deadline 或替换 E2E Principal；
- Link Contract 省略字段不能由远端自行开启；
- Tag 长度有定量攻击预算。

### 18.3 固定资源

- Context、Flow、Label、Replay Window 都有编译期上限；
- 表满不驱逐安全状态；
- 无动态内存；
- Composition 的 Contract/Feature 支持矩阵明确，Nano/Lite/Full 分别给出容量与资源门禁；
- 删除或耗尽不打开 ID 复用 ABA。

### 18.4 功能隔离

- Cluster OFF、Realtime OFF、Transfer OFF 时，普通 DATA Golden 完全相同；
- Feature Header 不被安装到生产库时，符号和静态状态为零；
- Capability 只能证明“可用”，不能单独授予 Authority；
- 无需某能力的 Endpoint 不承担该能力的线上字段。

## 19. 最终建议

建议选择：

> **方案一作为无状态保底，方案二负责高频直接流，方案三负责稳态多跳，方案四只作为经过证明的 Bearer 优化；统一由一个 v6 Version 和有限固定 Contract 分派。**

该组合最符合 UCN 的初衷：

- 只要自动寻路和传输的节点，可以使用 C1，保持接近 v5 的低开销；
- 高频节点通过 C2 把重复字段压到本地 Context；
- 多跳骨干通过 C3/C4 使用 Label，减少中继解析和转发成本；
- Cluster、Realtime、Transfer、安全等级按 Endpoint/Path/Link Policy 选择，不污染无关数据；
- 不可信无线仍保留真正不可省略的 Tag 和 anti-replay，而不是用本地校验制造虚假的安全感；
- 整体仍是单一 v6 协议，不是重新制造 v4/v5 双栈。

## 20. 需要用户确认的设计选择

开始精确 Wire 设计前，需要确认以下决策：

1. 是否接受推荐的 C0～C5 固定 Contract 家族，而不是单一最大头；
2. 默认 Realm 是否采用 A1/2 B 地址，以覆盖最多 65,534 个普通地址；
3. 是否接受 Context Setup 的一次性成本，换取 C2/C3 稳态短头；
4. 多跳稳态是否正式采用逐跳 Label，为未来 FPGA/硬件转发保留直接路径；
5. 默认不可信链路的 Hop Tag 是保持 16 B，还是先以 12 B 为候选并做定量安全审计；
6. 是否允许经过静态认证的 Carrier 省略 UCN Hop Tag/CRC/Magic/Length；
7. 是否同意删除当前 v6 Contract 1，不保留线上兼容和双栈。

在这些选择确认前，不应继续修改生产 Wire/Runtime，也不应把本文的目标字节数视为已冻结规范。
