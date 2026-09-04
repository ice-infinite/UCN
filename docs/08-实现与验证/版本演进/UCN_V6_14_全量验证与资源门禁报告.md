# UCN V6-14 全量验证与资源门禁报告

## 1. 结论与证据等级

V6-14 当前完成了可在本机重复的软件验证框架，但没有完成参考产品实机、真实掉电、24 小时
长稳、MSVC 和可运行的 TSan。故结论严格分为：

- 软件验证资产：`SELF REVIEW PASS`；
- 当前可用 GCC/Clang/ASan/UBSan/Analyzer 门禁：`PASS`；
- MSVC、TSan：`ENVIRONMENT HOLD`；
- ESP32-S3、UART/RS-485、ESP-NOW、CAN/CAN-FD、USB、Flash、功耗和 24 h：
  `HARDWARE HOLD`；
- V6-14 整体：`PARTIAL / SOFTWARE GATES COMPLETE`；
- UCN 1.0 RC：`NOT AUTHORIZED`。

Host 单元测试、模拟器和静态分析只能证明相应软件合同，不能替代 MCU 上的 ISR/DMA、Flash
掉电、真实时间戳、链路误差、吞吐、尾延迟或功耗证据。

## 2. 可执行验证资产

| 资产 | 作用 | 失败含义 |
|---|---|---|
| `tools/v6/check_v6_boundaries.py` | 检查模块、固定内存、依赖和默认关闭边界 | v6 源码边界或构建隔离漂移 |
| `tools/v6/run_v6_software_matrix.ps1` | Windows GCC 六组配置矩阵 | Profile、优化级别或裁剪配置不一致 |
| `tools/v6/run_v6_sanitizers.sh` | WSL ASan/UBSan 与 Analyzer | 内存、未定义行为或静态路径风险 |
| `tools/v6/ucn_v6_scale_sim.c` | 真实 Cluster Owner 的 1k/10k 模拟 | 固定容量、持久恢复或 Authority Fence 失败 |
| `tools/v6/ucn_v6_resource_report.c` | 输出 Manifest 与全部公开静态 Storage 上界 | 资源合同或配置 Hash 失配 |
| `tools/v6/validate_v6_evidence.py` | 校验 commit、artifact 与 SHA-256 绑定 | 证据不完整、被替换或错误宣称 release-ready |

所有验证脚本均失败关闭。边界检查只扫描 `src/v6` 与 `include/ucn/v6` 的运行时源码；Host-only
模拟器可以使用堆内存构造大规模场景，但生产 v6 archive 禁止 `malloc/calloc/realloc/free/alloca`。

## 3. 分项自审

### 3.1 Source Boundary

检查内容包括：

1. v6 运行时 `.c/.h` 集合非空；
2. 生产 v6 源码不调用堆分配；
3. v6 不包含 `ucn/v6/` 之外的旧公共头；
4. 源码没有 `TODO/FIXME` 未完成标记；
5. Cluster 不依赖 Realtime，Realtime 不依赖 Cluster；
6. Config、Identity、Wire、Message、Owner、Security、Capability、Route、QoS、Transfer、
   Realtime、Cluster、Adapter 目标均存在；
7. V6-13 的 UART、Wi-Fi、CAN、USB、FreeRTOS 文件分离存在；
8. 当前过渡阶段 `UCN_BUILD_V6_EXPERIMENTAL` 仍为 default-OFF，且默认 Core/Cluster archive
   中 `ucn_v6_*` 符号为 0。

结果：`PASS`。V6-15 切换 v6 为唯一生产面后，第 8 条必须随发布架构改写，不能继续把
default-OFF 当作完成。

### 3.2 编译器、优化和配置矩阵

Windows GCC/Ninja 实际结果：

| 配置 | v6 门禁 |
|---|---:|
| Full Debug | 19/19 |
| Full Release | 19/19 |
| Lite Debug | 19/19 |
| Nano Debug | 19/19 |
| Service OFF | 19/19 |
| 最小 Adapter：1 Link、1 RX、1 TX、64 B Frame | 4/4 |

此外，当前完整过渡树 Full Debug CTest 为 `77/77`；带中文的构建目录使用 Ninja 完成 v6
`17/17`。MinGW Makefiles 对中文源码根存在生成器限制，中文门禁统一使用 Ninja。

WSL 结果：

- GCC 13 ASan/UBSan：20/20；
- GCC 13 `-fanalyzer`：18/18；
- Clang 18 `-Wall -Wextra -Werror`：18/18；
- pthread 双线程双 Link Adapter：包含于上述矩阵并通过。

本机未安装 MSVC，不能把旧记录或其他机器的结果冒充本提交证据。

### 3.3 模型、属性、Fuzz 与安全负向

- Wire 使用固定 seed 的 4096 次 raw fuzz；任何被接受输入必须能 canonical 往返；
- 各模块测试覆盖坏 Version/Schema/Flag/Selector/Tag、旧 Binding/Session、Serial 回退、
  输出不写回和 Provider 假成功；
- Message Journal、Identity、Realtime、Cluster 均覆盖 reload 后的 fail-closed 状态；
- QoS 覆盖 per-Class/per-Source/per-Flow 配额与 Budget 注入；
- Transfer 覆盖丢片、乱序、重复、SACK/Credit 丢失和 Path/Session 变化；
- Adapter 覆盖同步 completion、cancel 竞争、Link reopen 和迟到 Generation。

本项为模型/软件证明，不等于真正密码算法的侧信道评估、无线攻击测试或 Flash 电源切断。

### 3.4 规模与恢复

`ucn_v6_scale_sim` 不是虚构状态计数器，而是为每个逻辑簇创建真实 Cluster Owner、Store 和
callback gate。测试分别运行 1,000 与 10,000 个逻辑节点，以最多 16 个成员组成一个簇：

- 创建并持久化 Cluster Epoch；
- 逐个接收认证成员并建立 quorum；
- 检查 Head Authority；
- 每 17 个簇执行一次持久 Record reload；
- reload 后没有恢复 volatile lease 时，Authority 必须为 false。

1,000 和 10,000 节点两组均通过。它证明算法和固定单簇容量可以分组扩展，不证明一块 MCU
同时保存 10,000 个节点，也不证明真实无线网络已在该规模收敛。

## 4. 默认资源上界

资源工具输出当前 Full 默认公共 Storage 上界：

| Owner | 字节 |
|---|---:|
| Identity Authority | 2,624 |
| Bootstrap | 4,864 |
| Operation Allocator / Journal | 256 / 3,072 |
| Protocol Owner | 1,024 |
| Security | 16,384 |
| Capability | 9,024 |
| Route | 70,656 |
| Metric / QoS | 9,216 / 36,864 |
| Transfer | 79,872 |
| Realtime | 3,840 |
| Cluster | 22,272 |
| Adapter | 48,128 |
| FreeRTOS Port | 2,048 |

这些数值是为避免私有结构变化破坏调用方而保留的编译期安全上界，不是要求每个产品同时分配
全部对象。可选 Feature 不启用时不应声明对应 Storage。最小 Adapter 配置的上界为 4,864 B。
V6-15 发布前仍须建立正式 Nano/Lite/Full 产品 Manifest，明确各 Profile 启用模块和容量；
仅在 CMake 中写 `UCN_PROFILE=NANO` 而不改变 v6 Product Manifest，不构成资源裁剪证据。

## 5. 并发与 TSan 边界

共享 Gate、task blocking lock、ISR try-lock、Buffer token 和 completion 生命周期已有双线程
Host 回归。GCC TSan 在当前 WSL 上两次都于测试逻辑开始前报告
`ThreadSanitizer: unexpected memory mapping`；Clang 18 环境缺少 TSan runtime。因此当前只
声明 pthread 并发功能测试通过，不声明 TSan 通过。发布证据必须在可工作的 TSan 环境重跑，
或由目标 RTOS/SMP 形式验证提供等价且经外审接受的证据。

## 6. 实机证据合同

实机证据统一存放于 `docs/08-实现与验证/实机证据/V6/`。每个 PASS 必须绑定：

- 40 位 Git commit；
- 固件 SHA-256 与逐板刷入记录；
- 板号、芯片、SDK、编译器、供电、接线、Bearer、引脚和速率；
- 测试开始/结束时间与阈值；
- 原始日志文件及 SHA-256；
- 断电、复位、断链、重连、丢包、乱序、并发、温度和功耗条件。

校验器要求 `release_ready` 与“全部 required gate 为 PASS”完全一致。HOLD/FAIL 项不得附带
伪证据路径；PASS 项的 artifact 缺失或哈希不符必须失败。

## 7. 未关闭门禁

下列项目需要真实环境，当前不能通过继续写 Host 测试来关闭：

1. ESP32-S3 N16R8/N8R8 固件和真实 RAM/栈/Flash 尺寸；
2. UART/RS-485 1～5 跳、3 Mbit/s、高负载 Q0～Q3 与 T32～T8K；
3. ESP-NOW 及 UART+ESP-NOW 混合路径；
4. Classic CAN、CAN-FD、USB 的 Carrier/ISR/DMA/completion；
5. Realtime 硬件 timestamp、asymmetry 和 uncertainty 实测；
6. Flash 双槽撕裂写、掉电窗口和启动恢复；
7. 断链/reopen 与迟到事件；
8. CPU、栈、RAM、吞吐、P99/P999 延迟、功耗和 24 小时长稳；
9. MSVC 和可运行的 TSan 工具链。

因此 V6-15 可以继续准备发布面清理、denylist 和文档，但不得生成 `UCN 1.0 RC`、发布 Tag
或生产 GO，直至上述适用门禁由同一候选提交的证据关闭。
