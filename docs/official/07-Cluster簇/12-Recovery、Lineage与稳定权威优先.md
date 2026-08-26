# Recovery、Lineage 与稳定权威优先

> 文档级别：`NORMATIVE`
> 实现状态：`PARTIAL（Current 与默认关闭实验层级见正文）`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：Cluster 公共头、生产源码、独立 Archive、CMake 与测试
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：部分 ESP32-S3 实测；v4/掉电/完整多 Bearer 未发布验收

Recovery 用于无可用稳定 Head 时建立受限恢复域，并最终回到稳定 Authority。它不是绕过 quorum 或持久化的快捷选举。

## Recovery 身份

Recovery Epoch/ID、candidate、nonce 和 lineage 必须来自合法域；同一 identity 的 `(term, nonce)` 更新单调。不同 Cluster 不比较 Term 大小。

## 优先级

当节点观察到可验证的稳定 Authority 时，稳定簇优先于临时 Recovery。Recovery Head 必须退让或进入合并/观察流程，不能仅因自身 Term 数值更大而压过 foreign stable Cluster。

## Backoff 与收敛

Recovery election 使用确定性/有界 backoff，减少同时建簇；网络重新连通后按 lineage、稳定 Config、Authority 和 Fence 规则收敛。时间窗口只减少冲突，不构成唯一安全依据。

## Persistence

Recovery Head 创建属于 persist-before-promise 操作。接收侧 Recovery Join/ACK 的完整生产持久化闭环仍需按当前任务边界判断，不能将局部模型测试扩大为全部 Recovery 已掉电安全。

## Tombstone

Rekey 产生的 retired identity Tombstone 跨重启保留。普通 Cluster create 不能删除 Tombstone 或重新创建已退休 Cluster ID；在无法表达完整历史集合的阶段，采用保守 fail-closed。

## Recovery 何时启动

节点在观察窗内找不到可验证稳定 Head、旧 Authority Lease 已失效且当前 Phase允许时，才进入 Recovery backoff/竞选。它不是收到一次丢包就立即自建簇，也不能在 Persistence fault 下继续 promise。

## 为什么稳定 Authority 优先

Recovery Head 是网络分区/失联时的临时收敛手段。两个分区恢复连接，若一边有可验证 Stable Config+Authority，临时 Recovery 应退让；否则每个临时组都用自己的较大 Term 争胜，会长期分裂。

跨 Cluster 比较先返回 FOREIGN，再应用 stable-over-recovery policy。只有同 identity/Cluster 的 serial 才单调比较。

## Lineage 解决什么

Lineage 记录 Recovery 身份从哪个历史域演化、replacement/nonce/term 关系，帮助节点区分同名重放、合法恢复和新建。相同 Identity 的 `(term,nonce)` 更新必须单调；域变化清旧 proposal 样本但不能删除退休历史。

## Backoff 与同时建簇

多个节点同时失去 Head 时使用确定性/随机有界 backoff，候选质量/Node identity 可参与排序。Backoff 只降低碰撞概率，安全仍来自持久 Vote/quorum/Fence。到点最早不代表无条件获得 Authority。

## Detach 后新 Cluster

普通同 Cluster EPOCH_COMMIT 只能 exact term next。Detach 后建立不同 Cluster/Term 1 要使用专用 create operation，约束新 ID 非零/广播、不复用 parent/retired，Active/Max一致，清理/保留各历史字段按合同处理。

发生 Rekey/Tombstone 后当前 Record v4 保守拒绝普通 create，避免删除 Tombstone或重建 retired ID；这是可用性换安全的阶段性边界。

## Recovery 持久化范围

Recovery Head 创建已进入 persist-before-promise 合同；接收端 Join/ACK 是否全部 durable 要逐路径核对，不能用一句“Recovery 已持久化”覆盖未接线部分。

## 收敛失败处理

- 看到稳定 Authority：撤 Recovery Authority/Fence 后观察/Join；
- 多 Recovery foreign：按 lineage/merge policy，不比 Term；
- Provider failed：保持 Detached/Fenced；
- ID 历史无法证明：拒绝 create/reuse；
- 长期网络分区：各域可局部运行的业务范围由产品定义，不能假装全局单 Authority。

## 验证清单

- [ ] 无稳定 Head+Lease 到期才启动；
- [ ] stable authority 总是优先于 foreign recovery；
- [ ] 同 identity term/nonce 单调，foreign 不比较；
- [ ] 多候选 backoff 的安全不依赖时间唯一性；
- [ ] dedicated cluster create 与普通 epoch commit 分开；
- [ ] Tombstone/rekey 后不删除退休历史；
- [ ] Head create/Join/ACK 每条持久化声明逐路径有证据。
