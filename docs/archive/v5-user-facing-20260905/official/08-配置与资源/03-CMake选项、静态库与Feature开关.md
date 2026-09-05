# CMake 选项、静态库与 Feature 开关

> 文档级别：`REFERENCE`
> 实现状态：`CURRENT`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：ucn_config.h、ucn_profile.h、CMake 与资源门禁
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：Host 资源可用；目标 MCU 资源需按产品配置实测

## 主要公共选项

| 选项 | 默认 | 作用 |
| --- | --- | --- |
| `UCN_BUILD_TESTS` | ON | 单元/集成测试 |
| `UCN_BUILD_SCALE_SIM` | ON | Host Core 规模模拟 |
| `UCN_BUILD_CLUSTER_SIM` | ON | Host Cluster 模拟 |
| `UCN_BUILD_CONFIG_CONTRACT_TESTS` | OFF | 独立全局/default/fallback 配置合同矩阵 |
| `UCN_FEATURE_SERVICE` | ON | Service Router/Bridge |
| `UCN_CLUSTER_ENABLE_V3_COMPAT` | 按仓库默认 | Cluster v3 兼容路径 |
| `UCN_ENABLE_WIRE_V4_RELEASE_GATES` | OFF | Wire v4 发布门禁测试 |
| `UCN_BUILD_CLUSTER_TAKEOVER_EXPERIMENTAL` | OFF | M10 Archive |
| `UCN_BUILD_CLUSTER_HANDOVER_EXPERIMENTAL` | OFF | M11 Archive |
| `UCN_BUILD_CLUSTER_REKEY_EXPERIMENTAL` | OFF | M13 Archive |

准确默认值和组合以当前 `CMakeLists.txt` 为准；发布说明不得从旧构建缓存推断。

### 典型配置命令

```powershell
cmake -S . -B build/full -DUCN_PROFILE=FULL -DUCN_BUILD_TESTS=ON
cmake --build build/full --config Debug
ctest --test-dir build/full -C Debug --output-on-failure
```

低资源配置应使用独立目录：

```powershell
cmake -S . -B build/nano -DUCN_PROFILE=NANO `
  -DUCN_BUILD_SCALE_SIM=OFF -DUCN_BUILD_CLUSTER_SIM=OFF
cmake --build build/nano --config Debug
```

不要在同一 build 目录里反复切换 Profile 后把旧测试结果混在一起。

## 静态库边界

Core、Service、Transfer、Cluster、Federation 和实验 Archive 按 target 分离。头文件存在不代表对象已进入最终固件；应检查最终 link map 或 archive symbols。

这一区分对 Cluster 尤其重要：Wire v4、Takeover、Handover、Rekey 可能有源码、头文件和定向测试，但默认产品 target 未链接或 encoder 关闭。文档必须写成“实验 Archive 已实现并测试”，不能简写为“产品已经支持”。

核对方法包括：

- 查看 CMake target 的 source list 和 compile definitions；
- 用 `nm`/`dumpbin` 检查静态库符号；
- 查看最终固件 map，确认未引用对象是否被链接器裁剪；
- 搜索生产源码是否调用实验 API；
- 在默认构建中加入“禁止出现某符号/宏”的门禁。

## 编译定义传播

`UCN_PROFILE`、`UCN_FEATURE_SERVICE`、产品配置头和会改变 ABI 的 Feature 必须由 target 公开传播。测试专用宏只能绑定测试 target，禁止泄漏到生产库。

### 测试宏与产品宏

测试宏可以开放私有 builder、实验 encoder 或故障注入，但必须满足：

- 只出现在命名清晰的 test target；
- 生产静态库不继承该定义；
- 测试若要证明生产行为，必须链接生产 archive，而不是测试专用重编译副本；
- release gate 应另外用 Release 优化构建运行，避免 Debug padding/未初始化行为被掩盖。

### 改动后的最低门禁

| 改动类型 | 必须重跑 |
| --- | --- |
| 配置宏/default | 配置合同 + Nano/Lite/Full |
| 公共结构/API | 公共头编译 + 各 Profile 链接 + ABI/sizeof |
| 实验开关 | 默认 OFF archive 检查 + 实验 ON 定向测试 |
| Codec/Wire | Debug/Release golden + negative + sanitizer |
| 容量默认 | 低容量配置 + scale 拓扑适配 + 资源报告 |
