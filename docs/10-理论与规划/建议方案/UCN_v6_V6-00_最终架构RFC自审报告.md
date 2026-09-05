# UCN v6 V6-00 最终架构 RFC 自审报告

> 审查日期：2026-09-04
> 审查对象：[UCN v6 最终协议架构与破坏性重构 RFC](UCN_v6_最终协议架构与破坏性重构_RFC.md)
> 阶段：`V6-00`
> 结论：`V6A-01～V6A-25 EXTERNAL GO；V6-00 = DONE / EXTERNAL FINAL REVIEW GO（最终架构 RFC/纯文档范围）`
> 限制：本报告只审查顶层架构、职责边界和实施依赖，不冻结 Wire 字节偏移，也不表示 v6 源码已经开始。

## 1. 自审目的

V6-00 的任务不是在旧 v5 上继续追加一个兼容分支，而是回答以下问题：

1. UCN 正式发布前，哪些当前机制应保留为架构原则；
2. 哪些过渡机制应直接删除；
3. Identity、Address、Generation、Wire、Security、Route、Transfer、Realtime 和 Cluster
   应如何共享同一组基础合同；
4. 后续任务按什么依赖顺序实施，才能避免再次出现先写 Codec、后补安全字段或持久化语义的情况；
5. 什么证据才允许把 v6 称为 UCN 1.0 候选版本。

本轮自审以当前源码为事实基线，但不要求新 RFC 与当前 v5 Wire/API 保持兼容。当前源码用于证明
“为什么要重构”，不能反过来限制最终协议设计。

## 2. 当前源码事实复核

### 2.1 Core 仍是 v5

- 当前 `UCN_PROTOCOL_VERSION` 仍为 5，见
  [`ucn_config.h`](../../../include/ucn/ucn_config.h) 和
  [`ucn_types.h`](../../../include/ucn/ucn_types.h)；
- 当前 Core 仍使用 W0～W3 Profile，并由
  [`ucn_frame.c`](../../../src/core/ucn_frame.c) 中的 descriptor 同时决定地址、Session、
  Route Epoch、Path ID、Hop 上限和长度字段宽度；
- 当前 `HELLO` 业务载荷仍为 1 B，见
  [`ucn_node.c`](../../../src/node/ucn_node.c) 中的 `UCN_HELLO_PAYLOAD_BYTES`；
- 当前 Transfer 已存在 T32、T64、T128、T256、T512、T1K、T2K、T4K、T8K 九档，
  但 Peer 能力主要依赖显式 setter，见
  [`ucn_transfer.h`](../../../include/ucn/ucn_transfer.h)；
- Cluster 当前仍同时保留生产 v3 32 B 边界和隔离的 v4 40 B Codec，见
  [`ucn_cluster_wire_v4.h`](../../../include/ucn/ucn_cluster_wire_v4.h)。

因此，RFC 中的 v6 Identity、A0～A3、Capability Record、统一 Security、RouteSet 和
Transfer v2 都是目标合同，不是当前实现状态。

### 2.2 当前成果仍有保存价值

自审没有把 v5 判定为“无价值”。下列机制应作为实现经验或测试资产迁移：

- MCU-first、固定内存、无堆默认；
- Protocol Owner、ISR/Driver 只提交事件；
- Q0～Q3 和 per-flow Latest 公平性；
- Candidate Probe/Activate/ACK、冻结证明和失败回滚；
- 多 Origin Route 所有权；
- Transfer 消息等级、并发和完成层级经验；
- Realtime uncertainty、双门禁、事件生命周期和 generation 防回退；
- Cluster Target FSM、persist-before-promise、Quorum 与 Authority Fence；
- Host Fake、属性测试、Fuzz、Sanitizer、Analyzer、规模模拟和实机报告方法。

这些内容迁移的是原则、状态机和测试意图，不是原样复制旧 Wire、公开结构体或 legacy bridge。

## 3. 顶层决策逐项自审

| 决策 | 自审结果 | 理由 |
| --- | --- | --- |
| 不兼容 v4/v5 测试协议 | PASS | 用户已明确协议尚未发布；旧版由 Git/Tag 保存比 Runtime 双栈更清晰。 |
| MCU-first、固定资源 | PASS | v6 的高级能力仍为可选模块，Core 不依赖 Linux、ROS2、动态内存或 Cluster。 |
| Identity、Address 与 Binding 分离 | PASS | Address Binding Generation 防止地址被不同 Device 复用时产生 ABA。 |
| 只压缩地址 | PASS | Session、Sequence、Route/Path/Capability/Time/Cluster Generation 均保持固定安全宽度，低档节点不能缩短防重放域。 |
| 全 Profile 解码 A0～A3 | PASS | 低资源节点可以接收高档节点控制消息；发送能力与解码能力不再混为一谈。 |
| 单一 DATA + 独立 Traffic/Guarantee/Interaction | PASS | Reliable/Latest 与 Request/Result 分域，Operation ID 和重复执行合同独立。 |
| Bootstrap、HELLO、Capability 分离 | PASS | 未绑定节点只用一跳 Bootstrap；普通 HELLO 和能力交换必须位于认证 Session 后。 |
| Capability 不等于授权 | PASS | 能解析、能承载、能使用和被 ACL 允许是四种不同事实。 |
| Security 与 Wire 同期设计 | PASS | Device Identity、Session、AAD、Hop Tag、E2E Tag、Replay 和持久化在 Codec 冻结前拥有位置。 |
| RouteSet 统一自动/手动/负载均衡 | PASS | Pinned、Standby、Weighted 与 Candidate 属于同一事务模型，不再建立旁路路由系统。 |
| 指标向量而非单一 Cost | PASS | 不同业务可按时延、抖动、丢包、带宽、拥塞、能耗和稳定性选择，同时保留确定性 Algorithm ID。 |
| Selective Repeat + Credit + Pipeline | PASS | 解决 Stop-and-Wait 多跳吞吐近似 `1/N` 的实现性退化，同时保持有界窗口。 |
| 单一 Owner/Driver 合同 | PASS | UART、CAN、USB、Wi-Fi 和各 RTOS 只替换适配层，不复制协议状态机。 |
| C99 Opaque State + caller-owned storage | PASS | 编译期 Storage 常量/union/声明宏解决文件作用域静态分配，对齐与 Manifest 启动复核。 |
| Realtime、Cluster 为正交可选模块 | PASS | 两者互不链接、互不依赖，只复用 v6 Identity、Generation、Security、Capability、Owner 和 Persistence。 |

## 4. Wire 最终基础长度算术复核

V6-03 及后续统一自审已经冻结无扩展、无 Payload、无认证 Tag、含 CRC32C 的基础长度。
按当前精确字段逐项计算：

```text
Common Prefix               9 B
Realm                       4 B
Source + Destination        2 × AddressBytes
Source + Destination Binding 8 B
Session Generation          4 B
Origin Sequence             4 B
Hop Sequence                4 B
Payload Length              2 B
CRC32C                      4 B
------------------------------------------------
Total                       39 + 2 × AddressBytes
```

因此：

| Address Class | 单地址宽度 | 最终基础帧头+CRC |
| --- | ---: | ---: |
| A0 | 1 B | 41 B |
| A1 | 2 B | 43 B |
| A2 | 3 B | 45 B |
| A3 | 4 B | 47 B |

这里的基础长度已由 v6 Golden 固定；Endpoint、Route、Path、Hop Tag、E2E Tag 和可选扩展
继续按精确合同增加长度。Common Prefix 使用 16-bit Hop Limit，Origin/Hop Sequence 分属
端到端与逐跳安全域；精确 Flag、Offset、保留位、AAD 和非法组合以
[Core Wire 精确格式 RFC](UCN_v6_Core_Wire_精确格式_RFC.md)为准。

## 5. 安全边界自审

### 5.1 已在顶层冻结

- Device Identity 不等于 Node Address，普通所有权键必须包含 Address Binding Generation；
- 未绑定节点只有不可转发的一跳 Bootstrap；唯一 JOIN 必须建立经过认证的
  Session/Address Binding；
- 可变逐跳头与不可变端到端语义分开；
- 控制面需要逐跳认证，业务可按 Endpoint 要求 E2E AEAD；
- Sequence、Session Generation 和所有所有权 Generation 不得使用地址档次缩短；
- Replay 高水位、Key/Generation 持久化必须 fail-closed；
- Capability 记录不能授予角色、Endpoint 或 Cluster Authority，V6-06 无 `ADMITTED`
  写权限；
- 激活新 Route/Path/Config/Authority 前必须先完成对应证明。

### 5.2 故意留给后续 RFC 的字段

下列项目在 V6-00 中只冻结职责，不提前冻结 ABI：

- Device Identity 的规范编码、证书或公钥指纹格式；
- Bootstrap/JOIN 密码套件、算法编号与密钥派生；
- Hop Tag 与 E2E Tag 的算法和长度；
- Header Contract 位分配、扩展存在位和已列出逻辑字段的精确 AAD 字节序列；
- Replay Window 大小和持久化批次；
- ACL record、撤销列表和 Key Rotation record 的最终布局。

这些不是遗漏，而是 V6-02、V6-03、V6-07 的显式任务。任何一项在相应外审前都不能进入
生产 Encoder。

## 6. 跨模块一致性自审

### 6.1 Generation

RFC 要求 Session、Route、Path、Capability、Time 和 Cluster 各有自己的所有权域，但共用：

- 固定 32-bit；
- 0 为无效；
- checked-next；
- 到安全阈值后轮换身份或进入故障；
- 不允许自然回绕；
- 持久化失败不能发布新权威状态。

这既避免把所有状态误塞进一个全局 Epoch，也避免各模块各自发明不同的回绕规则。

### 6.2 Deadline 与时间

- 普通消息不携带时间元数据；
- Timed Endpoint 才启用 Realtime Envelope；
- Protocol Owner 的调度 Deadline 与业务 Domain Time Deadline 是不同概念；
- 动态 Route 不得伪装成具备可信非对称误差界的同步 Path；
- Realtime 继续复用已经外审的软件模型，但生产接线必须重新绑定 v6 Security 和 Capability。

### 6.3 路由与传输

- RouteSet 决定候选路径集合与选择策略；
- Path Contract 冻结一条已安装路径的逐跳能力交集；
- Transfer 根据 Path Frame MTU、精确 Fragment Budget、消息等级、窗口和 Credit 决定
  分片流水；
- QoS 只决定调度次序和资源份额，不能篡改 Delivery Guarantee 或 Interaction Role；
- 路径切换必须使旧 Path/Capability/Realtime 证明失效，不能继承可变 Candidate 的旧证明。

### 6.4 Cluster

- Core 即使完全不编译 Cluster 或 Realtime 也必须可以寻址、路由和传输；
- Cluster 不再保留 v3/v4 双栈或 Mixed Version；
- Cluster Target FSM 复用 v6 Device Identity、Session、Wire、Security、Transfer、Deadline、
  Generation、Owner 和 Persistence；
- Quorum、Persistence 与 Security 三者同时成立才可授予 Authority。
- Cluster 与 Realtime 只能共享基础设施，二者不得互相链接或形成实施顺序依赖。

## 7. 删除清单复核

RFC 已明确删除或替换以下过渡设计：

1. v4/v5 Runtime 双解码和 legacy bridge；
2. Cluster v3/v4 双栈；
3. W0～W3 同时缩短地址与安全字段；
4. DATA_Q0/DATA_Q1 类型复制；
5. HELLO 只靠单字节长期承载全部协商；
6. 外部直接分配并修改透明大状态结构；
7. 多个配置头各自提供默认值且优先级不透明；
8. Capability、可解析能力和授权混用；
9. Stop-and-Wait 作为唯一大消息模式；
10. 各模块自行实现 serial、deadline、persistence 和 callback gate；
11. 用 Host 测试替代 Flash、密码库、无线和硬件时间戳实测。

该清单与“不承担兼容”的顶层决策一致，没有一边声明删除、一边在任务中要求 Runtime
fallback。V6-01 还必须建立逐文件/符号 Compatibility Removal Manifest；V6-15 用
`rg/nm/CMake` denylist 证明旧可编译源码、符号、Option 和 Target 已归零。旧版本明确拒绝、
`struct_size/api_version` 和旧输入负向测试属于安全门禁，不得误删。

## 8. 任务依赖复核

任务表已经拆为 V6-00～V6-15。关键依赖满足：

```text
V6-00 Architecture RFC
  ↓
V6-01 v5 snapshot + removal manifest + clean v6 baseline
  ↓
V6-02 Identity / Bootstrap / Address Binding / Generation
  ↓
V6-03 Wire → V6-04 Message → V6-05 C99 Storage/Owner
  ↓
V6-07 Security + unique JOIN
  ↓
V6-06 authenticated HELLO / Capability / Path Budget
  ↓
V6-08 RouteSet → V6-09 QoS → V6-10 Transfer
  ├─ V6-11 Realtime integration
  └─ V6-12 Cluster Target integration
       (parallel optional features; no link dependency)
  ↓
V6-13 reference product → V6-14 verification → V6-15 RC
```

特别确认：

- V6-03 只允许隔离、default-OFF Codec；不会在 Identity/Security 字段未定义或 V6-07
  未获外审 GO 前接入生产 RX/TX/Encoder；
- 不会在 Capability/Path Frame MTU/Payload Budget 未完成前让 Transfer 占用重组槽；
- V6-07 是唯一 JOIN/ADMITTED Owner，V6-06 不会形成无认证 Join；
- 不会在 RouteSet 与 QoS 未定义前宣称负载均衡完成；
- 不会在 M05 顶层 HOLD 尚未解除时把旧 Cluster v4 直接接入生产；
- 不会在 Host 全绿后直接把真实 Flash、无线、DMA 或硬件时间戳标为通过。

## 9. 机械检查

本轮自审执行了以下文档门禁：

| 检查 | 结果 |
| --- | --- |
| 外部终审内容基线 | 1632 行；SHA256=`F79FC9B310CE6FFD5E662F3A7A986A1E61C54F1CB46B8AD7EA5A4405D04E1680` |
| 状态同步后当前 RFC | 1633 行；SHA256=`14E505A06E22A11F52DF7E037A408877586F3E8328FA23272EBD4074282163C7`；仅修改顶端状态和末尾阶段说明，不改变受审规范合同 |
| RFC 一级编号章节 | 23，编号 1～23 连续 |
| 未决占位标记 | 0 |
| V6 任务数 | 16，V6-00～V6-15 连续 |
| 五轮外审整改项 | V6A-01～V6A-25 共 25 项，全部有正文和任务映射并已外审 GO；最终终审 P0/P1/P2 均为 0 |
| Markdown / 本地链接 | 509 个 Markdown；扫描 1562 条本地链接，0 坏链 |
| Cluster 文档合同脚本 | PASS，`phases=22 wire_types=33 tasks=12` |
| 工作树/临时索引 diff check | `git diff --check` 与隔离临时索引 `git diff --cached --check` 均无空白错误；后者明确包含未跟踪 RFC/自审报告，仅有既有换行格式提示 |

本轮没有运行 CTest。原因是 V6-00 只新增/修改规划文档，没有改变源码、CMake、Wire、API、
ABI 或测试行为；当前工作区中原有 V5-64 代码改动及其既有验证证据不被本报告重新冒充为
v6 测试结果。

## 10. 首轮外部审计 V6A-01～V6A-10 整改映射

| ID | 结论 | RFC 整改 |
| --- | --- | --- |
| V6A-01 | 已整改 | Delivery 拆为 Guarantee 与 Interaction Role；新增固定 Operation ID、重复 Payload 冲突和 durable at-most-once 合同。 |
| V6A-02 | 已整改 | 新增全零 UNBOUND、一跳 Bootstrap Opcode、Identity Digest、64-bit txid、本地 Peer 区分和禁止转发规则。 |
| V6A-03 | 已整改 | Source/Destination Address Binding Generation 进入 Wire、AAD、Route Key；增加各 Generation Owner/父域/持久化/重置表。 |
| V6A-04 | 已整改 | canonical AAD 纳入 Version/Class、不可变 Flags、Header Contract、Binding、Route/Path、Interaction/Operation；Principal ACL 强制 E2E Auth-only/AEAD。 |
| V6A-05 | 已整改 | C99 使用编译期 Storage Bytes/Alignment、生成 union/声明宏；运行期 required 只核对，init 前检查容量/对齐/Manifest/Layout。 |
| V6A-06 | 已整改 | V6-07 成为唯一 JOIN/ADMITTED Owner并先实施；V6-06 只处理认证 Session 后的 Discovery/Capability。 |
| V6A-07 | 已整改 | 区分 Carrier、Link Frame、Path Frame MTU；Payload/Fragment Budget 按精确 Contract、Suite、Class 和扩展 checked-subtract。 |
| V6A-08 | 已整改 | 默认 Deadline 只用于源/目标；中继只使用可选 Hop Scheduling Budget，预算只减不增且不能放宽 E2E 门禁。 |
| V6A-09 | 已整改 | V6-01 增加逐文件/符号 Removal Manifest；V6-15 增加 `rg/nm/CMake` denylist，历史源码只留 Git。 |
| V6A-10 | 已整改 | V6-11/V6-12 改为共同基座后的并行 Optional Feature，明确互不链接、互不依赖。 |

## 11. 整改后外审重点

建议外部评审优先挑战以下问题：

1. Bootstrap 的 Link-local key、Identity proof 和 Address Commit 是否仍存在未认证写入路径；
2. Address Binding Generation 的 Authority high-water 是否足以关闭地址复用 ABA；
3. 候选公共前缀是否能无歧义编码 Guarantee/Interaction/Hop Budget 和完整 canonical AAD；
4. Operation ID、Endpoint 幂等类别和 durable at-most-once 是否存在“可靠即恰好一次”的误解；
5. 唯一 JOIN 与认证 Capability 的任务边界是否仍有循环依赖；
6. RouteSet、Path Contract、Flow pinning 和负载均衡是否共享一个原子激活模型；
7. Transfer Selective Repeat、逐跳 Credit 和 QoS 是否存在循环依赖；
8. Realtime/Cluster 是否既复用基础设施又保持互不依赖；
9. V6-02～V6-15 的先后顺序是否允许任何未审字段提前进入生产 Encoder；
10. UCN 1.0 完成定义是否仍含“默认关闭但宣称完成”或“Host 替代实机”的漏洞。

## 12. 第二轮外部审计 V6A-11～V6A-18 整改映射（当前版复核确认）

| ID | 结论 | RFC/任务整改与自审要点 |
| --- | --- | --- |
| V6A-11 | 当前版复核确认 | Bootstrap 改为 JOINING_DEVICE 与 REALM_ADDRESS_AUTHORITY 双向认证；完整 transcript 绑定双方 Principal、Authority Generation/Fence、双 nonce、txid、地址租约和 Suite/Key。新增 `ADDRESS_BOUND_NO_PEER` 的逐 Peer 重认证；普通 HELLO 默认认证单播，Group HELLO 只能提示发现。 |
| V6A-12 | 当前版复核确认 | 冻结单一逻辑 Realm Address Authority。动态分配要求 durable Lease/Fence、allocation high-water 和 quorum；少数分区不得创建/续期 Binding，Authority 换主必须先失效旧 Fence并继承高水位。SELF_PROPOSED 只产生候选，STATIC 也须签名预留/登记。 |
| V6A-13 | 当前版复核确认 | 固定 Hop/E2E Security Context 的 Suite、Key ID、Key Generation 与顺序；Protocol Opcode 位于固定 Protocol Context。冻结 Hop/E2E selector、Tag 覆盖范围、current/previous Key grace 和禁止试 Key/隐式默认规则。 |
| V6A-14 | 当前版复核确认 | Durable Journal 增加 `PREPARED/EXECUTING/COMMITTED_RESULT/IN_DOUBT/TOMBSTONED`。只有外部副作用与 Journal 原子或可认证查询对账时才自动恢复；其余掉电窗口进入 IN_DOUBT、不得重试。表满、retention、ACK、Tombstone 和 GC 顺序已冻结。 |
| V6A-15 | 当前版复核确认 | Hop Budget 只能在原 Traffic Class 和已认证 per-peer/source/flow 配额内排序；不能升级 Class 或侵占 Q0/其他 Flow。冻结 `0<remaining<=initial<=policy_max`、逐跳扣减和耗尽即 DROP，禁止删除扩展后继续普通传播。 |
| V6A-16 | 当前版复核确认 | V6-03 只产出隔离 default-OFF Codec/Golden/Negative；生产 Node/Adapter RX/TX/Encoder 必须等待 V6-07 Security/JOIN 外审 GO。RFC、任务表和路线图一致。 |
| V6A-17 | 当前版复核确认 | 核实任务表“V5-64 A06 外审 GO”与 OP-460“等待复审”冲突；仓库无独立 A06 签字记录，因此 V5-64 恢复 AUDIT HOLD，V6-01 增加该前置门。没有根据聊天摘要补造外审证据。 |
| V6A-18 | 当前版复核确认 | Opaque 只阻止合法代码依赖私有布局，不物理阻止同地址空间 memset/越界写；明确由 magic/Schema/Layout/Fence/canary 检测并失败关闭，强隔离需 MPU/TrustZone/进程。 |

### 12.1 交叉反例自审

1. 未认证 Authority 即使能发送结构合法 `ADDRESS_OFFER`，也因 Authority proof/Fence/quorum
   和 transcript 不完整而在持久化前拒绝；已绑定节点换 Link 不会被迫退回 UNBOUND。
2. 两个网络分区不能仅比较本地 Authority Generation 选胜；无 quorum/Fence 的分区零新增、
   零续期，避免相同 Binding tuple 被合法重复签发。
3. Frame 明确携带 Suite/Key Generation/Opcode；修改 selector、Opcode 或 Header Contract
   会破坏 Hop Tag，修改 E2E selector/Opcode 会同时破坏 canonical E2E AAD。
4. 外部执行器动作后、Result 持久化前掉电会进入 IN_DOUBT；不会重复动作，也不会虚构可
   重放结果。Journal 表满在副作用前拒绝，未决槽不能被时间 GC。
5. 恶意 Peer 持续发送极小 remaining budget 只能消耗自身 Flow/Class 配额；它不能进入
   Q0 预留，耗尽后必须 DROP，不能删除扩展后回到普通调度继续传播。
6. 完成 V6-03 仍无法把 Codec 链接进生产 Node；V6-07 未获外审 GO 时生产发送门持续关闭。
7. V5-64 A06 缺少独立外审记录时，V6-01 的快照条件不成立；任务表和路线图不再写成已 GO。
8. 应用非法清零 Storage 会被视为内存损坏并失败关闭，但文档不再声称 Opaque 能物理阻止。

### 12.2 第二轮问题对原整改的闭合关系

- V6A-11/12 完成了 V6A-02/03 尚缺的互认证、已绑定重认证与 Address Authority 分区合同；
- V6A-13 完成了 V6A-04 的 Suite/Key/Opcode Wire 归属；
- V6A-14 限定 V6A-01 中 durable at-most-once 的真实承诺范围；
- V6A-15 限定 V6A-08 的 Hop Budget 只能是有配额的同级调度提示；
- V6A-16 修复任务依赖，避免先生产 Wire、后补安全；
- V6A-17/18 分别修复发布台账和 Opaque 威胁模型表述。

## 13. 第三轮交叉审计 V6A-19～V6A-22 整改映射

| ID | 结论 | RFC/任务整改与自审要点 |
| --- | --- | --- |
| V6A-19 | 外部复审 GO | 删除“各持有者收到 Final Commit 后取得完整本地租期”的歧义。Authority/Binding Lease 改为验证者先锁存 Challenge 本地起点；quorum signer 各自扣除本地误差并由聚合器取最小剩余上界，接收端建立保守半开 Deadline；不存在共同绝对时间的虚假假设。验证端完整误差扣减由 V6A-23 补齐。 |
| V6A-20 | 外部复审 GO | Bootstrap 与无 Session Reauth 在分配 pending、验公钥、访问 Provider 或放大响应前，必须先通过 boot-scoped 无状态 Cookie 和认证前预算。冻结全局/per-Link/per-discriminator 固定槽、3 s checked 绝对 timeout、`now==deadline`、无驱逐/无续时、响应放大和每次 Owner 昂贵操作上限。 |
| V6A-21 | 原问题外部复审 GO；新增 V6A-24 | 为 Group HELLO 新增与 Peer Hop 互斥的 Group Security Context、完整 GroupKeySelector、Group Tag/Replay 和 `LINK_LOCAL_GROUP` 目的地址合同。共享 Group Key 只证明组 Key 持有，不能证明 claimed Principal；唯一副作用是受固定槽/Token/cooldown 约束的 Reauth 提示。Generation 所有权表由 V6A-24 补齐。 |
| V6A-22 | 外部复审 GO | ACL Key 增加 canonical `protocol_opcode`。非 DATA 以精确 Opcode 授权，DATA 固定为 0；Bootstrap 和 Group discovery 使用独立准入策略，禁止以 Frame Type 通配获得整个 CONTROL 类权限。 |

### 13.1 交叉反例自审

1. Device 在本地 t=0 发出租约 Challenge，Final Commit 在 t=900 ms 才到；若 proof 只承诺
   1000 ms，设备最多再使用不足 100 ms，而不是重新获得 1000 ms。重放相同 Final Commit
   不改起点，`now==deadline` 已过期。
2. 攻击者轮换伪造 Identity Digest/txid 发送首包，只能消耗 per-Link 认证前 Token；未回显
   有效 Cookie 前不分配 pending、不验公钥、不访问 Provider。合法 Cookie 洪泛也受固定槽和
   Owner crypto budget 限制，满载不驱逐已认证事务。
3. Group HELLO 同时携带 Peer Hop Context、使用普通单播目的地址、旧 Group Key、坏 Replay
   Sequence 或试图续期 Neighbor/Capability 时必须零写拒绝；合法帧最多创建有界 Reauth 提示。
4. 同一个已授权 `Frame Type=CONTROL` 帧把 `protocol_opcode` 从只读诊断改成配置写入时，
   Hop/E2E Tag 与精确 ACL 至少一道必然拒绝，不能继承 Frame Type 级授权。
5. Lease freshness proof、Cookie pending、Group Replay 和 ACL lookup 都必须是固定内存、
   checked arithmetic、无 Callback 重入和无失败写回；这些合同分别进入 V6-02/03/06/07/13
   的实现验收，不能只留在顶层说明中。

### 13.2 与前两轮整改的闭合关系

- V6A-19 完成 V6A-12 的 Address Authority Lease/Fence 合同：安全排他仍由 Fence/quorum，
  缓存新鲜度由挑战相对的保守本地 Deadline，二者不混用；
- V6A-20 完成 V6A-11 的 Bootstrap/Reauth 状态机在未认证阶段的资源安全；
- V6A-21 完成 V6A-13 中只覆盖 Peer Hop selector 的缺口，并保持 Group HELLO 不提升权限；
- V6A-22 把 V6A-13 已认证的 Protocol Opcode 真正带入 V6A-04 的 Principal ACL 决策键。

## 14. 第四轮交叉审计 V6A-23～V6A-24 整改映射

| ID | 结论 | RFC/任务整改与自审要点 |
| --- | --- | --- |
| V6A-23 | 外部复审 GO | Lease verifier 不再只扣慢钟裕量。统一公式额外扣除一整个本地 Timer Resolution 和 Challenge/验证两次读取误差；Resolution 必须已知且非零，read uncertainty 必须已知，所有运算 checked，安全时长为零或下溢均失败关闭。 |
| V6A-24 | 外部复审 GO | `Group Policy Generation` 与 `Group Key Generation` 已进入统一 Generation 表，唯一 Owner、父域、persist-before-publish/use、high-water、跨 Authority/Key Owner 换主连续性、合法重置和耗尽语义成立；固定容量表示已由 V6A-25 收口。 |

### 14.1 区分性反例自审

1. Timer Resolution 为 `10,000 us`、proof 剩余 `15,000 us`、慢钟和已证明的读数误差均为
   0 时，安全时长最多 `5,000 us`；真实经过 `19,000 us` 而 raw now 只前进 `10,000 us`
   必须拒绝。旧公式会错误接受，新公式会在半开 Deadline 前失败关闭。
2. Timer Resolution Unknown/0、read uncertainty Unknown、两倍读取误差溢出、扣减至 0 或
   下溢时，不能建立本地 Lease Deadline，也不能等 Final Commit 到达后重新计时。
3. 动态 Group Policy Authority 换主、设备重启、配置恢复或分区合并时，新的 Owner 必须继承
   `{realm,group_id}` high-water；无法证明连续性时停止新 Policy/Key，而不是从 Generation 1
   开始。静态签名模式和动态模式由 Realm Manifest 互斥选择，不能在故障时互相降级。
4. Group Key Generation 只有在父 `group_generation` 或 `group_key_id` 不可逆变化后才能从
   1 开始；同父域内 Key Owner 更换不构成重置。达到冻结阈值时只能推进父域或 Fault，不能
   回绕、复活旧 selector 或重新打开旧 Replay Window。

### 14.2 与前三轮整改的闭合关系

- V6A-23 补齐 V6A-19 验证端新鲜度公式，使 Producer 和 verifier 都覆盖本地 Timer 量化误差；
- V6A-24 补齐 V6A-21 的 Group selector 代际所有权；固定资源表示已由 V6A-25 收口；
- V6A-20/V6A-22 的外部 GO 保持不变，本轮没有改写其 Cookie/Opcode ACL 合同。

## 15. 第五轮交叉审计 V6A-25 整改映射

| ID | 结论 | RFC/任务整改与自审要点 |
| --- | --- | --- |
| V6A-25 | 外部复审 GO | 删除不可判定或无界的“已用 Group/Key ID 集合”。动态 Group ID 改为 Realm 单个持久化分配高水位并只做 checked-next；静态 Group 使用 Manifest 固定槽及永久 `RETIRED`；Group Key 使用每 Group 固定 Key 槽，日常轮换只推进 Key Generation。所有数组容量编译期确定，摘要仅校验完整性。 |

### 15.1 区分性反例自审

1. 动态 Group 已依次分配 1、2、3，删除 2 后下一次创建只能取得 4；不能扫描空洞得到 2。
   高水位必须在发布 Policy/Key/HELLO 前持久化，撕裂或 reload 回退直接 Fault。
2. `active_groups[]` 已满时，创建零写拒绝，不驱逐旧 Group；删除只释放活动运行槽，不降低
   `dynamic_group_id_high_water`。到达 ID 冻结阈值后禁止创建，现有合法 Group 是否继续由
   独立租约/撤销策略决定。
3. 静态 Manifest 槽从 `ACTIVE` 删除后只能进入 `RETIRED`。重启、重刷配置或恢复旧 Manifest
   都不能回到 `NEVER_ACTIVATED`；固定槽耗尽后拒绝创建，不能扩大数组或丢弃历史槽。
4. 常规换钥保持 `group_key_id`，只执行 Key Generation checked-next。只有同一 Policy 中从未
   激活的固定 Key 槽可使用新 ID；退休槽不复活。需要全新槽集合时先提交 checked-next
   Group Policy Generation，使完整 selector 进入新父域。
5. 固定摘要发生碰撞或无法回答成员查询时，不得用它判断 ID 是否用过；协议唯一准入依据是
   动态分配高水位或固定槽状态。删除整个 Group 后，因为动态 ID 不复用或静态槽永久退休，
   可以释放该 Group 的 Key 槽记录而不重新开放旧 selector。

### 15.2 与 V6A-24 和顶层原则的闭合关系

- V6A-24 冻结“谁拥有、在哪个父域连续、何时持久化、何时允许重置”；V6A-25 冻结“用什么
  固定容量状态实现”，两者合并后才构成完整防 ABA 合同；
- 动态模式只增加一个 Realm 标量高水位和固定活动表，静态/Key 模式使用 Manifest 固定槽，
  不需要随运行时间增长的集合、堆或 Host 服务；
- 删除与垃圾回收不会释放身份命名空间；容量耗尽显式 Fault，符合 MCU-first 失败关闭边界。

## 16. 第五轮整改后的自审结论

V6-00 在第五轮整改后达到“可送第五轮外部复审”的文档范围：

- 顶层不兼容决策明确；
- 当前事实与目标状态分开；
- 保留、删除和重构对象完整；
- V6A-01～V6A-23 对应原问题保持外审 GO；V6A-24/V6A-25 已逐项映射到 RFC 和任务表，
  并用固定容量状态替代无界历史集合；
- Identity/Bootstrap/Binding/Wire/Security/Capability/Route/Transfer/Realtime/Cluster 依赖闭合；
- 具体 Wire/Security ABI 没有越权提前冻结；
- 后续 16 项任务均有验收和失败边界；
- 没有修改现有 Encoder/Decoder 或创建兼容层。

第五轮自审当时的状态为“等待外审”；最终外部终审已在受审内容基线
`F79FC9B310CE6FFD5E662F3A7A986A1E61C54F1CB46B8AD7EA5A4405D04E1680` 上确认
P0/P1/P2 均为 0，因此当前状态更新为：

```text
V6-00 = DONE / EXTERNAL FINAL REVIEW GO / RFC-AND-DOCS SCOPE ONLY
V6-01 = BLOCKED BY V5-64 A06 SIGN-OFF + USER AUTHORIZATION
V6-02..V6-15 = NOT STARTED
```

## 17. 外部终审签字与范围

外部终审确认 V6A-25 的固定资源整改通过，并据此将 V6A-24、V6A-25 和 V6-00 一并签署
GO。签字绑定的规范内容基线是 1632 行、SHA256
`F79FC9B310CE6FFD5E662F3A7A986A1E61C54F1CB46B8AD7EA5A4405D04E1680`。终审后只同步了
RFC 顶端状态与末尾阶段说明，因此当前文件变为 1633 行、SHA256
`14E505A06E22A11F52DF7E037A408877586F3E8328FA23272EBD4074282163C7`；规范字段、算法、
失败关闭合同和任务依赖没有改变。

本次签字严格限于最终架构 RFC/纯文档范围，不代表以下事项已经完成：

- v6 源码、Wire Codec、生产 RX/TX、Security/JOIN 或公共 ABI；
- 真实 Flash、掉电恢复、密码实现、ESP32/其他 MCU 或多 Bearer 实测；
- V6-01 的提交、v5 实验快照/Tag、分支调整、Compatibility Removal Manifest 或 v6 基线。

V6-01 不再受 V6-00 阻塞，但仍必须先取得 V5-64 A06 可追溯独立外审记录，并获得用户对
提交、建立 v5 快照/Tag 和 v6 基线的明确授权。在这两项满足前，不开始 V6-01，也不绕过
它直接进入 V6-02/V6-03 或生产接线。
