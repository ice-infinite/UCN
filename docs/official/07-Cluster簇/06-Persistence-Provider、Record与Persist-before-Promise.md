# Persistence Provider、Record 与 Persist-before-Promise

> 文档级别：`NORMATIVE`
> 实现状态：`PARTIAL（Current 与默认关闭实验层级见正文）`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：Cluster 公共头、生产源码、独立 Archive、CMake 与测试
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：部分 ESP32-S3 实测；v4/掉电/完整多 Bearer 未发布验收

## Provider 合同

Persistence Provider API v1 提供 `load`、`submit` 和可选 `poll`：

- `load` 在初始化可见前同步完成；
- `submit` 可同步完成或返回 `PENDING`；
- 异步 Provider 必须复制请求数据，不能借用调用栈；
- `poll` 可多次返回 `PENDING`；
- 回调重入必须被独立 I/O gate 拒绝。

## Record

当前 writer 使用 Cluster persistence Record schema v4，记录大小 388 B；解码兼容既有 v1/v2 280 B 和 v3 292 B。Record 使用 canonical 编码、CRC、明确状态和 operation journal，零初始化不能伪装成成功提交。

## Persist-before-Promise

会形成不可撤销承诺的动作必须先持久化，再产生外部可见效果。例如 Epoch、Vote、Config、Rekey、Recovery 创建等：

1. 校验 `committed_state + operation + next_state`；
2. 建立 I/O Fence；
3. Provider 提交；
4. reload 并核对 generation/journal/fingerprint；
5. 才允许 ACK、Advertise、Commit continuation。

持久化失败、撕裂写或 reload 不一致必须 fail-closed。持久化成功后的链路背压属于传输错误，不得误记为 persistence fault；可以保留 durable promise 并有界重试响应。

## Legacy PREPARED

旧 Record 的 `PREPARED` 迁移只能由专用操作原子清理并推进 boot incarnation。未来真正开放 Config/Rekey PREPARED 前必须通过 schema/来源标记区分旧迁移记录，防止新事务被误清除。

## 未完成证据

软件 Provider 与对抗测试不能替代目标 MCU 上的真实 Flash 双槽、擦写寿命、写入原子性和可控掉电测试。

## 为什么 Vote/Epoch 必须持久化在 ACK 前

节点若先向候选 A 发送 Vote ACK，随后掉电且未记住这次 Vote，重启后可能在同一 Epoch 又投 B，形成两个多数派。Persist-before-promise 的目的不是保存调试状态，而是让重启节点继续受旧承诺约束。

同理，Head 宣布新 Epoch/Config 前必须保证掉电恢复后不会回到旧权威。

## Record v4 主要状态类别

当前 writer 记录 Active/Max Epoch、完整 VoteId、committed/staging Config、Config/Rekey transaction、boot incarnation、operation journal、Tombstone 等安全恢复所需状态。精确字段见公共 schema/API 参考；任何 `has_* = false` 的关联字节都要 canonical。

v1/v2/v3 decode 兼容不等于旧固件能读 v4。升级方向、writer 版本和回滚策略是不同问题。

## Provider Completion 合同

Completion 必须有显式非零状态，零初始化结构永远不是 COMMITTED。同步 Provider 无 poll 却返回 PENDING 必须失败；异步 Provider 在 submit 返回前复制 operation/record，之后 poll 可多次 PENDING。

I/O callback 前先建立独立 `io_active` gate，防止 Provider 同步重入 step/init/submit/poll。返回后才设 pending 会留下重入窗口。

## Journal 与幂等

operation ID、kind、fingerprint 和 next state 共同验证：

- 相同 ID + 相同状态可幂等证明/重放；
- 相同 ID + 不同状态拒绝；
- Commit 必须匹配同 txid 的 Prepare/staging；
- Config/Rekey 不并发；
- Tombstone 只能与匹配 Rekey Commit 原子产生；
- Epoch/Config/txid/incarnation 严格单调或按专用新域操作。

一个通用 EPOCH_COMMIT 不能绕过 Takeover 专用 Vote 约束。

## 启动 load-before-init

REQUIRED 模式初始化先同步 load/校验 Record，再推进 boot incarnation，最后才把 Cluster 对象暴露为 enabled。损坏、非法状态、Provider 缺失或 reload 不一致时清空/禁用并 fail-closed。

旧合法 PREPARED 的迁移由专用 legacy abort 操作完成，且只能清唯一旧事务、保持其他 durable 状态、单调推进 incarnation。未来新 PREPARED 必须用 schema/source marker 区分。

## 双槽 Flash 应如何实现

产品常用 A/B slot：写 inactive slot 的完整 record+CRC+generation，刷写/校验后原子选择最新完整 generation。掉电可留下旧完整或新完整，不允许一个“半新”被当 COMMITTED。

真实 Flash 还要考虑 erase block、write alignment、cache、wear leveling、NVS garbage collection 和 brownout；通用 Provider API 不替产品选择实现。

## 持久化成功后的网络失败

Vote 已 durable 后发送 ACK 遇到 NO_SPACE/LINK_DOWN：保留 durable Vote，建立有界 ACK retry；不能把传输错误记成 persistence fault，也不能重新投另一个候选。重启后收到相同 Prepare，reload 的 durable Vote可用于幂等 ACK。

## 验证清单

- [ ] 全零 completion/load result 无效；
- [ ] canonical encode/decode/golden/CRC；
- [ ] 每个 Provider callback 重入被拒绝；
- [ ] PENDING 多轮、撕裂写、reload mismatch；
- [ ] Vote/Epoch/Config/Rekey 在外发承诺前 durable；
- [ ] 成功持久化后的传输背压不污染 persistence fault；
- [ ] 旧 schema 迁移和新 PREPARED 隔离；
- [ ] 真实 MCU 双槽/掉电/磨损完成产品证据。
