# UCN V5-45 产品配置全量回归报告

> 文档编号：DOC-041。
> 状态：代码与 Host 软件验证完成；不包含硬件、驱动或实际介质性能结论。
> 分支：`codex/v5-adaptive-wire`；本任务不修改任何线上帧或 Core 运行语义。

## 1. 目标与复现配置

V5-35 已证明静态 Preset Resolver 能在产品公共配置下编译和链接，但完整历史测试仍隐含默认容量。V5-45 的目标是让既有代表性产品头也能运行完整 Full 测试集，而不通过放宽产品门禁掩盖问题。

`tests/config/ucn_test_user_config.h` 固定以下代表性低资源参数：

| 项目 | 值 |
| --- | ---: |
| `UCN_MAX_FRAME_BYTES` / `UCN_MAX_PAYLOAD_BYTES` | 128 B / 64 B |
| `UCN_MAX_HOPS` | 8 |
| `UCN_MAX_LINKS` / `UCN_MAX_NEIGHBORS` / 每邻居 Bearer | 3 / 4 / 1 |
| Service Binding（总 / Q0 / Q1） | 4 / 1 / 3 |
| Bridge Validator | 1 |

这不是统一推荐的产品规模，只是验证全局覆盖是否真正能传递到完整测试矩阵的代表性组合。

## 2. 修复方式

| 测试区域 | 原隐含前提 | 现在的派生规则 |
| --- | --- | --- |
| Frame | 最大 Payload 只按默认 256 B Frame 计算 | 同时由当前帧头、E2E Tag、`UCN_MAX_FRAME_BYTES` 与 `UCN_MAX_PAYLOAD_BYTES` 限制。 |
| Wire/Profile | W1～W3 初始化固定使用 16 Hop | 使用当前 `UCN_MAX_HOPS`；测试仍覆盖档位编码，不假设产品搜索半径。 |
| AODV Ring | 写死 `2→4→8→16` 与具体时间点 | 从当前最大 Hop 递增至上限，核对请求数、扩张数与最终耗尽。 |
| Service / Bridge | 固定需要 6 Binding、2 个 Q0 Binding 和 2 个 Validator | 根据当前容量选用合法最小 Binding 子集；容量足够时仍执行双 Q0/双 Validator 分支。 |
| 动态压力 | 无条件创建 32 Node×4 Link | 先检查每 Node 至少有 4 个 Link/Neighbor；不满足时报告跳过该拓扑，固定资源边界测试仍运行。 |

所有变化都在测试代码中。Core、公开协议、Wire 格式、MTU 决策、Preset 数值和产品配置文件均未为本测试而放宽。

## 3. 验证结果

| 构建/测试 | 结果 |
| --- | --- |
| Windows MSVC Debug，默认 Full | CTest `4/4` 通过。 |
| Windows MSVC Debug，默认 Lite | CTest `1/1` 通过。 |
| Windows MSVC Debug，默认 Nano | CTest `1/1` 通过。 |
| Windows MSVC Debug，Full + 128 B 产品头 | CTest `2/2` 通过：完整 `ucn_tests` 与 `ucn_standard_adapter_product_config_test`。 |
| WSL GCC Debug，默认 Full | CTest `4/4` 通过。 |

MSVC 保留已有 `ucn_endpoint.h` 和 `ucn_security.h` 的 C4819 代码页警告；本任务没有新增该类警告。

## 4. 正确解释压力结果

默认 Host 动态压力的拓扑是 32 Node×4 Link。代表性产品头的 `UCN_MAX_LINKS=3`，所以它没有足够的每 Node Link 表项承载该场景。测试输出会明确标记跳过，不能将此配置写成已通过 32×4 压力；它继续运行固定资源边界测试，因此低资源配置的容量门禁仍受覆盖。

要验证 32×4 场景，应使用至少 `UCN_MAX_LINKS=4`、`UCN_MAX_NEIGHBORS=4` 的配置，或另建并记录一个符合该拓扑的产品配置。Host 压力也不替代 MCU RAM、CPU、空口吞吐、功耗或三板收敛实测。

## 5. 后续

下一项实现任务是 V5-37：为裸机、FreeRTOS、Zephyr、NuttX 建立 Protocol Owner、固定队列、统一 `step_at` 时钟和 Host Fake HAL 外壳。真实 UART/ESP-NOW、CAN-FD/USB、经典 CAN Carrier 与目标板标定仍分别属于 V5-38～V5-41。
