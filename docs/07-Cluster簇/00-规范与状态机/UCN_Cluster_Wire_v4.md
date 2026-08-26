# UCN Cluster Wire v4 RFC

> 文档编号：`UCN-CL-WIRE-4-RFC4`  
> 状态：**FROZEN；外部冻结复审 GO（仅 CLV2-05-01 / RFC 范围）**  
> 日期：2026-08-21  
> 适用分支：`codex/v5-adaptive-wire`  
> 实现状态（2026-08-25）：**RFC4 已冻结；strict codec、semantic builder/parser、dual dispatcher、capability/diagnostic、Carrier 40 B 与 fuzz 门禁已在隔离 archive 完成。encoder 默认关闭，生产 Cluster RX/TX/FSM/Authority 仍未接线，M05 顶层继续 `AUDIT HOLD`。**

> 修订说明：RFC1 的 Handover 仅绑定了旧 Epoch、`HANDOVER_READY` 错置为 Backup，且单 32-bit Takeover bitmap 无法覆盖 `MAX_MEMBERS + 1` 与 Joint Config；RFC2 仍未定义 `HEAD_TAKEOVER.P2` 的 Joint 语义，且误把 Handover 限制为跨 Cluster；RFC3 又使同簇目标 Backup 以未持久化的 `HEAD` 角色发送 READY，并让未收到 READY 的成员错误承担 READY 匹配责任。RFC4 使 Type 27 角色随 Handover 模式严格分派，并分离 A、B、成员三方的 READY/Stepdown 验证责任。外部冻结复审已于 2026-08-21 对 RFC/05-01 签署 GO；RFC1/RFC2/RFC3 不得作为实现依据。

## 1. 目的与边界

Cluster Wire v4 为后续 Joint Config、可信 Takeover、Handover 和 Rekey 预留完整且有界的控制面表达能力。它只定义 Cluster 控制消息的载荷格式；Core UCN Frame 的 W0–W3、路由、Link、Adapter 和安全封装均不在本 RFC 中修改。

当前源码的 Cluster Format v3 为固定 **32 B**。v4 定义固定 **40 B** 格式，且 Cluster Format 与 `UCN_PROTOCOL_VERSION` 完全独立：升级 v4 不得修改 Core 协议版本。

除非后续 RFC 显式定义，否则本文中的“必须/不得”均为规范性要求。任何已冻结的 v4 字节语义若需改变，必须使用新的 Cluster Format 版本，不能复用 v4。

## 2. 术语与通用规则

- `Epoch`：`(cluster_id, term, head_node_id)`。
- `P0`…`P5`：类型专属的六个网络序 `uint32_t` 载荷字段。
- `Node ID`、Cluster ID、Term、Generation、Transaction ID、Nonce 等均为无符号 32 位值。
- 所有多字节数值使用 **big-endian**（network byte order）。
- 外层 UCN 源/目的地址负责报文的实际发送者和接收者；Cluster Wire 不重复编码 destination。
- v4 不提供认证或加密。解析成功不等于获得 Authority；认证、源准入、replay admission 和 FSM 授权必须仍在 Codec 之后执行。
- 受 M03 no-wrap 约束的 serial 必须使用既有 checked-serial 规则；不得以 v4 为理由重新允许回绕。

## 3. 固定 40 B 布局

| 偏移 | 长度 | 字段 | 规则 |
|---:|---:|---|---|
| 0 | 1 | `cluster_format` | 固定 `4`。 |
| 1 | 1 | `message_type` | 本 RFC 的 Type 编号。 |
| 2 | 1 | `sender_role` | 发送方声明的 Cluster Role；必须满足该 Type 的角色表。 |
| 3 | 1 | `flags` | Type 专属；未定义位必须为零。 |
| 4 | 4 | `cluster_id` | 非零、非 broadcast。 |
| 8 | 4 | `term` | 非零、合法 serial 域。 |
| 12 | 4 | `head_node_id` | 此消息绑定的 Epoch Head ID；非零、非 broadcast。 |
| 16 | 4 | `P0` | Type 专属，网络序。 |
| 20 | 4 | `P1` | Type 专属，网络序。 |
| 24 | 4 | `P2` | Type 专属，网络序。 |
| 28 | 4 | `P3` | Type 专属，网络序。 |
| 32 | 4 | `P4` | Type 专属，网络序。 |
| 36 | 4 | `P5` | Type 专属，网络序。 |

Common Header 恰为前 16 B，类型载荷为后 24 B。编码器必须先将完整 40 B 清零，再按本表写入；解码器必须在成功前检查所有要求为零的字段。禁止按 C struct 内存布局直接收发。

`cluster_id`、`term`、`head_node_id` 在所有已定义 Type 中都必须有效。新建 Cluster、Recovery、Rekey 等使用新 identity 的流程仍须把其正在声明或绑定的 Epoch 完整填入 Common Header；不得以全零 Header 表示“未加入”。

## 4. Type 编号与不可复用规则

Type `0` 非法；未知 Type、被废弃 Type、未定义 Type `34..255` 一律 fail-closed。v3 已存在 Type 的编号在 v4 中保留，但其 v4 载荷语义只由本文定义，不能用 v3 decoder 解释。

| 编号 | 名称 | v4 状态 |
|---:|---|---|
| 1..19 | `ADVERTISE` 至 `BACKUP_REJECT` | 保留既有控制语义并补齐 v4 绑定字段。 |
| 20 | `CONFIG_BEGIN` | 新增，M07 使用。 |
| 21 | `CONFIG_MEMBER` | 新增，M07 使用。 |
| 22 | `CONFIG_PREPARE` | 新增，M07 使用。 |
| 23 | `CONFIG_ACK` | 新增，M07 使用。 |
| 24 | `CONFIG_COMMIT` | 新增，M07 使用。 |
| 25 | `CONFIG_ABORT` | 新增，M07 使用。 |
| 26 | `HANDOVER_PREPARE` | 新增，M11 使用。 |
| 27 | `HANDOVER_READY` | 新增，M11 使用。 |
| 28 | `HANDOVER_COMMIT` | 新增，M11 使用。 |
| 29 | `HEAD_WITHDRAW` | 新增，M11 使用。 |
| 30 | `REKEY_PREPARE` | 新增，M13 使用。 |
| 31 | `REKEY_ACK` | 新增，M13 使用。 |
| 32 | `REKEY_COMMIT` | 新增，M13 使用。 |
| 33 | `TAKEOVER_CERTIFICATE` | 新增，M10 使用；携带可分片 Vote Certificate。 |

Type 编号只增不复用。未来 Type 必须从 34 起分配；若现有 Type 缺少字段，必须升级 Cluster Format，而不是改变该 Type 在 v4 中的字段含义。

## 5. Flags 与 Capability

除下表所列 Type 外，`flags` 必须为 `0`。同一 Type 的多个 flag 不得组合。

| Type | 合法 `flags` | 含义 |
|---|---:|---|
| `BACKUP_MEMBER_SYNC` (12) | `0x00` | 完整 Snapshot 的成员记录。 |
|  | `0x01` | `SNAPSHOT_BEGIN` marker；`P3..P5` 必须全零。 |
|  | `0x02` | `SNAPSHOT_END` marker；`P3..P5` 必须全零。 |
|  | `0x04` | `SNAPSHOT_DELTA` 的成员记录。 |
| `CONFIG_MEMBER` (21) | `0x10` | 将成员加入 proposal。 |
|  | `0x20` | 将成员从 proposal 移除。 |
| `TAKEOVER_CERTIFICATE` (33) | `0x40` | `CERT_SET_OLD`：Stable Config，或 Joint Config 的 `C_old` voter set。 |
|  | `0x80` | `CERT_SET_NEW`：仅 Joint Config 的 `C_new` voter set。 |

为同时携带 format range 与 capability，v4 使用 32 位 `wire_offer`：bits `31..24` 为 minimum format、bits `23..16` 为 maximum format、bits `15..0` 为 capability bitmap。`ADVERTISE.P3` 与 `JOIN_REQUEST.P1` 均为 `wire_offer`；`JOIN_ACCEPT.P4` 为 `selected_wire_offer`，bits `31..24` 必须为零、bits `23..16` 为选定的单一 format、bits `15..0` 为已选择集合。v4 已定义 capability 位如下；位 `6..15` 必须为零。

| 位 | 名称 | 声明含义 |
|---:|---|---|
| 0 | `BACKUP` | 节点可承担 Backup snapshot 角色。 |
| 1 | `TAKEOVER` | 节点可验证并参与 Takeover。 |
| 2 | `JOINT_CONFIG` | 节点支持 M07 Joint Config。 |
| 3 | `PERSISTENCE` | 节点存在满足 REQUIRED 合同的持久化能力。 |
| 4 | `RECOVERY_LINEAGE` | 节点支持 M12 Recovery lineage。 |
| 5 | `REKEY` | 节点支持 M13 Rekey。 |

v4 节点必须在 `wire_offer` 中声明一个包含 `4` 的闭区间。`JOIN_ACCEPT` 选定的 format 必须在双方 range 的交集内，且 selected capability 必须是双方 capability 的子集。

Capability 只是解码后续的资格输入，不是信任凭据：严格 v4 Cluster 的 Head/Backup/voter 准入和 v3 降级规则由 M05-07/08 实现。

## 6. Type 专属 Payload 合同

除表中明确使用的字段外，所有 `P` 字段必须为零。表中的 `score_capacity` 表示高 16 位 score、低 16 位 available capacity；`ordinal_count` 表示高 16 位 ordinal、低 16 位 total count。

| Type | 合法 `sender_role` | `P0..P5`（按顺序） |
|---|---|---|
| 1 `ADVERTISE` | CANDIDATE、HEAD | `score_capacity`, `lease_ms`, `advertise_nonce`, `wire_offer`, `0`, `0` |
| 2 `JOIN_REQUEST` | JOIN_PENDING | join transaction ID, `wire_offer`, current config ID, boot incarnation, `score_capacity`, join nonce |
| 3 `JOIN_ACCEPT` | HEAD、RECOVERY_HEAD | join transaction ID, target config ID, lease ms, member flags, `selected_wire_offer`, member nonce |
| 4 `JOIN_REJECT` | HEAD、RECOVERY_HEAD | `join_nonce`, reject reason, `retry_after_ms`, `0`, `0`, `0` |
| 5 `KEEPALIVE` | MEMBER | `lease_ms`, `keepalive_nonce`, `0`, `0`, `0`, `0` |
| 6 `LEAVE` | MEMBER | `leave_nonce`, leave reason, `0`, `0`, `0`, `0` |
| 7 `HEAD_DECLARE` | HEAD | `score_capacity`, `lease_ms`, `declare_nonce`, `wire_offer`, `0`, `0` |
| 8 `HEAD_TAKEOVER` | HEAD | backup generation, snapshot ID, certificate anchor Config ID, takeover transaction ID, required certificate-set mask, certificate CRC32 |
| 9 `HEAD_STEPDOWN` | HEAD | handover transaction ID, target Cluster ID, target Term, target Head ID, stepdown nonce, `0` |
| 10 `BACKUP_ASSIGN` | HEAD | backup generation, selected Backup ID, sync token, config ID, config hash, `0` |
| 11 `BACKUP_READY` | BACKUP | backup generation, snapshot ID, final membership sequence, config ID, config hash, ready nonce |
| 12 `BACKUP_MEMBER_SYNC` | HEAD | backup generation, snapshot ID, membership sequence, member ID, member nonce, member lease ms |
| 13 `PRIMARY_HEARTBEAT` | HEAD | backup generation, config ID, snapshot ID, membership sequence, lease ms, heartbeat nonce |
| 14 `TAKEOVER_PREPARE` | BACKUP | backup generation, snapshot ID, config ID, proposed Term, takeover transaction ID, challenge nonce |
| 15 `TAKEOVER_ACK` | MEMBER | backup generation, snapshot ID, config ID, proposed Term, takeover transaction ID, member nonce |
| 16 `RECOVERY_DECLARE` | RECOVERY_HEAD | parent Cluster ID, parent Term, parent Config ID, recovery round, recovery nonce, recovery TTL ms |
| 17 `RECOVERY_ACK` | MEMBER | recovery nonce, member nonce, `0`, `0`, `0`, `0` |
| 18 `BACKUP_RESYNC_REQ` | BACKUP | backup generation, snapshot ID, expected membership sequence, request nonce, `0`, `0` |
| 19 `BACKUP_REJECT` | BACKUP | backup generation, config ID, reject reason, reject nonce, `0`, `0` |
| 20 `CONFIG_BEGIN` | HEAD | config transaction ID, old config ID, proposed config ID, old config hash, proposed config hash, proposal nonce |
| 21 `CONFIG_MEMBER` | HEAD | config transaction ID, target config ID, member ID, member nonce, member capability bitmap, `ordinal_count` |
| 22 `CONFIG_PREPARE` | HEAD | proposed config ID, old config hash, proposed config hash, old/new voter count packed, config transaction ID, prepare nonce |
| 23 `CONFIG_ACK` | MEMBER、BACKUP | proposed config ID, config transaction ID, voter slot, config phase, persistence generation, ACK nonce |
| 24 `CONFIG_COMMIT` | HEAD | committed config ID, config transaction ID, committed config hash, committed voter count, commit nonce, `0` |
| 25 `CONFIG_ABORT` | HEAD | config transaction ID, old config ID, aborted proposal ID, abort reason, abort nonce, `0` |
| 26 `HANDOVER_PREPARE` | HEAD | handover transaction ID, target Cluster ID, target Term, target Head ID, target config ID, target config hash |
| 27 `HANDOVER_READY` | 跨 Cluster Merge：HEAD；同 Cluster Planned Transfer：BACKUP | handover transaction ID, target Cluster ID, target Term, target Head ID, target config ID, target config hash |
| 28 `HANDOVER_COMMIT` | HEAD | handover transaction ID, target Cluster ID, target Term, target Head ID, target config ID, target config hash |
| 29 `HEAD_WITHDRAW` | HEAD | handover transaction ID, target Cluster ID, target Term, target Head ID, withdraw nonce, `0` |
| 30 `REKEY_PREPARE` | HEAD | successor Cluster ID, successor Term, rekey transaction ID, old config ID, successor config ID, rekey nonce |
| 31 `REKEY_ACK` | MEMBER、BACKUP | successor Cluster ID, successor Term, rekey transaction ID, successor config ID, persistence generation, member nonce |
| 32 `REKEY_COMMIT` | HEAD | successor Cluster ID, successor Term, rekey transaction ID, old config ID, successor config ID, commit nonce |
| 33 `TAKEOVER_CERTIFICATE` | HEAD | backup generation, snapshot ID, config ID, takeover transaction ID, fragment index/count packed, vote bitmap word |

Additional structural requirements:

1. `member_id`、selected Backup ID、successor Head ID 和 successor Cluster ID 必须为非零、非 broadcast ID。
2. `BACKUP_MEMBER_SYNC` marker 的 `P3..P5` 必须为零；普通/DELTA 成员记录的 member ID、nonce、lease 必须有效。
3. `HEAD_TAKEOVER` 本身不携带 vote bitmap；它只能引用已完整接收并验证的 Type 33 `TAKEOVER_CERTIFICATE`。证书的确定性排序、分片和 quorum 验证见 §6.2，不能按当前 RAM member 表顺序猜测。
4. Config、Handover、Rekey 的 transaction ID、config ID、generation 和 sequence 均须先经现有 serial/epoch validator；Codec 不得自行把零或回绕值归一化。
5. `REKEY_PREPARE/ACK/COMMIT` 的 successor Term 必须为 `1`，successor Cluster ID 必须不同于 Common Header 的旧 Cluster ID。v4 Rekey 的 successor Head 固定等于 Common Header 的旧 Head、也等于 outer source；若要更换 Head，必须先走 Handover，不能在 Rekey 中暗中换 Head。其余 Rekey/Tombstone 验证由 M13 FSM 和 Persistence 合同执行。

### 6.1 Handover 的双 Epoch 事务合同

对 Type 26..29，Common Header **固定编码 Losing Head A 的旧 Epoch**：

```text
old_epoch = (header.cluster_id, header.term, header.head_node_id)
target_epoch = (P1, P2, P3)
handover_txid = P0
```

`target_epoch` 的三个字段均必须为有效 ID/Term。Type 26/27/28 的 `P4/P5` 是 target Config 的 `(config_id, config_hash)`；同一 txid 的三个消息必须逐字节绑定同一 old Epoch、target Epoch 与 target Config。Type 9/29 的 `P4` 为单独 nonce，`P5=0`。

Handover 的模式由 old/target Cluster ID 唯一决定，不能由接收端猜测：

| 模式 | `target_cluster_id` | Term 规则 | Head / Config / Certificate 规则 |
|---|---|---|---|
| **跨 Cluster Merge Handover** | 必须不同于 old Cluster ID | 两个 Cluster 的 Term **不可比较**；只校验 target Term 合法，绝不以 `old_term ± 1` 推导。 | Type 27 必须由 target Head 以 `sender_role=HEAD` 发送；target Head 和 target Config 由 Type 26/27/28 的完整 target 字段精确绑定；这是 M11 Merge 路径。 |
| **同 Cluster Planned Leadership Transfer** | 必须等于 old Cluster ID | `target_term` 必须等于 `checked_next_term(old_term)`；不得回退、跳跃或回绕。 | target Head 必须不同于 old Head，且必须是 old frozen Config 中已确认、满足 M11 策略的可接管 Backup。Type 27 必须由该节点以 `sender_role=BACKUP` 发送；`P4/P5` 必须逐字节等于 old active/frozen Config 的 `(config_id, config_hash)`，不得借切换暗中变更 Config。此路径由 old authoritative Head 的完整 Handover 事务证明，**不得**混用 Type 8/33 Vote Certificate；若 old Authority 不可证明，则只能改走独立的 Type 8/33 Takeover 路径。 |

```text
A(old Head) -> B(target Head): HANDOVER_PREPARE
B(target Head) -> A(old Head): HANDOVER_READY
A: authority_active = false
A -> A members: HEAD_STEPDOWN(target Epoch)
A -> B: HANDOVER_COMMIT
A -> A members: HEAD_WITHDRAW (optional diagnostic/fence repeat)
```

角色和 outer source 的精确要求：

| Type | Common Header | outer source | receiver 必须验证 |
|---|---|---|---|
| 26 `HANDOVER_PREPARE` | old Epoch A | `old_epoch.head_node_id`（A） | A 当前具有可发起 Handover 的 Authority；target Epoch/Config 合法，且满足上表对应模式。 |
| 27 `HANDOVER_READY` | old Epoch A | `target_epoch.head_node_id`（B） | source 必须等于 `P3`；跨 Cluster 必须为 `HEAD`，同 Cluster 必须为 `BACKUP`；B 已验证 capacity、Wire capability、Config/Backup 接纳策略及上表对应模式。 |
| 9 `HEAD_STEPDOWN` | old Epoch A | A | **成员接收规则**：仅验证 old Authority、非零 txid/nonce、完整 target Epoch 及对应模式；成员不得要求见过 READY。跨 Cluster 不比较 Term；同 Cluster 必须 exact `checked_next_term(old_term)`、不同 target Head，且不得据此改变 Config。 |
| 28 `HANDOVER_COMMIT` | old Epoch A | A | **B 接收规则**：必须匹配本地已发送的 READY，以及相同的完整 old/target Epoch、txid/Config/模式；A 必须已撤销 `authority_active`。 |
| 29 `HEAD_WITHDRAW` | old Epoch A | A | 只作旧 Authority 的可诊断 fence；必须携带同一 target Epoch，不得取代 READY/COMMIT。 |

**A 的本地发送规则：** A 只能在已缓存并精确验证来自 B 的 READY 后发送 `HEAD_STEPDOWN`，并且必须先撤销 `authority_active`。READY 是 A/B 单播事务的一部分，成员不参与且不可见。

同 Cluster Planned Transfer 中，B 的 READY 只表示“作为 old Epoch Backup 已准备好接收 Commit”；它**没有 Authority**，不得在收到 Commit、成功持久化 target Epoch、重新读取并证明 durable state、且完成正式 Head 角色转换前发送 `ADVERTISE`、`HEAD_DECLARE`、`HEAD_TAKEOVER`、`PRIMARY_HEARTBEAT` 或任何 Head Authority 写入。任一步持久化/验证失败，B 保持 Backup 或进入安全失败路径，不得提升。

任何 Type 26..29 的 old/target Epoch、txid、target Config、sender role 或模式规则不匹配，均必须拒绝且不修改 Authority、Join 或 replay 状态。`HANDOVER_READY` 在跨 Cluster 是 Head 消息、在同 Cluster 是 Backup 消息。同 Cluster Planned Transfer 的 `HEAD_STEPDOWN` 只在接收方已由自身当前 Config 验证 old Epoch、`target_term=checked_next_term(old_term)` 和不同 target Head 时，才允许开始 Join target；它不能单独构成 Config、Certificate 或 Authority 变更。

### 6.2 Takeover Certificate：分片、Joint Config 与本地验证

v4 的完整 Vote Certificate 使用一个 Type 8 `HEAD_TAKEOVER` announcement 与零个或多个 Type 33 fragment 组成；单个 32-bit bitmap 不是完整证书。Common Header 对两类消息都编码 proposed new Epoch：

```text
proposed_epoch = (cluster_id, proposed_term, proposed_head_id)
```

其中 `proposed_head_id` 等于 outer source，且必须是原 Backup。旧 Term 由接收方的精确旧 Epoch 与 `checked_next_term(old_term)` 共同验证，不能仅按数值减一猜测。

`HEAD_TAKEOVER` 的 `P0..P5` 为 `{backup_generation, snapshot_id, certificate_anchor_config_id, takeover_txid, required_set_mask, certificate_crc32}`。`certificate_anchor_config_id` 是 Type 8 对完整 Certificate 的唯一 Config 锚点，必须非零，且规则固定如下：

- Stable Config（`required_set_mask=0x01`）：anchor 必须等于唯一 `C_old` fragment set 的 `config_id`。
- Joint Config（`required_set_mask=0x03`）：anchor 必须等于 `C_new` fragment set 的 `config_id`；`C_old` 仍由 `CERT_SET_OLD` fragment 明确携带，两个 set 都必须通过 quorum。

因此 `P2` 不是“任意 primary config”或实现自行选择的当前 Config。它必须与对应 frozen Config 的 `config_id`、Type 33 的 `P2` 和 Canonical Certificate 同时精确匹配。`required_set_mask` 只允许：`0x01=stable/C_old`，或 `0x03=C_old + C_new`；Stable Config 必须为 `0x01`，Joint Config 必须为 `0x03`。

每个 Type 33 fragment 使用 `CERT_SET_OLD (0x40)` 或 `CERT_SET_NEW (0x80)`，不得组合：

```text
P0 = backup_generation
P1 = snapshot_id
P2 = config_id for this voter set
P3 = takeover_txid
P4 = fragment_descriptor = (fragment_index << 16) | fragment_count
P5 = vote_bitmap_word
```

Voter order 必须是该 Config 中包含 Head 的完整 voter set 按 Node ID 升序排列。第 `i` 个 fragment 的 bitmap word 覆盖 slots `[32*i, 32*i+31]`，最低位表示最小 slot；超出该 voter set `count` 的 bit 必须为零。fragment count 必须精确等于 `ceil(voter_count / 32)`，因此当前 `UCN_CLUSTER_MAX_MEMBERS=32` 时，最大 `MAX_MEMBERS + 1 = 33` voter 的证书需要两个 fragment；Joint Config 最多需要 old/new 各自的完整 fragment 集。

接收端只能在下列条件全部满足后接受 `HEAD_TAKEOVER` 并进入新 Head 处理：

1. 已收齐 required set mask 中每个 Config 的全部 fragment，且重复 fragment 的字段与 bitmap 完全相同；冲突重复、缺片、越界 index/count 一律拒绝整个 Certificate。
2. `HEAD_TAKEOVER.P2` 必须满足上文的 anchor 规则：Stable 等于 `C_old.config_id`；Joint 等于 `C_new.config_id`。每个 Config ID 都能在本地 frozen ConfigState 中找到相同的 config hash 与确定性 voter order；Stable 使用一个 quorum，Joint 同时验证 `quorum(C_old)` 和 `quorum(C_new)`。
3. 每一置位票的 Node ID 都属于对应 voter set，且对应持久 VoteId 精确绑定 `{cluster_id, old_term, proposed_term, config_id, backup_node_id=proposed_head_id, backup_generation, snapshot_id}`。
4. `certificate_crc32` 必须等于对完整 Canonical Certificate 的 CRC-32/ISO-HDLC（init/final xor `0xFFFFFFFF`，reflected polynomial `0xEDB88320`）结果。Canonical 输入依次为无 NUL 的固定 ASCII domain `UCN-CL4-TAKEOVER-CERT`、proposed Epoch（三个 u32）、backup generation、snapshot ID、takeover txid、**certificate anchor Config ID（Type 8 `P2`）**、required set mask（均为 u32），以及按 OLD 后 NEW 顺序排列的 `{set_flag(u8), config_id(u32), config_hash(u32), voter_count(u32), fragment_count(u32), bitmap_words[0..n-1] (each u32)}`；所有 u32 均为 big-endian。

CRC32 只用于确定性重组指纹，不是认证机制；票据真实性仍来自链路/帧安全、source 准入和持久 VoteId。

#### Certificate-pending cache 的固定资源合同

M05-02 的每个 `ucn_cluster_t` **必须只有一个**固定大小的 Certificate-pending slot（规范名 `UCN_CLUSTER_TAKEOVER_PENDING_MAX=1`）；不得动态分配，也不得按发现到的 Head 数量扩张。slot key 为：

```text
(proposed_epoch, backup_generation, snapshot_id,
 certificate_anchor_config_id, takeover_txid,
 required_set_mask, certificate_crc32)
```

只有已通过 Type/role/source/字段结构门**及 frozen Config set 准入**的 Type 8 才能创建 slot；先到的 Type 33 不缓存，直接丢弃。M05-02 的隔离 helper 不拥有 RX gate 或 ConfigState，因此 `pending_begin()` 与 `pending_accept_fragment()` 都必须携带由 caller 在 RX gate 后构造的 `certificate_admission`：outer source、已准入标志、`C_old`、可选 `C_new` 及 frozen-Config 已准入标志均不可来自 wire。helper 自身再强制 `outer_source == header.head_node_id`、Stable 时 `C_old == Type8.P2`、Joint 时 `C_new == Type8.P2`，以及 Type 33 OLD/NEW fragment 分别匹配 `C_old/C_new`；任一 admission/source/Config 不匹配只拒绝，**不得占用、替换、清空或延长**现有 slot，**即使 `now_ms == deadline_ms` 也一样**。隔离 helper 的 RX convenience path 必须先完成这些非破坏性校验，才允许 lazy expiry；未来 RX owner 必须由自己的 timer 调用 `pending_expire()` 做时间驱动回收，不能让未准入帧兼作回收触发器。slot 存在时，只有完全相同 key 且 admission context 相同的 Type 8，和与其 `backup_generation/snapshot_id/takeover_txid`、frozen Config set 相匹配的 Type 33 才能写入。重复的相同 fragment 幂等；已通过 admission 的同 index 不同内容、CRC/Config/bitmap 冲突、越界 index/count 才立即清空该 slot 并拒绝该 Certificate。

slot 已满时，不同 key 的 Type 8 或任何不属于当前 key 的 Type 33 **一律拒绝且不替换、不淘汰、不延长现有 slot deadline**。slot 仅在四种情形释放：完整验证成功、完整验证失败、local active old Epoch 变化、或从创建时刻起满固定 `1000 ms` 的 Certificate-pending timeout。相同 key 的 announcement 重传不延长该 `1000 ms` deadline。Head 必须在此窗口内周期性重发完整 fragment 集与 announcement；超时、校验失败或资源满载都不得默认相信 Head，也不得产生 Authority 副作用。

### 6.3 统一字段合法域

下列规则是 v4 decoder 的最小结构门；FSM 可以在此基础上继续拒绝不符合当前 Phase、Epoch 或 quorum 的消息。

| 字段类别 | 合法域 |
|---|---|
| `*_nonce`、transaction ID、backup generation、snapshot ID、membership sequence、config ID、config hash | 必须非零，并通过所属 serial/transaction 的专属校验；仅 `JOIN_REQUEST.P2` 的 current config ID 可为零，表示请求者尚无 committed Config。 |
| `lease_ms`、TTL、deadline、retry-after | 必须是既有 `ucn_duration_is_valid()` 接受的非零 duration。 |
| `score_capacity` | score 不得超过 `UCN_CLUSTER_SCORE_MAX`；capacity 可为零。 |
| capability bitmap | 仅可使用第 0..5 位；其余位为零。 |
| `wire_offer` | min/max 均非零且 `min <= max`；v4 发件人必须包含 4；仅定义 capability 位 0..5。 |
| `selected_wire_offer` | bits `31..24=0`、selected format 非零且属于双方 range 交集、selected capability 是双方 bitmap 的子集。 |
| `ordinal_count` | `total` 为 `1..UCN_CLUSTER_MAX_MEMBERS`，`ordinal < total`。 |
| Type 33 vote bitmap word | 不得置位对应 Config voter count 之外的 slot；整套 Certificate 还须满足 §6.2 的完整 fragment/quorum/CRC32 合同。 |
| `JOIN_ACCEPT.member_flags` | 仅 bit 0 `PROVISIONAL` 可置位；v4 Join Accept 不得直接将节点标为 voter，M07 的 Config Commit 才能完成该提升。 |
| `CONFIG_ACK.config_phase` | 使用低 8 位：`1=PREPARE`、`2=JOINT`、`3=COMMIT`；高 24 位必须为零。 |
| reason code | 使用低 8 位；高 24 位必须为零，且只能是本节列出的值。 |

统一 reason code：`1=CAPACITY`、`2=UNSUPPORTED`、`3=EPOCH_CONFLICT`、`4=PERSISTENCE`、`5=TIMEOUT`、`6=POLICY`、`7=QUORUM`、`8=STALE`、`9=SERIAL_EXHAUSTED`、`10=ADMIN`。`0` 和 `11..255` 均为保留值；未识别 reason 必须拒绝，不得静默当作通用失败。

### 6.4 外层 Source 与 Common Header 的绑定

v4 Codec 完成结构校验后，Cluster RX gate 必须按下表在执行任何 replay/FSM 副作用前绑定外层 UCN source。对未列出的 Type，outer source 必须等于 Common Header 的 `head_node_id`。

| outer source 可不同于 `head_node_id` 的 Type | 允许发送角色 | 额外绑定 |
|---|---|---|
| `JOIN_REQUEST`、`KEEPALIVE`、`LEAVE` | JOIN_PENDING、MEMBER、MEMBER | source 是请求/成员身份；nonce 必须与该 source 的事务或 member record 对应。 |
| `BACKUP_READY`、`BACKUP_RESYNC_REQ`、`BACKUP_REJECT` | BACKUP | source 必须是当前或待验证的 Backup 身份，并绑定 backup generation。 |
| `TAKEOVER_PREPARE` | BACKUP | Header 仍指向旧 Primary Head；source 必须匹配已宣布 Backup 与 generation。 |
| `TAKEOVER_ACK`、`RECOVERY_ACK`、`CONFIG_ACK`、`REKEY_ACK` | MEMBER、MEMBER、MEMBER 或 BACKUP、MEMBER 或 BACKUP | source 的 member/backup 身份、nonce 与 Config/Backup Epoch 必须精确匹配。 |
| `HANDOVER_READY` | 跨 Cluster：HEAD；同 Cluster：BACKUP | Common Header 仍是 old Epoch；source 必须等于 P3 的 target Head ID，并匹配 Type 26 已缓存事务。target Cluster 不同则角色必须为 HEAD；target Cluster 相同则角色必须为 BACKUP，且 source 必须是该 old Config 中已确认的 eligible Backup。 |

因此 `HEAD_TAKEOVER` 的 outer source 必须等于 Common Header 的新 Head ID，且 `sender_role` 必须是 `HEAD`；它不是 Backup 身份下的消息。此约束与当前 v3 的 Head Takeover 发送/接收语义一致。

## 7. 严格解析、版本分派与兼容策略

M05-02 必须实现如下双分派，不能采用“先试一个 decoder、失败后猜另一个”的策略：

| 输入长度 | `byte[0]` | 处理 |
|---:|---:|---|
| 32 | 3 | 仅交给 v3 decoder。 |
| 40 | 4 | 仅交给 v4 decoder。 |
| 32 | 非 3 | `UCN_ERR_MALFORMED`。 |
| 40 | 非 4 | `UCN_ERR_MALFORMED`。 |
| 其他 | 任意 | `UCN_ERR_MALFORMED`。 |

v4 decoder 的顺序必须是：固定长度/版本 → Type → role → flags → Common Header → Type payload 的零值与域约束。任一步失败都不得修改 Cluster FSM、replay 状态、token 或持久化状态。

M05-12 前，v4 encoder 必须默认关闭；现有 v3 实机/测试行为不得变化。M05-08 才实现 mixed-version policy：Strict v4 Cluster 不允许 v3 节点担任 Head、Backup 或 voter；是否允许 v3 non-voting legacy member 必须由产品显式选择，不能静默降级。

## 8. 规范性 Golden Vector

以下 vector 是 M05-02 的字节级门禁输入；空格仅为可读性分隔。

### 8.1 ADVERTISE

```text
04 01 05 00  01 02 03 04  00 00 00 02  0A 0B 0C 0D
00 64 00 20  00 00 07 D0  01 02 03 04  03 04 00 0B
00 00 00 00  00 00 00 00
```

解码为：HEAD，Epoch `(0x01020304, 2, 0x0A0B0C0D)`，score `100`，capacity `32`，lease `2000 ms`，nonce `0x01020304`，`wire_offer=0x0304000B`（format range `[3,4]`，capability bitmap `0x000B`）。

### 8.2 BACKUP_MEMBER_SYNC DELTA

```text
04 0C 05 04  01 02 03 04  00 00 00 02  0A 0B 0C 0D
00 00 00 05  00 00 00 09  00 00 00 0B  00 00 00 20
AB CD 00 01  00 00 07 D0
```

解码为：HEAD 的 DELTA；backup generation `5`，snapshot `9`，sequence `11`，member `0x20`，member nonce `0xABCD0001`，member lease `2000 ms`。

### 8.3 REKEY_COMMIT

```text
04 20 05 00  01 02 03 04  00 00 00 07  0A 0B 0C 0D
11 22 33 44  00 00 00 01  00 00 00 21  00 00 00 09
00 00 00 0A  DE AD BE EF
```

解码为：HEAD 对旧 Epoch `(0x01020304, 7, 0x0A0B0C0D)` 的 `REKEY_COMMIT`；successor Cluster `0x11223344`、Term `1`、txid `33`、old/new Config ID 为 `9/10`、commit nonce `0xDEADBEEF`。successor Head 由 Common Header 的 Head（`0x0A0B0C0D`）继承。

### 8.4 HEAD_STEPDOWN：old/target 双 Epoch

```text
04 09 05 00  01 02 03 04  00 00 00 02  0A 0B 0C 0D
00 00 00 31  11 22 33 44  00 00 00 05  55 66 77 88
DE AD BE EF  00 00 00 00
```

Common Header 是 Losing Head A 的 old Epoch `(0x01020304, 2, 0x0A0B0C0D)`；`P0=txid 49`，target Epoch B 为 `(0x11223344, 5, 0x55667788)`，`P4=stepdown nonce`。没有 target Cluster ID、Term、Head 任何一个字段时，此 vector 必须拒绝。

### 8.5 HEAD_TAKEOVER 与单片 Certificate

下列示例采用 proposed Epoch `(0x01020304, 3, 0x0A0B0C0D)`、backup generation `7`、snapshot `9`、Stable Certificate anchor Config ID `15`、takeover txid `33`。旧/Stable Config 的 hash 为 `0xAABBCCDD`，有 4 个排序 voter，bitmap `0x00000007`。按 §6.2 的 Canonical Certificate 输入（现包含 Type 8 `P2=anchor Config ID`）计算，CRC32 为 `0x12D221F9`。

```text
04 21 05 40  01 02 03 04  00 00 00 03  0A 0B 0C 0D
00 00 00 07  00 00 00 09  00 00 00 0F  00 00 00 21
00 00 00 01  00 00 00 07

04 08 05 00  01 02 03 04  00 00 00 03  0A 0B 0C 0D
00 00 00 07  00 00 00 09  00 00 00 0F  00 00 00 21
00 00 00 01  12 D2 21 F9
```

第一帧是 Type 33、`CERT_SET_OLD` 的 fragment `0/1`；第二帧是 Type 8 announcement，二者的 `P2` 均为同一个 Stable anchor Config ID `15`。M05-02 只验证字节 codec；M10 必须额外验证 CRC、frozen Config、VoteId 和 quorum。缺少第一帧、篡改 Type 8 或 Type 33 的 `P2`、篡改 `P5`、把 flag 改为 NEW、或把 fragment count 改为 2 时，后续 M10 验证必须拒绝 Head Takeover。

### 8.6 同 Cluster Planned Transfer：PREPARE / BACKUP READY / STEPDOWN

下列三帧使用 old Epoch A `(0x01020304, 2, 0x0A0B0C0D)`；目标仍为 Cluster `0x01020304`，但 target Epoch 为 `(0x01020304, 3, 0x55667788)`，符合 `checked_next_term(2)=3`。B 是 old frozen Config 中已确认的 Backup；transaction ID `49`，frozen Config 为 `(15, 0xAABBCCDD)`。三帧均为 40 B。

```text
04 1A 05 00  01 02 03 04  00 00 00 02  0A 0B 0C 0D
00 00 00 31  01 02 03 04  00 00 00 03  55 66 77 88
00 00 00 0F  AA BB CC DD

04 1B 06 00  01 02 03 04  00 00 00 02  0A 0B 0C 0D
00 00 00 31  01 02 03 04  00 00 00 03  55 66 77 88
00 00 00 0F  AA BB CC DD

04 09 05 00  01 02 03 04  00 00 00 02  0A 0B 0C 0D
00 00 00 31  01 02 03 04  00 00 00 03  55 66 77 88
CA FE BA BE  00 00 00 00
```

第一帧由 A（HEAD）单播给 B；第二帧只能由 B（BACKUP，role byte=`0x06`）单播给 A，且不授予 B Authority；第三帧只能由 A（HEAD）向成员发送。A 发送第三帧前在本地精确匹配第二帧 READY；B 接收后续 `HANDOVER_COMMIT` 时匹配自己已发送的 READY；成员只验证第三帧的 old Authority、txid/nonce、完整 target Epoch 与同 Cluster 规则，**不得**因未见 READY 拒绝 Stepdown。

下列是第二帧的角色错误负向 vector；它仅将 READY role byte 从 `0x06` 伪造成 `0x05`（HEAD）。由于 target Cluster 等于 old Cluster，Codec/RX gate 必须在任何 FSM/Authority 副作用前拒绝：

```text
04 1B 05 00  01 02 03 04  00 00 00 02  0A 0B 0C 0D
00 00 00 31  01 02 03 04  00 00 00 03  55 66 77 88
00 00 00 0F  AA BB CC DD
```

M05-02 必须为每个 Type 建立至少一个合法 vector，并为错误长度、错误版本、未知 Type、错误 role、错误 flag、非零 reserved、非法 ID、serial 回绕和每种 Type 的关键零字段建立负向 vector。

## 9. 分片、资源与非目标

- 固定 40 B 指的是 Cluster payload，不代表一次物理链路传输。CAN/Stream/USB/Wi-Fi 等 Carrier 的承载与分片由 M05-09 统一改为使用 `UCN_CLUSTER_MESSAGE_BYTES`，本 RFC 不改变其现有实现。
- 40 B 比 v3 增加 8 B；该增量是为了消除 Config/Snapshot/Takeover 中的截断或隐式绑定，不允许在没有新版本的情况下再把字段塞回未定义 reserved 位。
- 本 RFC 不实现 Join Epoch install、Config、Handover、Rekey、Capability policy、双栈 decoder 或诊断输出；它只冻结未来实现的输入/输出合同。
- 本 RFC 不解除 M04 的实际硬件边界：真实 Flash、掉电、MCU 栈和资源测量仍需后续实机门禁。

## 10. M05 后续执行顺序

1. `CLV2-05-01`：**DONE / 外部冻结复审 GO**；RFC4 已冻结，任意字节修改必须升 Cluster Format。
2. `CLV2-05-02`：独立实现 v4 codec 与严格 v3/v4 分派，先加 golden/negative vectors，不接入生产发送、生产 FSM 或默认 encoder。
3. `CLV2-05-03..06`：以 type-specific builder 取代 giant message 的跨字段复用，并保留本文 Type/字段合同。
4. `CLV2-05-07/08`：实现 capability negotiation 与混合版本资格策略。
5. `CLV2-05-09..12`：完成 Carrier 载荷迁移、fuzz、诊断和受控 encoder 开关。

`CLV2-07-00` 仍是 M07/M13 开放真实 `PREPARED` 的前置阻断项；v4 Wire 冻结不改变该持久化来源区分要求。
