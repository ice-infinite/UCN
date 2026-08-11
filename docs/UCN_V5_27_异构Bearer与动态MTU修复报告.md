# UCN v5 异构 Bearer、动态 MTU 与 Policy 修复报告

> 日期：2026-08-11
> 对应任务：V5-27～V5-30
> 对应审计：V5-AUD-01、V5-AUD-02、CUR-AUD-01

## 1. 修复结论

三项审计问题已完成代码与 Host 软件测试闭环：

| 问题 | 当前处理 |
| --- | --- |
| 异构主备 Bearer 可能形成 Path 黑洞 | Path 发送前使用逻辑下一跳全部合格 Bearer 的共同 Wire Profile 上限和最小 MTU；控制器还可把整条 Path 的瓶颈写入每跳表项。中继发现已封装帧不兼容时撤销 Path、累计诊断并回送 Path-RERR。 |
| `link->mtu == 0` 语义矛盾 | 冻结为“静态 MTU 未知”；由 `get_status().mtu` 提供运行时值。两者都非零取较小值，两者都为零时 Link 暂不可发送。Full、Lite 共用 Full Core，Nano 使用相同公共辅助函数。 |
| Policy 把主 Bearer 故障误判为 Path 故障 | Link 质量仍逐物理 Link 采样，但 Path 活性、拥塞和 AUTO_BALANCE 评分都先解析逻辑下一跳当前 Bearer；备链健康时保持或恢复 `VERIFIED`，全断或 Wire Path 不存在时才 `DOWN`。 |

这表示软件状态机不再静默保留已知不可达 Path；不表示真实 Wi-Fi/UART/CAN 多板切换时延已经验证。

## 2. 动态 MTU 公共契约

统一计算入口为 `ucn_link_effective_mtu(link, status)`：

| `link->mtu` | `status->mtu` | 有效 MTU |
| ---: | ---: | ---: |
| 非零 | 非零 | 两者较小值 |
| 非零 | 0 | 静态值 |
| 0 | 非零 | 运行时值 |
| 0 | 0 | 0，当前不可发送 |

因此 Adapter 可以注册 `.mtu = 0` 的动态 Link。注册阶段只在静态值非零时校验最小接收帧头；每次发送和自动选档都会重新读取状态 MTU。运行时值从 0 恢复后不需要重新注册 Link。

`status->mtu` 是 Adapter 已完成必要分段/重组后提供给 UCN Core 的逻辑帧上限。Core 本身仍不做跨 Link 分片。

## 3. Path 能力模型

### 3.1 本地逻辑下一跳

对于属于同一 Neighbor 的 Primary/Backup Bearer，Core 对所有当前 `ADMITTED/SUSPECT` 且可用的 Bearer 求交：

```text
maximum_wire_profile = min(各 Bearer 的 peer RX Ceiling)
minimum_mtu          = min(各 Bearer 的有效 MTU)
```

源节点在 E2E Seal 前按这个交集选档。这样主 W3/备 W0 时，正常帧会直接选择 W0；主 MTU 256/备 MTU 40 时，帧必须在 Seal 前满足 40 B。

### 3.2 整条 Path 瓶颈

新增公开能力值：

```c
typedef struct ucn_path_capability {
    ucn_wire_profile_t maximum_wire_profile;
    uint16_t minimum_mtu;
} ucn_path_capability_t;
```

产品控制器已知整条路线时，使用：

- `ucn_node_install_local_path_capable()`；
- `ucn_node_send_path_install_capable()`。

把相同的端到端瓶颈安装到源和每个非终端转发表项。Core 总是再与本地 Bearer 交集求较窄值，控制器只能收窄、不能放宽物理能力。

原 `ucn_node_install_local_path()` 仍由每跳派生本地交集；原 `ucn_node_send_path_install()` 在 V5-31 后固定发送 v5 基础 Schema，以兼容旧 v5 接收端。若后续跳比源端已知范围更窄，中继会执行第 3.4 节的确定性失效，不会静默黑洞。

### 3.3 PATH_INSTALL 双格式载荷

V5-31 后同时保留两种精确 Schema：

```text
基础：PathID(P) + Destination(A) + NextHop(A) + Lease(4) + RemainingHops(1)
扩展：基础 + MaximumWireProfile(1) + MinimumMTU(2)
```

基础格式 W0/W1/W2/W3 为 `8/11/14/17 B`，由 `ucn_node_send_path_install()` 发送；扩展格式为 `11/14/17/20 B`，只由 `ucn_node_send_path_install_capable()` 显式发送。新接收端接受两种格式，其他长度失败关闭。基础格式以及扩展格式中的 `MaximumWireProfile=UNSPECIFIED + MinimumMTU=0` 都表示接收节点派生本地交集；其他扩展组合必须同时有效。旧 v5 节点不理解扩展长度，因此调用 capability API 前必须确认目标支持。Node Storage Layout Version 已由 4 升到 5。

### 3.4 中继确定性失效

受保护帧的 Profile 属于 AAD，中继不能重新编码。若当前 Path 能力返回：

- `UCN_ERR_UNSUPPORTED`；或
- `UCN_ERR_TOO_LARGE`，

则中继执行：

1. `path_capability_failures++`；
2. 撤销匹配 `(Owner, Session, PathID, Destination)` 的本跳表项；
3. 向源方向发送 Path-scoped RERR；
4. 源端撤销对应 Path，并让 Policy 进入 `DOWN`/重建流程。

低 TX 档节点只要 RX 上限和下一 Bearer 允许，仍可透明转发已经收到的高档 E2E 帧；中继不会调用 `open()`。

## 4. Policy 与负载均衡

`ucn_policy_refresh_link_quality()` 只负责物理 Link 快照。每次真正完成采样后，Node 层针对每个 Policy Path：

1. 核对 Wire Path 是否仍存在、未过期且绑定关系一致；
2. 通过 Neighbor/Bearer 选择器解析当前实际 egress；
3. 使用当前 egress 的 `is_up`、Cost 与 Queue Pressure；
4. 当前 egress 健康时，旧 `DOWN` 可恢复为 `VERIFIED`；
5. 没有可用 egress 时才清拥塞样本并置 `DOWN`。

AUTO_BALANCE 的基础 Cost 和持续拥塞采样也使用当前 Bearer，不再读取已经故障的原始主 Link。

## 5. 软件验证

专项测试覆盖：

- Full/Nano 静态 MTU、仅运行时 MTU、共同最小值、两者均零、运行时缩小和恢复；
- 主 W3/备 W0，同时备链 MTU 40 B，E2E Path 在主链阶段就选择共同 W0，切换后继续交付；
- 首跳宽、后续跳只支持 W0，W3 中继失败后 Path-RERR 和两端撤销；
- 首跳宽、后续跳 MTU 40，`TOO_LARGE` 后 Path-RERR 和两端撤销；
- W0 TX/W3 RX 中继透明转发 W3 E2E 帧且不解密；
- `PINNED_STRICT` 主 Bearer 掉线并跨过质量采样周期后，经备链继续发送；恢复迁移和全断边界；
- AUTO_BALANCE 继续通过既有流亲和、拥塞重绑定和 down 重绑定回归。

验证结果：

| 环境 | 结果 |
| --- | --- |
| Windows Full Debug/Release | 各 10/10 |
| Windows Lite Debug/Release | 各 10/10 |
| Windows Nano Debug/Release | 各 1/1 |
| WSL Full ASan+UBSan + 配置契约 | 13/13 |
| WSL GCC `-fanalyzer` + 配置契约 | 13/13 |
| `git diff --check` | 无空白错误 |

## 6. 保留边界

- S02/V5-21 生产身份、密钥、AEAD 与 Authorized Class 仍未完成；本修复不扩大安全宣称。
- S06/S07 的 ESP32/STM32、多板、多介质、实际切换时延、吞吐、CPU、栈和功耗仍待实机。
- 控制器不提供整条 Path 瓶颈时，Core 能保证失败可见和可重建，但不能保证后续窄跳仍可无缝承载原帧。
- Core 仍不做分片；小于最小帧的 Carrier 必须由 Adapter 提供有界分段/重组。
