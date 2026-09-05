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

时间采样不是调用方可独立填写的旁路参数。`TIME_FOLLOW_UP` 使用固定 48 B canonical Payload，
Domain 只从已经完成 Security Open 的 Frame Payload 解码，并精确绑定 Opcode、Master
Principal、Binding、Session、Route 和固定 Path；任何错绑在修改滤波器前拒绝。

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

## 5. 时间戳事件

T1/T4 属于 Master，T2/T3 属于 Member。四个本地 event key 不上 Wire；双方通过认证的
`{message role,sync_seq,session_generation,path_generation}` 事务键关联。RX 必须原子入队
`{完整帧,event key,timestamp}`，不能靠两个 Ring 的顺序猜测配对。

节点只能取消自己持有的 key。超时、替换、切路或 reopen 采用 `peek → driver retire → ack/pop`
释放 obligation；Driver 失败时 key 留队等待重试。远端旧事件由 Session/Path Generation、
Abort 或绝对超时收敛。

## 6. HOLDOVER

REQUIRED 固定拒绝远端 `SOURCE_HOLDOVER`，因为 Envelope 不携带可独立验证的远端 holdover
age。PREFERRED 只有产品显式信任源端自检、E2E 与 ACL 全部成立时可作受限诊断/使用。
