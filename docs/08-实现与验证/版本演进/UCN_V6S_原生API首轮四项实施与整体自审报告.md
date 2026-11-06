# 原生API首轮四项实施与整体自审报告

## 1. 结论和范围

本轮完成共同合同、最小点对点send、静态Pub/Sub与有界应用分发、只读示例与C++/Rust
薄外观四项实现。完成逐项自审、真实软件Core/Adapter回归和新鲜只读内部代码/架构复审。
这是**首轮静态Native后端的实现收口**，不是整个API-00～09、rmw_ucn或生产放行。

完整资源矩阵仍为HOLD：GCC Release/Nano部分栈门禁和GCC分析器失败，修改前的精确
快照也能复现，见第7节。不能把通过的Debug、MSVC和Sanitizer结果表述成全部配置通过。
未操作实机，未安装ROS，未暂存、提交、推送或更改历史签字哈希。

执行入口：[四项计划](../../00-项目管理/07-原生API最小闭环实施计划.md)。
使用入口：[真实示例与接口说明](../../../examples/native/README.md)。

## 2. 审计基线与差异归属

- 分支`v6-simplified-rust`，HEAD `83eed914ea9ef547f435fbf91bc3fbca46293abb`加继承工作树。
- 开始前存在大量未提交实现；不以`git diff HEAD`将其全部归为本轮成果。
- 保存506项精确快照：`.v6s-native-api/baseline-83eed91-20261106.zip`，SHA256
  `B425BD284B2A92D934BE7B38B0856F744D9F7EDCE9F24E7D27D12773F94945B0`。
  内含500项源码/配置/测试/工具和6份相关文档，以及逐项Manifest。
- 改前Full Debug CTest为108/108；Rust workspace退出0。失败比较从快照解压目录构建，
  不是凭历史报告归因。快照未覆盖的`cmake/`包模板只复制未改动版本作为构建辅助输入。
- 本轮代码范围以快照差异为准；生成证据位于`.v6s-native-api/`，不是外部签字基线。

## 3. 四项交付及模块边界

| 项目 | 实际交付 | 明确未做 |
| --- | --- | --- |
| 共同合同 | 静态通道/Schema、复制/队列/关闭/退休合同、基线快照 | 不一次冻结全部高级用户API |
| 最小send | C `ucn_send`、Rust `CoreNode::send`、真实接收序号/配置身份、只读View、原子Endpoint安装 | 不保留旧目标式publish兼容别名 |
| 静态Pub/Sub | 可选`UCN::native`、独立静态Root、固定fanout、FIFO/KeepLatest、锁外有界dispatch | 不复制Wire/Replay/Driver退休，不直接调用私有Owner |
| 语言与例子 | C++17 typed RAII、独立Rust no_std外观、三节点只读Health示例及测试 | 无ROS安装、CDR/GID/Graph、MAVLink动作或真实设备 |

C依赖仅为`Native → 公共Core`；Core无Native字段、include或符号依赖。CMake默认OFF，
OFF不安装Native/C++头，不生成Native target。Rust新crate只依赖Core、Adapter、Types，
`no_std + forbid(unsafe_code)`，不用FFI、堆或第二套协议编码器。

C++只是C外观，业务Codec留在产品侧；Rust Health也只在examples/support，不进入协议库。
架构扫描、archive导出/依赖检查、实际安装C/C++ include/link/run及OFF安装检查均执行。

## 4. 首轮支持的精确语义

### 4.1 后端与费用

只支持显式产品可信、隔离静态网络的`C1/O0/H0/BestEffort/OneWay`。配置Principal是
静态绑定标签，不是密码认证证明。Native要求Origin认证、加密、可靠、RPC、动态匹配或
严格源年龄时，在接纳前返回Unsupported；已有高级Owner没有被删除，也没有自动接线。

发布订阅通过静态Service/Schema目录工作，不逐帧携带Topic字符串、Publisher ID或Schema。
16 B Health正文全部big-endian：电压u32、电流i32、温度i16、模式u8、flags u8、故障u32。
A0头11 B，总27 B；A发B/C是两次27 B发送，不能把fanout总费用算成一次。

C/Rust真实软件示例产生相同的两帧：

```text
614004010200400000000100005DC0FFFFFB5009E6020180000001
614004010300400000000200005DC0FFFFFB5009E6020180000001
```

普通头新增0 B；未新增Opcode、Record或Wire Contract。

### 4.2 队列、应用回调与来源

- 一次发布复制一份父Job正文，冻结目标集；按原期限逐目标交给Core，不等某个目标物理
  完成才推进剩余目标。同通道父Job维持接纳顺序。成功只表示本地接纳，fanout可部分失败。
- FIFO对每个订阅保序；KeepLatest按精确来源流合并，旧但尚未见过的合法序号不能覆盖新值。
  重开订阅不能清来源高水位。本机与线上序号域分开，不互相污染。
- dispatch有调用预算和持久轮转游标，回调不持Core/Native锁。回调可以deferred publish或
  self-close，不能递归step/dispatch。close独立于数据池，即使池满仍可围栏。
- RX原时间保留；IRQ晚于step时钟快照时，年龄锚点为`min(RX时间,接纳now)`，不提前交付，
  不用未来时间延长寿命；到期采用半开区间。这不是跨节点源采样年龄证明。
- source_fence清该来源排队事件并阻断未交给Core的本机Job，不能撤回Core已接纳的物理发送。
- Native统计不是同一漏斗：父Job、逐目标Core接纳、未接纳失败、逐订阅callback分别计数；
  不用统计差值冒充网络丢包或远端业务完成率。

### 4.3 所有权、关闭与退休

- Core/Native存储、配置、输入/输出别名先检查。Endpoint批次在一个Core gate内完整预检，
  平台锁故障后清未暴露Endpoint并保持Core Fault，不能留下指向失败Root的回调。
- Entity句柄精确绑定实例、slot、generation；不得回绕或用重开复活旧context。
- stop围栏新业务，继续step真实排空；deinit必须确认Core quiescent、Job和callback退出。
  未start的Root也能关闭。Unknown不能靠清对象、超时、Drop或数字reopen伪造退休。
- C后端要求Driver确认旧硬件实例已物理围栏，软件例子模拟此事实，不证明真实Driver。
  Rust允许非冲突Unknown由精确迟到完成排空，但Link仍被围栏，Fault排空后不得restart。
  矛盾完成即使逻辑reopen仍隔离，不用第三条冲突事实消除前两条冲突。
- C++对象禁止复制/移动；callback/context在注册可见前就准备好。析构不能隐式阻塞，也
  不能丢失关闭失败的句柄：失败终止诊断，不留下悬空this；建议显式close/quiescent/release。
- Rust首轮Inbox有意`!Sync`，单任务Actor；Core/Adapter/Driver/Inbox永久借用。编译失败
  用例验证不能宣称Sync、不能把临时后端挂入永久Root。不承诺与C相同的内存字节布局。

## 5. 自审发现及区分性修复

逐项RED→GREEN和三轮只读内部复查重点如下；不是将一次绿色总测试当作逻辑证明。

| 问题/反例 | 实际修复与验证 |
| --- | --- |
| 初始化失败后残留Endpoint/context | pristine批次安装；锁故障1～24注入，失败Root不被回调引用 |
| 同长度不同Schema被typed实体接纳 | 精确Schema ID/revision/size预检；错误Schema拒绝且不占槽 |
| C++注册返回前可见、析构或step争用丢句柄 | 先建context、close独立latch、失败诊断；可见性proxy仅单元测试，不冒充认证网络 |
| 多订阅FIFO先读新、旧序号覆盖Latest | 逐流接纳次序/高水位与公平游标；多订阅budget1/交替来源回归 |
| future IRQ时间延长寿命 | 保留原RX+保守锚点、禁止RX之前交付；C/Rust同类回归 |
| 数据池满无法close、callback递归 | 独立close latch、active context退休；满池/self-close/deferred/递归拒绝 |
| Rust Unknown永远不退休或冲突被reopen洗白 | 区分非冲突Unknown与硬冲突，精确迟到证明可排空，冲突跨reopen仍隔离 |
| Core Fault无法排空或排空后恢复权限 | fault latch允许真实drain，禁止restart；Runtime旧测试同步强化“不恢复Session/Capability” |
| Native初始化Host栈272 B超限 | 拆出配置验证helper；同一256 B门限下最大240 B，不放宽阈值 |

新鲜只读内部审查最后返回：Native首轮范围内未发现新的重要遗留问题。它不授予外部GO，
也不覆盖所有旧模块、真实密码Provider、Flash或物理Bearer。

## 6. 实测软件回归

下列数字是本轮实际执行，不复用旧审计数字。C构建/日志在`.v6s-native-api/`；WSL构建
在`/tmp/ucn-native-*`；Rust完整结果在`rust/target/software-validation/latest.json`。

| 配置/检查 | 结果 |
| --- | --- |
| Windows GCC14.2 Full Debug，Native/Persistence ON | 113/113 PASS |
| Windows GCC14.2 Lite Debug，Native/Persistence ON | 113/113 PASS |
| MSVC19.51 Full Release `/W4 /WX` | 94/94 PASS，含实际C/C++安装消费者 |
| WSL Full ASan/UBSan | 110/110 PASS，Sanitizer构建不充当生产栈尺寸测量 |
| WSL Native pthread TSan | 20,000次并发发布、与step争用close、status/deinit PASS，无race报告 |
| C Native自身Host静态栈 | 32个函数，最大240 B / 256 B |
| ARM Cortex-M4 `-Os`，Nano/Lite/Full | Native及32-bit C安装ABI交叉编译PASS，81项函数记录最大176 B / 256 B |
| Rust Debug/Release/MSRV1.85 workspace | 各410项PASS（含文档测试），11类矩阵全部退出0 |
| Rust静态检查/裁剪/交叉编译 | fmt、clippy `-D warnings`、32种Feature组合、软/硬浮点Cortex、双Codec oracle和资源命令PASS |
| Rust Native文档 | `RUSTDOCFLAGS=-D warnings cargo doc -p ucn-native --no-deps --locked` PASS |
| Windows GCC Full Release全量 | 108/113；5项旧模块栈门禁失败，见第7节 |
| Windows GCC Nano MinSizeRel，Native ON，其余可选OFF | 67/72；5项栈门禁失败，Native自身门禁通过 |
| Windows GCC Nano Debug，Native及其余可选OFF | 66/67；旧Composition调用链784 B > 768 B |
| WSL GCC `-fanalyzer -Werror` | 编译失败；原始digest相同诊断，未宣称全量分析通过 |

TSan初次PIE进程出现运行时memory mapping错误，改为进程级`-fno-pie/-no-pie`后可运行；
未更改系统ASLR/sysctl。实际成功命令直接运行原生并发测试，退出0。单次无报告不是任意
产品并发模型的证明；Rust Native仍按单Actor限制。

## 7. 未通过项、修改前对照及处理

本节保留首轮失败事实。后续单独授权的资源整改、精确增量与重新验证见
[Release/Nano 栈与分析器整改报告](UCN_V6S_Release_Nano栈与分析器整改及整体自审报告.md)，
不要把这里的历史失败数字或后续通过数字互相覆盖。

1. Full Release五项：Kernel/Composition、Security、Admission、Routing、Realtime。
   例：Composition352>256、Flow1168>1024、Realtime480>384 B。快照Release同五门禁
   5/5失败、同函数同尺寸；日志`baseline-release-stack.log`。不可移除这些门禁报绿。
2. Nano MinSizeRel四项单函数门禁在快照中4/4复现：C0 decode288>256、Security288/272>256、
   Capability288>256、Admission272>256、Flow1088>1024 B。当前另有Kernel调用链超限，
   尚未单独完成快照MinSizeRel调用链可比构建，因此该项保留未完全归因，不能整体推为旧问题。
3. Native OFF Nano Debug的Composition→SHA256调用链784>768 B，在快照Debug中同链同值
   复现。OFF安装/符号检查通过不等于整个OFF资源配置通过。
4. WSL分析器在`ucn_digest.c:148`报告canonical prefix经update/memcpy的uninitialized
   诊断。快照相同文件/编译器独立编译也失败。未关闭`-Werror`、未压制该诊断，不判定整个
   分析矩阵已完成；需要单独调查并收口。

这些遗留不是本轮扩大功能范围去重写旧Composition的授权，也不阻止交付首轮Native工作树；
但会继续阻止“全Profile资源通过”或“可发布最终协议”的结论。真实MCU调用栈仍需实测。

## 8. 接口/资源变更与后续

- Core API版本升到2，Endpoint DTO追加真实序号/配置Principal，C/C++安装ABI同步。
  是显式重编译变更，不保留旧目标式publish别名，不变更Wire版本。
- C Native额外静态存储Nano/Lite/Full分别2048/6144/12288 B，不含Core/Driver/context；
  对应channel/pub/sub/job/event上限4/8/16，正文上限64 B。Rust布局独立，不伪报同尺寸。
- 以后将认证后端接入Facade，必须消费真实认证的immutable SourceFacts，不能把当前静态
  Principal标签改名成verified；必须补安全、授权、路径、期限和失效回归。
- rmw_ucn、动态匹配、可靠/RPC/耐久、严格Timed、生产密钥、Flash/掉电、MCU和物理Bearer
  都保留各自合同/外审/硬件门禁，不因本轮Host接口可运行而放行。

## 9. 最终闭合记录

Rust最终矩阵覆盖21个crate、11类配置/命令；Debug、Release、MSRV1.85各410项，全部退出0，
32种Feature组合通过，441项源码前后指纹完全相同。未执行ESP32编译或实机，这不影响本轮
Host/Cortex编译范围，也不扩大其签字范围。

最终定向回归11/11：C/C++/Native例子、源码边界、archive符号、API索引、文档、Native栈及
两种实际安装consumer。现有文档门禁156篇/470条链接通过；另显式核对本轮9份文档的313条
本地链接，无坏链，新引入尾随空白为0（原有Markdown换行等保留）。API索引337项。
基线ZIP重新逐成员校验，最终本地差异库存为526项：相对原506项25项内容改变，20项为
新增纳入库存；其中README等原有文件不在原快照，不能把“新增纳入”全说成新写文件。
机器结果保留于`.v6s-native-api/final-candidate.json`，仅用于本地复核，不是外部签字清单。
Git暂存区为空，分支/HEAD未变；`git diff --check`无空白错误，未跟踪的新代码也单独扫描。

最终使用者应同时阅读第6节通过项和第7节失败项。四项工作达到“静态Native实现、测试和
内部审查交付”的定义；整体仍不满足所有Profile/优化等级资源门禁，不授予外部或生产GO。
后续可先单独收口遗留栈/分析器问题，再按方案接认证Native后端，最后由用户安排硬件验收。
