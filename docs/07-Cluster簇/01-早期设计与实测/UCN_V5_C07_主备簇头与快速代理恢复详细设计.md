# UCN V5 C07 · 主备簇头与快速代理恢复详细设计

> 历史草案提示：本文保留此前方案推导，但其中“Cluster Format v1 不变”“复用现有 KEEPALIVE/HEAD_TAKEOVER 即可接管”“C06 不参与”的假设已经过源码核对后被否定。后续实现必须以 [C07 主备簇头、快速恢复与目录交接整体方案](UCN_V5_C07_主备簇头与快速代理恢复整体方案.md) 为准。

> 状态：历史设计草案（C07 设计输入）；当前实现事实、v3 控制格式和测试证据以链接的 C07 整体方案为准。
> 目标：主簇头失联 1-2s 备用接管；主备双失联 2-3s 快速代理临时恢复；后台 10-15s 重选最优主/备并平滑切回。
> 历史约束：ucn_cluster 保持独立 Extended、Core Wire v5 与消息格式 v1 不变（该“v1 不变”结论已被 v3 破坏性升级取代）。

## 1. 目标与非目标

### 目标
1. 选举阶段同时产生主簇头（HEAD）与备用簇头（BACKUP），备用持有与主一致的成员租约视图。
2. 主失联（链路抖动/断电）→ 备用 1-2s 接管，成员免重新加入。
3. 主备同时失联 → 任意可担任节点 2-3s 快速选代理（PROXY），临时维持连通与数据转发。
4. 代理期间后台重选最优主/备，代理寿命到期后平滑切回新主（20% 评分门槛防抖动）。
5. 全程控制面峰值有界（复用广告轮转 + 新增 Token 预算）。

### 非目标
- 数据面双活/零丢帧（Q1 无端到端 ACK；"无缝"定义 = 控制面秒级恢复 + 业务中断 ≤1-2s，关键命令由 Transfer 保证）。
- 跨簇路由/目录（C06）；生产安全（S02，require_protected_control 仍由产品开启）。
- 多级分簇与万级验证（C08）。

## 2. 角色与状态机扩展

### 2.1 角色表（现有 8 + 新增 1）

| 角色 | 现状 | 本设计 |
| --- | --- | --- |
| DISABLED/DETACHED/JOIN_PENDING/MEMBER/CANDIDATE/HEAD | ✅ 已实现 | 不变 |
| BACKUP | 枚举已存在，状态机未使用 | **实现**：静默监听成员抄送、维护成员租约视图、主失联后接管 |
| STEPPING_DOWN | 枚举已存在，未使用 | **实现**：代理/旧主退位过渡态 |
| **PROXY** | ❌ 新增 | 快速代理：临时 Head（不看评分、临时超容、寿命受限） |

### 2.2 状态转移（新增/修改的转移）

```
选举完成(complete_election)
  HEAD ──BACKUP_ASSIGN──▶ 次优候选节点
  次优 ──BACKUP_ACCEPT──▶ BACKUP（静默）

主失联检测（备用侧：连续 BACKUP_MISS_LIMIT 次未收到 HEAD_BEAT）
  BACKUP ──HEAD_TAKEOVER──▶ HEAD（term+1）

主失联检测（成员侧：租约到期前 MIGRATE_ADVANCE_MS）
  MEMBER ──JOIN_REQUEST(向备用)──▶ JOIN_ACCEPT ──▶ MEMBER(新主=备用)

主备双失联（成员/节点侧：2×FAST_LEASE 无主无备）
  DETACHED/MEMBER ──退避到期+PROXY_DECLARE──▶ PROXY
  其余节点 ──收到 PROXY_DECLARE──▶ MEMBER(跟随代理)

后台重选完成（新主 HEAD_DECLARE，评分≥20%门槛且代理任期已满）
  PROXY ──HEAD_STEPDOWN──▶ MEMBER(跟随新主)
  新主 ──BACKUP_ASSIGN──▶ 次优 → 新一轮 BACKUP

代理寿命到期（PROXY_TTL）且新主未就绪
  PROXY ──HEAD_STEPDOWN──▶ DETACHED ──退避──▶ 重新 PROXY
```

## 3. 消息格式设计（格式 v1 不变，仅扩展 Type）

### 3.1 载荷布局（28B 固定，现有字段完全复用）

| 偏移 | 长度 | 字段 | 用途 |
| ---: | ---: | --- | --- |
| 0 | 1 | Format Version | 保持 1 |
| 1 | 1 | Message Type | 1-13（见下） |
| 2 | 1 | Sender Role | 含 PROXY |
| 3 | 1 | Flags | 新增位：0x01=BACKUP_WANTED（JOIN 时自荐）、0x02=PROXY_MODE |
| 4 | 4 | Cluster ID | 复用 |
| 8 | 4 | Term | 复用（接管/代理必须 term+1） |
| 12 | 4 | Head Node ID | 复用 |
| 16 | 2 | Head Score | 复用 |
| 18 | 2 | Available Capacity | 复用 |
| 20 | 4 | Lease ms | 复用 |
| 24 | 4 | Nonce | 复用 |

### 3.2 消息类型扩展（1-9 现有，新增 10-13）

| Type | 名称 | 方向 | 语义 |
| ---: | --- | --- | --- |
| 1 | ADVERTISE | 广播轮转 | 现有 |
| 2-6 | JOIN_* / KEEPALIVE / LEAVE | 现有 | 现有 |
| 7 | HEAD_DECLARE | 现有 | 现有 |
| 8 | HEAD_TAKEOVER | 备用→成员 | **接管声明**（现已有 case，本设计补全逻辑） |
| 9 | HEAD_STEPDOWN | 代理/旧主→成员 | **退位声明**（现已有 case，本设计补全逻辑） |
| 10 | BACKUP_ASSIGN | Head→指定节点 | 指定其为备用（携带当前 term/head/score） |
| 11 | BACKUP_ACCEPT | 节点→Head | 确认担任备用 |
| 12 | PROXY_DECLARE | 节点→邻居 | 快速代理宣布（携带临时 term） |
| 13 | PROXY_ACK | 成员→代理 | 成员确认跟随（可选，用于代理统计覆盖数） |

### 3.3 复用消息的扩展语义
- **KEEPALIVE 抄送备用**：成员把 KEEPALIVE 同时发给 HEAD 与 BACKUP（type=5、role=MEMBER 不变）；备用收到后按与 Head 相同规则更新 members 表租约（不回复）。
- **HEAD_BEAT**：Head 向 BACKUP 发送 KEEPALIVE（type=5、role=HEAD、head_node_id=self）；备用据此判定主存活。
- **JOIN_REQUEST 迁移**：成员迁移时复用 type=2；备用在成员视图命中时直接 JOIN_ACCEPT（跳过容量检查，因视图已存在）。

### 3.4 向后兼容说明
- 格式版本保持 1（字段布局未变）。旧固件对新 Type 返回 MALFORMED 并拒绝——同一簇内要求版本一致；跨版本混跑时新消息被旧节点拒绝属预期升级约束，文档注明。

## 4. 备用选择与成员视图同步

### 4.1 备用选择（complete_election 扩展）
1. Head 从候选表中选**评分次高且 head_capable** 的节点；
2. 无现成候选时，Head 广告中置 BACKUP_WANTED 位，成员 JOIN 时带自荐位，Head 指定首个自荐者；
3. Head 发 BACKUP_ASSIGN（type=10，role=CANDIDATE→目标节点）；目标节点回 BACKUP_ACCEPT 进入 BACKUP；
4. BACKUP_ASSIGN 重试间隔 UCN_CLUSTER_BACKUP_ASSIGN_RETRY_MS=1000ms，最多 3 次；失败则 Head 换候选。

### 4.2 备用成员视图
- 备用直接复用 members[] 表（BACKUP 角色下该表空闲）：成员 KEEPALIVE 抄送 → 按现有 handle_keepalive 同规则更新租约与 nonce；
- 备用对成员 JOIN_REQUEST 仅登记（不 ACCEPT，避免与主双写），接管时才 ACCEPT；
- 成员离开（LEAVE/租约过期）在备用侧同样清理。

### 4.3 控制开销
- Head→备用心跳：1 帧/keepalive 周期；
- 成员抄送备用：1 帧/keepalive 周期（成员 TX 翻倍：DEFAULT 16 成员 = 16 帧/s 额外；FAST 500ms = 64 帧/s 额外——**建议 FAST 仅用于故障窗口或故障前 10s 动态切换档案**）；
- 可选项：成员抄送半频（每 2 个 keepalive 抄送一次，开销减半，接管时备用租约略旧但可接受）。

## 5. 备用接管协议（主失联，目标 1-2s）

### 5.1 主失联判定（防双主）
- 备用侧：连续 UCN_CLUSTER_BACKUP_MISS_LIMIT=3 次 keepalive 周期未收到 HEAD_BEAT（FAST 下 1.5s；DEFAULT 下 6s）→ 判定主失联；
- 成员侧：head_lease 到期前 UCN_CLUSTER_MEMBER_MIGRATE_ADVANCE_MS=500ms → 若已知备用 ID 则向备用发 JOIN_REQUEST；
- **双主防护**：接管必须 term+1；收到更高 term 的 HEAD_DECLARE/TAKEOVER 的节点立即跟随高 term；主恢复后因 term 低只能作为成员/候选加入（复用现有分裂收敛）。

### 5.2 接管动作
1. 备用：role→HEAD、term+1、发 HEAD_TAKEOVER（逐成员单播，16 成员约 16 帧）；
2. 成员：收到 TAKEOVER（term 更高）→ 更新 head 指向备用、续租，**免重新加入**；
3. 未收到 TAKEOVER 的成员：JOIN_REQUEST 迁移路径兜底（备用视图命中 → 直接 ACCEPT）；
4. 新主（原备用）随后正常执行 BACKUP_ASSIGN 重建自己的备用。

### 5.3 期望时序（FAST 档案）
```
t=0       主失联
t=0.5-1.5s 备用连续 3 次缺失判定（或成员提前迁移）
t=1.5-2.0s TAKEOVER + 成员跟随 / JOIN 迁移完成
▶ 控制面恢复 1.5-2.0s（业务中断 ≤2s）
```

## 6. 快速代理协议（主备双失联，目标 2-3s）

### 6.1 触发
- 成员：2×FAST_LEASE（4s）内既无主租约续期、也未收到备用 TAKEOVER → 进入 DETACHED + recovery_observation（FAST 500ms）；
- 或：成员收到邻居 PROXY_DECLARE（无需自己触发）。

### 6.2 代理选举（不看评分，确定性最快）
1. DETACHED 且 head_capable 节点计算确定性退避：backoff = (node_id * 7919) % UCN_CLUSTER_PROXY_BACKOFF_MAX_MS（500ms）；
2. 退避到期 → 发 PROXY_DECLARE（type=12，role=PROXY，term = 本地 term+1）；
3. 首个声明者成为代理；同退避值冲突时 ID 小者胜（收到竞争声明且 ID 更小 → 让位）；
4. 其余节点收到 PROXY_DECLARE → role=MEMBER、head=代理、续租（可选回 PROXY_ACK 供代理统计覆盖）。

### 6.3 代理职责与限制
- 只维护成员连接与数据转发（复用现有 HEAD 的成员表与转发路径）；
- **临时超容**：容量检查放宽到 members 表物理上限（跳过 member_capacity）；
- **暂停评分切换**：代理任期内的 better-candidate 逻辑挂起（防抖动）；
- **寿命硬上限**：UCN_CLUSTER_PROXY_TTL_MS=30000；到期强制 HEAD_STEPDOWN → 成员 DETACHED → 若无新主则重新代理，若新主已就绪则跟随新主。

### 6.4 期望时序
```
t=0       主备同时失联
t=4.0s    成员双倍租约过期检测
t=4.5s    恢复观察(500ms)完成
t=4.5-5.0s 退避(≤500ms) + PROXY_DECLARE
t=5.0-5.5s 成员跟随代理
▶ 控制面恢复 5.0-5.5s（相对失联时刻）；代理即选即用 0.5-1.5s
```

## 7. 后台重选与平滑切回

### 7.1 重选（代理期间并行）
- 代理当选后，代理自身与所有 head_capable 节点继续按现有流程收集候选（观察 1s + 选举窗 1s，FAST）；
- 不设"代理不参与选举"——代理同时是候选者之一（其评分真实反映能力，但代理任期内的评分切换暂停仅限"换代理"动作，不阻碍"新主产生"）。

### 7.2 切回条件（防抖动）
- 新主评分 ≥ 当前代理评分 × (1 + 20%)（复用 switch_improvement_percent）；
- 且代理任期 ≥ min_tenure（FAST 10s）；
- 满足后：代理发 HEAD_STEPDOWN → 成员跟随新主（新主 HEAD_DECLARE/TAKEOVER，term 更高）→ 新主执行 BACKUP_ASSIGN 重建备用。

### 7.3 期望时序
```
t=5s      代理恢复
t=6-7s    后台重选完成（观察1s+选举窗1s）
t=15-17s  代理任期(10s)满足 → STEPDOWN
t=17-18s  成员跟随新主 → 切回完成
▶ 全程业务中断仅出现在 t=0-5s 窗口；t=5s 后业务连续
```

## 8. 新增常量与配置

| 常量 | 默认 | 说明 |
| --- | ---: | --- |
| UCN_CLUSTER_BACKUP_MISS_LIMIT | 3 | 备用判定主失联的连续缺失次数 |
| UCN_CLUSTER_BACKUP_ASSIGN_RETRY_MS | 1000 | BACKUP_ASSIGN 重试间隔 |
| UCN_CLUSTER_MEMBER_MIGRATE_ADVANCE_MS | 500 | 成员租约到期前提前迁移窗口 |
| UCN_CLUSTER_PROXY_BACKOFF_MAX_MS | 500 | 代理退避上限（确定性） |
| UCN_CLUSTER_PROXY_TTL_MS | 30000 | 代理寿命硬上限 |
| UCN_CLUSTER_FLAG_BACKUP_WANTED | 0x01 | JOIN 自荐备用位 |
| UCN_CLUSTER_FLAG_PROXY_MODE | 0x02 | PROXY 声明标记 |

config 新增字段：backup_enabled（bool）、proxy_capable（bool，默认=head_capable）、proxy_ttl_ms（默认 30000）。

## 9. 控制面预算（防风暴）

| 阶段 | 峰值（16 成员 FAST） | 说明 |
| --- | ---: | --- |
| 稳态 | Head 心跳 2/s + 成员主/备抄送 32×2/s ≈ 66/s | 建议稳态用 DEFAULT（8/s） |
| 接管 | TAKEOVER 16 帧 + 迁移 ≤16 帧（一次性） | 一次性突发，可接受 |
| 双断代理 | 全员退避广播（≤16 帧） | 退避随机化防同步 |
| 切回 | STEPDOWN 16 帧 + JOIN 迁移 ≤16 帧 | 一次性 |
| 防护 | 新增 Cluster 独立 Token Bucket：burst=8、refill=500ms/枚（每消息类型共享） | C07 必备，防止高密度反复触发 |

## 9.1 参数权衡附录：心跳/租约/恢复时间的参数-时间-开销对照表

> 目的：回答"加快心跳能否缩短在网/离网感知与选簇时间"。结论：**能，但收益递减且有三个硬边界**（误判风险、介质空口、CPU/调度）。本表供产品选择档案时权衡。

### 9.1.1 恢复时间的构成（各环节由哪个参数决定）

```
成员感知"主失联"（占 ~55%）--> 恢复观察（~14%）--> 选举比较（~27%）--> 广告/加入传播
```

| 环节 | 决定参数 | DEFAULT | FAST_FIXED | 极限档案 X |
| --- | --- | ---: | ---: | ---: |
| 感知主失联（最耗时） | **lease_ms 租约**（成员靠 Head 广告续租，收不到等租约到期） | 8s | 2s | 1s |
| 感知成员离网（Head 侧） | lease_ms（成员 KEEPALIVE 续租，容错 4 次缺失） | 8s | 2s | 1s |
| 恢复观察 | recovery_observation_ms | 5s | 0.5s | 0.25s |
| 选举比较 | election_window_ms | 3s | 1s | 0.5s |
| 广告/加入传播 | advertise_interval_ms / join_retry_ms | 1s | 0.25s | 0.125s |
| 续租频率（不直接决定感知时间） | keepalive_interval_ms | 2s | 0.5s | 0.25s |

**关键结论**：keepalive 只是续租频率；真正决定"多久判定失联"的是**租约**（= 4 个 keepalive 周期的容错）。加快心跳的价值在于**允许缩短租约**（容错次数不变），而不是心跳本身。

### 9.1.2 加快心跳的收益与代价（16 成员簇，量化推演）

| 档案 | 恢复时间 | 成员侧控制开销 | 误判风险（1s 抖动概率） |
| --- | ---: | ---: | ---: |
| DEFAULT（8s/2s/5s/3s/1s） | 16.2s（实测） | 8 帧/s | 极低（容错 4 次 x 2s） |
| FAST_FIXED（2s/0.5s/0.5s/1s/0.25s） | 3.68s（实测） | 32 帧/s + 广告 | 低（容错 4 次 x 0.5s） |
| 极限档案 X（1s/0.25s/0.25s/0.5s/0.125s） | **~2s（推算）** | 64 帧/s + 广告 ≈ 72 帧/s | **中（一次 1s 无线抖动即误判）** |
| **本设计（备用接管，不改全员心跳）** | **1.5-2s（目标）** | 32 帧/s + 备用抄送 32 ≈ 64 帧/s | 低（备用 3 次缺失判定 1.5s） |

**收益递减规律**：3.7s→2s 需要心跳翻倍（32→72 帧/s），而 2s 之后每再快 0.5s 都需再次翻倍。备用接管方案以相近目标时间、几乎零额外全员开销达到同样效果。

### 9.1.3 加快心跳的三个硬边界

| 边界 | 说明 | 量化 |
| --- | --- | --- |
| 误判风险 | 租约/心跳比值 = 容错次数（当前 4 次）。租约 1s 时一次 1s 无线抖动即误判 → 频繁重选抖动 | 比值建议保持 ≥3-4 |
| 介质空口 | 控制帧共享介质：UART 115200 ≈ 45 帧/s 上限（16 成员 FAST 心跳已占 70%）；3M UART ≈ 1000 帧/s 无压力；ESP-NOW ~400 帧/s 紧张 | 介质是最终瓶颈 |
| CPU/调度 | 每 250ms 一轮 16 成员发送/处理，挤占业务 step | FAST 实测 CPU 0.04-0.07%（4 节点），大簇线性上涨 |

### 9.1.4 动态档案建议

- **稳态**：DEFAULT 档案（8 帧/s 开销，误判风险极低）；
- **故障窗口**：成员检测到主失联/租约将到期时，**动态切换 FAST 档案**（缩短恢复观察与选举窗），恢复后切回 DEFAULT；
- **本设计（备用接管）**：主备之间的 HEAD_BEAT 可用 FAST 频率（500ms），其余成员保持 DEFAULT——以最小开销获得 1.5-2s 接管。

## 9.2 簇与 Core 备用路径的分层关系（双保险澄清）

> 回答"有了簇以后，节点的备用路径（用于无缝切换的路径）还在吗"。结论：**全部保留**——簇是组织层，与 Core 数据面备用机制完全正交。

### 9.2.1 代码事实：簇与 Core 路由/路径机制隔离

grep 验证：ucn_cluster.c 对 Core 的唯一调用是 `ucn_node_copy_neighbor_summaries()`（只读邻居摘要）。簇不调用/不修改任何路由、Candidate、Path、Policy、转发表 API，也不注册数据路径。

| 数据面备用机制 | 所在层 | 簇是否影响 |
| --- | --- | --- |
| 多 Bearer 主备（同对端 UART+WiFi，Primary 断切 Backup） | Core Link 层（UCN_MAX_BEARERS_PER_NEIGHBOR=2） | ❌ 完全不受影响 |
| Candidate Route（候选路由 + Probe 验证 + 低成本替换） | Core 路由层（8 槽） | ❌ 完全不受影响 |
| Policy Path 备用（PINNED_FAILOVER 主路径断→备路径） | Core 策略层（8 Path） | ❌ 完全不受影响 |
| Path 转发表（PATH_INSTALL 显式路径） | Core 路径层（8 条目） | ❌ 完全不受影响 |
| RREQ/RERR 断链重发现 | Core 路由层 | ❌ 完全不受影响 |

**根本原因**：ucn_cluster 只决定"谁是谁的簇头"，不替换"数据往哪走"。成员→簇头、成员↔成员、任何业务路径全部照常走 Core 原有机制（直连 → 路由缓存 → Candidate → Policy/Path）。

### 9.2.2 两类"备用"的区分（不要混淆）

| 类别 | 机制 | 管什么 | 所在层 |
| --- | --- | --- | --- |
| **数据面备用** | Bearer 主备 / Candidate / PINNED_FAILOVER / PATH_INSTALL | 业务帧走哪条路 | Core（簇无关） |
| **控制面备用** | 备用簇头 BACKUP（持成员视图、TAKEOVER 接管） | 簇头死了谁来当 | Cluster（C07 新增） |

- 数据面备用：**簇建不建都一模一样地工作**（帧级/秒级，已有）；
- 控制面备用：C07 的备用接管（1.5-2s），与数据面备用**互补**，不是替代。

### 9.2.3 故障场景的双保险叠加表

| 故障 | 谁兜底 | 恢复 |
| --- | --- | --- |
| 主簇头**物理链路**断（单个 Bearer 死） | Core：同对端 Backup Bearer 自动接管 | 帧级（已有） |
| 主簇头**路由**失效 | Core：Candidate/Policy 备用路径 | 秒级（已有） |
| 主簇头**整机**失联 | Cluster：备用簇头接管（C07） | 1.5-2s（设计） |
| 主备簇头**都死** | Cluster：快速代理 + **Core 直连/多跳兜底** | 5s + 物理可达即通信 |

**最关键兜底**：即使簇头全死，成员之间的 Core 直连/多跳路由照常工作（簇是组织不是通信前提）——"簇头死了数据全断"不成立，除非业务流量设计成"必须经簇头"。

### 9.2.4 产品设计约束（据此规划流量）

1. **业务路径不得只依赖簇头**：成员间直连/多跳应保留为兜底（簇头聚合转发是优化，不是唯一通道）；
2. **簇控制帧本身也享受数据面备用**：Endpoint 0xA0 的消息走普通 QoS/多 Bearer 路径，无需额外处理；
3. **无缝切换的完整链路 = 数据面（Bearer/Candidate）+ 控制面（BACKUP）两层同时就绪**：任一故障层级都有对应兜底；
4. **测试时分别验证两层**：Bearer 切换测试（Core 已有）、簇头接管测试（C07 新增），两者独立覆盖。

## 10. 测试场景设计

### 10.1 单元测试（test_cluster.c 扩展）
| # | 测试 | 判定 |
| --- | --- | --- |
| T1 | BACKUP_ASSIGN/ACCEPT 编解码与状态转移 | 次优候选成为 BACKUP；重复指定拒绝 |
| T2 | 备用成员视图同步 | 成员 KEEPALIVE 抄送 → 备用租约更新；LEAVE/过期清理一致 |
| T3 | 主失联 → 备用接管 | 3 次缺失 → TAKEOVER；成员免重入；term 递增 |
| T4 | 主未真断（抖动） | 备用不抢先（缺失计数 <3 时重置）；无双主 |
| T5 | 双断 → 快速代理 | 退避确定性 → 唯一 PROXY；成员跟随；超容允许 |
| T6 | 代理寿命到期 | STEPDOWN → 成员 DETACHED → 重新代理或跟随新主 |
| T7 | 后台重选 + 切回 | 20% 门槛；任期满足；STEPDOWN 后成员跟随新主；新主重建备用 |
| T8 | 代理期间数据面 | 代理转发成员业务帧成功（Q1 端到端） |
| T9 | 控制预算 | 高密度反复触发时 Token 拒绝而非无界广播 |

### 10.2 模拟器场景（ucn_cluster_sim.c 扩展）
| 场景 | 内容 | 门禁 |
| --- | --- | --- |
| backup-failover | Head 失联 → 记录备用接管时刻 | 接管 <2s（FAST）；无业务丢帧窗口 >2s |
| dual-fail-proxy | Head+Backup 同时失联 | 代理恢复 <5.5s；代理寿命内新主选出率 100% |
| proxy-return | 代理期间重选 + 切回 | 切回无抖动（无连续 3 次以上角色翻转）；业务中断累计 <3s |
| impaired-backup | 15% 丢包 + 抖动下重复上述场景 | 接管成功率 ≥90%（3 轮）；无双主时长 >2s |
| scale-256/1000 | 每 8 节点组内主备+代理混合 | 收敛时间相对首阶段不劣化 >2×；控制峰值 ≤40/s |

### 10.3 MCU 门禁（C07 实机，接续 C05）
- 四节点线形拓扑：A—B(主)—C—D(备)；
- 断电 B：测量 A/C 迁移到 C 的时间（目标 <2s）与业务中断；
- 同时断电 B+D：测量快速代理恢复时间（目标 <5.5s）；
- 120s 稳定性：无角色抖动、无租约误过期；
- 独立 Bearer 插拔、1h 长稳、功耗（C05 遗留项一并闭环）。

## 11. 代码对接点（改动清单）

| 文件 | 改动 |
| --- | --- |
| include/ucn/ucn_cluster.h | 角色枚举加 PROXY；消息类型加 10-13；Flags 位；新常量；config 加 backup_enabled/proxy_capable/proxy_ttl_ms；BACKUP_MISS_LIMIT 等宏 |
| src/extended/ucn_cluster.c | complete_election 后 BACKUP_ASSIGN；handle_backup_assign/accept；KEEPALIVE 双目标发送（成员侧）；HEAD_BEAT（Head 侧）；handle_takeover 补全（term+1+成员迁移）；PROXY 状态机（退避/声明/寿命/暂停切换）；handle_stepdown 补全；step() 扩展 |
| tools/ucn_cluster_sim.c | 新场景 backup-failover/dual-fail-proxy/proxy-return/impaired-backup；控制预算模拟 |
| tests/test_cluster.c | T1-T9 |
| docs/00-项目管理/00-任务表.md | C07 拆分：C07a 备用接管 / C07b 快速代理 / C07c 预算与测试 |

## 12. 验收门禁

```text
1. 单测 T1-T9 全绿（含防双主 T4、预算 T9）；
2. 模拟器 5 场景通过：backup-failover <2s、dual-fail-proxy <5.5s、
   proxy-return 无抖动、impaired 接管成功率 ≥90%、256/1000 控制峰值 ≤40/s；
3. 四节点 MCU：B 断电 A/C 迁移 <2s；B+D 双断代理 <5.5s；120s 无抖动；
4. 独立 Token Bucket 生效（反复触发被拒绝而非无界广播）；
5. 文档同步：任务表、调用手册、全局配置说明、容量口径（仍不宣称万级）。
```

## 13. 风险与缓解

| 风险 | 缓解 |
| --- | --- |
| 成员抄送流量在 FAST 下偏高（64/s@16 成员） | 抄送半频；稳态 DEFAULT/故障窗 FAST 动态档案 |
| 备用与主之间视图不一致（成员刚加入未抄送） | 新成员首次 JOIN 后立即向备用补发一次 KEEPALIVE |
| 代理覆盖不足（部分成员听不到声明） | 成员侧自身退避选举兜底（同一确定性退避保证唯一代理） |
| 代理能力弱导致数据降级 | 明确代理为"临时维持"语义；30s 硬上限 |
| 旧固件拒绝新消息 | 同一簇版本一致；升级文档注明 |

## 14. 总结

> 本设计在 ucn_cluster 首阶段基础上，以"消息类型扩展 + 状态机补全"方式实现三层恢复：
> **主断 → 备用 1-2s 接管（成员免重入）；主备双断 → 确定性退避代理 2-3s 临时恢复；代理期后台重选 → 20% 门槛平滑切回新主并重建备用**。
> 关键防线：term 仲裁 + 连续缺失判定防双主、退避随机化防风暴、代理寿命硬上限防劣化、独立 Token Bucket 限频。
> 工作量：约一个完整开发迭代；建议 C07 拆为 C07a（备用接管）/ C07b（快速代理）/ C07c（预算与测试）三子任务顺序推进。
