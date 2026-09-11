# V6S-00 简化版实施合同冻结

> 状态：`SELF-REVIEW PASS / EXTERNAL REVIEW REQUIRED / AUDIT HOLD`
>
> 适用分支：`v6-simplified`
>
> 实现门禁：`IMPL-00` 必须等待 `V6S-00-08` 外部复审 GO。

## 1. 为什么先做合同冻结

简化版的目标不是把现有源码机械删短，而是在不丢失安全语义的前提下，让最小通信路径、
可选模块、固定资源和低开销 Wire 都只有一套明确解释。若先写代码再决定标量宽度、字段
offset、Generation 归属、Feature OFF 行为或 Persistence Envelope，后续必然出现二次搬迁、
双语义和“为了兼容刚写出的中间版本”而保留的冗余。

所以本目录先冻结实现必须服从的输入。这里的 `DONE` 只表示文档与合同经过本轮定向自审；
不表示源码已经实现，更不表示真实 Flash、密码 Provider、MCU、Bearer、性能或长稳已经验证。

## 2. 权威顺序

发现冲突时按以下顺序停止并裁决：

1. [V6 最终协议架构 RFC](../UCN_v6_最终协议架构与破坏性重构_RFC.md)中的 MCU-first、失败关闭和顶层安全不变量；
2. `V6S-00-08` 取得外部复审 GO 后，本目录才成为专项精确冻结合同；届时对标量、Wire、API、资源和 Persistence 的 byte/bit/数值问题，本目录是 RFC 顶层原则的规范化细化；
3. [全局不变量与模块对接登记表](../UCN_v6_逻辑模型与伪代码/26-全局不变量与模块对接登记表.md)；
4. 对应模块的逻辑模型、状态图和伪代码；
5. 当前完整 V6 源码仅作行为与测试参考，不能反向覆盖冻结合同。

RFC 中明确标记为历史/被替代的旧布局不与本目录构成平级候选。同一层级出现真实冲突时，
不允许实现者任选其一，必须登记问题并回到合同阶段处理。

## 3. 分项目录

| 子项 | 文档 | 唯一职责 |
| --- | --- | --- |
| V6S-00-00 | [00-冻结范围、权威关系与签字规则](00-冻结范围权威关系与签字规则.md) | 范围、基线、状态词和变更规则 |
| V6S-00-01 | [01-文档结构与机器门禁](01-文档结构与机器门禁.md) | 表格、链接、编号、围栏与检查器自测 |
| V6S-00-02 | [02-基础标量、Generation与Registry命名空间](02-基础标量Generation与Registry命名空间.md) | Wire 标量和代际所有权 |
| V6S-00-03 | [03-Core Wire精确布局与安全覆盖](03-Core-Wire精确布局与安全覆盖.md) | C0～C5 精确线上 ABI |
| V6S-00-04 | [04-Golden、Negative、Fuzz与属性测试合同](04-Golden-Negative-Fuzz与属性测试合同.md) | 与实现独立的测试 oracle |
| V6S-00-05 | [05-公共 API、ABI 与 Feature OFF 合同](05-公共API-ABI与Feature-OFF合同.md) | C99 公共面和物理裁剪 |
| V6S-00-06 | [06-Profile 容量公式与资源门禁](06-Profile容量公式与资源门禁.md) | Full/Lite/Nano 固定资源 |
| V6S-00-07 | [07-Persistence Foundation 共同合同](07-Persistence-Foundation共同合同.md) | 共同持久化基础，不含业务 Body |
| V6S-00-08 | [08-跨合同追踪与最终自审](08-跨合同追踪与最终自审.md) | 全体一致性、基线门禁和外审候选 |

尚未创建的文档表示对应子项尚未开始，不能从文件名或任务表推断其内容已经冻结。

## 4. 代码阶段的唯一入口

合同阶段完成后的实现顺序为：

```text
IMPL-00  CMake 物理解耦骨架 + 共同类型 + Owner/Coordinator
IMPL-01  静态身份/静态绑定/单帧通信/基础队列/Adapter Token
IMPL-02  统一 Persistence Foundation
IMPL-03  静态安全 Session
IMPL-04  Dynamic Admission + Capability
IMPL-05  Dynamic Route/Flow
IMPL-06  Reliable/Transfer
IMPL-07  Service/Operation/高级 QoS
IMPL-08  Realtime、Group、Cluster 按依赖矩阵独立装配
```

其中物理解耦不是 `IMPL-02` 之后的一次性工作：从 `IMPL-00` 开始，每个提交都必须证明
CMake target、Owner 依赖、Feature OFF 符号、公共头和 Storage 没有重新耦合。

## 5. 当前状态

```ini
V6S-00-00 = DONE / SELF-REVIEW PASS
V6S-00-01 = DONE / SELF-REVIEW PASS
V6S-00-02 = DONE / SELF-REVIEW PASS
V6S-00-03 = DONE / SELF-REVIEW PASS
V6S-00-04 = DONE / SELF-REVIEW PASS
V6S-00-05 = DONE / SELF-REVIEW PASS
V6S-00-06 = DONE / SELF-REVIEW PASS
V6S-00-07 = DONE / SELF-REVIEW PASS
V6S-00-08 = DONE / SELF-REVIEW PASS / EXTERNAL REVIEW REQUIRED / AUDIT HOLD
IMPL-00 = BLOCKED
```
