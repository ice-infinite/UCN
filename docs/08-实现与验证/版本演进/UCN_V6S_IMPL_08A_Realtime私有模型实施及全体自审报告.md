# UCN V6S IMPL-08A Realtime 私有模型实施及全体自审报告

> 日期：2026-09-24  
> 分支：`v6-simplified-rust`  
> 结论：`OFFLINE PRIVATE MODEL IMPLEMENTED / SELF-REVIEW PASS / AUDIT HOLD`

## 1. 本阶段解决什么问题

普通 UCN 数据不应为“不需要实时”的业务永久承担时间字段。Realtime 因此是独立可选
模块：Endpoint 选择 NONE 时，基础 Frame、队列和转发路径都不增加时间字节；只有明确选择
LOCAL、SYNCED 或 DEADLINE 的消息才携带 16 B Envelope。

本阶段建立可执行的私有 C 模型，用来验证以下核心不变量：

1. Network Time Domain Generation 必须持久化后才能发布；
2. 同一 Generation 发布过的 Domain Time 不得倒退；
3. T1～T4 的物理事件由各节点本地拥有，Wire 只关联认证事务键；
4. uncertainty 必须保守、完整、可审计，未知不得伪装成零；
5. Deadline 在接收和执行前都可用同一门禁重新计算；
6. Realtime 与 Group、Cluster 相互独立，Feature OFF 必须零详细状态和符号。

这不是公共产品接线。内部头 `src/internal/ucn_realtime.h` 不安装，详细 target 使用
`EXCLUDE_FROM_ALL`，仅测试和后续 Coordinator 适配器可消费。

## 2. 模块边界

```text
Authenticated immutable facts
        │
        ▼
Coordinator（后续生产接线）
        │ typed requirement/event/view
        ▼
Realtime Owner ──► 16 B Envelope Codec
      │
      ├──► Domain FSM / Clock View
      ├──► Member Sync Pending / Release Obligation
      └──► Persistence Requirement ──Coordinator──► Foundation
```

`ucn_v6s_realtime` 只链接 `ucn_common`。真实 Persistence 集成测试在测试可执行文件层同时
链接 Realtime 与 Foundation；Realtime 源码本身不 include、不调用 Persistence Owner。
它也不持有 Route、Security、Adapter、Group 或 Cluster Owner 指针，只消费 Coordinator
未来传入的冻结 Path、认证标志、ACL 标志和 Durable Proof。

## 3. IMPL-08A-00：Owner、容量与 Feature 边界

Profile 容量均为编译期常量：

| Profile | Domain | Endpoint Policy | Sync Pending | Release Obligation | Owner bytes |
| --- | ---: | ---: | ---: | ---: | ---: |
| Nano | 1 | 4 | 1 | 2 | 752 |
| Lite | 2 | 12 | 4 | 8 | 1896 |
| Full | 4 | 32 | 8 | 16 | 3992 |

Owner 由调用方提供全零内存与 caller-owned lock。初始化拒绝对象、配置或锁上下文别名；
销毁先复制锁操作，再在锁内清零 Owner，最后用已复制的锁退出，避免对清零字段解引用。

`UCN_FEATURE_REALTIME=OFF` 时不创建 `ucn_v6s_realtime` 详细 archive、详细单元测试或
Persistence 集成测试；已有 scaffold 仍只是统一架构占位，不含 Realtime 状态机。

分项自审：通过。未发现动态分配、全局可写 Owner 或跨模块直连。

## 4. IMPL-08A-01：Envelope 与 uncertainty class

Envelope 固定为 16 B、big-endian：

```text
Byte 0      version + mode
Byte 1      uncertainty class + capture/domain/holdover flags
Byte 2..3   clock_domain_id
Byte 4..7   domain_generation
Byte 8..15  capture_time_us
```

LOCAL 要求 Domain ID/Generation/valid/holdover 为零，uncertainty 为 UNKNOWN；SYNCED 和
DEADLINE 要求非零 Domain、合法非回绕 Generation、有效 Domain Time 和 known class。
class 使用向上取整的 `ceil(log2(upper_bound_us))`，解码得到不小于真实值的上界。

Codec 先完整验证并写局部 staging，成功后才复制输出；typed/raw 完整或部分重叠均拒绝。
Golden、坏版本、坏保留位、错误模式组合、精确长度和失败输出哨兵均已覆盖。

分项自审：通过。

## 5. IMPL-08A-02：持久 Time Domain

Domain Record 固定 128 B，绑定 Domain ID、Generation、同步/HOLDOVER 参数、完整六分量
uncertainty、冻结 Path、Session/Capability/Link Generation 与 Capability Digest。

发布顺序为：

```text
prepare candidate
  → export immutable Requirement
  → Coordinator 提交 Persistence Foundation
  → inactive slot / readback / Marker / Witness
  → reload/proof
  → exact accept_proof
  → publish UNSYNCED Domain Generation
```

Proof 精确核对 Persistence Handle/Owner、Runtime/Caller Owner、Domain、Foundation Tx、
期望 Record Generation 的 checked-next、Witness、Schema、Operation Kind、Body Digest 和
绝对 Deadline。`now == deadline` 固定拒绝。错误 Proof 不清除 pending，正确 Proof 仍可在
截止前重试；未持久候选不进入可用 Domain。

真实 Foundation 集成测试没有伪造“写入成功”：它执行 Marker、Witness、proof 获取和退休，
再由 Realtime 接受 proof。

分项自审：通过。修复了 Persistence Owner 未精确核对、迟到 Proof 缺少对抗回归以及
Domain/Persistence Generation 宽度混用。

## 6. IMPL-08A-03：Domain FSM 与单调时间

状态为：

```text
GENERATION_LOADING / GENERATION_PENDING
                 ↓ durable proof
              UNSYNCED
                 ↓ valid samples
              ACQUIRING ── consecutive threshold ──► LOCKED
                    ▲                                  │
                    └──────── fresh sample ────────────┤
                                                       ▼ timeout
                                                    HOLDOVER
                                                       ▼ max age
                                                    UNSYNCED
```

固定五样本中位窗口抑制单点异常，`lock_sample_count` 控制连续合格门槛。进入 UNSYNCED 时
清除样本、游标、offset 和 uncertainty，但保留同 Generation 的最后发布时间高水位。
新候选若会使时间低于已发布高水位，`ingest` 当场进入 FAULT，不先暴露瞬时 LOCKED。

HOLDOVER uncertainty 按 oscillator ppb 与 elapsed time 向上取整增长，所有乘加先做溢出
检查。仅成功返回 Clock View 时推进发布高水位。

分项自审：通过。

## 7. IMPL-08A-04：四时间戳 Member 事务与事件释放

当前实现的是 Member 侧语义闭环：认证且冻结的 Path 上收到 SYNC 时，T2 RX event 与
`{domain_generation, sync_sequence, path}` 一起进入 pending；提交 DELAY_REQ 后绑定本地 T3
TX event；收到含 T1/T4 的响应后计算候选 offset 和完整 uncertainty。

响应还必须在 Member 本地绝对 Deadline 前到达。响应参数中的 Master T4 不能替代这个本地
新鲜度检查。较高 sequence 替换旧 pending、事务到期或成功完成时，Owner 都把本节点持有的
T2/T3 转成 release obligation。上层按 `peek → Driver retire → ack` 处理；退休失败不 ack，
下一轮仍得到同一 obligation。

Event key 含 Link ID、Link Generation、方向和 Token，只在本节点比较。release slot 使用独立
非回绕 generation 防止迟到 ACK 清除新 obligation。

分项自审：通过。生产 Master 侧 T1/T4 reservation 与四类控制 Wire 仍是开放项。

## 8. IMPL-08A-05：Policy、Envelope 和 Deadline

策略模式：

| 模式 | 行为 |
| --- | --- |
| NONE | 不产生 Envelope，普通路径零额外字节 |
| LOCAL_STAMP | 只表达发送端本地时间，不建立跨节点时间 |
| SYNCED_STAMP | 携带持久 Domain Generation 与保守 uncertainty |
| DEADLINE | 在 SYNCED 基础上执行半开年龄门禁 |

REQUIRED 获取不到有效 Clock View 时失败；PREFERRED 可显式回退 LOCAL。任何收到的实时元数据
都必须已经通过 E2E 认证和 Source ACL；REQUIRED 固定拒绝远端 HOLDOVER。接收端计算：

```text
U = checked_add(sender_uncertainty_upper, receiver_uncertainty)
age_upper = checked_add(max(0, domain_now - capture_time), U)
accept iff U <= max_uncertainty && age_upper < max_age
```

若 capture time 位于未来，仅当偏移不超过组合 uncertainty 才可接受。相同 `admit` 门可以在
入队前和业务执行前以新的 `local_now_us` 再调用，从而实现双门禁而不保留不可信缓存结论。

分项自审：通过。补审中关闭了 REQUIRED LOCAL_STAMP 绕过 E2E+ACL 的路径。

## 9. IMPL-08A-06：资源与工具链

验证结果：

| 环境 | 结果 |
| --- | --- |
| Windows GCC Full 全量 | 85/85 |
| Windows GCC Feature-OFF 全量 | 69/69 |
| Windows GCC Full/Lite/Nano Realtime 定向 | 各 6/6 |
| MSVC 19.29 Release | 6/6 |
| WSL GCC ASan/UBSan | 7/7 |
| WSL GCC `-fanalyzer -Werror` | 7/7 |
| WSL Clang 18 Release | 7/7 |
| WSL GCC TSan non-PIE 双线程 | 1/1 |

原始 PIE TSan 以及从 DrvFS 直接启动的 non-PIE 产物在当前 WSL 报告
`unexpected memory mapping`，未进入测试逻辑；把同一 non-PIE instrumented 产物复制到
WSL ext4 `/tmp` 后通过 1/1。该处理只绕开 WSL/DrvFS 地址布局限制，不替代目标 SMP/ISR 验证。

GCC `.su/.ci` 强制门禁结果：73 个 Realtime 函数，单函数最大 320 B；最长静态可见同步
调用链 672 B。Nano 上限为 768 B，仍有 96 B 余量。Provider/Driver 内部栈不在该数字内，
产品任务栈必须在目标 RTOS 重新测水位。

## 10. 两轮全体自审

### 10.1 合同到实现

按“可选元数据→持久代际→同步事务→样本→Clock View→Policy/Deadline→释放”的顺序回读，
确认普通 Frame 零开销、Generation persist-before-publish、同代际单调、完整 uncertainty、
半开 Deadline 和跨 Owner typed facts 均有实现与测试。

### 10.2 故障到副作用

反向检查坏 Codec、错 Proof、迟到 Proof、错 Persistence Owner、重放 Sequence、Path 漂移、
迟到响应、release ACK 冲突、未知 uncertainty、未来时间、ACL 拒绝、HOLDOVER 和时间倒退。
所有拒绝路径都在对应发布点前停止；需要重试的 Proof、Sync pending 与 release obligation
不会因一次错误输入丢失。

本轮自审发现并关闭的主要问题：

1. Owner destroy 清零后再从 Owner 取锁；
2. 32-bit Time Domain Generation 被误当作 16-bit Persistence Handle Generation；
3. Proof 未绑定 Persistence Owner、迟到 Proof 缺少正式回归；
4. 四时间戳 uncertainty 未把基础六分量与全部 event bound 保守求和；
5. UNSYNCED 错误清除同代际时间高水位；
6. release generation 到顶后可能复用；
7. Sync response 没有使用 Member 本地接收时刻执行 Deadline；
8. LOCAL_STAMP 接收可绕过 E2E+ACL；
9. Feature OFF 仍会创建详细 target；
10. Realtime 只有 `.su`，没有独立强制栈/调用链门禁。

整改后未发现仍开放的已知 Host 私有模型 P0/P1。

## 11. 明确保留的边界

以下事项没有被本报告宣称完成：

- Master 侧 T1/T4 reservation、SYNC/DELAY_REQ/DELAY_RESP 生产 Wire 与完整 Owner；
- 无可信 path asymmetry 时的显式诊断结果 ABI；
- Coordinator 到 Security/Route/Persistence/Adapter 的生产接线；
- 硬件 RX/TX timestamp、ISR/任务通知、timer resolution 和实际误差标定；
- 真实 Flash/Witness/断电撕裂；
- ESP32-S3 的栈水位、性能、功耗、长稳和多 Bearer；
- 公共用户 API、C/Rust 互操作和发布兼容承诺。

因此本阶段保持 `AUDIT HOLD`，但可以按既定内部顺序继续 IMPL-08B Group。任何生产接线或
硬件结论必须单独实施、自审和外审。
