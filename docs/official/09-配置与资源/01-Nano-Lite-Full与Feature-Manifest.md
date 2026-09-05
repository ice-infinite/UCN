# Nano、Lite、Full 与 Feature Manifest

## 1. 配置入口

CMake 接受 `UCN_PROFILE=NANO|LITE|FULL` 以及 `UCN_FEATURE_REALTIME`、
`UCN_FEATURE_CLUSTER`、`UCN_FEATURE_ADAPTER`。产品需要更精细容量时，通过
`UCN_USER_CONFIG_HEADER` 和 `UCN_USER_CONFIG_INCLUDE_DIR` 注入统一配置头；不能让不同
Translation Unit 各自定义不一致宏。

## 2. Profile 含义

Profile 只提供一组默认容量。Nano/Lite/Full 都解析 A0～A3、同一安全上下文和全部 v6 合法
控制语义。低档不能发送超过自己 Message Class/Storage 的业务，但可明确拒绝、转发或通过
Capability 告知对端，不能把“低资源”解释成“低权限”或“旧协议”。

默认主要差异：

| 维度 | Nano | Lite | Full |
|---|---:|---:|---:|
| Binding / Session | 4 / 2 | 8 / 4 | 16 / 8 |
| RouteSet / Paths per set | 4 / 2 | 8 / 3 | 16 / 4 |
| QoS flows | 4 | 16 | 32 |
| Transfer TX/RX slots | 1 / 1 | 2 / 2 | 4 / 4 |
| 最大发送 Message Class | 256 B | 2 KiB | 8 KiB |
| Adapter links | 2 | 4 | 8 |
| Cluster members | 4 | 8 | 16 |

完整数值以 `include/ucn/v6/ucn_v6_config.h` 为准。

## 3. Manifest 与 Layout Hash

`ucn_v6_compiled_manifest()` 返回当前库的 API Version、Storage Layout、Profile、Feature bits、
容量和 Layout Hash。应用编译得到的期望 Manifest 必须用 `ucn_v6_manifest_validate_exact()`
与库逐字段匹配；任一不同都应在初始化前失败。

Feature 开关和 Profile 已进入 Layout Hash。不能用 Full 头编译应用再链接 Nano 库，也不能
关闭 Cluster 后仍分配/调用 Cluster Owner。安装包导出的 target 会传播构建时 Profile 和
Feature 宏，但产品仍应保存最终 Manifest 作为固件证据。

## 4. Storage

每个 Owner 的 `*_STORAGE_BYTES` 和 Storage 类型支持文件作用域静态分配，避免 C99 不能用
运行期函数值定义静态数组的问题。初始化同时检查地址对齐、提供容量和对应布局。Opaque
不能物理阻止恶意代码 `memset` 调用方内存；它只防止合法代码依赖私有字段，并通过 magic/
schema/fence 检测部分损坏。

不要因为宏给出上界就一次性在栈上分配所有 Owner。产品按启用 Feature 和角色静态/全局
放置所需对象，使用资源报告核对 BSS，再在目标链接 map 和运行时 high-water 中验证。

## 5. 当前 Host 上界

V6-15 自审时 Host 公共 Storage 上界已分 Profile 记录。它们是跨私有布局变化的安全预算，
不是私有对象实际 `sizeof`，也不等于目标 MCU 的最终 BSS/栈。发布必须用目标编译器、链接
脚本和真实 Feature 组合重新生成证据。
