# 多串口、多 CAN 与多 Bearer 实例化

> 文档级别：`NORMATIVE GUIDE`
> 实现状态：公共多实例模型 `CURRENT`
> 最近核对：`a093862`，2026-08-25

一个 Node 支持多个 Link，不限制“只能一个 UART 和一个 CAN”。真实数量由 `UCN_MAX_LINKS`、Neighbor Bearer 上限、RAM 和产品配置决定。

## 每实例必须独立

| 项目 | 是否可共享 |
| --- | --- |
| Driver handle/context | 否 |
| local link ID | 否 |
| Stream Byte Ring | 否 |
| CAN Frame Ring/Carrier slots | 否 |
| Link metrics/timestamp | 否 |
| Adapter stats | 否 |
| Protocol Owner | 同一个 Node 共享一个 |
| Event Runtime | 可共享，作为多 Source 调度器 |

## 示例

```text
Node A
├─ UART1 Link → Node B
├─ UART2 Link → Node C
├─ CAN1 Link  → 总线 D/E/F
├─ CAN2 Link  → 独立控制总线
└─ ESP-NOW Link → Node B/C（产品自定义 Adapter）
```

若 B 同时经 UART1 和 ESP-NOW 可达，它们合并为 B 的两个 Bearer；若 C 只经 UART2 可达，则是另一 Neighbor。

## 配置步骤

1. 为每实例分配 Source/Adapter/Link storage；
2. 配置唯一 local link ID、MTU、Cost 和 Driver context；
3. 分别初始化 Driver/Source/Adapter；
4. 注册到同一个 Node；
5. 注册多个 Source 到 Event Runtime；
6. 由一个 Owner 统一 service/step。

## 容量

增加 Link/Source 会线性增加固定对象和 Ring RAM。产品应按真实接口数覆盖宏并运行配置合同、资源和压力测试。

## 实例化示意

```c
static uint8_t uart1_ring[UART1_RING_BYTES];
static uint8_t uart2_ring[UART2_RING_BYTES];
static ucn_stream_source_t uart1_source;
static ucn_stream_source_t uart2_source;
static ucn_can_source_t can1_source;
static ucn_link_t uart1_link, uart2_link, can1_link;
static ucn_event_runtime_t runtime;
```

每个 Source config 都指向自己的 storage、Driver context 和对应 Link；Runtime 只保存 Source 注册/调度信息，不把几个 Ring 合并。

## Local Link ID 规划

Link ID 在本 Node 内唯一即可，但产品应冻结可诊断映射，例如 `1=UART1, 2=UART2, 3=CAN1, 4=CAN2, 5=ESP-NOW`。不要按启动发现顺序动态随机分配，否则日志和 Path 配置重启后难以对应。

物理 peer 地址仍独立：CAN 总线上的多个 Node 可能共享一条 CAN Link/Source，再由 CAN ID/UCN Frame 识别 peer；点对点 UART 通常一个 Link 对一个直连 peer。

## 同 Neighbor 多 Bearer 的合并

Node B 从 UART1 和 ESP-NOW 都发来合法 HELLO 后，Neighbor B 有两个 Bearer。它们各自保持 liveness、Profile、MTU、Metrics；逻辑 Neighbor 的身份和安全准入必须一致。若两个物理 peer 都声称 Node B 但安全身份冲突，不能无条件合并。

## Event Runtime 公平

为 UART1/UART2/CAN1/CAN2 分别绑定 Source ID 和 budget。高频 UART1 持续 pending 时，round budget 到达后必须让 CAN1 前进。硬件可同时 DMA，但 Owner 串行处理完整 Frame。

## TX 并行和队列

Node 可以在同一个 step 中把不同 Flow 提交到不同 Link Driver，两个 DMA/控制器随后并行发送。每个 Driver 有独立 busy/queue。一个 Link `NO_SPACE` 不应阻止 Owner 推进其他 Link，但共享 Q0/Node budget 仍要维持公平。

## 热插拔/重配置

接口断开时只标对应 Link Down，撤销依赖 Route/Path；不要销毁仍被 Node 引用的静态对象。重新接入后 Driver/Source 清理旧 Carrier，更新能力，再恢复 Link/HELLO。若要彻底 unregister，必须遵守公开生命周期合同并确认没有表项引用。

## RAM 预算

```text
total_adapter_ram = Σ(each Link + Source + Ring + Driver queue)
                    + Event Runtime
                    + shared Node/Service/Transfer
```

UART 3M 可能需要比低速 CAN 更大的 Ring；不要给所有接口复制最大统一缓冲。配置减小 Ring 后还要在最坏 Owner 延迟下压力测试。

## 多实例验证矩阵

- 每实例单独收发和最大 Frame；
- 两/全部实例同时满载；
- 相同 Neighbor 双 Bearer；
- 一个实例断开/overflow/重配，其他实例继续；
- Q0 固定 CAN、Q1 Balance UART/Wi-Fi；
- Source budget 公平与 pending 恢复；
- Link ID/统计/Trace 能定位真实物理口；
- 总 RAM、Task stack、CPU、功耗和 Driver queue 高水位。
