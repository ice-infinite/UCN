# UCN S21 极限规模模拟测试方案

## 1. 目标

S21 用电脑直接实例化大量真实 `ucn_node_t`，回答四个不同问题：

1. 稀疏网络中，UCN Core 能否在大量节点同时存在时继续正常运行。
2. 每个节点都持续发包时，交付率、延迟、协议字节效率和公平性如何变化。
3. 默认固定 Route/Neighbor/Discovery/Queue 表在远端工作集增大时从哪里开始成为瓶颈。
4. 丢包、重复、延迟、链路抖动和全对全路由震荡是否会产生环路、重复业务投递、无界增长或 Harness 自身溢出。

本测试不以一个孤立的“最大节点数”代替产品边界。最终结论必须写成“Profile + 拓扑 + 活跃目的地 + 负载 + 故障条件 + 验收门槛”下的最大稳定规模。

## 2. 证据边界

- 每个模拟节点运行仓库当前真实 Core、Node、AODV-Lite、QoS、Neighbor 和统计代码。
- Host Harness 只替换物理 Link，并用动态内存批量创建多个固定布局 Node；这不改变 MCU 固件的无 `malloc` 设计。
- 虚拟时间用于协议时延、路由收敛和故障复现；真实墙钟只描述当前电脑运行模拟器的速度。
- `sizeof(ucn_node_t)`、固定表当前/峰值可精确统计；MCU 的 Task 栈、ISR/DMA、CPU、Heap、功耗、空口冲突和驱动 WCET 仍必须实机测量。
- Harness 事件队列背压单独统计。只要它非零，该轮不能归因于 UCN Core 容量，应先扩大 `--event-slots` 重跑。

## 3. 模拟器结构

入口：`tools/ucn_scale_sim.c`，CMake 目标：`ucn_scale_sim`。

```text
业务发生器（所有 Node 同一虚拟时刻提交）
                    │
        多个真实 ucn_node_t 实例
                    │
      ucn_link_t + 可控 Link Metrics
                    │
  Host 二叉最小事件堆（延迟/丢失/重复）
                    │
       目标 Node 的 ucn_node_receive()
```

事件堆是 Host 测试资源，不进入 MCU。单线程按同一虚拟时刻依次执行所有 Node，不需要为每个节点创建 OS 线程；这样仍能产生并发业务和共享链路压力，同时可由固定 Seed 重放。

## 4. 拓扑与业务模式

| 维度 | 模式 | 目的 |
| --- | --- | --- |
| Topology | `tree` | 每节点度数不超过 4，4096 节点路径直径仍受控，适合规模阶梯。 |
| Topology | `ring4` | `±1/±4` 环形弦拓扑，观察路径长度接近 Hop Limit 时的边界。 |
| Traffic | `local` | 每个 Node 向直连邻居持续发送，测全网对象数量和纯 Link/Core 上限。 |
| Traffic | `two-hop` | 每个 Node 固定向两跳目的地发送，测动态路由建立与中继压力。 |
| Traffic | `pairs` | 前后半区远端配对，主动压 Route 工作集和中心转发节点。 |
| Traffic | `incast` | 多节点向一个目标汇聚，测热点路由/接收压力。 |
| Traffic | `all-to-all` | 目的地逐 Tick 轮转，测默认 8 Route 下的淘汰、发现和控制预算。 |
| Traffic | `mixed` | 固定远端 Q1 加周期 Q0，测实时业务、队列和维护公平。 |

## 5. 规模与场景矩阵

### 5.1 阶梯规模

`8 → 16 → 32 → 64 → 128 → 256 → 512 → 1024`

如果直连规模仍通过，再执行 `2048 → 4096`。4096 是首版 Harness 主动设置的测试上限，不是协议地址空间上限。

### 5.2 负载与故障

- 每 Node 每 10 ms 产生 1/4/8 条 Q1，Payload 分别采用 16 B 和 32 B。
- `mixed` 每 10 Tick 额外产生一条 Q0。
- 丢包 25‰、重复 80‰、最大延迟 7 ms。
- 每 25 Tick 随机断开一对 Link，10 Tick 后恢复。
- 冷启动路由风暴和预热后的稳定负载分开解释。
- 默认 `--warmup-batch 0` 表示全部 Node 同时预热，用于冷启动风暴；设置为 `1` 可逐 Node 建路，用于区分“控制面启动风暴”和“稳定路由后的业务容量”。
- 事件堆故意缩小的负向测试只验证 Harness 背压门禁，不计入协议规模结论。

## 6. 每节点统计表

每轮 `${prefix}_nodes.csv` 至少包含：

| 分类 | 字段 |
| --- | --- |
| 拓扑/表 | Node ID、Degree、Admitted、Route 当前/峰值、Discovery/Candidate/Path/Flow 峰值。 |
| 队列 | Q0/Q1/Pending Q1 峰值。 |
| 业务 | Generated、Accepted、Delivered、交付率、Payload Bytes。 |
| 线效率 | Origin Wire Bytes、Wire Efficiency、TX/RX/Forward/Control/Business 帧。 |
| 延迟 | P50/P95/P99/Max（虚拟毫秒）。 |
| 错误 | No Space、No Route、Link Down、其他拒绝、Core 控制预算丢弃。 |
| 资源 | Host ABI 的 `sizeof(ucn_node_t)`、该 Node 的 Host `step+receive` 工作时间。 |

`${prefix}_summary.csv` 记录全网总量、Jain 公平性、事件堆峰值、Harness 背压、重复业务、Route Loop、Node 固定存储总量、Host 分配量和墙钟耗时。

## 7. 判定规则

协议正确性一轮通过必须同时满足：

- 初始化和全部 Node Step/Receive 只返回声明允许的结果。
- 没有重复业务交付。
- 没有可执行的同 Epoch Route Loop。
- Harness 事件堆没有背压。
- 固定表和队列峰值不超过编译期容量。
- 进程无崩溃、无 Sanitizer 错误。

低交付率本身不自动等于“测试程序失败”：在全对全、路由表耗尽和故障注入场景，它是要记录的容量结果。但重复业务、Route Loop、Harness 溢出和内存错误始终是失败。

## 8. 使用方法

```powershell
cmake -S . -B build_scale -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build_scale --target ucn_scale_sim --parallel

.\build_scale\ucn_scale_sim.exe `
  --nodes 256 `
  --ticks 200 `
  --warmup-ticks 150 `
  --traffic two-hop `
  --messages-per-node 1 `
  --report-prefix docs\results\S21\full_256_two_hop
```

故障组合：

```powershell
.\build_scale\ucn_scale_sim.exe `
  --nodes 256 --ticks 200 --traffic mixed `
  --loss-per-mille 25 --duplicate-per-mille 80 --delay-ms 7 `
  --flap-every 25 --flap-duration 10 `
  --report-prefix docs\results\S21\full_256_fault
```

## 9. 不能由 S21 得出的结论

- 不能把 Host 墙钟换算成 ESP32/STM32 的 CPU 占用。
- 不能把虚拟 10 ms 延迟写成 Wi-Fi、CAN 或 UART 的真实时延。
- 不能证明真实空口同时容纳同等数量节点。
- 不能替代 S06/S07 的多板、Task 栈/Heap、功耗和物理断链测试。
- 不能将某个稀疏直连场景的 4096 Node 通过写成“所有 4096 Node 可以全对全通信”。
