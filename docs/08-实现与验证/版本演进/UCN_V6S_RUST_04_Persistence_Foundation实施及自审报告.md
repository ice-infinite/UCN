# UCN V6S RUST-04 Persistence Foundation 实施及自审报告

## 1. 结论

```ini
RUST-04 = DONE / SELF-REVIEW PASS
RUST-05 = ALLOWED TO START
EXTERNAL_REVIEW = DEFERRED UNTIL THE COMPLETE RUST WORKSTREAM
```

本阶段完成 Rust 独立 Persistence Foundation：相同 Durable Manifest、Record image、Commit
Marker、双槽、独立 Witness、恢复选择、异步 Provider continuation 与 durable proof 合同均由
`ucn-persistence` 自己实现，没有链接 C archive，也没有通过 FFI 转发状态机。

结论只适用于 Host 软件模型和两个 Cortex-M `no_std` 编译目标。内存 Fake Provider 不能证明真实
Flash 原子写、独立 Witness、掉电时序、擦写寿命、ESP32-S3 任务栈或生产硬件 Provider。

## 2. 权威输入与隔离边界

实现直接映射以下合同：

1. V6S-00-07 Persistence Foundation 共同合同；
2. 持久化与掉电恢复简化设计；
3. Owner/Coordinator、Handle/Generation/Fence 和公共 API/SPI 边界；
4. C99 IMPL-02 已签 Host 软件语义与 X01-B 前驱证明整改；
5. Rust 总体设计的独立实现、`no_std`、固定内存和共享 Oracle 要求。

本阶段刻意没有接入：

- Identity、Security、Operation、Group、Time Authority 或 Cluster 的业务 Record Body；
- RUST-03 CoreNode 的启动门或业务 Coordinator；
- 真实 Flash、文件系统、NVS、硬件 monotonic witness；
- 动态内存、异步 Runtime、线程池和第三方序列化/Hash crate。

因此 Persistence Proof 只表示“指定 Record 已经过介质 reload，并由精确 Witness 证明”，不表示业务
Authority、Security Session、Address Lease 或 Cluster Epoch 已获准生效。

## 3. 实现结构

### 3.1 Crate 与依赖

```text
ucn-persistence
├─ codec.rs       Record/Marker/Manifest/CRC32C/BLAKE2s-128
├─ provider.rs    Provider SPI、Completion、共享回调 Gate
└─ foundation.rs  Domain、请求、双槽提交、恢复与 Proof
```

生产依赖只有工作区 `ucn-types` 和 `ucn-owner`；没有 crates.io 第三方依赖。源码声明
`#![no_std]` 与 `#![forbid(unsafe_code)]`。

### 3.2 固定存储

`PersistenceOwner<'a, DOMAINS, BODY, SLOT>` 把容量放进类型：

- `DOMAINS` 个 Domain state 和每域唯一 Pending；
- 两个完整原始槽缓存；
- 一个待写槽、一个 readback 槽；
- 两个恢复正文 scratch；
- 一个固定 216 B Codec Workspace；
- 一个调用方持有的 12 B 原子 Provider Gate。

Host `size_of` 门禁结果：

| Profile | Domain | Body | Slot | PersistenceOwner |
| --- | ---: | ---: | ---: | ---: |
| Nano | 2 | 256 B | 368 B | 3,968 B |
| Lite | 4 | 512 B | 624 B | 9,072 B |
| Full | 8 | 1,024 B | 1,136 B | 25,424 B |

这些对象应放在调用方静态存储或专用 arena，不要求在任务栈创建。表中数值是当前 Host ABI 布局，目标
MCU 仍须用最终编译器、link map 和任务栈水位重新测量。

## 4. Durable Codec

### 4.1 Record 与 Marker

固定槽布局为：

```text
[ 96 B Record Envelope ][ Body ][ erased tail ][ 16 B Commit Marker ]
```

所有多字节字段使用 big-endian。Envelope 精确绑定：

- Magic、Envelope Version、Header Bytes；
- Domain Kind/ID、Schema ID/Version；
- 完整 Durable Manifest Digest；
- Record Generation、Foundation Transaction ID、Operation Kind；
- Body Bytes、Body CRC32C、Header CRC32C、Body BLAKE2s-128 Digest；
- 八个保留字节必须为零。

Marker 绑定 Magic、Record Generation 和 CRC32C。完全等于 Provider 擦除值才是 Erased；任何非擦除
但不合法的 Marker 都是 Torn，不能当作未提交记录静默忽略。

Encoder 先在 96 B 小型局部 Envelope 完成全部可能失败的验证与摘要计算，再写调用方完整槽；Decoder
先验证完整 Marker、Envelope、CRC、Digest、Manifest 和 erased tail，成功后才写 Meta/Body 输出。

### 4.2 C/Rust 共享 Oracle

`rust/tests/conformance/v6s_persistence_v1.h` 是语言无关 fixture，当前冻结：

- BLAKE2s-128 empty/`abc`；
- Nano/Lite/Full 单域 Durable Manifest Digest；
- ProductConfig generation 1 / transaction 5 的 Body Digest；
- `manifest_digest=00..0F`、Body=`01 02 03` 的完整 115 B Record+Marker image。

C 与 Rust 分别编码并逐字节比较共享 fixture；测试运行期间不允许一套实现调用另一套实现生成期望值。

## 5. Provider 与 exact continuation

Provider SPI 包含六个阶段：

```text
LOAD_SLOT
WRITE_INACTIVE
READBACK
PUBLISH_MARKER
LOAD_WITNESS
ADVANCE_WITNESS
```

每个 `begin_*()` 只能返回同步 Completed、Pending 或 Failed。Pending 后，Owner 只允许以同一个全局
不回绕 Token 和 Phase 调用 `poll()`。完成事实必须精确匹配：

```text
io_token + phase + slot_index + exact_bytes + blob_state + result
```

任何字段错配都使当前 Domain 失败关闭。Provider geometry 在 init 前完整校验并冻结；每次 `step()`
准备进入 I/O 前再次逐字段匹配，对齐、槽容量、Marker 原子宽度或擦除值漂移均零 I/O 拒绝。

调用方持有的 `ProviderGate` 使用原子 compare-exchange 阻断跨 Owner 同步重入/并发 callback，并在门内
推进 64 位 Token。到达 `u64::MAX` 后返回 Exhausted，不回绕到零。Provider callback panic 不属于
可恢复输入合同；产品 `panic=abort`，Provider 的正常失败必须通过 `IoStart/Completion` 返回。

## 6. 提交、恢复与 Proof

### 6.1 提交顺序

```text
完整业务/容量/expected-state/transaction/deadline preflight
                       ↓
编码未发布的 inactive slot image
                       ↓
WRITE_INACTIVE → READBACK → byte-exact compare
                       ↓
原子 PUBLISH_MARKER
                       ↓
CAS ADVANCE_WITNESS(old, old+1)
                       ↓
reload Witness + slot0 + slot1
                       ↓
恢复选择再次验证精确新状态
                       ↓
发布 immutable PersistenceProof
```

Marker 前 Deadline 可明确终止且不产生 Proof。Marker 后任何无法证明的失败返回 InDoubt 并 Fault，
不得用 RAM 中的待提交正文继续业务。相同 Transaction/Generation/Body/Operation 的 exact replay 在
已恢复 Ready 状态直接返回等价 Proof，不执行 Provider I/O；同 Transaction 不同正文返回 Replay。

### 6.2 恢复选择

恢复只接受完整 raw slot 与明确 Present 的独立 Witness。Factory Empty 也必须由产品 provisioning
事实明确证明；缺失或损坏 Witness 不能猜测为空。

规则如下：

- Witness=0：两槽均空则发布空 Ready；唯一 generation 1 可补推进到 1；
- Witness=n：必须存在 generation n 的完整槽；
- 若同时存在 n+1，只有恢复流程允许补推进，且 n 前驱必须仍完整、同 Domain/Schema/Manifest，
  generation 精确相邻，后继 Transaction ID 严格大于前驱；
- 两个有效槽同代、跳代、Transaction 相等/回退，或者存在高于 Witness+1 的记录时 Fault；
- 已经推进 Witness 的正常 reload 不允许再次发现未证明 successor；
- required Domain Fault 使整个 Owner Fault；optional Domain Fault 不阻断其他 required Domain 恢复。

### 6.3 请求与输出

每域同时至多一个 Pending。请求绑定 Runtime、业务 Owner、易失 Domain Generation、Domain、Schema、
Expected Record/Body Digest、Transaction、Operation、Deadline 和 continuation。Handle 绑定 Runtime、
Persistence Owner、Domain slot 和不回绕代际；取消只允许发生在首次 Provider I/O 前，成功/失败请求都
必须显式退休才能释放槽。

正文只可在 Domain Ready 时复制；恢复前、恢复中和 Fault 后返回 State，调用方输出不写回。Proof
在 reload 之前不可取得，且只包含耐久事实与原始调用方 continuation。

## 7. 分项自审

### 7.1 Codec 与字节合同

- C/Rust 完整 115 B Record image 逐字节一致；
- Magic、版本、长度、Domain、Schema、reserved、Manifest、Generation、Transaction、Operation、
  Suite、Body length、两个 CRC、Digest、正文、erased tail 和 Marker 均有单点破坏；
- 错误 Decode 保持 Meta 与 Body 哨兵不变；
- generation/transaction 0 与全一、非法容量和错误 Manifest 失败关闭。

结论：通过。

### 7.2 Provider/异步状态机

- 六个阶段分别可同步完成或只 Pending 一次后完成；
- Token、Phase、字节数、Blob State、Slot 五类错误完成事实全部 Fault；
- 错 Token/Phase 不能推动 continuation；
- Provider geometry 运行期漂移在任何 I/O 前拒绝；
- 共享 Gate 重入拒绝且 Token 到顶不回绕。

结论：Host 软件范围通过。

### 7.3 提交、掉电与恢复

- write、readback、marker、witness、reload 五类边界分别注入失败并以新 Owner 重启；
- Marker 前失败恢复旧代/空代；Marker 后 Witness 未推进时由完整前驱证明补推进；Witness 已推进但
  reload 失败时，重启精确选择新代；
- 缺前驱、相等/回退 Transaction、同代、跳代、坏最新槽和 torn Marker 均不发布正文；
- Factory `0→1` 与合法相邻 `1→2` 可以恢复；
- readback 不一致、Deadline、非法请求和取消不产生 Proof；
- exact replay 零 Provider I/O。

结论：Host Fake Provider 故障模型通过；不等于真实 Flash 掉电证明。

### 7.4 多 Domain 与资源

- 两个 Domain 可拥有独立双槽、Witness、Handle、正文和 Proof；
- optional Domain 损坏不会发布其正文，也不会阻断 required Domain 恢复；
- 固定 Profile 对象尺寸均低于本阶段 Host ceiling；
- 生产 crate 没有堆分配、`std`、`unsafe`、动态容器或第三方依赖。

结论：通过。

## 8. 自审中发现并修复的问题

| ID | 问题 | 修复与回归 |
| --- | --- | --- |
| RUST04-S01 | 首版恢复若直接信任 Witness advance 的同步完成，尚未重新读取介质就可能继续选择 | 补推进后强制 reload Witness 与两个原始槽；只有重读事实可发布正文 |
| RUST04-S02 | Pending read 的输出 scratch 若不在每次 poll 前清理，Provider 部分填充可能继承旧字节 | 每次 begin/poll 前按擦除态重置唯一输出，并要求 completion 精确字节数 |
| RUST04-S03 | Exact replay 若先计算 next Generation，到达终值时会错误拒绝已提交事实的幂等重放 | 先分类 exact replay；只有新提交才计算 checked successor |
| RUST04-S04 | Witness 非零的 `n+1` 补推进若缺少 generation n 前驱，无法证明 Transaction 严格递增 | 同时要求完整相邻前驱/后继和严格递增 Transaction；Factory 仅例外允许 `0→1` |
| RUST04-S05 | Provider 几何最初只在 init 完整校验，step 前仅比较 erased value | 保存完整 geometry，并在每次 step 的首次 I/O 前逐字段复核；漂移零 I/O 拒绝 |
| RUST04-S06 | 仅共享摘要不能发现两套实现对 Header offset 或 Marker 位置同时理解不同 | 新增独立固定的完整 115 B Record+Marker fixture，C/Rust 分别编码逐字节比较 |
| RUST04-S07 | C 测试已消费 Rust 目录共享 fixture，但 IMPL-02 候选生成器原先没有覆盖这些依赖文件 | 将全部 `rust/tests/conformance/*.h` 纳入精确 SHA256 与逐项变异清单，Oracle 改一字节即使候选门禁失效 |

## 9. 验证结果

| 门禁 | 结果 |
| --- | --- |
| `cargo fmt --all --check` | PASS |
| `cargo clippy --workspace --all-targets -- -D warnings` | PASS |
| Host Debug `cargo test --workspace` | 67/67 PASS |
| Host Release `cargo test --workspace --release` | 67/67 PASS |
| Rust 1.85 MSRV | 67/67 PASS |
| `RUSTDOCFLAGS=-D warnings cargo doc --workspace --no-deps` | PASS |
| `thumbv7em-none-eabi` / `eabihf` | PASS |
| C Persistence Codec 共享完整槽 Oracle | PASS |
| C GCC Full/Lite/Nano | 各 51/51 协议与构建门禁 PASS；最终 Full 在候选清单重生成后 52/52 PASS |
| C MSVC 19.29 `/W4 /WX` | 当前 Persistence 相关目标重编译，定向 4/4 PASS |
| `git diff --check` | PASS |

Rust 67 项由 Adapter 14、Core 13、Owner 7、Persistence 20、Types 3、Wire 10 组成。Persistence
20 项包括 4 个内部单元测试、2 个 C/Rust conformance、13 个 Foundation/故障测试和 1 个 Profile
资源门禁。

## 10. 未完成与下一步

本报告不能证明：

- 真实 Flash 的 Marker 原子性、独立 Witness、防回退硬件或掉电恢复；
- 磨损均衡、坏块、介质寿命、掉电能量窗口；
- ESP32-S3 编译、实际 RAM/Flash、任务栈水位和长期运行；
- Security Session、持久 Replay/Key Generation 与生产密码 Provider；
- Admission、Capability、动态 Route、Reliable、Transfer、Service、Realtime、Group、Cluster。

下一项为 RUST-05 Security Session。它必须把 O1/O2/H1/H2/H3、E2E/Hop Key Selector、Replay
high-water 与密码 Provider 建立为独立模块，并通过 Persistence typed dependency 消费 proof，不能
直接调用或篡改 Persistence Owner。RUST-05 获得的只是实施顺序许可，不继承 RUST-04 外审或硬件结论。
