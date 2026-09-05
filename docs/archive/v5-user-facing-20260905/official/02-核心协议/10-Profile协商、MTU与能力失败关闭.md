# Profile 协商、MTU 与能力失败关闭

> 文档级别：`NORMATIVE`
> 实现状态：`CURRENT`
> 事实源：`include/ucn/ucn_frame.h`、`include/ucn/ucn_config.h`、Node/Path/Standard Adapter profile logic
> 最近核对：`a093862`，2026-08-26

## 1. 为什么需要 Profile

UCN 既要运行在 RAM 很小、地址范围很小的 MCU 上，也要支持较大 Node ID、更多跳数和更宽路径字段。若所有设备都固定使用最大 Header，小节点会为永远用不到的字段付出 RAM、带宽和编码成本；若只使用最小 Header，大网络又无法表示地址和路径。

因此项目把“资源与功能等级”拆成三个相互独立的维度，而不是一个含糊的“大/中/小档”。

## 2. 三类 Profile 不可混淆

| 概念 | 例子 | 主要决定 | 不决定什么 |
| --- | --- | --- | --- |
| Build Profile | Nano/Lite/Full | 编译哪些表、状态机、API 实现和 RAM 上限 | 单帧地址字段一定多宽 |
| Wire Profile | W0/W1/W2/W3 | Frame Header 字段宽度、可表示地址、Hop、Path/Cost 范围 | MCU 是否编译了 Path/Policy 功能 |
| Link/Bearer preset | UART 3M、CAN 500K、Wi-Fi | 物理/逻辑 MTU、基础 Cost、活性参数和驱动能力 | 端到端业务 ABI |

例如 Nano 节点仍可以解码合法 W3 普通 Frame，但如果某帧要求 Nano 未编译的 Path Policy 控制能力，它会明确拒绝。反过来，Full 节点通过一条只允许 W0 的 Link 发送时，也不能强行写 W3。

## 3. Wire Profile 的表示能力

| Profile | 地址/主要宽字段 | 最大 Node ID | 最大 Hop | 基础 Header |
| --- | ---: | ---: | ---: | ---: |
| W0 | 1 B | 254 | 4 | 17 B |
| W1 | 2 B | 65534 | 16 | 21 B |
| W2 | 3 B | 16777214 | 64 | 26 B |
| W3 | 4 B | 4294967294 | 254 | 30 B |

表中的最大值是 Wire 可表达边界，不是建议产品一定使用的网络规模。安全 Tag、Flags 和具体可选字段还会影响最终 Header/Frame 长度。

## 4. 发送能力如何确定

一个节点要发送某帧，需要同时求以下上限的交集：

1. 本地编译和配置允许发送的最大 Wire Profile；
2. 出口 Link 的本地发送限制；
3. HELLO/邻居状态中对端声明的最大接收 Profile；
4. 已安装 Path 的端到端 capability；
5. 当前逻辑 MTU；
6. 该 Frame 各字段实际数值所需的最小 Profile。

算法不是“永远用配置的最大档”，而是调用 profile-aware 选择逻辑，在共同上限内选能完整表示该帧且放得进 MTU 的最小官方档。这样小地址、小 Payload 的常见帧自动减少 Header 开销。

## 5. 最小档选择的完整条件

候选 Profile 必须同时满足：

- Network ID、Source、Destination、Session、Sequence 可表示；
- Route Epoch、Path ID、Route Cost 和 Payload Length 可表示；
- 请求 Hop Limit 不超过该档；
- Frame Flags 在该版本合法；
- 受保护帧为固定 E2E Tag 预留空间；
- 编码总长不超过 `UCN_MAX_FRAME_BYTES` 和当前逻辑 MTU；
- 对端与路径声明可接收；
- 所需功能没有被 Build Profile 裁掉。

找不到候选时必须返回明确错误。禁止通过截断 Node ID、缩小 Sequence、删除安全 Flag、改成明文或偷偷换业务 Payload 来“兼容”。

## 6. MTU 有哪几层

| 层级 | 含义 |
| --- | --- |
| Core 最大 Frame | UCN 逻辑帧编译上限，当前默认 256 B |
| Standard Adapter logical MTU | 本适配器承诺一次可交付的完整 UCN Frame 上限 |
| 物理 Carrier MTU | 一次 UART chunk、CAN/CAN-FD frame、Wi-Fi datagram 或 USB transfer 的大小 |
| Path capability MTU | 多跳路径所有 hop 共同可支持的端到端上限 |

物理 Carrier 可以比逻辑 Frame 小：Classic CAN 需要把一个 UCN Frame 拆成多个 Carrier 后重组。也可以比逻辑 Frame 大：CAN-FD DLC round-up 可能产生 padding，接收端要根据 UCN Header 取精确 encoded size，并验证 padding 合同。

## 7. 异构多跳路径示例

路径 A→B 使用 UART，B→C 使用 Classic CAN，C→D 使用 Wi-Fi：

- 普通 Frame 必须适合三段共同逻辑能力；
- B 的 CAN Adapter 可做定义明确的 Carrier 分片/重组，但不能改变 UCN Source、Destination、Session 或安全语义；
- 如果已安装 Path 声称支持 W2/200 B，而 B→C 实际只允许更小 logical MTU，Path 能力已经失真；
- 检测到能力不满足时应撤销本地转发条目并传播确定性 RERR，而不是静默丢帧。

大消息 Transfer 可以在 Core Frame 之上做端到端分片，但每个 Fragment 仍必须分别满足当前路径的 Profile/MTU。

## 8. HELLO 协商的边界

HELLO 只向直连邻居声明接收能力和邻居活性，不是全网一次性协商。多跳路径能力由路由/Path 安装过程逐跳收敛。

对端声明最大 W3 不表示：

- 它支持所有控制类型；
- 它编译了 Full Profile；
- 它授权本节点使用诊断或管理功能；
- 中间每一跳也支持 W3；
- 当前 Link 的 MTU 足以承载最大 W3 Frame。

能力、实现、权限和当前可用状态必须分别验证。

## 9. Build Profile 的 API 行为

公共头文件可为多个 Build Profile 提供一致 API 表面，但 Lite/Nano 对裁掉的能力通过 stub 返回 `UCN_ERR_CONFIG` 或对应失败，而不是链接时随机缺符号。这样应用可以共用集成代码，同时在运行/初始化时明确知道某构建没有该功能。

这不等于所有 Profile RAM 占用相同。编译期表容量和被链接模块仍然决定实际对象大小，产品需要查看对应 Profile 的资源报告和 Map 文件。

## 10. 能力失败为什么必须 fail-closed

协议协商最危险的错误不是“无法通信”，而是两端对同一字节产生不同解释。UCN 因此要求：

- 未知 Wire Profile：在完整解码前拒绝；
- 长度与 Profile 不匹配：拒绝，不尝试猜另一个格式；
- 必需 capability 缺失：拒绝对应功能；
- 对端 offer 非法：返回参数/版本错误，不降级成 legacy；
- 安全能力不匹配：拒绝，不自动切明文；
- Path 能力过期：撤销/Fence 后重新发现。

兼容模式只能由产品显式开启，并且必须定义能放行的精确旧版本、角色和 capability；不能使用“尽量解析”。

## 11. 配置示例

一个小型 8 节点产品可以：

- Build Profile 选择 Lite；
- 默认发送最大档限制为 W1；
- 接收仍允许 W0～W3 普通帧；
- UART Link logical MTU 设为 256 B；
- Classic CAN Link 由 Carrier Adapter 承载同样逻辑 MTU；
- 路径选择自动使用 W0，只在 Node ID/字段超过 W0 时升到 W1；
- 收到需要 Full Policy/Cluster 实验能力的控制帧时明确拒绝。

这样既保持和更高档普通节点通信，又不会假装本机具备未编译功能。

## 12. 验证清单

- [ ] 每个 W0～W3 的边界值和越界值均有编码/解码测试；
- [ ] 选择器确实选择满足条件的最小档；
- [ ] 对端最大接收档、Link 档和 Path 档取共同上限；
- [ ] 受保护帧计算 MTU 时包含 Tag；
- [ ] 版本/长度边界严格分派，无猜测解析；
- [ ] CAN-FD padding 与 Classic CAN 重组交付精确 Frame 字节；
- [ ] Lite/Nano 公共 API 符号完整，裁剪能力明确失败；
- [ ] 异构路径能力失效会撤销并产生可诊断错误；
- [ ] 安全或 capability 不匹配不会静默降级；
- [ ] 产品资源报告与实际链接模块/Profile 一致。
