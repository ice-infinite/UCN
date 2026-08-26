# UCN 文档中心

当前实现的正式说明统一从 [official/](official/README.md) 进入。`reference/` 保存可核对的源码参考，`evidence/` 保存测试证据索引，`experimental/` 保存默认未启用组件的边界，`archive/` 管理旧建议和阶段材料。

> 旧的 `01-入门与使用`～`11-历史与复盘` 是迁移期历史主题树，不再作为当前事实入口；内容将逐步建立替代关系并迁入 `archive/`。源码、公共头和构建配置优先于任何旧文档。

## 正式入口

1. [官方文档](official/README.md)
2. [源码参考与图表](reference/README.md)
3. [验证证据](evidence/README.md)
4. [实验组件](experimental/README.md)
5. [历史归档](archive/README.md)
6. [项目任务与操作记录](00-项目管理/README.md)

## 独立机器资料

- [调用树](calltree/README.md)：面向代码导航的 YAML 调用树。
- [`results/`](results/)：规模、资源和测试生成结果；由 `evidence/` 建立 commit 级索引，机器路径保持不变。

## 文档状态规则

- “目标、建议、设计”不等于代码已经实现。
- “软件测试通过”不等于真实 MCU、Flash 掉电或无线环境已经验证。
- Cluster 当前边界先看 [官方 Cluster 文档](official/07-Cluster簇/README.md)，里程碑细节再回到历史任务表核对。
- Core 实施状态以 [UCN Core 任务表](00-项目管理/00-任务表.md) 和 [项目操作记录](00-项目管理/01-项目操作记录.md) 为准。
