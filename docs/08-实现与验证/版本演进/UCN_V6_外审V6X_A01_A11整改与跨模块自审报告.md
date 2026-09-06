# UCN V6 外审 V6X-A01～A11 整改与跨模块自审报告

> `SUPERSEDED IMPLEMENTATION SNAPSHOT`：本文中的 V6X-A04 48 B Realtime Sample 是
> 当时整改证据，已由 V6X-S28 的真实 T1/T2/T3/T4 事件模型替代；请勿把旧 API 或 Payload
> 当作当前发布合同。
>
> 本文后部“没有仍开放的软件 P0/P1”也是当时的自审结论，已被后续外审 AUD-01～05 取代。
> 当前状态见[最新整改报告](../../09-审计与整改/UCN_V6_外审AUD_01_05整改与自审报告_2026-09-06.md)。

> 状态：`REMEDIATION IMPLEMENTED / SELF-REVIEW PASS / EXTERNAL RE-REVIEW PENDING`
>
> 范围：v6 软件实现、Host 模型、构建与静态门禁。本文不把 Host Fake Store、Host 并发、
> 软件 Tag Provider 或规模模拟写成真实 Flash、掉电、生产密码、MCU ISR/DMA、无线链路、
> Realtime 精度或 24 h 长稳证明。

## 1. 为什么需要本轮整改

V6-01～V6-15 首轮连续实现建立了单一 v6 发布面，但外部审计进一步从跨模块组合角度发现
7 项 P0 与 4 项 P1。问题不在单个模块能否通过正向测试，而在以下边界是否真正闭环：

1. 持久化状态能否在重启、回滚和撕裂写后证明没有回退；
2. E2E 原始发送序号与每跳防重放序号是否拥有独立所有者；
3. 中继能否验证入站逐跳证明并为下一跳重新认证，而不改变端到端密文；
4. Realtime、Cluster 等上层语义是否来自已经认证的精确 Payload；
5. 全局 Operation ID、Capability 租约和固定预算是否会被局部实现误解释；
6. 高频接收是否会把 Flash 写放大到每帧一次。

本轮按外审编号逐项整改，每项先加入能够复现旧行为的负向测试，再修改实现，随后执行定向
自审。完成全部项目后，新增跨模块五节点链路并重新执行全软件矩阵。

## 2. Wire 与 Security 的序号所有权

### 2.1 原始序号与逐跳序号分离

旧结构只有一个 Packet Sequence，同时被 E2E 和 Hop Replay 使用。一个源节点同时向多个
最终目标发送时，E2E Session 与下一跳 Peer Session 会争用同一序列域；中继也无法在保留
端到端身份的同时为下一跳生成新鲜序号。

当前帧固定为两个字段：

- `origin_sequence`：由原始 E2E/Group 发送安全上下文分配，写入 canonical AAD，中继保持
  不变；
- `hop_sequence`：由当前发送节点到下一跳的 Peer Session 分配，每跳重新生成，只参与该
  Link/Peer 的 Hop Tag 与 Replay。

因此 A0～A3 的基础开销先由原草案的 `36/38/40/42 B` 调整为
`40/42/44/46 B`。后续统一架构自审为消除 16-bit 累计跳数与 8-bit
Wire Hop Limit 之间的不可达路径，再升级为当前 `41/43/45/47 B`。这是 v6
尚未发布阶段的破坏性修正，不提供任何旧 v6 草案解码。

### 2.2 中继验证与重新认证

`ucn_v6_security_relay_frame()` 现在执行完整的 Security 所有权路径：

1. 从原始编码字节严格解码；
2. 验证入站 Link/Peer selector、Hop Tag 和 Hop Replay；
3. 输出由 Security 产生的 `verified_ingress`，Route/QoS 不能用调用方伪造的布尔值代替；
4. 只扣减允许修改的 Hop Budget、替换下一跳 selector、分配新 `hop_sequence` 并重新计算
   Hop Tag；
5. Source、Destination、Binding、Session、Route、Path、Message、`origin_sequence`、密文
   和 E2E Tag 保持逐字节不变；
6. 最终 Endpoint 才校验 E2E Tag、E2E Replay 和精确 Endpoint ACL。

Relay 的 `frame_work` 不得与入站编码区或输出区重叠；保护/编码路径也拒绝所有会让工作区
与 Payload、输入或输出部分重叠的调用。拒绝时编码字节、结果对象和输出长度保持不变，防止
“尚未验证完就原地覆盖证据”的别名漏洞。

### 2.3 RX Flash 写预算

Hop/E2E Replay Window 改为易失运行态，不再每接受一个包就提交整个 Security Snapshot。
持久化仅用于低频安全代际和 TX Sequence 区间预留。TX 分成独立 Hop/E2E 预留域，每次以
固定块预留高水位；重启跳过已预留但未使用的区间。重启后所有 Peer Session 必须 Reauth，
Group Key 必须 Rekey，因此旧易失 RX Replay Window 不会在原安全上下文中被错误复用。

## 3. V6X-A01～A11 逐项整改与自审

| ID | 原问题 | 整改结果 | 定向自审结论 |
| --- | --- | --- | --- |
| V6X-A01 | Identity 无 load/witness，重启和回滚后高水位可丢失 | Identity 使用完整 Snapshot、单调 `record_generation` 与独立 witness；提交顺序为 witness 预留→state 写入→reload 语义字段核对，启动拒绝 witness/state 回退；Provider 只序列化字段，编译器 padding 不属于持久语义 | 重启、撕裂写、旧快照回放、跨 Provider padding 扰动、Binding/Group 分配与退休均通过；回退路径零状态发布 |
| V6X-A02 | E2E 分配和 Hop Replay 共用 Packet Sequence | Wire、Session Record、TX 预留与 Replay 拆成 `origin_sequence`/`hop_sequence` 两个域 | 同一源经同一下一跳发送不同 E2E 目标不冲突；重启分别跳过两个预留区间 |
| V6X-A03 | 中继没有 verify/retag | 新增 Security-owned Relay API，入站验证后只改逐跳可变域并为下一跳重签 | A→B→C 与五节点逐跳链路通过；篡改、Replay、坏 selector、重叠缓冲区均失败关闭 |
| V6X-A04 | Realtime Sample 可由调用方构造，未绑定认证 Payload | 冻结 48 B canonical `TIME_FOLLOW_UP` 采样载荷；Domain 只从 `security_open_result.frame.payload` 解码 | 错 Opcode、非规范尾字节、错误 Principal/Binding/Session/Route/Path、旧采样均不改变 Domain 状态 |
| V6X-A05 | Cluster ACK/Vote/Ready/Directory 语义未绑定认证 Payload | 为 Cluster Control/Directory 冻结 canonical codec；所有入口从已认证 Frame Payload 解码并匹配 Type/Opcode | 调用方无法以独立结构替换已认证语义；Payload 单字节篡改与字段错绑均拒绝 |
| V6X-A06 | Cluster Store 无独立 rollback witness | Cluster Snapshot 增加 record generation，Store 增加独立 witness，执行 witness-first 与 reload 精确比较 | 正常恢复、旧 Record 回放、witness 超前/落后、撕裂写均 fail-closed |
| V6X-A07 | Transfer 把全局 Operation ID 误当本地连续序列 | 删除 `last_started_message_id + 1` 约束，只拒绝当前活动事务的精确冲突；跨重启幂等归 Durable Operation Journal | `1000→7`、重启后 `9000000` 等合法间隙均接受，精确活动重复仍拒绝 |
| V6X-A08 | Cluster 使用过期 Capability | Cluster 成员、Authority 和 Directory/Path 导出前均校验 discovery/capability 半开 Deadline | `now == deadline` 当场撤销资格，不能继续投票、发 Authority 或建立 Tunnel |
| V6X-A09 | Bootstrap/Capability 预算槽不回收 | 只有显式 Timer Owner 在绝对 Deadline 到达后回收没有 pending/hint 的闲置代际槽；不按输入请求惰性驱逐 | 大量 Link generation churn 不永久耗尽表；未到期、活跃或有 pending 的槽绝不被攻击流量驱逐 |
| V6X-A10 | Group retire 缺 Authority 与持久化门禁 | 动态 Group retire 要求当前 Authority Lease/Fence/Quorum 有效，并以 Identity Snapshot/witness 提交 | 失权、过期、回滚、提交失败均不退休；删除不回退单调 ID 高水位 |
| V6X-A11 | Security 每个 RX 都提交完整 Snapshot | Replay 变为易失状态，TX 区间预留和代际变化才持久化 | 连续接收不增加 Store submit；重启强制 Reauth/Rekey；TX 不复用已保留序号 |

本表的“通过”均指当前软件自审。外部复审状态仍为等待，真实硬件证据仍按第 8 节保留。

### 3.1 全体自审追加发现与整改

完成原 11 项后没有直接送审，而是重新从公共合同、Owner、持久化、跨模块资格和固定工作量
五条主线审查全部 v6 发布面。该轮发现并关闭：

| ID | 严重度 | 追加发现 | 当前闭环 |
| --- | --- | --- | --- |
| V6X-S01 | P0 | Message Journal 的 `record_generation` 仍与 Snapshot 同存，合法旧 Snapshot 可让终态和 Operation ID 历史一起回退 | 新增独立 Message Witness；执行 pending-witness→Journal→committed-witness 三阶段提交，启动对两个掉电窗口确定性 reconcile，旧 Snapshot/缺失证据失败关闭 |
| V6X-S02 | P0 | Realtime 只持久化 Domain Generation，same-generation 下可替换 Path/Session 等 Proposal Identity | Store 改为完整 Domain Proposal Record；same-generation 必须逐字段 exact，任何绑定或配置变化均拒绝 |
| V6X-S03 | P1 | Protocol Owner 在退出 `run()` 时若 task try-lock 失败，会遗留 `running=true` | task lock 改为 acquire-before-return，只有 ISR 使用 try-lock；所有退出路径都能清理运行门，FreeRTOS Port API 同步破坏性升级 |
| V6X-S04 | P1 | Cluster Handover 没有证明目标 Head 属于目标 Config VoterSet | `begin_handover()` 在写事务前精确匹配 Principal+Binding 的 Voter；非成员和错 Binding 零写拒绝 |
| V6X-S05 | P2 | Capability Path 归约一次接收调用方任意 hop 数组，Owner 单次预算不可证明 | 改为 begin→one-hop reduce→finalize 流式接口，单次 O(1)，总跳数上限 65534，溢出零写返回 EXHAUSTED |
| V6X-S06 | P0 | Identity/Bootstrap/Security 仍存在由调用方声明 trusted、父代际未完整持久绑定和 commissioning 撕裂状态可被误接纳的交叉入口 | 所有证明改由验证 Owner 产生；记录逐字段绑定 Authority/Link/Session 和 reload 结果，pending witness、首次/续期/换主序列均以反例钉住 |
| V6X-S07 | P0 | Link 重开后上层缓存可能继续消费旧 Peer Session，或宽泛失效误伤新代际 | Security 提供精确 `{link_id,generation}` 失效并输出统一 invalidation；Stack Owner 固定顺序扇出到 Capability/Route/QoS/Transfer/Realtime/Cluster |
| V6X-S08 | P0 | Capability、Route、Path 与上层模块各自保留父链，可能在部分失效后形成互相矛盾的“当前路径” | 只保留 Route Owner 的 canonical dependency 和稳定 Route Ref；late event、Capability 到期、Route/Path 换代均先撤销上层资格 |
| V6X-S09 | P0 | Cluster 的成员/Directory/Tunnel 路径可把 last-hop 当 origin，或在成员依赖过期后保留 Backup/Joint 权威 | Cluster 入口绑定已认证 origin 与来源序号；成员依赖到期同步撤销 Backup/Joint/Tunnel，Directory 必须带当前 Authority proof |
| V6X-S10 | P0 | Durable Message 对请求内容、Provider 恢复结果和回调重入的约束不足，存在同 ID 异请求或掉电后重复副作用风险 | Journal 绑定 request digest，Provider reconcile 和真实结果终态逐字段核对；回调前建门，EXECUTING 不可证明时进入 IN_DOUBT，retired floor 拒绝重放 |
| V6X-S11 | P1 | Owner 在零进度 backlog、阶段失败或事件风暴下可能无限自唤醒或越过失败阶段 | 预检总预算、合并事件、固定阶段顺序；`has_more && work_done==0` 直接 Fault，失败不运行后继阶段且锁/运行门完整释放 |
| V6X-S12 | P1 | QoS 的 Q0 热流、公平游标、Link 代际和多跳来源配额存在饥饿、ABA 或 last-hop 冒充 origin 的组合风险 | arrival 序号 no-wrap、per-flow Q0 公平、已认证 origin 配额、精确 Link invalidation、失败选择零写和闲置流安全回收均加入回归 |
| V6X-S13 | P0 | Realtime 可在旧 Session/Capability/Path、same-generation 重绑定或故障恢复后发布不再受当前父链支持的时间 | 持久 Proposal 与运行父链逐字段 exact；Capability/Route 当前性、same-domain 高水位、Holdover/rebind 和 fault→LOCKED 计数全部失败关闭 |
| V6X-S14 | P1 | Credit 到期清槽可让同 Session/Link 重建旧 Generation 扩容；完成重组又会被内部 timeout 提前回收 | 过期 Credit 保留高水位直至精确 Session/Link 失效；完成 RX 归应用所有并显式释放，仅未完成槽按重组超时回收 |
| V6X-S15 | P1 | 累计 Hop 是 16 位而 Wire Hop Limit 曾为 8 位；Route/Path 可共存但 AAD 只编码一个 Context Kind | Wire Hop Limit 升为 16 位有界域 `1..65534`，65535 拒绝；AAD 改为 Route/Path presence mask，Both=3 并以逐字节测试冻结 |

严格 MSVC Release 构建还发现局部 Witness/Snapshot 临时对象未显式初始化；现已统一 `{0}` 初始化。
Message Witness 定向复核同时发现内部预检曾把 candidate 当成 previous，现已让 helper 显式接收
两个对象，并由测试同时钉住本地预检和 Fake Provider transition validator。

## 4. 跨模块安全流水

新增 `v6_secure_multihop_pipeline`，不是在测试中直接伪造各模块输入，而是按实际所有权顺序：

```text
认证 Capability Advertise
  → Capability Cache / Path Budget
  → Route Candidate / Probe / Activate / Select
  → Transfer Fragment
  → Security Protect（A→B）
  → Security Relay Verify + Retag（B→C）
  → QoS admission/schedule
  → 可选 Adapter frame ownership
  → Security E2E Open（C）
  → Transfer Reassembly
```

另有五节点安全链路使用每节点最多两个 Peer Session，确保 Nano 容量下也能运行，不靠 Full
专属大表掩盖错误。多目标用例验证 A 经同一 B 分别向 C/D 发送时，Hop Sequence 连续而两个
E2E Origin Sequence 各自在自己的目标 Session 中从 1 开始。

## 5. Profile、布局与资源自审

公共基础类型已拆到 `ucn_v6_types.h`，消除 Profile 配置与 Identity 头的包含环。这样库源码与
外部消费者使用同一 Profile/Layout，不会再发生“库按 Full 编译、消费者按 Nano 解释 storage
union”的静默布局错配。

由于受保护 Transfer 帧在 Nano 下也可能超过 128 B，而项目要求所有 Profile 能解析全部 v6
Frame Contract，Nano Adapter Frame 上限调整为 256 B。该阶段曾把 Storage Layout 从 2 升到 3；
Message Witness、Realtime Domain Record、Owner task/ISR 锁合同和 Capability 流式接口曾把
布局推进到 4；本轮可信父代际、Stack Owner、Capability/Realtime/Cluster/FreeRTOS 公开存储
再次变化，最终统一升级为 Layout 6，固定基代为 `0xD65A000600000000`。中间 Layout 5 与所有
旧 Layout 都只能明确拒绝，不能混用。

本轮最终资源报告：

| Profile | Layout Hash | Security | Route | Transfer | Realtime | Cluster | Adapter |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Full | 14760542101407088479 | 16384 | 70656 | 79872 | 4608 | 24832 | 48128 |
| Lite | 12642647491480237841 | 7680 | 29696 | 29696 | 3328 | 16192 | 17920 |
| Nano | 9093659346181745797 | 4288 | 11776 | 14592 | 3136 | 12032 | 7936 |

单位均为编译期公开 storage 上界字节数，不等于运行时堆用量，也不表示单个产品必须同时实例化
所有可选对象。

## 6. 最终软件验证矩阵

| 门禁 | 结果 |
| --- | --- |
| Windows GCC Full Debug / Release | 26/26，26/26 |
| Windows GCC Lite / Nano Debug | 26/26，26/26 |
| Nano Feature-Off | 21/21 |
| Nano Realtime-only / Cluster-only / Adapter-only | 22/22，24/24，22/22 |
| MSVC 19.29 Full Release | 26/26 |
| WSL GCC ASan/UBSan | 27/27 |
| WSL GCC `-fanalyzer` | 27/27 |
| WSL Clang 18.1.3 Release `-Wall -Wextra -Werror` | 27/27 |
| Windows GCC ISO C99 `-pedantic-errors` | 26/26 |
| WSL GCC TSan | 当前源码编译通过；4 个定向 CTest 中 Owner/Adapter 2 个通过，Identity 遇到 `unexpected memory mapping`，Message 首轮异常退出但隔离重跑通过，表现为运行时地址映射不稳定，**整体不计为通过** |
| Cluster Scale | 1,000 / 10,000 节点均通过 |
| 当前文档、API 索引、archive、旧 CMake surface、安装 consumer | 全部通过 |

为避免外审误读其他工作树版本，本轮关键实现文件 SHA256 如下：

| 文件 | SHA256 |
| --- | --- |
| `src/v6/wire/ucn_v6_wire.c` | `63B929BAD35DB1DF4F7A21A4D1805C42A1AE4A6331FBF64A1B01797FAB31F38E` |
| `src/v6/security/ucn_v6_security.c` | `F8374FA5CCE6E2512520DFAEA4FBC5956C36B55A80A0834EAF0038C4F271C909` |
| `src/v6/identity/ucn_v6_identity.c` | `254DB09C26DB1E8EEB1DA4DFCFC72BE325ACA0146DCF797E97A82224F5655B57` |
| `src/v6/message/ucn_v6_message.c` | `3694CAA2FED9EC671A0A283BDD51B53E7E2B2B5BAA2AE03D136340A12C547FB3` |
| `src/v6/owner/ucn_v6_owner.c` | `F3551039D6851943B53905D20EAB4EC09D48097688508409B3D247F1E9BD876D` |
| `src/v6/capability/ucn_v6_capability.c` | `11EEF832D5FD2A19F4CFF367C3AC2D6D1670823A08A67D6FC8EA1B8A2E015A5B` |
| `src/v6/route/ucn_v6_route.c` | `CE4792A075DA449C85B2E1526FE599D06CB6E76976498005719D86C4B1535E99` |
| `src/v6/qos/ucn_v6_qos.c` | `81DBC1C9D79755886D98F3A84D3BB15ECB4D569D62F8574B455AE6391153E186` |
| `src/v6/cluster/ucn_v6_cluster.c` | `F44634E1AC32FA10C9F78316C3C7F4DB2BEF2792ED93BE85DF542FA725FF2B63` |
| `src/v6/realtime/ucn_v6_realtime.c` | `473A524106A2FAC6C5925F2AFCF2B7E164A3F64ABB2AD29DD75A1BFB6682C576` |
| `src/v6/transfer/ucn_v6_transfer.c` | `B19527D9F1D9C26BA3BA98728C3B995CBD06BCA0D844B2B1F2A29A0D80ACC39E` |

上述哈希只标识当前未提交外审候选工作树；不代表 Git commit 或发布 Tag。

## 7. 多轮全局自审结论

### 第一轮：身份、安全和 Wire 所有权

确认每个可变序号、Generation、Replay Window、Lease 和 Key Selector 只有一个 Owner；
中继不能改变 E2E 不可变域。该轮额外发现并关闭 Relay 工作区与入站/输出缓冲区别名问题。

### 第二轮：持久化、重启和故障关闭

检查 Identity、Security、Cluster 与 Operation Journal 的 witness、提交顺序、reload 证明、
回滚和撕裂写。该轮额外发现 Identity Snapshot 曾用整结构 `memcmp` 核对 Provider 回读，
会把编译器 padding 错当持久语义；现已改为逐字段比较，并用回读 padding 扰动回归锁定。
确认高频 RX 不写 Flash，必须持久化的承诺均在发布状态前完成。

### 第三轮：跨模块语义和生命周期

检查 Security Open Result 到 Capability、Route、QoS、Transfer、Realtime、Cluster 的每个
入口。Realtime 与 Cluster 的决策只能来自已认证 canonical Payload；Capability 到期、Route
变化、Session 重建和 Adapter Buffer 生命周期均不能保留旧权限。

### 第四轮：Profile、静态资源、编译器和发布面

检查 Nano/Lite/Full、单 Feature、安装消费者、MSVC/GCC/Clang、Sanitizer/Analyzer、API
索引与旧符号 denylist。Storage Layout、Layout Hash 与文档统一为 4；没有新增动态内存或
运行期兼容分支。

### 第五轮：并发门禁与工具链反证

复核全部 Provider/Driver callback gate 的调用方所有权、锁覆盖和重入失败关闭；并分别使用
ASan/UBSan、`-fanalyzer`、GCC/Clang/MSVC 验证。另实际构建并运行 WSL GCC TSan 定向门禁：
Owner/Adapter 两项通过，Identity 遇到 `unexpected memory mapping`，Message 在整组 CTest 中
异常退出但隔离重跑通过，显示当前 TSan runtime/地址布局仍不稳定。该结果不冒充整体并发通过，
TSan 继续保留为发布阻断。

### 第六轮：发布表面、文档与证据一致性

复核安装消费者、公共聚合头、archive/旧 CMake surface denylist、API 索引、当前文档链接、
Profile 资源报告和 `git diff --check`。确认发布面只包含 v6；资源数字、Layout 6 和 241 项 API 索引
均来自同一轮当前源码构建，报告明确区分 Host 软件证明、不可用工具链和待完成硬件证据。

### 第七轮：全所有者与反回退复审

从零重新枚举 Operation ID、Journal generation、Domain proposal、Cluster authority 和 Route/Path
能力的所有者、父域、持久证据与重置条件。该轮发现 V6X-S01～S05，并逐项补入能区分旧实现的
对抗回归；不以原 11 项测试通过代替新增边界证明。

### 第八轮：整改后从头到尾全体自审

在追加整改完成后，重新执行 Identity→Wire→Message→Owner→Security→Capability→Route→QoS→
Transfer→Realtime→Cluster→Adapter 的合同、失败原子性、Profile 容量、安装发布面和文档证据
审查。MSVC 严格编译发现的临时对象初始化问题，以及 Message Witness 内部 previous/candidate
错位均在本轮关闭；最后用 GCC/MSVC/Clang、Sanitizer、Analyzer 和八种 Profile/Feature 组合
重新验证，不沿用整改前的测试结果。

### 第九轮：精确失效、父代际与失败原子性复审

从 Link→Session 开始逐层注入重开、过期、迟到完成、容量满和 Provider/Driver 失败，核对
Security 产生的统一 invalidation 是否以固定顺序到达 Capability、Route、QoS、Transfer、
Realtime 与 Cluster。该轮发现并关闭 V6X-S06～S14；同时确认失败入口不删除旧可用状态、
不消耗序号/Token、不留下半提交事务。

### 第十轮：Wire 数值域与 canonical 认证复审

逐项核对 Header 字段宽度、累计算术、shift、Deadline、Sentinel 与 RFC。该轮发现累计 Hop
支持 65534 而 Wire 只能表达 255，以及 Route/Path 同时存在时 AAD 只声明 Route 两项矛盾；
现已统一为 16-bit Hop Limit 和 Route/Path presence mask，并补精确上界、65535 反例和 Both=3
逐字节 AAD 回归。GCC 深度告警、MSVC、Clang、Sanitizer 和 Analyzer 均使用修正后源码重跑。

当前十轮自审未发现仍开放的软件 P0/P1。该结论是送外审的候选结论，不替代外部签字。

## 8. 明确保留的发布阻断

以下项目没有因本轮软件整改而变成完成：

1. 真实双槽 Flash、witness 独立介质、断电窗口、擦写寿命与回滚攻击；
2. 生产密码 Provider、TRNG、密钥注入、安全存储与侧信道评估；
3. ESP32-S3、CAN/CAN-FD、USB、UART/RS-485、ESP-NOW 的真实 ISR/DMA 与多跳链路；
4. Realtime 硬件 timestamp、路径非对称、晶振漂移、温度与 uncertainty 标定；
5. 1～5 跳吞吐、Q0～Q3 P99/P999、功耗与 24 h 长稳；
6. 可用且稳定的 TSan runtime 下完整并发门禁；当前 WSL GCC TSan 构建成功，Owner/Adapter
   定向用例通过，但 Identity/Message 运行受不稳定地址映射影响，不能作为全体通过证据；
7. 本轮统一外部复审。

因此当前只能进入“软件整改完成、等待统一外部复审”状态，不能生成 UCN 1.0 RC 或发布 Tag。
