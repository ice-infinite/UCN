# Cluster 发布阻断、硬件验收与回滚

> 文档级别：`NORMATIVE`
> 实现状态：`PARTIAL（Current 与默认关闭实验层级见正文）`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：Cluster 公共头、生产源码、独立 Archive、CMake 与测试
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：部分 ESP32-S3 实测；v4/掉电/完整多 Bearer 未发布验收

## 当前结论

Cluster 软件结构已经覆盖大量 Current/Target 组件，但发布状态仍是 **NO-GO**：M05 顶层 `AUDIT HOLD` 未解除，不能启用默认 v4 生产 RX/TX/FSM 或宣称完整 Authority 已产品化。

这里的 `NO-GO` 不是说所有 Cluster 代码都不可使用，而是发布声明必须精确分层：当前默认 v3/32 B 行为可按其已验证范围使用；Wire v4 Codec、M07～M13 模型只能用于实验构建和继续审计；真实掉电安全、多 Bearer、MCU 资源和产品安全仍无完整证据。

### 候选版本需要绑定的基线

每次发布候选必须固定以下内容，任何一项变化都要重新判断受影响门禁：

- Git commit、分支、子模块和 dirty 状态；
- 协议版本、Wire format、Type registry、公共 API/ABI；
- Persistence schema、迁移路径和擦除策略；
- 编译器、优化级别、Profile、容量宏和功能开关；
- 测试清单、硬件型号、接线、固件 hash 与原始日志；
- 已知限制、AUDIT HOLD 项、回滚包和操作步骤。

## 发布前必须关闭

- 生产 v4 decoder/encoder、RX Owner、FSM 和资格/Authority 接线的独立审计；
- 真实 Flash 双槽、撕裂写、反复掉电与磨损测试；
- 四板及更多节点的选举、Join、分区、恢复、Backup/Head 故障测试；
- UART/CAN/Wi-Fi 等多 Bearer 下控制面隔离、延迟和拥塞测试；
- 目标 MCU 的 RAM/Flash/Stack/CPU/功耗测量；
- AEAD、密钥、ACL、重放状态和升级回滚安全；
- v3/v4 混跑、Storage schema、固件回退和擦除策略；
- 文档、测试证据、候选 commit、固件 hash 和发布签字一致。

### 验收矩阵

| 维度 | 最低验收内容 | 失败时结论 |
| --- | --- | --- |
| Codec/Wire | golden、全字段负向、长度/版本错配、混合版本 | 不得打开生产 v4 |
| Authority | Lease 到期 RX-first/TX-first、Joint 双 quorum、Fence 不可逆 | 不得发送权威帧 |
| Persistence | 同步/异步、重入、双槽、每个掉电点、升级/降级 | 不得承诺 persist-before-promise |
| Network | Join/leave、分区/合并、Head/Backup 故障、多跳丢包 | 不得宣称自动恢复完成 |
| Bearer | UART/CAN/Wi-Fi/USB 的 MTU、拥塞、断链、重连 | 不得宣称统一承载已实机覆盖 |
| Resource | Flash/RAM/Stack/CPU/功耗，满载与长稳 | 不得形成产品容量上限 |
| Security | 密钥生命周期、ACL、重放、升级、调试接口 | 不得用于安全边界产品 |

测试必须同时有正向收敛和负向零副作用证据。例如“掉电后还能启动”不够，还要证明未 durable 的 Vote 没有在掉电前发送 ACK；“多节点最后有 Head”也不够，还要证明任何时刻没有两个有效 Authority。

## 回滚原则

回滚不仅是刷回旧固件。必须先判断 Wire、API、ABI 和 Storage 四类兼容性；若旧固件无法理解新 Record 或新身份历史，应停止入网、备份诊断信息，并按发布包规定迁移或擦除持久化介质。

### 四类兼容性判断

1. **Wire**：旧节点是否会严格拒绝新长度/Type；混跑是否可能产生错误角色解释；
2. **API/ABI**：应用是否用位置初始化公共结构，结构大小/字段变化是否需要重编译；
3. **Storage**：旧固件能否识别新 schema、Tombstone 和 boot incarnation；
4. **身份历史**：回滚后是否会复用已退休 Cluster ID、Term、Sequence 或密钥 nonce 域。

### 推荐回滚流程

```text
发现候选故障
  → Fence Authority，停止产生新 promise
  → 导出版本、Epoch、Config、Record generation 和错误计数
  → 判断旧固件是否兼容当前 Storage/Wire
      ├─兼容：刷回已签名旧固件，验证只读 load，再允许入网
      └─不兼容：执行发布包指定迁移或受控擦除
  → 以新 incarnation/明确身份重新加入
  → 完成网络一致性与业务回归
```

不得在不知道 Record schema 的情况下直接量产擦除，也不得让旧固件加载它无法理解的记录后继续广播 Authority。

### 回滚包应包含

- 上一稳定固件、bootloader/分区表及 hash；
- Storage 检测、导出、迁移/擦除工具；
- 对应 Wire 混跑说明与最低可回滚版本；
- 恢复接线、串口日志命令和验收脚本；
- 失败时让节点保持离网/Fenced 的救援固件。

## 禁止表述

在上述门禁完成前，不得使用“Cluster 已生产完成”“掉电安全已实机验证”“v4 已兼容所有节点”或“万级规模已验证”等结论。

允许的准确表述应带范围，例如：“Wire v4 Codec 在 Host 受限测试中通过 40 B golden/negative tests，encoder 默认关闭，尚未接入生产 FSM”；“Persistence 软件模型通过同步/异步回归，真实 Flash 撕裂写尚未验收”。
