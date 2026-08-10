# UCN S21 极限规模模拟测试结果

## 1. 结论

本轮已完成电脑端多节点模拟器、8 节点自动 Smoke、8～4096 节点规模阶梯、远端路由工作集、高负载、故障和负向门禁测试，并输出全网汇总与每节点 CSV。

结论不能简化成一个脱离场景的“UCN 最多支持多少节点”：

- 在 `tree + local + 1×16 B/Node/10 ms` 的独立直连虚拟链路场景，Full Profile 从 8 到 Harness 主动上限 4096 节点均为 100% 交付，未发现环路、重复业务交付或 Harness 背压。
- 1024 节点直连高负载在 Full `4×32 B`、Full `8×32 B` 和 Lite `4×32 B` 三组均为 100% 交付。这证明固定 Node 实例和基本收发路径可在 Host 上稳定扩展，不代表一个 Wi-Fi 信道或一颗 MCU 能承载同样负载。
- 默认 Full 只有 8 条 Route 和有限 Pending Discovery。远端目的地工作集、集中转发以及全网同时冷启动时，交付率很快受到固定表和并发建路竞争限制；因此“4096 个对象可运行”不等于“4096 节点可全对全通信”。
- 25‰ 丢包、最大 7 ms 延迟和周期断链组合通过正确性门禁，交付率为 97.329%。
- 80‰ 重复注入在每节点每 Tick 4 条并发业务时暴露真实正确性问题：业务投递出现 1070 次重复，涉及 252 个接收节点。当前 8 项逐帧 Seen 环在乱序重复到达前被新帧淘汰，必须进入 S22 修复，不能把本轮写成“全部通过”。

## 2. 测试口径

- 构建：Windows x64、GCC 14.2、CMake/Ninja，Full/Lite/Nano Feature Profile。
- 时间：每个虚拟 Tick 10 ms；P50/P95/P99 只统计成功交付帧，不包含未交付消息，也不是硬件时延。
- `delivery_pct`：`Delivered / Accepted`；表内同时保留 `Generated / Accepted / Delivered`，避免路由表满或本机拒绝被交付率掩盖。
- Host Harness 为批量节点、虚拟 Link 和二叉事件堆使用动态内存；UCN Core 的 MCU 无堆、固定表设计未改变。
- `PASS` 表示无非法返回、无可执行 Route Loop、无重复业务交付、无 Harness 事件背压和无内存错误。容量场景交付率低仍可为 `PASS`，因为低交付率本身是被测容量结果。

## 3. 直连规模阶梯

固定条件：Full、Tree、`local`、100 个测量 Tick、每 Node 每 Tick 1 条 Q1、Payload 16 B。

| Nodes | Generated / Accepted / Delivered | Accepted→Delivered | Wire 效率 | P95/P99 | 事件堆峰值 | 结果 |
| ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 8 | 800 / 800 / 800 | 100% | 31.496% | 10/10 ms | 16/512 | PASS |
| 64 | 6,400 / 6,400 / 6,400 | 100% | 31.281% | 10/10 ms | 128/4,096 | PASS |
| 256 | 25,600 / 25,600 / 25,600 | 100% | 31.258% | 10/10 ms | 512/16,384 | PASS |
| 1024 | 102,400 / 102,400 / 102,400 | 100% | 31.252% | 10/10 ms | 2,048/65,536 | PASS |
| 2048 | 204,800 / 204,800 / 204,800 | 100% | 31.251% | 10/10 ms | 4,096/131,072 | PASS |
| 4096 | 409,600 / 409,600 / 409,600 | 100% | 31.250% | 10/10 ms | 8,192/262,144 | PASS |

完整 10 档数据见 [Tree Local 汇总](results/S21/ladder_tree_local_summary.csv)。4096 只代表当前 Harness 主动上限内的稀疏独立直连场景，不是协议地址空间或共享物理介质上限。

## 4. 高负载和 Profile 对比

固定条件：1024 Node、Tree、直连邻居、100 个测量 Tick、Payload 32 B。

| Profile / 负载 | Generated / Delivered | 交付 | Wire 效率 | Event HWM | 单 Node Host ABI | 全部 Node 固定存储 | Host 总分配 | 墙钟 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Full / 4 条 | 409,600 / 409,600 | 100% | 49.081% | 5,120/65,536 | 8,136 B | 8,331,264 B | 38,036,481 B | 583 ms |
| Full / 8 条 | 819,200 / 819,200 | 100% | 49.536% | 9,216/65,536 | 8,136 B | 8,331,264 B | 38,446,081 B | 1,070 ms |
| Lite / 4 条 | 409,600 / 409,600 | 100% | 49.081% | 5,120/65,536 | 5,456 B | 5,586,944 B | 35,120,129 B | 515 ms |

Host ABI 和墙钟只能用于同一台电脑上的档位/场景比较；不能换算为 ESP32/STM32 的 RAM、Task 栈、CPU、功耗或驱动吞吐。

## 5. 远端路由工作集

### 5.1 Tree 两跳

| Nodes | Generated / Accepted / Delivered | Accepted→Delivered | 活跃交付源 | Route HWM |
| ---: | ---: | ---: | ---: | ---: |
| 8 | 800 / 800 / 800 | 100% | 8 | ≤ 8 |
| 16 | 1,600 / 1,600 / 1,100 | 68.750% | 11 | ≤ 8 |
| 64 | 6,400 / 6,400 / 1,800 | 28.125% | 18 | ≤ 8 |
| 256 | 25,600 / 25,600 / 3,900 | 15.234% | 39 | ≤ 8 |
| 1024 | 102,400 / 102,400 / 12,500 | 12.207% | 125 | 最大 8、平均 5.999 |

这里所有业务均被本机 Q1/Pending 受理，但不是所有远端路由都在预热窗口内建成。完整数据见 [Tree Two-hop 汇总](results/S21/ladder_tree_two-hop_summary.csv) 和 [1024 节点逐节点表](results/S21/full_1024_tree_two-hop_nodes.csv)。

### 5.2 远端配对、汇聚和全对全

| 场景 | Nodes | Generated / Accepted / Delivered | Accepted→Delivered | 主要边界 |
| --- | ---: | ---: | ---: | --- |
| Tree pairs | 64 | 6,400 / 6,210 / 200 | 3.221% | 远端路径长、中心节点和并发建路工作集。 |
| Tree incast | 128 | 12,800 / 12,420 / 400 | 3.221% | 单目标及共享转发节点成为热点。 |
| Tree all-to-all | 32 | 3,200 / 782 / 481 | 61.509% | 2,418 条在本机受理阶段因固定容量拒绝，8 Route 无法容纳轮转目的地。 |
| Ring4 pairs / 同时预热 | 64 | 6,400 / 5,760 / 0 | 0% | 全网冷启动同时建远端路由。 |
| Ring4 pairs / 每次预热 1 源 | 64 | 6,400 / 6,192 / 1,200 | 19.380% | 分阶段预热缓解启动风暴，但固定 Hop/Route/中心状态边界仍存在。 |

对应汇总：[Pairs](results/S21/ladder_tree_pairs_summary.csv)、[Incast](results/S21/ladder_tree_incast_summary.csv)、[All-to-all](results/S21/ladder_tree_all-to-all_summary.csv)、[Ring4 同时预热](results/S21/ladder_ring4_pairs_summary.csv)、[Ring4 分阶段预热](results/S21/ladder_ring4_pairs_staged_summary.csv)。

## 6. Q0/Q1 混合

256 Node 的 `mixed` 场景中，Q1 为 25,600/25,600/3,900，Q0 为 2,560/2,560/390，二者成功比例同为 15.234%。Q0 不发起 RREQ，只能使用已经存在的路由；此处它复用同目的地 Q1 在预热阶段形成的可用 Route。结果说明 Q0 没有绕过路由容量，也没有因其优先级自动获得远端可靠性。完整数据见 [Mixed 汇总](results/S21/ladder_tree_mixed_summary.csv)。

## 7. 故障、重复与负向门禁

| 场景 | 结果 | 关键数据 | 解释 |
| --- | --- | --- | --- |
| 256 Node，4×32 B，25‰ 丢包，0～7 ms 延迟，每 25 Tick 断链 10 Tick | PASS | 102,400 生成，102,100 受理，99,373 交付；97.329%；公平性 0.9965；Event 1,347/16,384 | 无环路、无重复业务、无 Harness 背压。 |
| 256 Node，80‰ 重复，1 条/Node/Tick | PASS | 25,600/25,600；重复业务 0 | 当前负载下 Seen 可覆盖重复到达窗口。 |
| 同条件，2 条/Node/Tick | PASS | 51,200/51,200；重复业务 0 | 当前负载下仍通过。 |
| 同条件，4 条/Node/Tick | **FAIL** | 102,400 原始业务均交付，但另有 1,070 次重复业务，252 个节点受影响；Event 1,511/16,384 | 不是 Harness 溢出；默认 `UCN_SEEN_CACHE_SIZE=8` 的逐帧环在乱序重复返回前发生淘汰。 |
| 256 Node，8 条/Node/Tick，Event 人为缩至 512 | **FAIL（预期）** | Harness 背压 78,848，Event 512/512 | 证明门禁能把测试平台容量不足与 Core 容量分开。 |

重复异常的逐节点证据见 [M4 节点表](results/S21/full_256_tree_duplicate_m4_nodes.csv)。S22 将把普通业务序列窗口与 RREQ 的“相同请求但更低 Cost 可重新处理”语义拆开，使用固定、可配置的 `(Source, Session) + highest sequence + bitmap` 窗口；在资源报告和专项回归通过前，不直接扩大表掩盖问题。

## 8. 软件门禁

2026-08-11 最终复核：

- Windows Full Debug：CTest `2/2`。
- Windows Full Release：CTest `2/2`。
- Windows Lite Release、Service OFF：CTest `2/2`。
- Windows Nano Release、Service OFF：CTest `1/1`；Nano 不构建动态规模模拟器。
- WSL Ubuntu 24.04、GCC 13.3、Full/Service OFF、ASan+UBSan：CTest `2/2`；另跑 256 Node `local M4` 和 64 Node `two-hop`，均无 Sanitizer 错误。

## 9. 尚未验证

- 多 ESP32/STM32 的真实最大节点数、同时入网风暴、真实 Wi-Fi 空口争用和隐藏节点。
- UART/CAN/Wi-Fi 混合 Adapter 的真实仲裁、驱动队列、ISR/DMA 和 `send()` WCET。
- 每个 Profile 在目标 ELF 上的 `.bss/.data/.text`、Protocol Task 栈、Heap 最低点、CPU、功耗和温升。
- 真实断电、移动、遮挡、Bus-Off、无线干扰下的收敛时间、业务 P50/P95/P99 与丢失/乱序。

这些项目继续归 S06/S07 实机门禁。当前 S21 回答的是“软件状态机和固定资源在可控 Host 网络中如何扩展”，不替代硬件验收。
