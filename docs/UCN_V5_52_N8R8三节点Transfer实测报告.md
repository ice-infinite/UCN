# UCN V5-52 N8R8 三节点 Transfer 实测报告

> 日期：2026-08-13
> 结论：UCN-Extended Transfer 已在一组真实 ESP32-S3 台架的 **115200 UART 两跳路径**上完成 T32～T8K 九档功能验收。九档数据模式和完整 CRC32 均正确；7 条分片消息共 85 片，0 重试、0 Transfer 失败、0 CRC 错。该结果不替代 ESP-NOW-only、CAN-FD、切换中 Transfer、并发/Q0 干扰、CPU 或功耗门禁。

## 台架

| 角色 | 板卡/端口 | Node ID | 连接与职责 |
| --- | --- | --- | --- |
| A | ESP32-S3-N16R8 / COM5 | `0x10000001` | UART RX15/TX16 ↔ B；发送九档消息 |
| B | ESP32-S3-N16R8 / COM34 | `0x10000002` | UART0 ↔ A、UART1 RX19/TX20 ↔ C；只由 Core 透明转发，不重组 |
| C | ESP32-S3-N8R8 / COM38 | `0x10000003` | UART RX19/TX20 ↔ B；1 个最大 8 KiB RX Slot |

三块板同时启用 ESP-NOW，UART 基础 Cost=34、ESP-NOW 基础 Cost=45。稳定后 A→C 与 C→A 都选择 UART 两跳、Cost 68；B 到两端为 UART 单跳、Cost 34。ESP-NOW 是本轮可用 Backup，但最终 Transfer 业务没有走 ESP-NOW-only 路径。

## 构建资源

| 固件 | 静态 RAM | Flash |
| --- | ---: | ---: |
| A / N16R8 | 63,716 B | 601,435 B |
| B / N16R8 透明中继 | 48,756 B | 596,095 B |
| C / N8R8 | 55,332 B | 597,615 B |

三目标均完成 PlatformIO 构建、完整写入和 Hash 校验。B 没有实例化 `ucn_transfer_t` 或重组 Buffer；A 的测试固件额外保留 8 KiB 发送 Payload，不能把三者差值直接写成 Core 固定开销。

## 九档结果

本配置 `UCN_MAX_FRAME_BYTES=250`，Core Payload 上限 218 B；扣除 14 B Transfer 信封后，每片最多 204 B 业务数据。

| Class | 长度 | 片数 | 完成状态/耗时 | C 端 CRC32 | 校验 |
| --- | ---: | ---: | --- | --- | --- |
| T32 | 32 B | 单帧 | `sent` / 1 ms（本地提交） | `C0BC2DF6` | `valid=1` |
| T64 | 64 B | 单帧 | `sent` / 1 ms（本地提交） | `5EB50AA5` | `valid=1` |
| T128 | 128 B | 1 | `delivered` / 53 ms | `17E7A9C8` | `valid=1` |
| T256 | 256 B | 2 | `delivered` / 91 ms | `14764FA1` | `valid=1` |
| T512 | 512 B | 3 | `delivered` / 258 ms | `166301D9` | `valid=1` |
| T1K | 1,024 B | 6 | `delivered` / 329 ms | `965DF9DC` | `valid=1` |
| T2K | 2,048 B | 11 | `delivered` / 640 ms | `49CDADB7` | `valid=1` |
| T4K | 4,096 B | 21 | `delivered` / 1,326 ms | `C52DBC6C` | `valid=1` |
| T8K | 8,192 B | 41 | `delivered` / 2,528 ms | `841F4239` | `valid=1` |

T32/T64 的 `sent` 只表示原单帧 Endpoint 已提交；C 端独立 `RX valid=1` 才证明实际到达。分片消息 A 端汇总为 `accepted=7, fragments=85, retried=0, delivered=7, failed=0`；C 端为 `rx=85, reassembled=7, rejected=0, crc_failed=0, expired=0, ack=85`。9 个阶段约 14.9 s 完成，其中包含每阶段 1.2 s 的测试间隔，因此不能把总时长直接当成链路净吞吐。

## Carrier 与运行时快照

- A/C 本轮 UART decode/length/overflow/noise-budget/no-space/partial 均为 0；B 的历史累计 decode=2 在本轮未增加；三端 Queue Drop=0。
- 最小内部 Heap：A=`294,608 B`、B=`309,432 B`、C=`305,036 B`。
- `loopTask` 栈余量：A=`5,008 B`、B=`4,972 B`、C=`5,020 B`。
- Transfer 同时保持 Heartbeat/Ping 运行；固件未启用 CPU Run-Time Stats，未连接功耗仪表。

原始串口证据保存在外部硬件工程 `E:\File\PlatformIO\ESP32_UCN\ESP32S3_N16R8_UCN_Test1\test_results\v5_transfer_20260813_225109.log`。主仓库不提交本机 COM 日志；测试代码、结果与边界分别同步到外部工程台账和本任务表。
