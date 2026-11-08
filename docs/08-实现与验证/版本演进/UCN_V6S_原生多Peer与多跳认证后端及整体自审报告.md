# 原生多Peer与多跳认证后端及整体自审报告

日期：2026-11-08。状态：本轮限定后端实现、逐项/整体内部审查及最终稳定软件矩阵完成。
本报告不是外部签字、生产GO或真实MCU验收。

## 1. 基线、授权和范围

在`v6-simplified-rust@099d55908ccc7d46d129bfa416cb015e4cdfc1b5`及继承的单Session
Native认证后端工作树上继续。开始前保存481项源码/工具/示例快照；基线Full Debug118/118。
本轮不把继承的API和H0后端重复算作新实现，不修改旧候选签字哈希，不暂存/提交/推送。

依据[计划10](../../00-项目管理/10-原生多Peer与多跳认证后端实施计划.md)和
[共同合同24](../../10-理论与规划/建议方案/UCN_v6_C_Rust共用合同收口/24-原生多Peer与多跳认证后端共同合同.md)。
新功能是多Peer、独立Origin/Hop认证的数据路径接入已有Native发布订阅；不是新增Topic发现、
可靠传输、RPC、耐久业务、Group、Timed控制或RMW实现。

## 2. 模块怎样连接

```text
应用Publisher/Subscription
          │ 固定Channel、复制队列、逐订阅dispatch
       Native Facade（不依赖Route/Security实现）
          │ Backend SPI
  多Peer数据Coordinator（唯一RX入口和物理退休者）
     ├─ Endpoint：终点Origin Session、Service、完整来源身份
     ├─ Route Owner：原始Origin/Destination、精确Route域/下一跳
     ├─ Neighbor：物理来源/目的、当前Hop Session/Link代际
     └─ Adapter → Driver（Host验证中为替身）
```

A→B→C：AC Session保护Origin，AB和BC Session分别保护Hop。B只核验当前入站Hop、查路、
递减HopLimit并重做出站Hop；不持有AC密钥、不打开Payload、不把自己替代为Sample Principal。
终点同时验证Hop与Origin、各自Replay和当前Endpoint授权，再投影完整原始Principal。
本地副本为LOCAL、零/None Principal，并非伪造“远端认证样本”。

C增加独立私有数据Coordinator和最窄Security C1 Hop/context SPI；已有H0 API不被移除。
Rust增加default-OFF `ucn-runtime/native-routed`，借用实际RoutedNode及原有Owner/计数器，
只增加必要的只读Endpoint view和认证交付元数据，不另建路由/密码/Token状态机。
业务使用相同Publisher/Subscription；不让用户手工判断跳数或为每Peer建立争抢RX的Root。

## 3. 两侧实现支持集不能混淆

| 项目 | C首版工厂 | Rust后端 |
| --- | --- | --- |
| 身份/能力 | 不可变封存Static装配，强制绝对到期 | 复用当前动态Identity/Capability Owner |
| 路由 | 消费真实Route Owner静态/已形成动态路由 | 复用RoutedNode自动认证RREQ/RREP与数据路由 |
| 物理邻居 | 每Neighbor独占Link、精确来源鉴别 | 复用已有共享Link的Peer目的定位 |
| Service登记 | 每Peer最多一个Endpoint | 同Session可登记不同Service |
| Channel保护预检 | 同Service全部登记Endpoint均须达到minimum | 按实际引用Endpoint核验 |
| 热换Endpoint | 不支持，先围栏/排空再新Scope | 本次Native登记同样固定，不自动替换 |

这些是显式产品装配差异，不是两套Wire。C当前并未由本轮实现多邻居自动发现控制面、
动态Parent/Capability接线或通用公开高级factory。不能将真实Route Owner单测中的RREQ/RREP
形成动态路由，描述为C产品自动发现已完成。安装包提供可信Backend SPI；C完整工厂仍为仓内私有装配。

## 4. 逐项自审、区分性测试与修正

| 项目 | 实际问题或反例 | 修正/验证 |
| --- | --- | --- |
| 多Peer/多跳真实接线 | 原H0单Session不能承担唯一多邻居RX与AC/AB/BC分层 | C真实Security/Route/Adapter→Native；Rust真实Foundation激活Owner→RoutedNode→NativeRoot |
| 独立Origin/Hop上下文 | 配置fingerprint不能只因标签相同就声称授权 | C锁内精确context检查；Origin/Hop独立Owner事实，错误层fingerprint拒绝 |
| Route域 | 业务Session generation1不等于Route scope17 | 实际RREQ/RREP回归先失败再修复；精确动态域比较保留 |
| 地址复用/别名 | 合法Session也可能跨Endpoint/Neighbor表使用同地址不同Binding | 初始化全Binding一致性检查，冲突零写拒绝；本地地址不得冒充远端 |
| LOCAL身份 | C本地样本曾误填Principal | 回归先失败再修正为全零，远端仍投影完整认证Principal |
| 本地通道策略 | Rust本地副本曾漏查Endpoint长度/Traffic Class | 真实Owner反例RED→GREEN；本地、远端均按完整eligible谓词 |
| 原始RX时间 | 不能在解码/应用pull时重置年龄 | RoutedNode交付精确Endpoint、protection和原物理时间；测试刻意让捕获早于step |
| 生命周期 | NOT_SUBMITTED、Unknown不能被逻辑超时伪退休 | 原帧/序号重试；真实Holder保留、stop后持续物理排空 |
| 字节级跨语言对照 | 编解码双方同错自洽不能证明共同合同 | C/Rust实际Hop AAD各自匹配同一66 B字面向量，不比较两种假密码算法 |
| 优化构型栈 | 三Profile的MinSizeRel发送函数272 B超过256 B | 只复用既有tx暂存字段缩短跨密码调用变量生命周期，三档240 B；双层成功前prepared仍0 |
| MSVC严格诊断 | 测试gate_owner窄化及循环内pub创建路径触发C4244/C4701 | 数组容量断言后显式窄化；Publisher改为循环后无条件成功创建，不用默认句柄掩盖API漏写 |

C和Rust任务分别进行独立规格/质量审查，重要修正均再次核对。随后新鲜整体跨层审查核对
Origin/Hop、compact地址映射、唯一计数器、Callback授权、期限、退休和模块边界。
整体审查提出的两个C保守装配限制已如实写入合同和使用说明，不冒充全功能对齐。
内部审查不替代用户后续外审，也不是“所有可能缺陷不存在”的证明。

## 5. 内存、协议费用与栈证据边界

- C Native应用声明存储保持Nano2048、Lite6144、Full12288 B；新后端是独立可选装配，
  实测Host context为Nano4600、Lite4600、Full5880 B。每类Endpoint/Neighbor/ForwardDomain
  固定4项；含一个发送Holder、一个RX claim及固定密码工作区，不包含被借用Owner/Driver。
  不把这组数字当成整个节点RAM或MCU最终链接布局。
- Rust借用实际Owner，不复制它们；容量由const generic决定。RoutedNode应用接收项保存原始
  RX时间会增加内部状态，不能说整个运行时零RAM增量。没有生产堆分配或新unsafe。
- Native API/Layout2、Core Layout5不因本轮私有后端改变。普通C1头仍为11/13/15/17 B，
  H1原有16 B Trailer、O1/O2原有16 B Origin Tag；A0+16 B正文的H1每帧59 B，H0直连43 B。
  普通头新增字段0；完整Principal/Topic名/回调句柄不塞进每帧。
- 原256 B函数门禁、Nano768 B旧kernel链门禁均不放宽，不抑制分析器诊断。
  MinSizeRel修补后发送函数240 B，距离256 B为16 B；Cortex-M4 `-Os`该函数120 B。
  Nano Host旧kernel链Debug/Release/MinSizeRel仍为704/768/768 B，后两档无余量。
  旧kernel链门禁并不自动覆盖Native Backend间接函数表和全部可选Owner的完整调用链。
  因此单函数通过或旧kernel链通过，**不能推出新增可选全链总栈≤768 B**。
  Provider/Driver回调内部、RTOS中断嵌套及实机栈水位仍需产品单列预算和验证。

## 6. 最终稳定全矩阵

以下均在最后源码/测试修补后执行；正常测试通过但中途指纹变化的旧批次没有作为最终证明。

| 验证 | 最终结果 |
| --- | --- |
| GCC Full/Lite/Nano × Debug/Release/MinSizeRel | 九配置各120/120 |
| Nano OFF Debug/MinSizeRel | 两配置各68/68 |
| Nano Native-only MinSizeRel | 79/79 |
| MSVC19.51 Full Release `/W4 /WX` | 99/99，C/C++真实安装消费者包含在内 |
| WSL GCC严格fanalyzer Full/Nano | 各134/134，无诊断抑制 |
| WSL ASan/UBSan、Clang | 各115/115 |
| 可运行WSL TSan并发门禁 | 14/14；不是新后端任意多线程调用承诺 |
| Cortex-M4 C99 `-Os -Werror` | 三Profile各59源文件编译、20项原门禁通过；纯编译、未连接MCU |
| Rust workspace默认Debug/Release/MSRV1.85 | 各412项通过 |
| Rust native-routed Debug/Release/MSRV1.85 | 各421项通过，含真实多Peer/多跳Native集成 |
| Rust完整软件矩阵 | 17类命令全退出0，含fmt、严格Clippy、Cortex软/硬浮点与认证feature |
| Rust可裁剪/独立Owner | 21包、36组合，三Optional Owner互不链接 |
| 跨语言Codec/Admission oracle | 91向量、7检查通过；新Hop AAD66 B向量另由实际两侧单测覆盖 |
| Native/Runtime认证feature严格rustdoc | `-D warnings`通过 |

最后执行源码指纹复算并与工作树逐文件比较：C11配置490项、Linux5配置283项、Rust17类459项
各自批次前后稳定，三套当前源码图均匹配。原始JSON和汇总在忽略目录
`.v6s-stack-audit/native-route/final-proof.json`；源码图SHA256为：

```text
C     9c351099d0f03c907079db011f75f02bf3a015098f64b2b34e078e939b0b0e6b
Linux 687dea4411430d9ac824a25bee17a2bbfc58cdf8e2c602d2bf9b2aabf37b7371
Rust  ef408d2bc6a13da315cc3a11fc0874403956e5678d85619f9cb73cb91b95d0f9
```

这些是各自验证范围的源码图指纹，不是整个Git提交或可下载外审候选的签字哈希。
完整日志在忽略目录`.v6s-stack-audit/native-route/`及既有matrix/ARM/Linux/Rust验证目录，
均不作为自动提交产物。收口文档/实现边界/Native边界三项复跑3/3，当前文档门禁
159篇/491链接通过；22个未跟踪文件尾随空白0，`git diff --check`通过（仅CRLF提示），
真实暂存区为空，HEAD和分支均未改变。后续提交必须完整包含新增后端、头文件及测试，不能只提交已跟踪差异。

## 7. 明确未验收的事项

未打开COM/USB、未枚举/控制板卡、未烧录、未操作Probe、未修改Vault；未提交推送。
Host Flash、Witness、Driver和O2可逆测试密码均是fixture，不证明真实Flash原子性、掉电恢复、
生产AES/ChaCha、硬件打点、ESP32吞吐/时延、功耗或24小时长稳。
源端Deadline只约束本机发现/排队/重试，不刷新；普通帧不含全网绝对时间。
中继用原RX时间加本机固定转发生命期及现有授权期限；不得据此宣称端到端Timed控制完成。
