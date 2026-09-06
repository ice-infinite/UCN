# 审计与整改

> `AUDIT ARCHIVE / NOT CURRENT BY DEFAULT`：每份报告只绑定其审计快照；当前边界以[官方文档](../official/README.md)和最新源码为准。

这里保存跨 Core/Extended 的缺陷审计、代码—文档一致性检查和整改建议。

审计报告描述的是对应提交或工作树快照。问题关闭状态必须以当前源码、任务表和最新复审证据共同确认，不能只看旧报告标题。

当前 v6-only 软件基线的最新全量内部审计见
[UCN v6 全量代码审计与整改报告（2026-09-06）](UCN_V6_全量代码审计与整改报告_2026-09-06.md)。该报告只证明其绑定工作树的软件门禁，
不替代真实 Flash/掉电、生产密码、MCU ISR/DMA、硬件时间戳、多板性能和长稳证据。

针对其后外审发现的 Authority/JOIN Lease、Cluster live quorum/Handover proof、缓冲区重叠及
安装发布面问题，见[UCN v6 外审 Lease、Quorum、缓冲区与发布面整改报告（2026-09-06）](UCN_V6_外审Lease_Quorum_缓冲区与发布面整改报告_2026-09-06.md)。

针对 Cluster durable 中间态、Realtime 真实四事件、标准 Runtime Owner 和全部公开初始化
别名保护的后续闭环，见[UCN v6 最终架构闭环整改报告（2026-09-06）](UCN_V6_最终架构闭环整改报告_2026-09-06.md)。

针对其后外审发现的 Cluster 历史票复活、公共调用链大栈、AAD Payload 别名、Runtime 四事件
所有权和回调内 Link reopen，见[UCN v6 外审 AUD-01～05 整改与自审报告（2026-09-06）](UCN_V6_外审AUD_01_05整改与自审报告_2026-09-06.md)。该报告是当前最新软件整改快照；外审签字前仍保持 `AUDIT HOLD`。
