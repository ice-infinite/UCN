# UCN V6 简化版 IMPL-07 Service、Operation 与 QoS 私有模型实施及全体自审报告

## 1. 结论

`IMPL-07 = OFFLINE PRIVATE MODEL IMPLEMENTED / SELF-REVIEW PASS / AUDIT HOLD`。

本阶段在 C99、固定容量、零动态内存、单 Owner 写入与 Coordinator 跨 Owner
路由约束下，完成了：

1. Client Request/Result 与 Server retained receipt/幂等结果回放；
2. Q0～Q3 `6:3:2:1` 有限等待调度、Latest、源/流配额与 Deadline 回收；
3. 掉电不确定语义的 Durable Operation Journal；
4. 64-bit Operation ID 持久化区间预留和重启跳号；
5. Operation Journal 与 Operation ID 独立 Domain 在真实 Persistence Foundation 中的提交、
   Witness、reload 和下一代提交。

这是私有 Host 离线模型，不是生产接线。`ucn_v6s_service` 为 `EXCLUDE_FROM_ALL`
目标，不安装、不导出、不进入 `UCN::simplified`。公共 Runtime、Adapter、Security、
业务执行器、真实 Flash/Witness、MCU 和物理 Bearer 均未因本阶段获得放行。

## 2. 模块边界

| 对象 | 位置 | 当前职责 |
| --- | --- | --- |
| 私有类型与 API | `src/internal/ucn_service.h` | 固定容量 Owner、Request/Receipt/QoS/Operation/ID DTO；不安装。 |
| 内部辅助 | `src/service/ucn_service_private.h` | 锁、Handle、精确查找与辅助；不跨模块发布。 |
| Service Owner | `src/service/ucn_service.c` | Request/Result、retained receipt、有界维护。 |
| QoS Owner | `src/service/ucn_qos.c` | Q0～Q3 调度、Latest、配额、Deadline 与 sendable Handle 所有权。 |
| Operation Journal | `src/service/ucn_operation.c` | Journal 状态、canonical Record、Requirement/Proof、恢复与退休。 |
| Operation ID | `src/service/ucn_operation_id.c` | ID 区间预留、发号、reload 跳号和耗尽处理。 |

该 target 只链接 `ucn_common`。Service 不持有 Persistence、Transport、Security、Adapter、
Runtime 或 Coordinator Owner 指针。跨 Owner 交换只能是不可变 typed Requirement、Proof、
Failure、View 或 sendable Handle。

## 3. Service Request/Result

### 3.1 精确交互键

每个 Request 和 Receipt 同时绑定：

- Client/Server Address 与 Binding Generation；
- Client/Server Principal；
- Session/Key/Policy Generation 与 Origin Security/ACL 事实；
- Realm、Service ID、Opcode 和 64-bit Operation ID。

重复判定不只比较 Operation ID。Result 必须按完整反向键命中原 Request；错
Source、Binding、Session、Service、Opcode 或 Operation ID 均在首次状态写入前拒绝。

### 3.2 Client 状态

```text
begin -> WAIT_SEND -> note_sent -> WAIT_RESULT
                              -> exact Result -> COMPLETE
                              -> Deadline     -> FAILED/TIMEOUT
                              -> cancel       -> CANCELLED
terminal -> copy/view -> retire
```

Deadline 是绝对半开边界：`now >= deadline` 已过期。终态不会被迟到 Result、
重放或维护步骤拉回。结果复制会对声明长度之后的未使用字节做 canonical 清零，
避免栈残留字节成为结果等价性的一部分。

### 3.3 Server retained receipt

首次合法 Request 分配固定 Receipt 并返回 `INVOKE`。完全相同的已完成 Request 返回
`REPLAY_RESULT`；同一交互键但 Request Digest 不同时拒绝，不重复执行。Receipt
在结果提交后才开始保留倒计时；处理中 Receipt 不会被普通超时清理。

## 4. QoS

### 4.1 调度和公平性

调度表为固定 `Q0:Q1:Q2:Q3 = 6:3:2:1`。全局 schedule cursor 和每类 class cursor
都持久在 Owner 中，不从槽 0 重新扫描，因此同类多流在持续热点更新下仍可有限
前进。Q0 项必须携带明确控制授权，不允许普通业务伪装成控制流。

### 4.2 队列所有权

QoS 只保存可发送对象的 typed Handle，不复制 Payload。`pick()` 原子把项目标记为
selected；`note_submitted()` 使其进入 submitted；`release_pick()` 只能释放尚未提交的
选中项。同一项不能并发被两个发送者取走。

### 4.3 Latest、配额和 Deadline

- Latest 只在 Traffic Class、Source Key、Flow Key 和 16 B Latest Key 全部相同时候选替换；
- 已 selected/submitted 的旧项不得被替换；
- per-source、per-flow 和 Latest 总量均是编译期配额；
- 配额满返回 `NO_SPACE`，不驱逐现有项，输出 Handle 保持哨兵不变；
- 维护只回收未 selected、未 submitted 且 Deadline 已到的项。

## 5. Durable Operation

### 5.1 单向状态

```text
PREPARED --durable proof--> PREPARED(stable)
PREPARED --prepare/persist--> EXECUTING(stable)
EXECUTING --executor observed--> external side effect may run
EXECUTING --persist result--> COMMITTED_RESULT
PREPARED  --persist no effect--> ABORTED_NO_EFFECT
EXECUTING after restart/unknown outcome --persist--> IN_DOUBT
terminal + authenticated result ACK + retention + retired floor --> TOMBSTONED
```

`PERSIST_PENDING` 是内部中间态，只接受当前 continuation 对应的精确 Proof。任何终态、
EXECUTING 或 IN_DOUBT 都不会因迟到请求被重置为 PREPARED。

### 5.2 外部副作用边界

执行器只能在 EXECUTING Record 经 Persistence Proof 提交后收到所有权。`mark_executor_observed()`
将“外部系统可能看到副作用”写入易失状态。从耐久 EXECUTING 重启时，实现保守地
当作 executor 已可能观测，禁止再执行或伪造 `ABORTED_NO_EFFECT`，只能对账后
提交 COMMITTED_RESULT，或在无法对账时持久为 IN_DOUBT。

### 5.3 Record 和 Proof

Operation Body 是 `110 + RESULT_BYTES` 的 canonical big-endian 记录，并分别保存：

1. canonical body digest；
2. Foundation 当前 published digest；
3. 待提交 body 的 pending published digest。

这三者不能混用。Proof 必须精确绑定 Persistence Handle、Continuation、Domain、
Domain Generation、Foundation Transaction、Record/Witness Generation、Runtime、双方 Owner、
Schema、Operation Kind、next phase、body bytes 和 published digest。任一不匹配都不推进 Journal。

### 5.4 真实 Foundation 集成

集成测试不伪造内存 Proof，而是使用 Fake Provider 驱动真实 Persistence Foundation：

```text
Service PREPARED requirement
  -> Foundation submit/poll
  -> durable PREPARED body + witness
  -> Service accepts exact proof
  -> prepare EXECUTING
  -> Foundation generation 2 commit
  -> copy durable body
  -> fresh Service owner import/reload
  -> recovered EXECUTING treated as possibly observed
  -> persist IN_DOUBT as generation 3
```

这条路径同时验证了 reload 后的 current published digest 会成为下一次 Foundation
transition 的 exact expected prior digest。

## 6. Operation ID 区间

| Profile | 每次耐久预留的 ID 数 |
| --- | ---: |
| Nano | 16 |
| Lite | 64 |
| Full | 256 |

初始化后 `next=1, reserved_through=0`，不能发号。Owner 首先生成 `OID1` canonical body
与 typed Persistence Requirement，只有 exact Proof 通过后才发布新区间。区间内 ID 严格
递增且不回绕。

重启导入已持久高水位 `H` 后，设置 `next=H+1, reserved_through=H`，不重用旧区间中尚未
发出的尾段。必须先持久下一区间才能继续发号，以一次性跳号换取重启后不 ABA。

集成回归在同一 Foundation 中为 ID 区间配置独立 Durable Domain，已经过：

```text
factory (no interval)
  -> persist interval 1 + witness
  -> issue ID 1
  -> reload high-water
  -> skip all unused IDs in old interval
  -> persist interval 2 + witness
  -> issue first ID in interval 2
```

该证据使用 Fake Provider 驱动真实 Foundation，仍不代表真实 Flash 原子性或物理掉电已验证。

## 7. 固定资源与栈

| Profile | Service Owner | Request | Receipt | QoS | Operation | Result bytes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Nano | 1,648 B | 2 | 2 | 8 | 1 | 32 B |
| Lite | 6,560 B | 8 | 8 | 32 | 4 | 64 B |
| Full | 19,744 B | 16 | 24 | 96 | 8 | 128 B |

`src/service` 中无 `malloc/calloc/realloc/free`。GCC Full Debug `-fstack-usage` 扫描的最大
单函数自动栈为 `752 B`（`ucn_i_service_operation_import`），低于 1,024 B 门限。这只是
Host 编译器门禁，不代表 ESP32/FreeRTOS
完整调用链栈水位。

## 8. 分项自审与整改

| 自审项 | 发现 | 处理 |
| --- | --- | --- |
| Digest 语义 | canonical body digest 与 Foundation published digest 被当成同一值会让下一次 transition 错绑。 | 分离 canonical/current published/pending published 三类摘要，Proof 后只推进 current published。 |
| Reload 连续性 | 只导入 Body 无法证明下一次 expected prior digest。 | import 必须同时接受 Foundation 验证的 loaded published digest。 |
| 维护饥饿 | 每次从槽 0 扫描，小 budget 下高位 Request/Receipt/QoS 可能永久不被检查。 | 统一线性 maintain cursor 跨三类表持久轮转，每次检查一个物理槽才消耗预算。 |
| Generation 宽度 | Service durability 的 Domain Generation 初始与 Foundation 定义不一致，MSVC 报窄化。 | 统一为 Foundation 的 16-bit 合同，在 `/W4 /WX` 下重跑。 |
| Operation ID 掉电 ABA | 重启后从“已发出最后 ID”继续可重用旧预留但未发出的 ID。 | Record 持久区间高水位；reload 跳过整个旧区间，要求新持久区间。 |
| Latest/Deadline | 只测成功替换不能区分 selected 项被静默替换或回收。 | 新增 selected 保护、到期维护和解除选中后回收的对抗回归。 |
| 配额零写 | 只测队列满不足以证明 per-source/per-flow 配额失败不写输出。 | 补充两类配额极限、额外项、Handle 哨兵和已有项不变回归。 |

## 9. 全体交叉自审

第一轮从 `Owner -> 入参 -> 状态 -> Requirement -> Proof -> 终态 -> 退休` 正向回读，
检查每个输入是否在首次写状态前被验证。第二轮从“掉电、旧 Proof、错 Handle、超时、
表满、重放、并发选中、ID 耗尽”反向追回 Owner 和持久化根。

本轮未发现尚未处理的 Host 私有模型软件 P0/P1。这不是独立外审结论，也不对
未接线的生产组件、实机或 Flash 原子性做出承诺。

## 10. 验证矩阵

| 门禁 | 结果 |
| --- | --- |
| Windows GCC Full Debug | 77/77 PASS |
| Windows GCC Lite Debug | 77/77 PASS |
| Windows GCC Nano Debug | 77/77 PASS |
| MSVC 19.29 Full Release `/W4 /WX` | Service 定向 5/5 PASS |
| WSL GCC ASan/UBSan Nano | Service 定向 6/6 PASS |
| WSL GCC `-fanalyzer -Werror` Nano | Service 定向 6/6 PASS |
| WSL Clang Release Nano | Service 定向 6/6 PASS |
| Boundary / archive / stack | PASS；私有符号不进入公共 archive |
| 普通 pthread 并发 | 双 Owner 固定轮次压力 PASS |
| WSL GCC TSan | 运行时 `unexpected memory mapping`，不计为 PASS |

## 11. 开放项

1. 公共用户 API 与 Runtime 尚未消费私有 Service Owner；
2. Transport sendable、Security facts、Adapter completion 与 QoS queue 尚未组成生产链；
3. 真实业务执行器的可查询对账或原子提交能力未接入；
4. Operation ID 区间已有独立 Foundation Domain 集成，但仍需真实 Flash 和物理掉电测试；
5. C↔Rust Service/Operation/Wire 互操作尚未建立；
6. MCU 实际 RAM/栈/CPU/功耗、物理 Bearer 丢包与长稳尚未验证；
7. 当前 TSan 运行时在 WSL 不可用，必须在可运行环境中重补。

上述开放项使 `AUDIT HOLD` 保持有效。下一阶段可以内部进入 IMPL-08 的 Realtime、
Group、Cluster 三个互不依赖的可选分支，但不得继承 IMPL-07 的自审结论或宣称外审 GO。
