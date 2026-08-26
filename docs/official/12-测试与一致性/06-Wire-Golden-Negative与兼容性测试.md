# Wire Golden、Negative 与兼容性测试

> 文档级别：`NORMATIVE`
> 实现状态：`CURRENT（测试规范）`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：CMake、tests、tools、results 与审计门禁
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：实机证据按具体报告；规范本身不代表已验收

Golden 向量冻结精确字节、长度和 CRC。Negative fixture 独立定义每个 Type 的允许 Role/flags、必填、零尾、Node ID、serial、range/count，逐规则破坏并验证：raw decode、strict dispatch、semantic parser 全部拒绝且 output 不写回。

兼容测试覆盖版本/长度混搭、截断、超长、未知 class、保留位和 encoder 默认关闭。双向 round-trip 不能替代字段顺序断言，因为 parser/builder 同时写错仍可能互相抵消。

## Golden向量

每条向量固定：语义字段、完整hex bytes、精确长度、CRC/tag输入和预期decode。字段使用互不相同值，便于发现字节序/错位。向量需由独立脚本或人工第二实现重算关键CRC。

编码测试做`semantic→raw→exact bytes`；解码测试直接输入冻结bytes并逐字段检查，不让两者互相生成fixture。

## Negative规则表

每个Type/Class独立列出：允许sender role、flags mask、P0..Pn字段类别、必非零、必须零、Node ID合法域、serial阈值、count/range和组合约束。测试逐规则只破坏一项，并执行：

```text
raw decode拒绝
strict dispatcher拒绝
semantic parser拒绝
output完整不写回
状态/pending slot不改变
```

规则表不能从被测validator反射生成，否则实现漏规则时测试也漏。

## 长度与padding

Core W0～W3测试peek profile/encoded size、截断、超长和Carrier padding。CAN-FD 57→64 B明确检查7个零padding并拒绝任一非零。Cluster v3只接受32 B，v4只接受40 B。

## 版本隔离

输入矩阵包括`v3+32`成功、`v4+40`在受限codec成功、`v3+40/v4+32`拒绝、未知版本拒绝，无降级尝试。v4 encoder默认关闭时，调用返回错误且完整40 B output哨兵不变。

## Pending/分片边界

证书/Carrier/Transfer重组还要测source/key/config admission、deadline边界、槽满和不匹配不清合法slot。例如`now==deadline`收到伪造fragment时，应先拒绝且保持slot，只有owner timer显式expire。

## 混合版本

兼容不只是codec可解。还要验证能力协商、角色/Authority、旧节点是否non-voting、发送端选择格式和升级顺序。Host dual stack只证明分派隔离，不等于生产网络允许v3/v4混跑。
