# UCN v5 最新审计交叉问题修复建议

> 建立日期：2026-08-11
> 源码基线：`codex/v5-adaptive-wire@a8567e0`
> 适用范围：v5 Adaptive Wire、AODV-Lite、Candidate、Q1 Freshness 与产品 Hop Scope 的交叉行为。
> 实施原则：MCU-first、固定内存、失败关闭；不把 Wire Profile 当权限，不以 Host/虚拟测试替代真实 MCU 与介质验证。
> 阶段状态：V5-22～V5-26 已实现、测试并以 `f941ae9` 推送；第 1～8 节保留修复前问题与决策，第 9 节描述 V5-26 当时结果。V5-30 增量见 [异构 Bearer、动态 MTU 与 Policy 修复报告](../08-实现与验证/版本演进/UCN_V5_27_异构Bearer与动态MTU修复报告.md)，V5-33 当前发布修复见 [PATH_INSTALL 兼容与 API 符号修复报告](../08-实现与验证/版本演进/UCN_V5_31_PATH_INSTALL兼容与API符号修复报告.md)。

## 1. 结论

最新审计提出的四项 P1 均有当前源码依据，应该在下一轮实机测试前完成：

1. Pending Q1 内部重试不得刷新旧值的绝对 Deadline，也不得在发送尝试前先清除槽位；
2. W0/W1 的线上累计 Cost 位宽必须覆盖其合法最大 Hop 范围，不采用只修单跳的 W0 2 B 方案；
3. Candidate 必须保存发现时的 Wire Profile，Probe、ACK、Activate 和 Route Epoch 都在同一 Wire Domain 中运行；
4. `UCN_MAX_HOPS` 与 Node `default_hop_limit` 必须在 Full/Lite/Nano Ingress、路由学习和 Path 安装中形成真实产品硬边界。

两项 P2 文档问题同时修正：Cost 契约以 `include/ucn/ucn_link.h` 为准；V5-17～V5-20 的实现/推送状态按当前远端事实更新。

生产身份、审计 AEAD、密钥/Session Generation、逐跳控制认证和 Authorized Class 执行层继续归 S02/V5-21。本轮不得用 Wire Profile、Node ID 或 Bearer 自报信息伪实现权限。

## 2. V5-22：Pending Q1 绝对 Deadline

### 2.1 当前问题

Pending Q1 Ready 后会先清除 `occupied`，再重新调用公开发送入口；如果当前 Link/Route 存在但不满足 Hop、Cost 或已验证 RTT 约束，发送入口会再次排队并生成新的 `now + timeout`。旧 Latest Value 因此可能被反复续命，临时背压也可能让已清除的 Pending 丢失。

### 2.2 冻结行为

- 应用首次提交某个 `(destination, Endpoint/Message Type)` 时创建绝对 Deadline；
- 同一键的新应用值覆盖旧 Payload，并允许刷新 Deadline；
- Protocol Task 内部重试复用原 Deadline，不允许重新进入 Pending Queue；
- 发送成功、Deadline 到期或明确永久错误后才清除槽位；
- `NOT_FOUND`、`NO_SPACE`、`LINK_DOWN` 等可恢复结果保留槽位至下一次 Step；
- 每个 Step 最多处理一个 Pending，保持有界调度和固定内存。

实现采用内部 `allow_pending_queue`/等价参数，避免复制公开 API，也不在 `ucn_pending_q1_item_t` 增加动态对象。

## 3. V5-23：累计 Cost 线上表示域闭环

### 3.1 为什么 W0 改 2 B 仍不足

单跳 Link Cost 契约允许 `1..65534`，各 Profile 最大合法累计值为：

| Profile | 最大 Hop | 最坏累计 Cost | 最少字节 |
| --- | ---: | ---: | ---: |
| W0 | 4 | 262,136 | 3 B |
| W1 | 16 | 1,048,544 | 3 B |
| W2 | 64 | 4,194,176 | 3 B |
| W3 | 254 | 16,645,636 | 3 B |

3 B 线上字段保留 `0xFFFFFF` 为 Unknown 后，最大 Known 为 `0xFFFFFE=16,777,214`，可覆盖所有官方最大 Hop 下由合法单跳 Cost 形成的动态路线。

### 3.2 采用方案

固定 Cost Width：

```text
W0 / W1 / W2 / W3 = 3 / 3 / 3 / 4 B
```

- 普通业务 Base/Route/Path Header 不变；Cost 只存在于 RREQ/RREP 控制 Payload；
- RREQ 长度更新为 `10/11/12/14 B`；
- RREP 长度更新为 `10/11/11/12 B`；
- 不采用 Profile 相关 Cost 缩放、不途中升档、不要求 Adapter 理解 Wire Profile；
- 保留 W3 4 B 和 Canonical `uint32_t`，不缩窄内部上限；
- v5 尚未冻结，本项作为 pre-release Wire Schema 修正；旧/new v5 固件不能混合参与动态寻路，升级时必须整网一致。

## 4. V5-24：Candidate Wire Profile 连续性

`ucn_candidate_route_t` 增加发现时的 `wire_profile`。RREQ/RREP 学习 Candidate 时记录收到的 Profile；相同 Candidate ID 若出现不同 Profile，失败关闭，不能拼接不同 Wire Domain。

发送规则：

- Source 的 `PATH_PROBE`、`PATH_ACTIVATE` 使用 Candidate Profile；
- Target 的 `PATH_PROBE_ACK`、`PATH_ACTIVATE_ACK` 继承请求 Profile，并核对本地 Candidate Profile；
- 中继保持收到的 Profile，不升档或降档；
- `allocate_route_epoch()` 按 Candidate Profile 分配，W0 只使用 `1..254`，回绕后从 1 继续且禁止 0；
- 固定 Wire Profile 模式保持现有行为。

核心交叉场景是 `W3 TX Maximum + Auto Source → W0-only Relay → W0-only Target` 完成 RREQ、RREP、Probe、ACK、Activate、ACK 和 Data 全链。

## 5. V5-25：产品 Hop Scope 硬门禁

运行期有效上限为 `node->config.default_hop_limit`，它不得超过编译期 `UCN_MAX_HOPS`。接收顺序冻结为：

```text
3 B Profile Peek
→ 完整 Decode/CRC
→ Network ID
→ 产品 Hop Scope
→ Security Authorize
→ Duplicate/Neighbor/Route/Path 状态修改
```

具体规则：

- 所有 Frame 的当前 `hop_limit` 不得超过 Node 运行期上限；
- RREQ 同时验证已走 `hop_count` 和原始 Ring Scope，禁止高范围 RREQ 走到后段后进入低范围 Node；
- RREP/Route/Candidate 学习后的 `hop_count` 不得超过运行期上限；
- PATH_INSTALL 的 `remaining_hops` 不得超过运行期上限；
- Nano 静态转发执行同样的普通 Frame Scope 门禁；
- 超范围帧返回 `UCN_ERR_TTL` 并增加独立统计，不对广播请求产生回复放大。

“可解析 W3”只表示统一 Decoder 能理解 W3，不表示一个 4/8/16 Hop MCU 必须替 64/254 Hop 域转发。

## 6. 明确保留、不修改的内容

- S02 生产安全仍是发布门禁；
- V5-21 继续阻塞，不从 Wire Profile 推导权限；
- Extended Gateway/Alias/跨域目录仍归 V5-06；
- `route_cost` 只表示稳定、可加的基础 Cost，不混入同次 RTT、失败率和队列压力；
- Q0 不等待寻路；Q1 仍是固定深度 Latest Value；
- 不增加堆分配，不在正常业务帧增加 Cost/初始 Hop 字段；
- 不把 Host 模拟结果写成 ESP-NOW/CAN/UART/LoRa 实机性能。

## 7. V5-26 软件验收矩阵

| 任务 | 必测场景 |
| --- | --- |
| V5-22 | 路线持续不合格超过 Pending Timeout 不续命；新 Latest 覆盖后刷新；到期前恢复只发最新值一次；临时背压不提前清槽。 |
| V5-23 | W0 一跳 Cost=300；W0 四跳/W1 十六跳最大合法累计边界；Unknown/最大 Known/越界；四档 RREQ/RREP 新长度和旧长度拒绝。 |
| V5-24 | W3 Auto Source 经 W0-only Candidate 完成 Probe→ACK→Activate→ACK→Data；Profile 混入拒绝；W0 Epoch 回绕不产生 0。 |
| V5-25 | Build 16/Config 16 注入 W3 Hop=64 Data/RREQ 不改变状态；Build 16/Config 8 仍按 8 拒绝；RREQ 原始 Scope、RREP/Candidate/Path 超范围拒绝；Nano 同语义。 |

每项完成后执行 Windows Debug/Release Full/Lite、Nano、配置正负构建、最小 MTU、W0～W3 单档/混档 Scale Smoke 和 WSL ASan+UBSan。最后更新资源尺寸、任务表、操作记录、调用树、架构/使用文档和 UCN Obsidian 知识库。

## 8. 执行顺序

```text
V5-22 Q1 Freshness
→ V5-23 Cost Wire Domain
→ V5-24 Candidate Profile Continuity
→ V5-25 Ingress Hop Scope
→ V5-26 Cross-feature Regression
→ DOC-033 文档/知识库闭环
→ S02/V5-21
→ S06/S07 实机
```

## 9. 实际执行结果

V5-22～V5-26 已按本建议完成：

- Q1 内部 Retry 保留原绝对 Deadline；只有新的应用 Latest Value 才覆盖 Payload 并刷新 Deadline；
- Cost Width 已改为 `3/3/3/4 B`，RREQ/RREP 当前长度为 `10/11/12/14 B` 与 `10/11/11/12 B`；
- Candidate 保存实际 Wire Profile，Probe/Activate/ACK/Epoch 全程保持同一域，Storage Layout Version 升到 4；
- Full/Lite/Nano 在 Network 后、Security/状态前执行运行期 Hop Scope，路由学习和 Path Remaining Hops 同步限制；
- Windows Full/Lite Debug、Release、Service OFF 均为 CTest `10/10`，Nano 为 `1/1`；WSL Full ASan+UBSan 与配置契约为 `13/13`；
- GCC 14.2 Release/Service OFF 的 Nano/Lite/Full Node 为 `2648/5960/9744 B`，静态库 `.text` 为 `19724/67316/125448 B`，Link 仍为 40 B。

上述均为 Host 软件和静态资源证据。本轮没有访问硬件/COM；S02/V5-21、真实 MCU 栈/Heap/CPU/功耗以及 Wi-Fi/UART/CAN/LoRa 多板行为继续保持原门禁，不因本轮通过而改写。
