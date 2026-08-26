# Majority Takeover 实验模型

> 文档级别：`NORMATIVE`
> 实现状态：`PARTIAL（Current 与默认关闭实验层级见正文）`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：Cluster 公共头、生产源码、独立 Archive、CMake 与测试
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：部分 ESP32-S3 实测；v4/掉电/完整多 Bearer 未发布验收

M10 是默认关闭的独立 Archive，用于验证 Backup 通过多数派证书接管 Head 的模型。它不在默认 `ucn_cluster` archive 中，也没有生产 Wire 接线。

## 主要对象

- 冻结的 Stable/Joint VoterSet；
- proposed Epoch；
- 完整 VoteId，绑定 Cluster、Term、Backup generation、候选节点等身份；
- voter bitmap、双 quorum 证书与 canonical CRC；
- persistence owner 和终态 transaction。

## 安全边界

- 同一 Active Epoch 只允许一个完整 VoteId；相同 Vote 可幂等重放，冲突 Vote 拒绝；
- 历史 Epoch 的 Vote 可被新 Epoch 的 Vote 原子替换；
- 普通 `EPOCH_COMMIT` 不能绕过完整 M10 Vote，只有专用 Takeover Epoch 操作可提交其指定的 successor Head；
- `EPOCH_DURABLE` 是单向终态，后续 step、迟到 vote 或 unreachable 事件不得撤销；
- Joint Certificate 必须分别验证旧、新 voter set quorum。

## 能证明什么

现有模型和测试证明内存状态机、证书/持久化转换的受限软件合同。它不证明真实网络中已经能自动接管，也不证明 Flash 掉电、恶意节点或生产 RX owner 安全。

## Takeover 的目标安全性质

旧 Head 失联后，只有当前 Config 指定且 Mirror/Coverage 合格的 Backup，在取得当前 VoterSet 多数派后才能成为新 Head。网络分区中的少数派不能仅靠 timeout 自封。

## 实验事务流程

```text
Backup冻结active Epoch/Config/VoterSet
→ 生成proposed Epoch和完整VoteId
→ 向Voter发送Prepare
→ 每个Voter persist Vote before ACK
→ 收集bitmap/certificate，Stable或Joint quorum
→ 专用 TAKEOVER_EPOCH_COMMIT 持久新Epoch
→ EPOCH_DURABLE终态
→ 才可由未来production continuation建立Head Authority
```

模型本身不会发真实 Wire v4，也不会修改默认 Cluster FSM。

## 完整 VoteId 为什么包含 generation

同 Cluster/Term 下 Backup assignment generation 或 candidate 不同，属于冲突 Vote。只记录 Term 会让重启 Voter 对不同 Backup 再投一次。完全相同 Vote 可幂等 ACK，当前 Active Epoch 的不同 Vote 拒绝；历史 Epoch 的 Vote 才能被新 Epoch 原子替换。

## Certificate

Voter bitmap 的位序来自 canonical VoterSet。Joint Certificate 要分别携带/验证 C_old 与 C_new 的片段、Config anchor、proposed Epoch、VoteId 和 canonical CRC。单个 32-bit bitmap不足覆盖最大成员+Head或双集合，因此 Wire v4 使用受限分片模型。

## 为什么通用 EPOCH_COMMIT 要围栏

如果已有完整 M10 Vote，攻击者/错误调用方使用普通 `EPOCH_COMMIT` 把任意 Head 写入同 Cluster Term+1，就绕过证书。持久层检测当前 Vote 域后只允许专用操作提交 VoteId 指定 successor。

## EPOCH_DURABLE 终态

新 Epoch 已落盘后，timeout、迟到 Vote、unreachable 事件和普通 step 都不能改回 QUORUM/ABORTED。精确 durable replay 可幂等成功；其他输入零写拒绝。否则 RAM 说事务失败而重启又加载新 Epoch，会分裂运行/持久事实。

## 两轮连续接管

第一轮从 Term 5→6 完成后，历史 Vote 仍保留用于证明。第二轮基于 Active Term 6 新 Snapshot/Config 生成新 Vote；持久层允许替换历史 Term 5 Vote，但仍禁止对当前 Term 6 冲突双投。

## 当前隔离与启用前提

M10 CMake 默认 OFF，默认 `ucn_cluster` archive 不含 takeover object，生产 Core/Adapter 无调用。启用 production 前还需 Wire v4 RX/TX、M09 Backup、M04 Persistence、Authority continuation、攻击与掉电整体审计。

## 验证清单

- [ ] Stable/Joint certificate canonical/bitmap/quorum；
- [ ] 冲突 Vote、重复 Vote、历史 Vote轮换；
- [ ] 通用 Epoch commit 绕过拒绝；
- [ ] durable 后未到期/到期 step 和迟到事件全无副作用；
- [ ] 两轮连续 takeover；
- [ ] Provider PENDING/restart/reload；
- [ ] 默认 OFF archive/符号隔离。
