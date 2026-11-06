# Release/Nano栈与分析器整改计划

> **For agentic workers:** REQUIRED SUB-SKILL: use superpowers:executing-plans inline, task by task.

**Goal:** 在原有门限和严格诊断下收口Release/Nano栈超限与分析器失败。

**Status:** 2026-11-07，五项实施/小项自审及最终矩阵完成，内部交付；不是外审或硬件GO。
结果与实际资源代价见[结案报告](../08-实现与验证/版本演进/UCN_V6S_Release_Nano栈与分析器整改及整体自审报告.md)。

**Architecture:** 保留单Owner、调用方固定工作区、失败不写输出；消除多余自动副本、
过深包装和重复局部状态，不引入全局scratch、堆、递归或新的跨Owner依赖。

**Tech Stack:** C99、GCC14.2/WSL GCC、MSVC、CMake、CTest；Rust只做一致性回归。

**Spec:** [上轮实际失败清单](../08-实现与验证/版本演进/UCN_V6S_原生API首轮四项实施与整体自审报告.md)第7节，
现有`check_v6_stack_usage.py`、`check_v6_call_stack.py`及实际模块行为测试。

## Global Constraints

- 不放宽任何单函数/调用链阈值，不移除检查范围，不用关闭告警、优化等级或强制noinline绕过。
- 不更改Wire、密码算法、持久化正文、安全/Replay/生命周期语义；失败输出与Owner保持原合同。
- 不用全局可变缓冲、不靠增大所有Profile容量换栈；必要静态scratch只在明确已有生命周期内复用。
- 保留继承工作树；当前精确快照区分本轮差异。原地继续，不暂存/提交/推送。
- 不操作实机、串口、固件、目标Probe；不修改Vault或记忆。

## Review Focus

- 移除临时副本后，错误输入/溢出/别名仍须零输出、零业务提交。
- 工作区复用不能覆盖仍被Provider、Driver或已发布View持有的数据。
- 更浅栈不能把锁内状态暴露给回调，不能让不同Owner共享未同步scratch。
- Digest边界0/55/56/63/64/65、domain长度64、canonical长度前缀逐字节不变。
- Full/Lite/Nano、Debug/Release/MinSizeRel、Feature OFF都验证，不能只挑有利优化等级。

## Task 1: Digest与分析器

**Files:** `src/core/ucn_digest.c`、必要的内部digest头、`tests/simplified/test_common.c`。
**Interfaces:** 保持sha256_128/256/canonical签名及摘要字节，供所有Owner消费。

- [x] 保存精确快照，重跑原栈/分析器RED并记录根因。
- [x] 独立Known Answer覆盖分块/填充/域前缀边界、别名零写。
- [x] 简化摘要局部状态/调用层次，消除分析器根因；不抑制诊断。
- [x] 运行Common/跨语言Digest与相关栈门禁，自审。

## Task 2: Codec、Security与Admission

**Files:** `src/wire/ucn_control_wire.c`、`src/security/ucn_security_session.c`、
`src/capability/ucn_capability.c`、`src/admission/ucn_admission*.c`、`ucn_identity_binding.c`。
**Interfaces:** 保持现有入出参、认证/选择/replay合同，消费Task1原字节摘要。

执行补充：公开API/Wire入出参不变；私有Replay/Adapter句柄改为`const *`，Record Encode
改内部Request DTO；所有C及Rust一致性工具的C调用者共同迁移，无旧值传递兼容别名。

- [x] 逐函数确认RED及局部对象生存期；定位重复自动副本或参数/内联增长。
- [x] 用现有及新增长度/别名/输出哨兵测试约束最小重构。
- [x] Release/Nano对应门禁GREEN后运行模块套件并逐小项自审。

## Task 3: Flow与Realtime

**Files:** `src/routing/ucn_flow.c`、`src/realtime/ucn_realtime_domain.c`及必要私有工作区。
**Interfaces:** 保持proposal/durable requirement/输出发布顺序，复用已有Owner锁与工作区。

- [x] 移除重复Flow Candidate/Proposal副本，保留失败原子性。
- [x] Realtime Requirement构建保持失败不改Domain/输出。
- [x] 行为/故障/容量与原门限回归，自审。

## Task 4: Composition与真实同步调用链

**Files:** `src/runtime/ucn_composition.c`及必要私有workspace，相关Composition测试。
**Interfaces:** 使用前述模块原接口，不新增Owner直连或Core字段。

执行偏离（已记录并自审）：仅消除临时副本仍不能通过真实 Nano 同步链，因而保留单份
Core RX staging（Host +96 B、32位 +80 B），Layout 4→5；不是扩大队列或声明 Storage。
另将 Core TX 的准备/提交拆成同一 Owner 的两个实现单元，同一预算操作内立即消费 action。
Persistence ON 的 Composition lifecycle 暂存改为互斥 union（Runtime +16 B），Codec 的
encode/decode Request 互斥复用旧 64 B 区，Persistence Owner 大小须核对不增长。
这些变化没有增加 Owner 直连、普通 Header 或公开 API 字段，不能隐瞒为“零 RAM 变化”。

- [x] 按Delivery、Capability、Transport、Service、RX/TX、Bootstrap逐函数整改并自审。
- [x] 纳入Callees真实栈，Nano Debug/MinSizeRel调用链必须<=768 B。
- [x] 复跑路由/认证/耐久/退休/回调及并发集成，任何错误不伪造进展或权限。

补充发现：全可选 ON 的 Nano Release/MinSizeRel 还暴露 Persistence 恢复链超限。
Record Header 预检与正文验证分阶段、八参数 Encode 改内部 Request DTO；保留
Witness/Transaction/Schema/Manifest 和零发布语义，增加独立 BLAKE2s 块边界向量。

## Task 5: 全矩阵与整体代码/架构审查

**Files:** 测试/必要回归工具、报告、任务表和操作记录；生产门限不变。
**Interfaces:** 消费Task1～4最终固定候选，源码稳定后运行矩阵。

- [x] C Full/Lite/Nano Debug/Release/MinSizeRel及OFF、真实C/C++安装、MSVC。
- [x] WSL ASan/UBSan、完整`-fanalyzer -Werror`、可运行TSan、ARM编译/栈。
- [x] Rust workspace/共同Codec/裁剪/格式/lint/Cortex完整回归，普通帧同字节。
- [x] 新鲜内部代码和架构审查，重要反例补测试修复；生成差异/资源前后数值与结案报告。

完成定义：所有请求范围内门禁真实通过，或明确报告尚未关闭项；不授予硬件/外审/生产GO。
