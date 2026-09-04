# UCN V6-08 RouteSet、Path 与 Multipath 实现报告

## 1. 目标与阶段边界

V6-08 将 V6-02 的身份所有权、V6-07 的安全 Session 和 V6-06 的 Path Capability 收束成
唯一 RouteSet 模型。它解决“谁发往谁、属于哪次地址租约和 Session、使用哪一代 Route、
经哪个 Next Hop/Link/Path”的精确归属，并防止发现、Probe、Activate、ACK 分属不同路径
时被错误拼接。

本阶段仍处于 `UCN_BUILD_V6_EXPERIMENTAL=ON` 的隔离 archive：不接 v5 Node，不让默认
产品解码或发送 v6 Route 控制帧，不把 Host 模型测试称作无线/CAN/串口实机证明。

## 2. Route Domain 与代际

Route Domain 固定为：

```text
{origin Principal,
 origin Realm/Address/Binding Generation,
 origin Session Generation,
 destination Principal,
 destination Realm/Address/Binding Generation}
```

Route Generation 是该域的子代际：首代必须为 1，之后必须是当前代的精确 checked-next，
达到 `UCN_V6_SERIAL_ROTATION_THRESHOLD` 后拒绝继续分配。它不跨 Origin Session 比较；
节点重启后，V6-07 会把持久 Session 置为 `requires_reauth`，完成 REAUTH 时推进 Session
Generation，因此新的 Route Owner 不会在旧 Session 域复用 Route Generation 1。

Candidate Transaction ID 在同一 Route Domain 内也只允许严格增加。Owner 为每个固定
Domain 槽保存 64-bit high-water；失败、到期或取消 Candidate 不会释放历史 ID 供复用。

## 3. 固定容量对象

所有容量进入 V6 Manifest/Layout Hash：

| 对象 | 默认容量 | 满载行为 |
|---|---:|---|
| Route Domain/high-water | 16 | `NO_SPACE`，不驱逐历史域 |
| RouteSet | 16 | `NO_SPACE`，旧 Route 保持 |
| Path / RouteSet | 4 | `NO_SPACE`，Proposal 不变 |
| Candidate | 16 | `NO_SPACE`，不清理其他事务 |
| Flow Pin | 32 | `NO_SPACE`，不退化为逐包随机 |

`ucn_v6_route_owner_t` 为 opaque caller-owned storage，初始化前验证 Manifest、容量和对齐；
运行期检查 Magic、Schema、Layout Hash 与 Canary。没有堆分配、LRU 驱逐或输入驱动清槽。

## 4. Candidate 冻结规则

Candidate 创建时固定 Route Domain、Transaction ID、Proposed Route Generation 和绝对
Deadline。经认证 RREP 得到的 Path 必须同时满足：

1. Path 的目标 Principal/Binding 与 Route Domain 完全一致；
2. Route/Path Generation 与 Candidate 完全一致；
3. Next Hop Session、Egress Link/Generation、Hop Count、权重与优先级合法；
4. V6-06 Capability Owner 中存在同一份未过期 Path Capability；
5. Path ID 和 `{Next Hop, Egress Link}` 在 Proposal 内不重复。

首次记录 Probe 成功前可以继续收集 Path；首次 Probe 会一次性冻结完整 Proposal。冻结后
任何 `add_path()` 都返回 `UCN_V6_ERR_STATE`，无论改动发生在 Probe 后、Activate 前、
Activate 等 ACK 时，还是一次物理发送失败后。换路只能使用更大的新 Transaction ID 启动
新 Candidate。

Proposal Digest 不使用 C 结构体原始内存，而以字段级 canonical 顺序覆盖：双 Identity、
Route/Path Generation、Next Hop Session、Link、Hop Count、优先级、权重以及完整 Path
Capability。它用于精确事务关联；真正的网络认证仍由 V6-07 Hop/E2E Tag 负责。

## 5. Activate、重试与原子 ACK

只有 Proposal 内所有 Path 都有 Probe 证明时，Owner 才会导出不可变 Activation：

```text
{candidate transaction ID, route domain,
 route generation, proposal digest}
```

物理发送失败不增加 `activation_attempts`，也不允许改写冻结 Proposal。成功提交后增加尝试
次数，并按固定 Retry Interval 产生同一 Activation 内容；外层 Packet Sequence 应由
V6-07 重新分配。达到最大尝试次数或 Candidate 总 Deadline 时失败关闭，清理动作只能由
显式 Owner timer 执行。

ACK Commit 前依次完成：

1. Candidate、Domain、Route Generation、Digest 与已发送状态精确匹配；
2. 当前 RouteSet 仍允许该 Generation 作为精确下一代；
3. 新 RouteSet 有固定槽，Previous Grace Deadline 可安全构造；
4. 每条 Path 再从 Capability Owner 读取并逐字段核对，排除到期、换 Session、换 Link、
   换 Capability 或被撤销的预算。

全部通过后才一次性提交 Current，并把旧 Current 复制为有界 Previous。失败不会写 Route、
删除 Candidate、清 Flow Pin 或增加 activation 成功统计。

## 6. 路径选择策略

- `PINNED`：只接受调用方给出的精确 Path ID/Generation；失效即返回错误；
- `ACTIVE_STANDBY`：优先使用 Proposal 首选 Path，否则按静态 Priority 选健康备用；
- `PER_FLOW_HASH`：对同一 Flow 建固定 Pin，保持 Route Generation 内顺序稳定；
- `WEIGHTED_MULTIPATH`：按 Weight 和 Packet Sequence 选路，必须由 Endpoint 明确声明允许
  乱序或具备重排，否则返回 `ACCESS`。

所有策略只在 `available && now < capability.deadline` 且能从 V6-06 Capability Owner
重新读到逐字段完全相同能力的 Path 中选择。Capability 到期或推进后，旧 Route 即使仍在
RouteSet 内也不能发送；V6-07 撤销/替换 Session 时再调用精确 invalidation 回收其
RouteSet、Candidate 与 Flow Pin。V6-09 将在这些安全候选之上添加 Metric/QoS，而不是
绕过 Route Domain 或直接改写 Candidate。

## 7. RERR、Previous Grace 与失效

RERR 必须精确匹配 Route Domain、当前 Route Generation、Path ID 与 Path Generation。
迟到或跨域 RERR 返回 `REPLAY/NOT_FOUND`，不影响其他 Origin 或 Path。命中后只撤销对应
Path，删除绑定该 Path 的 Flow Pin；其他 Path 仍可完成主备切换。

入站帧可在有界 Grace 内匹配 Current 或 Previous Route Generation；`now == deadline` 已
过期。只有显式 `ucn_v6_route_expire()` 会清理 Previous、Candidate 或 Flow Pin，敌对输入
不能借“顺便过期”修改无关状态。

## 8. 分项自审与反例

| 小节 | 自审结论 |
|---|---|
| 08-01 Domain/Generation | 双 Principal/Binding/Session 精确；Route 和 Candidate ID 不回绕、不复用 |
| 08-02 Candidate/RREP | 跨 Origin、Binding ABA、重复 Path、表满均零写拒绝 |
| 08-03 Probe/Freeze | Probe 后、发送失败后、等 ACK 时均不可换 Path |
| 08-04 Activate/ACK | 错 Generation/Digest/未发送/迟到 ACK 不提交；Capability Commit 前重验 |
| 08-05 RouteSet/Grace | Current/Previous 原子交换，Grace 半开且由 timer owner 清理 |
| 08-06 Multipath | Pinned、主备、Flow Pin、显式乱序 Weighted 均有定向回归 |
| 08-07 RERR/Capacity | 精确撤销、Pin 清理、固定容量不驱逐和失败输出不写回均覆盖 |

自审期间发现并修正两处初始缺口：Proposal Digest 原先未包含目标 Session Generation 和
Hop Count；Candidate ID 原先只检查当前活动槽，过期后可能复用。最终实现已分别补齐
canonical 字段与 Domain high-water，并加入回归。

## 9. 验证结果与未完成项

| 门禁 | 结果 |
|---|---|
| Windows GCC Full（Scale/Cluster 模拟关闭） | 39/39 |
| Windows GCC Route/Config | 2/2 |
| MSVC Release Route/Config | 2/2 |
| WSL ASan/UBSan Route | 1/1 |
| WSL `-fanalyzer -Werror` Route | 1/1 |
| default-OFF `ucn_core` v6 symbol | 0 |
| `git diff --check` | 无空白错误，仅行尾转换提示 |

仍未完成：生产 Route Control Wire/RX/TX、真实 RREQ/RREP 跨节点交换、动态 Metric 与
Hop Budget、Transfer 流水、真实 Bearer 切换、分区恢复和 MCU RAM/CPU/时延测量。它们分别
属于 V6-09、V6-10、V6-13/14；本报告不作这些能力的完成声明。

当前状态：`V6-08 软件实现与分项自审完成 / FINAL EXTERNAL REVIEW DEFERRED`。
