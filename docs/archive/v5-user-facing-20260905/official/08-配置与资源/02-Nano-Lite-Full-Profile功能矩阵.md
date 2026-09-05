# Nano、Lite、Full Profile 功能矩阵

> 文档级别：`REFERENCE`
> 实现状态：`CURRENT`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：ucn_config.h、ucn_profile.h、CMake 与资源门禁
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：Host 资源可用；目标 MCU 资源需按产品配置实测

| 能力 | Nano | Lite | Full |
| --- | --- | --- | --- |
| Core 帧、固定 Link、Endpoint | 是 | 是 | 是 |
| Dynamic Mesh | 否 | 是 | 是 |
| Security 接口 | 否 | 是 | 是 |
| Candidate Routing | 否 | 否 | 是 |
| Path | 否 | 否 | 是 |
| Policy/负载均衡 | 否 | 否 | 是 |
| 高级诊断 | 否 | 否 | 是 |
| Service Router/Bridge | 独立开关 | 独立开关 | 独立开关 |

Profile 常量为 `UCN_PROFILE_NANO=1`、`LITE=2`、`FULL=3`，默认 Full。

### “有/无”具体代表什么

- **Dynamic Mesh**：邻居、路由发现、缓存维护等动态网络能力；关闭后仍可使用预配置 Link/固定路径；
- **Security**：安全 Provider、策略与封装接口进入构建；它不等于产品已经配置密钥；
- **Candidate Routing**：保存/比较多条候选路由，为验证后切换提供基础；
- **Path**：显式 Path Identity、安装、转发和诊断；
- **Policy**：固定路径、自动最优、负载均衡及 flow lease；
- **Diagnostics**：高级快照和诊断控制能力，基础错误码/统计不应因此消失；
- **Service**：与 Profile 正交，按产品是否需要本机任务/远程服务语义单独开启。

## 接收和 API 边界

Profile 不允许把不支持的功能静默模拟成成功。被裁剪的公开 API 由 stub 保持符号/源码可用时，应返回 `UCN_ERR_CONFIG` 等明确错误；Wire/Profile 解码仍必须按合法格式 fail-closed。

这带来两个边界：

1. “头文件能调用”不代表当前 Profile 有实现，应用必须处理 `UCN_ERR_CONFIG`；
2. 低 Profile 节点仍不能把未知高级帧当作普通 payload 接收，避免产生跨 Profile 语义分歧。

公开 API 的链接门禁应同时构建 Nano/Lite/Full，保证无条件声明的符号在各 Profile 都存在；被裁剪功能的 stub 只用于明确失败，不负责降级为另一种行为。

## 选择建议

- Nano：很小的 MCU、固定拓扑、最少表和最少控制功能；
- Lite：需要自动组网和安全接口，但不需要候选路径、Policy；
- Full：需要完整动态路由、Path、负载均衡和诊断。

实际 RAM 还取决于容量宏和可选 Archive，Profile 名称本身不是资源上限。

### 决策示例

| 产品场景 | 推荐起点 | 原因 |
| --- | --- | --- |
| 两个 MCU、固定 UART/CAN、只做可靠消息 | Nano | 固定拓扑已知，无需动态发现和 Policy |
| 多个传感器自动加入、有安全需求、只取单条路由 | Lite | 保留动态 Mesh 与 Security，裁掉候选/Path/Policy |
| UART+CAN+Wi-Fi 多 Bearer、固定路径和负载均衡 | Full | 需要候选路由、Path 与 Policy 联动 |
| Host 网关需要服务桥，但 MCU 很小 | MCU Nano/Lite + Host Full | Profile 是每个构建目标的能力，不要求全网同档 |

选择低 Profile 的前提是协议协商和应用需求允许。高级节点向低 Profile 节点发送消息时，必须使用双方共同支持的 Wire/能力，不能假设“高级节点能解析所以低级节点也一定能”。

### 验证矩阵

每次改变公共 API、条件编译或结构体时至少构建：Nano、Lite、Full、Service ON/OFF，以及一个低容量产品配置。所有配置都应运行公共头链接测试和对应可用功能的单元测试。
