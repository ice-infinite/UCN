# UCN V5-11 审计问题确认与执行方案

> 日期：2026-08-11
> 基线：`codex/v5-adaptive-wire@fe3cb0d`
> 方法：逐项对照当前本地源码、测试、任务表和知识库；外部审计意见只作为线索，不直接作为事实。

## 1. 总结

v5 的可变 Wire Header、失败关闭、统一内部 32 bit 身份和低 TX/宽 RX 方向成立，不需要推翻。审计指出的问题分为四类：

1. 已确认且需要立即修复的状态一致性/解析问题；
2. 已确认、需要成组修改线格式和路由状态的协议升级；
3. 不是当前契约错误，但值得增加的跨介质能力；
4. 已知并有意延期到 Extended、生产安全或实机阶段的边界。

## 2. 逐项确认

| 审计项 | 源码结论 | 是否更新 | 处理决定 |
| --- | --- | --- | --- |
| 自动选档没有覆盖 HELLO/RREQ/RREP 等控制面 | 属实。`send_control_on_link()` 与 `begin_route_discovery()` 当前写死 `node->tx_wire_profile`。 | 是，P1 | V5-12 让普通控制帧走最小可表达档，并给路由发现建立显式 Profile/Hop 选择；完整覆盖 HELLO→RREQ→RREP→Data。 |
| Path 安装可写入当前 TX 域不可表达的 Path ID/地址 | 属实。Local Install 只做 Security/Path 表校验，远端 Payload 仍是 32 bit 固定字段。 | 是，P1 | V5-11 在状态写入前按有效 Wire 能力校验 Path ID、Destination、Next Hop；非法远端安装不消费表项、不污染 Path 状态。 |
| W2/W3 长跳数与 16 bit 累计 Route Cost 不匹配 | 属实。Link Cost 与累计 Route Cost 都是 `uint16_t`，累加在 65534 饱和。 | 是，P1 | V5-14 保留单跳 Link Cost 的有界输入，累计 Route Cost 升级为 32 bit；RREQ/RREP 按 W0/W1/W2/W3 使用 1/2/3/4 B Cost。 |
| Node 只有全局 RX Profile，不能给不同 Link 独立收窄 | 能力缺口属实，但不是当前契约错误。仓库把 `link->mtu` 定义为 Adapter 分段/重组后的 UCN 逻辑 MTU。 | 是，P1 能力 | V5-13 增加每 Link 本地 RX Ceiling；Ingress 上限取 Node 与 Link 较小值。默认继承 Node 上限，不破坏现有 Adapter。 |
| Wire Profile 尚未成为权限/限流等级 | 属实，但不是 Bug。当前 Wire Profile 只表示编码能力，不能由帧自报档位获得权限。 | 设计更新，不在本轮直接赋权 | V5-16 单独定义认证身份上的 Authorized Class；在 S02 身份/密钥没有完成前，不把它伪装成已实现安全功能。 |
| Linux 不能统一访问多个 W0 扁平域 | 属实且已知。V5-10 已明确 W0/mixed 单域 254 Node。 | 不改小 Core | 继续归 V5-06 `ucn_gateway_ext`；等待 S02 的身份、Session 与安全信封前置条件。 |
| RREP 保留 Origin/Target，且未检查 Origin==Header Destination | 属实。当前只检查 Payload Target 与 Header Source。 | 是，P1/P2 | V5-11 先失败关闭；V5-14 删除重复 Origin/Target，并按 Profile 编码 Cost/Epoch。 |
| RERR/PATH_INSTALL/TRACE/SNAPSHOT 等控制 Payload 未全部 Profile-aware | 属实，但 V5-03 原本只承诺 HELLO/RREQ。 | 是，P2 | V5-15 统一控制载荷字段 Codec，逐类改协议并保留坏长度/旧格式拒绝测试；不与 V5-11 紧急修复混成一次不可审计改动。 |
| W0-only Build 仍编译四档 Decoder、静态 Buffer 未降至极限 | 属实但符合 V5-08 已冻结目标。 | 当前不改 | 用户目标是所有 Node 能解析所有档位。继续用统一 Decoder；目标 MCU 资源不足时再新增显式产品变体，不能暗中改变默认互操作。 |
| Protocol Version 缺 6 bit 编译期断言 | 属实。Encoder 会掩码而 Decoder 比较完整宏。 | 是，P1 小修 | V5-11 增加 `UCN_PROTOCOL_VERSION<=0x3F` 编译门禁和 64 负向构建。 |
| 生产安全仍是发布 P0 | 属实，任务表已保留。 | 需要，但不由本轮虚拟测试冒充完成 | S02 继续负责真实身份、审计 AEAD、密钥、Session Slot/轮换和持久 Replay；V5 Wire 测试通过不改变该状态。 |

## 3. 实施顺序

```text
V5-11 解析/状态硬门禁
  → V5-12 自动控制面选档
  → V5-13 per-Link 本地 RX Ceiling
  → V5-14 32 bit 累计 Cost + Profile-aware RREQ/RREP
  → V5-15 其余控制 Payload Codec
  → V5-16 Authorized Class 设计（依赖 S02）
```

V5-12 与 V5-14 分开实现：先证明窄 RX 节点能完成控制面互操作，再修改 RREQ/RREP Payload。V5-14 完成后重放同一完整链路，避免同时改变选档、字段偏移和路由度量却无法定位回归。

## 4. 必须新增的测试

1. W3 TX Maximum + Auto 到 W0 RX 节点，完整完成 HELLO、RREQ、RREP、Endpoint Data；实际线上帧档逐步断言。
2. W0 Fixed 安装 Path ID 256、越界 Destination/Next Hop，在写表前失败。
3. 远端 PATH_INSTALL 越界，拒绝且 Path 表/统计无错误增长。
4. 同 Node 两条 Link 分别设置本地 RX W3/W0；W0 Link 拒绝 W1，W3 Link 接受 W3。
5. W3 长链累计 Cost 超过 65534 后仍能比较两条不同总 Cost；W0/W1/W2/W3 Cost 编解码边界失败关闭。
6. RREP 旧格式不一致先拒绝；新压缩格式删除冗余 Origin/Target 后验证 Source/Destination 语义。
7. `UCN_PROTOCOL_VERSION=64` 编译失败。
8. 三 Build Profile、严格 Release、Full ASan+UBSan、单档/混档规模 Smoke 重新通过。

## 5. 不改变的边界

- Linux 仍只是可选 Host/Adapter，不是路由中心。
- Core 继续固定容量、无动态内存、无无界 Flood/Retry。
- Wire Profile 表示编码能力；Security Authorized Class 表示权限，二者不混用。
- W0/mixed 的 254 Node 是扁平域边界；跨域必须经过 Extended Gateway。
- Host 虚拟长链与 Sanitizer 不能替代 ESP32/STM32、Wi-Fi/CAN/UART 实机结果。

## 6. 执行后反向核对（2026-08-11）

| 原审计项 | 最终状态 | 证据入口 |
| --- | --- | --- |
| 控制面自动选档 | 已修复 | V5-12：W3 Auto→W0 的 HELLO→RREQ→RREP→Data 全链。 |
| Path 表达性与 Version 6 bit | 已修复 | V5-11：本地/远端越界不写表；Version=64 编译拒绝。 |
| per-Link 本地 RX | 已修复 | V5-13：同一 Node 的 W0/W3 Link 独立接收上限和 HELLO 广告。 |
| 16 bit 累计 Cost、RREP 冗余 | 已修复 | [V5-14 报告](UCN_V5_14_长距离Cost与RREQ_RREP实现报告.md)：32 bit 累计、四档 Cost、200 Edge、RREQ/RREP 新格式。 |
| 其余 Node/Path 控制 Payload | 已修复 | [V5-15 报告](UCN_V5_15_Profile感知控制载荷实现报告.md)：RERR、Path Control、Trace、Snapshot。 |
| Lite 旧 RREP 最小 MTU 残留 | 审计实施中发现并修复 | Lite 46 B 构建通过、45 B 拒绝；裁剪 helper 无未使用警告。 |
| Wire Profile 误作权限 | 设计完成，未伪装实现 | [V5-16 设计](UCN_V5_16_Authorized_Class与控制预算设计.md)；执行层等待 S02。 |
| W0 跨域 Gateway、生产安全、实机 | 有意不在小 Core 本轮修改 | 分别继续归 V5-06、S02、S06/S07。 |
| W0-only Decoder 裁剪 | 不采用 | 保持 V5-08“所有 Build Profile 可解析 W0～W3”的用户目标。 |

最终软件门禁：Windows Debug/Release Full/Lite `10/10`、Nano `1/1`，配置契约 `4/4`，WSL ASan+UBSan `13/13`；调用树、项目 Markdown、Obsidian 内链与 `git diff --check` 均通过。该结论不包含提交/推送或实机验证。
