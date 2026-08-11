# UCN V5-05 路由感知自动选档实现报告

> 日期：2026-08-11  
> 状态：V5-05 的业务自动选档已完成；V5-12/V5-15/V5-18/V5-19/V5-24 已继续补齐控制面、Path Hop、Expanding Ring 和 Candidate Profile。Auto 仍默认关闭，实机效率归 S06/S07。

## 使用方式

Node 初始化仍为固定 W3。产品先用 `ucn_node_set_wire_profiles()` 设置固定 TX 上限与本机最大 RX，再按需调用：

```c
ucn_node_set_wire_profile_auto(&node, true);
```

关闭或不调用时，所有新发帧继续使用固定 TX 档。V5-05 当时 Auto 只覆盖新发业务帧；当前源码已经扩展为：HELLO/Heartbeat 可按一跳字段和 Link 能力选档，RREQ 每一轮按搜索 Scope 选档并在转发中保持，RREP/ACK/回复继承请求档，RERR、Path 与诊断控制帧按完整字段选择或保持档位。任何控制帧都不会在中继途中静默升降档。

## 选择输入

`ucn_frame_select_min_wire_profile()` 从 W0 向上检查：Network/Source/Destination/Session、Hop、Payload Length、Route Epoch、Path ID、最大配置档、Link MTU，以及 E2E Protected 时固定 16 B Tag。任何字段都不截断；无可用档位返回错误。

- 直连业务使用 1 Hop。
- AODV Active Route 使用已学习的 `hop_count`；未知路由按 2→4→8→16 Expanding Ring 逐轮选择能表达当前搜索 Scope 的最小档，每轮使用新 Request ID。
- 固定 Path 已由 V5-18 增加真实 `remaining_hops`；自动模式按该值而不是 `default_hop_limit` 选择档位，源/中继/终端分别校验并递减 Scope。
- HELLO 的线上档表示本帧编码，1 B Payload 独立发布对端最大 RX 档；`ucn_link_t.peer_wire_profile` 只记录该 RX Ceiling。Auto HELLO 可以使用满足一跳字段、MTU 和能力的最小档；静态 Link 也可显式设置或清除 Peer Ceiling，每个 Bearer 独立保存。
- 已解码/转发帧已有明确 Profile，Core 保持原值；若下一 Bearer 的已知 Ceiling 更小则失败，不升级、降级或截断。
- Candidate Route 保存发现时的实际 Profile；Probe、ACK、Activate 与 Epoch 都保持同一 Profile，错误 Profile ACK 失败关闭。

## 软件验证

- Selector：W0/W1/W2/W3 地址边界、Hop、Path ID、配置上限、MTU 和 Protected Tag。
- Node：固定模式不因对端较小档位静默改变；自动模式小地址直连选 W0，目标 300 选 W1，对端只支持 W0 时显式失败。
- 路由：先以 W3 RREQ 建立 A→B→C 路径，再开启 A 自动模式；2-Hop 业务在 A/B 两段均保持 W0。
- HELLO：W0 TX/W3 RX 与 W3 TX/W3 RX 双向入网后，两条 Link 均记录对端 W3 RX Ceiling，W3→W0 节点业务不再被错误拦截。
- V5-05 当时 Full/Lite/Nano Debug 分别通过 2/2、2/2、1/1；V5-33 当前 Full/Lite 为 10/10、Nano 为 1/1，WSL Full ASan+UBSan 与配置契约为 13/13，GCC `-fanalyzer` 与配置契约为 13/13。

当前结果证明协议选择逻辑和固定资源边界，不等于 Wi-Fi/CAN/UART 实际吞吐提升；Host 规模效率已有 V5-07/V5-10 证据，目标 ELF、真实介质容量和多板切换仍需 S06/S07。
