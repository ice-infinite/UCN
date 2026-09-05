# UniLink / UCN v6

**UniLink** 是协议品牌名，**UCN（Unified Communication Network）** 是代码与架构名称。当前
`v6-development` 分支只构建 UCN v6；v4/v5 运行时代码、公共头、兼容 Stub、双栈测试和旧
CMake 开关已从当前发布树删除。v5 的最后实验快照保存在 Git 分支
`v5-final-experimental` 与 Tag `v5.0.0-experimental-final`。

UCN v6 是面向 MCU 的 C99 有界通信协议实现。它把身份、Wire、安全、消息语义、路由、QoS、
可靠传输和物理接口分层；Realtime 与 Cluster 是互不依赖的可选模块。运行时不用堆分配，
Nano/Lite/Full 只改变容量和可发送上限，不改变解析同一 v6 Wire 的能力。

## 当前实现

- 单一 v6 Wire：A0～A3 地址档、CRC32C、Hop/Group/E2E 安全选择器与 canonical AAD；
- Device Principal、Realm Address Binding Generation、Bootstrap 与 Peer Reauth；
- Q0～Q3、Delivery Guarantee、Interaction Role、64-bit Operation ID 与 durable Journal；
- 认证 Capability、Path Frame MTU 与精确 Payload Budget；
- Binding-aware RouteSet、原子 Candidate 激活、多路径和动态 Metric；
- 有界 QoS、公平调度、每源/每流配额与不可提权的 Hop Budget；
- 32 B～8 KiB Message Class、Selective Repeat、SACK、Credit 和多 Path Pipeline；
- 可选 Realtime：时间域、uncertainty、Deadline 双门禁；
- 可选 Cluster：单一 Target FSM、Joint Config、Backup、Takeover、Handover、Recovery、Rekey；
- Event→Ring→Protocol Owner Adapter，以及 UART、ESP-NOW/Wi-Fi、CAN、USB、FreeRTOS 和
  ESP32-S3 参考绑定。

## 快速构建

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug `
  -DUCN_BUILD_TESTS=ON -DUCN_PROFILE=FULL
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

裁剪示例：

```powershell
cmake -S . -B build-nano -G Ninja `
  -DUCN_PROFILE=NANO `
  -DUCN_FEATURE_REALTIME=OFF `
  -DUCN_FEATURE_CLUSTER=OFF `
  -DUCN_FEATURE_ADAPTER=OFF
cmake --build build-nano --parallel
```

安装并在外部工程消费：

```powershell
cmake --install build --prefix install
# 外部工程：find_package(UCN 6 CONFIG REQUIRED)
#            target_link_libraries(app PRIVATE UCN::ucn)
```

公共入口为 `#include <ucn/ucn.h>`。

## Profile 与 Feature

| Profile | 定位 | 最大可发送 Message Class | 说明 |
|---|---|---:|---|
| Nano | 极低资源端点/简单中继 | 256 B | 表项最少，仍解析 A0～A3 和所有 v6 合法帧 |
| Lite | 常规 MCU 节点 | 2 KiB | 中等 Route、QoS、Transfer 与邻居容量 |
| Full | 网关、簇头、高容量节点 | 8 KiB | 完整默认容量，不代表必须同时实例化全部 Owner |

Realtime、Cluster、Adapter 通过 CMake Feature 开关裁剪。Feature 关闭时对应头不会由
`<ucn/ucn.h>` 引入、对应 archive 不构建，Feature Manifest 和 Layout Hash 也会变化。

## 重要成熟度边界

当前已完成的是 v6 软件实现、Host 测试、模型和发布面收口，不是 UCN 1.0 RC：

- ESP32-S3、多 Bearer、CAN/CAN-FD、USB、ISR/DMA、真实 Flash 掉电尚需同一候选提交实测；
- Realtime 的硬件 timestamp、asymmetry 与 uncertainty 上界尚需测量；
- P99/P999 延迟、吞吐、CPU、RAM/栈、功耗和 24 小时长稳尚未形成 v6 发布证据；
- MSVC 已完成当前软件矩阵；可运行的 TSan 环境仍需补验；
- 安全 API 和状态机不等于已内置生产密钥系统或经过密码学产品认证。

因此仓库当前状态是 **v6 单一协议开发基线 / 软件自审中**。不得把测试绿色等同于硬件、
掉电、安全或发布放行。

## 文档入口

- [官方文档](docs/official/README.md)
- [用户手册](docs/用户手册/README.md)
- [源码阅读指南](docs/源码阅读指南/README.md)
- [任务表](docs/00-项目管理/00-任务表.md)
- [项目操作记录](docs/00-项目管理/01-项目操作记录.md)
- [V6 架构 RFC](docs/10-理论与规划/建议方案/UCN_v6_最终协议架构与破坏性重构_RFC.md)
- [V6-14 验证报告](docs/08-实现与验证/版本演进/UCN_V6_14_全量验证与资源门禁报告.md)
- [V6-15 单一发布面与多轮自审](docs/08-实现与验证/版本演进/UCN_V6_15_单一协议发布面与多轮自审报告.md)
- [V6X-A01～A11 外审整改与跨模块自审](docs/08-实现与验证/版本演进/UCN_V6_外审V6X_A01_A11整改与跨模块自审报告.md)

源码以 `include/ucn/v6/` 和 `src/v6/` 为当前事实；旧文档只用于解释历史决策。
