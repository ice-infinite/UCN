# UCN v6 源码参考

当前可机械核对的入口：

- [公共函数签名索引](../源码阅读指南/06-公共函数签名索引.md)
- [调用关系入口](../calltree/README.md)
- [`include/ucn/v6/`](../../include/ucn/v6/ucn_v6_config.h)：公共配置和 API
- [`src/v6/`](../../src/v6/config/ucn_v6_config.c)：唯一当前运行时
- [`tests/v6/`](../../tests/v6/test_v6_config_contract.c)：可执行合同

旧 v5 生成表和架构图已移入 `docs/archive/v5-user-facing-20260905/reference/`。当前语义先看
[官方文档](../official/README.md)，精确签名和条件编译以公共头为最终依据。
