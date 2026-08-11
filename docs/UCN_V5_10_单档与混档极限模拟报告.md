# UCN V5-10 单档与混档极限模拟报告

> 日期：2026-08-11
> 范围：Wire Profile W0/W1/W2/W3 单档节点、四档混合节点、Host 极限规模与故障压力。本文的“档位”不是 Nano/Lite/Full Build Profile。
>
> 当前一致性说明：CSV、交付率和 Host 工作量是 V5-10 的历史复现实验，不随协议文档改写；当前 Node/Archive 资源、Cost 线格式、Candidate Profile、Hop Scope、Path 能力、动态 MTU 与 PATH_INSTALL 双格式已由 V5-23～V5-33 更新。

## 1. 结论

本轮已经把原先固定 W3 的 Host 规模模拟器扩展为 `w0`、`w1`、`w2`、`w3`、`mixed` 五种布局，并保存逐节点、逐档和全网 CSV。正式汇总中的 19 组 Release 场景全部通过正确性门禁；Windows Debug 的 Lite/Full、GCC 严格 Release 的 Lite/Full 均为 `9/9`，Nano Debug 为 `1/1`；Full ASan+UBSan 的 Core、配置契约、五种布局 Smoke、地址负向和自动档共 `12/12` 通过。

需要分开理解两个结论：

1. **协议正确性没有发现档位特有故障。** 所有正式场景均无非法返回、重复业务投递、可执行 Route Loop 或 Harness 事件堆背压；混档故障场景也通过。
2. **大规模远端工作集会耗尽默认固定表能力。** 本地直连高负载均为 100% 交付；但所有节点同时建立两跳远端流时，254 Node 为约 15.35%，4096 Node 为约 11.38%。W0～W3 的交付数完全一致，说明瓶颈来自默认 Route/Discovery/Pending 工作集和并发建路，不是线编码档位。

## 2. 本轮发现并修复的问题

V5-08 已规定“最低够用 TX/W3 RX”，但旧 HELLO 把收到的固定 TX 档直接记录成 `peer_wire_profile` 接收上限。结果是 W0 TX/W3 RX 节点虽然能解码 W3，远端 W3 节点却会在发送前错误拒绝。

现在 HELLO Payload 使用 1 B 明确发布 `max_receive_wire_profile`：

- 入站帧自身的 Wire Profile仍表示发送编码档；
- HELLO 1 B Payload 表示该节点允许接收的最大档；
- 接收端拒绝非法档位以及“接收上限小于当前 HELLO 发送档”的矛盾声明；
- `ucn_link_t.peer_wire_profile` 只表示 Peer RX Ceiling，不再冒充 Peer TX 档。

专项测试使用 W0 TX/W3 RX 的 A 和 W3 TX/W3 RX 的 B 互发 HELLO 与业务，证明 A→B 使用 W0、B→A 可使用 W3。混档 Smoke 修复后在 Lite/Full 都通过。

## 3. 地址域极限

| 布局 | 本轮最大节点数 | 边界解释 |
| --- | ---: | --- |
| W0 | 254 | W0 的 1 B 地址中 `0` 非法、`255` 为广播，单一扁平域只有 254 个单播 Node ID。 |
| mixed（含 W0～W3） | 254 | 同一扁平域只要包含 W0，Heartbeat、HELLO、RREQ 和业务目标都必须能被 W0 表达，因此同样受 254 限制。255 已作为负向门禁拒绝。 |
| W1 | 4096 | 4096 是当前 Harness 主动上限，不是 W1 的地址上限。 |
| W2 | 4096 | 同上。 |
| W3 | 4096 | 同上。 |

跨越 254 后仍保留 W0 节点需要 V5-06 的 Domain/Gateway/Alias；不能把高 Node ID 截成 1 B，也不能把一次无效的 4096 四档混测写成协议失败。

## 4. 单档与混档本地高负载

Tree + local；100 个测量 Tick；32 B Payload。W0/mixed 在真实单域上限 254 Node、每 Node 每 Tick 8 帧；W1/W2/W3 在 Harness 上限 4096 Node、每 Node 每 Tick 4 帧。

| Wire布局 | Nodes | Generated/Delivered | 交付率 | 线效率 | P99 | 正确性 |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| W0 | 254 | 203,200 / 203,200 | 100% | 64.976% | 10 ms | PASS |
| W1 | 4096 | 1,638,400 / 1,638,400 | 100% | 59.724% | 10 ms | PASS |
| W2 | 4096 | 1,638,400 / 1,638,400 | 100% | 54.533% | 10 ms | PASS |
| W3 | 4096 | 1,638,400 / 1,638,400 | 100% | 50.988% | 10 ms | PASS |
| mixed | 254 | 203,200 / 203,200 | 100% | 57.350% | 10 ms | PASS |

mixed 的逐档线效率为 W0 `64.977%`、W1 `60.049%`、W2 `54.853%`、W3 `51.298%`，四档都为 100% 交付。结果符合头部从 17/21/26/30 B 递增的预期。

## 5. 两跳远端工作集

Tree + two-hop；100 个测量 Tick；每 Node 每 Tick 1 帧、16 B Payload；所有节点同时预热。

| Wire布局 | Nodes | Accepted/Delivered | 交付率 | 公平性 | 背压/重复/Loop | 正确性 |
| --- | ---: | ---: | ---: | ---: | --- | --- |
| W0 | 254 | 25,400 / 3,900 | 15.354% | 0.153543 | 0 / 0 / 0 | PASS |
| W1 | 4096 | 409,600 / 46,600 | 11.377% | 0.113770 | 0 / 0 / 0 | PASS |
| W2 | 4096 | 409,600 / 46,600 | 11.377% | 0.113770 | 0 / 0 / 0 | PASS |
| W3 | 4096 | 409,600 / 46,600 | 11.377% | 0.113770 | 0 / 0 / 0 | PASS |
| mixed | 254 | 25,400 / 3,900 | 15.354% | 0.153543 | 0 / 0 / 0 | PASS |

逐节点分批预热把 254 Node 的交付提高到 29.588%，但仍未达到 100%，说明冷启动风暴和默认 8 Route 等固定容量都会影响结果。`PASS` 只表示状态机正确性通过，不表示该负载满足产品交付率目标。

## 6. 丢包、重复、延迟、断链和 Q0/Q1 压力

条件：Tree + mixed traffic、每 Node 每 Tick 4 条 Q1、每 10 Tick 1 条 Q0、32 B Payload、25‰ 丢包、80‰ 重复、最大 7 ms 延迟、每 25 Tick 断一对 Link 并在 10 Tick 后恢复。

| Wire布局 | Nodes | Accepted/Delivered | 交付率 | P95/P99 | NoSpace | 重复/Loop/背压 | 结果 |
| --- | ---: | ---: | ---: | ---: | ---: | --- | --- |
| W0 | 254 | 102,812 / 29,546 | 28.738% | 20/30 ms | 1,311 | 0 / 0 / 0 | PASS |
| W1 | 1024 | 415,960 / 119,635 | 28.761% | 20/30 ms | 3,855 | 0 / 0 / 0 | PASS |
| W2 | 1024 | 415,960 / 119,635 | 28.761% | 20/30 ms | 3,855 | 0 / 0 / 0 | PASS |
| W3 | 1024 | 415,960 / 119,635 | 28.761% | 20/30 ms | 3,855 | 0 / 0 / 0 | PASS |
| mixed | 254 | 102,740 / 30,371 | 29.561% | 20/30 ms | 1,375 | 0 / 0 / 0 | PASS |

故障场景下低交付率包含固定表竞争、业务队列背压、主动丢包和断链，不应解释成介质实测速率。重要的正确性结论是重复注入没有形成重复业务投递，断链没有形成 Route Loop，事件堆也没有溢出。

## 7. Build Profile 与资源边界

Wire档位与 Build Profile 正交。Full 是本轮主极限矩阵；Lite 另外重放 W3/4096 和 mixed/254 本地高负载，两组均 100% 交付。V5-10 当时 Host ABI 的 `sizeof(ucn_node_t)` 为 Full `9400 B`、Lite `5888 B`；4096 个 Node 对应当时纯 Node 固定存储约 `38,502,400 B` 与 `24,117,248 B`。

V5-33 后当前 x64 GCC 14.2 Release/Service OFF 的 Nano/Lite/Full Node 为 `2648/5960/9752 B`，Link 为 `40 B`，Archive `.text` 为 `19884/68244/127792 B`，Storage Layout Version=5。V5-10 的 4096 Node 固定存储估算不能套用到当前结构。Harness 的事件堆、直方图和 CSV 内存不进入 MCU；也不能从 Host 工作时间推算 ESP32/STM32 CPU、Task 栈、功耗或真实空口容量。

## 8. 复现入口与结果文件

模拟器新增：

```text
--wire-profile w0|w1|w2|w3|mixed
--wire-mode fixed|auto
```

每个报告前缀输出：

- `_nodes.csv`：逐节点 Wire Profile、表/队列峰值、交付、字节、时延和 Host 工作量；
- `_profiles.csv`：同一混合网络中逐档聚合；
- `_summary.csv`：全网结论和 W0～W3 节点数量。

总表位于：

- `docs/results/V5_10/V5_10_extreme_summary.csv`
- `docs/results/V5_10/V5_10_profile_summary.csv`

批量入口为 `tools/run_ucn_scale_ladder.ps1`，可指定 Build Profile、Wire Mode、多个 Wire Profile 和节点阶梯。脚本会自动跳过 W0/mixed 超过 254 的无效组合。

## 9. 尚不能得出的结论

- 不能将 4096 Host 虚拟节点写成一条 Wi-Fi/CAN/UART 总线能同时承载 4096 节点。
- 不能将 11%～30% 的容量场景交付率写成协议日常交付率；它是默认固定表在指定全网并发工作集下的结果。
- 不能宣称 W0 已支持跨域高地址；V5-06 Gateway/Alias 仍未实现。
- 不能替代目标 MCU 的 ELF、栈/Heap、CPU、功耗、真实介质碰撞和多板长稳测试。
