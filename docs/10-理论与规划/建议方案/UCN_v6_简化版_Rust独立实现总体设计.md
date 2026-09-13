# UCN v6 简化版 Rust 独立实现总体设计

> 文档状态：`RUST-01 / DONE / SELF-REVIEW PASS`
>
> 对应分支：`v6-simplified-rust`
>
> 基线：C 简化版 `IMPL-02 = DONE / EXTERNAL REVIEW GO（受限 Host 软件范围）`

## 1. 目的

本项目新增一套 Rust 实现，用于验证 UCN v6 简化协议能否在保持 MCU-first、固定内存、低 Wire
开销和可裁剪模块边界的前提下，获得更强的类型、生命周期和并发安全。

Rust 版不是新协议版本，也不是 C 版包装器。C 与 Rust 必须遵守同一份语言无关协议合同：

```text
协议 RFC / Wire Registry / Golden / Negative / 恢复矩阵
                         │
             ┌───────────┴───────────┐
             │                       │
        C99 独立实现             Rust 独立实现
             │                       │
             └───── 互操作与差分门禁 ─────┘
```

任何一份实现的当前行为都不能单独反向修改协议规范。发现实现冲突时，先停止编码，回到冻结合同裁决。

## 2. 目标与非目标

### 2.1 目标

1. 默认 `#![no_std]`，不依赖操作系统和堆分配。
2. 所有容量编译期确定，达到上限后失败关闭，不驱逐安全状态。
3. 保留单一 Protocol Owner、Coordinator 编排和模块 Owner 边界。
4. 与 C 版逐字节 Wire 互通，拥有独立 Encoder、Decoder 和状态机实现。
5. Feature OFF 时对应状态、符号、依赖和 Wire 终止能力同时消失。
6. 将 `unsafe` 集中在 Port/HAL 边界；协议 crate 默认禁止 `unsafe`。
7. Host、Cortex-M 和后续 ESP32-S3 使用相同协议核心，不建立 Host 特权路径。

### 2.2 非目标

1. 不把 C API 逐函数翻译成 Rust API。
2. 不通过 FFI 调用 C 版完成协议处理。
3. 不使用 `serde`、`bincode` 或 Rust 内存布局作为线上格式。
4. 不默认引入 Tokio、Embassy 或其他异步 Runtime。
5. 不在 `RUST-00` 实现正式 Wire、路由、安全、持久化或簇状态机。
6. 不因 Rust 类型安全降低密码、掉电、物理链路和实机验证要求。

## 3. 权威关系

Rust 实现的输入优先级固定为：

1. V6 最终协议架构 RFC；
2. V6S-00 冻结合同；
3. Wire Registry、Golden、Negative 和持久化恢复矩阵；
4. 全局不变量与 Owner Connection Registry；
5. 已外审通过的模块专项合同；
6. C 版实现，仅作为差分证据和错误案例来源。

三类测试必须区分：

| Oracle | 作用 | 不能证明什么 |
| --- | --- | --- |
| 规范 Golden/Negative | 决定唯一线上字节和非法输入 | 完整状态机正确性 |
| C↔Rust 差分 | 找到实现分歧 | 两边没有复制同一错误 |
| Rust 独立属性/故障测试 | 验证 Rust 所有权与状态机 | 真实 MCU/Flash/射频行为 |

## 4. Workspace 与 crate 边界

```text
rust/
├─ Cargo.toml
├─ rust-toolchain.toml
└─ crates/
   ├─ ucn-types          # 无行为基础类型
   ├─ ucn-wire           # C0～C5 严格 Codec
   ├─ ucn-owner          # Owner/Fence/Handle/Generation
   ├─ ucn-adapter        # Driver Facts、RX/TX Token
   ├─ ucn-core           # 基础队列、Endpoint、静态通信
   ├─ ucn-persistence    # Record/Journal/Witness/Recovery
   ├─ ucn-security       # Session、认证、加密
   ├─ ucn-admission      # 动态准入与 Capability
   ├─ ucn-routing        # SoftRoute、Discovery、Flow
   ├─ ucn-transfer       # Reliable、Fragment、Operation
   ├─ ucn-realtime       # 可选实时和时间同步
   ├─ ucn-group          # 可选群组通信
   ├─ ucn-cluster        # 可选簇管理
   ├─ ucn-runtime        # 选定模块的 Coordinator 装配
   └─ ucn-testkit        # Fake Driver/Provider 与 Oracle
```

`RUST-00` 只创建 `ucn-types` 与 `ucn-wire` 的空行为边界。其余 crate 必须随对应任务逐个加入，禁止
预建空壳后宣称模块已经支持。

## 5. 依赖方向

```mermaid
flowchart TD
    Types[ucn-types]
    Wire[ucn-wire]
    Owner[ucn-owner]
    Adapter[ucn-adapter]
    Core[ucn-core]
    Persist[ucn-persistence]
    Optional[security / routing / transfer / realtime / group / cluster]
    Runtime[ucn-runtime]

    Wire --> Types
    Owner --> Types
    Adapter --> Types
    Core --> Types
    Core --> Wire
    Core --> Owner
    Core --> Adapter
    Persist --> Types
    Persist --> Owner
    Optional --> Types
    Optional --> Owner
    Runtime --> Core
    Runtime --> Persist
    Runtime --> Optional
```

模块 Owner 不直接调用另一个模块 Owner。跨模块 Requirement/Event 必须由 Runtime Coordinator
路由。Driver 和 Provider 只能通过各自 Adapter 发布完成事实。

## 6. `no_std`、内存和资源合同

默认 Feature 下禁止：

- `extern crate alloc`；
- `Vec`、`Box`、`String`、哈希表或运行期增长容器；
- 递归协议处理；
- 无界 iterator 收集；
- 无界队列扫描；
- 在回调中借用可变 Runtime 再次进入公共写 API。

每个 Profile 由生成配置给出精确容量。实现可以使用固定数组、位图和带显式长度的 slot 表；是否
采用第三方 `heapless` 必须另行审计其 MSRV、panic、代码尺寸和审计面，`RUST-00` 不引入依赖。

每个模块必须输出 `size_of`/`align_of`、固定队列/slot、Host 与目标 Flash/RAM、最大调用栈或
实机任务栈水位，以及 Feature OFF 前后差值。

## 7. 执行与并发模型

Rust 不使用共享可变全局状态。默认运行模型仍是单一写 Owner：

```text
ISR/Driver/Provider
       │ 原子事实、token、generation
       ▼
固定槽/队列 ──唤醒提示──> Protocol Owner
                              │
                              ▼
                         Coordinator
                              │
                     每模块有界 step/budget
```

通知允许合并或丢失，协议真值必须保存在 token、slot 或 durable record 中。低频维护 step 负责发现
遗漏的 wakeup 和到期状态。

```text
fn step(runtime, now, budget):
    require runtime.owner.try_enter()
    facts = adapter.take_bounded_facts(budget.fact_cap)
    coordinator.route(facts)
    coordinator.step_classes_with_rotating_cursor(now, budget)
    runtime.owner.leave()
```

Rust 借用检查不能代替 ISR/SMP 原子协议、临界区和硬件内存屏障。跨中断状态必须使用已审计的
Port 原语。

## 8. Wire 实现合同

Rust Codec 必须：

1. 手工按冻结 offset 和 big-endian 读写；
2. 先完整校验，再写输出；
3. 拒绝输入/输出的完整或部分别名合同违规；
4. 不依赖 `repr(Rust)`、结构体转字节或宿主字节序；
5. raw→typed 与 typed→raw 使用独立 fixture；
6. 先通过冻结 Golden，再允许 round-trip；
7. 任何保留值、错误长度、非法 C/O/H 组合均失败且输出不变。

```text
encode(value, output):
    validate_every_field(value)
    validate_exact_output_length(output)
    write_to_local_fixed_staging()
    copy_staging_to_output_once()
```

## 9. Persistence 合同

Rust Persistence 必须独立实现但保持相同 96 B Envelope、16 B Marker、摘要、Witness 和恢复结果。
禁止调用 C Foundation，也禁止根据 C 结构体布局序列化。

恢复由共享原始槽 fixture、C↔Rust 介质镜像差分和 Rust 独立掉电/撕裂测试共同验证。真实 Flash
原子性仍必须在目标硬件证明，Host 文件或内存 Provider 不构成掉电证明。

## 10. Security 与密码边界

Security crate 只接受显式 Suite/Key/Generation、canonical AAD 和调用方固定 workspace。密码实现
通过受控 Provider trait 注入；Feature OFF 不保留密钥槽、Replay 窗口或密码依赖。

密码 Provider 是允许 `unsafe` 的外部边界之一，但安全协议 crate 仍保持 safe Rust。任何
Provider callback 重入、同步早到、异步 token 错绑或 buffer overlap 必须在首次 sequence reserve
之前拒绝。

## 11. Feature 与 Profile

Cargo Feature 表达静态组合，不表达线上协议版本。初始规则：

- `default = []`；
- `std` 只用于 Host Adapter、日志和测试；
- `alloc` 不作为 MCU 协议核心依赖；
- `security`、`routing`、`transfer`、`realtime`、`group`、`cluster` 分别独立；
- 非法 Feature 组合在编译期拒绝；
- 关闭模块后仍能透明转发的能力由 Wire Contract 决定，不能解释为本地支持该模块。

Nano/Lite/Full 由受审 Manifest 生成容量和值域常量，不是任意 Cargo Feature 集合。

## 12. 错误、panic 与故障

协议公共路径返回定宽、稳定的错误分类，不以字符串决定行为。产品构建要求 `panic=abort`，但正常
外部输入、容量耗尽、时间到期和 Provider 错误不得依靠 panic 处理。

no-wrap 耗尽、durable high-water 损坏、Provider 违反 completion、冲突事实和无法证明的持久化
历史必须进入显式错误或 Fault。

## 13. `unsafe` 规则

当前两个基础 crate 使用 `#![forbid(unsafe_code)]`。未来只有 MMIO/DMA/ISR、RTOS/HAL、必要 C ABI
和经审计的锁/原子 Port 可以申请例外。每个 `unsafe` 块必须紧邻写出安全前提并具备 Host 模型、
目标编译和实机测试；协议状态机、Wire 和 Persistence 选择逻辑不得用 `unsafe` 绕过所有权合同。

## 14. 验证矩阵

```text
cargo fmt --all --check
cargo clippy --workspace --all-targets -- -D warnings
cargo test --workspace
cargo check --workspace --target thumbv7em-none-eabi
cargo check --workspace --target thumbv7em-none-eabihf
```

后续增加 Miri、fixed-seed fuzz/property、C↔Rust Golden/Negative/Record/状态差分、map/size/stack
资源门禁，以及 ESP32-S3 编译、烧录、六板互通、故障和长稳。

## 15. 实施顺序

| 任务 | 内容 | 放行条件 |
| --- | --- | --- |
| RUST-00 | 总体合同与空行为 Workspace | Host/Cortex-M 编译，零正式协议行为 |
| RUST-01 | Types、Registry、C0/C1 Wire | 共享 Golden/Negative 与 C 互通 |
| RUST-02 | Owner、Coordinator、Adapter Token | 重入、并发、同步早到与资源门禁 |
| RUST-03 | 最小静态通信 | 单帧 TX/RX、Endpoint、四级队列 |
| RUST-04 | Persistence Foundation | Record/Witness/恢复差分与故障矩阵 |
| RUST-05 | Security Session | 认证、加密、Replay、Key Generation |
| RUST-06 | Admission 与 Capability | Bootstrap/JOIN、租约、Profile 协商 |
| RUST-07 | 自动路由 | SoftRoute、RREQ/RREP/RERR、Flow |
| RUST-08 | Reliable/Transfer/Service | Receipt、重试、分片、Operation |
| RUST-09 | Realtime/Group/Cluster | 三个独立可选模块和 Feature OFF |
| RUST-10 | 双实现与硬件闭环 | C↔Rust、MCU、Flash、Bearer、长稳 |

每项均执行分项自审；全部 Host 软件完成后执行至少两轮不同顺序的全体自审，再送外部审计。

## 16. RUST-00 验收条件

1. 分支从已外审通过的 C IMPL-02 基线建立。
2. Workspace 在 Host、`thumbv7em-none-eabi` 和 `thumbv7em-none-eabihf` 编译通过。
3. `ucn-types`、`ucn-wire` 默认 `no_std` 且禁止 `unsafe`。
4. 没有正式 Encoder/Decoder、状态机、Driver 或 Provider 行为。
5. 没有第三方 crate、动态内存或异步 Runtime。
6. 任务表和操作记录明确 C/Rust 共用规范、实现相互独立。
7. 当前没有宣称 ESP32-S3、实机、密码或掉电证明。

满足上述条件只能放行 `RUST-01`，不能宣称 Rust 协议已经可用。

## 17. RUST-01 实施边界与结果

RUST-01 已将 V6S-00-02/03/04 的基础标量、Registry、C0 与 C1 Core Wire 映射为独立 Rust
实现。为了保持 Security Owner 的唯一所有权，本阶段仅公开 `encode/decode_*_o0_h0`：

- C0/C1 的 Common Header、地址宽度、big-endian offset 和基础长度已经实现；
- C0 Bootstrap 与已绑定地址域分开校验；
- C1 Data、Control、Diagnostic 和固定 Fragment/SACK subtype 已做 raw 结构校验；
- O1/O2/H1 不接受调用方直接提供伪 Tag，而在 RUST-05 由唯一 Security Context 产生 sealed frame；
- semantic policy 仍由后续 Operation、Transfer、Admission 和 Security Owner 完成，raw 合法不代表
  业务可执行。

共享 Oracle 位于 `rust/tests/conformance/v6s_wire_core_v1.h`。C1 的 C/Rust Codec 使用同一份 4 个
地址宽度 Golden 与 6 个 Negative 常量；C0 使用冻结 A1 Bootstrap Golden，并由独立 Python Wire
Oracle 逐字段重建。RUST-01 自审报告记录于
`docs/08-实现与验证/版本演进/UCN_V6S_RUST_01_基础类型Registry与C0C1_Wire实施及自审报告.md`。

RUST-01 完成不表示安全、Adapter、Runtime 或实机已经完成，只解除 RUST-02 的顺序阻塞。

## 18. RUST-02 实施边界与结果

RUST-02 已实现 `ucn-owner` 与 `ucn-adapter` 两个独立 `no_std` crate，但尚未建立 Runtime、
Endpoint 或业务发送 API：

- Owner 状态只能经 `&mut self` 修改；任务/ISR 只能向 caller-owned 原子 Mailbox 或受限 Event
  Ingress 发布事实；
- Callback Gate 使用精确 Owner/Operation/Generation/Kind Claim 和不可回绕 Lease，拒绝递归、错门、
  旧 Lease 及并发第二进入者；
- Coordinator 只路由 typed Requirement/Event，保留完整 canonical requirement；摘要不得代替精确相等；
- Adapter TX Token 精确绑定 Adapter、Slot、Token Generation、Link Slot、Link Handle Generation
  和 Link Instance Generation；
- Driver 只实现最小 `TxDriver` SPI，并只能持有 `AdapterEventIngress`；它无法取得
  `&mut AdapterOwner`，因此同步 completion 可以到达但不能递归控制 Owner；
- RX Frame 与 Link/时间戳事实由同一固定槽拥有并一起 claim/retire，不依赖两个 Ring 的顺序配对；
- `NOT_SUBMITTED` 保留同一 Attempt/Token 供上层有界重试；`UNKNOWN` 或矛盾事实进入
  `IN_DOUBT` 并围栏旧 Link；Link 实例重开后未知旧事务可安全退休，迟到精确证明仍可收敛；
- 所有容量由 const generic 在编译期确定，达到上限不驱逐，代际终值不回绕。

RUST-02 的详细代码—测试—边界映射见
`docs/08-实现与验证/版本演进/UCN_V6S_RUST_02_Owner_Coordinator与AdapterToken实施及自审报告.md`。
本阶段完成只解除 RUST-03 的顺序阻塞；没有证明最小静态通信、真实 Driver/ISR 时序、目标 MCU
资源或生产可用性。
