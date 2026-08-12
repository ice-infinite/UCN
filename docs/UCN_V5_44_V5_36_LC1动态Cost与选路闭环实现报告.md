# UCN V5-44 / V5-36 LC-1 动态 Cost 与选路闭环实现报告

> 文档编号：DOC-047
> 完成日期：2026-08-12
> 范围：Host 软件实现与回归；未进行真实 Wi-Fi/UART/CAN/USB 标定或烧录
> 协议边界：UCN Core 5.0.0 / Wire Protocol v5 不变

## 1. 结论

V5-44 与 V5-36 已完成软件闭环：UCN 现在能把 Adapter 的通用单跳指标按 LC-1 固定整数规则解析为本地 `effective_select_cost`，并在 Full Profile 中用于同 Neighbor Bearer、Candidate Route 的本地出口贡献和 Q1 `AUTO_BALANCE` Flow 评分。

这次没有修改帧头、RREQ/RREP/PATH_INSTALL 或业务载荷。稳定的 `route_cost` 继续作为线上可加的基础 Cost；Queue、失败率、RTT、介质质量等局部状态只影响本节点决策，不能传播成全网事实。因此仍是 v5 节点，不需要协议版本升级。

## 2. 实现组成

### 2.1 纯 LC-1 Resolver

新增：

- `include/ucn/ucn_link_cost.h`
- `src/core/ucn_link_cost.c`

公开 API：

- `ucn_link_cost_ewma_update()`：固定执行 `floor((3×previous+sample)/4)`；
- `ucn_link_cost_resolve()`：执行状态门、Unknown、固定惩罚表、管理偏置、陈旧惩罚和 `1..65534` 饱和；
- `ucn_link_cost_is_sufficiently_better()`：执行候选至少改善 20% 的精确整数比较。

Resolver 是无状态纯 C 函数，不读取 Node、RTOS、SDK 或驱动对象，不使用浮点和动态内存。Full/Lite/Nano 均可链接该 API；只有 Full Node 自动维护动态质量快照。

### 2.2 Metrics 与诊断

`ucn_link_metrics_t` 在原字段尾部追加以下可选输入：

- RX failure；
- medium busy；
- medium quality；
- busy/quality 是否来自同一原始计数；
- 快照单调时间戳；
- RTT 参考值；
- 静态管理偏置；
- Adapter 自有 bad-metric 累计计数。

Full 的 `ucn_policy_link_quality_snapshot_t` 保存各项独立 EWMA、LC-1 结果、排除原因、Invalid 位和拒绝计数。产品可通过既有 `ucn_node_get_link_quality()` 与 `ucn_node_get_policy_stats()` 读取本地诊断；本轮没有扩大远端 Policy Diagnostic Wire Payload。

兼容边界：Wire 完全兼容；公共 Metrics 结构尾部扩展要求外部 Adapter 随本版头文件重新编译，并应先清零再填写有效位。旧源码按字段赋值/清零初始化可继续工作，未提供的新字段全部按 Invalid 处理；不保证旧二进制与新头文件混合链接的 C ABI。

## 3. 选路闭环

### 3.1 同 Neighbor Bearer

Full 对当前活动 Bearer 与同 Neighbor 候选 Bearer 比较本地有效分：

1. Link Down、有效 MTU 为零、快照超过 5000 ms 等不可选条件立即排除；
2. Known 基础 Cost 优先于 Unknown；同分以当前活动 Bearer优先，无活动项时以 `link_id` 升序；
3. 候选必须至少改善 20%，连续 3 个 500 ms 样本成立；
4. 再在候选 Bearer 上完成 2 次 Probe/ACK；
5. 切换后保持 3000 ms，期间禁止新的软切换；硬失效不受保持期限制。

Probe 复用现有一跳 Heartbeat 控制格式，仍受固定控制预算与维护调度约束；正常业务在验证完成前继续走原链路。

### 3.2 Candidate Route

收到候选路线时，只替换“本节点本地出口的基础 Cost 贡献”为该 Link 的 LC-1 有效分，然后与活动路线比较。候选表、活动 Route 以及 RREP 中保存/转发的累计 `route_cost` 均保持原值，不把本节点拥塞写回网络。

多跳 Candidate 仍使用既有端到端 Probe/Activate 状态机；LC-1 没有把每次发送改成重新寻路，也没有删除原先的 Route Epoch/grace。

### 3.3 Q1 AUTO_BALANCE

新建或租约到期的 Q1 Flow 使用：

```text
score = effective_select_cost × (active_flow_count + 1)
```

选取最低分已验证 Path，同分选择较小 `local_path_id`。一个 Flow 在租约内仍固定到同一路径，不逐帧轮询、不复制、不条带化。未到期 Flow 只在 Path 硬失效或既有连续 3 次 Queue EWMA `>=800‰` 拥塞门限时重绑；单纯出现更低软 Cost 不会打断正在传输的 Flow。

Q0、`PINNED_STRICT` 与 `PINNED_FAILOVER` 没有改成软动态切换。Pinned 仍只按原有硬失败、约束和授权语义工作。

## 4. Profile 与资源边界

| Build Profile | 动态行为 | Host `sizeof(ucn_node_t)` | 相对 V5-33 记录 |
| --- | --- | ---: | ---: |
| Nano | Node 保持静态基础 Cost；纯 Resolver API 可选链接，无常驻快照。 | 2,648 B | +0 B |
| Lite | Mesh/Bearer 继续按基础 Cost；无 Full Policy 质量表。 | 6,024 B | +64 B |
| Full | 固定质量快照、LC-1、Bearer/Candidate/Q1 接入。 | 10,080 B | +328 B |

三档 `sizeof(ucn_link_t)` 都保持 40 B。GCC 13.3 Release `-O3` 下纯 Resolver 对象 `.text` 为 1,625 B；它存在于静态库不等于一定进入最终固件，Nano/Lite 产品未引用时可由最终链接器不拉入。以上都是 Host ABI/Archive 证据，不是 MCU ELF、任务栈或运行时 CPU 结论。

## 5. 软件验证

| 门禁 | 结果 |
| --- | --- |
| Windows MSVC Full Debug + Scale | 11/11 |
| Windows MSVC Lite Debug + Scale | 11/11 |
| Windows MSVC Nano Debug | 1/1 |
| Windows MSVC Full / Service OFF | 11/11 |
| Windows MSVC Full + 128 B/3-Link 产品头 + 配置契约 | 15/15 |
| WSL GCC 13.3、`-Wall -Wextra -Wpedantic -Werror`、ASan+UBSan+Leak | 11/11 |
| WSL GCC 13.3、`-fanalyzer -Werror` | 11/11 |

专项用例覆盖：

- LC-1 C01～C10；
- `65534` 饱和、非法管理偏置、Invalid 输入、busy/quality 同源去重；
- 25% 定点 EWMA 舍入；
- Full 动态分优于静态基础 Cost 的 Bearer 切换；
- 3 样本、2 ACK、3000 ms 保持期与陈旧快照硬失效；
- Candidate 使用本地动态贡献但保存的 Wire Route Cost 不变；
- Q1 Flow 使用有效分与活跃 Flow 数做确定性分散；
- Lite/Nano 的静态边界、公共 API 链接与现有协议回归。

MSVC 仍会报告既有 `ucn_endpoint.h`、`ucn_security.h` C4819 代码页警告；本轮没有新增编译错误或 Sanitizer/Analyzer 报告。

## 6. 尚未完成

以下内容不属于本次 Host 软件结论：

- V5-38～V5-40 的真实 UART、ESP-NOW/Wi-Fi、CAN-FD、USB CDC 与经典 CAN Adapter；
- V5-41 在不同速率、摆位、拥塞和故障条件下对默认 Cost、RTT 参考值与切换收益的实机标定；
- MCU ELF/Map、协议 Task 栈高水位、CPU、功耗和 ISR/驱动 WCET；
- 生产身份、密钥、AEAD 与逐跳控制面认证。

因此当前可以称为“动态 Cost/选路的协议代码与 Host 模拟闭环”，不能称为“所有真实介质的最优默认值已验证”。
