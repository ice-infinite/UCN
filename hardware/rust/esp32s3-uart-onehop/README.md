# UCN v6 简化版 Rust ESP32-S3 UART 一跳测试

该固件是六板 Rust 冒烟之后的第二级实机门禁。B、C 使用同一镜像，固件按 eFuse MAC
自动识别角色，并通过现有物理接线执行双向 C1 一跳通信：

- B：MAC `7c:4f:ad:2a:92:44`，控制台 `COM53`；
- C：MAC `7c:4f:ad:29:9c:d8`，控制台 `COM58`；
- B TX GPIO20 → C RX GPIO19；
- C TX GPIO20 → B RX GPIO19；
- UART2，`691200 baud`，双方共地。

六板唯一周期 GPIO 探针确认，现场不变的实际物理链顺序是：

```text
B(COM53) -- C(COM58) -- D(COM56) -- A(COM40) -- E(COM5) -- F(COM34)
```

板号只表示设备身份，不表示桌面上的接线顺序。后续多跳测试必须以该实测映射为准。

每一帧发送都真实经过 Rust `ucn-adapter` 的：

```text
tx_reserve → tx_submit → Completed → tx_retire
```

首次发送还会注入两次零副作用 `NOT_SUBMITTED`，验证同一 Token 的有界重试。每一帧接收
都经过：

```text
UART 成帧 → rx_publish → rx_claim → C1 decode → rx_retire
```

UART 是字节流，测试固件使用 `magic + length + C1 + CRC16` 恢复物理帧边界。该封装只属于
测试承载，不是 UCN Wire 字段，也不改变 C1 原始字节。

```powershell
Set-Location E:\File\MESH\UCN\hardware\rust\esp32s3-uart-onehop
. $env:USERPROFILE\export-esp.ps1
cargo build --release
espflash flash --chip esp32s3 --port COM53 target\xtensa-esp32s3-none-elf\release\ucn-v6s-rust-esp32s3-uart-onehop
espflash flash --chip esp32s3 --port COM58 target\xtensa-esp32s3-none-elf\release\ucn-v6s-rust-esp32s3-uart-onehop
```

正式采集必须确认双方均出现 `PING_OK`，`SUMMARY` 中 `timeout/carrier_err/wire_err/seq_err/
tx_err/rx_err` 均为 0，并保留原始双串口日志和 Host 汇总 JSON。

板已经持续运行时，可使用稳态门禁；它通过不重复 `PING_OK` 和周期性零错误统计尾部判定，避免
USB 串口把一条日志拆成多段时误判：

```powershell
python tools\capture_two_node.py `
  --b COM53 --c COM58 --seconds 35 --minimum-pings 20 --steady `
  --output uart-onehop.log --summary summary.json
```

本门禁证明真实 ESP32-S3、真实 UART 线、C1 Wire 和 Adapter Token 生命周期能够闭环；它不
证明自动寻路、多跳、生产非阻塞 DMA/ISR Driver、无线承载、加密、Flash 掉电或长期稳定性。

## 三节点动态路由固件

`ucn-v6s-rust-routing-three-node` 使用同一镜像覆盖 B/C/D：B 与 D 各自发起 Discovery，C
转发 RREQ/RREP 并对每个 C1 数据帧调用 `RouteOwner::resolve()`。B 还验证错误 RERR 不误删、
软件失效后旧路由立即拒绝和自动重发现。

```powershell
Set-Location E:\File\MESH\UCN\hardware\rust\esp32s3-uart-onehop
. $env:USERPROFILE\export-esp.ps1
cargo build --release --bin ucn-v6s-rust-routing-three-node

$elf = Resolve-Path target\xtensa-esp32s3-none-elf\release\ucn-v6s-rust-routing-three-node
espflash flash --chip esp32s3 --port COM53 $elf
espflash flash --chip esp32s3 --port COM58 $elf
espflash flash --chip esp32s3 --port COM56 $elf

C:\Users\y2389_4rq4ld9\.platformio\penv\Scripts\python.exe tools\capture_three_node.py `
  --b COM53 --c COM58 --d COM56 --seconds 35 --minimum-pings 12 `
  --output routing-bcd.log --summary summary.json
```

该固件的路由控制 Payload 和 C1 数据帧使用生产 Rust codec；外层 UART Carrier 仍只用于
字节流成帧。本固件没有接入 Adapter Token、Security、Flow 或持久化，不能替代对应专项门禁。
