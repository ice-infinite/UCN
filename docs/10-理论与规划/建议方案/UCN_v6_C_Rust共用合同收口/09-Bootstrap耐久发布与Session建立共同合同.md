# Bootstrap 耐久发布与 Session 建立共同合同

## 1. 目标和放行边界

本段把[认证事件与分片合同](08-Bootstrap认证事件分片与身份校验共同合同.md)的五类事件，
接到实际 Identity / Foundation / Security 状态机，而不是把收到 Final 或一个 bool 当作
已经持久化。这里的“实际”指 Host 中真实执行 Foundation 的双槽、Marker、Witness、
回读和 Proof 流程；**不代表真实 Flash、物理 Witness 或断电实测通过**。

Rust 新增独立 `bootstrap-join` Feature，依赖 `bootstrap-runtime + Identity + Persistence`。
仅用 Cookie/双向身份校验的产品可以关闭它。协调逻辑位于 Runtime；Identity、Admission、
Security 均不直接调用另一个业务 Owner，也不直接驱动介质。

C/Rust 共用既有 0004～0008、379 B Transcript、36 B 分片头和方向/阶段规则；新增共同
Session 种子算法。C 本轮同步该纯算法和回归，已有 Identity/Admission/Session Proof 消费
路径保留；**尚未新增完整 C 自动 JOIN Root，也未证明完整 C↔Rust Runtime 入网互通**。

## 2. 先捕获本地 Challenge，不能到 Final 才开始计时

`JoinFreshness::begin()` 调用 Identity Owner，获得私有 Authority/Binding Challenge Handle。
设备必须在创建并发送 HELLO 之前捕获；Authority 必须在自己的首份身份证明之前捕获。
设备起点晚于 Bootstrap Client 的创建时刻，`BootstrapJoin::new()` 直接拒绝。

Challenge 精确绑定 Authority Principal、预期 Device Principal、freshness nonce 和 JOIN
transaction。nonce/transaction 必须等于已认证 Cookie transcript；句柄不能由调用方构造。
两端使用自己的单调时钟，**不比较两端 uptime，也不假设两端 Link ID/Generation 相等**。
Authority 端的本地预留时间不要求等于设备端捕获时间。

Authority/Binding Deadline 由 Identity 既有保守 Lease Policy 计算；半开边界保持不变。
接收迟到 Offer、Commit 或 Final 不会重建 Challenge、刷新原始事务 Deadline 或延长租约。
所有写入口核对实际本地 Adapter Link；耐久完成时再次核对，不能让旧 Link 的迟到 Proof
在 reopen 后生效。JOIN、Identity、Security 和 Driver 必须共享同一个实际 Callback Gate。

本阶段支持一个固定信任根、一个预期 Device、一个专属 Link 和配置好的地址 Domain。
这不是未知设备的自动地址池分配器，也不实现 Authority 换主/分区自治。

## 3. 五事件与三道耐久门

| 顺序 | 线上动作 | 发送前必须成立的本地事实 | 接收端得到什么 |
| --- | --- | --- | --- |
| 1 | AuthorityProof / 0004 | 产品身份/信任根证明；不能冒充 Session | 已认证 Authority 事实；设备仍须自己的 Authority Record 回读 |
| 2 | DeviceProof / 0005 | 完整身份及选定安全参数的证据 | 已认证 Device 与冻结的安全选择，不是地址授权 |
| 3 | AddressOffer / 0006 | Authority 当前有效；精确候选 Binding 已经 Foundation 回读发布 | 可验证的地址提议，不是本地已持久化 |
| 4 | DeviceCommit / 0007 | 设备当前 Authority 有效；本地同一 Binding 已经 Foundation 回读发布 | 设备接受精确 Offer 的认证事实 |
| 5 | FinalDurable / 0008 | 已认证精确 Commit；Authority 预留 Binding 仍有效；Admission 消费真实 Binding View | 对端 Final 观察；还不是本地 Session 的耐久 Proof |

Authority 的地址预留在 Offer 之前就提交。这是有意的安全取舍：后续丢包或停止可以浪费
一个 Generation，但不能重新使用已经公开过的 Generation。删除/放弃网络事务不回滚
Foundation/Witness 历史，也不自动释放旧地址代际。

设备收到 Offer 后先调用 `persist_binding()`，由 Runtime 路由 Identity Requirement 到
Foundation；`finish_durability()` 消费精确 Proof 后才能 `publish()` Commit。Authority 同理
先预留，再 `publish()` Offer。Final 必须走 `publish_final()`，普通认证交换 API 不能绕过。

各端的持久化链如下，步骤均可在预算有限的多轮主循环中推进：

```text
Coordinator 调用 Identity/Security.prepare
    -> 不可变 Requirement
Coordinator 提交 Foundation 并绑定实际 PersistenceHandle
    -> Foundation.step(provider, now, budget)
    -> inactive slot / readback / commit marker / Witness / reload
Coordinator.finish_durability
    -> exact Proof + 原 Challenge + 当前 Authority/Link/Deadline
    -> 本端发布
    -> 显式 retire Proof
```

普通网络接收不驱动 Flash，也不会同步阻塞等待持久化。Foundation Provider 的异步 I/O
仍由其原有 ProviderGate、Token 和预算拥有；网络 TX Token 不充当持久化 Token。

## 4. Session 计划由 Final 精确推导

不能向该 JOIN 入口传入任意 `HandshakeCandidate`。Runtime 从最终认证 Transcript 推导
双方地址、Binding Generation、Principal、Session Generation、Suite、Key Generation，
并结合本机实际 Link 与产品本机 Policy，形成计划。计划本身不是权限，之后仍必须经过
Crypto Provider 验证及 Session Record 的实际回读发布。

共同规范：

```text
SessionDigest32 = SHA256(
    ASCII("UCN6-JOIN-SESSION-V1")       // 20 B，不含NUL
    || u32be(379)
    || canonical Final Transcript379
)
OriginFingerprint = SessionDigest32[0..16]
HopFingerprint    = SessionDigest32[16..32]

SessionDeadline = min(
    本机已发布 Binding Deadline,
    本机已发布 Authority Deadline,
    checked_add(最初本机 freshness 起点, 本机配置的 Session 最大寿命)
)
```

本机 Policy Generation、实际本机 Link 和本机 Deadline 是可信本地约束，不是从远端绝对
时间取得。Verifier 必须核对完整 Final 的真实性，并核对这些本地约束，不能只比较一个
摘要或接受固定万能 proof。Runtime 还要求 Session 使用的 Evidence 精确等于已验证/已排队
的 Final；设备的 Authority/Binding 持久化同样绑定对应已认证事件的 Evidence。

当前 Root 固定 H1 / Suite 16；Origin 允许 O1 / Suite 1 或 O2 / Suite 2、3，不自动降级。
O2 的实际密码实现仍由产品 Provider 提供，不把参考 Host HMAC 当作 AEAD。

## 5. 收发必须是两个不同 Key selector

Transcript 不增加字段。现有 `selected_e2e_key_id` 和 `selected_hop_key_id` 解释为产品
Manifest 预留的**方向槽对基址**，不是两方向共用同一把 Key：

| 方向 | Origin Key ID | Hop Key ID |
| --- | --- | --- |
| Device → Authority | selected E2E ID | selected Hop ID |
| Authority → Device | checked(selected E2E ID + 1) | checked(selected Hop ID + 1) |

两个槽均须在 1～65534；基址 65534/65535 拒绝，不能截断回绕。普通换代使用签名选择的
Key Generation；产品 Provider 必须把两个槽映射到独立方向密钥，并保证产品槽分配无冲突。
Authority 的本地 TX/RX 取向与 Device 正好相反。Fingerprint 和 Digest 两端相同；实际
Link Generation 是两端各自的本地值，不能硬要求相同。

C 的 `ucn_i_admission_session_seed()` 和 Rust 的 `join_session_candidate()` 是纯计划函数。
共享 Python / 原生 C / Rust Oracle 独立核对 Digest、方向槽和截止期。C Workspace 由
调用方静态持有；失败保持输出哨兵不变，拒绝别名、非法 Suite 和算术溢出。

## 6. Final 不等于两端同时 ACTIVE

Authority 只有在 Final 至少一轮实际发送成功并退休完 TX Token 后，才能开始本端 Session
持久化。Device 则须先收到并验证完整 Final。两端分别提交各自 Security Record192，消费
各自 exact reload Proof 后，`session()` 才返回本端 Active View。

这不是跨两台设备的原子提交。允许一端 Active、另一端仍 AwaitingDurability；本端 Active
也不证明远端应用已经接收业务。Final 没有新增“裸 ACK”或新 Opcode；原字节按原 Deadline、
固定间隔和次数重发。之后的认证 HELLO/Capability 可以观察对端准备情况，但不能伪造
远端耐久成功。本段尚未把 Bootstrap、Capability 和后续 Route/Flow 合并成全自动 Node。

正常网络重试不重新签发证据、不刷新租期、不重复提交 Foundation。同阶段重复片使用
完成缓存；进入下一事件才切换重组槽。首片丢失时后续片可以拒绝，完整重传后收敛。

## 7. 错误、停止和恢复

| 反例 | 必须结果 |
| --- | --- |
| Authority Record 回读损坏 | 不发布 Authority；不能完成后续 Binding/Session |
| Authority 地址预留回读损坏 | 不发送 Offer；不发布 Proof |
| Device Binding 回读损坏 | 不发送 Commit；Authority 预留不回滚 |
| Session 回读损坏 | 不激活 Session；网络 Final 不能替代本地 Proof |
| Final 迟到或相同 Final 反复到达 | 不续租、不重建本地 Challenge |
| Link reopen 后迟到 Proof | 拒绝激活/查询旧链上的 Session |
| TX UNKNOWN / 取消不确定 | 保留或隔离原 Token，不能假报已退休；不因迟到 completion 擦除不确定历史 |
| 停止时 Foundation 仍在工作 | Session Fence；保留实际请求，完成后仅退休，不重新发布权限 |
| 重启后恢复完整三个 Record | 只恢复正文/高水位；不恢复 Challenge、ADMITTED 或 Active Session |

Foundation 明确拒绝尚未接纳的提交时，Identity 候选走原有 abort；Session 候选 Fence 并
退休，避免无请求却占槽。已接纳的请求则必须保留 Handle 到明确终态。Root 停止不回滚
地址预留，也不会替产品擅自 reset 整个共享 Identity Owner；新事务仍须受其槽位、历史和
恢复规则约束，不能靠清零旧 Owner 重获权限。

Root 的寿命必须覆盖已经接纳的 Foundation 请求；停止后继续推进 Foundation 并调用
`retire_stopped_durability()`。不得直接丢弃 Root 来规避待退休义务。Driver UNKNOWN 的
隔离状态同样不能用“补一个成功完成”洗掉；没有明确物理退休证明时保留 Holder 并进入
产品恢复流程，不宣称已经 quiescent。

## 8. 费用和裁剪

普通 Data/C1～C5 头新增 **0 B**。保持八类首次入网消息：初始三帧加五个认证逻辑事件。
在 A1、62 B 分块、每份 Evidence32、无重传时：每个认证事件 412 + 7×65 = 867 B，首次
完整八类交换共 `265 + 5×867 = 4600 B`。这是建链一次性 UCN 帧字节，不是每条数据负担，
也不包含 MAC/PHY、Carrier、额外证书获取或后续 Capability 交换。Evidence128 时相应
上界为 `265 + 5×(508 + 9×65) = 5730 B`；丢包重传另计。

关闭 `bootstrap-join` 时，新增挑战起点缓存、Evidence 缓存和 JOIN 协调状态不进入
`bootstrap-runtime` 的相关结构。两 Feature 都不依赖 Service、Realtime、Group、Cluster
或 Capability Owner。Host sizeof、交叉编译和 Host 栈门禁分别记录，不能推断 MCU 任务栈。

## 9. 验证入口

- Rust 定向：`cargo test -p ucn-runtime --test bootstrap_runtime`。
- C 定向：`ucn_v6s_admission_tests`，含 seed 边界、别名与零写入。
- 三方共同算法：`python tools/v6/check_rust08_09_codec_oracle.py`。
- 裁剪：`python tools/v6/check_rust_optional_boundaries.py`。
- 完整软件矩阵与不可变快照：`python tools/v6/run_rust_software_matrix.py`。

本段实施记录、逐项/整体自审及最终计数见
[耐久发布自审报告](../../../08-实现与验证/版本演进/UCN_V6S_RUST_SW_02_Bootstrap耐久发布与Session建立自审报告.md)。
独立外审、生产密码 Provider、真实 Flash/Witness/掉电、物理 Bearer 和目标资源/性能仍 HOLD。
