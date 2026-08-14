# UCN V5-63 扩展环 Epoch 一致性与四节点三跳实测

> 日期：2026-08-14
> 结论：扩展环同路径 Epoch 不一致缺陷已由软件回归复现并修复；四块
> ESP32-S3-N16R8 的 UART-only 单源三跳九档消息 9/9 通过。
> 版本边界：不修改 Wire v5；只修正动态 Route Cache 的学习行为。

## 1. 问题现象

四节点线形拓扑 A→B→C→D 中，AODV-Lite 首次 2 Hop Ring 可以到达 B/C，
但不能到达 D。Ring Timeout 后以更大 Hop Limit 和新的 Request ID/Epoch 重试，
这次可以到达 D 并返回 RREP。

旧 `learn_route()` 只有新路线 Cost 更低时才更新 Epoch。B/C 在第二个 RREQ 上
看到的是同 Cost、同 Hop、同出口，因此只刷新路线寿命，反向 A 路线仍是旧
Epoch；A→D 的新正向路线却来自第二轮 RREP。合法业务带新 Epoch 到达 B 后，
会被源端反向路线的旧 Epoch 检查拒绝。

该缺陷表现为“路由表看起来正确、Ping/Transfer 却全部不达”，不是 UART
物理错误、CRC 错误或 Transfer 重组失败。

## 2. 修复规则

动态路线学习采用以下确定规则：

1. 新 Cost 更低：按既有规则替换出口、Cost、Hop 和 Epoch，并清除旧的端到端
   RTT 证据。
2. Cost、Hop、出口完全相同：视为同路径刷新；如果 Epoch 改变，先把旧
   Epoch/出口放入 `UCN_ROUTE_EPOCH_GRACE_MS`，再发布新 Epoch。
3. 只是同路径 Epoch 刷新时保留 RTT，因为端到端路径没有变化。
4. 更高 Cost 或不同但不更优的路径不因此覆盖 Active Route。

这保持固定内存、没有引入动态分配，也没有改变 Frame、RREQ 或 RREP 的 Wire
格式。

## 3. 软件回归

新增 A-B-C-D 测试明确执行：

- 2 Hop Ring miss；
- Ring Timeout 后 4 Hop Ring hit；
- A→D、B→A、B→D、C→A、C→D、D→A 六条路线 Epoch 相同；
- A 发出的 Q1 业务实际到达 D。

修复后结果：

| 配置 | 结果 |
| --- | --- |
| Windows Full Debug | 14/14 |
| Windows Lite Debug | 11/11 |
| Windows Nano Debug | 1/1 |
| Windows 128B/3-Link 产品头 | 5/5 |
| Windows Full Service OFF | 11/11 |
| 独立 WSL ASan/UBSan | 1/1 |
| 独立 WSL GCC `-fanalyzer` | 1/1 |

## 4. 四板 UART-only 实测

外部 Bench：
`E:\File\PlatformIO\ESP32_UCN\ESP32S3_N16R8_UCN_Test1`。

| 角色 | 端口 | Node ID | 物理连接 |
| --- | --- | --- | --- |
| A | COM40 | `0x10000001` | GPIO19/20 ↔ B GPIO20/19 |
| B | COM41 | `0x10000002` | GPIO15/16 ↔ C GPIO16/15 |
| C | COM43 | `0x10000003` | GPIO19/20 ↔ D GPIO20/19 |
| D | COM44 | `0x10000004` | 终点 |

三段 UART 均为 3,000,000 baud；Join ACL 只允许相邻节点；ESP-NOW 关闭，
只允许 A 发起周期业务，以隔离多源 Epoch 所有权问题。

最终路由 A→D 为 3 Hop/Cost 102，相关路线 Epoch 均为 10。九档结果：

| 消息等级 | D 实收 | A 完成耗时 | 结果 |
| --- | ---: | ---: | --- |
| T32 | 32 B | 0 ms | valid=1 |
| T64 | 64 B | 0 ms | valid=1 |
| T128 | 128 B | 13 ms | valid=1 |
| T256 | 256 B | 25 ms | valid=1 |
| T512 | 512 B | 32 ms | valid=1 |
| T1K | 1,024 B | 45 ms | valid=1 |
| T2K | 2,048 B | 71 ms | valid=1 |
| T4K | 4,096 B | 129 ms | valid=1 |
| T8K | 8,192 B | 260 ms | valid=1 |

汇总：9/9、85 Fragment、0 重试、0 失败；D 的 CRC 错、拒绝和过期均为 0。
75 s 新启动窗口没有 Guru Meditation、Panic 或 Watchdog。原始日志：
`test_results/v5_62_four_node_uart_only_epoch_fix_20260814.log`。

## 5. 尚未关闭的边界

- ESP-NOW-enabled 四节点基线仍出现多 Peer/Wi-Fi 路径相关 Panic，不能写作
  双 Bearer 已通过。
- 当前 Route Cache 的 Epoch 主要按目的地址维护；多个 Origin 同时发现共享目标
  时的所有权与覆盖需要 V5-64 单独设计和回归。
- 后续 T8K-only 清洁启动测试发现 Transfer 在首片无 Route 时会反复重启活动
  2-Hop Ring，阻止 250 ms 到期后扩到 4 Hop；该问题已由
  [V5-65](UCN_V5_65_Transfer冷启动寻路重入缺陷.md) 独立修复并通过自动 H1/H2/H3
  各 10 轮。本报告的九档业务先经过小消息/路线准备，仍只作为 V5-63 和当时数据面
  证据；冷启动结论以 V5-65 报告为准。
- 未执行三段 UART 逐段热断/恢复、中间节点离线、ESP-NOW 备份切换、多源高
  负载、长稳、CPU、功耗或 EMC。
- 本轮只证明这组四板、短线 3M UART、单源三跳环境；不外推为任意规模或
  任意介质的生产保证。
