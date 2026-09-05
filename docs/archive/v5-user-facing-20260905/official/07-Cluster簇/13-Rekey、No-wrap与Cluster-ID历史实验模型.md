# Rekey、No-wrap 与 Cluster-ID 历史实验模型

> 文档级别：`NORMATIVE`
> 实现状态：`PARTIAL（Current 与默认关闭实验层级见正文）`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：Cluster 公共头、生产源码、独立 Archive、CMake 与测试
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：部分 ESP32-S3 实测；v4/掉电/完整多 Bearer 未发布验收

M13 是默认关闭的 Archive，用于在 serial 接近安全阈值或身份必须轮换时建立 successor Cluster。它不是普通 Term 自增，也不能由 `EPOCH_COMMIT` 代替。

## 触发条件

- Term、Config generation、transaction ID 或 incarnation 接近 no-wrap 阈值；
- 当前身份需要受控退役；
- 策略允许且 Stable/Joint quorum、Backup/Persistence 条件成立。

## 事务

Rekey Prepare 持久化 predecessor Epoch/Config、successor Cluster ID、目标 Term、incarnation 和 txid；Commit 必须精确匹配 Prepare，并原子生成 retired identity Tombstone。Config 与 Rekey 不能同时 PREPARED。

## No-wrap

所有 serial 使用统一 checked-next 规则。达到阈值后拒绝继续自增，通过 Rekey 或明确恢复路径进入新身份域；禁止自然整数回绕后复用旧消息空间。

## 历史与重放

Tombstone 绑定 `retired -> replacement`，重启后仍拒绝旧 ID 创建和旧事务重放。当前 Record 的历史表达能力有限，因此在不能证明安全迁移时宁可拒绝新建簇。

## 当前状态

M13 软件模型不在默认 archive。生产启用前仍需完成 Wire/FSM 接线、真实持久化、跨版本升级、Cluster ID 生命周期和回滚验收。

## 为什么要在阈值前轮换

32-bit serial 自然回绕后，旧捕获消息可能再次落入“看起来更新”的区间。UCN 使用低于最大值的 rotation threshold，预留足够空间完成 Rekey；达到阈值后 checked-next 失败，不继续加一。

Term 可表示很大不代表实际应累加到 4G。跳数同理：表示范围不是建议运行范围。

## Rekey 与普通换 Key 的关系

Cluster Rekey 不只是替换密码 Key，还建立 successor Cluster identity、Term=1、新 incarnation，并退休 predecessor。它影响 Epoch、Config、Directory、Tombstone和重放域；不能只更新一个 key pointer 后沿用旧 Cluster ID。

## Prepare/Commit 原子状态

Prepare 冻结 predecessor Epoch/Config、successor Cluster ID/Term/incarnation、txid和必要摘要。Commit 必须逐字段匹配，并在同一 durable next state 中：

- active/max 切 successor；
- committed rekey 记录完成；
- 生成 `retired -> replacement` Tombstone；
- 清/推进允许的 transaction；
- 保持 boot incarnation 等非本操作字段单调/不变。

提前 Tombstone、Prepare A/Commit B 或 Config/Rekey 双 PREPARED 都拒绝。

## Tombstone 生命周期

Tombstone 防止旧 Cluster A 在 A→B 后重新创建。普通新建 C 不能顺手清 A→B 历史。当前单记录若无法保存无限 retired set，协议在不能证明新 ID 未退休时 fail-closed；未来可扩 schema/外部 lineage store，但必须定义迁移。

## 跨版本升级

旧 Record v1/v2/v3 可能没有新来源标记/完整 history。升级时要么安全迁移到 v4，要么明确擦除并让设备以受控新身份入网。不能让新固件把真正新 PREPARED误作 legacy abort。

旧固件回滚也可能无法读 v4，不能只刷 binary；发布包需要 storage compatibility/rollback matrix。

## 触发与运维

产品应在阈值前足够早发告警/启动维护，不等最后一个 serial。Rekey 需要 stable quorum、Backup和持久介质；条件不足时停止继续产生新 Authority并进入可诊断状态。

## 验证清单

- [ ] 每个 serial threshold-1/threshold/overflow；
- [ ] Prepare/Commit 全字段 exact match；
- [ ] Config/Rekey 并发与提前 Tombstone 拒绝；
- [ ] A→B 后普通 create A/C 的保守规则；
- [ ] 重启保留 retired→replacement；
- [ ] schema v1～v4 升级/擦除/回滚；
- [ ] 默认 M13 OFF 且无 production符号/调用；
- [ ] 真实 Flash、Key provisioning 和跨节点 Rekey 尚需独立验收。
