# UCN v6 调用关系入口

v5 YAML 调用树已归档到 `docs/archive/v5-user-facing-20260905/calltree/`，不适用于当前源码。
v6 当前以以下材料共同完成调用导航：

1. [核心阅读顺序](../源码阅读指南/01-v6核心阅读顺序.md)：端到端模块顺序；
2. [公共函数签名索引](../源码阅读指南/06-公共函数签名索引.md)：当前 210 个公共函数签名；
3. `tests/v6/test_v6_*.c`：每个 Owner 的可执行调用顺序；
4. `src/v6/`：最终事实。

主链为：

```text
Driver/ISR
  -> ucn_v6_adapter_publish_rx()
  -> ucn_v6_protocol_owner_post()
  -> ucn_v6_protocol_owner_run()
  -> ucn_v6_wire_decode()
  -> ucn_v6_security_open_frame()
  -> Route/Transfer/Realtime/Cluster or application

application
  -> ucn_v6_message_validate()
  -> optional Journal/Realtime/Transfer
  -> ucn_v6_route_select()
  -> ucn_v6_qos_enqueue()/select_next()
  -> ucn_v6_adapter_enqueue_tx()/service_tx()
  -> Driver completion
```

当前不保留手写逐函数 YAML，避免函数调整后出现静默漂移；公共签名索引由脚本生成，调用
行为以测试和源码核对。后续如恢复机器调用图，生成器必须只扫描 `src/v6` 并纳入 CI 漂移门禁。
