# UCN V6S：C 动态接入与 Native 受控热续接整体自审

基线：`v6-simplified-rust@b9913d4`，本轮候选为该基线上的完整未提交工作树。实施范围见[计划 12](../../00-项目管理/12-C动态接入与原生后端热续接实施计划.md)及[共同合同 26](../../10-理论与规划/建议方案/UCN_v6_C_Rust共用合同收口/26-C动态接入原生后端与受控热续接合同.md)。本文不授予外审、实机或生产放行。

结论：2026-11-14，受限 C Native 动态装配完成；逐项自审、整体代码/架构复核及本轮软件矩阵均已收口。未确认新的软件发布阻断项；本结论仅覆盖下面列出的候选、装配和验证范围。

## 1. 实际接线与边界

| 小项 | 实现与逐项自审 |
| --- | --- |
| 首次 JOIN | 双端 Adapter 真实传送控制帧，绑定固定 Realm、身份、Link 及最初 Challenge 起点；实际 Foundation 双槽提交、Witness 和回读分别作为对应发布及 ACTIVE 的必要条件。验签失败、迟到输入、背压、未知 TX 结果与停止路径不得发布授权或伪造排空。 |
| Session/Capability | 每代 Session 独立绑定精确 ACL；Capability 通过认证帧完成协商，覆盖控制丢包重试、重复 SELECT、撤销、父域变化及过期。旧 Profile 失效后不在同 Session 原地恢复；通过受控新 Session 重新协商。 |
| Reauth/Native 热续接 | 同一 Native Root 和原 Pub/Sub 句柄走 `1→2→3`；旧 scope、待消费事件、未知物理状态、旧 Route/Reverse 必须按各自规则退休。两端耐久记录分歧 `2/1` 和 `1/2` 走真实后继握手，不回退任一方历史。 |
| C/Rust 共用合同 | 均保留当前精确 ACL、来源 scope/代际及序号退休语义；现有 Opcode、认证正文、Record、普通 C1 帧头未改。C 集成链通过不等于 Rust Native 获得同一套自动装配，也不等于 C↔Rust 实物互通。 |

普通数据头新增 **0 B**；既有 A1 认证业务示例仍为 `13 B 头 + 16 B Origin Tag + 16 B Hop Trailer + 4 B 业务 = 49 B`。动态功能默认关闭，动态构建仅面向 Lite/Full；标准 Nano 仍提供静态 Native，不对外宣称支持三域动态 JOIN。地址 Authority、密钥、实际 Driver 和 Provider 由产品装配提供。

产品内部装配顺序：启用 `UCN_FEATURE_NATIVE`、`UCN_FEATURE_PERSISTENCE` 和 `UCN_FEATURE_NATIVE_DYNAMIC`，以私有 Coordinator 驱动 JOIN 直至本地耐久回读和实际排空；接着协商 Capability，再启动已有 Native Backend。换代时先围栏、排空，再 `attach_reauth → stage → commit → detach_reauth`。应用层仍使用原发布、订阅、轮询和分发接口，不直接发 JOIN 或切换密钥。双端部署和配置的可执行例子见[127/128 B 集成回归](../../../tests/simplified/test_native_dynamic.c)，它的 Provider 和 Driver 仅为软件测试夹具。

## 2. 整体代码与架构复核

- 动态 Coordinator 负责编排，不新增第二个授权 Owner。Adapter 每帧只领取、退休一次；各叶消费同一已领取事件，不能自行制造“已认证”事实。跨层交接按精确父域、真实回读、真实排空检查，不靠重置存储恢复权限。
- 复核修复了忙回调期间的退休、借用规则/查询输出别名、Route 的冻结 `LinkRef` 与实际 MTU 漂移、内部 Reauth 复验未计入预算等问题。控制与数据认证选择不互相放宽；实际工作区与持久化均固定容量。
- Native 功能及私有动态叶不进入最小 Core 的安装和链接面。归档符号与依赖检查使用精确清单；公共函数索引按实际安装头重新生成。
- 跨工具链复跑发现并修复：Release 测试误比较 C 句柄填充字节；MSVC 在 `/W4 /WX` 下拦截窄化、符号比较和测试变量遮蔽；MinGW 深层安装暂存路径超出该工具链能力。修复不跳过既有测试，也不放宽告警。
- 最后一次代码复核又在生产 JOIN 父域检查复现了填充字节误判。改为比较 Authority/Binding 所有语义字段及完整摘要；只改变填充字节的正例和改变摘要的反例分别验证通过与拒绝。调整不改变授权判定的字段集合。
- 栈检查继续使用 256 B 单函数和 Nano/Lite/Full 768/1536/3072 B 完整协议链门限。编译器生成的具体 ARM 除法调用单独列入有剩余预算的外部边界，未把产品回调或运行库栈算作零。过期栈图仍被拒绝。

## 3. 复跑证据

初始失败、因源码/检查器修正而过期的批次和修复前后对照均保留在本地 `.superpowers/native-dynamic/`；过期批次不作为最终候选的通过证据。最终批次先重新配置并重建受影响目标，再完整执行 CTest。

| 软件门禁 | 当前候选结果 |
| --- | --- |
| Windows GCC Full / Lite | Debug、Release、MinSizeRel 各 `144/144` |
| Windows GCC Nano | Debug、Release、MinSizeRel 各 `127/127`；不构建动态叶 |
| Windows GCC 裁剪 | Nano Feature-OFF Debug/MinSizeRel 各 `70/70`；Native-only Debug `93/93` |
| Windows MSVC 19.51 Full Debug | `/W4 /WX`，`120/120` |
| WSL GCC `-fanalyzer -Werror` | Full `158/158`、Nano `141/141` |
| WSL ASan/UBSan、TSan | Full `136/136`；15 项定向并发测试全部实际执行并通过 |
| WSL Clang Release | Full `136/136` |
| Rust workspace、native-routed | 17 类通过，482 项输入稳定；默认 Debug/Release/MSRV 各 `421` 项，native-routed 各 `430` 项；严格 Clippy、fmt、rustdoc、两个 Cortex-M 目标、36 个功能组合、91 个 Codec 向量及 7 项 Admission oracle 通过 |

本地证据批次：Windows `windows-recheck-20261114T094613Z-321be3487419`、Linux `linux-recheck-20261114T094611Z-a70ac7708580`、MSVC `msvc-recheck-20261114T094610Z-71dc955bdeec`、ARM `arm-matrix-20261114T094608Z-661ee9f07029`。C 系列以同一 567 项 `source-before.json` 为输入，清单文件 SHA256 为 `5B6D83CA29E5298B9F0F8E67ECACBD411CCE2E98C03395C3D962D73A5B731BF2`，对当前工作树独立重算匹配。Rust `rust/target/software-validation/latest.json` 文件 SHA256 为 `22EEC19446F9C7ABBDF12B867D55271B50C214E2B0A23111BDFF5699254B13BE`；未运行 ESP32 目标验证。

四套 C 系列批次的输入输出清单一致，所有配置退出码为 0。缩短 Windows 构建/安装暂存根只用于避免本机 MinGW 深路径失败；安装消费者仍实际执行 C/C++ include、link 和 run。

Windows GCC 的完整 Native 协议链峰值（单位 B，按 Debug / Release / MinSizeRel）：

| Profile / 装配 | Debug | Release | MinSizeRel | 原门限 |
| --- | ---: | ---: | ---: | ---: |
| Nano 静态 H0 | 736 | 752 | 752 | 768 |
| Nano 静态 H1 | 768 | 752 | 760 | 768 |
| Lite 动态 | 1296 | 1304 | 1512 | 1536 |
| Full 动态 | 1296 | 1304 | 1512 | 3072 |

Cortex-M4 / GCC 13.3.1 `-Os` 仅编译：Nano/Lite/Full 分别编译 60/66/66 个生产源，17 项栈检查均通过。H0/H1 峰值分别为 Nano 392/392、Lite 384/752、Full 392/760 B；Lite/Full 动态为 752/760 B。Nano 不构建动态叶，显式启用会在 CMake 配置时拒绝。Host Nano MinSizeRel 的最紧 `driver + lock` 预算仅 16 B；Lite MinSizeRel 动态协议链距门限仅 24 B。真实产品依然必须验证各外部调用的联合预算。

## 4. 尚未覆盖

真实 MCU 任务栈水位、Flash/Witness 掉电、生产密码 Provider、物理 Bearer、C↔Rust 实物互通、性能及长稳均未验收。本轮未操作实机，未暂存、提交或推送。
