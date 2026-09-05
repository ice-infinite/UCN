# Full Profile 阅读与审计

Full 默认 Binding 16、Session 8、RouteSet 16、每 Route 4 Path、QoS Flow 32、Transfer TX/RX
各 4、Adapter Link 8、Cluster Member 16，最大发送 8 KiB。它适合高容量节点和软件验证矩阵，
但“Full”不授予额外协议权限。

Full 最适合审计组合爆炸：A0～A3、多个安全上下文、多 Path Pipeline、32 个 Flow、4 个并发
Transfer、Realtime 双 Domain、Cluster Joint/Takeover/Handover 和 8 Link Adapter。检查所有
乘法/加法在分配和 Wire 长度前 checked，避免大容量才出现整数溢出。

阅读时按核心指南跟踪一次端到端流程，然后在测试中找 1k/10k Cluster scale、4096 Wire fuzz、
QoS 饥饿、Transfer 乱序和 Adapter 并发。Full Host Storage 数字仅是公共上界；产品可不实例化
不用的 Owner，也应评估是否需要自定义容量而不是盲目接受最大默认值。
