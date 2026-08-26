# 当前默认 Cluster v3 32B 行为

> 文档级别：`NORMATIVE`
> 实现状态：`PARTIAL（Current 与默认关闭实验层级见正文）`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：Cluster 公共头、生产源码、独立 Archive、CMake 与测试
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：部分 ESP32-S3 实测；v4/掉电/完整多 Bearer 未发布验收

## Wire 合同

默认 Cluster 消息格式为 v3，固定 32 B，Type 范围 1～19。解码器严格校验版本、长度、角色、保留字段和各 Type 的字段合法域；错误帧不得部分写入输出对象。

## 当前 FSM 覆盖

默认实现包含：

- Detach/Observe 与自动选举；
- Join 请求、接受和成员租约；
- Head/Member/Backup 的当前兼容状态机；
- Recovery、Merge 和 Term conflict 的现有边界；
- Cluster 状态、统计和诊断视图。

## 已加的 v3 Authority 围栏

Membership v2 引入后，生产 RX 会拒绝能够通过旧 v3 控制面建立或消费 Backup/Takeover 权威的 Type。历史 v3 行为只可存在于明确的测试桥中，不能进入默认产品 archive。

这意味着当前 v3 仍可承担兼容控制面与基础簇行为，但不能被描述为已经提供完整的 v4 Voter/Backup/Takeover 安全语义。

## 限制

- v3 没有 Wire v4 的完整 capability、Config/Joint、证书分片语义；
- 生产 v4 RX/TX/FSM 尚未接线；
- 真实掉电、Flash 双槽和多板故障恢复尚未完成发布验收；
- Cluster 当前能力不能扩大解释为万级网络已经实机成立。

## v3 Frame 与 Core Frame 的关系

Cluster v3 的“32 B”指 Cluster 控制消息 Payload/专用编解码合同，不是 Core Wire v5 W0～W3 的普通 Header 大小。Cluster message 仍通过 UCN Core Endpoint/Route/Link 承载。两套版本号分别管理不同层。

## 默认启动过程

概念上经历：初始化配置/Provider→DETACHED 观察→发现合法 Head 则 Join→无 Head 时按观察/选举策略成为 Candidate/Head→成员 Keepalive/Lease 维护。实际 Phase/定时和发送门以当前源码配置为准。

初次上电选举需要观察窗口和退避，避免所有节点同时立即宣称 Head。稳定后普通业务不等待每次选举。

## 当前成员与 Head 行为

- Head 可维护当前 v3 成员/租约并发布兼容控制；
- Member 只接受匹配 Epoch/来源/Phase 的消息；
- 旧/foreign Epoch 按 domain 规则处理；
- Link/Lease 失效进入当前 Recovery/Merge/Term conflict 边界；
- 状态表固定，不随节点数动态分配。

这些行为要按“当前兼容 FSM”描述，不能自动套用 v4 Committed Voter/Joint Certificate 的更强安全语义。

## 为什么围栏旧 Backup/Takeover Type

Membership v2 规定 v3 legacy 成员不能成为 v4 受保护 Backup。若生产 RX 继续接受 v3 `BACKUP_ASSIGN/TAKEOVER`，攻击者/旧节点可绕过 Committed VoterSet 和证书建立 Authority。

因此生产入口在状态写入、计时、统计和 mirror 前统一拒绝相关 v3 Type。测试桥可在私有 target 重放历史行为，但不会链接进默认 archive。

## 混跑网络的现实边界

v3 节点可参与当前 v3 基础簇，但不能被 v4 policy 视为 Voter/Backup。未来 v4 生产接线必须显式定义 legacy non-voting 兼容；不能收到 v3 后“尽量解析”为 v4。

## 当前能用于什么

- 研究/验证基础自动分簇、Join、成员租约和 Current 状态；
- 在明确限制下做 ESP32 多板基础测试；
- 为 v4/Target 状态机提供兼容基线和回归；
- 不能用于宣称 majority takeover、Joint Config、完整持久 Authority 已生产闭环。

## 测试要求

- 精确 32 B、Type 1～19、所有保留位/角色负例；
- 入簇/离簇/Head 丢失/foreign Epoch；
- v3 Backup/Takeover 生产 archive 全部 `ACCESS` 且对象逐字节不变；
- test bridge 宏/对象不泄漏默认目标；
- 真实多板选举/Lease/RF/掉电与 Host 模型分开记录。
