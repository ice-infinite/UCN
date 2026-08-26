# Backup Mirror、Snapshot、Coverage 与 Profile

> 文档级别：`NORMATIVE`
> 实现状态：`PARTIAL（Current 与默认关闭实验层级见正文）`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：Cluster 公共头、生产源码、独立 Archive、CMake 与测试
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：部分 ESP32-S3 实测；v4/掉电/完整多 Bearer 未发布验收

## Mirror

Backup 保持 committed/staging 双缓冲镜像。Snapshot 的 BEGIN、MEMBER、END 使用严格 sequence：`BEGIN=0`、成员 `1..N`、`END=N+1`；完整校验后原子交换，未完成 staging 不能污染 committed view。

## Delta

Delta 绑定 Epoch、Config、snapshot generation 和 sequence。静态资格字段不能被普通动态 Delta 修改；错序、重复、旧 Epoch 或不匹配 Config 必须拒绝。

## Coverage

Coverage 验证 protected voter 是否在镜像中处于允许状态：

- 明确 `SUSPECT` 才能进入有界 grace；
- `REMOVED` 或缺失 protected voter 立即永久取消本次 assignment 的 takeover 资格；
- 后续 ADMITTED 不能复活旧 assignment，必须重新分配 Backup。

Joint 模式同时检查 `C_old` 和 `C_new` 的受保护集合。

## Profile

Backup Profile 汇总镜像完整度、Coverage、延迟、容量和 capability，用于候选排序；它不是单独的 Authority。候选达到分数阈值还必须连续满足样本要求，任一不达标样本会清零连续计数。

## 当前边界

M09 模型尚未接入生产旧 v3 Backup handler，也未授权生产 v4 FSM。软件镜像正确性不替代实机链路中断、突发流量和掉电恢复验证。

## Backup 为什么需要完整 Mirror

Backup 接管后若不知道当前成员、Voter/Config 和资格状态，就可能依据过期视图授予 Authority。Head 因此先发完整 Snapshot 建基线，再用 Delta 更新变化。只有 committed 镜像完整且 Coverage 合格，Backup 才有接管资格。

## Snapshot 双缓冲

Staging 在 BEGIN 时冻结 Epoch、Config、generation、snapshot ID 和期望序列。MEMBER 逐条写 staging；END 校验数量/序列/marker/完整性后一次交换到 committed。中途 timeout、乱序、重复冲突或新 BEGIN 不能污染上一个 committed。

双缓冲会增加 RAM，但避免业务/Takeover 读到“半张新表”。

## Delta 的允许范围

Delta 适合更新 lease/status 等动态字段。Node ID、Voter 资格、Wire capability、Config binding 等静态安全字段不能由普通 Delta 提升；这些变化必须经完整 Snapshot/Config 事务。

错 sequence 不应猜补，通常要求重新 Snapshot。

## Coverage 规则为何严格

protected voter 缺条目与明确 REMOVED 都说明镜像不能证明完整，立即使本 assignment 永久 ineligible；只有条目存在且明确 SUSPECT 才能短 grace。后续收到 ADMITTED 也不能复活，必须 Head 重新 assign Backup，防止旧 assignment ABA。

Joint 时 C_old/C_new 的 protected voter 都要覆盖，否则接管无法验证双 quorum。

## Backup Profile 和候选排序

Profile 可包含镜像覆盖、同步延迟、容量、Wire/capability 和策略分。分数达到阈值需要连续样本；不合格样本清零。Proposal domain 中 Epoch/Config/容量/wire/capability/Backup policy 任一变化，旧连续样本和 nonce history不能错误继承。

短期动态指标不应无限塞进 identity 导致每帧域变化；只有改变资格语义的字段进入 domain。

## 完整同步时序

```text
Head选择候选
→ BACKUP_ASSIGN(epoch/config/generation)
→ Backup进入SYNCING并清staging
→ BEGIN, MEMBER 1..N, END
→ atomic committed swap
→ Coverage/Profile连续合格
→ BACKUP_READY
→ 后续Delta维持
```

任何阶段 Head/Config/Epoch 改变都应终止或重开，不把旧 Snapshot 拼进新域。

## 验证清单

- [ ] BEGIN=0/MEMBER=1..N/END=N+1；
- [ ] staging 失败保持 committed逐字节不变；
- [ ] Delta 不能改静态资格；
- [ ] Stable/Joint 缺 protected voter 永久 ineligible；
- [ ] ADMITTED 不复活旧 assignment；
- [ ] 连续样本被不合格样本/domain变化清零；
- [ ] v3 production handler 未接 M09；
- [ ] 实机断链/突发/重同步的 RAM、时延、流量已测。
