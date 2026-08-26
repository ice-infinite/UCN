# 配置与资源

> 文档级别：`REFERENCE`
> 实现状态：`CURRENT`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：ucn_config.h、ucn_profile.h、CMake 与资源门禁
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：Host 资源可用；目标 MCU 资源需按产品配置实测

UCN 采用“公共默认值 + 可选产品配置头 + 编译期合同”的静态配置方式。产品应先选 Profile 和功能，再按真实节点规模、并发和 RAM 调整表容量；不要直接修改库内默认值作为唯一产品配置。

配置不是“让代码能编译”的附属项，而是产品协议合同的一部分。它同时决定节点具备哪些能力、能维护多少状态、最坏时延/控制流量以及公共对象 ABI。两个使用不同配置的编译单元，即使来自同一源码版本，也可能不能安全地共享对象。

## 配置形成流程

```text
业务/硬件需求
  → 选择 Profile 与模块
  → 计算 Link/Neighbor/Route/Queue/Transfer/Cluster 容量
  → 写产品配置头和 CMake preset
  → 配置合同 + 多 Profile 构建
  → Host 资源/规模测试
  → 目标 MCU RAM/Stack/CPU/功耗验证
  → 冻结配置、固件 hash 与回滚策略
```

## 阅读顺序

1. [全局配置、回退值与覆盖优先级](01-全局配置、回退值与覆盖优先级.md)
2. [Nano/Lite/Full Profile 功能矩阵](02-Nano-Lite-Full-Profile功能矩阵.md)
3. [CMake 选项、静态库与 Feature 开关](03-CMake选项、静态库与Feature开关.md)
4. [固定表、队列、槽位与容量参数](04-固定表、队列、槽位与容量参数.md)
5. [RAM、Flash、Stack 与动态分配边界](05-RAM-Flash-Stack与动态分配边界.md)

随后阅读：

6. [时间参数、维护周期与 Deadline 预算](06-时间参数、维护周期与Deadline预算.md)
7. [默认 Cost、速率与动态 Cost 调优](07-默认Cost、速率与动态Cost调优.md)
8. [产品裁剪与容量规划方法](08-产品裁剪与容量规划方法.md)
9. [配置合法性与编译失败排查](09-配置合法性与编译失败排查.md)

配置宏会改变公共对象布局。所有编译 UCN 公共头和源码的 translation unit 必须看到一致的 `UCN_PROFILE`、`UCN_FEATURE_SERVICE` 和产品容量宏。

## 事实边界

文档给出的默认值来自当前 fallback，不是所有产品推荐值；Host 资源数字只用于回归，不代表 ESP32/STM32；Conditional preset 需要目标驱动确认。产品报告必须同时写明 commit、Profile、配置头、编译器、优化级别和硬件，否则数字不可复现。
