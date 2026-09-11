# 建议方案

这里保存尚未完全进入当前发布规格的长期目标、稳定化建议与能力边界。

采用建议前应先核对任务表和当前源码；建议文档中的“应该、计划、目标”不代表已经实现或实测。

## 当前建议

- [UCN v6 低开销统一 Wire：各 Contract 字段与运行机制详细设计](UCN_v6_低开销统一Wire各Contract字段与运行机制详细设计.md)：当前唯一的下一轮线上字段候选权威，定义 3 B Common Header、C0～C5、O0～O2、H0～H3、Realm 固定地址宽度、6-bit Hop Limit、Context 和实际开销。语义与逻辑交叉已内部自审，精确 Registry/Golden 仍需外审冻结；在此之前不得替换当前 Codec。
- [UCN v6 面向用户意图的自动传输策略与配置接口详细设计](UCN_v6_面向用户意图的自动传输策略与配置接口详细设计.md)：唯一负责用户 Intent、Policy 合并、自动选型、显式收紧覆盖、Request/Attempt/Buffer 和 Completion 用户语义；普通用户无需选择 C/H/O 组合。
- [UCN v6 逻辑模型、伪代码与状态图](UCN_v6_逻辑模型与伪代码/README.md)：包含 00～14 完整合同/对抗附录与 15～26 简化实现主线；后者补齐了基础通信、SoftRoute、Security、Reliable/Transfer、Service/QoS、Realtime、Group、Cluster、Persistence、Capability Resolver、Advanced Flow、统一 Public API/Internal SPI，以及全局不变量/模块对接登记表。当前为 `SELF-REVIEWED / EXTERNAL REVIEW REQUIRED`，不表示源码已按此实现。
- [UCN v6 简化文档收口与全体自审报告](../../09-审计与整改/UCN_V6_简化文档收口与全体自审报告_2026-09-08.md)：记录 34 份简化体系文档与 1 份当前 Core Wire 事实边界的逐篇结论、跨文档冲突、整改结果和机械门禁；它是内部自审证据，不替代独立外审。
- [UCN v6 可裁剪模块边界、依赖、资源与静态装配详细设计](UCN_v6_可裁剪模块边界依赖资源与静态装配详细设计.md)：定义最小通信内核、Composition/Profile/Policy/Intent 四维配置、Identity/Security/Network/Flow/Transport/Service/Operation/QoS/Realtime/Group/Cluster 等模块边界、唯一状态所有权、依赖与失效传播、独立 Storage、静态装配、用户 Intent 自动解析、标准产品组合、分阶段重构和 Flash/RAM/Stack/CPU/Wire 五本验收账。已完成与简化主线的内部自审，仍需外部复审，不表示现有源码已完成全部裁剪。
- [UCN v6 最终协议架构与破坏性重构 RFC](UCN_v6_最终协议架构与破坏性重构_RFC.md)：拥有 MCU-first、身份、Owner、安全、持久化和发布门禁等顶层不变量。其原第 6 章的 9 B 前缀候选已明确降为历史记录，不能再与低开销 C0～C5 混合作为字段来源。V6A-01～V6A-25 的外审签字只属于当时的历史哈希；2026-09-08 的低开销 Wire、模块化、用户 Intent 和逻辑模型修订已完成内部交叉自审，当前为 `SELF-REVIEWED / EXTERNAL REVIEW REQUIRED`。低开销精确 Registry/Golden、统一 API ABI 和整套文档外审冻结前，不授权替换生产 Codec 或宣称源码已完成该架构。
- [UCN v6 Core Wire 精确格式 RFC](UCN_v6_Core_Wire_精确格式_RFC.md)：精确描述当前 V6-03/V6-15 已实现的 Contract 1 和现有回归，作为迁移前代码事实保留；它已被标记为下一轮低开销重构的被替代方案，不是 C0～C5 的目标字段来源。
- [UCN v6 V6-00 最终架构 RFC 自审报告](UCN_v6_V6-00_最终架构RFC自审报告.md)：记录 V6A-01～25 历史基线的五轮整改与外部签字。它是可追溯证据，不是 2026-09-07 后当前修订版的外审结论。
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
