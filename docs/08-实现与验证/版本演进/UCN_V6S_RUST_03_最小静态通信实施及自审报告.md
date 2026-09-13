# UCN V6S RUST-03 最小静态通信实施及自审报告

## 1. 结论

```ini
RUST-03 = DONE / SELF-REVIEW PASS
RUST-04 = ALLOWED TO START
EXTERNAL_REVIEW = DEFERRED UNTIL THE COMPLETE RUST WORKSTREAM
```

本阶段在独立 Rust Workspace 中完成 `ucn-core`。它能在调用方提供的固定内存中，以静态身份、
静态单跳 Path 和可信 O0 网络完成 C1 单帧发送、接收、Endpoint 分派、四级队列与有界停止。

这不是生产放行。安全会话、持久化、动态准入、自动寻路、可靠/分片传输、Realtime、Group、
Cluster、真实 Driver/ISR、ESP32-S3 和物理 Bearer 均不属于本阶段完成范围。

## 2. 权威输入与实现边界

本阶段按以下合同实施：

1. V6S 最小发送接收闭环；
2. 基础通信与自动路由简化设计中的“基础通信”边界；
3. 公共 API/SPI 的 Owner、回调和 `StepBudgetPlan` 约束；
4. RUST-01 的 C1/O0/H0 big-endian Codec；
5. RUST-02 的 Callback Gate 与 Adapter Token 生命周期。

明确保持以下隔离：

- 只支持已绑定地址域的 C1 Data、`BEST_EFFORT + ONE_WAY + O0 + H0`；
- 只使用静态 Binding 与静态单跳 Path，不发现、不学习、不修复 Route；
- O0 只允许显式 `trusted_o0_network=true` 的产品配置；
- Replay 只在 RAM 中维护，重启后延续必须等待 Persistence/Security；
- Endpoint 回调只得到不可变消息视图，不能取得 `&mut CoreNode`；
- Driver 仍只能持有 RUST-02 的受限 Ingress，不能递归进入 Owner。

## 3. 新增实现

### 3.1 Crate 与依赖

新增 `rust/crates/ucn-core`：

```text
ucn-core
├─ ucn-types
├─ ucn-wire
├─ ucn-owner
└─ ucn-adapter
```

依赖树没有第三方 crate。生产源码使用 `#![no_std]` 和 `#![forbid(unsafe_code)]`，不使用
`alloc`、`Vec`、`Box`、`String` 或运行期扩容容器。

### 3.2 静态对象模型

`CoreNode<'a, BINDINGS, ENDPOINTS, PATHS, TX_SLOTS, FRAME_BYTES>` 在类型上冻结所有容量。
对象内部只含固定数组：

- Binding slots：地址、Binding Generation、Replay 高水位和 64 位窗口；
- Endpoint slots：Service ID、Generation 和调用方持有的 Handler 引用；
- Path slots：目标、目标 Binding Generation、精确 Link Handle 和 Path Frame MTU；
- TX slots：Frame 固定数组、状态、Sequence、Deadline、Path 和 Adapter Token；
- 单个 RX workspace：由 Adapter claim 后复制完整 Frame，再解码和交付。

Nano/Lite/Full 的当前 Rust 类型别名与队列容量为：

| Profile | Binding | Endpoint | Path | TX slots | Frame bytes | Q0/Q1/Q2/Q3 |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| Nano | 4 | 8 | 4 | 16 | 256 | 4/4/4/4 |
| Lite | 8 | 16 | 16 | 48 | 256 | 8/8/16/16 |
| Full | 16 | 32 | 32 | 96 | 512 | 16/16/32/32 |

这些是当前受审实现常量，不代替未来 Manifest 生成器和目标 MCU map 文件。

### 3.3 发送路径

`publish()` 的提交顺序冻结为：

```text
Lifecycle/Target/Binding/Path/MTU/Class/Deadline/Queue preflight
                              ↓ 全部成功
                    reserve Origin Sequence
                              ↓
                 encode C1/O0/H0 into TX slot
                              ↓
                    publish slot as QUEUED
```

任何预检失败均不消耗 Sequence、不写部分 TX slot、不驱逐旧请求。每个 Traffic Class 有独立固定
分区，满载只拒绝该 Class。

调度使用冻结十二槽表：

```text
Q0 Q1 Q0 Q2 Q0 Q1 Q0 Q3 Q0 Q1 Q0 Q2
```

游标跨 `step()` 持久化；同 Class 选择 Origin Sequence 最小的请求。Adapter 暂时返回 `NoSpace`
时既不终结请求，也不移动调度游标。`NOT_SUBMITTED` 保留同一 Adapter Token、Frame 和 Sequence；
Deadline 到达半开边界时才本地取消或请求 Driver 取消。

### 3.4 接收与回调路径

```text
Adapter atomic RX item claim
        ↓
C1 exact decode
        ↓
Destination + fixed semantics
        ↓
static Source Binding
        ↓
Endpoint exists
        ↓
volatile 64-bit Replay window
        ↓
CallbackGate + immutable EndpointMessage
        ↓
Adapter RX retire
```

未知 Endpoint 不提前消耗 Replay，因此同一合法 Sequence 在 Endpoint 安装后仍可被接纳。重复或窗口
外旧 Sequence 被拒绝；窗口内未见过的乱序 Sequence 可接受。畸形、未知 Source、错误 Destination
和未知 Service 均不调用业务 Handler。

### 3.5 Lifecycle 与停止

生命周期为：

```text
INITIALIZED ─start→ RUNNING ─stop→ STOPPING ─bounded step→ QUIESCENT
      ▲                                                   │
      └──────────────────────── start ────────────────────┘
                     any indeterminate invariant → FAULT
```

`stop()` 通过 Adapter 与 RX publisher 共用的原子分配门关闭新 publication。成功返回意味着不存在
更早观察到 RX enabled、但尚未完成写入的 publisher。后续 `step(budget)` 每次最多消费预算规定的
工作量，取消/退休所有 TX/RX 后进入 Quiescent。

## 4. 分项自审

### 4.1 静态 Binding、Endpoint 与 Path

- Handle 精确绑定实例、槽位和 Generation；删除重建后旧 Handle 返回 `NotFound`；
- Path 安装验证目标 Binding、Adapter 实例、Link Generation 和双重 MTU；
- 配置对象只允许在 Initialized/Quiescent 修改，运行中修改失败且状态不变；
- 容量耗尽不覆盖已有槽。

结论：通过。

### 4.2 TX 与四级调度

- 12 帧饱和样本逐帧符合冻结表；
- 12,000 次持续补队列得到 Q0/Q1/Q2/Q3=`6000/3000/2000/1000`；
- Adapter 满载时请求留在 Core 队列，容量恢复后继续；
- 队列满、MTU 超限、未知目标均不消耗 Sequence；
- 同步早到 completion、异步 completion 和 `NOT_SUBMITTED` 均有回归；
- Driver `UNKNOWN` 使 Core 进入 Fault，不伪造成功或失败。

结论：通过。

### 4.3 RX、Replay 与 Endpoint 副作用

- 双节点 Loop Driver 完成真实 Encode→Adapter→Decode→Endpoint 链；
- exact duplicate、窗口外旧值拒绝，窗口内乱序一次性接受；
- 不满足语义或静态身份的 Frame 零业务调用；
- RX Frame 与物理元数据由同一 Adapter 槽拥有，Endpoint 看到精确 timestamp；
- 回调门异常被视为内部不变量破坏并进入 Fault。

结论：通过。

### 4.4 Lifecycle、预算和资源

- 停止后立即拒绝 publish/RX publication；
- 每次 `step(1)` 的 `work_done <= 1`，最终检查不偷用额外预算；
- 所有队列清理后才发布 Quiescent，随后可显式 restart；
- `CoreNode` 不需要 Drop，内部没有动态资源；
- Host `size_of`：Nano 5,976 B、Lite 16,920 B、Full 58,264 B。

结论：Host 软件范围通过；目标 MCU RAM/栈/Flash 待硬件阶段。

## 5. 自审中发现并修复的问题

| ID | 问题 | 修复与回归 |
| --- | --- | --- |
| RUST03-S01 | stop 仅写 RX enabled 时，已观察旧值的并发 publisher 仍可能晚到 | Adapter 增加共享原子分配门；stop 只有在取得门后才关闭 RX |
| RUST03-S02 | Adapter 暂时满载若推进调度游标，会改变下一次可服务 Class | `NoSpace` 返回零进展且游标不变；加入 Q1/Q2 同时等待的顺序回归 |
| RUST03-S03 | 停止末尾为确认 RX 空而 claim 一次，曾可能使一次 step 超预算 | 所有实际 retire 均计入 budget，空检查不产生工作量 |
| RUST03-S04 | 单独的 `u16 callback_generation` 会在 65,535 次 RX 后错误耗尽 | 删除调用次数代际；Callback Claim 使用合同版本，调用新鲜性只由 Gate 的 `u32` no-wrap Lease 拥有 |
| RUST03-S05 | Path 安装后 Link 若已重开，旧 `publish()` 可先接纳并消耗 Sequence，调度时才失败 | `publish()` 增加只读 Adapter 快照，在 Sequence 前复核精确 Link Generation 与当前 MTU；补旧 Link 拒绝、换新 Path 后首个 Sequence 仍为 1 的回归 |
| RUST03-S06 | Adapter 同时保留无互斥保证的公开 RX setter，会允许上层绕过 stop 的 publication 证明 | 删除旧 setter；所有 RX 开关和测试统一使用共享原子分配门的 `try_set_rx_enabled()` |

## 6. 验证结果

| 门禁 | 结果 |
| --- | --- |
| `cargo fmt --all --check` | PASS |
| `cargo clippy --workspace --all-targets -- -D warnings` | PASS |
| Host Debug `cargo test --workspace` | 47/47 PASS |
| Host Release `cargo test --workspace --release` | 47/47 PASS |
| Rust 1.85 MSRV `cargo test --workspace` | 47/47 PASS |
| `RUSTDOCFLAGS=-D warnings cargo doc --workspace --no-deps` | PASS |
| `thumbv7em-none-eabi` | PASS |
| `thumbv7em-none-eabihf` | PASS |
| 生产 Rust `std/alloc/dynamic container/unsafe/TODO/panic` 扫描 | 0 |
| Rust 第三方依赖 | 0 |
| C Full fresh CTest | 47/47 PASS |
| `git diff --check` | 无空白错误，仅仓库换行提示 |

Rust 47 项由 Adapter 14、Core 13、Owner 7、Types 3、Wire 10 组成。Core 的 13 项包含 1 项内部
Replay 测试与 12 项集成测试；其中回调长运行回归连续交付 65,537 帧。

## 7. 未完成与下一步

本报告不能证明：

- O1/O2/H1 的认证、加密与持久 Replay；
- 动态 Admission、Capability、Route 或多跳转发；
- Reliable、Fragment、Operation、Realtime、Group、Cluster；
- 真实 Flash/Witness/掉电、生产密码 Provider；
- ESP32-S3 编译、任务栈水位、DMA/ISR、物理 Bearer、性能功耗和长稳。

下一项为 RUST-04：独立实现统一 Persistence Foundation，并以相同 Record image、Witness、恢复
矩阵和 C↔Rust 介质 fixture 做差分。RUST-04 不得把 Host Fake Provider 解释为真实掉电证明。
