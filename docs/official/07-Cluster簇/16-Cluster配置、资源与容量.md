# Cluster 配置、资源与容量

> 文档级别：`NORMATIVE`
> 实现状态：`PARTIAL（Current 与默认关闭实验层级见正文）`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：Cluster 公共头、生产源码、独立 Archive、CMake 与测试
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：部分 ESP32-S3 实测；v4/掉电/完整多 Bearer 未发布验收

## 固定资源模型

Cluster 使用编译期上限控制成员、voter、snapshot、locator、事务和 pending slot。产品应从预计簇规模、故障并发和可用 RAM 反推上限，而不是把 Host 配置直接复制到小 MCU。

固定上限的目的，是让最坏 RAM、遍历时间和消息数量在编译时可审计。UCN 不在运行中通过堆扩容成员表；达到上限后必须返回明确的容量错误、拒绝新事务或延迟诊断请求，不能覆盖已有权威记录。

### 主要容量对象

| 对象 | 容量由什么决定 | 满载时必须如何处理 |
| --- | --- | --- |
| Membership | 每簇最多成员数 | 拒绝新成员或由上层触发分簇，不能静默覆盖 voter |
| Stable/Joint VoterSet | 最大 voting 成员数 | Config Prepare 失败，不产生 ACK |
| Backup Snapshot | 成员数、字段宽度、双缓冲 | BEGIN 前拒绝超容量 snapshot |
| Locator/Directory | 可缓存远端 identity 数 | 按明确策略拒绝或淘汰非权威旧项，不能破坏本簇状态 |
| Pending Certificate | 同时重组的证书事务数 | 固定槽满后 fail-closed，不抢占已通过 admission 的事务 |
| Config/Takeover/Handover/Rekey | 并发事务数 | 同域只允许受控单事务，冲突 txid 拒绝 |
| RX/Event Queue | 峰值中断与 Owner 消费速率 | 统计背压/丢弃；控制面不得无限占 RAM |

### 产品配置步骤

1. 给出目标 MCU 的可用 SRAM、每任务栈和中断缓冲预算；
2. 给出单簇预期/硬上限成员数、voter 数、Backup 数和 Federation 邻簇数；
3. 选择 Cluster 功能层级：Current v3、实验 v4 Codec、或未来经审计的生产 v4；
4. 由构建产物测量 `.bss/.data/.text`、对象 `sizeof` 和 `-fstack-usage`；
5. 加入至少 20%～30% 产品余量，并对满载拒绝路径做测试；
6. 若不满足预算，先减容量/关闭模块，再考虑更大 MCU，不能依赖“平时不会满”。

## 当前 Host 证据

M14 当前记录的 Host 观测值包括：

- `ucn_cluster_t`：1608 B；
- Cluster archive text：133559 B；
- 最大静态栈帧：1840 B；
- 动态分配：0。

这些数字绑定当前候选源码与 Host 工具链，只能用于回归趋势和上限审查，不能替代 ESP32/STM32 上的 map、task stack high-water、IRAM/DRAM/PSRAM 和运行峰值。

### 如何解释这些数字

- `ucn_cluster_t=1608 B` 是单个 Cluster 对象的 Host ABI 大小，不包含外部队列、Adapter、任务栈、持久化 Provider 缓冲和应用数据；
- archive text 是静态库相关代码段观测，不等于最终固件 Flash 增量，链接裁剪、LTO、Libc 与架构指令集都会改变结果；
- 最大静态栈帧来自编译器分析，是单函数 frame，不等于嵌套调用、中断和 RTOS task 的最坏总栈；
- “动态分配 0”表示 Cluster 实现不依赖堆，不代表集成它的驱动、RTOS 或应用没有堆分配。

目标板应至少记录：编译器/优化级别、链接 map、Cluster 开关、容量宏、空闲/峰值 heap、Owner task stack high-water、中断栈、每秒 step/RX 数及持续时间。缺少这些上下文的“占用多少字节”不能作为跨项目结论。

## 控制预算

HELLO、Advertise、Heartbeat、Snapshot、Lease、Recovery 和 Federation 都消耗控制预算。规模增大时优先限制广播域、静态表和每步处理预算；簇头性能不能只按节点数判断，还取决于消息频率、变化率和链路带宽。

### 预算组成

可将每条 Bearer 的控制占用估算为：

```text
control_bytes_per_second
  = Σ(message_wire_bytes × send_rate × fanout × retry_factor)
```

其中 `message_wire_bytes` 必须包含 UCN 帧、Carrier 封装和物理层开销；`fanout` 对广播/逐邻居发送影响很大；`retry_factor` 受链路丢包和拥塞影响。Snapshot 属于突发流量，应额外计算一次完整镜像的总字节和最大完成时间。

Owner 的 CPU 预算可分为：固定 step 扫描、每帧解码/验证、成员/Lease 遍历、持久化回调、诊断复制。实现应给每次 step 有界工作量，剩余事件留到下一轮，避免 Cluster 控制面占满 MCU。

### 优先级原则

1. 已经 durable 但尚未发送的 ACK/Commit continuation；
2. Lease/Authority 撤权与关键 deadline；
3. Join、Snapshot、Recovery 等状态推进；
4. Advertise、Federation、诊断等可延迟任务。

业务高负载时也不能饿死前两类；反过来，Cluster 控制面也必须受 token/budget 限制，不能让传感器或控制数据永久得不到发送机会。

## 容量结论

源码支持参数化容量和规模模拟，但当前不能给出“固定最多多少万节点”的产品承诺。最终容量必须以具体 Profile、每簇上限、分簇策略、Bearer、流量模型和硬件测量共同确定。

### 规模扩展的真实路径

UCN 的万级目标不是把所有节点装进一个成员表，而是通过多个有界 Cluster、Head/Federation、按需 Locator/Directory 和 Core 路由形成层次化网络。单簇大小决定局部 quorum 和恢复成本；簇数量决定 Federation/目录压力；跨簇业务流量决定隧道和 Bearer 负载。

因此至少要分别给出：

- 单簇最大成员/voter 与完成一次 Snapshot 的时间；
- 单 Head 可维护的邻簇/Locator 数；
- 网络总节点数、平均/最坏跳数与跨簇流量比例；
- Head 故障时控制流量峰值和重新稳定时间；
- 满载时的拒绝、分簇、降级和告警行为。

只有这些维度都在目标硬件和流量模型下达标，才可以形成产品容量声明。
