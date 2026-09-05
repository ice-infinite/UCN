# UCN v6 源码阅读指南

本目录帮助人工从架构到函数逐层阅读当前 v6。推荐顺序：

1. [核心阅读顺序](01-v6核心阅读顺序.md)
2. [Nano Profile](02-Nano.md)
3. [Lite Profile](03-Lite.md)
4. [Full Profile](04-Full.md)
5. [可选模块与平台](05-可选模块与平台.md)
6. [公共函数签名索引](06-公共函数签名索引.md)

Profile 不对应三套源码：三者编译同一实现，差异来自 `ucn_v6_config.h` 的容量和 Feature
Manifest。审计时先理解共同状态机，再检查不同容量是否触发窄索引、边界和不对称行为。
