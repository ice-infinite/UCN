# Core Wire v5 与 Profile 兼容矩阵

> 文档级别：`NORMATIVE`
> 实现状态：`CURRENT（规则）；RELEASE NO-GO`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：版本宏、Wire/API/Storage 合同、CMake 与发布门禁
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：完整发布实机门禁未完成

Wire v5 使用 W0～W3 Class。发送方只能选择双方支持且满足当前字段/MTU的 Class；接收方可解析的 Class 由编译能力决定，不等于其会启用对应高级功能。

低档节点接收高档节点命令的前提是发送格式落在共同 Class/Capability 内。协商失败应明确拒绝或选择共同格式，禁止将未知高档帧当作低档帧解析。

## 两种Profile不要混淆

- Build Profile（Nano/Lite/Full）：编译了哪些功能；
- Wire Class（W0～W3）：地址/长度/扩展字段能否在线表示。

Nano可以配置较宽RX并解析W3普通Endpoint帧，但不会因此拥有Path/Policy代码。Full也可向小地址节点发送W0，节省header。

## 发送选择

发送Class必须同时满足：

1. frame所有地址/sequence/route/path字段可表示；
2. 编码总长不超过当前Link/Path MTU；
3. 不超过本地TX maximum；
4. 不超过对端HELLO/静态声明RX ceiling；
5. 路径每跳都满足maximum profile/minimum MTU capability。

自动选择默认关闭时使用固定TX domain；开启后可选择最小共同Class，但没有能力信息时必须回到配置的保守fallback或失败，不能乐观猜测。

## 接收矩阵

| 输入 | 本地RX上限足够 | 字段合法 | 结果 |
| --- | --- | --- | --- |
| W0普通数据 | 是 | 是 | 解码/按Build功能处理 |
| W3普通数据 | 是 | 是 | Nano也可交普通Endpoint |
| W3 Path控制 | 是 | 是 | Full处理；Lite/Nano明确unsupported/config |
| 超本地RX上限 | 否 | 任意 | 早期拒绝 |
| 未知Class/版本 | 任意 | 否 | VERSION/MALFORMED，不尝试降级 |

## Node ID与地址规划

API Node ID为32-bit，但W0～W2可表示范围更窄。若产品未来会超过小Class地址域，应从一开始分配可升级地址或固定W3。手动可管理ID、MAC派生ID和Flash配置都需防冲突；MAC并非绝对永不重复的产品管理策略。

## PATH_INSTALL兼容

v5 base schema保持旧接收器可读；带Wire/MTU capability的extended schema通过显式API发给已知支持节点。新接收器可双格式decode，旧节点可能拒绝extended，因此发送端不能自动使用。

## 混合验证

至少测试每个Build Profile×Wire Class普通Endpoint、字段超过Class、Link RX ceiling、Path capability、MTU突降和中继。兼容结论绑定具体能力，不写“所有档次互通”。
