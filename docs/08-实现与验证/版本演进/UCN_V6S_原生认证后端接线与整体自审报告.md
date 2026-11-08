# 原生发布订阅认证后端：实施与整体自审

日期：2026-11-08。基线：`v6-simplified-rust@099d55908ccc7d46d129bfa416cb015e4cdfc1b5`。
本次工作树未提交/推送；首个限定后端完成Host实现、逐项自审、全回归及整体代码/架构自审，
本轮自审未发现尚未整改的范围内重要问题。本报告不授予外审或生产GO。

## 1. 做到了什么，没做到什么

业务继续使用Publisher/Subscription/最小send，不需要自行处理密码、Replay或物理Token。
新增可信Backend SPI及C/Rust对等的首个Session Coordinator后端，真实接通：

```text
publish → 有界Native命令 → Session/ACL与现有Security保护 → Adapter/Driver
Driver原子RX → Security认证/Replay → 真实RX退休 → Native复制队列
→ 每个订阅回调前复验当前Session/ACL → 只读业务Sample
```

本后端只支持**一个不可变、已耐久ACTIVE的直连H0 Session、一个Service、固定通道、单帧、
OneWay/BestEffort、O1/O2**。它不是所有既有高级Owner向Native的总装配：多Peer/多跳、动态
Admission/Capability热续接、可靠/RPC/Transfer/严格Timed仍未通过此后端接线。

C高级factory是仓内私有产品装配；安装面公开可信端口而非完整高级Root factory。Rust提供
默认OFF的`ucn-runtime/native-backend`。这是明确的首步范围，不把假Backend、接口桩或配置标签
称为真实认证。共同规范见[合同23](../../10-理论与规划/建议方案/UCN_v6_C_Rust共用合同收口/23-原生发布订阅认证后端共同合同.md)，
装配与可运行回归见[使用说明](../../../examples/native/README.md)。

## 2. 四个小项的实施与自审

| 小项 | 实现 | 区分性自审 |
| --- | --- | --- |
| 共同合同 | Trusted Backend、精确Scope/来源、原始时间、保护最低值、真实退休 | O0不能满足O1/O2；digest只是配置标签；本地复制不伪造Principal |
| C接线 | 独立Native Session Coordinator复用Security/Adapter，Native只处理业务外观 | 实际保护帧、篡改、Replay、错误fingerprint/ACL、排队后Fence、同帧背压重试、Unknown不释放 |
| Rust接线 | 可选Coordinator借用现有Security/唯一Origin序列/Adapter，NativeRoot负责复验 | O1及O2 Provider路径实际Backend→Root→回调；独立Inbox dispatch不能绕过当前授权 |
| 资源与整体审查 | 原门限、Feature OFF、安装/ABI、全配置、分析器、sanitizer、交叉编译 | 保留失败日志，修复后重新运行；不以普通成功测试掩盖失败配置 |

每个小项先建立或运行能区分旧实现的反例，再实现/修正并复验。另执行新鲜只读内部代码审查，
重点检查认证事实来源、失效后的业务副作用、固定资源、所有权、发送/退休生命周期与依赖方向。
这不是外部审计签字。

## 3. 审查中发现的问题与整改

| 问题 | 整改与反例 |
| --- | --- |
| 非法/旧格式帧不被选择，长期占RX Ring | 独占后端认领原始帧并有界退休；先填满畸形帧，再接收合法帧 |
| 只看NOT_FOUND便声称排空 | Adapter锁内检查RX/TX/claimed/staging；初始化拒绝遗留Token，Unknown时拒绝deinit |
| 解锁验证时订阅槽被关闭重建 | 回锁后核对订阅代际和recipient；旧事件不得交给新订阅 |
| Rust发送使用上次step时钟 | send明确接收当前now；已过Deadline的直接发送零Driver调用 |
| C物理Driver调用不在共享安全控制域 | submit/cancel经现有Security Gate；占门时零Driver调用，释放后沿原帧重试 |
| 错Magic/已deinit Root仍解引用后端 | 先验证不可变Magic；在短锁内检查dead，缓存不可变别名范围，死Root不访问后端 |
| 新dead标志在锁外读，与deinit并发 | 移入原短锁；Root及锁保持到并发调用者退出；原并发回归重复执行，不以改宽返回码掩盖竞争 |
| 回调借用payload无法直接复制发布 | 只允许当前回调范围内的有界借用，回调后不允许留存内部指针 |
| Release新测试的assert被NDEBUG关闭 | 测试显式恢复断言，保持-Werror；不接受空跑 |
| Native step Release栈304 B超过256 B | 互斥阶段共享Root内单份静态union；step降至224 B，门限不变 |
| 上述union重构时NULL成员取址 | UBSan定向反例先失败；入口校验后才取工作区成员地址，随后复验 |

失败包括真实Release编译、栈超限、并发配置失败与UBSan诊断，均保存在本地忽略构建目录。
最终验收只能使用修复后的候选；中途发生源码变化的矩阵不当作稳定候选证明。

## 4. 全量验证

以下均在最后生命周期整改后重建/运行；原失败批次不计入最终PASS。

| 门禁 | 实际结果 |
| --- | --- |
| Windows GCC Full/Lite/Nano × Debug/Release/MinSizeRel，全ON | 9配置，各118/118 |
| Nano全OFF Debug/MinSizeRel | 各68/68 |
| Nano Native-only MinSizeRel | 77/77 |
| MSVC19.51 UTF-8 Release | 98/98，C/C++真实安装运行包含在套件内 |
| WSL GCC严格fanalyzer，Nano/Full | 各132/132，不抑制诊断 |
| WSL ASan/UBSan | 114/114，另有NULL反例整改后直接运行 |
| WSL Clang Release | 114/114 |
| WSL TSan可运行并发套件 | 14/14；仅进程级setarch，不改系统ASLR |
| Cortex-M4 C99/Os，三Profile | 各58生产源编译、各20项原栈/调用链门禁通过；不连接板卡 |
| 文档/索引/Native边界 | 158篇/484链接、338函数；无Facade私有Owner调用、普通Wire字段增加或堆调用 |
| Rust完整17类矩阵 | 全退出0，454源码前后指纹一致；默认Debug/Release/MSRV1.85各411项，认证feature各415项 |

Rust矩阵还包括fmt、两种严格Clippy、34种可选Feature组合、默认/认证两组Cortex软硬浮点编译、
C/Rust Codec及Admission oracle、资源程序。另运行Native/Runtime认证feature的严格rustdoc，
`-D warnings`通过。没有运行ESP32编译/烧录或任何物理测试。

C 11配置与Linux五配置各自记录前后源码指纹一致；最终再次逐文件核对这些指纹与当前工作树。
对应源码成员分别为485、280，Rust为454；最终核验还覆盖14个未跟踪文件，新增文件尾随空白0。
新增源码/测试/头/文档均检查，不仅依赖Git对已跟踪差异的空白检查；暂存区保持为空。
Nano Host内核调用链Debug/Release/Os分别
704/768/768 B，后两项仍无余量；本轮没有以新业务为由放宽768 B门限。ARM同组调用链424 B。
这些是现有编译模型的门禁，不覆盖未知产品回调的任意深度或真实IRQ嵌套。

## 5. 架构与资源结论

- 业务Facade不包含私有Security/Route/Persistence头和调用；跨Owner编排在独立Coordinator。
  最小Core没有反向引入Native，基础Rust Native不依赖Runtime；Group/Cluster/Realtime仍独立。
- Session/ACL/Replay只由原Security Owner决定。C复用其唯一序列；Rust借用唯一OriginSequenceOwner。
  Native没有复制安全FSM、耐久承诺、密码或网络序列分配器。
- 发送背压保持原帧、原序列、原期限；已经暴露给Provider的序列不回收。
  协议接纳不等于远端交付，BestEffort认证后应用队列满可以丢弃，不回滚Replay以复活旧帧。
- 当前授权在**每次**订阅回调前复验。完整16 B Principal由安全事实投影；标签、Scope不上Wire。
  后端不支持活跃换Session；新Session使用新Root，不继承旧队列，不重置同Session计数器。
- C Native API/Layout2；Core Layout保持5。三档Native声明存储仍为2048/6144/12288 B。
  认证Coordinator的独立静态区另外预算，不隐藏在零开销口号中；完整Security/Adapter/Driver还需另计。
- C99产品、Rust no_std/forbid-unsafe生产路径不引入动态分配。Host fixture的Box::leak只作测试装配。

最终修复后的C静态布局（字节）：

| Profile | Native声明存储 | Host实际Root | Host认证后端 | Cortex-M4实际Root | Cortex-M4认证后端 |
| --- | --- | --- | --- | --- | --- |
| Nano | 2048 | 1912 | 2768 | 1808 | 2568 |
| Lite | 6144 | 4312 | 2768 | 4144 | 2568 |
| Full | 12288 | 11512 | 3792 | 11216 | 3592 |

调用方按声明存储分配，不用Host实际Root尺寸替代公共ABI；后端另外分配。此表不包含借用的
Security、Adapter、Driver/Provider、业务context，也不要求再额外分配一个未使用的Core Node。
测量通过包含实际类型的独立Host探针与ARM ELF符号尺寸完成，不用COFF nm推断的跨度冒充sizeof。

Rust同候选Host `size_of`：认证Backend在帧池256/512 B时为2056/3336 B；Inbox的4/8/16容量为
3752/7432/14792 B，NativeRoot本体32 B。Backend借用的Owner/Driver/Provider/Counter不含在这些
数字中；Rust没有宣称与C布局相同。这是编译布局测量，不是运行期峰值或目标板RAM水位。

Full Release Native单帧最大256 B（init），step240 B、dispatch208 B；认证后端send192 B、poll176 B。
原256 B门限不变。
测量不包含任意产品回调/Driver最坏调用链，不把编译期栈报告称为MCU任务栈实测。

## 6. Wire费用、Provider与放行边界

普通头新增**0 B**。既有C1头为`9+2×address_width` B（11/13/15/17），O1/O2沿用16 B Tag；
H0没有Hop Trailer。16 B健康正文在A0上共43 B，O0为27 B；fanout逐目标发送，并不免费群发。

O1 Rust fixture使用HMAC；C fixture及O2可逆测试Provider为明确的Host替身。O2回归证明Provider
调用、密文路径、Tag拒绝和业务交接，不证明AES/ChaCha、生产密钥安全或硬件加速性能。
既有C/Rust独立Codec/密码oracle单独执行，不能用测试替身宣称生产算法完成。

本轮没有操作板卡、COM/USB、Probe、烧录、物理Bearer或Vault，没有暂存、提交、推送。
真实密码Provider、Flash/Witness/掉电、MCU栈水位/IRQ/DMA、性能功耗、长稳以及生产/外审验收
仍是独立工作。当前首版晚于step时间快照的RX保守丢弃，未承诺无损IRQ并发接收或跨节点时间同步。
