# UCN V6S Release/Nano 栈与分析器整改及整体自审报告

日期：2026-11-07。范围：C 简化版资源整改、Host 软件回归、C/Rust 字节一致性与交叉编译。
本报告不授予外审、真实 MCU、Flash、生产密码 Provider 或物理 Bearer 放行。

实施计划见[整改计划](../../00-项目管理/08-Release-Nano栈与分析器整改计划.md)。
原始失败保留在[上轮报告第7节](UCN_V6S_原生API首轮四项实施与整体自审报告.md#7-未通过项修改前对照及处理)，不删除历史失败证据。

## 1. 基线与授权

- 原地继续 `v6-simplified-rust`，HEAD 为 `83eed914ea9ef547f435fbf91bc3fbca46293abb`。
  上轮继承工作树不是本轮新实现，也不能仅用 HEAD 差异代表本轮改动。
- 本轮修改前快照 `.v6s-stack-audit/before.zip`，527 个基线文件（另含 Manifest，共528个ZIP成员），SHA256
  `512f9de0f673a066c61408198970d38efeeb2db087b456c632ca66f53efd6bef`。
- 审查使用精确快照差异及成员哈希。未暂存、提交、推送或改写历史；未操作实机、串口、
  Probe、固件、Vault、系统 ASLR 或 ROS 环境。
- 不提高栈门限，不删除检查对象，不关闭 `-Werror`，不降低优化等级，不新增 noinline
  绕开内联，不把同步提交改成下一次 step 才处理。

## 2. 根因与逐项整改

| 小项 | 根因 | 整改与自审 |
| --- | --- | --- |
| SHA/分析器 | canonical 前缀临时数组逃逸进入 update/memcpy 的别名路径，严格分析器拒绝；摘要包装层和独立 schedule 增加同步栈 | 前缀直接写入已初始化块；16-word rolling schedule 复用已消费的 64 B block；工作区仍 112 B。独立 KAT、生命周期、别名与计数一致性验证 |
| Capability | SELECT 同时保留 152 B canonical 临时区和摘要调用栈 | 内部流式摘要保留 93 B context，与 59 B profile 分段输入；准确保留原 domain、BE 长度前缀和正文，不改变摘要字节 |
| Security | 大句柄值传递、重复标量和候选 View 副本，在 Win64 参数 ABI/内联后叠加 | 私有 Replay SPI 借用只读句柄，包工作区复用瞬时标量；Provider 解锁阶段仍受 callback gate 保护。Origin-only 不被误当作有 Hop Tag |
| Admission | HELLO/Fragment 编解码和回滚保留完整临时 DTO | 紧凑 shape 预检后才写最终输出；回滚 View 在受锁 Owner 槽内保存。失败/容量/别名语义保持 |
| C0/C1 Codec | 动态宽度 helper 用于固定宽度字段，重复结构校验增加嵌套栈 | 地址仍按宽度处理，固定字段直接 BE 读写；shape 与 Registry 分阶段验证，encode/decode/AAD 均独立校验 Opcode |
| Flow | Candidate 与 Proposal 同时存在自动副本 | 借用 Candidate，仅保留必要 Proposal staging；摘要成功后才提交计数和状态 |
| Realtime | FSM Requirement 与本地摘要工作区叠加 | 记录正文摘要归入 Codec TU，FSM 保留状态职责，不增 Domain Owner RAM |
| Adapter | 退休/取消/匹配过程反复按值传 Token，冗余 View 副本 | 私有 SPI 用 const 指针；直接输出只发生在全部检查后、原锁内；补 NULL、Owner alias 和输出别名拒绝 |
| Composition | 长同步链上保留瞬时 Owner 指针和重复长度、Token | 使用 caller-owned 且 Coordinator 门控的工作区；Delivery +24 B（64 位），Bootstrap +8 B。未增加排队槽 |
| Core TX/RX | prepare/encode/submit/回调退休处于同一个深栈；RX DTO 横跨 Decoder/回调生存 | TX 准备产生小型 action，同一次预算操作立即提交/终结/重排；`ucn_tx.c` 仍属于同一 CoreOwner。单份 gated RX staging 避免 DTO 跨同步栈叠加 |
| Persistence ON 恢复链 | 八参数 Encode 在上层调用方留下参数区；Header 预检和正文摘要同栈；Compression 保留恒等 buffer 参数 | Header Codec 单元先校验 Envelope/Marker/Manifest，正文仍验证 CRC/Digest；Encode/Decode Request 复用同一 Owner union；简化 loader 和四工作字 G 循环，摘要输出不变 |
| 默认发送选项 | 每次 send 都为不可变默认选项预留自动对象，包含非默认调用 | 改成共享只读常量；无共享可变状态、无每次初始化，也未改变默认 Q2/Best-effort/One-way/hop=1/copy=1 |

私有 SPI 修改要求所有内部调用者共同重编译，没有保留值传递旧版别名。公共 API 版本仍为 2，
Core 私有 Storage Layout 从 4 升为 **5**，编译 Manifest 随之变化。

## 3. 新增反例及审查中发现的问题

1. 摘要独立 Known Answer 覆盖 0/55/56/63/64/65 B，以及 61～64 B domain 与 65 B 正文；
   stream 的 append/finish 必须具有活动状态及一致的 block/total 计数，退休后禁止复用。
2. Adapter/Replay 借用句柄增加 NULL、Owner alias、token/output alias；拒绝时 Owner、锁状态、
   输出哨兵不变。发现并修复 `tx_submit_ref` 缺少 Owner alias 预检。
3. Core 单次 `max_work=1` 测试验证 completion 回调和真实退休在同一次预算操作完成，
   不能靠少做工作或推迟动作换取较浅栈。
4. 独立整体审查发现 C0 encoder 拆分 shape 后遗漏 Registry 检查：opcode=0 曾可编码成功，
   自己的 Decoder 却拒绝。正式负向测试先复现失败，再补 encode 检查；七类非法 Opcode
   均拒绝，output 与长度哨兵不变。修复后独立复验通过。
5. 32 位 Cortex 编译发现 `size_t > UINT64_MAX/8` 为恒假而触发 `-Wtype-limits`。
   仅在类型宽度可能超过 SHA 长度上界时编译显式检查；64 位拒绝仍在首次读写前，32 位
   由类型范围保证。没有压制诊断。
6. MSVC 新增测试局部名遮蔽触发 `/W4 /WX`：只重命名测试局部变量，不加 unused/告警豁免。
7. 旧 v6 Adapter pthread fixture 写死每 Link 16 个 RX 槽，不适用于 Nano。
   改为两生产者各占编译容量的一半；逐项核对成功数、ISR 争用数与真实退休数，未改生产容量。
8. 全 ON 的 Nano Release/MinSizeRel 暴露 Persistence 链 912/880 B 超限，不能用其他配置
   通过覆盖。恢复/事务/Schema/Manifest 用例和独立 BLAKE2s 0/63/64/65/127/128/129 B
   Known Answer 均保留；新的 Header 单元仍在 Foundation target 内，不引入 Coordinator/Core 依赖。
9. 最后别名自审复现 Decoder 输出可覆盖输入 Request；增加 Request→meta/body 的双向
   重叠拒绝，正式反例先 RED 再 GREEN。Encoder 同时覆盖 Request/slot、workspace/slot、
   workspace/manifest digest，拒绝保持 Request、正文、metadata 和 workspace 哨兵不变。

## 4. 栈门禁的准确含义

单函数门限保持：Common/Native/Kernel/Security/Admission **256 B**，Routing/Transport/Service
**1024 B**，Realtime/Group/Cluster **384 B**；旧 v6 资源检查仍为其原有 **3072 B**。
Core 同步链保持 Nano/Lite/Full **768/1536/3072 B**，Common 链仍为 **768 B**。

调用链工具原来只接受 `static`，误拒绝 Linux GCC 对参数 push/pop 给出的 `dynamic,bounded`。
现在严格累计该记录的**完整上界**；不忽略这一帧，也不把未知/无界记录当作 0。
[GCC 栈用量定义](https://gcc.gnu.org/onlinedocs/gcc/Developer-Options.html#index-fstack-usage)
明确区分“动态但有完整上界”与“仅知道静态部分”。

新增 CLI 自测证明：512+256=768 接受，513+256=769 拒绝；未知、无界、缺记录、递归、
冲突记录拒绝；Windows 单反斜线与转义反斜线均纳入源文件范围。
ARM 首轮“找不到调用图”的错误也保留，修正路径归一化后才重新验收，不跳过该门禁。

编译器可见协议链不包括未知的外部 Driver/Provider 内部栈、ISR 叠加和 RTOS 保存区；
产品任务栈必须另外预留并实测，不能把 768 B 当作完整任务栈配置。

## 5. 固定 RAM 与布局代价

本轮不是靠放大所有 Profile 存储和队列容量换取通过；声明 Storage 仍为
Nano/Lite/Full **16384/49152/98304 B**，槽数不变。但实际对象确实增长，必须披露。

| 对象（Nano） | 64 位 Host 前→后 | Cortex-M4 前→后 |
| --- | --- | --- |
| Core Node | 7744→7840 B（+96） | 7496→7576 B（+80） |
| Adapter | 1576→1576 B | 1472→1472 B |
| SHA workspace | 112→112 B | 112→112 B |
| Security packet workspace | 1720→1728 B | 1600→1600 B（布局填充容纳） |
| Delivery workspace | 1760→1784 B | 1760→1776 B |
| Bootstrap RX workspace | 440→448 B | 416→424 B |
| Ingress workspace | 5816→5832 B | 5560→5568 B |

Ingress 已包含嵌套 workspace 增量，不能再次相加。Core 增量来自一份 RX view/frame staging，
不是另一条 RX Queue；Adapter 在真实 retire 前继续持有 frame/token，原 Core API lease
防止解码/回调期间并发复用。没有全局可变 scratch 或堆分配。

Host Lite/Full Node 当前为 **22816/77792 B**；Cortex-M4 Lite/Full 当前为 **22400/77072 B**。
ARM 资源来自交叉编译对象符号尺寸，不是板上水位实测。

Persistence ON 的 Composition Runtime：Host **1424→1440 B**、Cortex-M4 **1272→1288 B**。
新增的 16 B 用于生命周期互斥暂存，Persistence OFF 不增加这个 union。
Encode/Decode Request 复用旧空间后，Persistence Owner 的 Host Nano/Lite/Full 仍为
**3800/8456/23912 B**，Cortex-M4 仍为 **3736/8392/23848 B**，均未增长。
新增默认选项和 32 B BLAKE mix 表是只读常量；没有以可写全局数据换栈。

## 6. 最终软件矩阵

以下是最后 Request 别名修复后的实际结果。C/Linux/Rust 三组矩阵均记录源码前后指纹一致，
完成后再次与当前文件逐项核对，漂移为 0。前一轮中途源码变更的批次不作为最终验收。

| 配置 | 实际结果 |
| --- | --- |
| Windows GCC14.2 Full Debug/Release/MinSizeRel，全可选 ON | 各 114/114 PASS |
| Windows GCC14.2 Lite Debug/Release/MinSizeRel，全可选 ON | 各 114/114 PASS |
| Windows GCC14.2 Nano Debug/Release/MinSizeRel，全可选 ON | 各 114/114 PASS |
| Nano Debug/MinSizeRel，全可选 OFF | 各 68/68 PASS |
| 原始失败配置：Nano MinSizeRel，只有 Native ON、其余可选 OFF | 73/73 PASS |
| MSVC19.51 Full Release `/W4 /WX`，全新 UTF-8 构建目录 | 95/95 PASS，含真实 C/C++ 安装、链接、运行 |
| WSL GCC Full `-fanalyzer -Werror` | 严格构建成功，128/128 PASS |
| WSL GCC Nano `-fanalyzer -Werror` | 严格构建成功，128/128 PASS |
| WSL Full ASan/UBSan | 111/111 PASS，Leak/UB 失败即停 |
| WSL Clang18 Full Release | 111/111 PASS |
| WSL Full TSan，并发套件 | 14/14 PASS，无 race 报告 |
| Cortex-M4 GCC13.3 `-Os -Werror`，Nano/Lite/Full | 每 Profile 57 个简化版 C 源文件编译；各 20 项原门限栈/调用链检查全部退出 0 |
| Rust Debug/Release/MSRV1.85 workspace | 各 410 项 PASS，含文档测试 |
| Rust 11 类软件矩阵 | 全部退出 0；fmt、clippy `-D warnings`、32 种 Feature 组合、软/硬浮点 Cortex、双 Codec oracle、资源命令；443 项指纹稳定 |
| 实际 C/Rust Native health 示例 | 两帧逐字节一致：11 B Header + 16 B Payload，总 27 B |

Windows Nano 全 ON 的 Core 同步链实测 Debug/Release/MinSizeRel 为 **704/768/768 B**。
Cortex-M4 三 Profile 的对应链均为 **424 B**，分别对照 768/1536/3072 B 门限。
这些是编译器和相应 ABI 的数值，不能相互代替，更不能称 MCU 任务实际水位。

相对首轮失败示例，Full Release Delivery prepare **352→256 B**、Flow begin_probe
**1168→800 B**、Realtime prepare **480→368 B**，均在原门限内。

最终 C 11 配置机器结果在 `.v6s-stack-audit/matrix/matrix.json`，原始 Native-only 配置
独立保留在 `nano-native-only-final`；Linux 在 `linux-matrix.json`，ARM 在
`arm-final/results.json`，Rust 在 `rust/target/software-validation/latest.json`。
这些本地构建产物不是外审候选提交，也不应全部加入源码库。

### 6.1 保留的环境失败及修复方法

- TSan 无进程级设置时曾出现 `unexpected memory mapping`；最终使用
  `TSAN_OPTIONS=halt_on_error=1 setarch x86_64 -R ctest ... -R concurrency`，
  14 个真实并发测试退出 0。没有修改系统 sysctl/ASLR，也没有把未启动测试计为通过。
- MSVC 旧目录的 `/showIncludes` 中文前缀以错误编码缓存，Ninja 显示依赖为 0，头文件
  改变后出现旧/新布局混用。该目录的失败不用于最终验收，也不能靠多链接一次判定修复。
  本机只安装中文 `clui.dll`，单设 `VSLANG=1033` 无效；最终在同一 `cmd` 会话先
  `chcp 65001`，再调用 `vcvars64.bat`，全新配置、编译、测试。
  已回读正确中文前缀及真实 **25 项** Composition 测试头文件依赖，包含私有布局头；95/95。
  更换编译器语言/编码后必须重新配置干净目录，不能复用错误的依赖缓存。

## 7. 整体代码与架构审查

新鲜独立只读审查以本轮快照为基线，不把继承的巨量 HEAD 差异混入；检查了 Workspace
复用、Provider 解锁生存期、Token/Replay 借用、Core 同步 action、真实调用图和 Owner 边界。
发现并关闭第3节的 Encoder 重要问题；后续窄范围 32 位/工具修正由实施方继续自审。

独立审查补充验证：Security、Core、Adapter、Control Wire 的 `-O2 -Werror` 用例通过；
Common 在 `-O3 -fstrict-aliasing` 通过；4737 例 SHA 与独立 .NET SHA256 一致，
131841 种两段切分一致。该审查不能替代最终全矩阵，也不是外部签字。

针对最终矩阵新暴露的 Persistence 链，同一独立审查者另做 focused delta 复核：Header
抽取、loader 合并、Request union、lifecycle union 未发现新的 Important/Critical；
独立 Nano Release Codec 用例通过，并提供了冗余 buffer 参数的对照栈证据。最后 Request
别名缺口由实施方补正式 RED/GREEN 与全矩阵回归，不能冒称这项也已获得外部签字。

职责未改变：Codec 负责字节与摘要，业务 Owner 负责状态，Coordinator 仍是跨 Owner 编排边界；
`ucn_tx.c` 是同一 Core 的实现拆分，不是新增 Owner。没有引入 Native→Persistence/Cluster
运行依赖，没有 C/Rust 互相链接，也没有将普通 Wire Header 扩大。

## 8. 明确保留的边界

- 普通数据头、Opcode 注册、密码算法、AAD、Record 正文与失败关闭合同不因省栈而减弱。
- 新 Layout/Manifest 要求整体重编译；旧二进制与新私有对象不可混用。
- Windows Nano 紧邻栈上限的配置仍需持续检查，不能把本轮通过视为以后天然安全。
- 不证明硬件时间戳、真实 Flash/Witness 原子性、掉电、实际 WCET/栈水位/功耗/长稳。
- 当前没有提交或推送；本报告的 Host/编译结论不等于生产协议、RMW 或硬件发布完成。

## 9. 结案

本轮明确授权的 Release/Nano 栈和分析器失败已修复；逐项自审、完整软件矩阵以及两轮
不同重点的内部整体/增量审查完成，在本轮整改范围内未发现尚未关闭的 P0/P1。
保留 Nano 部分链恰好达到门限、真实 Provider/ISR/RTOS 栈未纳入等限制，不承诺生产 GO。

计划、任务与 OP 已同步。最终精确代码差异41项，快照逐成员重新校验；C/Linux/Rust 当前
文件与矩阵指纹漂移0。Git HEAD/分支未变、暂存区为空；边界门禁、337项API索引、
157篇/477条链接文档门禁、新增代码尾随空白0和 `git diff --check` 均通过。
本地证据索引为 `.v6s-stack-audit/final-candidate.json`，不冒充外部签字清单。
