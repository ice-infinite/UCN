# Nano Profile 阅读与审计

Nano 编译共同 v6 核心，但默认 Binding 4、Session 2、RouteSet 4、每 Route 2 Path、QoS Flow 4、
Transfer TX/RX 各 1、Adapter Link 2、Cluster Member 4，最大发送 256 B。

阅读时先确认 `UCN_V6_PROFILE=NANO` 进入 Manifest/Layout Hash，再检查所有循环使用配置上限，
索引类型不会在边界回绕。Nano 容量小，最容易触发表满、单 slot 替换、并发事务冲突和 Feature
关闭路径，是审计固定内存语义的首选。

Nano 仍能调用基础 Config/Identity/Wire/Message/Owner/Security/Capability/Route/QoS/Transfer
全部公共函数；差异是部分创建/发送操作会因容量或 Message Class 返回错误。Realtime、Cluster、
Adapter 的函数是否存在由 Feature 开关决定，不由 Nano 身份自动删除。

建议测试顺序：config contract → wire → security → capability → route → qos → transfer →
optional modules → umbrella/install consumer。特别验证 Full 发来的合法 A3 控制帧可解析，而
超出 Nano 256 B 主动发送上限会明确拒绝、不会截断。
