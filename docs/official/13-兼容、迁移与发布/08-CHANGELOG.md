# CHANGELOG

> 文档级别：`NORMATIVE`
> 实现状态：`CURRENT（规则）；RELEASE NO-GO`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：版本宏、Wire/API/Storage 合同、CMake 与发布门禁
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：完整发布实机门禁未完成

本文件只记录已经合入当前分支的用户可见变化，不收录尚未实现的建议。

## 5.0.0（开发中，未发布）

- Core Wire v5 与 W0～W3 Adaptive Wire Class；
- MCU-first 的静态 Node、Neighbor、AODV-Lite Route、RERR 和多 Link/Bearer；
- Path Identity/Install/Revoke/Trace、Pinned/Failover/Auto-Best/Q1 Balance；
- LC-1 Link Cost、Standard Adapter preset 与动态质量/滞回；
- Port API v2、Protocol Owner、Event Runtime、Stream/COBS、CAN-FD/Classic CAN Source；
- Service Router/Bridge，统一本机任务与跨 MCU 命令/结果语义；
- Transfer T32～T8K、有界分片、CRC32、累计 ACK、窗口与并发能力；
- Security Policy/Provider、E2E opaque forwarding、Session/Sequence/replay接口；
- Cluster Current v3/32 B、Persistence/成员/恢复等当前软件路径；
- Wire v4/40 B Codec和M07～M13受限/默认关闭实验组件；
- Nano/Lite/Full、全局产品配置覆盖、资源/测试门禁；
- 官方文档、reference/evidence/experimental/archive分层重建。

### 破坏性变化

- Port API升级到v2，task/ISR critical拆分，需要具名初始化与全量重编译；
- 部分公共Storage/API进入版本化/opaque边界，旧二进制ABI不承诺；
- Cluster Persistence writer为Record v4/388 B，读取历史v1/v2/v3但回滚需单独判断；
- 当前仍处于预发布优化期，尚未冻结最终生产兼容承诺。

### 安全与发布限制

- 库不提供生产AEAD/key backend；
- Wire v4 encoder默认关闭，未接生产Cluster FSM；
- M10/M11/M13默认关闭；
- 真实Flash掉电、完整多Bearer/RTOS、长稳/功耗尚未全门禁；
- M05保持`AUDIT HOLD`，M14发布结论`NO-GO`。

### 文档状态

本CHANGELOG只列当前分支已存在内容。建议/计划仍在archive/experimental，不加入“Added”。候选冻结时还需补精确commit、artifact hash、迁移工具、支持板卡和完整修复列表。

破坏性变化、Record 迁移和正式 release hash 在发布候选冻结后补充。当前条目不构成 release tag。
