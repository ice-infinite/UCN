# UCN V5-05 路由感知自动选档实现报告

> 日期：2026-08-11  
> 状态：Core/Host 软件完成；默认关闭，实机效率归 V5-07/S06。

## 使用方式

Node 初始化仍为固定 W3。产品先用 `ucn_node_set_wire_profiles()` 设置固定 TX 上限与本机最大 RX，再按需调用：

```c
ucn_node_set_wire_profile_auto(&node, true);
```

关闭或不调用时，所有新发帧继续使用固定 TX 档。自动模式只优化新发业务帧；HELLO、RREQ 和其他域控制帧继续使用固定档，以便证明路径能力和维持一致控制边界。

## 选择输入

`ucn_frame_select_min_wire_profile()` 从 W0 向上检查：Network/Source/Destination/Session、Hop、Payload Length、Route Epoch、Path ID、最大配置档、Link MTU，以及 E2E Protected 时固定 16 B Tag。任何字段都不截断；无可用档位返回错误。

- 直连业务使用 1 Hop。
- AODV Active Route 使用已学习的 `hop_count`；RREQ 曾用固定 TX 档穿越整条路径，因此该路径接收更小档位是单调安全的。
- 固定 Path 当前没有端到端 Hop 元数据，继续使用产品 `default_hop_limit`，选择更保守。
- HELLO 的线上档仍是对端固定 TX 档，但 1 B Payload 独立发布对端最大 RX 档；`ucn_link_t.peer_wire_profile` 只记录该 RX Ceiling。静态 Link 也可通过 `ucn_node_set_link_wire_profile_limit()` 显式设置或清除，每个 Bearer 独立保存。
- 已解码/转发帧已有明确 Profile，Core 保持原值；若下一 Bearer 的已知 Ceiling 更小则失败，不升级、降级或截断。

## 软件验证

- Selector：W0/W1/W2/W3 地址边界、Hop、Path ID、配置上限、MTU 和 Protected Tag。
- Node：固定模式不因对端较小档位静默改变；自动模式小地址直连选 W0，目标 300 选 W1，对端只支持 W0 时显式失败。
- 路由：先以 W3 RREQ 建立 A→B→C 路径，再开启 A 自动模式；2-Hop 业务在 A/B 两段均保持 W0。
- HELLO：W0 TX/W3 RX 与 W3 TX/W3 RX 双向入网后，两条 Link 均记录对端 W3 RX Ceiling，W3→W0 节点业务不再被错误拦截。
- Full/Lite/Nano Debug 分别通过 2/2、2/2、1/1。

当前结果证明协议选择逻辑和固定资源边界，不等于 Wi-Fi/CAN/UART 实际吞吐提升；规模效率、目标 ELF 和多板切换仍需 V5-07/S06。
