# UCN V5-60 CAN/CAN-FD Frame Source 与经典 CAN Carrier 设计

## 1. 目标与边界

V5-60 只解决“CAN 控制器收到物理帧后，如何有界地交给 UCN”，不把任何厂商 SDK、引脚、波特率寄存器或 RTOS 对象写进 Core。每个 CAN 控制器各自创建一个 `ucn_can_source_t`，由产品层负责：

- 配置 CAN/CAN-FD 控制器、硬件过滤器和收发中断；
- 把 ISR/DMA 收到的完整物理帧写入 Source 的固定 Frame Ring；
- 用静态 CAN ID 映射回对应 `ucn_link_t`；
- 在 `Link.get_status/get_metrics` 中报告 Bus-Off、总线负载、错误计数和动态 MTU。

Source 负责固定队列、Owner 唤醒、CAN-FD 补齐校验、经典 CAN 分段重组、超时和可观测统计。它不动态分配内存，不在 ISR 中解析 UCN，不等待 Heartbeat，也不自动操作硬件恢复。

## 2. CAN-FD 直接承载

CAN-FD 的合法数据长度为 `0..8、12、16、20、24、32、48、64 B`。UCN 逻辑帧长度不一定正好等于合法 DLC，因此发送端选择“不小于帧长的最小合法长度”，并把尾部全部补零。

接收端按以下顺序处理：

1. 检查 FD、RTR、Error 等物理标志和合法数据长度；
2. 通过 CAN ID Resolver 找到 ingress Link；
3. 从 UCN v5 帧头探测真实编码长度；
4. 要求真实长度不超过 CAN-FD 数据长度，且尾部补齐字节全部为零；
5. 只把真实 UCN 长度提交公共 Adapter RX Queue。

CRC、版本、Network、Security 和 Replay 仍由后续 UCN Decode/Core 校验。长度探测不替代完整解码，也不会修改 v5 Wire 格式。

## 3. 经典 CAN 有界 Carrier

经典 CAN 最多 8 B，无法直接容纳最小 UCN 帧。V5-60 固定如下 Carrier：

| 段 | Byte 0 | Byte 1 | Byte 2 | Byte 3..4 | Byte 5..7 / Byte 3..7 |
|---|---:|---:|---:|---:|---:|
| START | `0xC1` | Transfer ID | `0` | UCN 总长度，大端 | 首 3 B 数据 |
| CONT | `0xC2` | Transfer ID | Segment Index `1..255` | 后续最多 5 B 数据 | — |

约束：

- START 固定 8 B；非末段 CONT 固定 8 B，末段长度为 `3 + 剩余字节数`；
- Segment Index 必须严格递增，乱序、重复、缺段不会拼接；
- 单次 Carrier 最多 `3 + 255 * 5 = 1278 B`，还必须不超过产品 `UCN_MAX_FRAME_BYTES`；
- Reassembly Slot 以 `(CAN ID, Extended 标志)` 区分来源，Transfer ID 区分一次传输；
- 新 START 可显式重启同一来源的旧传输；槽满不淘汰其他活跃传输；
- 完整 UCN 帧在公共 RX Queue 背压时保留在 Slot 中，后续 Owner Round 重试；
- 丢段只能在后续乱序段或固定超时到达时确定；超时使用 UCN 的 32 位回绕安全 Deadline。

UCN 帧自身 CRC16 会覆盖重组后的完整 Header、Payload 和认证 Tag。Carrier 不再增加第二套 CRC，避免重复开销。

同一个 CAN ID 的 Carrier 必须串行发送，不能把两个 UCN 帧的段交错。产品 `Link.send()` 应把完整 UCN 帧有界复制到固定 TX Frame Queue 后立即返回，由 TX Worker 为该 CAN ID 分配递增 Transfer ID 并逐段发送；新 START 在接收端会有意替换同 ID 的旧未完成传输。只有产品确认整条 Carrier 已完成、失败或被显式终止后，才能开始同 ID 的下一帧。

## 4. Bus-Off 与恢复

Source 状态固定为 `ACTIVE / ERROR_PASSIVE / BUS_OFF / RECOVERING`：

- `BUS_OFF` 和 `RECOVERING` 拒绝新物理帧并返回 `UCN_ERR_LINK_DOWN`；
- 进入 `BUS_OFF` 时清空该控制器尚未处理的 Ring 和所有 Reassembly Slot，禁止把故障前后的片段拼接；
- 硬件恢复由产品驱动完成，只有产品明确上报 `ACTIVE` 后 Source 才恢复接收；
- Source 统计 Bus-Off、恢复、丢弃和当前队列压力；产品必须同步让对应 Link 的 `get_status().is_up=false`，Core 才会按硬故障处理路由。

## 5. 固定资源与并发

- Task 写入使用 `enter_critical/exit_critical`；ISR 写入必须使用独立、成对、可恢复 token 的 ISR 临界区；
- 单个物理帧“全入或全拒”，Frame Ring 满返回 `UCN_ERR_NO_SPACE`；
- Source Service 只在唯一 Protocol Owner 中运行，每轮受 Runtime `max_work` 预算约束；
- 默认存储只是便利配置，产品可按 RAM 提供更小/更大的静态 Ring、Slot 和重组区；
- 多控制器通过多个独立 Source ID 并行接入 Runtime，不共享重组状态。

## 6. CAN ID 过滤与 Cost 输入

软件 Resolver 是硬件 Acceptance Filter 之后的第二道静态映射。未知 ID 返回 `UCN_ERR_NOT_FOUND` 并计入过滤统计，不创建 Node、不分配 Link。

Source 提供以下确定性指标，供产品 `Link.get_metrics()` 采样：

- Frame Ring 当前占用和千分比压力；
- Ring 满、物理格式错误、补齐错误、Carrier 乱序、超时、槽满、Adapter Queue 背压累计值；
- 当前 Bus State。

默认静态 Cost 仍来自标准 Preset；动态 Cost 使用产品驱动报告的真实总线负载、错误率、RTT/队列压力。Source 统计不能凭空代替控制器 TEC/REC、仲裁等待或总线利用率。

## 7. 验收门禁

软件测试必须覆盖：

- 两个独立控制器 Source/不同 CAN ID 映射；
- CAN-FD 17..64 B、全部 DLC 边界、非零补齐、非法 Flags/长度；
- 经典 CAN 正常重组、乱序、重复、丢段超时、重启、槽满、末段长度；
- Ring 满和恢复、Adapter Queue 背压保留、Bus-Off 清理与显式恢复；
- Task/ISR 临界区及缺 ISR 锁失败关闭；
- Full/Lite/Nano、Service OFF、128 B 产品头、ASan/UBSan 和静态分析。

Host 虚拟测试只证明 Source/Carrier 状态机，不代表真实 CAN 仲裁、Bus-Off 时序、收发器电气或不同 SDK 驱动已经验证。实机验证继续归 V5-61/S06。

## 8. 当前软件实现与资源结果

实现文件为 `include/ucn/adapters/ucn_can_source.h` 与 `src/adapters/can/ucn_can_source.c`；`ucn_frame_peek_encoded_size()` 位于 Frame Codec，只做 Carrier 所需的真实长度探测，不改变 v5 Wire。

当前 Host x64 `sizeof(ucn_can_source_t)=256 B`。默认 `8 Frame Ring + 2 Slot + 2×256 B` Convenience Storage 为 1184 B；128 B 产品头把默认容量覆盖为 `4 Ring + 1 Slot + 1×128 B` 后为 464 B。对象、Storage、Event Runtime、公共 Adapter RX Queue、Node 和驱动 TX Queue 必须分别计量；这些 Host ABI 数字不能冒充 ESP32/STM32 的目标结果。

软件门禁结果：Windows Full/Lite/Service OFF 各 11/11、Nano 1/1、128 B 产品配置 5/5；WSL ASan+UBSan 和 GCC `-fanalyzer` 各 1/1。真实控制器接入仍未执行。
