# UCN V5-31～V5-33 PATH_INSTALL 兼容与 API 符号修复报告

> 日期：2026-08-11
> 范围：`PATH_INSTALL` v5 Wire Schema、Full/Lite/Nano 公共 API 符号、发布回归

## 1. 结论

两项 P1 发布阻塞问题已经完成源码、测试和文档闭环：

| 问题 | 修复结果 |
| --- | --- |
| `PATH_INSTALL` 在协议版本仍为 5 时只接受扩展长度 | 保留协议版本 5；旧 API 固定发送基础 Schema，新 capability API 显式发送扩展 Schema；新接收端按精确长度同时接受两种格式。 |
| Lite/Nano 公共头声明 capability API，但静态库不导出符号 | 两个非 Full Profile 均补齐 Stub；公共头与 Profile 测试直接调用，三档静态库符号表均核实导出。 |

本修复没有宣称“任意旧节点都理解扩展能力字段”。旧 v5 节点只能可靠接收基础 Schema；只有确认目标支持扩展 Schema 后，调用方才能使用 capability API。

## 2. PATH_INSTALL 双格式合同

设 `A=AddressWidth`、`P=PathWidth`：

| Schema | Payload | W0/W1/W2/W3 |
| --- | --- | --- |
| v5 基础 | `PathID(P)+Destination(A)+NextHop(A)+Lease(4)+RemainingHops(1)` | `8/11/14/17 B` |
| v5 扩展 | 基础字段 + `MaximumWireProfile(1)+MinimumMTU(2)` | `11/14/17/20 B` |

发送 API 的语义固定为：

- `ucn_node_send_path_install()`：只发送基础 Schema，兼容已部署的旧 v5 接收端；
- `ucn_node_send_path_install_capable()`：发送扩展 Schema，即使 capability 参数为 `NULL` 也仍是扩展长度；`NULL` 编码为 `UNSPECIFIED + 0`；
- 新接收端：只接受本 Profile 对应的基础长度或扩展长度，其他长度一律返回 `UCN_ERR_MALFORMED`；
- 基础 Schema：不携带端到端瓶颈，接收节点继续从本地逻辑 Neighbor 的合格 Bearer 推导能力；
- 扩展 Schema：显式瓶颈会与本地 Bearer 交集取更窄值，不能放宽物理能力。

之所以不升级整个协议到 v6，是因为这里可以由旧 API 保持 v5 基础 Wire 合同，并由显式新 API 隔离扩展行为。HELLO 当前没有 Path Schema 协商，所以 Core 不会自动向未知旧节点发送扩展格式。

## 3. 非 Full 公共 API 合同

公共头无条件提供：

- `ucn_node_install_local_path_capable()`；
- `ucn_node_send_path_install_capable()`。

Full 提供真实 Path 实现；Lite/Nano 因 `UCN_FEATURE_PATH=0` 提供固定 Stub：

| 调用条件 | Lite/Nano 结果 |
| --- | --- |
| `node == NULL` | `UCN_ERR_ARGUMENT` |
| Node 有效 | `UCN_ERR_CONFIG` |

这样应用可以跨 Profile 编译和链接，并在运行时获得明确的 Feature 不可用结果，不会出现 `undefined reference`。

## 4. 测试证据

专项测试覆盖：

- W0～W3 基础 `8/11/14/17 B` 发送和接收；
- W0～W3 扩展 `11/14/17/20 B` 发送和接收；
- W3 Auto 经 W0-only Link 选择 W0 基础格式；
- 四档坏中间长度在授权和写表前拒绝；
- Full/Lite/Nano 测试二进制均直接引用两个 capability API；
- `nm -g --defined-only` 确认三个 `libucn_core.a` 均导出两个符号。

发布回归结果：

| 环境 | 结果 |
| --- | --- |
| Windows Full Debug / Release+Service OFF | `10/10` / `10/10` |
| Windows Lite Debug / Release+Service OFF | `10/10` / `10/10` |
| Windows Nano Debug / Release+Service OFF | `1/1` / `1/1` |
| Windows Full 配置契约 | `13/13` |
| WSL Full ASan+UBSan + 配置契约 | `13/13` |
| WSL GCC `-fanalyzer` + 配置契约 | `13/13` |
| 项目 Markdown / 知识库 WikiLink | `204/525`，缺失均为 0 |
| 调用树 | 10 模块、132 节点、194 调用引用，结构错误 0 |
| `git diff --check` | 通过 |

当前 Windows x64 GCC 14.2 Release/Service OFF 的 Node 大小仍为 Nano/Lite/Full=`2648/5960/9752 B`，Link 均为 `40 B`。这些是 Host ABI 裁剪证据，不代表目标 MCU 的最终 RAM/Flash。

## 5. 使用与升级规则

1. 需要与未知或旧 v5 节点通信时，使用 `ucn_node_send_path_install()`。
2. 只有产品配置、节点清单或更高层管理面确认目标支持扩展 Schema 后，才使用 `ucn_node_send_path_install_capable()`。
3. 如果产品未来要求自动选择扩展格式，应先定义可认证的能力协商；不能仅凭自报 Profile 或 Node ID 推断支持。
4. 旧 v5 节点发来的基础格式在新节点上可继续使用，但只能保证本跳能力推导；若后续跳更窄，现有确定性 Path-RERR 负责暴露失败并触发重建。

## 6. 保留边界

- 本轮只完成 Host 软件验证，没有进行 ESP32/STM32 多板互操作、断链时延、吞吐、CPU、栈或功耗测试。
- S02/V5-21 的生产身份、密钥、AEAD、逐跳认证与 Authorized Class 仍未完成。
- Core 仍不做分片；低于协议最小帧的 Carrier 仍由 Adapter 提供有界分段/重组。
