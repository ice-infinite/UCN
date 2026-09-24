# UCN v6 简化版 Rust ESP32-S3 UART 一跳实测记录

## 1. 结论

2026-09-15，Rust 简化版在 B/COM53 与 C/COM58 的现有 UART 物理段上完成真实双向一跳闭环：

```ini
RUST_ESP32S3_UART_ONEHOP = PASS
PAIR                       = B(COM53) <-> C(COM58)
BAUD                       = 691200
UCN_FRAME                  = C1/A1/O0/H0/Q1, 16 B
STEADY_UNIQUE_RTT_SAMPLES  = 62
TIMEOUT/PROTOCOL_ERRORS    = 0
```

此前 OP-537 的 `BLOCKED_BY_PHYSICAL_LINK` 结论来自错误的相邻板假设，并非接线被改动，也不是
UART、Wire 或 Adapter 缺陷。用户确认六块板的串口接线从未变化；六板唯一周期 GPIO 探针最终
还原了实际物理链，改用真正相邻的 B/C 后，原始 UART 与正式 UCN 固件均立即通过。

## 2. 六板身份与实际物理链

| 身份 | 控制台 | MAC | 与下一物理节点连接的 GPIO |
| --- | --- | --- | --- |
| B | COM53 | `7c:4f:ad:2a:92:44` | GPIO19/20 ↔ C GPIO19/20 |
| C | COM58 | `7c:4f:ad:29:9c:d8` | GPIO15/16 ↔ D GPIO15/16 |
| D | COM56 | `7c:4f:ad:2a:8f:98` | GPIO19/20 ↔ A GPIO19/20 |
| A | COM40 | `7c:e8:b1:b1:ec:f4` | GPIO15/16 ↔ E GPIO15/16 |
| E | COM5 | `98:a3:16:e7:80:0c` | GPIO19/20 ↔ F GPIO19/20 |
| F | COM34 | `dc:da:0c:22:aa:d4` | 物理链末端 |

因此现场实际链为：

```text
B -- C -- D -- A -- E -- F
```

确认方法不是依靠板的摆放顺序或旧配置名，而是给 A～F 的 GPIO20/GPIO16 分配互不相同的
翻转周期，再在所有输入脚统计边沿数。证据见 `gpio-six-unique-map.log` 与
`gpio-six-unique-map-capture.json`。

## 3. 分层验证

### 3.1 GPIO 连接映射

六板同时运行唯一周期探针后，接收边沿频率唯一对应上述五段连接。该结果同时解释了为什么
A/COM40 与 B/COM53 都能发送却彼此收不到：它们本来就不是物理相邻节点。

### 3.2 原始 UART

在 B/C 上运行不含 UCN Wire 与 Adapter 的 UART2 探针，使用 GPIO19/20、691200 baud。约
64 秒后：

- B：`tx_frames=640`、`tx_bytes=2560`、`rx_bytes=2556`、TX/RX error=`0/0`；
- C：`tx_frames=640`、`tx_bytes=2560`、`rx_bytes=2564`、TX/RX error=`0/0`。

该测试使用正常 `esp-hal` UART API 即通过；正式固件不需要手写 UART 时钟或寄存器补丁。

### 3.3 正式 UCN 一跳闭环

两板烧录完全相同的正式镜像，按 eFuse MAC 自动选择 B/C 角色：

- 每帧 TX 执行 `reserve -> submit -> completion -> retire`；
- 两板首次 TX 都注入两次 `NOT_SUBMITTED`，以同 Token 有界重试；
- RX 执行 UART 成帧、`publish -> claim -> C1 decode -> retire`；
- 原始 UCN C1 帧为 16 B；测试 Carrier 只增加 `magic + length + CRC16`，总计 21 B，
  不属于 UCN Wire ABI；
- 双方交错 PING/ACK，检查 Origin Sequence 与错误计数。

35 秒最终稳态门禁结果：

| 节点 | 不重复 RTT 样本 | 最小 | 均值 | P50 | P95 | 最大 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| B | 31 | 322 µs | 323.65 µs | 324 µs | 325 µs | 325 µs |
| C | 31 | 323 µs | 323.52 µs | 324 µs | 324 µs | 324 µs |
| 合计 | 62 | 322 µs | 323.58 µs | 324 µs | 324 µs | 325 µs |

两端周期状态尾部均确认：

```ini
timeout=0
backpressure=2
carrier_err=0
wire_err=0
seq_err=0
tx_err=0
rx_err=0
```

启动后的首个 B 侧样本曾为约 10.057 ms，它属于两端复位、启动相位不同带来的首次同步样本，
不混入上述稳态统计。正式稳态汇总见 `summary-bc-steady-final.json`，原始日志见
`uart-onehop-bc-steady-final.log`。

关键工件 SHA-256：

| 工件 | SHA-256 |
| --- | --- |
| 正式 UART 一跳 ELF | `7D38BB1396077EB75AA316F2E924F0C0026905ADA25B284D8222FB6FEABD8E26` |
| 六板通用 smoke ELF | `5AEF44DDD01A646B6823C98695318485FE543AA04B6353EFA71C5DF9EF88FE3F` |
| 六板唯一周期映射日志 | `0FDEC0BF519903A9E9871762200BF8116C1D94849E0FC29D98183EEE98DE0314` |
| 最终稳态原始日志 | `67391FA5E0BB79796C9435A6D4BBD9E81BAD7305A2760C850554B120A45D18B0` |
| 最终稳态汇总 JSON | `4006FBF9ECEDEFD25755B7A252F8CA4DC0EFE209DF542A3A8F409E4035AF86FF` |
| A/D/E/F 恢复汇总 JSON | `0237BC9D7DD3FD3C98780D952A1E0ACDA8867859BB962D42E8C5016116A21073` |

## 4. 采集器判定说明

FTDI/USB 控制台偶尔会把同一条长日志拆成多个 USB burst。启动期原始证据仍保留；最终采集器
增加了 `--steady` 模式：只统计不重复的 `PING_OK`，并要求每端至少看到一条完整的零错误统计
尾部。它不会用缺失的日志前缀伪造失败，也不会忽略真实 Fault、读串口错误或协议错误字段。

## 5. 本次证明与未证明边界

本次已经证明：

- 六板身份、端口与实际 UART 邻接映射；
- B/C 的 GPIO 与 691200 UART 双向物理传输；
- Rust C1 Wire 与 Adapter Token 在真实 ESP32-S3 上的一跳双向闭环；
- 临时背压重试、接收槽退休、序列检查和稳定 RTT；
- 35 秒窗口内没有 Timeout、Carrier、Wire、Sequence、TX 或 RX 错误。

本次没有证明：

- 六板多跳与动态自动寻路；
- RREQ/RREP/RERR 或高级 Flow 的板端运行；
- Security、Admission、Capability、Persistence 的真实硬件闭环；
- DMA/ISR 生产 Driver、Flash 掉电、无线/CAN/USB Bearer；
- 长时间稳定性、功耗、最坏栈和生产放行。

因此它是 Rust 简化版基础通信的实机一跳 PASS，不是整个 Rust v6 简化协议或 RUST-10 的完成签字。

测试结束时，B/C 保留正式 UART 一跳固件；A/D/E/F 已恢复六板通用 Rust smoke 固件。四板各自
复位后均重新出现 `BOOT + SELFTEST PASS` 和 15 个连续心跳，汇总
`restored-smoke-adef.json` 为 `overall_pass=true`。诊断 GPIO 翻转固件没有留在非测试板上。
