# T21.5 目标板静态资源报告

> 日期：2026-08-09
> 状态：已完成可复现的 ESP32 静态构建比较；`bearer_diag` 的空闲两板 Heap/栈已采样，持续负载/物理断链/功耗峰值待采样。
> 对应任务：[T21.5](00-任务表.md) · 真实双介质验收仍属于 T21.6。

## 1. 测量对象与边界

本报告构建的测试工程为 `E:\File\PlatformIO\ESP32_UCN\ESP32S3_N16R8_UCN_Test1`。其 `scripts/ucn_core_sources.py` 从 `E:\File\MESH\UCN\src\` 直接编译当前 C99 Core，不复制源码，因此结果对应本仓库的当前实现。

S3 对比 Profile 使用同一份 Node A 双介质测试应用、相同的 `UCN_MAX_FRAME_BYTES=250`、`UCN_ADAPTER_RX_QUEUE_DEPTH=4`、Arduino-ESP32 3.3.7、PlatformIO Espressif32 2026.3.30 与 release 构建；唯一变量是 `UCN_MAX_BEARERS_PER_NEIGHBOR`。正常可烧录的 Node A Profile 未显式写该宏，默认即为 `2`，并已得到相同的双 Bearer 构建结果。

这是一份**整机测试固件**的静态资源报告，包含 Arduino、Wi-Fi/ESP-NOW、UART Adapter、测试 Endpoint 和日志，不是把全部数字归因于 UCN Core。当前正常/诊断 A/B 已让 ESP-NOW Peer Link 与 UART Link 分别进入 Core；`DualMediaLink` 只保留在 legacy 对照 Profile。`resource_bearer1/2` 的表格仍只回答固定 Bearer 表的静态空间问题，不代替 T21.6 的实际切换时延、丢失/乱序或功耗验收。

## 2. ESP32-S3 静态构建结果

| Profile | `UCN_MAX_BEARERS_PER_NEIGHBOR` | RAM | Flash | `g_node` 静态对象 | `.dram0.bss` | `.flash.text` |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `esp32s3_120_16_8-qio_opi_resource_bearer1` | 1 | 42,852 B / 327,680 B (13.1%) | 582,183 B / 6,553,600 B (8.9%) | 5,040 B (`0x13b0`) | 26,880 B | 483,164 B |
| `esp32s3_120_16_8-qio_opi_resource_bearer2` | 2 | 43,108 B / 327,680 B (13.2%) | 582,679 B / 6,553,600 B (8.9%) | 5,296 B (`0x14b0`) | 27,136 B | 483,660 B |
| 增量（2 − 1） | +1 | **+256 B** | **+496 B** | **+256 B** | **+256 B** | **+496 B** |

目标符号表显示 `g_node` 正好增加 256 B，且 `.dram0.bss` 的总增量也正好为 256 B。因此，在当前默认容量、250 B MTU 与该测试应用下，第二条同对端 Bearer 的静态 RAM 增量可明确归因到 `ucn_node_t` 内的固定 Neighbor Bearer 数组；Flash 增量来自支持两槽的条件路径代码。

正常烧录 Profile `esp32s3_120_16_8-qio_opi_node_a`（默认双 Bearer）也已重新构建：RAM `43,108 B`、Flash `582,679 B`，与 `resource_bearer2` 一致。

## 3. ESP-WROOM-32 兼容构建

`esp32_wroom_32` 在同一 Core、250 B MTU、默认双 Bearer下构建成功：RAM `45,216 B / 327,680 B (13.8%)`，Flash `609,103 B / 1,310,720 B (46.5%)`。

该 Profile当前不启用 UART1 Link，因此它证明的是当前 v4 Core/ESP-NOW 测试应用能进入 ESP-WROOM-32 构建，不可与 S3 双介质应用横向比较，也不能据此推导 WROOM 的真实空口性能或双 Bearer 增量。

## 4. 运行时观测与已采样基线

正常 S3 测试固件的周期 `STAT` 后会新增一行：

```text
RESOURCE heap_internal_free=... heap_internal_min=...
         heap8_free=... heap8_min=...
         loop_stack_hw_words=... loop_stack_hw_B=...
```

- `heap_*_min` 是自启动后的最小剩余堆，可反映运行期间的堆高水位。
- `loop_stack_hw_*` 是当前 Arduino `loopTask` 的剩余栈高水位；数值越小越接近栈风险。
- Wi-Fi、ESP-NOW 驱动和其它 IDF 任务拥有独立栈，不能由该单个数值代替；产品若调整这些任务，必须再分别测量。

两块 S3 已烧录 `node_a/node_b_bearer_diag` 并同步复位：空闲 `RESOURCE` 为内部最小 Heap `315,432 B`（A）/`315,428 B`（B），8-bit 最小 Heap `8,096,960 B`（A）/`8,096,956 B`（B），`loopTask` 剩余栈 `5,400 B`（A）/`5,396 B`（B）。同一窗口两端均为 `links=2/admitted=1/count=2`、队列丢弃 0。此数据是空闲观察值，不是压力峰值。

仍应在持续业务、UART-only 压测、**物理** UART 拔线/复接、Wi-Fi 故障/恢复和三板多跳各至少记录一次 `RESOURCE` 行。它们完成前，本报告不声明峰值堆、驱动任务栈、CPU、功耗或真实无缝切换已验收。

## 5. 可复现命令

```powershell
Set-Location E:\File\PlatformIO\ESP32_UCN\ESP32S3_N16R8_UCN_Test1
C:\Users\y2389_4rq4ld9\.platformio\penv\Scripts\pio.exe run `
  -e esp32s3_120_16_8-qio_opi_resource_bearer1 `
  -e esp32s3_120_16_8-qio_opi_resource_bearer2 `
  -e esp32s3_120_16_8-qio_opi_node_a `
  -e esp32_wroom_32
```

如需复核 `g_node`，使用同一 PlatformIO toolchain 的 `xtensa-esp32s3-elf-nm.exe -C -S --size-sort firmware.elf`；如需复核段大小，使用 `xtensa-esp32s3-elf-size.exe -A firmware.elf`。这些 ELF 分析命令是静态证据，不替代板上峰值测量。
