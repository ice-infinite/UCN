# UCN V5 C06 · 簇间寻址、目录与隧道详细设计

> 状态：**C06.0～C06.3 已完成当前软件闭环。** `ucn_cluster_federation` 已提供 `0xA1` 严格 Codec、Head Locator 的发布/撤销、固定 Directory、有限期 Query Cache、`Cluster ID -> Head ID` 锚点，以及受显式开关约束的单帧 `Submit → Data → Deliver` Tunnel。**跨簇 Transfer/大消息、自动 Gateway 和 MCU 两簇实测仍未完成**；普通 `ucn_node_send*()` 行为没有改变。

## 1. 要解决的实际问题

单层 `ucn_cluster` 已能让一跳邻居选出 Head 并维护成员租约，但业务帧仍然用 Core 的 `destination=最终 Node ID` 寻路。网络扩大后，这会使跨簇通信沿途的普通 MCU 也逐个学习远端成员的 Route/Path，分簇就没有带来数据面状态收益。

C06 的目标是把远端业务拆成三件事：

```text
业务节点 A
  └─只需到本簇 Head H1 的本地路由
       ├─目录：目标 Node ID -> {目标 Cluster ID, 目标 Head H2, 租约}
       └─隧道：H1 -> H2，只按“簇 Head”建立 Core 路由
                                    └─H2 再向本簇目标节点 C 本地交付
```

因此，普通节点与普通中继**不保存远端成员 C 的 Route/Path**；它们最多为 H1/H2 这类簇级锚点转发。远端成员 Locator 仅受控地保存在 Directory Authority 和有限期 Cache 中，簇级 Next-Cluster 状态只在 Head/Gateway 保存。

这仍是 MCU-first 设计：没有 Linux、云端或中心路由器是协议运行前提。第一阶段允许产品用一个或多个 MCU Head 充当目录副本；它们是可替换的控制角色，不是 Linux 控制器。完全自动的目录副本选举、边界 Gateway 发现与多级目录留给 C07/C08，不能提前宣称已经完成。

## 2. 已核对的代码边界

当前源码事实如下：

- `ucn_cluster` 是独立 Extended 对象，控制 Endpoint 为 `0xA0`，只有一跳 Peer、Candidate 和 Member 状态；没有 Locator、Gateway 或 Tunnel 表。
- Core 以 `ucn_frame_t.destination` 决定转发；普通业务帧若直接发送给远端 Node ID，就会建立到该成员的普通 Route。
- `ucn_node_send_endpoint()` 与 `ucn_node_set_endpoint_handler()` 已经提供静态 Endpoint 的发送/接收边界，且 Endpoint 不增加 Core Header 字段。
- Core 对一个外层帧可以在中继处透明转发；若外层目标是远端 Head，则沿途只需要该 Head 的 Route，而不是目标成员的 Route。

所以 C06 不改 Core Wire v5、不侵入 `ucn_node_t`，而是新增可选 Extended Archive：

```text
产品/BSP
  ├─把 Endpoint 0xA1 绑定到 Federation receive
  ├─在唯一 Protocol Owner 中调用 Federation step
  └─产品提供目录副本、授权与内层安全 Provider

ucn_cluster_federation（新 Extended，C06）
  ├─本簇成员 Locator 发布/续租
  ├─Directory Authority 与有限期 Locator Cache
  ├─Head/Next-Cluster 固定表与事务表
  └─Submit -> Head-to-Head Tunnel -> Local Delivery

ucn_cluster（既有 Extended）
  └─选举、成员、Head 租约；提供只读 Cluster View/成员快照

UCN Core（不改 Wire v5）
  └─HELLO、Core Route、Endpoint、QoS、Security 和逐跳转发
```

`ucn_cluster_federation` 只由产品显式链接和实例化；Core-only、只使用 Cluster 选举的产品不为目录、事务或隧道缓冲付出 RAM。

## 3. 第一阶段的网络模型与明确限制

### 3.1 C06.2～C06.3：Head Gateway + 固定目录副本

C06.2 把**已成为 Head 的节点**作为唯一 Locator 发布者与未来 Gateway。普通 Member 不承担簇边界转发，也不自行宣布外部 Reachability。这样不会和 C07 尚未实现的备用 Head、Gateway 容量、拆并和边界移动策略冲突。

Directory Authority 列表由产品以固定 Node ID 配置，默认最多两个副本。它们可以运行在普通 MCU Head 上；没有 Authority 可达时，新跨簇解析失败关闭，本簇选举与本簇通信不受影响。C06.2 不把“目录副本自动发现”写成已实现：产品必须先给出至少一个可达 Authority Head。

解析到目标 Head 后，C06.2 缓存 `目标 Cluster ID -> 目标 Head ID` 的直达锚点；C06.3 的显式 `ucn_cluster_federation_send()` 才使用产品 Send 回调正常发送 `H1 -> H2` 的 Endpoint `0xA1` 外层帧。Core 可以经任意普通中继到 H2，但中继只保存 H2 的 Core Route；多 Gateway 选择、Gateway 邻接通告和逐簇转发链仍属于 C07。

### 3.2 C06.2 当前不做什么

- 不拦截或改变既有 `ucn_node_send()` / `ucn_node_send_endpoint()`；调用这些 API 仍是“按最终 Node ID 的普通 Core 路由”。跨簇业务必须显式调用 `ucn_cluster_federation_send()`。
- 不做无配置的全网 Head/目录发现，不做全网广播式目录泛洪。
- 不做透明大消息隧道。C06.3 只处理一个外层 Frame 可容纳的单帧业务；C06.4 才可处理 Transfer/ACK/重组绑定。
- 不把 Head-to-Head 跳看成“零延时”或“自动负载均衡”。多出口 Cost、多个 Gateway 与故障域隔离由后续任务完成。
- 不用 C06.2 的有限目录表宣称 1k/10k 数据面已经实现。C08 必须在多级目录和实际表上界出现后再做规模门禁。

## 4. 标识、目录与固定表

### 4.1 Locator

目录中的一个租约记录为：

| 字段 | 宽度 | 含义 |
| --- | ---: | --- |
| `node_id` | 32 bit | 目标成员的规范 UCN Node ID；不是短地址。 |
| `cluster_id` | 32 bit | 该成员当前所属 Cluster；当前 Cluster 实现中由 Head 候选的 Node ID 生成。 |
| `head_node_id` | 32 bit | 可接收 Head-to-Head Tunnel 的当前 Head。 |
| `term` | 32 bit | Cluster Term；Head 切换后必须递增/重新注册。 |
| `lease_ms` / `expires_at_ms` | 32 bit | Directory 的绝对过期控制，使用权威单调时钟。 |
| `record_nonce` | 32 bit | 同一 `(node_id, head, term)` 的重放防护。 |

Directory 更新只能接受已授权 Head 的保护控制帧，且 `term/record_nonce` 必须单调前进。Head 失去成员、成员 Lease 到期、Head Stepdown 或显式 Withdraw 时必须撤销/缩短记录；Directory 不得因为一条延迟 Register 让旧 Head 重新获得成员所有权。

### 4.2 固定资源默认值

| 宏（计划） | 默认 | 所属节点 | 用途 |
| --- | ---: | --- | --- |
| `UCN_CLUSTER_FED_MAX_DIRECTORY_AUTHORITIES` | 2 | 所有 Federation 实例 | 固定目录副本 ID。 |
| `UCN_CLUSTER_FED_MAX_LOCAL_LOCATORS` | 17 | Head | Head 本身加最多 16 个当前成员的发布快照；必须不小于产品 Head 容量加 1。 |
| `UCN_CLUSTER_FED_MAX_DIRECTORY_RECORDS` | 32 | Authority | 可托管的远端/本地 Locator 租约。 |
| `UCN_CLUSTER_FED_MAX_LOCATOR_CACHE` | 16 | Head | 已解析远端成员的有限期 Cache。 |
| `UCN_CLUSTER_FED_MAX_NEXT_CLUSTERS` | 8 | Head | Cluster ID 到 Head Gateway 的聚合锚点。 |
| `UCN_CLUSTER_FED_MAX_PENDING` | 2 | Head | 未解析目录时的 Query 事务；业务帧不缓存，Directory 未命中会显式报错、应用随后重试。 |
| `UCN_CLUSTER_FED_MAX_SEEN_TRANSACTIONS` | 8 | Tunnel 参与节点 | `(Transaction, Origin, Final)` 固定去重和上游 Error 返回关联。 |
| `UCN_CLUSTER_FED_TRANSACTION_LEASE_MS` | 3000 ms | Tunnel 参与节点 | Seen 记录保持期；到期后槽才可复用。 |

所有 C06.2/C06.3 表都是调用者静态对象的一部分；无 `malloc`、无无限 Cache、无后台线程。产品可在用户配置头裁剪，但 `MAX_LOCAL_LOCATORS < UCN_CLUSTER_MAX_MEMBERS + 1` 会**编译失败**；无目录副本、重复副本、无时钟/发送回调或 Authority 不在自身副本列表时，启用 Runtime 的初始化会失败关闭；`MAX_PENDING=0` 或 `MAX_SEEN_TRANSACTIONS=0` 也会编译失败。当前 Windows x64 Debug 的 Full/Lite/Nano 均测得 `sizeof(ucn_cluster_federation_t)=2944 B`（该对象不随 Core Profile 自动裁剪，只有产品不实例化/缩小宏才不占用或减小）；这是 Host ABI 测量，不是 MCU RAM 承诺，C06.5 再报告目标 MCU ABI。

## 5. Endpoint 与消息格式

新增静态 Endpoint：

```c
#define UCN_CLUSTER_FEDERATION_ENDPOINT ((ucn_endpoint_t)0xA1U)
```

它复用 Core 的普通静态 Endpoint 机制，不新增 `ucn_message_type_t` 的 Core 控制消息，也不升级 `UCN_PROTOCOL_VERSION=5`。Endpoint Payload 的公共前缀固定为 8 B：

| 偏移 | 长度 | 字段 |
| ---: | ---: | --- |
| 0 | 1 | Federation Format Version（首版为 1） |
| 1 | 1 | Kind |
| 2 | 1 | Flags（未知位必须拒绝） |
| 3 | 1 | Federation Hop Limit |
| 4 | 4 | Transaction ID（非零） |

`Kind` 的 v1 预留如下：

| Kind | 名称 | 当前 Runtime | 用途 |
| ---: | --- | --- | --- |
| 1 | `LOCATOR_REGISTER` | 已实现 | Head 向 Directory 副本注册/续租成员 Locator。 |
| 2 | `LOCATOR_WITHDRAW` | 已实现 | Head 撤销 Locator。 |
| 3 | `LOCATOR_QUERY` | 已实现 | Head 查询目标 Node ID。 |
| 4 | `LOCATOR_REPLY` | 已实现 | Directory 返回当前 Locator。 |
| 5 | `TUNNEL_SUBMIT` | C06.3，显式启用 | 普通节点把已处理的单帧内层业务提交给本簇 Head。 |
| 6 | `TUNNEL_DATA` | C06.3，显式启用 | 本簇 Head 发送至目标 Head 的头到头外层帧。 |
| 7 | `TUNNEL_DELIVER` | C06.3，显式启用 | 目标 Head 向当前本簇最终成员交付。 |
| 8 | `TUNNEL_ERROR` | C06.2/C06.3 | Directory 向 Query Head 返回 Not Found；Tunnel 向 Origin 返回 TTL、Stale、MTU 或 Downstream。 |
| 9 | `NEXT_CLUSTER_ANNOUNCE` | 预留 | C07 的 Gateway 邻接与聚合状态。 |
| 10 | `NEXT_CLUSTER_WITHDRAW` | 预留 | C07 的撤销。 |

具体 v1 长度冻结为：`REGISTER/WITHDRAW=32 B`、`QUERY=20 B`、`REPLY=32 B`、`ERROR=20 B`；`SUBMIT=16 B+inner`、`DATA=28 B+inner`、`DELIVER=28 B+inner`。编码器必须精确比较长度、保留位、Node ID、Cluster ID、Term、Traffic Class、Endpoint 和 `inner_length`；不接受“至少这么长”的模糊解析。当前 9/10 是已编号但未实现的预留 Kind，解码器返回 `UCN_ERR_UNSUPPORTED`，不会把它们误当成格式损坏或可执行控制命令。

`TUNNEL_DATA` 固定头为 28 B：

| 偏移 | 长度 | 字段 |
| ---: | ---: | --- |
| 0..7 | 8 | 公共前缀 |
| 8 | 4 | Origin Node ID |
| 12 | 4 | Final Node ID |
| 16 | 4 | Origin Cluster ID |
| 20 | 4 | Destination Cluster ID |
| 24 | 1 | 最终业务 Endpoint |
| 25 | 1 | Q0/Q1 Traffic Class |
| 26 | 2 | Inner Payload Length |
| 28 | N | Inner Payload / 密文 |

`TUNNEL_DELIVER` 同样是 28 B 固定头：`Origin Node(4) + Final Node(4) + Origin Cluster(4) + Destination Cluster(4) + Endpoint(1) + Traffic Class(1) + Inner Length(2)`。它保留两个 Cluster ID，使最终 Member 能记录完整交付上下文；长度字段仍必须与真实 Payload 精确相等。

`Transaction ID` 与 `(origin_node_id, final_node_id)` 组成去重键。C06.3 在 H1、H2 和最终 Member 均维护有限期固定 Seen 表；每经过一个 Federation Head 都递减 Federation Hop Limit。为 0、Destination Cluster 不等于本 Head、最终 Member 已离开或已见相同事务时，必须产生有界 `TUNNEL_ERROR` 或拒绝重放，不能无限回送。C06.3 不实现第三个 Head 的继续转发；多 Gateway/逐簇链由 C07 决定。

## 6. 数据流程

### 6.1 注册与解析

```text
Member M  -- Cluster JOIN/Keepalive --> Head H2
Head H2  -- LOCATOR_REGISTER -------> Directory D1[, D2]

Origin A -- Federation Send ---------> local Head H1
H1       -- Locator Cache miss -----> Directory D1
D1       -- LOCATOR_REPLY ----------> H1  {C, cluster C2, head H2, term, lease}
H1       -- cache + NextCluster ----> {C2 -> H2}
```

Head 只为本机和当前 `ucn_cluster` 成员生成 Locator；普通成员不能代替 Head 直接注册。C06.2 的 Directory Query 只在 Head 发起，并受固定 Pending 容量约束。Authority 仅接受受保护且经产品 `authorize_head()` 认可的 Head；同一 Locator 的 Term/Nonce 必须单调，成员离开或本 Head 的 Term 变化先撤销旧记录。Cache 命中但租约已过期时一律视为未命中；不得为“先发再验证”绕过目录失效。

### 6.2 远端小消息交付

```text
A --TUNNEL_SUBMIT--> H1 --TUNNEL_DATA(destination=H2)--> [Core ordinary relays] --> H2
H2 --TUNNEL_DELIVER--> C
```

- A 仅需要到 H1 的本簇 Route；H1 仅需要到 H2 的簇级 Core Route；中继只保存 H2 Route；H2 仅需要到 C 的本簇 Route。
- H1 在 Directory 未命中时，最多占用一个固定 Pending Slot，并立即向 A 返回 `DIRECTORY_NOT_FOUND`；A 在 `find_locator()` 出现结果后以**新 Transaction**重试，C06.3 不在 Head 静态对象中缓存任意业务 Payload。
- H2 验证 Data 的 `destination_cluster_id` 等于当前 Cluster、外层来源经产品 Head ACL 接受，并验证 C 是自己现有 Member（或本机 Head）；任一条件不满足，H2 向 H1 返回 `DIRECTORY_STALE/TTL`。H1 只接受与 Seen 事务中预期远端 Head 对应的 Error，再转交 A。
- Endpoint 与 Traffic Class 是最终业务语义的一部分。Head 不得把 Q0 改成 Q1，也不得替换业务 Endpoint。

普通数据帧不会自动经过这一流程；调用方必须选择 `ucn_cluster_federation_send()`。成员 A 先在本机运行可选 `seal_inner()`，再把 `TUNNEL_SUBMIT` 发给当前 H1；H1/H2 不打开内层 Payload，只转交；最终 C 的 `open_inner()` 成功后才调用 `deliver()`。这保持既有 API 行为、便于渐进迁移，也避免一个 Cluster 尚未稳定时误劫持本簇直连业务。

## 7. MTU、Wire Class 与大消息

C06.3 的单帧 Tunnel 可发送条件为：

```text
inner_length + Tunnel固定头(28 B) + Core Frame Header/Auth Tag
    <= 每一段实际外层路由的最小 MTU
```

产品 Send 回调（通常包装 `ucn_node_send_endpoint()`）的最终编码/链路检查仍是唯一真实判定；C06.3 还把 Inner Payload 限制为 `UCN_MAX_PAYLOAD_BYTES - 28`，任何一段返回 `UCN_ERR_TOO_LARGE` 都以 `TUNNEL_ERROR(MTU)` 显式返回。禁止静默截断、禁止把不足 MTU 的 Frame 伪装成可达。

Core 外层 Wire Profile 将继续按 Head ID 和实际外层路径选择；Tunnel 载荷中的 Node/Cluster ID 始终按规范 32 bit 编码，不因外层 W0/W1/W2/W3 而截断。C06.3 不跨 Network ID 工作。当前也不会在 Directory Query 尚未完成时保留业务数据；应用应等待/观察 Locator 后重试。

大于一帧的消息需要 C06.4 把既有 `ucn_transfer` 的 Fragment/ACK 放入 Tunnel 语义并保持端到端重组归最终成员：

1. 每个 Fragment 都带 Tunnel 逻辑头或等价不可分割的 Flow Binding；
2. 选择整个 Head-to-Head 路径最小 MTU 后再开始，不允许中途换到更窄路径并截断；
3. Tunnel Gateway 不重组完整 8 KiB 消息，只透明转片；
4. 目标成员仍用既有固定 RX Slot、CRC32、ACK、重试与显式 Release；
5. 任何 Gateway/目录切换必须等到 Transfer Flow 结束，或以明确 Error 让发送端重试，不能混接两个 Destination Cluster。

在 C06.4 完成前，跨簇 Transfer API 必须返回 `UCN_ERR_CONFIG`，不能错误地把本簇 Transfer 当作跨簇已支持。

## 8. 安全与授权边界

### 8.1 默认失败关闭

- Directory Register/Withdraw/Reply、Head-to-Head Tunnel、Head-to-Member Deliver 均必须通过 Endpoint 安全策略；开启 `require_protected_control` 时明文一律拒绝。
- 除保护位外，Directory Authority 必须调用产品 `authorize_head()`；仅知道一个 Node ID 不能获得写目录权。
- H1 接收 Submit 时必须验证外层来源 Node ID 属于自己的现任 Member 或本机 Head；H2 交付时必须验证 Locator 指向当前 Cluster/Head/Term。
- Transaction ID、Head Term、Record Nonce、租约和固定 Seen 表共同抵御重放；固定表满时拒绝新事务，不能覆盖仍在执行的事务。

### 8.2 必须区分“外层受保护”与真正端到端

Core 对 `H1 -> H2` 的外层 Endpoint 帧可以提供 H1 到 H2 的保护，并允许普通 Core 中继透明转发；但 A→C 的 Submit/Deliver 分别以 A→H1、H2→C 为外层，**这不等价于 A 到 C 的业务端到端保密/认证**。

启用 `enable_tunnel=true` 后，Federation 默认安全模式为 `INNER_E2E_REQUIRED`：产品未提供成对 `seal_inner/open_inner` Provider 和 `deliver` 回调时初始化失败关闭。仅为台架诊断可以显式选择 `PROTECTED_OUTER_ONLY`，其风险必须在产品配置中可见；该模式不准标为端到端安全。C06.3 的内层安全 AAD 绑定 `Origin/Final Node ID`、业务 Endpoint、Traffic Class、Transaction ID 与 Origin Cluster ID。因为成员 A 在 Head 查询到目标 Locator 前就必须完成内层保护，Destination Cluster 在 C06.3 中由 H2/C 逐跳验证并传给交付回调，**不作为可由 A 预先绑定的安全 AAD**；C06.4 若引入提前 Locator 协商，才可升级为 Locator-bound AAD。

生产身份、密钥轮换和可信 Directory 的密码学质量仍依赖 S02；C06 只定义“无 Provider 时失败关闭”的协议接口，不虚构密码实现。

## 9. 生命周期、故障与错误语义

| 事件 | C06.2/C06.3 行为 |
| --- | --- |
| 成员离开/Head 删除成员 | Head 向 Directory Withdraw；未撤销的记录也在 Lease 到期后失效。 |
| Head 失效/Term 变化 | 新 Head 重新注册；旧 Term Register/Deliver 被拒绝；缓存删除后重查。 |
| Directory 副本不可达或 Query 超时 | 发送立即失败时同次 Query 轮询下一副本；已发送但超时则在 Owner 的下一次 `step()` 尝试尚未查询的固定副本。全部失败时只影响新跨簇解析，现有本簇通信保持。 |
| H1→H2 Core Route 断开 | H1 向 Origin 返回 `DOWNSTREAM`；当前不自动删除 Cache，也不把同一 Transaction 回退为直接寻 C。 |
| H2 不再拥有目标成员 | H2 返回 `DIRECTORY_STALE`，H1 只转交 Origin；应用可重新 Query 后以新 Transaction 重试。 |
| Federation Hop/重复门禁 | H1/H2 对 TTL 发 `TTL` Error；固定 Seen 表拒绝重复 `(Transaction, Origin, Final)`，C06.3 不继续向第三个 Head 转发。 |
| Head 降级为 Member | C06.2 立即停止观察为发布成员并尝试撤销旧 Locator；C06.3 不保留在途业务 Payload。 |

当前 C06.2/C06.3 只以固定表、Seen Lease、一个 Locator/Owner Step 的节流限制控制量；独立 Cluster/Federation Token Bucket、备用 Head 与目录副本接管归 C07，不能把控制风暴问题留给普通 Core Q1 队列“自然解决”。

## 10. 公共 API 与产品接入

已新增独立头 `include/ucn/ucn_cluster_federation.h`，不把内容继续堆进 `ucn_cluster.h`。C06.3 当前入口为：

```c
ucn_result_t ucn_cluster_federation_init(
    ucn_cluster_federation_t *federation,
    const ucn_cluster_federation_config_t *config);

ucn_result_t ucn_cluster_federation_receive(
    ucn_cluster_federation_t *federation,
    ucn_node_id_t outer_source,
    bool protected_outer,
    const uint8_t *payload,
    size_t payload_length);

ucn_result_t ucn_cluster_federation_step(
    ucn_cluster_federation_t *federation);

ucn_result_t ucn_cluster_federation_query_locator(
    ucn_cluster_federation_t *federation,
    ucn_node_id_t target_node_id);

ucn_result_t ucn_cluster_federation_send(
    ucn_cluster_federation_t *federation,
    ucn_node_id_t final_node_id,
    ucn_endpoint_t endpoint,
    ucn_traffic_class_t traffic_class,
    const uint8_t *payload,
    uint16_t payload_length);
```

`ucn_cluster_federation_find_locator()` 与 `ucn_cluster_federation_find_next_cluster()` 用于读取非过期异步结果。`ucn_cluster_federation_send()` 只在 `enable_tunnel=true`、本机是当前 Member/Head 且 H1 已有有效目标 Locator 时提交一帧业务；Member 成功仅表示 Submit 已交给 Core，最终失败通过可选 `on_error()` 异步报告。Cache 未命中时 H1 启动有界 Query 并立即报告 `DIRECTORY_NOT_FOUND`，调用方在解析完成后以新 Transaction 重试。

配置结构只接收：本机 Node ID、只读 `ucn_cluster_t *`、权威单调时钟、向指定 Head 发送 Endpoint **和 Traffic Class** 的回调、固定 Directory Authority 列表、授权回调、可选内层安全 Provider、最终 `deliver()`/`on_error()` 回调、表容量与显式安全模式。它不接收 GPIO、UART/CAN/Wi-Fi Handle、RTOS Task Handle 或动态内存分配器。

产品 Owner 流程：

```text
Node/Adapter Pump
  -> ucn_cluster_sync_node_neighbors()
  -> Endpoint 0xA0: ucn_cluster_receive()
  -> ucn_cluster_step()
  -> Endpoint 0xA1: ucn_cluster_federation_receive()
  -> ucn_cluster_federation_step()
  -> ucn_node_step()/Transfer/Service
```

`receive()`、`step()`、Cluster 状态同步和所有回调必须由同一个 Protocol Owner 串行执行；ISR 只写 Driver Ring/通知 Owner，不能直接访问目录或隧道表。

## 11. 分阶段实施任务与验收

| 子项 | 实现 | 必须通过的验证 | 完成边界 |
| --- | --- | --- | --- |
| C06.0 | 本文、格式/安全/资源/错误冻结；任务表建立。 | 与 `ucn_cluster`、Core Forward、Endpoint、Transfer 现状逐项核对。 | **已完成设计**，没有代码能力。 |
| C06.1 | 新增独立 Federation Archive、`0xA1` Codec、格式负向测试、Cluster View/成员只读 API；**暂不分配 Federation Directory/事务对象**。 | Full/Lite/Nano 公共头和链接；Codec Golden、坏长度/版本/保留位/Node ID/TTL。 | 不发送业务、Wire v5 不变。 |
| C06.2 | Head 本地 Locator 发布、Authority Register/Withdraw、双副本查询、租约/Term/Nonce、有限 Cache 和 Next-Cluster 直达锚点。 | 过期/旧 Term/重复/表满/副本发送失败与 Query 超时切换/无目录失败关闭；Host 两簇解析模拟。 | **已完成当前软件闭环**：Directory 仅处理 Locator/Query/Reply/Not Found；普通业务 Tunnel 仍不交付。 |
| C06.3 | Submit/Data/Deliver/Error 的小消息数据面、成员/Head 授权、Seen/TTL、Q0/Q1 不改写、内层安全 Provider 门禁。 | A→H1→H2→C、目录陈旧、H2 断链、来源伪造、循环、权限/安全缺失；中继没有 C 的 Route。 | **已完成 Host 软件闭环**：必须显式 `enable_tunnel`，默认要求 `seal_inner/open_inner/deliver`；Q0 模拟三段均保持 Q0，伪造、重放、Stale、Downstream、TTL 均有门禁。单帧负载，Transfer 留待 C06.4。 |
| C06.4 | Tunnel + Transfer 的 MTU/分片/ACK 绑定。 | 32 B～8 KiB、不同 MTU、窄路径拒绝、断链、缓存切换、无 Gateway 重组。 | 不新增动态内存，不破坏既有本簇 Transfer。 |
| C06.5 | Host Federation 模拟、资源报告与多 Profile/裁剪门禁。 | 两/三簇、目录副本失效、Head 故障、控制限速、Cache 上界、ASan/UBSan/Analyzer。 | 只给 Host 证据，不能代替板测。 |
| C06.6 | MCU 验收与文档/知识库同步。 | 至少四板两簇：A→本簇 Head→远簇 Head→D；注册、缓存命中/失效、目录/Head 故障、RAM/CPU/Heap/时延/丢包。 | 无线、多 Gateway、长稳和功耗继续 C07/C08。 |

## 12. C07/C08 的前置关系

- C07 的 Backup Head、Gateway 容量、邻接簇发现、目录副本接管、独立 Token Bucket 与分区合并，都必须建立在 C06 的 Locator/Tunnel 错误语义之上。
- C08 的两级目录与 1k/10k 验证，必须使用实际的 C06/C07 固定表、控制速率和媒体模型；不允许拿当前 64/256/1000 个隔离单层 Cluster 模拟替代。
- 在 C06.4 完成前，当前 UCN 的跨簇大消息、自动目录和万级数据面均为**未实现**。
