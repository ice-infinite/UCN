# API 参考

> 文档级别：`REFERENCE`
> 实现状态：`CURRENT`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：include/ucn 公共头、链接目标与公共 API 测试
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：API 合同不等同于各平台实机驱动完成

本目录解释 `include/ucn/` 公共接口的语义、所有权、Owner 规则和失败行为。准确函数签名、枚举和宏以当前公共头为准；后续由 `reference/generated/` 生成符号表，避免手工复制大量声明产生漂移。

所有 API 默认遵循：调用者提供静态存储、单一 Protocol Owner 推进状态、ISR 只投递事件/字节、失败时不部分写 output。可选组件是否可用还取决于 Profile、Feature 和链接 target。

## 如何使用本目录

本目录不是自动生成的函数签名清单，而是解释“为什么调用、按什么顺序、成功到哪一层、失败后对象如何”。准确声明仍应点击对应 `include/ucn/` 头文件；两者冲突时源码是事实源，并应修正文档。

推荐先阅读：

1. [Core/Types/Config/Time/Error](01-Core-Types-Config-Time-Error.md)
2. [Node 与 Storage](03-Node与Storage-API.md)
3. [API 所有权、返回值与失败写回](15-API所有权、返回值与失败写回规则.md)

然后按功能选择 Frame、Link、Route、Adapter/Port、Service、Transfer、Security、Cluster 或 Federation。

## 通用调用链

```text
静态 storage/config/ops
  → validate/init
  → 注册 Link/Endpoint/Source/Binding
  → 启动 Driver 与唯一 Owner
  → ISR 仅 push/signal
  → Owner receive/step/service
  → 应用通过 Endpoint/Service/Transfer completion 观察结果
  → 停止新流量并有序关闭
```

## 读 API 时要回答的十个问题

1. 哪个 Profile/Feature/target 才有实现？
2. 对象由谁分配、内容由谁管理？
3. config/ops/buffer 是复制还是借用？
4. 哪个上下文允许调用？
5. 是否可能同步触发 callback？
6. `UCN_OK` 只到接受、入队、送达还是执行？
7. `NO_SPACE/LINK_DOWN/PENDING` 如何恢复？
8. 失败时 output/状态是否保证不写回？
9. 容量和时间上限是什么？
10. 该 API 是默认产品路径还是实验 Archive？

只有这些问题都明确，才算完成一次可靠集成。
