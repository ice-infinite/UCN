# UCN V6-07 Security 与唯一 JOIN 实现报告

> 状态：隔离实现、六个小节自审与全阶段软件自审完成；最终统一外审延期。
> 生产状态：仅在 `UCN_BUILD_V6_EXPERIMENTAL=ON` 时编译；尚未接入 v6 生产 Node/Adapter。

## 1. 本阶段解决的问题

V6-03 只冻结了帧格式，不能仅凭一个结构合法的 Security Context 就信任来源。V6-07 在同一固定容量
Owner 内补齐从身份准入到普通帧使用的安全闭环：

```text
Bootstrap Cookie/预算/固定 pending
  -> 双方身份与完整 transcript
  -> Address/Binding/Authority Lease 证明已持久
  -> 显式 Hop/E2E selector
  -> Session 持久化并 reload 验证
  -> ADMITTED
  -> Hop/Group/E2E Tag + Replay + 精确 Opcode ACL
```

V6-06 不再拥有第二条 JOIN 或 `ADMITTED` 写入口。Group HELLO 也只能形成
`group_discovery_only` 提示，不能建立 Principal、Session、ACL、Capability 或续租。

## 2. 固定资源与持久快照

`ucn_v6_security_manager_t` 是 caller-owned opaque 对象。以下容量全部来自唯一编译期 Manifest：

| 资源 | 默认容量 | 满载行为 |
| --- | ---: | --- |
| Peer Security Session | 8 | 返回 `UCN_V6_ERR_NO_SPACE`，不驱逐 |
| 精确 ACL | 16 | 返回 `UCN_V6_ERR_NO_SPACE`，不覆盖 |
| Static Group Policy | 8 | 退休槽永久占位 |
| 每 Group Key Slot | 4 | 退休 Key ID 永久占位 |
| Group Replay Source | 16 | 返回 `UCN_V6_ERR_NO_SPACE`，不驱逐 |

Security Snapshot 保存本机 Binding、Session、ACL、Group Policy/Key、TX 预留高水位与 RX Replay。
每次状态更新固定执行：

```text
reserve_generation_witness(next)
  -> submit(snapshot next)
  -> reload(snapshot)
  -> reload(witness)
  -> 完整字段级一致性验证
  -> 发布 RAM committed state
```

Witness 已推进而 Snapshot 写失败、Snapshot 损坏、Generation 不相等或 Provider 重入都会使 Manager
永久 faulted。它不会静默回退旧 Snapshot，也不会用原始结构 padding 的 `memcmp` 代替语义验证。
本阶段 Fake Store 证明软件时序；真实双槽 Flash、掉电注入与擦写寿命仍属于 V6-13/V6-14。

## 3. 唯一 JOIN 与 REAUTH

Security 不接受调用方自行声称“认证完成”。`ucn_v6_security_commit_join()` 必须同时拿到：

1. opaque Bootstrap Owner；
2. 精确 Bootstrap Key；
3. 与 pending 完全一致且未到期的 transcript；
4. Bootstrap FSM 的 `FINAL_DURABLE` 状态；
5. Authority 与 Device 两份密码证明；
6. Authority durable Fence/quorum/Lease、Binding Certificate 和本地保守半开 Lease Deadline；
7. 显式 Hop/E2E Suite、Key ID、Key Generation、Session 与 Link Generation。

JOIN 只允许首个 Session Generation 1；REAUTH 只允许已有未撤销 Session 的精确下一代，且不得修改
双方 Binding 或原 Binding Certificate。重启后持久 Session 一律转为 `requires_reauth`，本机单调时钟
Deadline 不会被当作重启后仍有效。相同 durable 事务可以幂等重放，但必须逐项匹配原 transcript
nonce/txid/hash、Binding、selector 和 Deadline；冲突重放不能刷新租期或再次写 Provider。

## 4. Suite、Key、Tag 与 Replay

当前编译期 Suite registry 固定为：

| Context | 允许 Suite |
| --- | --- |
| Peer Hop / Group | HMAC-SHA256-128 |
| E2E Auth-only | HMAC-SHA256-128 |
| E2E AEAD | AES-GCM-128、ChaCha20-Poly1305 |

Frame 必须携带唯一显式 selector；接收方不会试遍 Key、猜默认 Suite 或把未知算法降级。Provider 只实现
密码原语，算法合法域由协议库固定验证。测试中的确定性 Fake Crypto 只用于验证 AAD、Tag、Replay 与
调用时序，绝不是生产密码后端。

发送侧先完成结构和容量预检，再从已持久预留区取得 Packet Sequence。输出空间不足不会消耗序号；
一旦进入密码操作，失败允许跳号但绝不复用。重启后从 durable reserved high-water 的下一号开始。
接收侧在 Tag、selector、Link/Session Generation 与 Replay 通过后才持久化 Replay，再发布结果。
64-bit 滑窗拒绝重复和窗口外旧包；轮换 grace 内 current/previous Key 各有独立 Replay 域，
`now == previous_deadline` 即拒绝旧 Key。

## 5. 精确 ACL 与 Group 边界

ACL Key 固定包含 Principal、双 Binding、Session Generation、Endpoint 或 Protocol Opcode、Frame Type、
Traffic Class、Delivery Guarantee、Interaction Role、Operation ID policy 和方向。非 DATA 必须精确匹配
非零 `protocol_opcode`；授权 Opcode 10 不会放行同 Frame Type 的 Opcode 11。

Group Policy/Key 更新要求 Realm Admin proof，并先持久化再使用。Static Group Slot 和 Key Slot 退休后
永久不可复活；普通换钥保持 Key ID，只推进 Key Generation。Group Tag 只能授权固定的
`CONTROL/GROUP_HELLO`、保留 Link-local Group 地址和 `hop_limit=1`。共享 Group Key 不证明独立
Principal，接收结果固定为 discovery-only，不能进入 Endpoint ACL 或普通控制权威路径。

动态 Group ID 的全局单调分配仍由 V6-02 Identity Authority 拥有；本阶段的 Security 表只消费已授权
Group Policy/Key，不重新发明 ID 分配器。

## 6. 六个小节自审结论

### 6.1 Suite/Key 与持久快照

- 未知、零值、模式不匹配的 Suite/Key 失败关闭；
- Witness/Snapshot 不一致、写失败和损坏 reload 不发布 RAM；
- Provider 回调期间重新进入 Security API 返回 STATE。

### 6.2 唯一 JOIN/REAUTH

- 非 FINAL、过期、Key/transcript 冲突均在证明和 Provider 写入前拒绝；
- 双 Proof、Authority Fence/quorum/Lease 和 Binding Certificate 全量绑定；
- exact replay 零写幂等，冲突 Deadline/Generation/Binding 拒绝；
- 重启 Session 必须走 REAUTH，撤销 Session 不能复活。

### 6.3 精确 ACL

- DATA 与 Protocol Context 使用互斥字段规则；
- Control Opcode 逐值授权，Frame Type 不能充当通配权限；
- inbound/outbound 分离，拒绝时结果对象不写回。

### 6.4 Hop/E2E/Group 与 Replay

- Hop Tag 覆盖完整外层帧，E2E Tag 绑定 80 B canonical AAD；
- AEAD 与 Auth-only 不混用，坏 Tag、重复 Sequence、错 Link Generation 拒绝；
- Group source 仅为 claimed binding，输出不提升为认证 Principal。

### 6.5 Rotation/Revocation/资源

- Session 与 Group Key 只接受精确下一代；previous grace 为半开区间；
- Static Group/Key 退休槽不复用；所有表满均零驱逐；
- TX 高水位分块预留，发送不足容量不消耗 Sequence。

### 6.6 隔离与工具链

- 默认产品 archive 不链接本阶段实现；
- v5 Core/Cluster/Adapter 没有调用 v6 Security；
- GCC、MSVC、ASan/UBSan 和 GCC Analyzer 的定向矩阵通过。

## 7. 验证结果

| 门禁 | 结果 |
| --- | --- |
| Windows GCC Full Debug 全量 | 65/65 |
| V6 Identity/Security 定向 | 2/2 |
| MSVC 19.29 Release `/W4 /WX` | v6 7/7 |
| WSL ASan/UBSan | Identity/Security 2/2 |
| WSL `-fanalyzer -Werror` | Identity/Security 2/2 |
| `git diff --check` | 无空白错误，仅行尾格式提示 |

## 8. 尚未完成的生产证据

本阶段不能被解释为“安全已经可发布”。以下证据仍缺失：

- 经过选型、侧信道与密钥生命周期审计的真实密码 Provider；
- ESP32/MCU 安全存储、RNG、Flash 双槽、掉电和磨损测试；
- 真实 RTOS task/ISR/SMP 锁与 Provider callback 并发测试；
- v6 Node/Adapter 生产 RX/TX 接线；
- V6-06 Capability、V6-08 Route、V6-10 Transfer 与业务 Endpoint 的组合攻击测试；
- 独立外部审计。

因此当前状态只能写作：`V6-07 软件实现与自审完成 / FINAL EXTERNAL REVIEW DEFERRED`。

## 11. V6X-A02、A03、A11 外审整改补充

Wire 与 Security 已把单一 Packet Sequence 拆为两个所有权域：

- `origin_sequence` 属于端到端或 Group 安全上下文，进入 canonical AAD，中继保持不变；
- `hop_sequence` 属于当前 Peer Session，每跳重新分配、验证 Replay 并重新生成 Hop Tag。

新增 `ucn_v6_security_relay_frame()` 后，中继必须先从原始编码帧完成 Hop Tag、selector 和
Replay 验证，才会输出 Security-owned `verified_ingress`。随后只允许修改 Hop Budget、下一跳
selector 和 Hop Sequence；Source、Origin Sequence、密文、E2E Tag 与所有 E2E AAD 字段保持
不变。工作区不得与入站或输出缓冲区重叠，拒绝路径不写回结果对象。

RX Replay Window 现在是易失运行态，不再每收一帧写一次完整 Snapshot。持久化只发生在低频
Session/Key 变化和 Hop/E2E TX Sequence 固定区间预留。重启后 Peer 必须 Reauth、Group 必须
Rekey，且 TX 会跳过整个已预留区间，因此不复用旧序号，也不会形成每包 Flash 写放大。

新增 A→B→C、A 经 B 到 C/D 多目标以及五节点安全中继回归；Nano 使用每节点最多两个 Peer
Session 也能完成整条链路。
