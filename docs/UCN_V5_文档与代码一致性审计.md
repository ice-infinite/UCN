# UCN v5 文档与代码一致性审计

> 核对日期：2026-08-11
> 初次核对基线：`codex/v5-adaptive-wire@f941ae9`
> 后续状态：V5-27～V5-33 已完成并纳入当前 `codex/v5-adaptive-wire` 分支；本文“当前代码权威事实”和资源表已同步到该实现，初次发现/文档计数仍保留为 `f941ae9` 阶段记录。增量见 [异构 Bearer、动态 MTU 与 Policy 修复报告](UCN_V5_27_异构Bearer与动态MTU修复报告.md)及 [PATH_INSTALL 兼容与 API 符号修复报告](UCN_V5_31_PATH_INSTALL兼容与API符号修复报告.md)。
> 核对原则：源码、公共头与正式测试是当前事实；历史实现报告保留当时证据，但必须明确指出后续替代项。

## 1. 结论

本轮核对了 `docs/` 下全部 16 份 v5 专题 Markdown，并交叉检查 README、整体架构、协议分层、网络容量、使用手册、全局配置、Link Metrics/Cost 契约、任务表和调用树。

发现并修正的主要偏差是：

1. V5-05 仍把 Auto 限定为普通业务，未反映 V5-12～V5-15 的控制面选档/继承；
2. V5-05 仍称固定 Path 没有端到端 Hop 元数据，未反映 V5-18 的 `remaining_hops`；
3. Adaptive Wire 总方案把权限写成 `Authorized Max Wire Profile`，与 V5-16 的 C0～C3 认证身份授权模型冲突；
4. Adaptive Wire 总方案写了 W0 Epoch “串行大小比较”，而代码实际是 Active/Previous 精确值 + grace，不做 Epoch 大小排序；
5. 多份 V5-01～V5-10 报告把阶段性测试数量、资源值和“下一项”写成当前状态；
6. 协议分层、容量总览和使用手册的状态头仍停留在 V5-09/V5-10；
7. Link Metrics 文档误写“v4 当前”，实际实现是当前 v5。
8. 快速手册总入口、调用树入口和 S08 历史报告的“当前版本/当前资源入口”仍分别停在 V5-07、V5-20 和 V5-01。

这些偏差已经更新。历史 CSV、当时测试数量和当时资源表没有被重写，只增加明确的历史/当前分界。

## 2. 当前代码权威事实

| 项目 | 当前源码事实 | 代码入口 |
| --- | --- | --- |
| 项目/协议版本 | CMake `5.0.0`；Wire Protocol `5`，Version 使用低 6 bit | `CMakeLists.txt`、`ucn_config.h`、`ucn_frame.c` |
| Wire Profile | W0/W1/W2/W3 官方固定描述符，不能自定义位宽 | `ucn_frame.c::UCN_WIRE_PROFILES` |
| 基础/Route/Path 头 | `17/21/26/30 B`、`18/23/28/32 B`、`19/25/31/36 B` | `ucn_frame_header_size_for_profile()` |
| Address/Length/Epoch/Path 宽度 | W0=`1/1/1/1`，W1=`2/1/2/2`，W2=`3/2/2/3`，W3=`4/2/2/4` B | `ucn_wire_profile_descriptor_t` |
| Wire Cost 宽度 | W0/W1/W2/W3=`3/3/3/4 B`；全 1 为 Unknown | Descriptor 与 RREQ/RREP Codec |
| Profile 最大 Hop | `4/16/64/254`，但默认 Build/Node 运行上限是 16，可继续收窄 | Descriptor、`UCN_MAX_HOPS`、`default_hop_limit` |
| Decoder | Nano/Lite/Full 都解析 W0～W3 基础帧；Feature 不存在时返回 Config | `ucn_frame_decode()`、Nano/Full Receive、Profile Stubs |
| RX 顺序 | 3 B Prefix Peek → Node/per-Link Ceiling →完整 Decode/CRC → Network →运行期 Hop Scope →安全/状态机 | `ucn_node_receive()`、Nano Receive |
| 自动发现 | Auto 模式默认 2→4→8→16 Ring，250 ms/轮、1000 ms 总预算，每轮新 Request ID/重新选档 | Discovery 状态与 `test_aodv_lite.c` |
| Candidate | 保存实际 Wire Profile；Probe/ACK/Activate/Epoch 保持同一 Profile | `ucn_candidate_route_t`、Candidate handlers |
| Q1 Pending | 深度默认 4、绝对 Deadline 默认 1000 ms；内部重试不续期，新应用 Latest 才刷新 | Pending Q1 helpers、`test_endpoint.c` |
| Storage ABI | `UCN_NODE_STORAGE_LAYOUT_VERSION=5` | `ucn_node_storage.h`；V5-28 Path 能力字段 |
| 安全 | AAD/透明密文与 Provider 门禁存在；生产身份、AEAD、逐跳认证、Authorized Class 执行层未完成 | Security Provider API、S02/V5-21 |
| Gateway | 只有 V5-06 设计，没有 `ucn_gateway_ext` 代码 | V5-06 设计评审 |

## 3. 当前控制载荷口径

设 `A=AddressWidth`、`C=CostWidth`、`E=EpochWidth`、`P=PathWidth`、`S=SessionWidth`。

| 类型 | 当前 Payload | W0/W1/W2/W3 长度 |
| --- | --- | --- |
| HELLO | RX Ceiling(1) | 1/1/1/1 B |
| HEARTBEAT/ACK | 固定控制 ID/时间域 | 8/8/8/8 B |
| RREQ | Target(A)+RequestID(4)+Cost(C)+Hop(1)+Flags(1) | 10/11/12/14 B |
| RREP | RequestID(4)+Cost(C)+Hop(1)+Flags(1)+Epoch(E) | 10/11/11/12 B |
| 普通 RERR | Unreachable(A) | 1/2/3/4 B |
| Path RERR | Unreachable(A)+OwnerSession(S)+PathID(P) | 3/6/9/12 B |
| PATH_INSTALL 基础/扩展 | 基础为 PathID(P)+Destination(A)+NextHop(A)+Lease(4)+RemainingHops(1)；扩展再加 MaximumWireProfile(1)+MinimumMTU(2) | 基础 8/11/14/17 B；扩展 11/14/17/20 B |
| PATH_REVOKE | PathID(P)+Destination(A) | 2/4/6/8 B |
| Path Trace | 固定 8 B + N×NodeID(A) | 请求含 1 个节点时 9/10/11/12 B |
| Node Snapshot Request/Reply | 固定 8 B / 8 B+NodeID(A) | 8 B / 9/10/11/12 B |
| Candidate Probe/Activate | 固定状态机 Schema | Probe/ACK 12 B；Activate/ACK 6 B |
| Policy Diagnostic | 固定分页 Schema | Request 8 B；Reply 32 B |

## 4. 16 份 v5 专题文档核对结果

| 文档 | 定位 | 本轮处理 |
| --- | --- | --- |
| V5-01 Codec 报告 | 历史实现报告 | 保留 2/2 与早期资源；补充当前资源、后续能力与默认 W3 语义。 |
| V5-02 Node 固定域报告 | 固定模式报告 | 修正 RX 顺序；区分固定 TX 与 Auto 控制面；补当前回归。 |
| V5-03 控制帧压缩报告 | 历史迁移报告 | 继续保留旧 RREQ 长度，同时明确 V5-23 当前格式和当前回归。 |
| V5-04 安全绑定报告 | 当前契约 + 历史测试 | 确认 AAD/透明中继未变；强调仍非生产密码。 |
| V5-05 自动选档报告 | 当前使用说明 | 修正控制面 Auto、Expanding Ring、Path Remaining Hops、Candidate Profile。 |
| V5-06 Gateway 评审 | 纯设计 | 明确当前仓库没有 Gateway 实现，不把设计写成能力。 |
| V5-07 发布报告 | 历史门禁报告 | 保留阶段测试/资源；更新当前任务范围和剩余门禁。 |
| V5-08 全档接收报告 | 当前互操作契约 | 保持统一 Decoder；补 Ingress/Hop 与当前回归。 |
| V5-10 极限模拟报告 | 历史 CSV/规模证据 | 不改实验数据；把 9400/5888 标为历史并补当前资源。 |
| V5-11 审计执行方案 | 历史决策 + 执行核对 | 补充 V5-17～V5-33 当前状态，保留基线证据。 |
| V5-14/V5-23 Cost 报告 | 当前 Wire 规范 | 已与 Descriptor、Codec、边界测试一致。 |
| V5-15 控制载荷报告 | 当前 Wire 规范 | 已与控制载荷 helper/测试一致；补全部 Profile 当前资源。 |
| V5-16 Authorized Class | 设计冻结 | 与源码一致：没有 C0～C3 执行层，继续阻塞 S02。 |
| V5 审计遗留修复方案 | 历史方案 + V5-17～20 结果 | 标注历史资源，并把 V5-26、V5-30、V5-33 阶段值与 V5-44/V5-36 当前值分栏。 |
| V5 最新审计修复建议 | 历史问题 + V5-22～26 结果 | 顶部明确任务已在 `f941ae9` 完成，并链接 V5-30/V5-33 后续报告。 |
| v5 Adaptive Wire 总方案 | 当前总设计 | 修正任务状态、权限正交、Epoch 语义和 Expanding Ring。 |
| V5-27 异构 Bearer/动态 MTU 报告 | 当前增量规范 | 记录动态 MTU、Path 能力、确定性 RERR、逻辑 Bearer Policy、资源和验证。 |

V5-09、V5-12、V5-13、V5-17～V5-26 没有全部各建一份独立编号报告；其实现由任务表、V5-11/V5-14/V5-15、两份审计修复文档、总架构、使用手册和正式测试共同记录。V5-27～V5-30 已由独立增量报告统一承接。这不是代码缺失，但后续若继续新增破坏性 Wire 修改，应优先建立单独当前规范而不是只追加历史报告。

## 5. 当前资源与软件证据

Windows x64 GCC 14.2、Release、Service OFF：

| Build Profile | `sizeof(ucn_node_t)` | `sizeof(ucn_link_t)` | Archive `.text` |
| --- | ---: | ---: | ---: |
| Nano | 2,648 B | 40 B | 27,662 B |
| Lite | 6,024 B | 40 B | 73,735 B |
| Full | 10,080 B | 40 B | 139,017 B |

V5-44/V5-36 当前软件回归口径：Windows Full/Lite/Nano 为 `11/11、11/11、1/1`，Full Service OFF `11/11`，产品头 `15/15`；WSL Full ASan+UBSan 与 GCC `-fanalyzer` 均 `11/11`。Storage Layout Version=5。这些是 Host 软件和 ABI 证据，不是目标 MCU Flash/RAM/栈/CPU/功耗或 Wi-Fi/UART/CAN/LoRa 性能。

## 6. 明确保留的未完成边界

- S02：生产身份、审计 AEAD、密钥/Session Generation、持久 Replay、逐跳控制认证；
- V5-21：认证身份上的 Authorized Class C0～C3、ACL、预算、Fanout 与撤销；
- V5-06：跨 Wire Domain 的 Alias/Directory/Session Slot/Gateway；
- S06/S07：目标 MCU 资源、真实多板、多介质切换、吞吐、时延、功耗和长稳；
- 小 MTU Carrier：经典 CAN 等低于最小 UCN 帧的有界分段/重组。

以上保持不改，不能因为 Host CTest 通过而写成已经实现。

## 7. 反向校验结果

- v5 文档：原有 16 份专题全部逐份核对；加上本文后共 17 份当前 v5 文档。
- 标识符：提取 50 个 `UCN_*`/`ucn_*` 名称，代码中没有意外缺失；仅 `UCN_ERR_SCOPE` 和 `ucn_gateway_ext` 是文档明确说明“当前不存在”的名称。
- 项目链接：README 与 `docs/` 共 59 份 Markdown 的相对链接无缺失。
- 调用树：10 个模块、128 个节点、189 个调用引用；重复 ID、内部缺失目标和真实源码坏路径均为 0。
- 知识库：UCN 58 份笔记的 WikiLink 无缺失；当前状态、测试证据、路线图、任务表和操作记录已同步。
- 软件回归：V5-44/V5-36 当前 Windows Full/Lite/Nano 为 `11/11、11/11、1/1`，Full Service OFF `11/11`，产品头 `15/15`；WSL Full ASan+UBSan 与 GCC `-fanalyzer` 均 `11/11`。
- 文本检查：`git diff --check` 和本文新增文件的尾随空白检查均通过。

本轮只更新文档和知识库，没有修改协议源码、构建配置或测试代码，也没有上传、烧录或访问硬件/COM。
