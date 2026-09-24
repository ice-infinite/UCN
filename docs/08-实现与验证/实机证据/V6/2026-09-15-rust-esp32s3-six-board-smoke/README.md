# UCN v6 简化版 Rust 六板 ESP32-S3 首轮实机自检

## 1. 结论

2026-09-15，六块 ESP32-S3 均刷入同一份 Rust `no_std` 固件并完成逐板冷启动式复位采集。

本轮在以下受限范围内通过：

- 六块板均能从 Flash 启动 Rust 固件；
- C1/O0/H0 Wire 冻结 Golden 编解码与错误版本拒绝通过；
- Routing RREQ Golden 编解码与保留位拒绝通过；
- Flow Label Setup Golden 编解码与零 Candidate 拒绝通过；
- 六块板均连续输出 10 个 `overall=PASS` 心跳，无 Panic、WDT、Fault 或重启循环；
- 当前 Rust Owner/模块能共同链接进 ESP32-S3 固件。

本轮没有接入板间物理 Bearer，因此不能解释为两板通信、六板自动寻路、Flow 激活、吞吐、延迟、
无线安全、真实 Flash Provider 或掉电恢复通过。顶层 `release_ready` 保持 `false`。

## 2. 审计对象与源码状态

| 项目 | 值 |
| --- | --- |
| Git 分支 | `v6-simplified-rust` |
| 基线提交 | `9b22a04d149d7c177363c8066486b1de6fff7f79` |
| 工作区 | 非干净；包含尚未提交的 RUST-07 与本轮硬件固件/证据 |
| 芯片 | ESP32-S3 rev 0.2 |
| Flash | 16 MB |
| Rust | `rustc 1.97.0-nightly (esp)` |
| `espup` | 0.17.1 |
| `espflash` | 4.6.0 |
| `esp-hal` | 1.1.2 |

由于固件由非干净工作区构建，单独的 Git SHA 不能重建本轮二进制；必须同时核对下列源码与固件
摘要。后续正式候选应先提交完整 Rust 工作区，再重新构建并复测。

## 3. 板卡映射

| 板 | 串口 | USB-UART | MAC | 烧录结果 |
| --- | --- | --- | --- | --- |
| A | COM40 | FTDI | `7c:e8:b1:b1:ec:f4` | PASS |
| B | COM53 | FTDI | `7c:4f:ad:2a:92:44` | PASS |
| C | COM58 | FTDI | `7c:4f:ad:29:9c:d8` | PASS |
| D | COM56 | FTDI | `7c:4f:ad:2a:8f:98` | PASS |
| E | COM5 | CH343 | `98:a3:16:e7:80:0c` | PASS |
| F | COM34 | CH343 | `dc:da:0c:22:aa:d4` | PASS |

COM7、COM9、COM80、COM81 不属于本轮六块 ESP32-S3，未进行烧录或串口操作。

## 4. 固件自检内容

### 4.1 Wire

固件直接构造并编码冻结的 C1/A1/O0/H0 帧，要求输出精确等于：

```text
61 40 03 12 34 56 78 01 02 01 02 03 04 DE AD BE EF
```

随后独立解码并逐字段核对；将协议版本改坏后必须返回 `Error::Malformed`。

### 4.2 Routing

RREQ 固定向量为：

```text
01 02 03 04 11 22 33 44 03
```

要求编码、解码和逐字段比较一致；保留 Flag 置位后必须拒绝。

### 4.3 Flow

Label Setup 固定向量为：

```text
11 22 33 44 55 66 77 88 99 AA BB CC DD EE F0 01
```

要求编码、解码和逐字段比较一致；Candidate ID 为零时必须拒绝。

### 4.4 链接与存活

固件同时实例化或引用 `ucn-core`、`ucn-adapter`、`ucn-owner`、`ucn-persistence`、
`ucn-security`、`ucn-identity`、`ucn-admission`、`ucn-capability`、`ucn-routing` 和
`ucn-flow`。三个功能自检全部通过后，每秒重新输出一次聚合结果。

## 5. 实测结果

| 板 | SELFTEST | 心跳范围 | 心跳数 | Fault | 结论 |
| --- | --- | ---: | ---: | ---: | --- |
| A | PASS | 1～10 | 10 | 0 | PASS |
| B | PASS | 1～10 | 10 | 0 | PASS |
| C | PASS | 1～10 | 10 | 0 | PASS |
| D | PASS | 1～10 | 10 | 0 | PASS |
| E | PASS | 1～10 | 10 | 0 | PASS |
| F | PASS | 1～10 | 10 | 0 | PASS |

D/FTDI 在 Boot ROM 输出开始前偶尔回送一段旧串口缓存；采集器只在新的
`UCN_RUST_HW BOOT` 后接受心跳。应用自检行和 1～10 心跳均完整，未把主机驱动缓存噪声记成协议
或板端故障。

板端打印的 Nano 对象大小：

| 对象 | 字节 |
| --- | ---: |
| Core Node | 5,896 |
| Routing Owner | 1,832 |
| Flow Owner | 3,424 |
| Adapter `<1,2,2,64>` | 276 |
| Admission Owner | 1,256 |
| Capability Owner | 472 |
| Identity Owner | 2,808 |
| Typed Coordinator | 36 |
| Persistence Owner | 1,976 |
| Security Owner | 2,328 |

这些是各类型/实例的独立 `size_of`，不能直接相加当成最终任务栈或完整静态 RAM；共享对象、对齐、
BSS、栈和 Link Driver 均需在真实集成固件中重新测量。

## 6. 固件摘要

| 工件 | 大小 | SHA-256 |
| --- | ---: | --- |
| Release ELF | 2,086,468 B | `5aef44ddd01a646b6823c98695318485fe543aa04b6353efa71c5df9ef88fe3f` |
| 合并烧录 BIN | 157,360 B | `b28d4551fdaf4e778675f97f7656ed0988e4ad0828a49ce982f39ec970dc9024` |
| 应用分区使用 | 91,824 B / 16 MB | 0.56% |
| `src/main.rs` | — | `e339d30d465d2ee3c1cdcd98377904d08b26c44f0248df66ffef6495638ae0bd` |
| 采集器 | — | `38e18a5f4d60571e3b0cdef406c1426255e097eed4c3f791f61cb33121094267` |

Release 构建与 `cargo clippy -- -D warnings` 均通过。Espressif linker 仍报告一个 RWX LOAD segment
警告；本轮记录该工具链/链接脚本边界，不把它解释为协议测试失败，也不在未分析前忽略。

## 7. 证据文件

每块板都有独立原始串口日志和机器摘要：

- `A-COM40.log` / `A-COM40.json`
- `B-COM53.log` / `B-COM53.json`
- `C-COM58.log` / `C-COM58.json`
- `D-COM56.log` / `D-COM56.json`
- `E-COM5.log` / `E-COM5.json`
- `F-COM34.log` / `F-COM34.json`
- `manifest.json`：六板 PASS 与后续必需 HOLD 门禁
- `firmware-image.sha256`：固件和关键测试源码摘要

## 8. 下一步

下一项应实现 Rust ESP32-S3 Adapter/Bearer 参考固件，先在 A/B 上做一跳 C1/O0/H0 真实收发、
Token 生命周期、背压和计数，再扩展为 A—F 线形多跳 RREQ/RREP/RERR 与 Flow 激活。上述物理通信
通过前，本轮只标记为 `RUST_ESP32S3_SMOKE = PASS`。
