# UCN v5 代码缺陷审计报告

> 审计日期：2026-08-11
>
> 审计基线：`v4.0.0-final-before-v5` (`8a24e72a5475645ba5852abffd7caa44167f8103`)
>
> 审计对象：`codex/v5-adaptive-wire`，当前 `f941ae95545dd85c86bff75f618b41fad6db703c`
> 审计性质：只读检查；本文不包含协议源码、测试代码或构建配置修改。

> 后续状态（2026-08-11）：本文记录的是 `f941ae9` 基线缺陷；V5-27～V5-30 已完成代码与 Host 软件修复，V5-31～V5-33 又完成 PATH_INSTALL 兼容与非 Full API 发布修复。对上述修复实现的复审确认原 P1 已关闭，但发现 1 项公开 C 结构体的源兼容性 P2（见第 6 节）。当前结果见 [异构 Bearer、动态 MTU 与 Policy 修复报告](UCN_V5_27_异构Bearer与动态MTU修复报告.md)和 [PATH_INSTALL 兼容与 API 符号修复报告](UCN_V5_31_PATH_INSTALL兼容与API符号修复报告.md)。下文代码行号和“当前”均保留为初审发生时的证据，不再代表修复后的源码状态。

## 1. 结论

严格按 v4 基线之后的改动，确认 2 项待修复缺陷：

| 编号 | 严重度 | 范围 | 结论 |
| --- | --- | --- | --- |
| V5-AUD-01 | P1 | Adaptive Wire Profile + 多 Bearer/Path | 主备 Bearer 的 Wire Profile 或 MTU 不一致时，故障切换可能把端到端业务送入持续黑洞。 |
| V5-AUD-02 | P2 | Full/Nano 动态 MTU | `link->mtu == 0` 的动态 MTU 语义在选择、注册和发送检查之间矛盾，当前实际上不可用。 |

同时发现 1 项根因位于 v4 的存量 P1 问题。它不是本次 v5 新增行，但会直接破坏 v5 已承诺的主备 Bearer 连续性，因此应与上述问题一起关闭：

| 编号 | 严重度 | 范围 | 结论 |
| --- | --- | --- | --- |
| CUR-AUD-01 | P1 | Policy + 多 Bearer/Path | 质量刷新按原始物理 Link 将 Policy Path 标为 `DOWN`，即使逻辑邻居的备用 Bearer 仍可用；当前没有自动恢复迁移。 |

本轮没有发现由静态检查直接证明的越界、泄漏或 Sanitizer 报错；这不抵消以上协议状态机和能力协商缺陷。

## 2. 审计范围与方法

### 2.1 范围

- 比对 `v4.0.0-final-before-v5..f941ae9` 的代码改动；文档改动不计入缺陷归因。
- 重点审计 v5 新增的 Wire Profile、逐 Link 能力上限、自动选档、E2E 安全绑定，以及它们与既有 Path/Policy/多 Bearer 故障切换的交互。
- 额外检查了当前 v5 运行时路径，发现时明确区分“v5 引入”与“v4 存量”。

### 2.2 验证证据

| 检查 | 结果 | 说明 |
| --- | --- | --- |
| Full Debug 回归 | 10/10 通过 | 常规 Host 软件回归。 |
| WSL Full ASan/UBSan + 配置契约 | 13/13 通过 | 未报告 Sanitizer 错误。 |
| GCC `-fanalyzer` 构建 + 回归 | 13/13 通过 | 未报告编译器静态分析告警。 |
| `git diff --check` | 通过 | 基线后差异无尾随空白等格式错误。 |

这些检查均没有覆盖“主备能力不一致后转发”和“`link->mtu == 0` 动态协商”场景，不能据此认定对应逻辑正确。

## 3. 缺陷详情

### V5-AUD-01 — P1：多 Bearer 能力不一致时，Path 故障切换无法保证端到端可达

**v5 归因。** v5 引入逐 Link `peer_wire_profile`、自动选档及其在 Path 发送前的封装；已有 Path/Bearer 故障切换逻辑没有同步纳入所有候选 Bearer 的共同能力。

#### 代码证据

1. `prepare_outbound_wire_profile()` 只按照当前传入的单条 Link 限制或选择 `frame.wire_profile`；已确定的 Profile 超出该 Link 上限时直接返回 `UCN_ERR_UNSUPPORTED`。

   - `src/ucn_node.c:2213-2245`

2. `ucn_node_send_path()` 先通过 `resolve_egress_link()` 选择一个 Bearer、读取其状态并调用 `protect_outbound_business()`；随后又把同一个已封装的 `frame` 交给 `send_frame_on_path_egress()`。

   - `src/ucn_node.c:6923-6964`

3. `send_frame_on_path_egress()` 在物理链路报 `UCN_ERR_LINK_DOWN` 后会重新选择备用 Bearer，但重试的仍是同一 `frame`，而且仅把 `LINK_DOWN` 当作可重解析条件；`UNSUPPORTED`、`TOO_LARGE` 不会撤销 Path 或产生 RERR。

   - `src/ucn_node.c:6843-6889`

4. 对 E2E 帧，Version/Profile 字节属于认证 AAD。中继节点不能在不破坏认证的前提下把 W3 改写成 W0，也不能把超出备用 MTU 的帧重新压缩。

   - `src/ucn_frame.c:301-313`

#### 可复现场景

1. 建立 `A -> B -> C` 的 Path；B 到 C 的逻辑邻居有主备两条 Bearer。
2. 主 Bearer 的 `peer_wire_profile` 为 W3、MTU 足够；备用 Bearer 仅接受 W0 或 MTU 更小。
3. A 发送需要 W1/W2/W3 的受 E2E 保护 Path 业务帧；B 的主 Bearer 随后失效，系统切换至备用 Bearer。
4. B 转发原帧时，备用 Link 因 Profile/MTU 返回 `UCN_ERR_UNSUPPORTED` 或 `UCN_ERR_TOO_LARGE`。该结果不被转换为 Path 失效或 RERR，A 继续沿原 Path 发帧，业务持续不可达。

即使源节点在下一次本地发送前能重新选档，中继转发的入站 E2E 帧仍无法改写。因此这不是单纯的“重试一次”问题。

#### 现有测试缺口

`tests/test_path_control.c:749-773` 只验证主备切换后 Path ID 保持不变；测试链路没有设置不同的 `peer_wire_profile` 或 MTU，因而无法触发本问题。

#### 修复要求与验收

1. 为逻辑邻居或 Path 维护所有可用 Bearer 的**共同** Wire Profile 上限和最小可用 MTU；Path 业务封装必须使用该共同交集，而不是仅使用当前首选物理 Link。
2. 若现有入站 E2E 帧不满足已切换 Bearer 的能力，必须产生明确的 Path/RERR 或可诊断的重建信号，不能静默保留不可达 Path。
3. 新增端到端回归：主链 W3、备链 W0；另加主备 MTU 不同的组合。验证主链掉线后业务要么继续送达，要么源端收到确定的失效结果并完成重建，不能静默丢帧。
4. 覆盖源端发送和中继转发两条路径，并分别覆盖受 E2E 保护与透明转发的合法边界。

### V5-AUD-02 — P2：动态 MTU 的零值语义自相矛盾，Full 与 Nano 行为不一致

**v5 归因。** v5 自动选档在 Full 中明确写入“静态 MTU 为 0 时采用运行时状态 MTU”的逻辑，但注册和发送路径仍把 `0` 当作非法或零上限；Nano 还实现了不同的回退条件。

#### 代码证据

1. Full 的 `effective_link_mtu()` 把 `link->mtu == 0` 视为“由 `status->mtu` 提供上限”。

   - `src/ucn_node.c:2202-2210`

2. 同一 Full Profile 的注册函数却要求 `link->mtu` 至少等于帧头长度，因此 `0` 根本不能注册成功；发送检查也无条件执行 `encoded_length > link->mtu`。

   - `src/ucn_node.c:6071-6089`
   - `src/ucn_node.c:2286-2294`

3. Nano 的自动选档仅在 `status.mtu < link->mtu` 时才采用状态 MTU。`link->mtu == 0` 时，这个条件恒为假；随后编码长度仍会因 `encoded_length > 0` 被拒绝。

   - `src/ucn_node_nano.c:125-158`
   - `src/ucn_node_nano.c:465-481`

#### 影响

- 适配器若只能在 `get_status()` 时给出可用 MTU，无法使用动态 MTU 约束。
- Full 与 Nano 对相同 Link 配置得出不同的自动选档行为，破坏 Profile 间的可预测性。
- 代码中的“零值回退”分支成为无法通过公开注册流程到达的死语义，后续维护容易误判能力边界。

#### 修复要求与验收

需要先冻结公共契约，二选一并在 Full、Nano、头文件和文档中统一：

1. **支持动态 MTU：** 允许 `link->mtu == 0` 注册；仅当静态 MTU 非零时再参与最小值计算和发送长度检查；Nano 复用与 Full 相同的 `effective_link_mtu()` 语义。
2. **不支持动态 MTU：** 删除所有零值回退分支，文档明确 `link->mtu` 必填且为硬上限。

无论选择哪种契约，都应添加 Full/Nano 参数化测试：静态 MTU、仅状态 MTU、两者均存在且状态更小、状态为零四组，并验证 Profile 选择及实际发送结果一致。

### CUR-AUD-01 — P1：Policy 刷新把单个物理 Bearer 故障误判为整个 Path 故障

**v4 存量，当前 v5 仍受影响。** 该问题的根因在 v4 基线之前，但 v5 的“逻辑邻居多 Bearer 故障切换”要求与它相冲突，因此必须纳入当前发布门禁。

#### 代码证据

1. Path 发送层明确要求：路由保留学习时的 Link，但发送时应解析健康备用 Bearer；只有整个 Bearer 集不可用时才撤销 Path。

   - `src/ucn_node.c:920-938`

2. Protocol Task 每轮调用 `ucn_policy_refresh_link_quality()`；该函数却以 `path->egress_link`（原始物理 Link）查询质量快照，并在其 `is_up == false` 时无条件写入 `UCN_POLICY_PATH_DOWN`。

   - `src/ucn_node.c:7913-7917`
   - `src/ucn_policy.c:314-334`

3. Policy Path 一旦为 `DOWN`，发送函数在进入 Path/Bearer 解析前立即返回 `UCN_ERR_LINK_DOWN`。当前源码中没有由健康备用 Bearer 把该状态自动恢复为 `VERIFIED` 的迁移。

   - `src/ucn_node.c:7124-7147`

#### 可复现场景

1. 配置一个 `PINNED_STRICT` Policy Path，`path->egress_link` 为主 Bearer，同时为同一邻居配置健康备用 Bearer。
2. 使主 Bearer `is_up=false`，保持备用 Bearer可用。
3. 调用足以触发质量刷新周期的 `ucn_node_step()`。
4. Path 被标为 `DOWN`；后续 Policy 发送在 `ucn_node_send_path()` 前就失败，无法到达已存在的 `resolve_egress_link()` 备链逻辑。

#### 现有测试缺口

`tests/test_path_control.c:749-773` 在 A-B 主 Bearer 掉线后立即发送，验证的是即时 Bearer 解析；该段没有对 A 运行质量刷新。测试在 B-D 主链掉线后运行了 B 的 `path_step()`，但 B 没有对应的本地 Policy Path，因此同样未覆盖 Policy Path 被置 `DOWN` 的路径。

#### 修复要求与验收

1. Policy 的活性判断必须针对逻辑邻居/Bearer 集，而不是仅针对 `path->egress_link` 指针。
2. 只有全部候选 Bearer 不可用时才能把 Policy Path 置 `DOWN`；主备切换时保留 `VERIFIED` 或进入可恢复的过渡状态。
3. 若保留 `DOWN` 状态，必须定义并实现从健康备用 Bearer 恢复的状态迁移和诊断事件。
4. 新增 `PINNED_STRICT` 回归：主 Bearer 掉线后执行足够的 `ucn_node_step()`，确认经备用 Bearer 仍可发送；再使全部 Bearer 掉线，确认 Path 才转为 `DOWN`。

## 4. 修复优先级与发布建议

1. **先关闭 V5-AUD-01。** 它会在“主备链路能力不同”这一 v5 支持的配置下形成业务黑洞，且安全保护禁止中继节点事后改写 Profile。
2. **同时关闭 CUR-AUD-01。** 否则一次正常主备切换会在质量刷新后反向使严格策略流永久失败。
3. **再冻结并实现 V5-AUD-02 的 MTU 契约。** 该项影响适配器集成和 Full/Nano 一致性，应在任何动态 Carrier 宣称支持前关闭。
4. 在三项回归均加入前，不应把“不同能力的主备 Bearer”或“动态 MTU Carrier”写成已通过发布门禁的能力。

## 5. 非结论与边界

- 本报告不把 Host CTest、ASan 或编译器静态分析通过表述为目标 MCU、多板、多介质、长稳、吞吐、功耗或生产密码学验证。
- 本报告未修改用户已有的未提交文档；仅新增本文。
- P1/P2 是按业务连续性、错误可见性和协议契约一致性分级，不等同于 CVSS 安全漏洞评分。

## 6. 2026-08-11 V5-31 后复审（当前未提交工作树）

### 6.1 原问题关闭结论

| 原编号 | 复审结果 | 当前证据 |
| --- | --- | --- |
| V5-AUD-01 | 已关闭 | Path 安装能力被限制为沿途 Bearer 交集；主备异构 Profile/MTU 与确定性 RERR 回归已加入。 |
| V5-AUD-02 | 已关闭 | Full/Nano 统一使用动态状态 MTU，注册、自动选档与实际发送的零值语义已对齐。 |
| CUR-AUD-01 | 已关闭 | Policy 刷新按逻辑邻居候选 Bearer 判断活性；健康备用 Bearer 不再把严格策略 Path 错标为 `DOWN`。 |
| V5-31 初审 P1（PATH_INSTALL 载荷兼容） | 已关闭 | 默认 API 固定发送 v5 基础 Schema；接收端严格接受基础或扩展两种合法长度；扩展格式仅由显式 capability API 发出。 |
| V5-31 初审 P1（Lite/Nano API 符号） | 已关闭 | Lite/Nano 已提供 capability API Stub；真实外部调用对象可编译、链接并返回明确的 `UCN_ERR_CONFIG`。 |

当前未发现新的 P1，也未发现由 ASan/UBSan 或 GCC `-fanalyzer` 直接证明的越界、泄漏、未定义行为或静态分析告警。

### 6.2 V5-REAUD-01 — P2：公开 `ucn_path_forward_config_t` 在中间插入字段，破坏旧调用方的按位置聚合初始化

**v5 归因。** `maximum_wire_profile` 与 `minimum_mtu` 被插入 `remaining_hops` 和既有 `egress_link` 之间，而不是追加到结构体末尾。

#### 代码证据

`include/ucn/ucn_path.h:35-48` 的当前字段顺序为：

```c
uint8_t remaining_hops;
uint8_t maximum_wire_profile;  /* v5 新字段 */
uint16_t minimum_mtu;          /* v5 新字段 */
ucn_link_t *egress_link;       /* 旧字段被后移 */
uint32_t expires_at_ms;
```

旧版应用中常见的八项 C 聚合初始化：

```c
ucn_path_forward_config_t cfg = {
    owner, session, path, destination, next_hop, hops, egress_link, expires_at_ms
};
```

在当前头文件下，`egress_link` 会被赋给 `uint8_t maximum_wire_profile`，`expires_at_ms` 会被赋给 `minimum_mtu`，真正的 `egress_link` 与过期时间则未初始化。使用 Windows x64 GCC 14.2、`-std=c99 -Wall -Wextra -Werror` 对该旧式初始化作独立语法检查，已稳定复现“指针初始化 unsigned char”及“缺少 egress_link 初始化”错误。

#### 影响

- 旧应用启用常见告警即报错，无法随库头文件直接升级；未启用告警时则存在指针截断、空出口 Link 和错误租期的运行时风险。
- 该问题不影响仓库内已改为指定初始化或已同步字段顺序的测试，因而现有 CTest 不会暴露第三方调用方的兼容性回归。

#### 修复要求与验收

1. 若承诺 v5 对既有 C 源码保持兼容，将两个新字段移至 `expires_at_ms` 后面；零值继续表示“不额外限制”，即可保持旧位置初始化的前八项含义不变。
2. 若有 ABI 版本策略而不能调整字段顺序，新增独立的 v5 配置结构或构造函数，并在发布说明中明确 `ucn_path_forward_config_t` 的源/ABI 破坏性变更。
3. 增加一个外部风格编译门禁：使用旧八项位置初始化编译一次，并使用新 capability 配置验证限制生效；该门禁应在 Full、Lite、Nano 头文件可见性检查中运行。

### 6.3 本次复审验证

| 检查 | 结果 |
| --- | --- |
| Windows Full / Lite / Nano 回归 | `13/13`、`13/13`、`4/4` 通过 |
| Lite / Nano capability API 外部链接 | 通过 |
| WSL Full ASan + UBSan | `13/13` 通过，无 Sanitizer 报错 |
| WSL Full GCC `-fanalyzer` | `13/13` 通过，无分析告警 |
| `git diff --check -- include src tests docs` | 通过（仅 CRLF 提示，无 diff 格式错误） |

以上均为 Host 软件证据；仍不等价于多板互操作、断链时延、吞吐、栈、功耗或生产密码学验证。
