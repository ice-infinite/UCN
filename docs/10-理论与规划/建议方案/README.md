# 建议方案

这里保存尚未完全进入当前发布规格的长期目标、稳定化建议与能力边界。

采用建议前应先核对任务表和当前源码；建议文档中的“应该、计划、目标”不代表已经实现或实测。

## 当前建议

- [UCN v6 最终协议架构与破坏性重构 RFC](UCN_v6_最终协议架构与破坏性重构_RFC.md)：在协议尚未正式发布、无需兼容 v4/v5 测试固件的前提下，重新冻结 Identity/双向认证 Bootstrap/Address Authority 与 Binding、Core Wire/AAD/Peer-Group-E2E Key Selector、交付与交互、生产安全、Capability/Path Payload Budget、RouteSet、Transfer、QoS、C99 Storage、Realtime、Cluster 和发布门禁；V6A-01～V6A-25 全部外审通过，`V6-00 = DONE / EXTERNAL FINAL REVIEW GO`（仅架构 RFC/纯文档范围）。当前 V6-01～13 软件实现已完成，V6-14/15 完成可执行软件范围并等待统一外审；TSan、目标硬件、Flash 掉电、性能长稳和生产密码 Provider 仍为发布阻断项。
- [UCN v6 V6-00 最终架构 RFC 自审报告](UCN_v6_V6-00_最终架构RFC自审报告.md)：逐项复核当前 v5 事实、v6 顶层决策、Wire 候选长度、安全边界、跨模块依赖、Compatibility Removal Manifest 和 V6-00～V6-15 实施顺序，并记录 V6A-01～25 五轮整改与最终外部签字；结论为 V6-00 纯文档范围终审 GO。
- [UCN FPGA 硬件转发节点实施方案](UCN_FPGA硬件转发节点实施方案.md)：规定未来使用 FPGA 作为可选高速转发、汇聚或骨干节点时的软硬件边界、数据流水线、控制接口、Route/Path 表、QoS、安全、故障回退、实施阶段与验证门禁。当前仅为设计建议，尚无 RTL 和实机结论。
- [UCN 可选实时元数据与分布式时间同步详细设计方案](UCN_可选实时元数据与分布式时间同步详细设计方案.md)：按 Endpoint/业务流定义 `NONE/LOCAL_STAMP/SYNCED_STAMP/DEADLINE`，以普通消息零额外时间 Wire 字节、Timed 消息候选 16 B 端到端 Envelope 起步，再分阶段建设 Time Domain、硬件时间戳、同步服务和可选 Hop-aware Deadline。RT-A01～A11 已完成整改、第四轮全体自审与受限软件范围外部复审；生产接线和实机继续 HOLD。
- [UCN Realtime Metadata v1 编解码 RFC](UCN_Realtime_Metadata_v1_RFC.md)：冻结供默认不链接的 RT-01 实验 Codec 使用的 16 B 布局、合法组合、uncertainty 算法、无写回错误语义和三条 Golden Vector；不授权生产 RX/TX、Domain FSM 或四报文同步。
- [UCN RT-01 Realtime Metadata Codec 自审报告](UCN_RT01_Realtime_Metadata_Codec_自审报告.md)：记录首个实时模块的源码边界、合同映射、全软件矩阵、静态资源与尚未验证项。
- [UCN RT-02 Endpoint 时间策略与双门禁自审报告](UCN_RT02_Endpoint时间策略与双门禁_自审报告.md)：记录按 Endpoint 选择时间模式、发送构建与接收/执行双新鲜度门禁。
- [UCN RT-03 Time Domain FSM 自审报告](UCN_RT03_Time_Domain_FSM_自审报告.md)：记录固定内存时钟换算、有效样本窗口、LOCKED/HOLDOVER/FAULT 与 generation 重绑。
- [UCN RT-04 Timed Link 与原子事件队列自审报告](UCN_RT04_Timed_Link与原子事件队列_自审报告.md)：记录 Driver 时间戳扩展、四类本地事件键、任务/ISR 队列及重入门。
- [UCN RT-05 四报文同步与 Path 准入自审报告](UCN_RT05_四报文同步与Path准入_自审报告.md)：记录四报文语义、固定 Path/动态 Route 分流、重复乱序与超时处理。
- [UCN RT-06 能力租约与时间权威防回退自审报告](UCN_RT06_能力租约与时间权威防回退_自审报告.md)：记录 capability lease 和 STATIC_MASTER generation 双持久化证明。
- [UCN RT-07 多域漂移与故障集成模拟自审报告](UCN_RT07_多域漂移与故障集成模拟_自审报告.md)：记录 10 节点、双域漂移/丢包/重放/切路/重启/Holdover 的确定性软件模拟与交叉整改。
- [UCN 实时模块 RT-01～RT-07 全体自审报告](UCN_Realtime_Module_RT01_RT07_全体自审报告.md)：汇总七个默认关闭组件、RT-A01～A11 整改、跨 Profile/Sanitizer/Analyzer/TSan/MSVC 门禁、静态资源与产品隔离；当前已获受限实验软件范围外部复审 GO，生产和实机边界仍未放行。
