# UCN v6 简化版 Rust 独立实现

本目录承载 UCN v6 简化协议的独立 Rust 实现。它与仓库中的 C99 实现共享协议规范、Wire
Registry、Golden、Negative 和持久化恢复合同，但不链接 C archive，也不通过 FFI 转发协议逻辑。

当前状态：`RUST-02 / DONE / SELF-REVIEW PASS`；`RUST-03 / ALLOWED TO START`。

当前已经建立：

- Cargo Workspace；
- `no_std` 与禁止 `unsafe` 的默认边界；
- `ucn-types` 的强类型标量、Generation 与标准 Opcode/Suite Registry；
- `ucn-wire` 的 C0/C1 `O0/H0` 无分配、big-endian Codec；
- `ucn-owner` 的单写 Owner、精确 Callback Lease、公平 Mailbox 和 typed Coordinator；
- `ucn-adapter` 的固定容量 TX/RX Token、同步早到 completion 与原子 Driver Facts ingress；
- C/Rust 共用的逐字节 Golden 与 Negative fixture；
- Host、Cortex-M 编译和静态检查入口。

当前明确没有实现：

- C2～C5 Codec；
- O1/O2/H1/H2/H3 的认证、加密、Tag 与 Replay；
- Runtime、Endpoint、静态 Path 或四级业务队列；
- Persistence、Security、Admission、Route、Transfer、Realtime、Group、Cluster；
- ESP32-S3 工具链、固件或实机证明。

## 构建

```bash
cd rust
cargo fmt --all --check
cargo clippy --workspace --all-targets -- -D warnings
cargo test --workspace
cargo check --workspace --target thumbv7em-none-eabi
cargo check --workspace --target thumbv7em-none-eabihf
```

权威架构和实施顺序见：

- [Rust 独立实现总体设计](../docs/10-理论与规划/建议方案/UCN_v6_简化版_Rust独立实现总体设计.md)
- [V6 简化版任务表](../docs/00-项目管理/00-任务表.md)
- [项目操作记录](../docs/00-项目管理/01-项目操作记录.md)
- [RUST-01 实施及自审报告](../docs/08-实现与验证/版本演进/UCN_V6S_RUST_01_基础类型Registry与C0C1_Wire实施及自审报告.md)
- [RUST-02 实施及自审报告](../docs/08-实现与验证/版本演进/UCN_V6S_RUST_02_Owner_Coordinator与AdapterToken实施及自审报告.md)
