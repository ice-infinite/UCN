# Merge 与 Handover 实验模型

> 文档级别：`NORMATIVE`
> 实现状态：`PARTIAL（Current 与默认关闭实验层级见正文）`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：Cluster 公共头、生产源码、独立 Archive、CMake 与测试
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：部分 ESP32-S3 实测；v4/掉电/完整多 Bearer 未发布验收

M11 是默认关闭的独立 Archive，覆盖跨 Cluster 合并和同 Cluster 计划领导权转移。当前仅为实验状态机与测试，不接生产 Cluster/Adapter。

## 两种模式

- 跨簇 Merge：旧 Epoch 与目标 Epoch 属于不同 Cluster，禁止比较两边 Term 数值；目标 Head 发送 READY。
- 同簇 Planned Transfer：目标是旧 Config 中确认的 Backup，目标 Term 必须是 `old_term + 1`；Backup 在持久化完成前仍无 Head Authority。

## 事务顺序

```text
Prepare -> Ready -> Stepdown -> Target Commit/Durable -> Authority handoff
```

Type 26～28 绑定 old Epoch、target Epoch、txid、target Config、mode 和 source；`stepdown_nonce` 只属于冻结 Wire 中实际承载它的 Type 9/29，模型不得虚构 Wire 不存在的字段。

## 选路与滞回

候选只有连续达到阈值才可形成 proposal；任一不达标样本会清零连续计数。proposal domain 绑定 Epoch、Config、容量、Wire/capability 和 Backup policy；域变化后旧样本与旧 nonce history 不得继承。同一域内的 replay history 在候选过期后仍需保留，防止小 nonce 重放。

## 不可逆 Fence

撤权、Stepdown 或 Commit 后写入 Fence。公开 reset 不能清除该 Fence，begin 只接受完整零对象，防止 `终态 -> reset -> begin` 恢复旧 Authority。

## 为什么 Merge 与 Planned Transfer 要分模式

跨簇 A→B 的 Cluster ID 不同，Term 没有共同历史；同簇 A→B 共享 Cluster ID，目标 Term 必须精确 old+1。两者若共用“更大 Term 胜出”，foreign Cluster 会被错误压制。

角色也不同：跨簇目标 B 已是另一稳定 Head，READY 由 HEAD 发；同簇目标 B 在 Commit durable 前仍是已确认 BACKUP，READY 由 BACKUP 发且没有 Head Authority。

## 双 Epoch 事务绑定

Prepare/Ready/Stepdown/Commit 必须绑定完整 old Epoch、target Epoch、txid、target Config、mode 和 source。只传 successor Head/Term 而无 target Cluster ID 会让接收端无法区分跨簇目标。

Stepdown nonce 只在 Wire 实际 Type 9/29 中存在；Type 26～28 模型不得额外要求一个线上无法携带的 nonce，否则未来 encoder 必须伪造。

## A、B、成员各自验证什么

- 旧 Head A：收到 B 的合法 READY 后，在本地验证并才发送 Stepdown；
- 目标 B：发送 READY 时仍无新 Authority，收到 Commit/持久化后验证 own READY/txid；
- 普通成员：通常没收到 B→A 单播 READY，只验证旧 Authority、Stepdown txid/nonce、完整 target Epoch 和模式规则；
- 任何节点都不能要求自己从未收到的 READY 来匹配。

## 候选观察和滞回

Score 合格必须连续出现；序列 `890,870,890,890` 在门限需要 2 样本时只在最后一个 890 合格。proposal domain 变化会清样本。候选过期后，同一 domain 的 replay nonce history仍保留，防 D1→expire→D1 小 nonce ABA；真正新 domain 才可从新 nonce 域开始。

## 事务 Deadline

所有 retry/transaction timeout 通过 duration helper，超过 INT32_MAX 拒绝。TARGET_COMMITTED 到期后不能再标 durable；`mark_target_epoch_durable()` 不能仅凭调用方传相同 Epoch 就伪造持久证明，未来 production 必须绑定 M04 submit→reload journal。

## 不可逆撤权

begin 只接受完整 canonical zero transaction。Revoke、Stepdown、Commit 写固定 Fence；reset 对非零 Fence 无副作用。Fence 由 caller-owned RAM 保护同次运行，不是原始内存破坏/掉电保证；生产接线还需 durable state。

## 验证清单

- [ ] foreign Term 大小不影响 Merge 选择；
- [ ] 同簇 target term=old+1 且目标为 confirmed Backup；
- [ ] READY 角色按模式分开；
- [ ] Wire raw↔typed 字段无虚构字段；
- [ ] proposal 连续样本、domain reset、expiry replay history；
- [ ] timeout/serial no-wrap 和 trace_count 结构校验；
- [ ] Revoke/Stepdown/Commit→Reset→Begin 全拒绝；
- [ ] 默认 archive OFF、无 production引用。
