# UCN v6 外审 Lease、Quorum、缓冲区与发布面整改报告

> 日期：2026-09-06
> 审计输入基线：`v6-development@af6d85672836a14ca1c70b516a5cf263aebd8b8c` 之后的未提交工作树
> 状态：`IMPLEMENTED / SELF-REVIEW PASS / EXTERNAL RE-REVIEW PENDING`
> 范围：外审指出的 4 项 P0、2 项 P1、2 项 P2，以及直接相关的确定性回归和发布门禁
> 非证明范围：真实 Flash/掉电、生产密码 Provider、MCU ISR/DMA、硬件时间戳、物理多跳、性能/功耗和长稳

## 1. 结论

本轮已关闭外审给出的八项问题。软件实现不再接受未来才开始的 Authority Challenge，JOIN/REAUTH
不能用调用方伪造的本地起点延长租约，Cluster 切换不能使用已过期成员的历史 Vote，Handover 也不能
让未证明具有当前 Authority 的目标永久 Fence 旧 Head。Security/Wire 输入输出重叠、旧 v5 用户入口、
空安装测试和 Feature-off 头文件暴露同时完成整改。

当前结论只到“自审通过、等待外部复审”。它不等于统一外审 GO，也不解除 RC 的硬件和生产安全门禁。

## 2. 问题与整改映射

| 编号 | 原问题 | 最终合同 | 关键回归 |
| --- | --- | --- | --- |
| V6X-S19 / P0 | Authority 可接受未来 Challenge 起点 | 起点由 Bootstrap Owner 以可信 `now_us` 捕获；安装必须满足 `start <= trusted_now < deadline` | 未来起点、已过期和无写回 |
| V6X-S20 / P0 | 延迟 JOIN/REAUTH 可重置 Lease 起点 | Commit 只能消费对应 Bootstrap Owner 的 `FINAL_DURABLE` pending；调用方不再提供起点 | 同 Proof 延迟提交、缺 pending、有 Receipt 旁路 |
| V6X-S21 / P0 | Takeover/Recovery 使用过期 Vote | Commit 按当前成员 Lease、Capability 与精确 Config 重算 quorum；过期位 durable 剪除 | 到期后拒绝、重新准入仍拒绝、重新投票后通过 |
| V6X-S22 / P0 | 未证明的目标可使旧 Head Fence | READY/Commit 两次验证目标 Authority Proof 与当前 Epoch/Config/quorum/Deadline | 非成员、非 Voter、过期、错绑、合法目标 |
| V6X-S23 / P1 | Security/Wire 部分重叠导致 UB 或破坏输出 | 所有输入、输出、工作区、Frame 和结果对象必须完全不重叠 | `payload+1`、encoded/plaintext、decode、AAD、完整哨兵不变 |
| V6X-S24 / P1 | 当前文档仍暴露 v5 API | 旧入口全部带 `ARCHIVED / NOT CURRENT` 前置围栏，当前导航指向 v6 | 文档门禁逐文件检查前 8 行 |
| V6X-S25 / P2 | 安装 consumer 门禁未运行程序 | 内层工程注册 test；外层配置、构建并检查真实 CTest 结果 | GCC 与 MSVC Release 安装执行 |
| V6X-S26 / P2 | Feature-off 包仍安装可选头 | CMake 按 Feature 安装头和 adapter 子目录 | All-Off 和三个 single-feature 安装矩阵 |

## 3. Lease 所有权与时间基准

### 3.1 Bootstrap 捕获起点

`ucn_v6_bootstrap_open_after_cookie()` 在创建 pending 时记录 `challenge_started_local_us`。该字段属于
Bootstrap Owner，后续 Transcript/Final 状态转换只复制它，业务调用方没有修改入口。这样 Authority
Lease 的本地计时起点与真实 Challenge 生命周期一致，而不是由 Commit 的到达时刻重新定义。

### 3.2 Authority 安装

`ucn_v6_identity_authority_install_epoch()` 新增可信 `trusted_now_us`。函数在写 Authority 状态前验证：

1. `challenge_started_local_us <= trusted_now_us`；
2. 起点和认证剩余时长能生成非零、无溢出的本地 Deadline；
3. `trusted_now_us` 仍位于半开区间 `[start, deadline)`；
4. Proof 的 Authority/Realm/Generation/Fence 等原有 canonical 绑定继续成立。

因此“未来起点”不会在当前时刻提前授予写权限。

### 3.3 JOIN/REAUTH Commit

Security Commit 已删除公开结构中的 `authority_challenge_started_local_us`。Commit 必须找到同一 Bootstrap
Owner/Key 下的 `FINAL_DURABLE` pending，并从中读取捕获起点。Durable Receipt 只用于补强已经存在的
Bootstrap 终态，不能在 pending 缺失时单独构造 Lease。相同 Final 延迟到达时会自然减少剩余租期；过期
后失败关闭，不能把起点平移到“当前时间”。

## 4. Cluster 当前性证明

### 4.1 Takeover 与 Recovery

Vote 收到时的验证不足以代表 Commit 时仍有效。Commit 现在重新计算当前 live Voter bitmap，逐成员检查：

- 成员仍处于当前 Config；
- Lease 在 `now_us` 尚未到期；
- Cluster Capability 仍满足；
- Stable/Joint Config 的精确 quorum 规则仍成立。

历史 bitmap 中不再 live 的位会先通过持久化路径剪除，随后本次 Commit 返回拒绝。成员重新准入不会恢复
旧 Vote；只有在当前 Epoch/Config 下重新投票才可重新计入 quorum。

### 4.2 Handover READY 与 Commit

Handover Transition 固定携带目标 Authority Proof reference。READY 时必须解析到目标 Config 中当前 live、
具备 Cluster Capability 的精确目标 Voter，并验证其 trusted Authority Proof 绑定目标 Epoch、Config、
Identity、Stable quorum、Digest 与 Deadline。Commit 再执行同一当前性验证；Proof 在两步之间到期或父状态
变化都会拒绝，旧 Head 不会被永久 Fence。

## 5. 缓冲区不重叠合同

Security 的 protect/open/relay、Peer Discovery 和 Group HELLO，以及 Wire 的 size/encode/decode/AAD
接口，均在任何 sequence reserve、密码回调和输出写入前执行 overflow-safe 区间重叠检查。合同不仅拒绝
完全别名，也拒绝 `output = input + 1` 一类部分重叠。

失败路径逐字节比较完整 Frame、结果结构和哨兵缓冲区，防止“只检查首尾字节”掩盖中间写入；相关 API
文档也明确要求调用方提供完全分离的对象和缓冲区。

## 6. 用户文档与安装发布面

`docs/01-入门与使用` 属于保留的 v5 历史树，不再作为当前手册。总入口和各快速手册在前 8 行内声明
`ARCHIVED / NOT CURRENT`，并导航到 `docs/用户手册`、`docs/official` 和机械 API 索引。文档门禁会
遍历旧树全部 Markdown，缺少围栏即失败。

CMake 安装面不再无条件复制整个 `include/ucn/v6`：基础头始终安装，Realtime、Cluster、Adapter
及其 Bearer/Port 子目录只在对应 Feature 开启时安装。安装 consumer 内层项目注册并执行真实程序，
外层脚本检查配置、构建和 CTest 返回值；MSVC 多配置通过显式 `--config/-C` 保持一致。

## 7. 验证结果

| 环境 | 配置 | 结果 |
| --- | --- | --- |
| Windows GCC Release | Full | 26/26 |
| Windows GCC Release | Nano、全部 Optional Feature OFF | 21/21 |
| Windows GCC Release | Full、Realtime-only | 22/22 |
| Windows GCC Release | Full、Cluster-only | 24/24 |
| Windows GCC Release | Full、Adapter-only | 22/22 |
| Windows MSVC Release | Full，多配置安装门禁 | 26/26 |
| WSL GCC Debug | ASan/UBSan | 27/27 |
| WSL GCC | `-fanalyzer -Werror` | 27/27 |
| WSL Valgrind 3.22 | 最终 Wire/Security 重叠反例 | 2/2 无错误 |

机械门禁还包括 v6 source/archive 边界、旧 CMake surface、当前文档、公共 API 索引、1k/10k Scale、
Feature-aware 安装内容和完整失败无写回。`git diff --check` 在最终自审阶段执行。

## 8. 尚未完成

- 未对真实 Flash/Witness 做断电窗口测试；
- 未接入审计过的生产密码 Provider 与真实密钥生命周期；
- 未在 MCU 上验证 ISR/DMA 并发、缓存一致性和缓冲区约束；
- 未验证 CAN/CAN-FD/USB/Wi-Fi/UART 物理 Bearer、多跳性能、功耗和 24 h 长稳；
- 当前工作树还包含此前 V6X-S16～S18 的未提交整改，本报告不把它们误记为本轮新改动；
- 未获得本批外部复审签字，未创建 RC Tag。

## 9. 外部复审建议入口

建议按以下顺序复审：Lease 起点所有权与延迟重放、过期 Vote 的 durable 剪除、Handover Proof 在 READY/
Commit 两时点的当前性、所有 Security/Wire 部分重叠反例、All-Off/Single-Feature 安装内容，最后复跑 Full、
MSVC 多配置、Sanitizer 与 Analyzer 全量矩阵。
