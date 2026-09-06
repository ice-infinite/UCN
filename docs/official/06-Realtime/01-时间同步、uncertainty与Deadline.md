# 时间同步、uncertainty 与 Deadline

## 1. 可选而非全局负担

Realtime 由 Feature Manifest 控制。普通消息不带 Envelope，也不承担同步开销；只有 Endpoint
策略要求 SYNCED_STAMP 或 DEADLINE 时，源端才生成实时元数据。Cluster 和普通 Route 不依赖
Realtime。

## 2. 时间域

Time Domain 从 UNSYNCED 经 ACQUIRING 到 LOCKED；源失效进入 HOLDOVER，超过上限回到
UNSYNCED 或 FAULT。有效样本必须来自认证固定定向 Path，并具备可信 asymmetry 上界。普通
动态 Route 或未知 asymmetry 只能产生诊断，不进入滤波窗口、不推动 LOCKED，也不产生有效
Envelope。

滤波历史与同 generation 单调输出高水位分开保存。失锁时清除 acquisition/filter，但不能
清除已发布时间高水位；重新锁定若会倒退，`ingest_sample()` 在暴露 LOCKED 前直接 Fault。

时间采样不是调用方可独立填写的旁路参数。当前交换使用三个认证控制消息和四个物理事件：

```text
Master  TIME_SYNC{domain,generation,sequence}   -- T1 TX -->
Member                                          <-- T2 RX
Member  TIME_DELAY_REQUEST{same key}            -- T3 TX -->
Master                                          <-- T4 RX
Master  TIME_DELAY_RESPONSE{same key,T1,T4及上界}        --> Member
```

`TIME_SYNC` 的 canonical announce 固定为 12 B；`TIME_DELAY_RESPONSE` 的 canonical response
固定为 40 B，只携带 Master 所有的 T1/T4。Member 的 T2/T3 永不上 Wire，必须由标准 Runtime
从当前 Adapter RX 原子项和实际 TX completion 绑定。Domain 只接受已完成 Security Open 的
`TIME_DELAY_RESPONSE`，并精确绑定 Opcode、Master Principal、Binding、Session、Route、
固定 Path、Domain Generation 和 Sequence；任何错绑都在修改滤波器前拒绝。正向
`Master→Member` 与反向 `Member→Master` 是两个独立的定向 Path Identity，可以使用不同
Path ID/Generation。Runtime 分别冻结两者：`TIME_SYNC` 与 `TIME_DELAY_RESPONSE` 必须命中
同一正向 Path，`TIME_DELAY_REQUEST` 必须命中当前反向 Path；任一方向的证明不能替代另一方向。

这一设计不接受远端直接声明 `offset_us` 或 `local_sample_us`。Member 根据四个事件本地计算：

```text
offset = ((T1 - T2) + (T4 - T3)) / 2
```

所有差值、求和与除法舍入都执行 checked arithmetic。缺失硬件时间戳、事件键重复、Link
Generation 错绑、反向 Path 失效、响应超时或任一 uncertainty 上界未知，均失败关闭。

## 3. uncertainty

发送端上界 S 保守聚合采样捕获、时间戳、滤波残差、计时器分辨率、链路打点、振荡器和舍入
等分量；任何 required 分量未知或算术溢出都失败关闭。Envelope 编码 S 的向上量化 class。
接收端再与本地 uncertainty checked-add 得到组合上界 U，并验证 `U <= max_uncertainty_us`。

## 4. Deadline 双门禁

接收入队前先检查 future skew、generation、S/U 和 `age_upper < effective_max_age`；业务执行前
用新时刻再次计算，避免消息在队列中等待后过期。Timed Command 的 `capture_time_us` 是唯一
年龄基准，Guard 绑定 Principal/Operation/generation 而不引入第二个 uptime 时钟域。

半开区间意味着恰好到 deadline 已过期。远未来 capture time 不能通过 `max(0, now-capture)`
伪装成年龄很小，必须单独按 uncertainty/future-skew 公式拒绝。

## 5. 时间戳事件与标准 Runtime

T1/T4 属于 Master，T2/T3 属于 Member。四个本地 event key 不上 Wire；双方通过认证的
`{message role,sync_seq,session_generation,path_generation}` 事务键关联。RX 必须原子入队
`{完整帧,event key,timestamp}`，不能靠两个 Ring 的顺序猜测配对。

节点只能取消自己持有的 key。超时、替换、切路或 reopen 采用 `peek → driver retire → ack/pop`
释放 obligation；Driver 失败时 key 留队等待重试。远端旧事件由 Session/Path Generation、
Abort 或绝对超时收敛。

`ucn_v6_runtime_owner_t` 是生产接入的标准编排层。Driver 只向 Adapter 发布 RX/TX completion；
Runtime 固定 Owner 阶段、退休顺序、依赖失效扇出和全部 T1～T4 绑定。Master 通过
`ucn_v6_runtime_time_start_sync()` 让 Runtime 构造、保护并排队精确 `TIME_SYNC`；Member 在
当前 RX callback 中用 `ucn_v6_runtime_time_observe_sync()` 获得 opaque handle，再由
`ucn_v6_runtime_time_send_delay_request()` 构造并排队精确请求；Master 在当前请求 RX 上调用
`ucn_v6_runtime_time_respond_delay_request()`；Member 只可用当前响应 RX 调用
`ucn_v6_runtime_time_complete()`。应用不能直接提交三元组或任意 TX event key 冒充事务。

产品回调只负责把已经过 Security Open 的控制/业务消息交回上述 Runtime API，以及接收已物理
退休的 Buffer token，不能重排协议阶段。Link reopen 必须调用
`ucn_v6_runtime_reopen_link()`，由它统一完成 Adapter quiesce、旧代际 token 回收和 Link
invalidation 扇出；任何 RX、TX completion 或应用回调动态范围内的 reopen 都失败关闭。

## 6. HOLDOVER

REQUIRED 固定拒绝远端 `SOURCE_HOLDOVER`，因为 Envelope 不携带可独立验证的远端 holdover
age。PREFERRED 只有产品显式信任源端自检、E2E 与 ACL 全部成立时可作受限诊断/使用。
