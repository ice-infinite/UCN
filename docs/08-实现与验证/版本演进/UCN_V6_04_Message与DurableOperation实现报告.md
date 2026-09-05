# UCN V6-04 Message 与 Durable Operation 实现报告

> 状态：隔离实现与分项自审完成；最终统一外审延期。
> 生产状态：default-OFF，不接入 v5 Node、Service 或 Endpoint 分派。

## 1. 本阶段解决的问题

旧实现容易把优先级、交付可靠性和请求/应答混成同一个枚举。v6 将它们拆成三条正交轴：

- `traffic_class` 只决定 Q0～Q3 的调度资源；
- `delivery_guarantee` 只表达 Best-effort、Latest 或 Reliable；
- `interaction_role` 只表达 One-way、Request、Result 或 Error。

因此 Reliable Request 不必伪装成某个优先级，Latest Result 也不必改变业务角色。16-bit
Endpoint、最大 Payload/Result 和允许组合由不可变 Endpoint Contract 明确限制。负枚举、
保留 Endpoint、超长载荷和合同外组合在占用队列或 Journal 之前拒绝。

## 2. Operation ID 的持久区间预留

Request、Result 和 Error 使用同一个非零 64-bit Operation ID。发送方分配器不逐个写 Flash，
而是通过独立、不可随 Journal Snapshot 一起回滚的 Witness 先持久化预留一段 ID，再从该段内发号：

```text
load durable witness high-water N
  -> reserve witness high-water N+B
  -> reload and verify exact witness
  -> publish N+1 ... N+B
```

掉电后未用完的号码会跳过，不会回收复用。持久化或回读不一致会永久 Fence 当前分配器；
达到 `0xFFFFFFFFFFFFFFFE` 后返回 EXHAUSTED，不允许回绕到 1。所有 Provider 回调共享 V6-02
的 caller-owned callback gate，回调中的递归分配失败关闭。Provider 一旦进入 commissioned
状态，后续不得把 Witness 报为 `NOT_FOUND`；否则等价于 anti-rollback 证据丢失，初始化失败关闭。

## 3. Durable Operation Journal

接收方使用 8 个固定 Operation 槽和 8 个固定 Principal retired-high-water 槽；无堆内存、
无自动驱逐。Operation Key 为：

```text
{initiator_device_principal, operation_id}
```

每个槽固定保存 Endpoint、执行合同、32 B Request Digest、Phase、结果码、最多 64 B Result
以及 32 B Terminal Digest。状态只能单向推进：

```text
PREPARED -> EXECUTING -> COMMITTED_RESULT -> TOMBSTONED -> RECLAIMED
                       \
                        reboot -> IN_DOUBT

PREPARED -> TOMBSTONED                 // 执行前 Abort
IN_DOUBT -> COMMITTED_RESULT/TOMBSTONED // 认证对账
```

`EXECUTING` 必须先持久化并回读一致，调用方才可把工作交给外部执行器。启动时若加载到
`EXECUTING`，初始化会先把所有此类槽原子迁移并持久化为 `IN_DOUBT`，完成前不接收新工作。
`IN_DOUBT` 绝不自动重试；只有认证对账才能关闭。相同 Key/相同 Request Digest 返回精确的
已有状态，相同 Key/不同 Digest 返回 REPLAY，不能再次执行。

## 4. Result 保留与 GC

COMMITTED_RESULT 保存可逐字节重放的结果。只有认证 Result ACK 已收到且最小保留期已过，
才允许抹去结果正文并写入 TOMBSTONED；Terminal Digest、结果码、Operation Key 和 Request
Digest 仍保留。

槽复用还要求：

1. 发起方提供的持久化 Operation high-water 已严格越过该 ID；
2. 最大网络重放寿命已过；
3. 本地 Principal retired-high-water 与槽清除处于同一持久快照。

后续旧请求即使槽已复用，也会被 retired-high-water 拒绝。高水位表满时 Tombstone 不会被
清除，返回 NO_SPACE。PREPARED、EXECUTING、IN_DOUBT 均不存在时间驱逐路径。

## 5. Provider 原子边界

每次 Journal 变更都复制当前 committed snapshot，完成全部合法性检查后才进入 Provider。
`record_generation` 不依赖 Snapshot 自证，而由独立 Witness 约束，提交顺序固定为：

```text
validate candidate generation against committed witness
    -> reserve pending witness generation
    -> reload and verify exact pending witness
    -> submit full journal snapshot
    -> reload and semantic-exact compare snapshot
    -> finalize witness committed generation
    -> reload and verify exact committed witness
    -> publish committed RAM view
```

启动时若 Witness 显示 pending 而 Snapshot 仍是旧代际，则单调取消该 pending；Snapshot 已是
新代际则完成 Witness finalize。Witness 已 committed 到新代际而 Snapshot 缺失或更旧时禁止静默
回退。submit、reserve、reload、全零/坏 Schema、非法 Witness 转换、字段冲突或不一致回读都不会
发布 candidate；由于底层写入结果可能不确定，Owner 同时进入 faulted，防止继续承诺。Provider
回调前先建立共享重入门，回调中的 Journal/Allocator 控制调用返回 STATE，不能产生第二次写入。

## 6. 分项自审与定向反例

本阶段自审额外关闭了以下实现风险：

1. 统一暴露 V6-02 callback gate 的加锁 enter/leave API，避免后续模块各自复制静态全局门；
2. 负 Traffic Class 使用无符号范围检查，覆盖 `-1/-255/-256`，消除编译器别名；
3. `commit_result()` 在任何 `memcmp` 前校验 Result 指针、长度和 Digest，零长度不解引用空指针；
4. Snapshot 增加 live slot 与 retired-high-water 的交叉验证，禁止回收后旧槽复活；
5. Provider reentrancy、固定表满、同 Key 异 Digest、重复 Mark Executing、ACK/Retention 未满足、
   重启 `EXECUTING -> IN_DOUBT`、未认证对账、全零 Record 与 submit failure 均有确定性回归；
6. 所有失败路径检查 admission/output 或 committed snapshot 不写回；持久化不确定错误只增加
   fault fence，不伪造成功。
7. 后续全局自审为 Journal 增加独立 anti-rollback Witness，并以合法旧 Snapshot 回放、Witness
   先写/Journal 后写两个掉电窗口、committed Witness 配旧 Snapshot 等反例锁定恢复规则；
8. MSVC Release 严格告警暴露的临时 Witness/Snapshot 未初始化风险已统一改为显式零初始化；
   内部预检也显式接收 `previous` 与 `candidate`，避免把已修改候选误当上一持久状态。

## 7. 尚未声称完成的范围

- V6-05 冻结 opaque storage 与 Layout/Manifest；Record 字节布局、双槽 Flash 和真实恢复
  仍由 V6-07/V6-13 完成；
- V6-07 才提供真实 32 B 密码 Digest、E2E Principal、ACL、认证 ACK 与 Replay 证明；
- 本阶段不执行外部业务副作用，也不声称任意执行器具有 exactly-once 能力；
- 本阶段未接生产 RX/TX，不代表真实 Flash、掉电、磨损或 MCU 资源已经验证。

## 8. 验证结果

| 门禁 | 结果 |
| --- | --- |
| Windows GCC Full Debug 全量 | 61/61 |
| Windows GCC V6-02/03/04 定向 | 3/3 |
| MSVC 19.29 Release `/W4 /WX` | 1/1 |
| WSL ASan/UBSan | 1/1 |
| WSL `-fanalyzer -Werror` | 1/1 |
| default-OFF 生产隔离 | v5 archive 不链接 `ucn_v6_*` |

以上均为 Host 软件证据。真正的双槽 Flash、写撕裂、复位瞬间、磨损预算和外部执行器对账
必须在 V6-05/V6-07/V6-13/V6-14 分层补证。
