# UCN S08 公共 API 与静态存储边界

> v5 后续说明：本文保留为 `v4.0.0-final-before-v5` 的 API/资源历史证据。当前 v5 V5-33 已把帧、控制面、路由约束、Path 能力、动态 MTU 与 Storage Layout 继续演进，但没有改变 Node API/Storage owner 边界；PATH_INSTALL 双格式和公共符号以 `UCN_V5_31_PATH_INSTALL兼容与API符号修复报告.md` 为准。

## 1. 当前结论

S08 已把 Node 的“对外 API”和“固定内存实现布局”分成两个显式头文件，同时保留 MCU-first 的静态分配方式：

| 头文件 | 谁包含 | 提供什么 | 不允许做什么 |
| --- | --- | --- | --- |
| `ucn_node.h` | 业务模块、Adapter 声明、Bridge 声明、只传递 Node 指针的 Port | `ucn_node_t` 不完整类型、配置/结果/统计类型和全部公开函数声明 | `sizeof(ucn_node_t)`、静态分配或访问内部字段 |
| `ucn_node_storage.h` | 唯一 Protocol Task 的 Node 所有者、Core 实现、明确的白盒测试 | 完整固定布局和 `UCN_NODE_STORAGE_LAYOUT_VERSION`，可静态分配 Node | 把内部字段当作应用 ABI、跨任务直接读写或用不同 Profile 编译 |

这不是把 Node 改成堆对象，也没有引入 `malloc`、Linux、RTOS、生成器或运行时容量协商。产品仍可写：

```c
#include "ucn/ucn_node_storage.h"

static ucn_node_t g_node;
```

但这段声明只应出现在拥有 Node 的一个 `.c/.cpp` 编译单元。其他模块只包含 `ucn_node.h` 并传递 `ucn_node_t *`。这样既保留小 MCU 需要的静态、可计算内存，又防止应用无意依赖 Route、Seen、Queue 等内部表的字段位置。

## 2. 头文件依赖与所有权

```text
业务 Task / Adapter 头 / Port 对外头
              │
              └── ucn_node.h（不完整 ucn_node_t + API）

唯一 Protocol Task owner / Core .c / 白盒测试
              │
              └── ucn_node_storage.h
                    └── ucn_node.h
```

必须遵守以下规则：

1. 同一固件的全部编译单元使用相同 `UCN_PROFILE`、`UCN_FEATURE_SERVICE` 和 `UCN_MAX_*` 定义。
2. 业务 Task 不持有或并发访问 Node 内部字段；它通过 Service/Port 或公开 Node API 提交请求。
3. Adapter 的公开头只声明 `ucn_node_t *`，不得因为实现方便而引入存储头。
4. Core 与白盒测试可以访问存储布局，但这些类型不构成稳定应用 ABI。
5. `UCN_NODE_STORAGE_LAYOUT_VERSION` 只用于发现存储头布局代际，不是线协议版本，也不能代替一致的编译定义。

## 3. 网络重复窗口与安全 Replay Window

两者用途不同，不能互相替代：

| 机制 | 当前保存键/状态 | 目的 | 掉电后要求 | 安全结论 |
| --- | --- | --- | --- | --- |
| Core Duplicate Source Window | 固定 `(Source, Session)` 槽、最高 Sequence 和位图；动态 RREQ 的 Request Key/Best Cost 位于另一张固定表 | 抑制网络重复转发和重复业务投递，允许窗口内乱序一次交付，同时保留更低 Cost RREQ | 不持久化，可丢失 | 不是身份认证，也不是生产防重放 |
| Security Provider Replay Window | 由产品按身份、Session/Key Epoch 和 Counter 维护 | 在认证/解密边界拒绝已接收、回退或撤销代际的数据 | 需要与单调 Counter、密钥和掉电恢复策略闭环 | 属于 S02 生产安全门禁 |
| Service 高风险命令 Replay 表 | 可选固定 Source/Session/Endpoint/Command ID | 在业务 Validator 中拒绝重复或过期执行命令 | 由产品决定持久化和 Session 轮换 | 只保护该业务命令格式，不等价于全协议 Replay Window |

因此，即使一个重复帧被 Duplicate Source Window 丢弃，也不能据此宣称链路已认证；反过来，Security Provider 已认证的数据仍需要网络重复窗口阻止合法重复包在 Mesh 中反复转发。

## 4. 小 MTU 与 Payload 边界

S08 继续沿用 S04 已实现的编译期门禁和实际 Payload helper：

- 32 B 基础头、36 B Route Extension、40 B Path Header；E2E 保护再占 16 B Tag。
- 调用方按实际 Flags 使用 Payload capacity helper，不能始终按 `UCN_MAX_FRAME_BYTES - 32` 假设可用净荷。
- Nano/Lite/Full 在 Service OFF 时的当前最小帧上限分别为 33/46/64 B；32/45/63 B 会在编译期拒绝。Lite 原 50 B 门禁已随 V5-14 的 RREP 去重和 14 B W3 RREQ 更新。
- Path Trace 容量先验证最小帧，再做无符号减法，不会在过小 MTU 下下溢成巨大容量。

这些是 UCN Frame 的编译边界。经典 CAN 等更小 MTU 仍需 Carrier 分段，不能把编译通过理解为任意介质都可直接承载完整帧。

## 5. 历史版本命名处理

生产源码和公开 API 均以 v4 为当前协议。测试入口已从含混的 `test_v3()` 改为 `test_protocol_version()`；`tests/test_v3.c` 的历史文件名和局部 Fixture 前缀暂时保留，以便旧失败日志可检索。该文件现在明确覆盖“当前 v4 编解码/安全行为 + 对旧协议版本的显式拒绝”，不代表当前产品仍运行 v3。

项目任务表中的“V3 Core 完成边界”和旧操作记录属于迁移历史证据，故意保留，不应批量改写成 v4。当前架构、使用手册和新增结论必须以 v4 表述。

## 6. 软件验证证据

本轮新增：

- `test_public_headers.c`：不包含存储头，只使用不完整 Node 指针和公开函数类型。
- `test_node_storage_header.c`：只由所有者语义包含存储头，证明三档仍可静态分配 Node。
- Full 白盒测试统一经 `test_support.h` 显式选择存储视图。
- ESP32 参考工程的 `main.cpp` 与 UART 协议 Bench 作为 Node 所有者显式包含存储头；Adapter 公开头继续只包含 API 头。
- PlatformIO 预构建脚本从“全量加入 `src/*.c`”改为按 `custom_ucn_profile` 和 `custom_ucn_feature_service` 选择与 CMake 相同的源文件集合；Full 不再误编 Nano Node/非 Full Stub，UART Bench 可明确关闭 Service。

实际结果：

| 门禁 | 结果 |
| --- | --- |
| GCC 14.2 Release，Nano/OFF、Lite/ON、Full/ON、Lite/OFF、Full/OFF | 全部构建并 CTest 通过 |
| MSVC Debug，Nano/OFF、Lite/ON、Full/ON | 全部构建并 CTest 通过 |
| 最小 MTU 33/46/64 B | 全部编译通过 |
| 低一字节 32/45/63 B | 全部按预期编译拒绝 |
| 三档公共 Node/Path/Policy 符号 | V5-33 当前筛选口径每档 74 个，无缺失；两个 capability API 均直接链接验证 |
| Host `sizeof(ucn_node_t)` | V5-44/V5-36 后 Nano/Lite/Full 为 2648/6024/10080 B；Storage Layout Version=5 |
| ESP32-S3 Full/Service ON 正常 Node | 构建成功，RAM 48124 B，Flash 600819 B |
| ESP32-S3 Full/Service OFF UART Bench | 构建成功，RAM 22152 B，Flash 185947 B |
| ESP-WROOM-32 Full/Service ON | 构建成功，RAM 50184 B，Flash 626803 B |

S08 当时的头文件拆分不改变 v4 线格式或运行时对象；后续 S22 因去重/RREQ 状态重构将 Storage Layout Version 升到 2，V5-18 因 Path Remaining Hops 升到 3，V5-24 又因 Candidate 保存实际 Wire Profile 升到 4。公开 API 仍以不完整 `ucn_node_t` 隔离内部布局。上述旧 ESP 数字是历史完整测试固件尺寸，不是当前纯 Core 增量，也不证明运行时栈、Heap 或通信行为。

## 7. 未由本轮证明的内容

- ESP32/STM32 各 Profile 的最终 ELF/Map、Task 栈、最低 Heap、CPU 和功耗。
- 真实 Wi-Fi/UART/CAN 多板的入网、故障恢复、吞吐和时延。
- 生产身份、逐跳链路认证、经审计 AEAD、持久 Counter 和 Replay Window。
- GitHub Actions 远端矩阵实际成功状态。

这些分别继续归 S02、S06、S07 和 S09；不能用公共头编译或 Host CTest 替代硬件与生产安全证据。
