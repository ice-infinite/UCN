# UCN V6S RUST-06 Admission 与 Capability 实施及自审报告

## 1. 结论

```ini
RUST-06 = DONE / SELF-REVIEW PASS
RUST-07 = ALLOWED TO START
EXTERNAL_REVIEW = DEFERRED UNTIL THE COMPLETE RUST WORKSTREAM
```

本阶段完成 Rust 独立实现中的 Identity、Dynamic Admission 和 Capability 三个单写 Owner。
它们分别负责 Authority/Binding、UNBOUND Bootstrap/JOIN、认证 Peer 能力事实和 Profile/Resolver，
不链接 C archive，也不通过 FFI 转发 C 协议逻辑。

结论只适用于 Host 软件模型、独立 Wire Oracle 和两个 Cortex-M `no_std` 编译目标。当前没有把
三个 Owner 接入统一动态 Runtime；没有证明生产密码 Provider、真实 Flash 掉电、随机数质量、
ESP32-S3 任务栈、物理链路、吞吐、延迟或长期运行。

## 2. 权威输入与范围

实现逐项核对以下冻结合同：

1. Identity/Admission/Security/Persistence 的 Owner 隔离和 Coordinator 唯一跨模块编排边界；
2. Authority Generation、Binding Generation、Lease、地址 ABA 和 persist-before-use；
3. Bootstrap 一跳、不转发、Cookie 前无放大、固定认证前资源和严格 JOIN transcript；
4. Capability 68 B Record、24 B Summary、20 B Query 与 Profile 交集规则；
5. 只读 Contract Resolver 的 typed dependency、确定性和零 Driver/Provider 副作用；
6. `no_std`、禁止 `unsafe`、固定容量、失败关闭、不 lazy-evict 和 C/Rust 共享 Oracle。

不属于 RUST-06 的内容：已绑定 Peer Reauth、动态 Route/Flow、Reliable/Transfer、Service、
Realtime、Group、Cluster 以及生产 Runtime 接线。

## 3. crate 与依赖边界

```text
ucn-types ──────────────┐
ucn-owner ──────────────┼──> ucn-identity ── typed BindingView ──> ucn-admission
ucn-persistence ────────┘

ucn-security ── AuthenticatedPeerView ──> ucn-capability
ucn-types ───────────────────────────────> ucn-capability
```

- `ucn-identity` 依赖 Persistence DTO 和 Proof 类型，但不持有 Persistence Owner/Provider；
- `ucn-admission` 只读取 Identity 签发的不可变 `BindingView`，不调用 Identity Owner；
- `ucn-capability` 只读取 Security 签发的 `AuthenticatedPeerView`，不调用 Security Owner；
- Profile Resolver 是纯计算，不接触 Owner、Provider、Driver、队列或时间源；
- `PersistenceProof`、`BindingView`、`AuthenticatedPeerView` 的来源字段不可由下游 crate 直接构造。

统一 Runtime 接线时必须形成：

```text
Business/Driver fact
  -> Coordinator
  -> exact Owner
  -> immutable Requirement/View/Event
  -> Coordinator
  -> next exact Owner
```

任何缓存 View 都只是“某时刻已验证事实”，不是以后使用时的永久授权。

## 4. RUST-06-01：Identity、Authority、Lease 与 Binding

`ucn-identity` 实现：

- 单一 `Principal`、Realm、Address Authority Epoch 和 Binding Certificate 模型；
- `start <= trusted_now < deadline` 的半开 Lease；
- Producer 慢钟 ppm、完整 timer resolution 和两次读取误差的 checked 保守扣减；
- 未知/零计时器参数、加减溢出、未来起点、到期对象全部失败关闭；
- Authority 192 B Record 与 Binding 96 B Record；
- `prepare -> immutable persistence requirement -> bind handle -> exact reload proof -> activate`；
- Authority 换代立即 Fence 当前 Binding，防止旧地址租约跨 Authority 代际复活；
- 候选尚未绑定 Persistence handle 前允许 Abort，并逐字段恢复先前有效状态；
- Binding challenge 使用 slot 与 generation 精确匹配，不以 Principal 相同代替同一事务。

### 4.1 分项自审

首次正向实现后，从“失败后旧状态是否还可用”反向回读，发现候选 prepare 会覆盖旧 Authority/
Binding。整改为固定 staging 保存旧状态，并增加 Authority/Binding Abort 恢复回归。

从“下游是否能伪造 proof/view”回读，发现公开字段可由其他 crate 使用 struct literal 构造。整改为：

- `PersistenceProof` 全字段私有，只提供只读 getter；
- `BindingView` 与 `AuthenticatedPeerView` 的来源字段私有；
- View 绑定 Runtime instance、签发 Owner instance 与父代际。

结论：Identity 分项自审通过。

## 5. RUST-06-02：认证前 Bootstrap 与 Cookie

`ucn-admission` 的认证前布局：

| 对象 | 固定长度/上限 | 关键绑定 |
| --- | ---: | --- |
| HELLO | 40 B | Bootstrap identity、Link、device nonce、能力摘要 |
| COOKIE_CHALLENGE | 40 B | txid、Authority nonce、Cookie、绝对 Deadline |
| HELLO_COOKIE | 82 B 固定部分 + 1～128 B Evidence | 原 HELLO、Cookie、Evidence |
| Bootstrap transcript | 379 B | 六阶段全部公共字段和 prior hash |
| Logical event | 最大 508 B | Opcode、phase、payload length |
| Fragment | 36 B header + 128 B data，最多 4 片 | Wire key、Opcode、phase、总长度、Deadline |

Cookie 只有一个公开原子签发入口，顺序固定为：输入合法性、每 Link/Principal 配额、无放大预算、
Provider 认证 Cookie，最后才占 pending。失败不得部分占槽或增加输出计数。

### 5.1 分项自审

对每个编码入口检查“首次写入前是否完成全部验证”；对每个解码入口检查 exact length、reserved、
枚举、方向和输出零写。增加 Evidence 128 B 成功和 129 B 拒绝测试。

容量测试证明：过期 pending 不在新 HELLO 路径被隐式驱逐；满载必须 `NoSpace`，Owner 只能由显式
maintenance/parent invalidation 回收。

结论：Bootstrap/Cookie 分项自审通过。

## 6. RUST-06-03：JOIN transcript、FSM 与分片

Admission pending 由完整 `AdmissionKey`、Link instance、txid、双 nonce、Authority Epoch、Suite、
绝对 Deadline 与阶段共同标识。六阶段只允许精确相邻推进；FinalDurable/Aborted 均为终态。

重复输入只能在全字段和 payload 完全相同时幂等返回，不能刷新 Deadline。更高阶段、换 Link、换
Authority、换 Suite、错误 txid/nonce、到期、相同分片索引不同字节全部拒绝且不改状态。

### 6.1 分项自审

全体回读发现旧实现只要求下一阶段 `prior_messages_hash` 非零，没有要求和已冻结 transcript 一致。
这会允许阶段间偷换历史。现已改成 exact equality，并补 mutation-before-provider 回归。

同时把 `bootstrap_header_contract` 从“非零”收紧为当前冻结值 1，防止未来未协商布局误入当前 FSM。

结论：JOIN/FSM/Fragment 分项自审通过。

## 7. RUST-06-04：Binding durable 后 Admission

Final Admission 只接受 Identity Owner 签发的当前 `BindingView`。校验同时绑定：

- Runtime instance 与 Identity Owner instance；
- Principal、Realm、Address、Binding Generation；
- Address Authority Generation；
- Binding Record generation/transaction/body digest；
- 当前半开 Lease。

任一字段不符、proof 非 reload 后签发、Authority 已换代或 Lease 到期都不能产生 `AdmittedView`。

结论：Identity→Persistence→Binding→Admission 集成分项自审通过。

## 8. RUST-06-05：Capability Codec 与认证 Peer Cache

Capability Wire 合同：

| 对象 | 长度 | 说明 |
| --- | ---: | --- |
| Capability Record | 68 B | Peer/Link 上限、Suite、Feature、Realtime 与 reserved |
| Capability Summary | 24 B | Generation + salted CRC32C digest |
| Capability Query | 20 B | 请求 Generation + 已知 digest |

Owner 在任何 Query 放大或槽写入前验证 typed Summary/Record。缓存只接受同 Runtime、Security Owner、
Realm 的当前认证 Peer View，并绑定 Principal、Address/Binding Generation、Peer Session Generation、
Link Instance Generation 和绝对 Session Deadline。

同一 Principal 换 Session/Binding/Link 时，必须先显式失效旧父代际；不能直接覆盖旧槽。过期槽不
lazy-evict：Nano 满两槽后即使两者过期，第三个 Peer 仍得到 `NoSpace`；只有精确父代际失效后可用。

结论：Capability Codec/Cache 分项自审通过。

## 9. RUST-06-06：Profile 与 Contract Resolver

`ProfileSelect::build()` 重新计算本地和 Peer Capability digest，再求 Feature、Hop/E2E Suite、
Message Class、RX Window、Transfer 并发和 Realtime Mode 的交集。所有 REQUIRED 字段只能满足或
拒绝，不能静默降级。

`ProfileAck::verify_exact()` 只用于 Security 已认证后的 typed payload 精确比较，同时匹配完整
ProfileSelect 和调用方给出的非零 transcript digest；它不冒充密码认证。

Resolver 的固定判决顺序为：Identity、Security、Capability/Profile、Path、Reliable、Realtime、
Transfer 等 typed dependency，然后才检查队列/Adapter 等瞬时资源。相同输入得到相同计划；函数
不占槽、不提交 Driver/Provider、不消耗 Sequence。

### 9.1 分项自审

初版在 typed dependency 前检查 TX/队列资源，会把“缺 Security/Path”和“当前背压”错误折叠成
Resource。现已按冻结优先级调整，并补同时缺依赖和资源时必须先报告 typed dependency 的回归。

初版 Profile Ack 只检查摘要非零，不能证明它对应当前选择。现已要求 expected transcript digest
精确相等，并补错误/零摘要拒绝。

结论：Profile/Resolver 分项自审通过。

## 10. RUST-06-07：独立 Oracle、Negative 与资源

共享语言无关 fixture：

`rust/tests/conformance/v6s_admission_capability_v1.h`

独立 Python oracle：

`tools/v6/check_rust06_admission_capability_oracle.py`

Oracle 不调用 Rust Codec，而是使用独立 `struct` 大端布局和独立 CRC32C 重建七个对象：HELLO、
Cookie Challenge、HelloCookie、Capability Record、Capability Digest、Summary、Query。Rust 测试
直接读取同一 C header fixture，避免 encoder/decoder 同错自洽。

Host 固定对象尺寸：

| Owner | Nano | Lite | Full |
| --- | ---: | ---: | ---: |
| Identity | 2,832 B | 7,440 B | 13,584 B |
| Admission | 1,264 B | 4,912 B | 9,776 B |
| Capability | 472 B | 1,528 B | 2,936 B |

单个 `Reassembly` 为 576 B。所有尺寸均低于 crate 内静态上限；这些是 Host ABI 布局，不是目标
MCU 的最终 RAM、栈或 Flash 数据。

结论：Golden/Negative/Capacity 分项自审通过。

## 11. RUST-06-08：两轮全体交叉自审

第一轮按正常数据流回读：

```text
Driver fact -> Cookie -> JOIN FSM -> Identity persistence -> BindingView
            -> authenticated Session -> Capability cache -> Profile -> Resolver
```

第二轮按失败和证明流反向回读：

```text
Resolver Reject <- stale Capability <- stale Session/Binding <- wrong proof
                <- torn/failed persistence <- changed transcript/Link/Authority
```

重点检查并关闭：

1. 同数值 ID 跨 Owner/Runtime 误用；
2. proof/view 公开字段伪造；
3. 同 Principal 错误 challenge 或缓存父代际复用；
4. prior hash、Profile digest、Ack transcript 的部分绑定；
5. 过期对象被输入路径隐式回收；
6. 资源错误掩盖更早的安全依赖；
7. 终态、重复、Deadline 和错误输入产生副作用。

本轮未发现仍开放的软件 P0/P1，但这只是自审结论，不替代后续外部审计。

## 12. 验证矩阵

| 门禁 | 结果 |
| --- | --- |
| `cargo test --workspace` | 100/100 PASS |
| `cargo test --workspace --release` | 100/100 PASS |
| `cargo +1.85.0 test --workspace` | 100/100 PASS |
| `cargo clippy --workspace --all-targets -- -D warnings` | PASS |
| `RUSTDOCFLAGS=-Dwarnings cargo doc --workspace --no-deps` | PASS |
| `thumbv7em-none-eabi` | PASS |
| `thumbv7em-none-eabihf` | PASS |
| RUST-06 independent Wire oracle | 7/7 PASS |
| C Full Debug，默认 Feature（Persistence OFF） | 47/47 PASS |
| V6 当前文档门禁 | 107 documents / 278 links PASS |
| V6S 合同冻结门禁 | PASS |
| `git diff --check` | PASS，仅换行提示 |

另行启用 C Persistence 后，51 项代码/功能门禁通过；唯一失败是历史 IMPL-02 精确外审候选哈希因
后续任务表和操作记录变化而按设计拒绝。该历史清单没有被重写，也不把它计作本阶段功能失败。

## 13. 未证明事项与下一阶段约束

RUST-07 可以开始，但必须遵守：

- Route Owner 只能消费当前认证、当前 Binding、当前 Capability 的不可变事实；
- 每次安装/发送前由 Coordinator use-time 重验，不能把缓存 View 当永久权限；
- Route/Flow 的 Generation、Path 和 Security Context 必须独立持有，不复用 Admission pending；
- Capability 只能说明能力，不等于 ACL、Authority、资源 reservation 或执行许可；
- 失败不得修改已发布旧 Route，不得 lazy-evict 过期安全/身份槽。

仍未证明：真实 Flash/Witness、断电恢复、生产密码实现与安全元件、硬件随机数、侧信道、物理
Bearer、ESP32-S3 固件、任务栈水位、吞吐/延迟/功耗和长稳。RUST-06 的自审通过不得被写成生产
放行或外部审计 GO。
