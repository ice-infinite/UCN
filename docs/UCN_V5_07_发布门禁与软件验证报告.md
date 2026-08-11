# UCN V5-07 发布门禁与软件验证报告

> 日期：2026-08-11  
> 分支：`codex/v5-adaptive-wire`  
> 结论：v5 的代码、Host 单元/模拟、资源复测和文档同步已完成；生产密码与目标板实机仍是独立门禁。

## 1. 本轮完成内容

- 将规模模拟器从旧版固定字节偏移统计改为调用 `ucn_frame_decode()`，因此 W0～W3 的 Source、Message Type 和 Wire Bytes 都按真实帧解释；解码失败会标记模拟失败。
- 新增 `--wire-mode fixed|auto`。`fixed` 使用保守 W3；`auto` 保持控制帧固定 W3，但业务帧按 Node/Route/Link/MTU/Tag 选择最小官方档。
- 三节点安全测试改为固定域 W3 建路、源端显式自动选档、受保护业务实际以 W0 通过 A→B→C；B 只转发密文且不调用 Open，C 解密后断言收到 W0。
- `test_profile` 同时报告 `sizeof(ucn_node_t)` 和 `sizeof(ucn_link_t)`。
- 同步架构、配置档案、Adapter 契约、使用手册、六种平台快速手册、网络参数总览、调用树和知识库。

## 2. 自动选档效率对照

参数：Full/Service OFF、Tree Local、256 Node、100 个测量 Tick、每 Node 每 Tick 4 条 Q1、Payload 32 B、80‰ 重复注入、固定 Seed `1554112803`。

| 模式 | Accepted/Delivered | Wire Bytes | Wire 效率 | 重复业务 | Route Loop | Harness 背压 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 固定 W3 | 102400/102400 | 6,503,840 | 50.383% | 0 | 0 | 0 |
| 自动最小档 | 102400/102400 | 5,175,840 | 63.310% | 0 | 0 | 0 |

本场景自动模式减少 1,328,000 B UCN Wire 流量，降幅约 20.42%。256 个 Node 中 ID 255/256 会迫使相关帧使用更宽档，因此这不是“所有业务都固定 W0”的理想化结果。CSV 证据：

- [固定 W3 汇总](results/V5/full_256_local_m4_dup80_fixed_summary.csv)
- [自动最小档汇总](results/V5/full_256_local_m4_dup80_auto_summary.csv)
- 同目录保留两种模式的逐节点 CSV。

Host 墙钟时间只用于发现模拟异常，不能写成 MCU CPU 性能或真实 Wi-Fi/CAN/UART 吞吐。

## 3. 软件回归矩阵

| 门禁 | 结果 |
| --- | --- |
| Windows Debug，Full + Service ON | CTest 3/3 通过（含 fixed/auto Scale Smoke）。 |
| Windows Debug，Lite + Service ON | CTest 3/3 通过（含 fixed/auto Scale Smoke）。 |
| Windows Debug，Nano + Service OFF | CTest 1/1 通过。 |
| GCC 14.2 Release，Full/Lite + Service OFF | 各 CTest 3/3 通过。 |
| GCC 14.2 Release，Nano + Service OFF | CTest 1/1 通过。 |
| WSL Ubuntu 24.04，Full/Service OFF，ASan+UBSan | CTest 3/3 通过；额外 256 Node 自动模式 M4/80‰ 重复通过，无 Sanitizer 报告。 |
| 最小 Build MTU 正向 | 本阶段历史值为 Nano 33 B、Lite 50 B、Full 64 B；V5-14 后 Lite 当前边界为 46 B。 |
| 最小 Build MTU 负向 | 当前 32/45/63 B 分别由静态编译门禁拒绝。 |

当前 33/46/64 B 是 Core 编译边界。通用测试程序含超出最小边界的 Golden Vector/业务载荷，因此没有把“最小 Core 可编译”误写成“最小 MTU 下完整 CTest 通过”。

## 4. Host 静态资源

Windows x64、GCC 14.2、Release `-O3`、Service OFF：

| Build Profile | `sizeof(ucn_node_t)` | `sizeof(ucn_link_t)` | `libucn_core.a` `.text` 合计 |
| --- | ---: | ---: | ---: |
| Nano | 2,648 B | 40 B | 19,116 B |
| Lite | 5,888 B | 40 B | 57,688 B |
| Full | 9,400 B | 40 B | 117,164 B |

自动选档状态已包含在上述 Node 大小中，没有运行时堆内存；每个 Link 的 Peer Ceiling 是固定字段。Archive `.text` 是 Host 裁剪证据，不等于任何 MCU 的 Flash。Adapter 队列、Service Router、RTOS Task 栈、驱动 DMA 和生产 Security Provider 均在 Node 对象之外。

> 当前值更新：V5-26 后重新测得 Nano/Lite/Full Node 为 `2648/5960/9744 B`，Archive `.text` 为 `19724/67316/125448 B`，Link 仍为 40 B，Storage Layout Version=4。本表保留 V5-07 当时的发布证据。

## 5. 仍需真实硬件完成的门禁

| 实机项 | 必须记录 |
| --- | --- |
| 两板固定 W0/W1/W3 | 实际线上 Profile、帧长、收发/丢包、错误码。 |
| 两板自动模式 | 小/大地址、不同 Session、明文/受保护 Payload 的实际选档。 |
| 不同 Peer Ceiling | HELLO 学习或静态配置后，无共同档必须失败且不发截断帧。 |
| Wi-Fi + UART/CAN 多 Bearer | 切换前后 Profile 不被中继改写；负载、Cost 和队列压力按真实 Adapter 采样。 |
| 小 MTU Carrier | 64 B CAN-FD 或具体串口封装；经典 CAN 必须先实现独立有界分段/重组。 |
| 生产安全 | 真实身份、AEAD、密钥轮换、持久 Counter/Replay、控制面逐跳认证和断电恢复。 |
| 目标资源 | ELF/Map 的 Flash/RAM、Task 栈高水位、最低 Heap、CPU、功耗和长时间稳定性。 |

上述项分别留在 S02、S06、S07；在它们完成前，v5 可以称为“Host 软件实现与模拟验证完成”，不能称为“生产安全完成”或“多介质实机性能已验证”。

## 6. 发布结论

V5-01～V5-05 已进入 Core，V5-06 已形成独立 Extended Gateway RFC，V5-07 完成软件发布门禁。Extended Gateway 不混入小 Core；没有 Linux 时，Lite/Full MCU 仍能独立发现、寻路、转发和恢复。下一阶段只进入生产密码与实机产品化，不再把缺少硬件的项目伪装成软件未完成。
