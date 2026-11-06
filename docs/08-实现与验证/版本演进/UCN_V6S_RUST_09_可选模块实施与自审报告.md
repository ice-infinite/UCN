# RUST-09｜Realtime、Group、Cluster 可选 Owner 阶段

> 日期：2026-10-13。状态：`PARTIAL / OWNER HOST SELF-REVIEW PASS / PRODUCTION HOLD`。
> 不宣称已完成 C5/H3、全部 Cluster 控制协议或生产四报文 Runtime。

## 1. 依赖与证据等级

三个独立 crate 均为 `no_std`、固定容量、禁止 unsafe，不直接互相链接。
`ucn-core` 不依赖三者。LOCAL Realtime、Static Group 等最小组合不引入 Foundation。
测试 Coordinator 路由真实 Foundation Requirement/Proof；真实外部 Driver/Flash 尚未接入。

本阶段的 sealed proof 是防止正常 Rust 调用方拼装字段的类型能力，不是可直接发送到远端的
密码学证书，更不是防原始内存覆盖、防掉电或物理攻击的证明。

## 2. Realtime

### 2.1 已实现链条

1. 16 B Envelope 与 12/24 B 时间报文 Body 的严格 Codec。
2. 128 B Domain Record、先持久化后启用 Generation、重启恢复后保持 Loading 直至换代。
3. 六类 known uncertainty、checked aggregate、向上量化及漂移/滤波残差保守覆盖。
4. 固定中位数窗口、ACQUIRING/LOCKED/HOLDOVER/UNSYNCED/FAULT 与当前时刻检查。
5. 调用方共享 CallbackGate、真实 TX/RX reservation 生命周期及原子打点 ingress。
6. 原子的完整帧＋EventKey＋Timestamp RX 项；Driver 退休失败时不释放该项。
7. Master 持有 T1/T4、Member 持有 T2/T3，各自保留本地 release obligation；WireKey
   关联 domain/generation/sequence，不把某节点本地 Token 发送给另一节点。
8. REQUIRED 的 E2E+ACL、combined uncertainty、future-skew、半开 Deadline 两级门禁。

实际 Host 四报文测试调用 `TimedLink::reserve/submit/timestamp/receive_capture`、完整 RX
队列、FollowUp/DelayRequest/DelayResponse Codec 与 SyncOwner，不是伪造四个 EventKey。
但远端认证身份/冻结 Path 仍通过可信 Coordinator SPI 输入，并非完整 Security raw dispatch。

### 2.2 关键安全边界

- UNSYNCED 清滤波，不清同代际采样/发布时间高水位。候选倒退由 ingest 本身进入 FAULT，
  不先短暂暴露 LOCKED。重启必须换 Generation，不能沿用失去 RAM 高水位的旧代际。
- uncertainty 包括窗口样本误差、与滤波输出的残差、旧样本漂移、四个打点误差及舍入上界。
  source stamp 另要求明确且非零的 sample capture bound。
- 无可信 asymmetry 的固定 Path 只产生 Diagnostic，不产生有效 TimeSample；动态 Route
  拒绝创建同步 pending。REQUIRED 不接纳远端 SOURCE_HOLDOVER。
- TX 同一 reservation 不能重复 submit。跨 Link 控制 API 共用真正原子 Gate；ISR 的唯一
  例外是原子 ingress 发布完成事实，不允许从 Driver callback 控制另一个 Owner。
- Release 使用 `peek -> real Driver retire -> sealed RetirementProof -> ACK`。失败保留 key，
  错 key、重复 ACK、旧 Claim、错误 Link instance 全部拒绝。
- Atomic RX release 验证与槽回收受独立 gate 保护，防止复制旧 Claim 的并发释放跨越槽位代际。

对应测试：`rust/crates/ucn-realtime/tests/realtime.rs`，含线程并发、短 Buffer、表满、重复
发布、失败退休重试、旧凭据、未知 asymmetry、倒退立即 FAULT 和 Deadline 边界。

## 3. Group

### 3.1 Static Group 已实现

只读 Manifest 固定成员、Principal/Binding/Sender Slot、Key/Policy/Tree Generation、
作用域和配额。发送冻结成员快照，有限 fanout，不复制全网消息。

LocalOnly、Any、All、加权 Quorum 和指定 Subset 的结果分别计算。只有精确成员终态
Receipt 才构成远端成功；本地接纳或构造 Attempt 不算远端交付。修改成员/Key/Tree/Policy
须 Fence；Manifest 已退休槽不会重新使用。

### 3.2 Dynamic catalog 的历史原型（已替换）

2026-10-13 共用合同整改已移除以下 GRC1 原型，替换为独立 Realm allocator 与逐槽 v2
Context Record；最新结论见[共用合同推进报告](UCN_V6S_C_Rust共用合同第一轮收口与Rust推进自审报告.md)。
本节保留原先实验的边界说明，不表示该 Schema 仍可编译或进入生产 Manifest。

新建一个固定容量的 Realm catalog，用真实 Identity Owner 签发的 `RealmAuthorityView`
及 Foundation reload Proof 控制 Group-ID 单调分配、退休及恢复。每次授权查询都重新验证
当前 Authority；不能用缓存的 Active 状态代替权限。Authority 过期后确认 durable truth，
但保持 Fenced。新 Owner 跳过恢复不能重新从 Group-ID=1 分配。

**这一 catalog 使用实验 Schema `0x0902`、`GRC1`，不是 C 的逐 Context Record。**
它验证“固定表＋单调高水位”模型，不在共同生产 Manifest 注册，不证明 C↔Rust Flash
Record 互换。正式接线前必须统一 Record/allocator 归属合同；不能将本原型直接标记为
最终架构完成。此差距是软件合同问题，不是实机问题。

### 3.3 未实现

C5/H3 群组安全、Tree Stage/Ack/Commit、认证成员变更/换钥的线上 Body 与共同 Runtime
dispatch。本轮 Static Group 的 fanout 是各成员单播引用，不冒充已经实现组播 Tree。

## 4. Cluster

### 4.1 已实现 Owner 模型

- canonical Stable/Joint Config 与当前双 quorum；每次 Authority 输出先按 now preflight。
- 144 B Header＋双 Config/完整 Vote 数组的 v2 Record、真实 Foundation reload 后发布。
- Member 的 Session/Capability/Digest/Flow/Link/Lease 变化立即失效历史票；重新准入必须重投。
- Pending Record 保持不可变；提交过程中发生重新准入，独立失效列表使新回读不能复活旧票。
- 指定 Backup Takeover、显式 Recovery、精确当前 quorum commit；EPOCH_DURABLE 单向终态。
- 双 Buffer Snapshot：BEGIN=0、MEMBER=1..N、END=N+1、完整规范 digest 后交换。
- Protected voter 缺失/Removed 永久失效；显式 Suspect 才可 grace，旧 assignment 不因
  新 ADMITTED 输入而复活。Joint 的新 voter 也受保护。
- Handover Ready 使用当前目标成员、完整 Capability/Flow/Link 依赖与 Snapshot；旧 Head
  精确复验后持久化 Fence，reload 前不能发出 HandoverCommit；重启也不得恢复旧 Authority。
- Realm/Cluster 分域 Directory、精确代际/租约与最小 lineage 检查，不比较异簇 Term。

测试以真实 Foundation 分别驱动 Head/Backup，两对象的 sealed proof 经测试 Coordinator
转交。它证明本地软件状态机，不证明跨 MCU 的认证证书传播和两个本地时钟可直接比较。

### 4.2 保留的软件边界

完整 voter-side persist-before-vote 证书签发、认证 Wire 票/Ready 的解码与验证、Snapshot
Delta 同步、Merge/Dissolve/完整 lineage 回收及完整 Federation 协议尚未接线。
`VoteEvidence`、`MemberFact`、`DirectoryEntry` 是可信 Coordinator SPI 输入，不是认证 API；
调用方不能把公开 DTO 构造成功当成证明已通过 Security。只有统一 ingress 后才可对外开放。

## 5. 两轮自审与结果

第一轮逐模块回读及第二轮恢复/超时/容量/别名/重入逆向回读，新增或修复：

| 发现 | 修正与回归 |
| --- | --- |
| 滤波残差或采样捕获误差低估 | 聚合旧窗口漂移/残差，source 明确 capture bound |
| 同代际重锁倒退 | 保留高水位；ingest 立即 FAULT |
| Token 无真正生命周期或跨实例错配 | reservation 状态机、共享 Gate、明确本地实例 |
| 退休失败丢失 release key | sealed retire proof 与 peek/ack |
| 旧 RX Claim 并发回收新槽 | 释放验证与 CAS 串行化、并发/旧 Claim 回归 |
| 恢复未绑定确切正文或新 Owner 跳过恢复 | body digest 复核、durable generation 门禁 |
| Pending vote 期间重新准入复活旧票 | 失效列表在回读后清票、对抗顺序回归 |
| Handover Ready 缺完整目标依赖 | 补 Digest/Flow/Link、阶段与 Config Generation 精确绑定 |

全 Workspace Debug/Release/MSRV 各 **166/166**，裁剪检查 9 组合，三个 Optional Owner 不互链；
详细原始证据见 [RUST-10 软件报告](UCN_V6S_RUST_10_软件矩阵与后续门禁.md)。

当前结论仅为已列明 Owner 切片自审通过；`RUST-09` 顶层仍为 PARTIAL，没有外审或生产 GO。
