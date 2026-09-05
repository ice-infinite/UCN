# UCN V6-14 全量验证与资源门禁报告

## 1. 当前结论

V6-14 已完成当前主机能够执行的软件验证体系，并在 V6-15 的单一 v6 发布面上重新执行。
结论必须按证据等级拆开：

- GCC、MSVC、Clang、Debug/Release、Nano/Lite/Full、Feature ON/OFF：`PASS`；
- ASan/UBSan、GCC `-fanalyzer`、中文构建目录、安装包消费者、1k/10k 模拟：`PASS`；
- pthread 并发功能回归：`PASS`；
- TSan：`ENVIRONMENT HOLD`，Clang 18 缺少 runtime，不能写成通过；
- ESP32-S3、真实 UART/RS-485、ESP-NOW、CAN/CAN-FD、USB、Flash 掉电、功耗和长稳：
  `HARDWARE HOLD`；
- V6-14：`PARTIAL / SOFTWARE SCOPE COMPLETE`；
- UCN 1.0 RC：`NOT AUTHORIZED`。

Host 结果能证明接口、状态机和固定内存合同，不能替代 MCU ISR/DMA、真实时钟误差、Flash
撕裂写、吞吐、尾延迟、功耗或 24 小时运行证据。

## 2. 可重复执行的门禁

| 资产 | 检查内容 |
|---|---|
| `tools/v6/check_v6_boundaries.py` | 单一 v6 源码面、无堆、无旧头/目标/选项、模块依赖 |
| `tools/v6/check_v6_archives.py` | GNU `nm` 或 MSVC `dumpbin` 检查 archive 名称和 `ucn_v6_*` 符号域 |
| `tools/v6/check_v6_current_docs.py` | 当前文档、本地链接和版本标记 |
| `tools/v6/generate_v6_api_index.py --check` | 公共函数索引与头文件一致，防止手册漂移 |
| `tools/v6/run_v6_software_matrix.ps1` | Windows GCC 的 Profile/Feature/优化矩阵 |
| `tools/v6/run_v6_sanitizers.sh` | WSL ASan/UBSan 和 `-fanalyzer` |
| `tools/v6/ucn_v6_scale_sim.c` | 真实 Cluster Owner 的 1k/10k 逻辑节点模拟 |
| `tools/v6/ucn_v6_resource_report.c` | Manifest、Layout Hash 和公共 Storage 上界 |
| `tools/v6/test_v6_install_consumer.cmake` | 安装后由独立工程 `find_package` 并链接 `UCN::ucn` |
| `tools/v6/validate_v6_evidence.py` | commit、artifact、SHA-256 和 release-ready 声明一致性 |

生产 `src/v6` 和 `include/ucn/v6` 禁止 `malloc/calloc/realloc/free/alloca`。规模模拟可在 Host
侧创建大量对象，但这种测试辅助行为不得进入安装库。

## 3. 多轮软件验证结果

### 3.1 Windows GCC/Ninja

V6-15 最终发布面矩阵：

| 配置 | CTest |
|---|---:|
| Full Debug | 26/26 |
| Full Release | 26/26 |
| Lite Debug | 26/26 |
| Nano Debug | 26/26 |
| Nano，Realtime/Cluster/Adapter OFF | 21/21 |
| Nano，仅 Realtime ON | 22/22 |
| Nano，仅 Cluster ON | 24/24 |
| Nano，仅 Adapter ON | 22/22 |

每组均包含适用的源码边界、archive 符号、当前文档、API 索引、配置合同、模块测试、资源报告、
证据校验和安装包 consumer。关闭 Feature 后对应源码、头引用和测试目标不进入链接闭包。

### 3.2 MSVC 与中文目录

- Visual Studio Build Tools 18、MSVC 19.51 的早期 v6-only 基线：`25/25`，本轮未重复使用该
  工具链；
- 当前整改候选由 Visual Studio 2019、MSVC 19.29、Release Full 重建：`26/26`；
- 中文构建目录、GCC/Ninja 的 v6-only 基线：`25/25`；当前候选仍由路径无关的相同 CMake
  门禁覆盖，但提交前若要把 Unicode 路径列为正式签字证据，仍应重新执行；
- 安装包 consumer 同时通过 GCC 和 MSVC；
- MinGW `ld` 不能打开带非 ASCII 的 archive 绝对路径，因此 Windows consumer 门禁把安装
  staging 放入确定的 ASCII 临时目录。UCN 主源码和主构建仍实际位于中文目录；该工具链
  限制不被误写成“任意 Unicode 安装前缀均支持”。

### 3.3 WSL GCC/Clang

- GCC 13 ASan/UBSan Full：`27/27`；
- GCC 13 `-fanalyzer` Full：`27/27`；
- Clang 18 `-Wall -Wextra -Werror` Full：`27/27`；
- Windows GCC 深度告警 `-Wconversion -Wsign-conversion -Wshadow -Wformat=2 -Wundef`：
  `26/26`；
- Windows GCC ISO C99 `-pedantic-errors`：`26/26`；
- pthread 双线程 Adapter 回归包含在三组 Linux 矩阵内。

### 3.4 TSan

当前 Clang 18 环境缺少 `libclang_rt.tsan-x86_64.a`。因此只能记录并发功能测试通过，不能声称
TSan 通过。后续必须在可工作的 TSan 工具链重跑，或提供经外审接受的目标 RTOS/SMP 等价证据。

## 4. Profile 与静态资源

三个 Profile 编译同一协议并都能解析 A0～A3；区别是固定容量和默认最大 Transfer 档。下表
是 64-bit Host 上公共 Storage 的编译期安全上界，不是运行时堆用量，也不要求产品同时创建
全部对象。

| Owner | Nano | Lite | Full |
|---|---:|---:|---:|
| Identity Authority | 1,040 | 1,568 | 2,624 |
| Bootstrap | 2,656 | 4,800 | 9,088 |
| Operation Allocator | 256 | 256 | 256 |
| Operation Journal | 1,152 | 1,792 | 3,072 |
| Stack Owner | 2,048 | 2,048 | 2,048 |
| Security | 4,288 | 7,680 | 16,384 |
| Capability | 3,536 | 6,048 | 11,072 |
| Route | 11,776 | 29,696 | 70,656 |
| Metric | 2,048 | 5,120 | 9,216 |
| QoS | 7,424 | 19,456 | 36,864 |
| Transfer | 14,592 | 29,696 | 79,872 |
| Realtime | 3,136 | 3,328 | 4,608 |
| Cluster | 12,032 | 16,192 | 24,832 |
| Adapter | 7,936 | 17,920 | 48,128 |
| FreeRTOS Port | 3,072 | 3,072 | 3,072 |

Manifest API 为 1，当前 Storage Layout 为 6。V6X-A01～A11 时曾为 3，Message Witness、
Realtime 完整 Domain Proposal Record、Owner 锁合同和 Capability 流式接口曾推进为 4；本轮
可信父代际、Stack Owner 与上层精确失效合同最终破坏性升级为 6，中间 Layout 5 不对外兼容。
Profile、Feature bits 和所有影响私有布局的容量均
进入 Layout Hash；头文件配置与已编译库不一致时必须在初始化前失败。

## 5. 模型、负向和规模验证

- Wire：精确 Golden、错误长度/版本/Flag/Selector/CRC/Tag、输出不写回和固定 seed fuzz；
- Identity/Security：Binding/Session ABA、旧 Key、重放、Provider 假成功、Bootstrap 预算；
- Message：Operation ID 区间、Journal 掉电状态、IN_DOUBT、结果重放、满表和 Tombstone；
- Route/QoS：Candidate 原子性、迟到 ACK、RouteSet、动态 Metric、饥饿与 Budget 注入；
- Transfer：乱序、重复、丢片、SACK、Credit、重传、Path/Session 变化；
- Realtime：完整 uncertainty、两级 Deadline、Domain Generation、高水位与固定 Path；
- Cluster：Record 回读、Joint quorum、Takeover/Handover/Recovery/Rekey、Fence 和 Tombstone；
- Adapter：完整 RX item、token、同步 completion、cancel 竞争、quiesce/reopen 和迟到事件；
- 规模：1,000/10,000 逻辑节点按固定成员容量分簇，周期性 reload 后没有 volatile lease时
  Authority 必须保持关闭。

10k 模拟证明按簇分治的数据结构和 Owner 可以扩展，不证明单 MCU 保存 10k 节点，也不证明
真实无线网络已在该规模收敛。

## 6. 安装与单一发布面

安装树只包含：`include/ucn/ucn.h`、`include/ucn/v6/**`、`libucn_v6_*.a/.lib`，以及
`UCNConfig.cmake`、版本文件和 `UCNTargets`。独立 consumer 使用
`find_package(UCN 6 CONFIG REQUIRED)` 和 `UCN::ucn` 编译运行。发布目录不再包含 v4/v5
头、源码、测试、工具、兼容 target 或旧公共符号。

## 7. 尚未关闭的发布门禁

以下项目需要外部环境或硬件，继续保持 HOLD：

1. 可工作的 TSan 工具链；
2. ESP32-S3 N16R8/N8R8 的固件、真实 RAM/栈/Flash 尺寸；
3. UART/RS-485、ESP-NOW、Classic CAN/CAN-FD、USB 的 ISR/DMA/completion；
4. 1～5 跳吞吐、Q0～Q3 尾延迟、32 B～8 KiB 与混合 Bearer；
5. Realtime 硬件 timestamp、asymmetry 和 uncertainty 上界；
6. Flash 双槽撕裂写、真实断电窗口和启动恢复；
7. CPU、栈、RAM、功耗、P99/P999 和 24 小时长稳；
8. 最终统一外审 P0/P1 归零。

所以 V6-14 的软件范围已完成，但整体仍为 `PARTIAL`；V6-15 可以完成单一发布面和外审包，
不能生成 1.0 RC 或发布 Tag。

## 11. V6X-A01～A11 整改后的追加验证

最终整改重新执行的软件矩阵为：Full Debug/Release `26/26`、Lite `26/26`、Nano `26/26`；
Nano Feature-Off `21/21`，Realtime-only `22/22`，Cluster-only `24/24`，Adapter-only
`22/22`。MSVC 19.29 Full Release `26/26`，WSL GCC ASan/UBSan 与 `-fanalyzer` 均
`27/27`，Clang 18.1.3 Release `-Wall -Wextra -Werror` 为 `27/27`，Windows GCC
ISO C99 `-pedantic-errors` 为 `26/26`。

新增门禁包含五节点逐跳安全重签、多 E2E 目标共享下一跳、Identity/Cluster rollback witness、
Operation ID 间隙、Realtime/Cluster 认证 Payload 绑定、Capability Deadline、预算代际回收和
“连续 RX 不写 Flash”。完整问题映射见
[V6X-A01～A11 整改报告](UCN_V6_外审V6X_A01_A11整改与跨模块自审报告.md)。

后续从头自审又新增 Message Witness 旧 Snapshot 回放/掉电窗口、Realtime same-generation
Proposal Identity ABA、Owner task/ISR 锁分离、Handover Voter 资格及 65534 跳流式归约反例；
整改后的测试数量不变，以上 `26/27` 组均已用最新源码重新执行，不沿用旧构建结论。
