# UCN S04 Feature Profile 与资源报告

## 1. 当前结论

UCN 已建立三个编译期网络 Profile，并让关闭能力同时从 `ucn_node_t` 状态和 CMake 源文件列表中移除。默认仍为 Full，保持现有完整 API/行为；Nano、Lite 对未启用的高级公开 API 提供确定的不可用语义（`UCN_ERR_CONFIG`、`NULL`、恒过期或维护空操作），避免运行时误以为能力可用，也避免公共符号在链接阶段缺失。

Service Router/Bridge 与网络 Profile 正交，通过 `UCN_FEATURE_SERVICE=ON/OFF` 单独选择。Linux 仍只是可选 Host/Adapter，不参与任何 Profile 的路由前提。

## 2. Profile 边界

| Profile | 包含 | 明确不包含 | 是否自动 Mesh |
| --- | --- | --- | --- |
| Nano | Frame、Link、Adapter RX Queue、Endpoint、Q0/Q1、静态直连与静态 Route。 | HELLO Scheduler、Neighbor、Heartbeat、AODV、Security、Candidate、Path、Policy、Trace、Snapshot、Policy Diagnostic。 | 否 |
| Lite | Nano + HELLO Scheduler、Join/Neighbor、Heartbeat、多 Bearer、AODV-Lite、RERR、最小 Security Provider 接口。 | Candidate 先测后切、显式 Path、Policy/Balance、三类诊断。 | 是 |
| Full | Lite + Candidate、Path、Policy、Q1 Flow/AUTO_BALANCE、Path Trace、Node Snapshot、Policy Diagnostic。 | 生产身份、密钥和 AEAD 算法仍由产品提供。 | 是 |

Service 开启后，任意 Profile 都可增加节点内 Service Router/Bridge；它不改变网络 Profile 的路由能力。

## 3. 构建方法

```powershell
# 小型静态网络，不需要 Service
cmake -S . -B build_nano -DUCN_PROFILE=NANO -DUCN_FEATURE_SERVICE=OFF

# MCU 自组网基础档，保留节点内任务通信
cmake -S . -B build_lite -DUCN_PROFILE=LITE -DUCN_FEATURE_SERVICE=ON

# 完整功能，默认配置
cmake -S . -B build_full -DUCN_PROFILE=FULL -DUCN_FEATURE_SERVICE=ON

cmake --build build_lite --parallel
ctest --test-dir build_lite --output-on-failure
```

CMake 会把 `UCN_PROFILE` 和 `UCN_FEATURE_SERVICE` 作为 `ucn_core` 的 `PUBLIC` 编译定义发布。产品若不使用 CMake，必须对所有包含 UCN 公共头的编译单元提供相同定义；它们会改变公开对象布局，不能只给部分 `.c` 文件设置。

## 4. 实际裁剪方式

- `include/ucn/ucn_profile.h` 是唯一 Profile/Feature 依赖图。
- Nano 使用独立的 `src/ucn_node_nano.c`，不编译 Full/Lite 的动态路由 Node 单体。
- Lite 编译动态 Mesh Node，但 Candidate、Path、Policy、Diagnostic 的处理函数和对象字段由预处理阶段移除。
- 非 Full 不编译 `src/ucn_path.c`、`src/ucn_policy.c`，高级公开 Node/Path API 由 `src/ucn_profile_stubs.c` 明确返回“未配置”或不可用；内部 Policy 维护 Hook 保留固定空操作符号，避免公共头可见声明在链接阶段缺失。
- Nano 不导出 HELLO Scheduler 类型/API，其实现也不会进入 Adapter 对象。
- Service 关闭时不编译 `src/ucn_service.c` 和 `src/ucn_service_bridge.c`。

因此裁剪不依赖链接器碰巧删除未引用函数，也没有用零长度业务表伪装功能关闭。

## 5. 最小帧上限

| 配置 | 编译期最小 `UCN_MAX_FRAME_BYTES` | 原因 |
| --- | ---: | --- |
| Nano + Service OFF | 33 B | 当前静态 Payload 缓冲仍按保守 32 B Build 边界至少保留 1 B；Wire W0 Header 本身为 17 B。 |
| Lite + Service OFF | 46 B | 当前最大必需 W3 动态路由控制载荷是 14 B `ROUTE_REQ`，并保留 32 B W3 Route Header 的通用 Payload 缓冲契约；旧 18 B RREP 门禁已由 V5-14 移除。 |
| Full + Service OFF | 64 B | 保守 W3 控制面和 32 B Policy Diagnostic Reply。 |
| 任意 Profile + Service ON | 64 B | 默认 `UCN_SERVICE_MAX_PAYLOAD_BYTES=32`。 |

GCC 严格构建已验证 33/46/64 B 的 `ucn_core` 分别可编译；32/45/63 B 分别按预期在编译期拒绝。这里是 **Core 正向/负向编译门禁**，不是最小配置下运行包含大帧向量的整套 CTest；也不代表 CAN 等小 MTU 介质无需 Carrier 分段。

## 6. Host 资源对比

以下结果来自 Windows x64、GCC 14.2、Release `-O3`、Service OFF。`node_bytes` 是 `sizeof(ucn_node_t)`；`archive .text` 是 `size -t libucn_core.a` 对全部对象的合计，用来证明源代码确实被裁剪。

| Profile | `sizeof(ucn_node_t)` | 相对 Full | 静态库 `.text` 合计 | 相对 Full |
| --- | ---: | ---: | ---: | ---: |
| Nano | 2,648 B | -72.8% | 19,884 B | -84.4% |
| Lite | 5,960 B | -38.9% | 68,244 B | -46.6% |
| Full | 9,752 B | 基线 | 127,792 B | 基线 |

该表已在 V5-33 后以 GCC 14.2 Release/Service OFF 重新测量。Node 为 `2648/5960/9752 B`，`ucn_link_t` 三档均为 40 B；V5-28 的 Path 能力和诊断只增加 Full 固定状态，Storage Layout Version 升到 5，V5-32 只为非 Full 增加 capability API Stub。固定状态仍无动态内存。Archive `.text` 只用于比较 Host 裁剪。历史变化见[S22 稳定化修复报告](UCN_S22_重复抑制与稳定化修复报告.md)、[V5-07 报告](UCN_V5_07_发布门禁与软件验证报告.md)和[V5-31～V5-33 修复报告](UCN_V5_31_PATH_INSTALL兼容与API符号修复报告.md)。

这些数字不是 MCU ELF 的最终 Flash/RAM：目标 ABI、编译器、LTO、表深度、`UCN_MAX_FRAME_BYTES` 和产品静态实例数都会改变结果。目标板必须另外报告 ELF 段、静态对象、运行时栈高水位和 Heap；不能把 Host `.a` 直接写成 ESP32/STM32 Flash。

## 7. 软件验证证据

- Nano：直接运行 Frame/QoS/静态 Route/Endpoint/Adapter RX/Service/去重行为；动态寻路和 Security 返回 `UCN_ERR_CONFIG`。
- Lite：直接运行 AODV/RERR、Neighbor/HELLO/Heartbeat、多 Bearer、安全 Provider、Control Budget、Stress；Candidate/Path/Policy/Diagnostic 返回 `UCN_ERR_CONFIG`。
- API 完整性：头文件声明的 `ucn_node_*`/`ucn_path_*`/`ucn_policy_*` 公共符号在 Nano、Lite、Full 静态库中均存在；V5-32 后按当前筛选口径三档各 74 个，两个 capability API 也由测试二进制直接引用，低档 Profile 不会在链接阶段才暴露缺失能力。
- Full：现有完整单元、虚拟拓扑、动态压力和 Profile 测试全部通过。
- v5：四档固定域、1 B RX Ceiling HELLO、3 B Ingress Peek、Profile-aware RREQ/Path、3/3/3/4 B Cost、Candidate Profile 连续性、Q1 绝对 Deadline、运行期 Hop Scope、动态 MTU、异构 Bearer Path 能力、逻辑 Bearer Policy、AAD Profile 绑定、跨档透明密文中继、路由约束与 2→4→8→16 Expanding Ring 均通过。
- Service：Nano/OFF 证明源码可移除；Lite/ON 证明正交组合可初始化 Router；Full/OFF 与 Full/ON 均可构建测试。
- 编译器：MSVC Debug 与 GCC 14.2 Release 均验证；GCC 启用 `-Wall -Wextra -Wpedantic -Werror`。
- CI：工作流已加入 Nano/OFF、Lite/ON、Full/ON 矩阵；只有远端 Actions 实际运行成功后，才能写成远端 CI 已通过。

## 8. 尚未完成的硬件门禁

- ESP32-S3、ESP-WROOM-32、STM32 等目标编译器下的独立 ELF 段与 Map 对比。
- 各 Profile 的协议 Task 栈高水位、Heap 最低值、CPU 占用与功耗。
- Lite/Full 在真实 Wi-Fi/UART/CAN 多板环境的入网、断链、吞吐和时延。
- 根据具体 MCU RAM 重新下调 Queue、Neighbor、Route、Source/Session Window、RREQ Cache 等固定表深度。

所以 S04 的“代码与软件验证”已闭环；S08 已进一步完成公共 API/静态存储边界，目标板绝对资源和实机行为继续按 S06/S07 的硬件门禁执行。
