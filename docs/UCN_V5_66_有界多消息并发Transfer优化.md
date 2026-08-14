# UCN V5-66 有界多消息并发 Transfer 优化

## 1. 目标与结论

V5-66 解决的是：同一节点连续发送大量独立 T128～T1K 可靠消息时，原单 TX Slot
必须逐条等待端到端完成，消息 RTT 会随跳数重复累加。

本次没有删除分片、完整消息 CRC32、累计 ACK、有界重试、Deadline、按需寻路或
小 MTU 兼容，而是在单条 Transfer 的 Fragment 窗口之外，增加了**多条独立
Transfer 的固定槽并发门控**。v5 的 Fragment/ACK 消息格式、编号和协议版本不变。

默认和未知 Peer 仍只允许 1 条分片消息在途。只有发送产品确认目标节点已经配置
足够 RX Slot 后，才可显式提高并发数：

```c
ucn_transfer_set_peer_capability(&transfer, peer,
                                 UCN_TRANSFER_CLASS_T2K);
ucn_transfer_set_peer_concurrency_capability(&transfer, peer, 4U);
```

发送端还必须编译 `UCN_TRANSFER_TX_SLOTS >= 4`，接收端必须编译
`UCN_TRANSFER_RX_SLOTS >= 4`。并发数为 0、未知 Peer、广播地址或超过本机固定
TX Slot 时失败关闭。T32/T64 继续走单帧 Endpoint，不受此限制。

## 2. 为什么保持为可选 Profile

四块 ESP32-S3-N16R8、3 Mbaud UART、A-B-C-D、1/2/3 跳实测中，
四槽 Profile 完成九档 × 三跳 × 三轮共 `81/81` 阶段，重传、Invalid、Duplicate、
发送拒绝和消息失败均为 0。

三跳吞吐相对单槽基线：

| 消息 | 单槽 KiB/s | 四槽 KiB/s | 变化 |
| ---: | ---: | ---: | ---: |
| 128 B | 32.520 | 84.229 | +159.0% |
| 256 B | 41.308 | 108.628 | +163.0% |
| 512 B | 69.972 | 155.857 | +122.7% |
| 1 KiB | 105.263 | 157.908 | +50.0% |
| 2 KiB | 138.737 | 156.862 | +13.1% |
| 4 KiB | 166.666 | 156.862 | -5.9% |
| 8 KiB | 186.046 | 150.943 | -18.9% |

因此四槽适合大量独立的中等消息，不适合无条件替换单槽大消息 Profile。单条 4/8 KiB
流继续使用默认单槽；后续需要单独优化“多槽对象中只有一个活动大消息”的快速路径。

## 3. 固定资源代价

ESP32 Bench 的发送端使用一个 8 KiB 只读 Arena 按消息长度切片，没有复制成四个
8 KiB Payload。A 静态 RAM 为 `64,556 B`，相对单槽基线 `64,364 B` 只增加
`192 B`。

接收端的每个 RX Slot 仍保留最大消息缓冲。四 RX Slot 的 B/C 静态 RAM 为
`83,716 B`，相对单槽 B 基线 `59,020 B` 增加约 `24.1 KiB`；运行时
`sizeof(ucn_transfer_t)` 观测为发送端 `8,888 B`、四 RX Slot 接收端 `33,404 B`。

因此：

- 小 RAM 节点保持 `TX/RX_SLOTS=1`，或降低 `UCN_TRANSFER_MAX_MESSAGE_BYTES`；
- 中继节点不绑定 Transfer Endpoint 时无需创建 Transfer 对象，只透明转发；
- 只有确实需要多个独立中消息并发的主控/网关才配置 2～4 个 RX Slot。

## 4. 兼容边界

- Wire v5 不变，旧节点不会因为本机开启四槽而自动收到并发 Transfer。
- `ucn_transfer_set_peer_capability()` 建立 Peer 时，并发能力自动默认为 1。
- 首版不扩展 HELLO；能力来自产品静态配置或受控能力目录。
- 公共 C 结构体随预发布源码增加字段，产品必须使用同一版本头文件全量重编译；
  不承诺与旧预编译对象保持二进制 ABI。
- 并发能力只限制 T128～T8K 的分片 Transfer；它不改变 Q0/Q1、窗口大小或路由策略。

## 5. 验证

- Windows Full `11/11`、Lite `11/11`、Nano `1/1`。
- 专用 `TX_SLOTS=4/RX_SLOTS=4` 产品构建 `12/12`。
- 单元测试覆盖默认 1、非法/未知能力，以及显式并发 2 后两条 T128 同时在途并交付。
- ESP32 六环境构建成功；四板正式矩阵 `81/81`，累计 648 KiB，业务完整性全通过。

外部原始证据位于：

```text
E:\File\PlatformIO\ESP32_UCN\ESP32S3_N16R8_UCN_Test1\test_results\
  v5_message_hop_pipe4_final_3m_h{1,2,3}_run{1,2,3}_20260815.log
  v5_message_hop_pipe4_final_3m_samples_20260815.csv
  v5_message_hop_pipe4_final_3m_summary_20260815.csv
  v5_message_hop_pipe4_final_vs_baseline_3m_20260815.csv
```

## 6. 后续门禁

1. 优化单活动大消息快速路径，要求四槽 Profile 的 4/8 KiB 不低于单槽基线。
2. 加入 Q0 控制流和多源并发，检查维护/Q0 延迟、Queue 水位与公平性。
3. 只有旧/新节点混合测试完成后，才评审是否通过可选 HELLO 扩展自动协商并发能力。
