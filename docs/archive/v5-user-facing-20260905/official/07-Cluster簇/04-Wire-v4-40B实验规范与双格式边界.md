# Wire v4 40B 实验规范与双格式边界

> 文档级别：`NORMATIVE`
> 实现状态：`PARTIAL（Current 与默认关闭实验层级见正文）`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：Cluster 公共头、生产源码、独立 Archive、CMake 与测试
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：部分 ESP32-S3 实测；v4/掉电/完整多 Bearer 未发布验收

## 固定格式

Wire v4 固定 40 B，定义 Type 1～33，并把语义载荷表达为 P0～P5。严格 dispatcher 只接受精确的版本和长度组合：v3/32 B 与 v4/40 B 不允许降级解析或长度猜测。

## 已实现范围

- Raw codec 与严格结构校验；
- Type 1～33 semantic parser/builder；
- Snapshot、Takeover certificate fragment、Wire Offer；
- golden、逐字段 negative、固定 seed fuzz；
- Host 双格式隔离测试；
- Stream、CAN-FD、Classic CAN 对完整 40 B 的承载测试。

## 未放行范围

`UCN_CLUSTER_WIRE_V4_ENCODER_ENABLED` 默认是 0。v4 API 只在独立测试/实验 target 开启；默认 `ucn_cluster` 没有 v4 生产 RX/TX/FSM 调用，也没有因解析成功而授予角色或 Authority。

## Takeover Certificate pending

pending 重组使用固定、单 slot、无淘汰的 fail-closed 模型；key、source、frozen Config admission 和 deadline 必须匹配。结构合法但身份不匹配的片段不能清空现有 slot。

## 使用约束

Wire v4 文档已经冻结 Codec 范围，不代表 M05 整体解除 `AUDIT HOLD`。任何生产接线都必须重新审计 RX owner、quorum/CRC、FSM 副作用、encoder 开关和多版本网络策略。

## 严格双格式分派

| Version | 精确长度 | 结果 |
| --- | ---: | --- |
| v3 | 32 B | 进入 v3 decoder |
| v4 | 40 B | 进入 v4 raw/semantic decoder |
| v3/40、v4/32 | 任意 | 拒绝 |
| 未知版本、31/39/41 B | 任意 | 拒绝 |

禁止先按 v4 失败再试 v3，或只看前几个字段猜格式。失败 output 保持不变。

## Raw、Semantic 与 FSM 三层

Raw codec 只负责固定字段、角色、flags、P0～P5 合法域；Semantic parser 把 P 字段转换为 Type-specific 对象；FSM 才能根据当前 Phase/Epoch/Config/Authority 决定副作用。

当前只放行前两层受限测试，不存在“decode 成功自动成为 Head/Voter”。这种隔离防止尚未审计的网络输入触发生产 Authority。

## Type 20～33 和冻结注册表

扩展 Type 包含 Config、Handover、Rekey、证书片等语义。测试使用独立 RFC fixture，逐 Type 冻结允许 Role/flags 和 P0～P5 字段，而不是让 parser 和 builder 相互 round-trip 自证正确。

所有合法 Role、256 个 flags、每个字段非法值都通过 raw/dispatch/semantic 三层负向矩阵，并验证失败不写 output；固定 seed fuzz 只是补充，不能代替确定性规则表。

## Certificate pending 为什么单 Slot

Takeover Type 8 和 Type 33 分片需要重组证书。当前固定一个 slot、1000 ms、无淘汰：只有通过 source、proposed Epoch、Config set 等 receiver-side admission 的 Type 8 才能占用。错误 source/config 的片段即使在 deadline 边界也不能清空合法 slot；只有显式 expire/timer owner 释放。

单 slot 限制并发，但使 MCU RAM和 DoS 行为可预测。扩大前必须重新定义容量/淘汰安全。

## Encoder 默认关闭

`UCN_CLUSTER_WIRE_V4_ENCODER_ENABLED=1` 只出现在明确测试 target。默认库的 encoder 调用失败且完整 output 哨兵不写回。CMake 宏隔离是发布安全边界，不只是优化选项。

## Carrier 兼容证据

Stream、CAN-FD、Classic CAN 测试精确 `memcmp` 全部 40 B；CAN-FD 还验证 DLC padding 全零和非零拒绝。这证明公共 Carrier 能承载 40 B，不证明生产 Cluster 已接入该 Carrier 或实机时序合格。

## 生产接线前 checklist

- 独立审计 RX owner/调用上下文与重入；
- Semantic→FSM 每 Type 状态门；
- Voter order/quorum/certificate CRC；
- Persistence-before-promise；
- encoder enable 的产品配置和回滚；
- v3 legacy 角色限制；
- 真实 UART/CAN/Wi-Fi 40 B 互通、攻击和资源；
- 版本升级/降级和 Storage schema。
