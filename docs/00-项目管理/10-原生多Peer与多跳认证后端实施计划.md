# 原生多Peer与多跳认证后端实施计划

> 按既有授权连续实现，C/Rust任务逐项RED→GREEN、自审并独立审查，最后全矩阵；不操作实机，不提交推送。

**Goal:** Native发布订阅复用实际多Peer/多跳数据运行时，交付原始认证Principal而非中继身份。

**Architecture:** 一个Coordinator独占Adapter入口与退休。Endpoint是端到端Session/Service；Neighbor是逐跳Session/物理Link。Native仍只处理通道、复制队列、Latest与应用回调，不复制协议FSM。

**Tech Stack:** C99/C++17、Rust no_std，无生产堆分配；既有C1/O1或O2与H1格式，普通头字段新增0。

**Spec:** [认证后端合同23](../10-理论与规划/建议方案/UCN_v6_C_Rust共用合同收口/23-原生发布订阅认证后端共同合同.md)，本轮增量合同24。

## 全局约束与本轮决定

- 在现有`v6-simplified-rust`工作树继续，HEAD099d5590；上一轮未提交修改完整保留，必须用开始快照区分本轮改动。
- 一个Adapter只有一个数据Coordinator。禁止每Peer各建单Session后端争抢RX。
- 端到端Origin与当前Hop分离：中继只验证/更新Hop，不持有终点Origin密钥、不解密Payload；HopLimit有限递减。
- 使用既有Route Owner发布的精确静态/动态路由。Rust复用RoutedNode自动发现；C数据接线不能伪称尚未接通的多邻居控制自动发现已完成。无路由不重置Native原Deadline，不臆造路由或降级O0/H0。
- Session/Parent/Capability来源必须来自当前Owner；业务配置标签只作精确匹配。每个业务回调前复验Endpoint当前授权。
- 首版固定Endpoint/Neighbor登记，不自动换Session/Binding/策略；热续接仍用原耐久发布/真实排空合同，不能用表槽覆盖。
- Native声明存储上限2048/6144/12288 B不增；后端另计。原函数栈与调用链门限不放宽，不能抑制分析器。
- Driver/Provider为Host替身不等于生产密码/Flash验收。无COM/USB/Probe/烧录/Vault或Git暂存提交推送。

## Review Focus

1. A→B→C的AC Origin与AB/BC Hop必须独立；B没有AC密钥仍能转发，错误Hop/Replay/TTL1拒绝。
2. 同Service多个Origin不能混Scope、Latest或Principal；同地址不同Binding/Session不能继承授权。
3. Native排队后Parent/Session/ACL/Capability撤销不得产生后续回调；身份绑定只来自实际Owner。
4. 发送背压、Route变化、stop与Unknown保留实际Token与原期限；不能重复Origin保护或假退休。
5. 全Feature-OFF、Nano单槽、短MTU、容量满、坏包及输出别名不破坏既有单Peer后端。

## Task 1: 共同合同与真实候选基线

Files: 本计划、任务表、操作记录、合同24、忽略目录内修改前快照。

- [x] 核对两侧既有Owner能力、精确Route/Endpoint/Neighbor与Scope/时间规则，写明实现差距。
- [x] 固定内存容量、错误零写、原期限、无路由和物理退休合同；对共享接口作预审。

## Task 2: C多Peer/H1认证数据协调器与Native端口

Files: Security最窄C1 Hop SPI、独立runtime/internal后端、实际Native集成测试；CMake由主任务统一集成。

- [x] RED：多Peer同Service、A-B-C实际认证/不透明中继、双向接收；现有单Peer后端不能满足。
- [x] GREEN：唯一入口/固定Endpoint与Neighbor表、Route Owner解析、实际Security Origin/Hop、真实Adapter提交/退休。
- [x] RED→GREEN：错误Hop/Origin/来源、Replay、TTL、短MTU、Scope撤销、原帧背压和Unknown关闭。
- [x] 小项自审，原256B函数门禁及Nano768B旧kernel链门禁；说明自动控制发现与数据后端的责任边界。新增可选Backend间接全链总栈没有被旧门禁自动证明，另列验证边界。

## Task 3: Rust RoutedNode → Native Backend

Files: `ucn-runtime`可选routed-native后端、RoutedNode最窄只读Endpoint/认证交付元数据，真实集成测试。

- [x] RED：多Peer/多跳实际RoutedNode→NativeRoot→业务回调，不能用FakeBackend代替。
- [x] GREEN：借用实际Identity/Security/Capability/Adapter/计数器，复用RoutedNode收发/发现/退休，独立Facade不依赖路由Owner。
- [x] RED→GREEN：回调前授权丢失、原RX时间、错误Scope、Latest来源隔离、背压及排空。
- [x] 小项自审，workspace/Clippy/MSRV及Feature OFF编译；不靠新增allow压掉诊断。

## Task 4: 跨语言对照、全回归与整体审查

- [x] 逐项核对C/Rust共同线格式/准入/保护/Scope/Deadline/退休语义，明确真实实现差异，不改旧签字哈希。
- [x] C三Profile三构型、OFF、MSVC/Clang、ASan/UBSan、分析器/TSan、ARM仅编译与原资源门禁。
- [x] Rust Debug/Release/MSRV、fmt/Clippy、可选组合及Cortex、Codec oracle，指纹冻结后整套运行。
- [x] 新鲜只读整体代码/架构审查，重要问题区分性RED→GREEN修复，再跑最终稳定矩阵。
- [x] 更新任务/操作记录、使用说明、完成报告与真实RAM/栈/原Tag费用；未接通项单列，不授予生产GO。

本轮结果见[完成报告](../08-实现与验证/版本演进/UCN_V6S_原生多Peer与多跳认证后端及整体自审报告.md)。
本计划完成只表示上述限定Host后端接线，不把工厂差异、可选间接全链栈、生产Provider和实机签字一起放行。
