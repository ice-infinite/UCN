# UCN V6-02 身份、Bootstrap 与地址代际模型实现报告

> 状态：软件实现与分项自审完成；最终统一外审延期。  
> 范围：隔离的 v6 Identity/Bootstrap 模型，不接入 v5 生产收发，不代替 V6-07 的密码实现与唯一 JOIN FSM。  
> 分支：`v6-development`。

## 1. 本小节解决的问题

V6-02 先建立“谁拥有地址、哪个代际仍然有效、未绑定设备如何安全开始通信”的基础语义。
它不复用 v5 的裸地址身份，也不允许未认证节点通过普通 HELLO 写入 Neighbor、Route、ACL
或 Cluster 状态。实现把以下对象明确分离：

1. 128-bit Device Principal；
2. 32-bit Realm；
3. 可复用 Node Address；
4. 同一 `{realm,address}` 内不可回退的 Binding Generation；
5. Address Authority Principal/Generation/Fence/Lease；
6. 一跳、不可转发的未绑定 JOIN Bootstrap；
7. 已绑定设备在新 Link 上建立 Peer Session 的 REAUTH；
8. 动态 Group ID 的单调分配高水位。

这些模型位于 `include/ucn/v6` 与 `src/v6/identity`。只有显式打开
`UCN_BUILD_V6_EXPERIMENTAL=ON` 才构建 `ucn_v6_identity`；默认 `ucn_core` 不链接它。

## 2. Identity 与 Binding

`ucn_v6_principal_t` 固定为 16 B。全零和全 `0xFF` 都是无效 Principal，避免把擦除态或
零初始化误当成真实身份。日常路由身份使用：

```text
BindingKey = { realm_id, node_address, binding_generation }
SessionKey = { BindingKey, DevicePrincipal, session_generation }
```

地址 0 和 `UINT32_MAX` 保留。所有 32-bit 所有权代际使用
`ucn_v6_serial_checked_next()`；达到 `0xFFFFFFFE` 后返回 `UCN_V6_ERR_EXHAUSTED`，禁止回绕。

Authority 的 Binding 表固定 16 槽。相同地址的活动 Binding 不可被覆盖；退休时先提交完整
retired slot，成功后才修改 RAM。再次分配同一地址时只能从该地址的持久化高水位精确加一。
表满、租约过期、无 durable quorum/Fence 或 Provider 失败时均不发布 Certificate。

## 3. Realm Address Authority

`ucn_v6_authority_epoch_t` 绑定 Authority Principal、非零 Generation、128-bit Fence、Lease
Sequence、最大 Lease Duration、allocation high-water digest 以及 durable quorum 证明。
首次安装只能使用 Generation 1；后续必须 checked-next、Fence 必须改变且 Lease Sequence
必须前进。精确重放只允许完整 Epoch 与本地 Deadline 一致。

所有外部持久化调用都经调用方持有的 `ucn_v6_callback_gate_t`。Gate 的 lock/unlock 必须覆盖
同一执行域内所有 Identity Authority；回调发生时先原子置位 active，再释放物理锁进入
Provider。Provider 递归初始化或另一个 Authority 同时进入时会在任何对象清零或写入前返回
`UCN_V6_ERR_STATE`。这既避免单对象重入，也避免使用未同步静态全局变量造成 SMP 数据竞争。

本小节只实现 persist-before-publish 的运行模型；从 Flash 加载、双槽 Record、Manifest/Layout
Hash 和掉电恢复属于 V6-05/V6-07，不能由当前 Host fake Provider 证明。

## 4. 保守租约 Deadline

验证端不把 Authority 的 uptime 当作共同时间。它在发送新鲜度 Challenge 前锁存本地时刻，
然后对认证的 `max_remaining_lease_us` 执行：

```text
clock_margin = ceil(remaining * max_slow_ppm / 1_000_000)
read_margin = 2 * local_timer_read_uncertainty_us
quantization_margin = local_timer_resolution_us + read_margin
safe = remaining - clock_margin - quantization_margin
effective = min(safe, local_policy_max_lease_us)
deadline = challenge_started_local_us + effective
live := now_us < deadline
```

乘法、加法、减法、零值、Unknown read uncertainty 和 Deadline 溢出全部失败关闭。10 ms 分辨率、
15 ms 剩余租期、零慢钟/读数误差时，只得到 5 ms 本地安全窗口；`now==deadline` 已过期。

## 5. Bootstrap 与 REAUTH

两条流程拥有独立状态数组，但共享同一认证前资源预算：

- 全局 pending 默认最多 8；
- 同一 ingress Link Generation 默认最多 2；
- 同一 `{link,local discriminator,identity digest}` 只能有 1 条；
- 每 Link Token Bucket 默认 burst 4、每秒补 2；
- pending 使用绝对 3 s 半开 Deadline；
- 只有显式 `ucn_v6_bootstrap_expire()` 能清理到期槽，恶意输入不能 lazy-expire 合法事务。

未通过 Cookie 时不分配 pending。响应字节数不得大于请求，防止认证前放大。通过 Cookie 后，
key 必须绑定 Link Generation、local peer discriminator、Identity Digest 和 64-bit txid；完整
transcript 再绑定双方 Principal、双 nonce、Realm/Address/Binding、Lease、Suite/Key Context
及前序消息摘要。

JOIN 的合法推进顺序为：

```text
COOKIE_VERIFIED
  -> AUTHORITY_VERIFIED
  -> DEVICE_VERIFIED
  -> ADDRESS_OFFERED
  -> DEVICE_COMMITTED
  -> FINAL_DURABLE
```

REAUTH 只能针对已存在且与 transcript 精确匹配的 Binding，并省略地址变更阶段。它不能改变
Realm、Address 或 Binding Generation。两条流程不能互相借用 pending 配额、身份或证明；
精确重复不会刷新 Deadline，冲突重放返回 `UCN_V6_ERR_REPLAY`。

## 6. 动态 Group ID

动态 Group 使用固定 8 个 active slot 与单个 32-bit 高水位。创建只能取 high-water 的
checked-next，并在发布 ID 前持久化新高水位。退休只释放 active slot，不降低高水位；下次
创建获得更大的 ID，不搜索历史空洞。静态 Group 和 Group Key 固定槽将在 V6-05/V6-07
实现，这里没有用摘要冒充历史成员查询。

## 7. 自审发现与整改

本小节实现后进行了两轮代码自审，并在提交前关闭以下内部发现：

1. Token Bucket 长时间未访问时，`elapsed_seconds * rate` 可能溢出。现改为先计算填满所需
   秒数，达到阈值直接饱和，不执行大乘法。
2. JOIN/REAUTH 分表最初分别计算容量，可能把“全局 8”扩大为 16。现统一跨两表统计全局、
   per-Link 与 per-peer 配额。
3. Provider 回调最初只有对象内 `io_active`，递归 `init()` 可先清零该位。现使用对象外、
   调用方持有且受公共锁保护的 callback gate；递归初始化在写入前拒绝。
4. 终态 pending 的测试最初忽略了另一条已完成 JOIN 也会在同一 timer tick 到期。测试改为
   分别锁定并验证两条事务的半开 Deadline，不再用含糊的总计数掩盖生命周期。

## 8. 验证证据

| 门禁 | 结果 |
| --- | --- |
| Windows GCC Full Debug，全量 CTest | 59/59 |
| Windows GCC v6 Identity/Bootstrap 定向 | 1/1 |
| MSVC 19.29 `/W4 /WX` 定向 | 1/1 |
| WSL GCC ASan/UBSan 定向 | 1/1 |
| WSL GCC `-fanalyzer -Wall -Wextra -Wpedantic -Werror` 定向 | 1/1 |
| 默认 `UCN_BUILD_V6_EXPERIMENTAL=OFF` Release `ucn_core` | 构建通过，`ucn_v6_*` 导出符号 0 |

测试覆盖 Principal 擦除值、serial exhaustion、租约量化边界、Provider 失败不发布、回调递归、
Binding 退休再分配、Group ID 不复用、Cookie 前零 pending、无放大、Token Bucket、精确重复、
冲突 transcript、错误状态顺序、JOIN/REAUTH 分离、跨流程总容量以及 `now==deadline` 清理。

## 9. 未放行范围

- 没有真实签名、MAC、KDF、证书链或 Trust Anchor；这些属于 V6-07。
- 没有生产 v6 Wire RX/TX；V6-03 也只能先做 default-OFF Codec。
- 没有 Flash 双槽、掉电、回滚或 MCU 实测；Host fake Provider 不是持久化证明。
- 没有把 Bootstrap 结果写入 Neighbor、Route、Path、ACL 或 Cluster。
- 本报告是内部自审证据，不是外部审计签字。

