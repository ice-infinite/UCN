# Lite Profile 阅读与审计

Lite 是常规 MCU 默认候选：Binding 8、Session 4、RouteSet 8、每 Route 3 Path、QoS Flow 16、
Transfer TX/RX 各 2、Adapter Link 4、Cluster Member 8，最大发送 2 KiB。

Lite 与 Nano/Full 共用所有状态机。审计重点是中等容量下多个 slot 的公平性和错绑：多个 Q1
Latest key、两个并发 Transfer、多个 Candidate、不同 Binding/Session 的迟到 ACK，以及
四条 Link 的 completion/reopen 交叉。

公共函数集合与 Full 基础模块一致；容量不通过 Stub 隐藏。阅读 `ucn_v6_config.h` 中
`UCN_V6_PROFILE_DEFAULT()` 展开值，再用公共函数索引对照每个 Owner 的 init、mutation、copy
view 和 expire/invalidate 入口。

Lite 自审至少运行 Debug 全量 CTest、Release 或 MinSizeRel 产品配置、Feature-Off 组合和安装
消费测试。目标板仍需用实际链接 map 验证 Storage 上界是否适合 RAM。
