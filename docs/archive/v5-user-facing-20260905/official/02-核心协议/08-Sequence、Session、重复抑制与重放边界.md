# Sequence、Session、重复抑制与重放边界

> 文档级别：`NORMATIVE`
> 实现状态：`CURRENT`；生产抗重放仍依赖产品 Security Provider 与持久化策略
> 事实源：Core Frame/Node duplicate windows、RREQ cache、Security Provider contract
> 最近核对：`a093862`，2026-08-26

## 1. 为什么仅有 Sequence 不够

网络可能因为多路径、重试、广播或环路让同一帧到达多次。Sequence 用于区分同一发送会话内的帧顺序，但节点重启后 Sequence 可能从较小值重新开始；如果只按 Source + Sequence 去重，新启动的合法数据会和旧数据冲突。

UCN 因此把普通帧身份绑定为：

```text
(Source Node ID, Session ID, Sequence)
```

- Source 说明谁发送；
- Session 区分同一 Source 的不同启动/安全会话；
- Sequence 区分会话内的具体帧。

这组身份用于有限窗口重复抑制，但并不自动证明 Source 真实可信。

## 2. 普通业务帧的滑动窗口

Node 为有限数量的 `(Source, Session)` 保存 Source Window。每个 Window 记录目前看到的最高 Sequence，并用 32 或 64 bit 位图表示其附近已接收位置。

收到帧时的概念流程为：

1. 查找相同 Source + Session 的 Window；
2. Sequence 比窗口最高值新：推进窗口并标记；
3. Sequence 落在窗口内且对应 bit 未标记：接受合理乱序；
4. 对应 bit 已标记：判为重复；
5. Sequence 比窗口还旧：判为超出窗口的陈旧数据；
6. 没有 Window：按固定回收规则占用一个槽。

位图允许多路径产生少量乱序，而不是强制“后一帧到达后前一帧永远无效”。窗口大小越大，容忍乱序越多，但每个 Source 消耗的 RAM 也越多。

## 3. 固定窗口的容量与回收后果

Source Window 表是有界 RAM，不会随网络节点数动态增长。当同时活跃 Source 超过容量时，旧槽可能被回收。回收后再次出现的旧帧可能不再命中原窗口，因此：

- 普通 Window 是资源受限的重复抑制机制；
- 它不能单独承担无限期安全重放防护；
- Profile 容量要按“同时活跃发送源”而非“网络理论总节点数”估算；
- 高安全命令还需 Command ID、持久化序列或业务 generation。

## 4. RREQ 为什么不用普通去重窗口

路由发现中，同一 RREQ 事务可能沿多条路径到达中继。较晚到达的一份虽然 Token 相同，但累计 Cost 更低，继续传播它可能建立更优路线。

如果套用普通“见过 Sequence 就全部丢弃”的规则，第一个到达但质量较差的路径会永久压住更优路径。因此 RREQ 使用独立的 Best-Cost/Token 状态：

- 完全重复或不更优的请求可抑制；
- 同一发现事务中明显更低 Cost 的候选可继续传播；
- 表仍是固定容量并有超时；
- RREQ 缓存和业务 Source Window 不能合并。

## 5. Session 的来源和生命周期

明文测试产品可以显式配置一个合法非零 plain Session。生产安全产品应让 Security Provider 负责 Session/Key/Sequence 生命周期，至少解决：

1. 开机如何取得不会和旧会话混淆的新 Session；
2. Sequence 下一值如何持久化，掉电后不能回退到已使用区间；
3. Key 或 Session 轮换如何原子切换；
4. 持久化损坏时是停止发送、重建身份还是要求人工恢复；
5. 恢复后旧密文和旧命令是否还能被接受。

Node ID 固定并不意味着 Session 也固定。恰恰相反，同一 Node 的新启动应能和旧启动区分。

## 6. 重复抑制与安全抗重放的区别

| 能力 | Core RAM Window | 生产 Security/业务策略 |
| --- | --- | --- |
| 抑制短期网络重复 | 可以 | 可以辅助 |
| 容忍小范围乱序 | 可以 | 必须与认证顺序一致 |
| 验证 Source 身份 | 不可以 | 必须 |
| 掉电后记住旧 Sequence | 不可以 | 必须持久化或换安全会话 |
| 阻止攻击者伪造更大 Sequence | 不可以 | 依赖认证 Tag/Key |
| 无限时间拒绝旧命令 | 不保证 | 依赖持久 ID、generation、业务状态 |

所以 Host 测试中 `duplicates=0` 或“重复帧被丢弃”只证明当前内存窗口行为，不能写成“产品已经抗恶意重放”。

## 7. 验证顺序为什么重要

接收端不能让未认证帧任意推进可信重放状态，否则攻击者可以伪造一个很大的 Sequence，把之后合法帧全部变成“旧帧”。安全产品需要明确：

1. Wire 结构与长度检查；
2. 基于不变 Header/AAD 的认证检查；
3. Session/Sequence 重放判定；
4. 只有可信接受后才提交安全状态；
5. 再进入 Endpoint/业务分发。

具体顺序以 Security Provider 合同为准，但必须避免“认证失败仍污染安全窗口”。

## 8. Serial no-wrap 和普通 Sequence 回绕

Cluster Term、Config generation、事务 txid 等权威 Serial 使用 no-wrap 阈值。达到阈值必须 rotate/rekey/创建新身份，不能自然回绕后继续比较。

普通 Frame Sequence 同样需要产品定义会话轮换策略。不能只依赖 C 语言 `uint32_t` 自动回绕，然后假设旧帧和新帧仍能无歧义排序。安全产品应在接近阈值前换 Session/Key，并把新状态持久化完成后才发送新会话帧。

## 9. 多路径、重传和业务幂等

重复抑制防止同一 Frame 被多次分发，但无法保证业务请求只执行一次：

- 发送方超时后可能创建一条新 Frame 重发同一业务命令；
- 两条 Frame 的 Sequence 不同，Core 会把它们都视为新帧；
- 远端任务如果没有 `command_id` 幂等表，就可能执行两次。

高风险命令应在 Payload 中携带产品级 Command ID，目标业务持久或有界记录已经处理的 Command ID，并让 Result Endpoint 对重复请求返回相同终态，而不是重复执行。

## 10. 重启与恢复示例

假设 Node A 的 Session 7 已发送到 Sequence 1000：

- 正确恢复：A 重启后取得 Session 8，从新的 Sequence 域开始；B 为 `(A,8)` 建新窗口；
- 可接受的持久恢复：A 从持久化预留区间之后继续 Session 7，例如从 2048 开始；
- 错误恢复：A 仍用 Session 7 且 Sequence 从 1 开始，B 会拒绝或在窗口回收后产生安全歧义；
- 更错误的做法：为了“能通信”而清空 B 的所有安全历史并接受旧 Session。

## 11. 测试清单

- [ ] 同 Source/Session 的精确重复被拒绝；
- [ ] 窗口内乱序的首次到达可接受；
- [ ] 超出窗口的陈旧帧被拒绝；
- [ ] 新 Session 不继承旧 Session 位图；
- [ ] Window 满载和回收行为确定且无越界；
- [ ] 更低 Cost 的同 Token RREQ 不被普通去重误杀；
- [ ] 认证失败不会推进可信重放状态；
- [ ] 掉电恢复不会重复使用已发送的安全序列域；
- [ ] 业务重复命令由 Command ID 幂等处理；
- [ ] 接近 no-wrap 阈值时 fail-closed 并触发轮换。
