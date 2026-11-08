# C动态发现控制与可选后端完整栈及整体自审报告

状态：本轮限定Host软件实现、逐项自审、全矩阵和整体代码/架构审查已完成。未操作实机；本文不是外部或生产GO。

## 1. 范围与基线

用户要求补齐C动态发现控制、可选后端完整调用链栈，C/Rust共同合同、原栈门限不变，逐项自审后全回归与整体架构审查；不操作实机。
当前`v6-simplified-rust@099d5590`原地续做，保留前轮Native/H0/H1未提交修改。
开始前保存486项实际工作树快照，Full基线120/120；增量不是单纯`git diff HEAD`。

本轮C工厂仍限定封存Static身份/能力、精确ACTIVE Session/ACL、每邻居专用Link及绝对装配到期。
“动态发现”指认证路由控制，不包括动态身份Admission、Parent/Session热换或共享Link目的定位。
`control=NULL`保留仅消费已发布路由的数据子集；可选固定control启用真实C0自动发现。
业务仍用原Publisher/Subscription；没有增加Topic、完整Principal或新普通数据头字段。

## 2. 小项实现与自审

| 小项 | 实现与自审证据 | 边界 |
| --- | --- | --- |
| C0发现接Native | 唯一Coordinator RX分派，实际Security Origin/Hop/Replay与精确ACL后调用Route；三节点空路由真实发布订阅 | Driver只搬字节，不手工安装动态Route |
| 小MTU控制 | 0201/0202/0203完整54/77/99 B正文，0206局部分片；address width1/2/4、127 B载体 | 不重新设计Wire或加普通头 |
| 物理义务 | Unknown/stop/reverse到期/reopen保持原token/frame，真实精确完成和退休后才清反向义务 | reopen不再伪造逐token终态，冲突已锁存终态不能洗白 |
| 有界与期限 | 控制job全部分片合计16次提交机会，持续背压不无限重试；业务仍原Native deadline | Unknown预算耗尽只禁止新发，不能释放物理借用 |
| Reverse原期限 | Route锁内Action给出原expires_at；重复RREQ/迟到RREP不用新接收时间续期 | 到期零新提交，已接受Unknown仍保留 |
| C/Rust重试 | Rust明确NOT_SUBMITTED退休后保留帧，不刷新Origin/Hop；真正已发送丢回复仍新外层事务 | Rust保持产品max_attempts配置，等效产品设16 |
| 完整协议链 | 真Backend虚表、跨TU、Codec/Security/Route/Adapter、允许回调协议续链，失败关闭工具与自测 | 产品函数栈用联合约束，不当作0 B |
| 优化构型 | 保留实际Actor阶段，固定借用描述符/union及必要编译器阶段注解；原门限检查全部帧 | 不用全局禁内联、不隐藏协议边、不增栈门限 |

### 2.1 发现并修复的问题

1. C控制等待Unknown无法由合法精确迟到完成排空：修正Adapter无冲突Unknown与已锁存冲突的区分。
2. 只有deadline而没有整job次数限制，可在恒定now下反复NOT_SUBMITTED：增加16次总预算，包含跨片次数。
3. 重复RREQ/迟到RREP刷新Reverse控制期限：真实RED后改用锁内原期限；Unknown不因到期假释放。
4. Rust明确未提交时仍重新保护帧，以及同类Reverse期限续长：字节不等、迟到RREP和重复RREQ均真实RED；修补后62项相关路由测试GREEN。
5. 旧kernel前缀栈门禁遗漏可选Backend间接链，Debug/Release/Os还存在不同内联峰：增加完整可达协议图，按实测整改，不将旧单模块通过视为整链通过。

一次审查声称mixed能力`3→1`无法交付，经原生产代码真实新回归及字段核对证伪：
`installed.next_hop.capability_bits`是直接入口，`installed.capability_bits`才是路径交集。
审查方已撤回误报，保留mixed正向用例，没有为迎合报告更改生产行为。

### 2.2 区分性回归

控制集成包含空路由A-B-C、两段返回路径丢首/尾片、错Origin/正确Hop、错Hop/Source/Role/Opcode/长度、
wrapper与inner ACL、Replay、跨组迟到、完成/超时后组高水位、原期限内业务Origin零消耗、撤权与公平调度。
另有跨片15+1预算、第16次Unknown、width4真实三片RERR、重复RREQ/迟到RREP等值期限及真实退休检查。
测试不以原typed事件或手工安装Route代替认证字节输入。

借用描述符单独检查NULL、非法模式、输出/长度/事实与描述符的完整或部分别名；拒绝时Owner、workspace及输出哨兵不写。
原C1/H0/O1/O2、Core、Adapter及Native语义测试仍随全矩阵执行。

## 3. 栈证明与产品条件

门限保持单函数256 B（Route/Transport/Service1024、Optional384），完整协议链Nano768/Lite1536/Full3072 B。
旧门禁继续执行；新门禁从完整Native入口和精确H0/H1工厂出发，不按旧kernel源码前缀删边。
保留源码指纹、实际虚表绑定、逐调用点解析及编译器依赖新鲜性检查；缺图、未知间接边、递归、无界动态帧均拒绝。

Native应用回调可调用的协议API、Driver同步completion/RX/link-event续链也计入协议帧。
外部Driver、Provider、锁、libc、应用和C0计数器的真实帧不在协议源树内，工具输出`driver+lock<=X`等联合条件，
以及仅供参考的单类投影。满足单类投影不等于联合条件成立；0余量仍拒绝。
`PRODUCT_STACK_NOT_PROVEN`不是MCU任务栈通过，IRQ/RTOS/产品Provider还必须实测。

最终冻结批次的Nano协议链如下（宽矩阵在下一节单列）：

| Nano构型 | H0 / 768 B | H1 / 768 B | 最紧产品边界投影 |
| --- | ---: | ---: | ---: |
| Debug | 736 | 768 | H0 64 B / H1 48 B |
| Release | 752 | 752 | 16 B |
| MinSizeRel | 752 | 760 | 16 B |

Debug的768 B最大路径为纯协议叶子，不包含外部调用；所有实际外部边界仍单独检查正余量。
Release/MinSizeRel最紧约束包含`driver + lock <= 16 B`，说明不能直接套用任意Host/MCU Driver；
必须在产品ABI上测整个Driver续链与锁并满足联合条件，或继续降低协议路径。不能把16 B写成已通过产品验收。

### 3.1 固定内存成本

三档Host真实测试程序报告下列`sizeof`；不是MCU ABI或整节点总内存：

| Profile | 可选control | routed数据后端 | 两者合计 |
| --- | ---: | ---: | ---: |
| Nano | 6,776 B | 4,680 B | 11,456 B |
| Lite | 8,712 B | 4,680 B | 13,392 B |
| Full | 15,528 B | 5,960 B | 21,488 B |

这些值不含Native Facade、借用的Security/Route/Adapter、产品计数银行、任务/IRQ栈。
control为调用方单独装配的可选固定存储；未启用它不分配该control池，普通数据帧也不因此增加字段。
本轮没有新增独立的control编译开关；`control=NULL`表示运行时/存储子集，不代表已将routed archive中的控制代码单独裁掉。
Action增加的原期限快照合计16 B已计入control；借用描述符及Driver claim复用互斥union。

### 3.2 跨工具链检查器整改

ARM GCC原始Windows路径中的`\native`曾被工具误当成标签换行，新增反例真实失败后改成按函数名/栈后缀解析，
不删除函数/边。加上`dynamic,bounded`精确边界和运行库分类后，检查器13/13自测通过。
WSL启用编译器栈保护的图包含`__stack_chk_fail`，本机libc符号表确认其定义；仅该精确符号显式归入
`compiler-runtime`条件边界，不通配未知符号、不将故障处理路径视为零栈。两项增量均通过内部复审。

## 4. 最终回归与源码稳定性

最终冻结后重新执行，全部退出0：

| 范围 | 实际结果 |
| --- | --- |
| Windows GCC Full/Lite/Nano × Debug/Release/MinSizeRel | 九配置各125/125 |
| Nano全部可选Feature OFF，Debug/MinSizeRel | 各70/70 |
| Nano仅Native ON | 84/84 |
| MSVC19.51 Full Release，严格警告 | 102/102，真实C/C++安装消费者编译/链接/运行 |
| WSL GCC严格fanalyzer，Full/Nano | 各139/139 |
| WSL ASan/UBSan、Clang | 各118/118 |
| 可运行WSL TSan并发门禁 | 14/14 |
| ARM Cortex-M4三档纯编译 | 各60个生产C文件、22项原/新增栈门禁；无板卡操作 |
| Rust软件矩阵 | 17类命令全通过；默认Debug/Release/MSRV各414项，native-routed各423项 |
| Rust其它门禁 | 严格Clippy/fmt/rustdoc、两个Cortex目标、36个Feature组合、91个Codec向量及7项Admission oracle |

完整Native协议链六条路径类型在各Profile的真实峰值：

| Profile | Debug H0/H1 | Release H0/H1 | MinSizeRel H0/H1 | 原门限 |
| --- | ---: | ---: | ---: | ---: |
| Nano | 736 / 768 | 752 / 752 | 752 / 760 | 768 B |
| Lite | 736 / 768 | 784 / 784 | 752 / 760 | 1536 B |
| Full | 736 / 784 | 816 / 816 | 768 / 768 | 3072 B |

ARM三档H0/H1分别为Nano392/392、Lite384/384、Full392/392 B，也使用原Profile门限；
这些是该编译器/ABI的协议部分，不是实际产品总栈或任务水位。

最终516项输入图SHA256为`b9ad8a62ffa2b47e55e3863bd00f5c07bf5ff2e6d4e8478d603e430269a35214`。
另外逐文件复算C502项、Linux291项、Rust463项的各矩阵输入图，前后稳定并与交付工作树一致。
`final-proof.json`登记各命令/结果，`complete-stacks.json`保留九配置完整链及全部联合预算。
33项未跟踪文件中的相关文本已单独检查尾随空白；真实暂存区为空，HEAD和分支不变。

工具移植整改前的第一批虽然包含不少绿色测试，但源码图发生变化，明确不作为最终验收。
131份日志/元数据复制保留在`pre-final-tool-portability/`，包括ARM路径问题和Linux未知编译器运行库符号；
没有删除失败证据、降低门限或把定向结果冒充全量结果。

## 5. 架构取舍与未完成边界

| 决策 | 原因 | 代价/不适用范围 |
| --- | --- | --- |
| 继续当前工作树并保存开始快照 | 这是前轮未提交接线的连续任务 | 增量评审必须用实际开始快照，不能只看HEAD |
| Route锁内最窄反向预检/期限快照 | 禁止Coordinator无锁读取另一个Owner的可变表 | 增加少量内部SPI/Action字段，不是Wire/API扩张 |
| 封存Neighbor用已有Codec加精确Session | 每Link只有一个配置邻居，不需要全Owner候选扫描 | 不能直接复用于共享Link、动态身份或热换产品 |
| 整体协议链加联合产品预算 | 不能隐藏回调协议续链或宣称任意产品函数0栈 | 外部产品栈、IRQ/RTOS仍有独立验收工作 |
| 保留实际阶段与编译器注解 | Release/Os不能把独立临时量扩大到深链 | 多少量阶段函数/调用，需每构型持续复测，不保证任意编译器相同帧 |

当前不操作COM/USB/Probe/开发板、不写Vault、不提交/推送或改旧外审候选哈希。
本轮即使Host验证全部通过，也不等于真实Flash/Witness/掉电、生产密码、物理时间戳、性能/功耗或长期运行放行。

## 6. 可追溯入口

- [实施计划11](../../00-项目管理/11-C动态发现与可选后端全链栈实施计划.md)
- [共同合同25](../../10-理论与规划/建议方案/UCN_v6_C_Rust共用合同收口/25-C动态发现控制与可选后端完整栈合同.md)
- [真实Native产品装配说明](../../../examples/native/README.md)

本地忽略目录`.v6s-stack-audit/native-discovery/`保留开始快照、RED/GREEN、任务复审、失败批次及最终指纹。
逐项及增量内部复审通过；新鲜整体审查完整覆盖开始快照到当前的36个文件增量，未确认新的Critical/Important/Minor。
审查明确不判断真实硬件、生产Provider、违规产品回调、动态身份/热换/共享Link，以及无关旧协议。
这些条件均保留为后续工作，没有随软件测试一并放行；外部审计由用户另行安排。
