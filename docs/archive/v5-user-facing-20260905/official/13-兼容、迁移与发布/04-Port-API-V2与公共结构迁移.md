# Port API v2 与公共结构迁移

> 文档级别：`NORMATIVE`
> 实现状态：`CURRENT（规则）；RELEASE NO-GO`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：版本宏、Wire/API/Storage 合同、CMake 与发布门禁
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：完整发布实机门禁未完成

Port API v2 将 task/ISR 临界区和通知语义显式分开，是预发布破坏性升级。旧位置初始化、旧结构大小和旧 archive 不保证 ABI 兼容。

迁移时改为具名初始化、实现 ISR-safe 回调、全量重编译库与应用，并运行公共 API 链接测试。不要把新头文件与旧静态库混用。

## v2变化的原因

旧Port把task和ISR共享临界区抽象成无token enter/exit，无法正确表达FreeRTOS等ISR mask保存/恢复。v2显式提供`enter_critical_from_isr()`返回token，以及对应exit；这样同一Adapter Queue可在合法平台合同下安全使用。

这主要影响Port/Adapter并发边界，不改变Core Wire字节。

## 迁移步骤

1. 找出所有`ucn_port_ops_t`初始化；
2. 改为designated fields，不再六字段位置初始化；
3. 实现task critical与ISR critical两套回调；
4. 检查`now_ms/random/counter`语义；
5. 更新Port wrapper/Owner/Event Runtime；
6. 全量重编译所有静态库与应用；
7. 运行task/ISR并发、公共头和各Profile链接测试。

```c
static const ucn_port_ops_t ops = {
    .api_version = UCN_PORT_OPS_API_VERSION,
    .struct_size = sizeof(ucn_port_ops_t),
    .now_ms = port_now,
    .random_bytes = port_random,
    .load_counter = port_load,
    .store_counter = port_store,
    .enter_critical = port_enter,
    .exit_critical = port_exit,
    .enter_critical_from_isr = port_enter_isr,
    .exit_critical_from_isr = port_exit_isr,
};
```

字段名/版本以当前头为准；示例强调具名与完整初始化。

## 公共结构规则

容量宏、Feature和追加字段会改变ABI。当前预发布阶段允许破坏，但应通过opaque storage、size/version字段和扩展config减少后续风险。应用不得把公共对象落盘或跨固件共享内存。

## 失败症状

- `-Wmissing-field-initializers -Werror`：仍用旧位置初始化；
- ISR随机死锁/中断mask错误：task API误用于ISR；
- 只在某Profileundefined reference：缺stub或archive；
- sizeof不一致：产品宏传播不同；
- 新头+旧lib运行崩溃：ABI混用。

## 回滚

Port API回滚要求应用和库整体回滚。Wire可能仍兼容，但不能只替换一个`.a`；构建manifest应防止这种组合。
