# Full、Lite、Nano 与 Feature 矩阵

> 文档级别：`NORMATIVE`
> 实现状态：`CURRENT（测试规范）`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：CMake、tests、tools、results 与审计门禁
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：实机证据按具体报告；规范本身不代表已验收

至少构建：Full、Lite、Nano、Full+Service OFF、Core-only/不链接可选 archive，以及产品最小容量配置。

矩阵验证两件事：启用功能的真实行为；被裁剪 API 的可链接和明确失败。不能只证明 Full，再假设 Lite/Nano 通过预处理。

规模测试要按容量选择可实现拓扑；例如 Link 上限不足时跳过不合法三叉树，或改用 line/binary tree，而不是把预期配置失败当协议回归。

## 最低矩阵

| Build | 重点 |
| --- | --- |
| Full + Service ON | 全部Core/Path/Policy/Service能力 |
| Full + Service OFF | 正交Feature真正裁剪且Core仍工作 |
| Lite | Dynamic Mesh/Security，Path/Policy明确失败 |
| Nano | 固定Core，所有公开裁剪API可链接/stub失败 |
| Core-only | 不生成/链接Cluster等可选Archive |
| 低容量产品 | 小Frame、少Link/Route/Queue的边界 |
| 实验ON | 仅对应M任务/Codec定向测试 |
| 默认OFF | 生产archive无实验符号/宏 |

## 测试同一个行为与不同预期

例如`ucn_node_install_local_path_capable()`：Full应真实安装并验证；Lite/Nano应符号存在且返回`UCN_ERR_CONFIG`，对象不变。若测试在低Profile直接跳过，就无法发现公共头声明但library漏stub。

## 配置传播

测试target与被测library必须看到相同`UCN_PROFILE`、产品头和容量宏。否则测试中的`sizeof`与library不一致。可用compile_commands和运行时config signature/sizeof断言检查。

## 低容量拓扑

拓扑生成器应读取容量：`MAX_LINKS=3`时line或binary tree，不能默认三叉树内部节点需要4Link。若某测试语义必须4Link，应在CTest注册时明确skip并说明，而不是运行后稳定失败。

## Profile混合网络

Build Profile不是Wire Class。混合测试还应让Nano接收共同Wire范围的普通Endpoint数据，并拒绝未编译高级语义；Full不能向低节点发送超Capability格式后期待对端自动理解。

## 报告

每个矩阵项记录配置摘要、发现test数、通过/失败、对象/text/stack和skip原因。不能把不同Profile的测试数量强求相同；应比较合同覆盖是否完整。
