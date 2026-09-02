# UCN Core 源码阅读指南

本目录服务于两种目标：

1. **快速理解**：先从架构和三条主调用链建立整体认识，再进入具体函数；
2. **人工审计**：把公共合同、实现、测试和失败路径放在同一条阅读线上，避免只看“正常能跑”的代码。

> 事实边界：本目录解释的是当前 `UCN 5.0.0 / Core Wire v5` 源码。准确签名、条件编译和行为始终以 `CMakeLists.txt`、`include/ucn/`、`src/` 为准。Cluster 是建立在 Core 之上的可选控制体系，不属于本轮 Core 阅读主线。

## 源码中的双语函数注释

非 Cluster 核心的每个实际函数定义前均有两行用途说明：`EN:` 给出英文职责，`中文：` 给出对应中文职责。覆盖 Core、Node、Routing、Transport、Adapter/Source、Port、Service、Transfer，以及非 Cluster 公共头中的 `static inline` 函数；函数指针类型、宏和 Cluster 函数不计入本轮覆盖范围。

修改 Core 函数后可执行以下只读门禁，检查是否存在漏写注释的新函数：

```powershell
python tools/manage_core_bilingual_comments.py
```

预期结果为 `missing=0`。函数签名或源码位置发生变化后，再执行：

```powershell
python tools/generate_core_reading_function_index.py
python tools/generate_core_reading_function_index.py --check
```

双语注释用于建立阅读入口，参数所有权、上下文限制和失败语义仍以公共头合同、实现及对应测试为准。

## 推荐阅读顺序

| 顺序 | 文档 | 读完应得到什么 |
| --- | --- | --- |
| 1 | [00-阅读方法、边界与路线图](00-阅读方法、边界与路线图.md) | 知道哪些代码属于 Core，怎样同时看头文件、实现和测试 |
| 2 | [01-公共基础层架构与函数](01-公共基础层架构与函数.md) | 理解三个 Profile 共同使用的 Frame、Link、Adapter、Owner 和 Source |
| 3 | [02-Nano架构、函数与调用顺序](02-Nano架构、函数与调用顺序.md) | 用最小实现理解 Node 的基本骨架 |
| 4 | [03-Lite架构、函数与调用顺序](03-Lite架构、函数与调用顺序.md) | 理解动态发现、AODV-Lite 和 Security 如何进入 Node |
| 5 | [04-Full架构、函数与调用顺序](04-Full架构、函数与调用顺序.md) | 理解 Candidate、Path、Policy、负载均衡和高级诊断 |
| 6 | [05-三档差异与构建地图](05-三档差异与构建地图.md) | 分清原生实现、裁剪 Stub 和 Service 独立开关 |
| 7 | [06-发送、接收与Step完整调用链](06-发送、接收与Step完整调用链.md) | 能沿真实函数调用顺序追踪一帧数据和一次维护循环 |
| 8 | [07-人工审计指南与记录模板](07-人工审计指南与记录模板.md) | 能把边界、状态机、回绕、并发和失败不写回转成审计用例 |
| 查表 | [08-函数签名与源码位置索引](08-函数签名与源码位置索引.md) | 按源文件查当前函数、参数、可见性和行号 |

## 三个 Profile 文档怎样使用

每个 Profile 文档都按相同结构组织：

```text
档次定位
  → 实际编译源文件
  → 对象与状态架构
  → 启动顺序
  → 公共函数分组和参数
  → TX / RX / Step 调用链
  → 内部 helper 分区
  → 不支持能力与 Stub
  → 推荐源码阅读顺序
  → 审计关注点和对应测试
```

不要把“公共头中存在声明”理解成当前 Profile 原生支持该能力：为了保持公共 API 可链接，低档 Profile 对被裁剪能力提供明确失败的 Stub。调用者必须处理 `UCN_ERR_CONFIG`、`NULL` 或 `false`，不能把它们当成静默降级成功。

## 阅读时同时打开的三个窗口

建议每次只审一个函数族，同时打开：

```text
窗口 A：include/ucn/*.h    —— 合同、参数、所有权、失败语义
窗口 B：src/**/*.c        —— 真实实现和内部调用
窗口 C：tests/test_*.c    —— 当前测试问过什么、没有问什么
```

机器导航可辅助使用 [`docs/calltree/`](../calltree/README.md)，但调用树不能替代源码；它描述主关系，不会展开每一个条件分支。

## 本目录不包含什么

- 不把 Cluster v4 实验组件写成 Core 已放行能力；
- 不把 Host CTest 写成 MCU、Flash 掉电或无线环境实测；
- 不复制所有结构体字段的定义，字段布局应直接对照公共头；
- 不建议从 `ucn_node.c` 第一行顺序读到结尾。Full 文件很大，应按公共入口和调用链分段阅读。
