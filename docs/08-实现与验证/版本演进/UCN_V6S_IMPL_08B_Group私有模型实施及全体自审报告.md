# UCN V6S IMPL-08B Group 私有模型实施及全体自审报告

> 日期：2026-09-24  
> 分支：`v6-simplified-rust`  
> 结论：`OFFLINE PRIVATE MODEL IMPLEMENTED / SELF-REVIEW PASS / AUDIT HOLD`

## 1. 目标和非目标

本阶段把 Group 从架构占位推进为可执行的私有 C 模型。目标是用固定内存证明：短 Group
身份不会跨 Realm 混淆，动态身份不会因退休而 ABA，发送使用冻结成员快照，安全接收在业务
副作用前完成认证提交，并且 Group 不成为 Realtime 或 Cluster 的隐藏依赖。

本阶段不接公共 Runtime，不实现 C5/H3 Wire，不直接调用 Security、Route、Persistence、
Adapter、Realtime 或 Cluster Owner。内部头不安装，详细 archive 不进入产品链接面。

## 2. 模块边界与 Owner

```text
Security / Route / Authority / Persistence immutable facts
                         │
                         ▼
                    Coordinator
                         │ typed facts, handles, proof
                         ▼
              Group Owner（一个 Owner 一个 Realm）
                 │          │           │
             Context      GroupSend    GroupRX
                 │          │           │
                 └──── frozen attempt / stable receipt ────┘
```

`ucn_v6s_group` 只链接 `ucn_common`。Owner 初始化时固定非零 Runtime、Realm、Owner Instance
和 caller-owned lock；所有 Context 的 Realm 必须精确匹配 Owner。若产品加入多个 Realm，
必须建立独立 Owner，而不能让 RX 或 dependency event 猜测短 Group ID 属于哪个 Realm。

## 3. IMPL-08B-00：容量、Feature 与物理解耦

| Profile | Static | Dynamic | Members | Sends | Attempts | RX | Owner bytes | Record bytes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Nano | 1 | 1 | 4 | 2 | 8 | 4 | 3536 | 224 |
| Lite | 4 | 2 | 8 | 8 | 32 | 16 | 13504 | 336 |
| Full | 8 | 4 | 16 | 16 | 128 | 64 | 45472 | 560 |

所有表和 staging 都位于 caller-owned Owner 内，无动态内存。`UCN_FEATURE_GROUP=OFF` 时不
生成详细 Group archive、详细测试和 Persistence 集成测试；统一 scaffold 仍只是零业务状态的
架构占位。Archive 与 source-boundary 检查确认 Group 不直接链接其他业务 Owner。

分项自审：通过。

## 4. IMPL-08B-01：Static 与 Dynamic Context

Static Group 来自认证且 anti-rollback 的产品 Manifest；同槽重复安装必须逐字段完全相同。
Dynamic Group 的创建、更新和退休要求当前、已认证、未 Fence、满足 Quorum 且未到半开 Lease
Deadline 的 Realm Authority。新 ID 只能来自 `checked_next(dynamic_id_high_water)`。

动态更新严格推进 Group Generation，Policy/Member Generation 不允许回退。Static Context
被 Fence 后只接受同 Realm/Group ID、checked-next Group Generation 且 Policy/Member 不回退的
新认证 Manifest；旧句柄随安装失效。Generation、句柄 Generation 或 ID 高水位到顶均失败
关闭，不回绕。Context 比较逐字段进行，不依赖结构 padding。

分项自审：通过。关闭了跨 Realm 短 ID 歧义和 padding `memcmp` 风险。

## 5. IMPL-08B-02：Record 与 Persistence Foundation

Dynamic Group Record 使用固定 big-endian 布局，包含 Realm、Group/Policy/Member Generation、
Endpoint/Opcode、Security/Tree 引用、动态 ID 高水位和完整成员。流程为：

```text
admin_prepare
  → immutable typed Requirement
  → Coordinator / Persistence Foundation
  → record + marker + witness + reload
  → exact published-record Proof
  → accept_proof 发布 Context
```

Group 分开保存 canonical Group body digest 与 Foundation published record digest。Proof 核对
Persistence Handle/Owner、Domain、Transaction、Record/Witness Generation、Runtime/Caller、
Schema、Operation Kind、正文长度、published digest 和半开 Deadline。真实集成测试证明两种
digest 不相同仍可正确提交，避免把业务正文摘要冒充耐久记录摘要。

分项自审：通过。

## 6. IMPL-08B-03：冻结发送计划

`send_begin` 原子冻结成员、成功范围、Policy/Member/Key/Tree Generation、Endpoint、Opcode、
Payload digest 和截止时间。支持 LOCAL_ONLY、ANY_MEMBER、ALL_MEMBERS、QUORUM 和 SUBSET。

存在当前 Tree 时只建立一个携带完整 Tree 引用的 Attempt；否则只在成员数不超过 Manifest
fanout limit 时建立逐成员单播 Attempt。资源不足时不发布半个 Send。成员 receipt 必须精确匹配
Send ID、Group Generation、成员地址/Binding/Principal 和 Payload digest；重复或冲突终态拒绝。

分项自审：通过。Tree 与有界单播均有正式回归。

## 7. IMPL-08B-04：两阶段接收

安全 Group 的 RX preflight 要求 E2E 认证、Source ACL、精确 Sender Slot/Principal、当前 Key
Generation 和未过期 Deadline。Relay attempt 在 preflight 后保持 HELD；只有精确 Security
commit 成功才发布本地业务和 Relay obligation。公开静态 Group 不伪造密码 Replay，而使用
独立 retained business receipt 抑制重复副作用。

稳定 receipt 可对精确重放返回相同结果；相同 Send ID 但不同摘要或身份视为冲突。应用完成、
Relay attempt 和 receipt 退休均有独立句柄 Generation，迟到句柄不能清除新对象。

分项自审：通过。

## 8. IMPL-08B-05：Fence、Deadline 与退休

Policy、Member、Key、Tree 或动态 Authority 任一不再精确当前时，Context 先进入 FENCED，
新发送/转发停止，未提交 Attempt 取消；已提交 Driver obligation 不伪造取消，继续等待终态退休。
轮转 `step` 用持久游标和显式预算清理过期 Send/RX，`now == deadline` 固定过期。

Dynamic retire 经 exact durable Proof 后释放 `active_groups[]` RAM 槽，但高水位保留。Nano 回归
覆盖 `ID 1 → durable retire → 同一 RAM 槽创建 ID 2`，旧句柄失效且 ID 1 永不复用。Reload
退休记录同样只推进高水位/清除旧活动槽，不重新占用永久 tombstone RAM。

分项自审：通过。该修正使“固定内存”与“全生命周期 ID 不复用”同时成立。

## 9. IMPL-08B-06：验证矩阵

| 环境 | 结果 |
| --- | --- |
| Windows GCC Full 全量 | 92/92 |
| Windows GCC Full/Lite/Nano Group 定向 | 各 5/5 |
| MSVC 19.29 Release | 5/5 |
| WSL GCC ASan/UBSan | 6/6 |
| WSL Clang 18 Release | 6/6 |
| WSL GCC `-fanalyzer -Werror` | 6/6 |
| WSL GCC TSan non-PIE 双线程 | 2000 次/线程 |

GCC 栈门禁：单函数最大 `240 B`，低于 `384 B`；最长静态调用链 `416 B`，低于 Nano
门限 `768 B`。这些数字
不包含未来 Coordinator、Driver、密码 Provider 和 RTOS 调用链，目标任务栈仍需实机水位。

## 10. 两轮全体自审

第一轮按“Realm/Context→Persistence→Send→Attempt→Receipt→RX→Dependency”正向追踪，确认
所有跨 Owner 输入都是不可变 typed facts/handles，发布点前均完成容量、代际和权限检查。

第二轮从异常输入逆向检查：错 Realm、过期 Authority、错 Proof、摘要混淆、成员漂移、Tree
漂移、Key 撤销、同 ID 冲突、Deadline、表满、句柄重放、动态退休和 Feature OFF。确认失败
不发布半 Context、半 Send、半 Relay 或伪造终态。

自审期间主动发现并关闭：canonical/published digest 混用、padding 比较、动态退休永久占槽、
Owner Realm 未冻结、Static Fence 后无法安装 checked-next Manifest、Tree 计划漏测、公开组
进行中重复可能二次占槽和部分别名门禁遗漏。
当前未发现已知软件 P0/P1；结论仍为自审，不替代独立外审。

## 11. 未放行边界

- C5/H3 Group Wire、真实多播和物理 Tree 转发；
- 公共 Runtime/Coordinator 与用户 API；
- 生产 Security/Route/Persistence Provider；
- 真实 Flash、掉电、密码硬件、MCU RAM/栈/ISR/SMP；
- 丢包、成员 churn、长稳、吞吐、功耗与实机攻击注入；
- IMPL-08C Cluster 及 Cluster→Group 可选事实接线。

因此本阶段只可标记 `SELF-REVIEW PASS / AUDIT HOLD`，不能据此宣称 Group 产品完成或生产放行。
