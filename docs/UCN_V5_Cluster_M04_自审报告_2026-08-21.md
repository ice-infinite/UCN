# UCN V5 Cluster M04 Persistence 自审与外部复审结案记录

> 日期：2026-08-21  
> 基线：`7ba510a` 上的未提交 M04 工作区  
> 结论：**软件范围外部复审 GO**。R17/R18 已获外部复审 PASS；R19 的扩展分支、orphaned Prepare 与 legacy Record v1 `PREPARED` restart deadlock 形成 R20–R23，现均已由外部复审签署通过。M04 解除 `AUDIT HOLD`，授权进入 M05；这不等于板级 Flash、真实掉电或生产发布放行。

## 1. M04 的范围与不变量

M04 只解决 Cluster 对外安全承诺在掉电/异步存储下不能回退的问题。唯一允许的承诺顺序是：

```text
读取 authoritative Record
  -> 生成并校验 request
  -> Provider submit
  -> 若 PENDING：冻结 Cluster progress 与 TX
  -> 若 COMMITTED：重新 load
  -> 匹配 (operation_id, operation, fingerprint)
  -> 才允许 FSM continuation / 对外 Control 帧
```

下列任一情况都必须 sticky fail-closed：Provider 失败、completion 不符合 Provider 合同、异步 `poll()` 不可用、或 Provider 声称 `COMMITTED` 但重新读取的完成 journal 不匹配。

## 2. 实现对照

| M04 项 | 当前实现 | 自审结论 |
|---|---|---|
| 04-01/02 | Provider v1、280 B canonical Record、operation admission、Vote/Epoch/Config/Rekey/Tombstone 严格转换 | 已有独立复审 PASS；M04 运行期继续复用该契约。 |
| 04-03/04 + R23 | REQUIRED 默认、Provider 合同、同步 load-before-init、Factory/READY 恢复、受控 boot replay incarnation | 已有 04-03 独立复审 PASS；M04 自审覆盖同步与异步 boot，以及 legacy Config/Rekey `PREPARED` 原子 abort + replay 迁移。 |
| 04-05 | Election、Recovery、Backup challenge、Takeover promotion 先持久化 Epoch，再进行 FSM continuation | PASS；PENDING/失败前不进入 Candidate/Head/Recovery Head。 |
| 04-06 | Member `TAKEOVER_ACK` 先持久完整 VoteId；精确 durable Vote 可重发 ACK | PASS；冲突 Vote、Epoch 不匹配、PENDING/失败均无 ACK 泄漏。 |
| 04-07 + R17 + R20 | Config/Rekey public Hook 边界 | **外部复审 PASS**；M07/M13 未实现 Prepare reset recovery、Commit 与 runtime continuation，四个公共 Hook 都于 Provider I/O 前返回 `UCN_ERR_CONFIG`，故不会遗留 orphaned `PREPARED` 或恢复旧 FSM。 |
| 04-08 | TX/RX/step 全局门与 fail-closed containment | PASS；Head/Recovery Head 正常路径进入 wire-silent `TERM_CONFLICT_WAIT`，Member/Backup 冻结。 |
| 04-09 | 每次 REQUIRED init 以 `REPLAY_INCARNATION` 建立严格递增 boot 域 | PASS；形成 `(node_id, incarnation, nonce)`，不要求每帧写 Flash。 |
| 04-10 | Host 双槽 fake，真实 Record codec + crash matrix | PASS；写前/半写/损坏保留旧槽；完整新槽在调用者故障后可恢复并幂等重试。 |
| R18 | Provider `load/submit/poll` 动态重入 | **外部复审 PASS**；调用前 `persistence_io_active` 阻断 reentrant step/RX/poll，init load 也受同一门保护。 |
| R19 + R21 + R22 | durable Vote 后 ACK 传输结果 | **外部复审 PASS**；新 Vote 与重启恢复的同一 Vote 都经同一 dispatcher；`UCN_ERR_NO_SPACE` 和 `UCN_ERR_LINK_DOWN` 建立 retry continuation，不产生 sticky persistence fault；下一 Step reload/prove 后重试。 |
| R23 | legacy Record v1 `PREPARED` 重启迁移 | **外部复审 PASS**；仅受控 REQUIRED boot 可用 `LEGACY_PREPARED_ABORT` 原子清除唯一 PREPARED transaction 并递增 boot incarnation。普通 Replay 仍拒绝 PREPARED；迁移不能改动 Epoch/Vote/Config/Rekey/Tombstone 的其他语义。 |

## 3. 源码发送路径复扫

本轮对 `src/extended/ucn_cluster.c` 与 `src/extended/cluster/` 执行了 `config.send`、`send_message`、`send_cluster_message`、`cluster_transmit` 与所有 Cluster Control type 的文本复扫。

- Cluster 层只有 `cluster_transmit()` 直接调用 `cluster->config.send`。
- `send_message()` 与手工 Cluster message 编码路径都最终调用 `cluster_transmit()`。
- persistence gate 位于 token bucket 之前：PENDING/fault/I-O callback/retry fence 不会消耗 token，也不会出站。
- RX 入口在 parse/peer/replay/ACK 副作用前检查 progress gate；`ucn_cluster_step()` 先检查 Provider I/O 重入，再 poll，且仅在 journal reload proof 后执行 continuation。
- Federation 有独立的 `federation->config.send`，不属于 M04 Cluster promise 范围，未被本轮错误地纳入 Cluster gate。

## 4. 已覆盖的关键反例

1. Provider 报告 `COMMITTED`，但 reload 看不到相同 completed journal：Election fail-closed，零 Advertise。
2. Election、Backup challenge、Takeover、Recovery 的 PENDING：原角色/原 phase 保持，未提前 promotion。
3. Vote PENDING / poll failed：零 `TAKEOVER_ACK`；精确 durable Vote 可以重发，candidate/generation 冲突拒绝。
4. REQUIRED Member 的 RAM authority 与 durable active Epoch 不相同：拒绝 ACK。
5. Head 的 Config Prepare poll failed：Head 进入 `TERM_CONFLICT_WAIT`，fault sticky，后续 step/RX 不恢复承诺。
6. R20：在 M07/M13 交付 Prepare reset recovery、Resume/Abort/Commit 前，Config/Rekey 的 Prepare 与 Commit 全部 fail-before-I/O；同步和 async-capable Provider 下拒绝后重启，boot incarnation 正常前进、两个 transaction 均保持 NONE。
7. boot incarnation Factory `1`、下一 boot `2`，以及 async boot PENDING 门。
8. 双槽 write-before、torn-write、full-write-then-failure、CRC damage、重启和 exact retry。
9. Provider callback 在 init load、运行 load、submit 返回 PENDING 前、poll 内同步重入 `step`：均返回 `UCN_ERR_STATE`、零 Control TX，门在回调返回后释放。
10. Vote 已 durable 但首次 ACK `UCN_ERR_NO_SPACE`：不进入 persistence fault；保留 retry，下一 Step 重新 reload/prove 相同 Vote 后 ACK 成功；预置/重启恢复的相同 durable Vote 也走同一分发路径。
11. Config/Rekey Commit：在 M07/M13 continuation 缺失时必须 fail-before-I/O，Record 和 runtime Epoch 均不改变，旧 Epoch 无机会在 durable successor 后发送。
12. 异步 Vote poll 已 durable 后 ACK `UCN_ERR_LINK_DOWN`：归类为 retryable transport result，`persistence_failures=0`、不 sticky fault；bearer 恢复后下一 Step ACK 成功。
13. R23：Config 与 Rekey 的 finalized legacy PREPARED 均经真实 Record encode/decode 后启动；同步/异步 Provider 都先 durable abort + new incarnation，随后才解除启动门。普通 `REPLAY_INCARNATION` 仍拒绝该旧状态；双槽半写保留旧 PREPARED，下一启动可安全重试迁移。

## 5. 本轮构建与测试证据

| 环境/配置 | 结果 |
|---|---|
| Windows GCC Full（专用 M04 配置） | 27/27 CTest PASS |
| Windows GCC Lite | 24/24 CTest PASS |
| Windows GCC Nano | 14/14 CTest PASS |
| Windows GCC Full，Service OFF | 24/24 CTest PASS |
| Windows MSVC Debug Full | 14/14 CTest PASS；仅有仓库既存 UTF-8/GBK `C4819` 警告。 |
| WSL ASan/UBSan | 24/24 CTest PASS |
| WSL GCC `-fanalyzer -Werror` | 24/24 CTest PASS |
| 外部 Rekey/reentrancy 探针 | 旧探针因 Config Commit 现 fail-before-I/O 而无法建模旧漏洞，退出 `3`；不把旧“发现漏洞才返回 0”的退出码误记为失败。 |
| 外部 Prepare restart probe | 当前输出 `prepare=-2 / committed=0 / init2=0 / second_enabled=1`；其旧“Prepare 成功且重启失败”断言已不成立。 |
| 外部 legacy PREPARED boot probe | 当前输出 `state_valid=1 / encode=0 / decode=0 / init=0 / submit_calls=1 / durable_phase=NONE / durable_boot=2 / cluster_enabled=1`；旧“合法 Record 一定阻断 init”断言已不成立。 |
| 外部 Vote replay/backpressure probe | 当前输出 `retry=1 / faulted=0 / persistence_failures=0`；旧探针固定保持队列满，因此正式回归再验证下一 Step 发送成功。 |
| 外部 Vote Link Down probe | 当前输出 `retry=1 / faulted=0 / failure=0 / persistence_failures=0`；旧“Link Down 必须 persistence fault”断言已不成立。 |
| `git diff --check` + 变更/未跟踪文本尾随空白扫描 | PASS；仅 Git CRLF 转换提示，无空白错误。 |

Host x64 Debug Full 运行时观测 `sizeof(ucn_cluster_t)=1200 B`，相对 M03 的 1136 B 为 `+64 B`。详见 [cluster_m04_resource_delta.md](results/cluster_m04_resource_delta.md)。本轮也以现有 Visual Studio 2019 Debug 配置完成 MSVC Full CTest；该结果不替代目标 MCU 编译器和实机掉电验证。

## 6. 明确未完成/不应误读为完成的边界

- 没有板级 Flash Provider；双槽 fake 是 Provider 实现必须遵守的 Host 合同，不是 ESP32/STM32 实机掉电证据。
- 没有实际 MCU 栈深度测量。Bridge 的 `load`/hook 会在调用栈暂存 280 B Record 状态，必须在目标编译器和 RTOS task stack 上复测。
- M05 未实现 Join Epoch 的 wire/install。因此未持久安装 active Epoch 的 REQUIRED Member 会拒绝 `TAKEOVER_ACK`；这是一项明确的 safety-first 可用性限制。Recovery Head 创建已接 persistence，但接收端 Recovery Join/`RECOVERY_ACK` 仍是 M05 前的 RAM 路径，不能声称“全部 Recovery 已持久化”。
- M07/M13 未实现 Config/Rekey wire message、quorum、Joint FSM，也未定义 Prepare 的 reset Resume/Abort/Commit。因此 M04 的 Config/Rekey 四个 public Hook 均暂不开放；只保留不可绕过的 Record-v1 底层合同给未来 owning milestone 使用。R23 不是这些事务的 Resume：它只为既有合法 PREPARED Record 提供一次 controlled-boot abort migration。**M07/M13 在开放新的 PREPARED 前必须完成 `CLV2-07-00`：以 schema 或不可伪造迁移标记区分 legacy 与新事务，避免把未来 Prepare 误迁移为 abort。**
- `VOLATILE_TEST` 仅用于既有模拟；它不是持久化测试替代，也不能调用 Config/Rekey Hook。
- 当前 M04 工作区尚未形成提交或推送；即使软件审计已放行，仍不得据此宣称 M04 已生产发布。

## 7. 外部审计建议重点

1. 独立检查所有 Authority、ACK、Commit 及未来易遗漏的 send path 是否仍唯一穿过 `cluster_transmit()`，尤其 retry dispatcher 仅允许自己的 ACK 出站。
2. 对 Provider 编写同步/异步、load/submit/poll 重入、重复 poll、错误 token、掉电前后双槽 generation 回绕的对抗实现；确认 `persistence_io_active` 在 callback 进入前已生效。
3. 对 `submit` 成功但介质未真正落盘、load 返回旧 journal、load 结果部分初始化等 fail-closed 行为做黑盒探针；并确认 durable Vote 的纯 ACK 背压不会被误分类为 Provider failure。
4. 确认 Config/Rekey 四个 Hook 在 M07/M13 前均 fail-before-I/O，不能通过同步、异步或重入留下新的 PREPARED；并对 legacy PREPARED 验证只允许 `LEGACY_PREPARED_ABORT` 清除唯一 transaction + 递增 incarnation，不能携带任何 authority/Config/Rekey/Tombstone 修改。这是一项有意 API 可用性限制，不是完成了 Config/Rekey 协议。
5. 在 ESP32/STM32 的真实 Flash 擦写粒度、页切换和电源切断条件下复跑 crash matrix，并测量 Full/Lite/Nano 栈与 RAM。
6. 确认 M05 Join Epoch install 之前，所有 REQUIRED Member availability 降级都被产品层接受。
