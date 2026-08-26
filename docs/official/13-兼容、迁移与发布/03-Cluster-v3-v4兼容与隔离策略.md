# Cluster v3/v4 兼容与隔离策略

> 文档级别：`NORMATIVE`
> 实现状态：`CURRENT（规则）；RELEASE NO-GO`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：版本宏、Wire/API/Storage 合同、CMake 与发布门禁
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：完整发布实机门禁未完成

Cluster v3 固定 32 B，v4 固定 40 B。strict dispatcher 按版本和精确长度分派，无降级解析。v4 encoder 默认关闭，生产 Current FSM 仍使用 v3。

Host dual-stack 测试证明 Codec 隔离，不代表生产混跑已放行。正式接线前需定义 Strict v4、显式 legacy non-voting、Capability、Authority 和网络升级顺序。

## 当前两条路径

| 路径 | 格式 | Type | 当前用途 |
| --- | ---: | ---: | --- |
| Current Cluster | v3 / 32 B | 1～19 | 默认生产源码/FSM，Authority能力受围栏 |
| Target Wire Archive | v4 / 40 B | 1～33 | strict codec/semantic/tests，encoder默认关 |

两者不共享“长度差不多就解析”的入口。`v3+40`、`v4+32`、39/41 B全部拒绝。

## v4 capability/角色

v4引入明确Wire Offer/Capability、Config/Joint/证书/Handover/Rekey等语义。legacy v3若未来在v4网络暂存，只能通过显式policy成为non-voting，`required_bits=0`，不能成为Voter、Backup或Head Authority。

这不是“v3节点永远不能传业务”：Core业务仍可在共同Wire内通信；限制的是Cluster控制权。

## 升级顺序草案边界

正式方案至少需要：

1. 所有节点先升级到能严格解v4但encoder仍关的版本；
2. 诊断确认capability/Storage/安全就绪；
3. legacy节点降为non-voting并从CommittedVoterSet移除；
4. 通过Joint Config完成voter切换；
5. 受控开启v4发送/生产FSM；
6. 保留回滚和Fence策略。

当前只完成部分软件组件，不能执行上述生产切换。

## 不能做的事

- 看到40 B就从v3尾部猜字段；
- 让v3 Backup/Takeover帧建立新Authority；
- codec测试宏泄漏生产；
- v4可解析就直接写Membership/Directory；
- mixed network中跨Cluster比较Term；
- 将测试`VOLATILE_TEST`作为升级掉电保障。

## 发布前测试

双格式strict dispatch、所有角色/flags/字段negative、capability协商、legacy non-voting、Joint迁移、重启/掉电、旧节点拒绝路径、网络分区和回滚。还需真实多板，Host dual-stack不够。
