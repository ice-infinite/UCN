# RUST-SW-02 自动多跳 Data 协调器自审报告

## 1. 本轮范围与不支持项

2026-10-18，在 C1/Hop 原语后继续离线软件任务。依据[共同合同16](../../10-理论与规划/建议方案/UCN_v6_C_Rust共用合同收口/16-自动多跳Data协调器与本地下一跳合同.md)，
新增 Rust `RoutedNode`。真实 Foundation/Identity/Session/Capability 是前置条件，不接收
测试伪造的“认证成功”布尔值。普通 `send()` 自动启动路由发现、排队、逐跳发送，终点
通过公开拉取接口一次交付。C1 Header、AAD、Opcode、Body 和协议 JSON 均没有新增字段。

仅支持固定已知邻居、预先发布端到端 Origin Session、动态 SoftRoute、尽力单向 Data。
本段**没有**完成 C 自动多 Peer Root、动态跨路端到端握手、Reliable/Transfer/RPC/Journal/
Time 全量统一 Node、Flow、Group/Cluster 控制或 C↔Rust Runtime 互通。

当前 `send/step` 仍显式借入实际基础 Owner、可信 now 和 Provider/workspace；这是产品
Coordinator 入口，不是已隐藏全部装配细节的最终用户 Facade。

仍未操作实机、COM/USB 枚举、烧录/复位、目标调试、Git 暂存/提交/推送或 Vault 修改。
旧 OP-580/581 的矩阵保留为历史，不继承为当前源码的最终证据。

## 2. 分项即时自审

| 小项 | 实现 | 实际自审 |
| --- | --- | --- |
| DN-01 | 固定 Endpoint、Data TX、App RX 和实际父域复验 | 输出不足保留原消息；错策略和无路径不消耗 Origin 序号 |
| DN-02 | 来源、邻居安全域分离及本地 Driver locator | A→B→C 和反向；共享 Link 的 B 必须选择正确100/102邻居，不从最终地址猜下一跳 |
| DN-03 | C0/C1 唯一原子 RX 和私有路由分派 | 实际 Atomic Adapter claim/retire；错误 Adapter 在领取前拒绝，帧仍可领取 |
| DN-04 | 有界六类轮转、6:3:2:1 TX 和双退休通道 | 同一TX槽竞争；连续明确未提交后成功，持续背压到总次数耗尽 |
| DN-05 | Unknown、stop 和真实迟到 completion | forget/退休拒绝，stop不重发，reopen仍不构成退休证明，真实completion后释放物理Holder |
| DN-06 | 实际业务端到端接收和拒绝路径 | 目标完整消息只取一次；精确Hop重放、TTL耗尽不交付，哨兵保持 |
| DN-07 | 全量矩阵、裁剪和资源 | 定向10项通过；最终完整证据单独登记，不复用原语层354项结果 |

可定位入口：[生产协调器](../../../rust/crates/ucn-runtime/src/route_runtime/data.rs)、
[实际三节点测试](../../../rust/crates/ucn-runtime/tests/bootstrap_runtime/route_tests/data_node.rs)。
定向命令：`cargo test -p ucn-runtime --test bootstrap_runtime route_tests::data_node`（在 rust 目录）。

| 正式测试 | 区分的反例 |
| --- | --- |
| automatic_three_nodes | send自动发现并正反向交付；完整Origin字节保持，重复不交付 |
| automatic_three_nodes_with_fragmented_route_control | 127 B链路的局部控制分片，不扩大普通数据头 |
| temporary_backpressure_preserves_send | Data明确未提交两次后成功，完全相同字节重试 |
| unreachable_deadline_and_policy_rejection_do_not_burn_origin_counter | 原截止不续期，未找到路径不消耗Origin |
| unknown_data_submission_stays_owned_after_stop_until_exact_late_completion | stop/reopen不代替物理证明，不抹除Unknown |
| ttl_exhaustion_does_not_forward_or_deliver | TTL=1的中继不能转发或虚假交付 |
| foreign_adapter_rejects_before_claiming_its_raw_rx | 错Adapter的输入仍保留，不误领其他Owner的帧 |
| bounded_data_backpressure_never_reseals_or_reports_delivery | 固定5次耗尽，只消耗一次Origin，不声称目标收到 |
| unread_application_slots_are_never_silently_evicted | 两个未读消息不能被第三个静默淘汰 |
| source_queue_capacity_rejects_before_counters_or_driver | Source六槽已满，额外send不改变队列、计数器或Driver |

## 3. 第一轮整体自审：信任与路由域

1. 审查真实入口：成功路径的测试只调用 Node register/send/step/receive，不在测试里手工
   调用 open_packet 伪装业务已送达；测试 Driver 的路由模拟仍将每帧放进实际原子 RX。
2. 下一跳提示只在本地 SPI 存在。它不是 Principal、不上 Wire，也不是新安全标签。
   无默认 fallback 让 Driver 忽略它；生产承载必须单独证明映射正确。
3. 反向业务回归先失败：C 本地使用 RREQ 返回提示发送成功，B 没有 C-Origin 的对应路由。
   修复为 Source 使用精确当前 RREP 回执才可封装；否则自动发起其自己的发现。修复后
   正反两个 Origin 域均自动收发，不允许用邻居的本地 RouteDomain 替代原始发送者。
4. 中继不调用 Origin 解密；收到的 Source/Destination/Service/Origin Sequence/Payload/
   Origin Tag 不变，TTL减1和H1重新认证。目标另验Origin Session及精确ACL和Replay。
5. Hop与Origin pending均先完成结构、父域、容量及当前事实预检，再在独占Coordinator
   quantum提交；失败释放未消费reservation。没有把邻居认证扩大成Endpoint权限。

## 4. 第二轮整体自审：生命周期与测试区分度

1. Driver `NOT_SUBMITTED` 才允许重试精确缓存，不重新封装或重用Nonce加密不同内容。
   测试专门作用于 C1 Data，而非只在路由控制阶段制造背压；最终传出的完整帧逐字节一致。
2. 持续背压只允许固定5次，本地返回 Exhausted；无远端交付，没有多消耗Origin序号。
   无路由的原截止另测Timeout，避免用“次数先耗尽”冒充Deadline分支覆盖。
3. Unknown不是未提交。明确停止仍不能擦除Token；根据已有Adapter合同先失效Link，再
   接受精确旧Token的真实completion；只reopen仍拒绝退休。不把迟到成功追认成业务成功。
4. App槽未读取不能覆盖。太短输出、重复读取和重放的零写哨兵均验证；统计区分Source接纳、
   Relay接纳、Driver本地成功、目标接纳和App拉取，不将不同漏斗差值称为丢包率。
5. Root直接拥有空的Route/Control固定存储，不把活动Holder移动进可能失败的构造函数。
   没有新增堆分配、unsafe或完整Owner自动栈副本。
6. Feature组合中显式补 `route-data-runtime` 和与 `peer-reauth` 的组合；Core最小依赖不
   因默认Runtime组合增加高级模块。Rust泛型固定容量不代表实际MCU最坏栈已验收。
7. 进一步补充满队列对抗测试：Source六槽已满，第七次send拒绝且Driver/计数器不动；
   Target两槽未读时第三条不能驱逐前两条。三次本地TX成功而目标只接纳两条时，统计仍
   分开报告。中继逐字节比较覆盖完整Origin Tag，只排除既有16 B H1尾部。

## 5. 资源与最终证据

Host 64-bit `sizeof`，仅 Root 本体：

| 配置 | 大小 | 对齐 |
| --- | --- | --- |
| P2/C4/Q2/App1/Endpoint1/B128，Route1/Reverse2/Route4/Static1 | 7824 B | 8 B |
| 实际三节点测试 P3/C12/Q6/App2/Endpoint2/B256 | 19624 B | 8 B |

上表不包含 Identity、Security、Capability、Adapter、Provider、独立 scratch、Driver 或调用
栈。Cortex-M/ESP 编译通过也不证明真实板卡 RAM/栈、ISR/WCET 和功耗已通过。

最终矩阵UTC `2026-10-18T14:05:24.519353+00:00`；[不可变LF规范化归档](../软件证据/Rust/2026-10-18/software-matrix-sw02-automatic-routed-data-final.json)
SHA256：`75E7BC8DA2A74F064198E454179844C303F08BE5C9E944C49FBF7B36D781EFCD`。
Windows工作目录中的原始JSON为CRLF，摘要是
`5084300F40914F4E9DCCA6E45AC44FD21DB0822B800C18C0FA08404158A14DCF`；两份JSON内容一致，
不可混用字节级哈希。归档逐项记录426份实际源码依赖，独立SHA256复算0漂移。

| 最终验证 | 实际结果 |
| --- | --- |
| Rust Debug / Release / MSRV1.85 | 各364/364 |
| 全矩阵 / 源码稳定 | 12配置exit0，source_stable=true，426项独立复算0漂移 |
| 定向Node回归 | 10/10 |
| Optional裁剪 | 32组合通过；最小Core与三类Optional边界保留 |
| C/Rust/Python raw / Admission Golden | 91向量 / 7项通过 |
| Clippy workspace all-targets / fmt | 通过 |
| Cortex-M soft/hard / ESP32-S3 | compile-only通过，不是实机运行 |
| C Full | 本轮重建检查及CTest 108/108；未冒充全新独立工具链矩阵 |
| C Lite / Nano | 本轮重建，路由/Wire/Capability/Security定向各5/5 |
| 公共C API索引 | 318项最新；本段不修改C公共API |
| 文档最终门禁 | 152篇、412条本地链接通过 |
| Git | diff-check无空白错误，仅既有CRLF提示；真实暂存区为空 |

先完成362项矩阵后，又补两个容量反例并修正完整Origin字节比较，最终重新完整执行得到
上面的364项结果。没有把先前绿灯或OP-580/581的348/354项历史矩阵当成最终源码证据。
DN-01..07和两轮整体自审完成；本段未独立复跑全部C MSVC/Clang/TSan，不沿用历史数字。

当前仍是受限软件自审候选；RUST-SW-02保持IN PROGRESS。下一步优先收口动态端到端
Session/重认证后的registry交接、全模块统一入口和Flow，以及C等价自动接线与互操作；
Group/Cluster继续按独立Optional合同推进。外审、生产密码、Flash/Witness和实机仍HOLD，
不能宣称“所有软件已完成，只剩硬件”。
