# UCN Cluster Persistence v4 当前合同

## 版本

- Provider API：v1；
- 当前 writer schema：v4；
- 当前 record：388 B（16 B header + 372 B payload）；
- v1/v2：280 B 只读迁移；v3：292 B 只读迁移。

## 不变量

1. load-before-init；损坏、未知或不规范 record fail-closed；
2. persist-before-promise；durable 前不得发送 Vote/ACK/Advertise/Commit；
3. `PENDING` 时冻结相关推进，回调前建立 reentrancy gate；
4. 完成后 reload 并验证 journal、fingerprint 与 exact state；
5. serial/generation 不回绕；
6. Config、Takeover、Rekey operation 不能用通用 Epoch operation 绕过专用 transition；
7. Tombstone 与 successor/transaction 原子绑定。

## schema 迁移

旧 record 只能经过明确 decoder/migration。legacy PREPARED 迁移有 provenance 约束；新的 PREPARED 不能被当成 legacy abort。产品物理槽必须容纳 388 B，并在 writer 切换前完成旧槽读取/迁移策略。

## 当前边界

Host fake、同步/异步/Pending/reentry/crash-order 测试已覆盖软件合同。真实 Flash 擦写、双槽原子性、掉电窗口、磨损和 MCU 启动时序仍由 M14-08 验证。
