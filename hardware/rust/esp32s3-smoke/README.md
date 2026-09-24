# UCN v6 简化版 Rust ESP32-S3 冒烟固件

该固件是 Rust 独立实现的首级实机门禁，不是生产 Adapter 或六板网络固件。它在同一块
ESP32-S3 上执行冻结 Core Wire、Routing 与 Flow Codec 的逐字节正向/负向测试，并让当前
Rust Workspace 的全部协议 crate 通过 Xtensa 目标编译。

同一镜像可烧录到所有测试板。板号由 Host 证据清单中的串口与芯片 MAC 绑定，不需要为
A～F 分别编译镜像。

```powershell
Set-Location E:\File\MESH\UCN\hardware\rust\esp32s3-smoke
. $env:USERPROFILE\export-esp.ps1
cargo build --release
espflash flash --chip esp32s3 --port COM40 target\xtensa-esp32s3-none-elf\release\ucn-v6s-rust-esp32s3-smoke
```

串口日志必须同时出现：

- `BOOT protocol=6 target=ESP32-S3`；
- `SELFTEST wire=PASS routing=PASS flow=PASS overall=PASS`；
- 连续递增的 `HEARTBEAT`。

该结果只证明板上 CPU 执行、Xtensa 目标兼容、串口输出和确定性 Codec 冒烟通过；它不证明
UART/ESP-NOW/CAN Adapter、动态寻路、多跳吞吐、真实 Flash、密码硬件或长期稳定性。

六板复位采集器位于 `tools/capture_multi_serial.py`。正式证据采用逐板独占串口采集，避免多个
FTDI 端口同时复位时的 Windows 驱动缓存噪声。2026-09-15 的首轮记录位于：

```text
docs/08-实现与验证/实机证据/V6/2026-09-15-rust-esp32s3-six-board-smoke/
```
