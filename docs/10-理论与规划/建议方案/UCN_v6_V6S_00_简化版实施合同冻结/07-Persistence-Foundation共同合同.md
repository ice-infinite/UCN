# V6S-00-07 Persistence Foundation 共同合同

> 状态：`DONE / SELF-REVIEW PASS / EXTERNAL REVIEW GO`
>
> 本文只冻结所有 durable 模块共享的 Record Envelope、Provider、双槽、Witness、Proof 与恢复顺序；业务 Record Body 不在此处设计。

## 1. 边界与目标

Persistence Foundation 解决五件共同问题：

1. 哪个 durable domain 正在提交哪一代 Record；
2. 如何在掉电后从双槽得到唯一可证明的最新状态；
3. Provider 同步/异步完成如何走同一状态机；
4. 如何生成不能被别的事务复用的 Durability Proof；
5. 一个 domain 损坏时如何只 Fence 依赖它的能力。

它不决定 Cluster Config、Security Key、Operation Result 或 Group Policy 的业务字段。每个业务
模块在实现前另行冻结自己的 `schema_id/schema_version/body_bytes/canonical rules/transition`
并提供纯验证器；Foundation 不能用万能 tagged union 把全部业务状态重新耦合。

普通 Best Effort、Latest、易失 Reliable、静态只读身份和不要求 durable promise 的 Service
不写 Flash。

## 2. 唯一 Owner 与调用路径

```mermaid
flowchart LR
    B[Business Owner] -->|immutable requirement| C[Runtime Coordinator]
    C -->|exact persistence request| P[Persistence Owner]
    P <--> S[Storage Provider]
    P -->|immutable durability proof event| C
    C -->|exact proof event| B
```

只有 Persistence Owner 持有 Provider 和 I/O 状态。业务 Owner 不持有 Persistence Owner/Provider
指针，不调用 `load/write/read/commit/poll`。同步 Provider 完成也必须回到
`Persistence Owner → Coordinator → requesting Owner`，不能通过函数返回直接让业务 Owner
发布 Authority。

## 3. Durable Domain

每个 Manifest 声明的 domain 拥有独立：

```text
domain_kind + domain_id
schema_id + schema_version
owner_instance + domain_generation
slot A + slot B
record_generation high-water
anti-rollback witness
one pending request/staging (simplified v1)
fault/fence
```

`domain_kind` 的初始 Registry 为：

| 值 | Domain | Body Owner |
| ---: | --- | --- |
| 1 | Identity/Address Binding | Admission |
| 2 | Security Context High-water | Security |
| 3 | Transport Parent/Transfer High-water | Transport |
| 4 | Durable Operation Journal | Operation |
| 5 | Group Policy/Key | Group/Security |
| 6 | Time Authority/Domain Generation | Realtime |
| 7 | Cluster Epoch/Config/Vote/Tombstone | Cluster |
| 8 | Product anti-rollback configuration | Product Integration |
| 9～65534 | 保留 | 未分配 |

同一种 kind 可以有多个 `domain_id`，但 Manifest 中 `{kind,id}` 必须唯一。`domain_id=0` 和
`UINT64_MAX` 保留。域之间不共享当前 snapshot、pending、record generation 或 Fault。

## 4. Canonical Record Envelope v1

所有多字节字段使用 big-endian。Envelope 固定 96 B：

| Offset | 长度 | 字段 | 规则 |
| ---: | ---: | --- | --- |
| 0 | 4 | Magic | ASCII `UC6R` |
| 4 | 2 | Envelope Version | 固定 1 |
| 6 | 2 | Header Bytes | 固定 96 |
| 8 | 2 | Domain Kind | Registry 合法值 |
| 10 | 2 | Schema ID | 对应 Domain Kind 内非零且唯一，业务模块冻结 |
| 12 | 2 | Schema Version | 非零，必须属于该 Schema ID |
| 14 | 2 | Header Flags | v1 全零 |
| 16 | 16 | Compiled Durable Manifest Digest | 精确匹配本机 durable manifest |
| 32 | 8 | Domain ID | 非零且非全 1 |
| 40 | 8 | Record Generation | nonzero checked-next |
| 48 | 8 | Foundation Transaction ID | nonzero、同 domain 不复用 |
| 56 | 2 | Operation Kind | 业务模块 Registry，0 保留 |
| 58 | 2 | Digest Suite | v1 固定 1 |
| 60 | 4 | Body Bytes | `0..DOMAIN_BODY_MAX` |
| 64 | 4 | Body CRC32C | 只覆盖 Body |
| 68 | 4 | Header CRC32C | 本字段清零后覆盖完整 96 B Header |
| 72 | 16 | Body Digest | Suite 1 的 128-bit digest |
| 88 | 8 | Reserved | 全零 |

Digest Suite 1 冻结为 BLAKE2s-128，输入为：

```text
ASCII "UCN6-PERSIST-BODY-V1" without NUL
u16be(domain_kind)
u64be(domain_id)
u16be(schema_id)
u16be(schema_version)
u64be(record_generation)
u64be(transaction_id)
u16be(operation_kind)
u32be(body_bytes)
exact canonical body bytes
```

Compiled Durable Manifest Digest 固定为 BLAKE2s-128，输入为：

```text
ASCII "UCN6-DURABLE-MANIFEST-V1" without NUL
u32be(protocol_manifest_version)
u32be(storage_layout_version)
u64be(composition_feature_bits)
u8(profile_id)
u16be(domain_entry_count)
for each domain entry sorted by (domain_kind,domain_id):
    u16be(domain_kind)
    u64be(domain_id)
    u16be(schema_id)
    u16be(schema_version)
    u32be(body_capacity_bytes)
    u32be(slot_capacity_bytes)
    u8(witness_policy)
    u8(provider_atomicity_class)
    u16be(digest_suite)
```

生成器必须对排序、重复 domain、整数溢出和未登记枚举失败关闭。Digest 用于防止错误固件/
布局加载，不是安全授权或抗恶意碰撞证明；产品若把存储视为恶意输入，仍须使用设备根密钥保护。

CRC 用于快速发现随机损坏；Digest 绑定 canonical 语义和事务，但不是防恶意存储的认证。威胁
模型要求对抗恶意 Flash/离线篡改时，Provider 必须额外提供设备根密钥 MAC/签名或平台安全
存储证明，并在 Manifest 中声明；不能把无密钥 digest 写成认证。

Body 紧随 Header，未使用的固定槽尾部必须擦除为 Provider erased value，但不参与 canonical
Body。Decoder 只读取 `Body Bytes`，并要求槽容量、Envelope、Body、Commit Marker 精确无溢出。

## 5. Commit Marker

每个 Slot 在固定末尾保留 16 B marker：

| Offset | 长度 | 字段 |
| ---: | ---: | --- |
| 0 | 4 | ASCII `UC6C` |
| 4 | 8 | Record Generation |
| 12 | 4 | CRC32C(`UC6C || generation`) |

写 inactive Record 时 marker 保持未提交。`publish_commit_marker()` 必须对外提供“完整 16 B
旧值或完整 16 B 新值”的原子语义；物理介质不能原子写 16 B 时，Provider 必须用自身冗余/
journal 实现等价语义。Foundation 不猜 Flash page/program unit，也不接受半 marker。

Marker 只发布已经完整写入并回读验证的 Record。相同 generation 的 exact marker 重放可幂等；
不同 generation、不同 Slot 或冲突字节失败关闭。

`load_slot()` 没有“逻辑空槽”旁路。Provider 必须始终返回固定长度的原始槽镜像并报告
`BLOB_PRESENT`，Foundation 自己读取槽尾 marker：

- marker 全为 Provider erased value：该槽未提交，正文即使完整、部分写入或随机残留也全部忽略；
- marker 是完整且与 Record Generation 精确一致的 16 B 合法值：该槽已提交，再继续验证
  Envelope、Digest、CRC 和 Body；
- marker 既非全擦除也非完整合法值：视为 torn marker，当前 domain 失败关闭；
- `BLOB_EMPTY` 仅允许 `load_witness()` 表示不存在 witness；`load_slot()` 返回它属于 Provider
  合同错误并失败关闭。

Provider 不能维护“这个槽是否提交”的易失 sidecar。进程重建或掉电重启后，返回值必须只由
真实持久介质内容产生；Host Fake Provider 也必须遵守同一规则。

## 6. Anti-rollback Witness

Witness 是 Provider 提供的 per-domain 单调对象：

```text
{domain_kind, domain_id, highest_maybe_published_generation, integrity_proof}
```

语义合同：

- `advance(old,new)` 只接受 `new==old+1`，或由明确 reservation protocol 证明的有限跳号；
- 成功后任何重启都不能返回更小值；
- 损坏/缺失/多个最大值时返回 FAULT，不返回猜测值；
- generation 到顶进入 domain Fault；
- witness 不得与两个 Record Slot 存在同一单点撕裂故障。

Provider 可以使用硬件单调计数器、独立冗余区或平台认证高水位实现。只有两槽、没有独立
witness 的产品不能承诺 anti-rollback；不得把“最新槽坏了就退旧槽”当恢复。

## 7. Provider SPI

Provider vtable 至少具有 exact `struct_size/api_version/context`。I/O token 由同一 Provider
回调域中所有 Owner 共享的 caller-owned gate 在进入 callback 前分配，是该回调域全局唯一、非零、
64-bit 单调且不回绕的值；不能由各 Owner 独立分配，否则两个 Owner 共享 Provider 时会产生歧义。
Owner 的固定 continuation slot 保存完整绑定，Provider 不得生成、替换或解释 token。所有开始函数
都接收同一个 input token：

```text
begin_load_slot(domain, slot, output_buffer, exact_bytes, io_token)
begin_write_inactive(domain, slot, input_buffer, exact_bytes, io_token)
begin_readback(domain, slot, output_buffer, exact_bytes, io_token)
begin_publish_commit_marker(domain, slot, marker16, io_token)
begin_load_witness(domain, output_witness, io_token)
begin_advance_witness(domain, expected_old, exact_new, io_token)
poll(io_token, expected_phase, output_completion)
```

开始函数返回值只有 `COMPLETED/PENDING/FAILED`；全零或未知值非法。`COMPLETED` 的输出只能写入
调用前预留的 latch，callback 返回后由 Owner 统一合并；`PENDING` 保留 exact continuation，后续
只允许用相同 token 和 phase 调用 `poll`；`poll` 可以继续返回 `PENDING`，此时必须原样保留
同一 continuation，不分配新 token、不重复发起底层 I/O；新 I/O 分配到 `UINT64_MAX` 前必须
失败关闭，不能回绕或复用旧 token；`FAILED` 退休 token 并按当前 phase
回滚/Fault。没有实现 `poll` 的 Provider 若从任一 `begin_*` 返回 PENDING，则立即 Fault 当前
domain。每个 I/O token 绑定
`{runtime,owner,domain,request,slot,record_generation,phase}`，不可跨 phase 重用。

Provider geometry 在 Owner Storage 首次写入和任何 Provider I/O 前完成验证。每个 Manifest
`slot_capacity_bytes` 必须不超过 `maximum_slot_bytes`，并且分别是
`minimum_write_alignment` 与 `minimum_erase_alignment` 的整数倍；两个最小对齐均必须非零。
不满足时初始化原子拒绝，Owner Storage、输出指针和 Provider 调用计数保持不变。

Provider callback 规则：

1. 调用前先在 caller-owned shared gate 中登记 exact continuation 和 `IO_ACTIVE`；
2. 同步 completion 只写预留 latch；callback 返回后统一合并；
3. callback 内递归 init/submit/poll/load、另一个 Provider I/O 或 Runtime 生命周期入口返回 STATE；
4. 多 task/ISR/SMP 的 gate 必须使用平台原子/临界区，不得使用普通静态指针或 volatile；
5. Provider 不解析业务 Body，也不直接通知业务 Owner。

shared gate 维护有界 Owner 引用计数。每个成功初始化的 Persistence Owner 原子登记一次，并在
成功反初始化时原子撤销一次；只要引用计数非零或 callback 活跃，gate deinit 必须零写拒绝。
因此不能在旧 Owner 仍持有指针时销毁并原地重建 Gate，多个 Owner 的共同回调域不会被生命周期
操作静默拆开。

状态锁在 Provider callback 返回后必须重新取得，Owner 才能合并 completion。若平台锁在该点返回
失败，调用栈不得继续读取、写入或解锁 Owner；shared gate 指针、Runtime/Owner identity、token 和
phase 必须在释放状态锁前冻结，只允许使用这些冻结值经独立 gate 锁撤销本次 callback 占用，然后
返回 `UCN_ERR_STATE`。Owner 保留 `IO_ACTIVE/call_active` 形成不可恢复的易失 Fence，后续
请求全部失败关闭；上层只能按产品故障策略重启该 Runtime，不能把这次 I/O 猜成成功或未发生。

## 8. Persistence Request 与 Proof

业务 Owner 产生的 request 必须包含：

```text
caller owner/runtime/domain generation
domain kind/id + schema/version
foundation transaction id + operation kind
expected record generation/fingerprint
canonical next body bytes/fingerprint
business transition proof/reference
volatile continuation handle
```

Digest 只作槽查找预筛选；命中后必须精确比较整个有界 canonical request。相同 digest、不同
request 是冲突，不能复用 pending。

这里的“整个 request”只适用于同一易失 pending 的重复提交。重启后或 proof 退休后的同事务
重放，只能根据 Record 自身判断下面的 durable identity：

```text
domain + schema/version + transaction id + operation kind
record generation + exact canonical body/body fingerprint
```

`absolute_deadline`、`business transition proof/reference` 和 `volatile continuation` 不进入
Record，也不得由 Foundation Proof 回显成“已持久化字段”。同一 durable identity 的重放只返回
同一耐久事实；它携带的当前业务前置条件和 continuation 是新的易失关联，必须由 Coordinator 与
业务 Owner 在本次调用中重新精确绑定、重新验证。相同 transaction ID 但 durable identity 中任一
字段或 Body 不同，必须作为冲突拒绝。

成功 reload 后 Foundation 生成只读 proof：

```text
runtime/owner/domain generation
domain kind/id
record generation
transaction id + operation kind
body bytes + body fingerprint
slot + witness generation
```

Proof 只证明这些 durable bytes 已按 Foundation 合同提交和重载。业务 Owner 在使用前还必须
精确匹配当前事务，并重新验证当前 Lease/Authority/Policy/Config/Capability；过期 Authority
不能因旧 Flash completion 恢复。

## 9. 单一提交状态机

```text
READY
 -> WRITING_INACTIVE
 -> READING_BACK
 -> PUBLISHING_MARKER
 -> ADVANCING_WITNESS
 -> RELOADING_SLOT_AND_WITNESS
 -> READY + PROOF
```

每一阶段都允许进入 `PENDING(exact_phase)`，poll 只能恢复记录的阶段。精确算法：

```text
submit(request):
    validate owner/domain/gate/request/canonical body
    require expected fingerprint == current snapshot
    require Coordinator-bound nonzero business transition reference
    checked-next record generation and reserve inactive slot/staging
    publish exact WRITING continuation before Provider call
    write full Header+Body with uncommitted marker
    read back and compare every byte
    publish exact 16 B marker
    advance independent witness
    reload both slots and witness
    select unique newest record and compare durable request identity exactly
    publish new domain snapshot and immutable proof
```

业务 pure transition validator 由业务 Owner 在形成 immutable requirement 前执行，Coordinator
把其 exact reference/digest 与本次 requirement 绑定；Persistence Foundation 不反向调用业务
Owner，也不解释该 digest。Foundation 自己负责的 schema/domain/capacity/expected-state/duplicate
检查仍必须全部发生在首次 Provider I/O 前。未来业务模块接线时，若业务验证结果不能被
Coordinator 精确绑定，提交必须在进入 Persistence Owner 前失败关闭。

所有配置、transition、capacity、expected-state 和 duplicate/conflict 校验在首次 Provider I/O 前
完成。不允许在 durable Commit 后才检查 Backup ACK、quorum、Authority 或业务门禁。

## 10. 双槽恢复矩阵

| 断电/损坏位置 | 重启观察 | 唯一结果 |
| --- | --- | --- |
| 新 Record 未写完 | 旧 committed + 旧 witness | 忽略未提交槽 |
| 新 Record 完整、marker 未提交 | 新槽 uncommitted | 忽略新槽，不发布 promise |
| 首次 marker 已提交、witness 仍为 0 | 唯一完整新代为 generation 1 | provisioning witness 有效后允许补推进，再 reload |
| 后续 marker 已提交、witness 仍为旧值 | 同时存在完整 `generation=witness` 前驱和 `generation=witness+1` 后继；Transaction ID 严格递增 | 验证完整历史后补推进 witness，再 reload |
| witness 非零、只有 `witness+1` 后继而前驱缺失/损坏 | 无法证明 Transaction ID 相对前驱严格递增 | domain Fault；witness 保持不变，不发布正文或 Proof |
| witness 已推进、最终 reload 未做 | 新 committed 与 witness 相等 | reload 精确成功后恢复新状态 |
| witness 指向的最新槽损坏/缺失 | 旧槽低于 witness | domain Fault；禁止回退 |
| 两槽同代但内容不同 | 冲突 | domain Fault |
| 两槽同代且内容相同 | 不可能由正常双槽提交顺序产生 | domain Fault |
| 相邻 generation 但 Transaction ID 相等或回退 | 事务历史不连续 | domain Fault |
| generation 跳跃无 reservation proof | 无法证明连续 | domain Fault |
| witness 损坏/回退/多个最大值 | 无法证明高水位 | domain Fault |

掉电允许安全跳号，不允许 ABA。Factory Empty 只能由产品 provisioning marker/Provider 明确证明；
不能把任意全 `0xFF`、CRC 错或旧版本 Record 当 factory empty。

Witness 补推进不是“看到下一代有效 Record 就接受”。除 generation 0 到 1 的首次提交外，补推进
必须同时读到同一 Domain、Schema 和 Durable Manifest 下的前驱与后继；两者 generation 精确
相邻，且后继 Transaction ID 严格大于前驱。任一证明缺失、Transaction 相等/回退或字段错绑，
必须在 Provider `ADVANCE_WITNESS` 前失败关闭。

## 11. 启动顺序

```text
validate generated Manifest + Provider + storage geometry
load Product anti-rollback/config domain
load Identity/Binding domains
load Security and Transport high-water domains
load Operation/Group/Time/Cluster domains in Manifest order
for each domain: unique selection or local fence
create fresh volatile runtime/owner/continuation generations
only then allow the dependent module to become READY
```

Required domain Fault 阻止 Runtime RUNNING；optional domain Fault 只 Fence 依赖能力。恢复不加载旧
callback、指针、Driver token、timer、Route、QoS cursor 或 volatile Handle。

旧 v4/v5/v6-full Record、旧 schema 和 legacy PREPARED abort 一律拒绝。一次性迁移只能作为刷机/
离线工具运行，不能链接进简化版 Runtime。

## 12. Operation 状态与 GC 边界

Foundation 只提供原子 Record；Operation Body 仍必须冻结：

```text
PREPARED -> EXECUTING -> COMMITTED_RESULT
PREPARED -> ABORTED_NO_EFFECT
EXECUTING -- reboot/unknown --> IN_DOUBT
IN_DOUBT -- authenticated reconciliation --> COMMITTED_RESULT/TOMBSTONED
```

外部副作用与 Journal 不能原子提交、也不能查询对账时，掉电后只能 `IN_DOUBT`，不得重试或
伪造结果。`PREPARED/EXECUTING/IN_DOUBT` 不按时间 GC；终态只有在 retention/replay 窗口和
对端高水位证明均完成后才可退休。表满只拒绝新 Durable Operation，不阻止普通消息。

## 13. 固定资源与 Feature OFF

简化版 v1 每个 domain 最多一个 pending，Persistence Owner 用公平游标推进多个 domain。
Storage 公式包括：

```text
DOMAIN_COUNT * (2 * SLOT_BYTES + WITNESS_VIEW_BYTES + DOMAIN_META_BYTES)
+ PERSIST_STAGING_BYTES
+ IO_CONTINUATION_BYTES
+ SHARED_GATE_BYTES
```

不得在栈上复制完整 Slot/Body/Owner。staging 由 Owner Storage 持有并有 busy fence。

Persistence OFF 时 Provider、Envelope、staging、恢复扫描和专用符号均为零；依赖持久化代际的
Dynamic Binding、durable Security/Transport high-water、Durable Operation、Dynamic Group
和 Cluster Authority 返回 UNSUPPORTED。普通易失通信继续，`VOLATILE_TEST` 不构成掉电证明。

## 14. 对抗测试合同

实现阶段至少覆盖：

- Header/Body/Marker 每个字段、长度、CRC、digest、reserved 的独立破坏；
- write/read/marker/witness/reload 每阶段同步、PENDING、失败、重入和早到 completion；
- 每个掉电点重启，尤其“最新已发布槽后来损坏”；
- 相同 transaction exact replay 幂等、相同 ID 不同 Body 冲突；
- wrong domain/schema/owner/generation/slot/phase Proof；
- deadline 边界恶意输入不能借 lazy expiry 清除别的 pending；
- 两 domain 并发时一域 Fault 不污染另一域；
- callback 递归 init/submit/poll 与双线程/双 domain gate；
- Full/Lite/Nano、Provider OFF、异步 Provider、真实 Flash 撕裂写/掉电；
- 失败输出、旧 committed snapshot、slot、witness 和统计的逐字节不变性。

Host fault injection 证明状态机；真实 Flash/power-cut 才证明介质和 Provider 的 atomic/durable
合同。两者不能互相替代。

## 15. 本项自审

- 96 B Envelope、16 B Commit Marker 和 digest 输入已逐字段冻结；
- CRC、digest、认证存储的威胁模型没有混淆；
- 双槽、独立 witness、最新槽损坏与 factory-empty 均失败关闭；
- 同步/异步 Provider 共享 exact continuation 和同一状态机；
- Durability Proof 与当前业务 Authority 二次门禁明确分开；
- 业务 Body/transition 没有被提前塞入 Foundation；
- Feature OFF、固定 staging、局部 Fault 和真实掉电证据边界完整。

结论：`V6S-00-07 = DONE / SELF-REVIEW PASS / EXTERNAL REVIEW GO`。
