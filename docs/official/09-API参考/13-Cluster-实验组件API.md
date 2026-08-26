# Cluster 实验组件 API

> 文档级别：`REFERENCE`
> 实现状态：`CURRENT`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：include/ucn 公共头、链接目标与公共 API 测试
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：API 合同不等同于各平台实机驱动完成

实验 API 包括 Wire v4 codec/semantic、Membership v2、Config/Joint、Authority、Backup Mirror、Takeover、Handover、Recovery target、Rekey 和 invariant/property model。

使用前必须同时检查：

- 对应 CMake target 是否默认 OFF；
- encoder/测试宏是否只对实验目标生效；
- 是否仅是纯模型，尚无生产 RX/TX/FSM；
- 是否需要 Persistence/Config/Authority 的前置状态；
- 当前外审状态和发布阻断。

M10/M11/M13 不得直接链接进默认产品。实验 API 的结构和签名仍可能在正式生产接线前破坏性调整。

## 组件与依赖

| 组件 | 主要 API 对象 | 前置证明 | 当前边界 |
| --- | --- | --- | --- |
| Wire v4 | raw/semantic frame、pending certificate | RFC4、strict codec | encoder 默认关，无生产 RX/FSM |
| Membership v2 | member table、Stable/Joint voter set | canonical identity | legacy metadata bridge，不授 Authority |
| Config M07 | tx、joint runtime、persist owner、backup gate | M04 Provider、双 quorum | 受限实验路径 |
| Authority M08 | runtime、Lease、Fence | committed voter set、current time | 未作为 v4 产品 Authority 放行 |
| Backup M09 | mirror、snapshot、coverage/profile | Config/Epoch、完整 protected voters | 与旧 v3 handler 隔离 |
| Takeover M10 | VoteId、certificate、persist owner | Backup coverage、quorum、M04 | default-OFF Archive |
| Handover M11 | candidate/transaction/fence | 双 Epoch、持续样本、persistence | default-OFF Archive |
| Recovery M12 | lineage/identity/backoff | 稳定 Authority 优先 | 软件模型范围 |
| Rekey M13 | history、transaction、tombstone | no-wrap、M04、Config | default-OFF Archive |
| Invariant M14 | object/network check | 上述对象的合法状态 | 诊断/门禁，不自动修复 |

## 使用原则

实验组件不是“把几个 API 顺序调用就能获得完整 Cluster”。它们是为了冻结每个安全合同并独立测试。生产接线还需要一个唯一 Owner 把 Wire admission、Persistence continuation、Config、Authority、Backup 和 FSM 原子串联。

例如 Config Commit 的正确条件不是：

```c
if (old_quorum && new_quorum) commit();
```

而是同时要求 durable Prepare、durable Joint、live Joint runtime、exact txid/C_new、Backup exact gate、两集合 quorum、当前 Authority/Fence 和 Provider I/O 重入门。绕过任一层都会产生已提交状态与 RAM 权威不一致。

## 实验构建

按任务选择明确 CMake 开关并使用独立 build 目录，例如 M10/M11/M13 各自 ON。验证默认 OFF 时生产 archive 不含对应 object/symbol；ON 构建运行定向测试、Release、sanitizer/analyzer 和资源报告。

## 不可跨越的边界

- 不从测试 builder 直接生成生产 v4 权威帧；
- 不把 `VOLATILE_TEST` 当掉电证据；
- 不让纯 model 修改默认 `ucn_cluster_t`；
- 不用 role/phase 代替 Authority preflight；
- 不在 Record schema 未区分 legacy/new PREPARED 时开放新事务；
- 不因外部审计某一个子项 GO 就解除 M05 顶层 `AUDIT HOLD`。

## 评估一个实验 API 是否可用

1. 查看对应头文件与 CMake target；
2. 查看任务表和最新外审范围；
3. 核实测试 target 是否带私有宏；
4. 检查生产 source 是否真的调用；
5. 核对依赖的 Persistence/Authority/Config 是否为同一状态；
6. 在产品文档中写清“模型/Codec/集成/实机”哪一层已完成。
