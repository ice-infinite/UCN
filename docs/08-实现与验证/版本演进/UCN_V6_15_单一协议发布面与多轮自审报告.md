# UCN V6-15 单一协议发布面与多轮自审报告

## 1. 结论

V6-15 已完成当前可执行的软件发布面收敛：仓库顶层构建、公共头、源码、测试、工具、安装包、
Profile/Feature Manifest 和当前用户文档只暴露 v6。V5 通过不可变提交、远端分支和 Tag 保存，
不再以可编译源码混入 v6 工作树。

当前状态是：

```ini
V6-01..V6-13 = SOFTWARE IMPLEMENTATION COMPLETE / WAIT UNIFIED EXTERNAL REVIEW
V6-14         = SOFTWARE SCOPE COMPLETE / HARDWARE AND TSAN HOLD
V6-15         = SOFTWARE RELEASE SURFACE COMPLETE / WAIT UNIFIED EXTERNAL REVIEW
UCN 1.0 RC    = NOT CREATED / NOT AUTHORIZED
```

这不是生产放行。真实密码系统、目标板、掉电、性能、时间精度和长稳仍必须绑定最终候选提交。

## 2. V6-15 子任务与逐项自审

### 2.1 V6-15-01：冻结可恢复的 v5 边界

- v5 快照提交：`c554239f0940d77a0a0a3baef81c6742be1a0c90`；
- 远端分支：`v5-final-experimental`；
- Tag：`v5.0.0-experimental-final`；
- 当前开发分支：`v6-development`。

自审：三个引用均存在且指向同一 v5 实验基线；文档明确它不是发布版。旧源码不复制进
`archive/*.c`，恢复依赖 Git，而不是在当前编译树保留双协议。

### 2.2 V6-15-02：建立 v6-only 构建图

顶层 CMake 项目版本为 6.0.0，只构建 Config、Identity、Wire、Message、Owner、Security、
Capability、Route、QoS、Transfer 以及可选 Realtime、Cluster、Adapter。公开聚合目标为
`UCN::ucn`。

自审：旧 CMake Option/Target 已删除；所有目标名、archive 名和公开符号均在 v6 命名域；
Feature OFF 时对应目标不进入聚合接口。

### 2.3 V6-15-03：真实 Nano/Lite/Full 与 Feature Manifest

Profile 不是三套协议，而是同一 Wire/安全/语义下的容量预设：Nano 默认最大 256 B Transfer，
Lite 为 2 KiB，Full 为 8 KiB。Realtime、Cluster、Adapter 可在编译期独立裁剪。

自审：Manifest API=1；首次发布面收敛时 Storage Layout 为 2，V6X-A01～A11 后为 3，
Message Witness、Realtime Domain Record、Owner 锁和 Capability 流式接口整改后为 4；
本轮可信父代际、Stack Owner、Capability/Realtime/Cluster/FreeRTOS 公开存储变更后为 6。
中间 Layout 5 与所有旧 Layout 一样只允许明确拒绝。
Profile、Feature bits 与容量进入 Layout Hash；非法
Profile、跨 Translation Unit 配置失配、对象容量/对齐错误均由编译或初始化门禁拒绝。

### 2.4 V6-15-04：Compatibility Removal Manifest 销账

本轮从发布目录移除 241 个已跟踪旧资产：68 个旧 `src` 文件、62 个旧公共头、94 个旧测试、
17 个旧工具。原 v5 官方/用户/阅读/调用树及 reference/evidence/experimental 资料移动到
`docs/archive/v5-user-facing-20260905/` 并加历史边界说明。

自审：CR-001～CR-028 全部为 `REPLACED`；`src/`、`include/ucn/`、`tests/`、`tools/` 的当前
发布范围只剩 v6 资产；旧输入拒绝、Version/Magic/Schema、Feature Manifest 等演进安全门保留。

### 2.5 V6-15-05：安装包与外部消费者

新增 CMake package config/export。安装只输出 v6 头、v6 libraries 和 `UCN::ucn`；独立工程
通过 `find_package(UCN 6 CONFIG REQUIRED)` 编译、链接并运行。

自审：GNU `nm` 与 MSVC `dumpbin` 两条 archive 检查路径通过；安装 inventory 中旧头、旧库、
旧 target 和旧符号为零。中文主构建通过；MinGW consumer 使用 ASCII staging 规避 GNU ld
自身的 Unicode archive 绝对路径限制，并在报告中明确该限制。

### 2.6 V6-15-06：当前文档重写

重写根 README、文档导航、13 个官方主题、6 个用户手册入口、6 个源码阅读入口，并机械生成
公共函数签名索引；本次整改后的当前索引为 241 项。历史建议和 v5 说明不再充当当前用户入口。

自审：当前文档链接检查通过；API 索引加入 CTest 漂移门禁；发生冲突时以公共头/实现、CMake/
测试、官方文档、提交绑定报告、任务记录、历史文档的顺序裁决。

### 2.7 V6-15-07：发布门禁可执行化

新增/强化源码目录 denylist、archive symbol gate、当前文档 gate、API 索引 gate、安装 consumer、
Profile 矩阵、Sanitizer/Analyzer、规模模拟、资源报告和证据 validator。

自审：检查器自身同时进入 CTest；旧文件、旧 include、旧 target、旧全局符号或索引漂移会使
构建门禁失败，不依赖人工记忆。

### 2.8 V6-15-08：成熟度与发布阻断

自审没有把“软件矩阵全绿”转换成 RC。任务表、README、官方测试文档、本报告都把硬件、
TSan、真实密码 Provider、Flash 掉电、性能和最终外审列为阻断项。

## 3. 全体自审第一轮：构建与行为

| 环境 | 结果 |
|---|---:|
| Windows GCC Full Debug | 25/25 |
| Windows GCC Full Release | 25/25 |
| Windows GCC Lite Debug | 25/25 |
| Windows GCC Nano Debug | 25/25 |
| Windows GCC Nano Feature-Off | 20/20 |
| Windows GCC Nano Realtime-only | 21/21 |
| Windows GCC Nano Cluster-only | 23/23 |
| Windows GCC Nano Adapter-only | 21/21 |
| Windows MSVC 19.51 Full Release | 25/25 |
| Windows MSVC 19.29 Full Release（本轮全新 VS2019 构建） | 25/25 |
| Windows 中文构建目录 GCC/Ninja Full Release | 25/25 |
| WSL GCC ASan/UBSan | 26/26 |
| WSL GCC `-fanalyzer` | 26/26 |
| WSL Clang 18 `-Wall -Wextra -Werror` | 26/26 |

结论：Profile、Feature、优化级别、编译器、Sanitizer、静态分析、并发、安装消费和文档门禁在
当前可用环境中一致通过。

## 4. 全体自审第二轮：发布边界

- Source boundary：`PASS`；
- v6 archive 名称/符号：`PASS`；
- 旧源码/头/测试/工具目录：`0`；
- 安装包旧头/库/target：`0`；
- 公共 umbrella：`PASS`；
- API 索引：241 个函数，`CURRENT`；
- 当前文档本地链接：`PASS`；
- Compatibility Manifest：28/28 `REPLACED`；
- `git diff --check`：`PASS`（CRLF 提示不属于空白错误）。

结论：当前工作树不再是“v5 产品 + default-OFF v6 实验库”，而是单一 v6 软件发布面。

## 5. 全体自审第三轮：安全、恢复与容量

- Identity/Binding/Session/Key/Route/Path/Time/Cluster Generation 均有 Owner、高水位和失败关闭；
- Provider 操作执行 submit 后 load 回读，不接受只返回成功的假持久化；
- Security/JOIN 是 Capability 和生产收发的前置门；
- Route Candidate、Operation Journal、Transfer、Realtime、Cluster 和 Adapter 均覆盖半提交、
  迟到事件、重放、超时和容量耗尽；
- Nano/Lite/Full 使用独立静态上界并由资源工具输出；
- 1k/10k 模拟使用真实 Cluster Owner/Store，但不越权声称硬件容量。

本轮并非只复跑既有测试，而是继续做跨模块对抗检查，并在最终矩阵前关闭了以下内部发现：

1. Cluster Tunnel 曾用整个结构体 `memcmp` 判定幂等，Release 下可能受 padding 影响；现改为
   Route Domain、Path Capability 与 Tunnel 字段逐项比较，并加入脏 padding 回归。
2. Realtime Domain 绑定曾在发现本地 generation replay/容量不足前推进 durable high-water；
   现先完成零副作用本地 preflight，再进入 Provider callback gate。
3. Capability Path 安装曾可接受调用方抬高的能力或不完整合法域；现独立校验全部字段，并以
   当前已认证 Peer Capability 为上界。
4. Bootstrap per-Link 预算曾只比较 generation，两个不同 Link 的相同 generation 会相互占用；
   Key 和预算域现统一为精确 `{link_id,generation}`。
5. Route、Transfer、Realtime 与 Cluster 的公开代际/枚举入口补齐 no-wrap 和负枚举门禁，
   阈值以上输入、未知 bit 和跨编译器负枚举均保持输出/持久状态不变。

结论：第三轮发现的问题均已转为确定性回归；修复后没有遗留已知软件 P0/P1。该结论仍等待
最终干净矩阵和独立外审确认。

## 6. 全体自审第四轮：跨生成器与可选模块隔离

第四轮不再只复用 Ninja/GCC 的成功结果，而是从 Visual Studio 生成器和单 Feature 组合检查
验证基础设施本身是否会给出假绿灯。该轮发现并关闭了两项门禁实现问题：

1. archive 检查器原来只扫描构建根目录，Visual Studio 把 `.lib` 放在 `Release/` 子目录时会
   错报“没有 archive”；现改为递归扫描 `libucn*.a` 与 `ucn*.lib`，GNU `nm` 和 MSVC
   `dumpbin` 继续分别验证 archive 名称与公开符号域。
2. 旧 CMake surface 检查器原来调用 `cmake --build --target help`，该命令不适用于 Visual
   Studio 生成器；现改读生成器无关的 `CMakeFiles/TargetDirectories.txt`，同一 denylist
   可在 Ninja、Makefile 与 Visual Studio 上执行。

修正后重新得到：Visual Studio 2019 / MSVC 19.29 Release Full `25/25`；Nano
Realtime-only `21/21`、Cluster-only `23/23`、Adapter-only `21/21`。三种组合分别证明
对应 Feature 可独立进入构建和安装闭包，未启用的另两个模块不被隐式链接。此前记录的
MSVC 19.51 证据继续保留，本轮 19.29 是另一套全新生成器/工具链复验，不互相替代。

结论：四轮自审后未发现遗留已知软件 P0/P1；验证脚本的跨生成器行为和可选模块正交性均已
形成可重复门禁，等待最终统一外审。

## 7. 保留 HOLD

1. TSan runtime 不可用；
2. ESP32-S3 和目标 RTOS 的构建、刷写、ISR/DMA、断链/reopen；
3. UART/RS-485、ESP-NOW、CAN/CAN-FD、USB 的真实多跳吞吐与尾延迟；
4. Realtime 硬件时间戳、非对称误差和 uncertainty 校准；
5. Flash 撕裂写、断电、重启恢复和密钥/地址/簇高水位；
6. CPU、栈、RAM、Flash、功耗、P99/P999 与 24 小时长稳；
7. 生产密码 Suite/Key/随机数 Provider 和安全评估；
8. 最终统一外审 P0/P1 归零。

上述任一 required gate 未通过前，均不得创建 1.0 RC、发布 Tag 或生产 GO。

## 8. 外审入口

外审应以当前完整工作树为一个整体，重点检查：

1. `include/ucn/v6` 与 `src/v6` 的 Wire/AAD/状态机一致性；
2. CMake install/export 是否存在旧符号或 Feature 泄漏；
3. Profile 容量、Layout Hash 和跨 TU/库失配；
4. Compatibility Removal Manifest 28 项是否真实归零；
5. Message/Route/Transfer/Realtime/Cluster/Adapter 的失败原子性；
6. 所有 PASS 是否能由报告中的命令独立复现；
7. HOLD 是否被任何 README、任务状态或 API 注释错误表述为完成。

本轮外审提交基线为 `v6-development` 分支、基准提交
`48b23270d403add87c1dea7e60afe791c6e37dba` 加当前完整未提交工作树。审计者必须把
`git status --short` 中的修改、删除和未跟踪文件一起纳入范围，不能只审 `git diff`；真实 Git
暂存区为空。v5 恢复边界固定为 `c554239f0940d77a0a0a3baef81c6742be1a0c90`、分支
`v5-final-experimental` 和 Tag `v5.0.0-experimental-final`。

建议外审首先执行：

```powershell
pwsh -File tools/v6/run_v6_software_matrix.ps1
python tools/v6/check_v6_boundaries.py --root .
python tools/v6/check_v6_current_docs.py --root .
python tools/v6/generate_v6_api_index.py --root . --output "docs/源码阅读指南/06-公共函数签名索引.md" --check
git diff --check
```

Linux Sanitizer/Analyzer 入口为：

```bash
bash tools/v6/run_v6_sanitizers.sh /tmp/ucn-v6-external-review
```

当前主机没有形成可签字的 ESP32 硬件证据；Clang 18 TSan runtime 不存在。WSL GCC TSan
当前源码可编译，Owner/Adapter 定向用例通过，但 Identity 遇到 `unexpected memory mapping`，
Message 在整组 CTest 中异常退出且隔离重跑通过，运行环境仍不稳定。
因此本轮提交的是统一**软件外审**，不是 TSan、硬件、掉电、Realtime 精度、性能长稳或
1.0 RC 签字。

## 11. 首轮统一外审后的整改状态

外审提出的 V6X-A01～A11 已全部完成软件整改与逐项自审。Wire 基础长度因拆分
Origin/Hop Sequence 先固定为 `40/42/44/46 B`；后续统一架构自审为使 Wire Hop
Limit 与 16-bit 累计跳数域唯一一致，当前已破坏性更新为 `41/43/45/47 B`。Storage Layout 因 Security/Identity/Cluster
持久合同、Profile 头边界与 Nano Frame 容量变更先升级为 3，随后因 Message Witness、Realtime
完整 Domain Proposal Record、Owner task/ISR 锁合同和 Capability 流式归约升级为 4；本轮
可信父代际、统一 Stack Owner 和上层精确失效合同最终升级为 6。当前发布面仍只有 v6，不新增旧版
解码、运行期 fallback 或兼容桥。

本轮累计执行十轮全局自审：序号与安全所有权、持久化与重启、跨模块认证语义、Profile/资源/
编译器/发布面、并发门禁/工具链反证、发布表面/文档/证据一致性、全所有者反回退复审，以及
追加整改后从 Identity 到 Adapter 的从头复审、精确失效/父代际/失败原子性复审、Wire 数值域与
canonical 认证复审。软件矩阵、MSVC/GCC/Clang、ASan/UBSan 和
Analyzer 全部通过；TSan 只获得 Owner/Adapter 定向通过，整体仍因当前 WSL runtime 不稳定而 HOLD。
当前没有已知开放的软件 P0/P1；正式状态仍是 `WAIT EXTERNAL RE-REVIEW`。硬件与掉电门禁保持
原样，不能据此生成 1.0 RC。

## 12. V6X-S01～S15 追加整改后的最终软件门禁

追加全体自审先关闭 Message Journal 独立反回退 Witness、Realtime Proposal Identity ABA、Owner
退出锁失败、Handover 非 Voter 目标和无界 hop 数组，再关闭可信父代际、精确 Link→Session
失效、单一 Route Owner、Message/Transfer 生命周期、QoS 公平、Realtime/Cluster 权威撤销、
16-bit Hop 域与 Route/Path 双上下文 AAD。最新矩阵为：Windows GCC Full
Debug/Release、Lite/Nano Debug 均 `26/26`；Nano Feature-Off `21/21`，Realtime-only/
Cluster-only/Adapter-only 分别 `22/22`、`24/24`、`22/22`；MSVC 19.29 Release `26/26`；
WSL ASan/UBSan、`-fanalyzer`、Clang 18 Werror 均 `27/27`。完整反例、资源和源码哈希见
[V6X 整改与跨模块自审报告](UCN_V6_外审V6X_A01_A11整改与跨模块自审报告.md)。
