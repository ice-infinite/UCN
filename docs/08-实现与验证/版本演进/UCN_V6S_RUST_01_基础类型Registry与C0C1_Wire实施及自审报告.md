# UCN v6 简化版 RUST-01 基础类型、Registry 与 C0/C1 Wire 实施及自审报告

> 状态：`DONE / SELF-REVIEW PASS / EXTERNAL REVIEW DEFERRED`
>
> 分支：`v6-simplified-rust`
>
> 范围：Host 与已安装 Cortex-M 编译目标的软件证据；不包含真实 MCU、物理 Bearer 或密码实现

## 1. 本阶段要解决的问题

RUST-00 只建立了两个没有正式协议行为的 crate。RUST-01 的目标不是先做一个“能 round-trip”的
自定义格式，而是把已经冻结并通过外审的 v6 简化版标量、Registry 和 C0/C1 Wire 原样映射到
Rust，并建立不会被 C/Rust 两套实现共同写错的共享 Oracle。

本阶段完成后的边界是：

1. Rust 可以严格构造、编码和解码 C0/C1 的 `O0/H0` Core Frame；
2. C1 的 C 与 Rust 实现会对同一语义对象产生完全相同的字节；
3. O1/O2/H1 尚未接入时只能显式返回 `Unsupported`，不能接受调用方伪造 Tag；
4. raw Wire 合法不等于 Admission、Operation、Transfer 或 Endpoint Policy 已经接受。

## 2. 实现结构

```text
ucn-types
├─ Wire enums: Contract / Traffic / Delivery / Role / Kind / O / H
├─ scalar wrappers: Address / ID / Sequence / Generation
├─ Protocol Opcode Registry v1
└─ Security Suite Registry

ucn-wire
├─ common.rs   三字节 Common Header 与 big-endian 基元
├─ c0.rs       C0 ABSOLUTE O0/H0
└─ c1.rs       C1 STATELESS O0/H0

rust/tests/conformance/v6s_wire_core_v1.h
├─ 5 组共享 Golden bytes
└─ 6 个共享字段级 Negative
```

`ucn-wire` 只依赖 `ucn-types`。两个生产 crate 都是 `no_std`、`forbid(unsafe_code)`，不依赖
`alloc`、第三方序列化库或 C FFI。

## 3. 强类型与 Registry

所有线上枚举都使用显式值转换，不依赖 Rust 枚举内存布局。地址、Service、Realm、Transaction、
Operation、Sequence、Group 与不同 Owner 的 Generation 使用不同 wrapper；调用方不能把
`RouteGeneration` 直接传成 `PeerSessionGeneration`。

关键边界如下：

| 类型 | 规则 |
| --- | --- |
| `RealmId` | 0、全 1 拒绝 |
| `NodeAddress` | 依地址宽度拒绝 0、全 1 和超宽值 |
| `HopLimit` | 只允许 1～63 |
| `ServiceId` | 只允许 1～0xFFFE |
| Transaction/Sequence/Generation | 最大值可接收；`checked_next(max)` 返回 `Exhausted`，不回绕 |
| `ProtocolOpcode` | 94 个标准值；未登记、Product-private、Experimental 和 Future 默认拒绝 |
| `SuiteId` | 只登记 1、2、3、16；其他值拒绝 |

Product-private Opcode 必须等待签名 Manifest Registry；RUST-01 不提供跳过 Registry 的
“unchecked constructor”。

## 4. C0/C1 Codec

### 4.1 共同规则

- Version 固定为 6；Contract 保留值拒绝；
- 所有多字节值为 big-endian；
- Payload 使用借用切片，不复制到内部动态容器；
- Encoder 先完成全部字段、组合、容量和 raw 地址检查，再首次写输出；
- 安全 Rust 无法合法构造与 `&mut output` 重叠的 Payload，因此别名由借用规则在调用前消除；
- Decoder 从调用方提供的可信 Carrier 精确边界借用 Payload，不保存悬空指针。

### 4.2 C0

C0 基础长度为 `25 + 2W`，即 A0/A1/A2/A3 分别为 27/29/31/33 B。实现校验：

- Delivery 只能 BestEffort 或 Reliable；
- Kind 只能 Control 或 Diagnostic，且 Opcode 域必须匹配；
- 未绑定 Bootstrap 的双 Binding 为 0、Opcode 属于 Identity/Security、Hop Limit 为 1；
- 激活 Binding 后双地址必须都是普通绑定地址；
- Transaction ID 非零，Realm 非零且非全 1。

### 4.3 C1

C1 基础长度为 `9 + 2W`，即 A0/A1/A2/A3 分别为 11/13/15/17 B。实现校验：

- 双地址、Service 与 Origin Sequence 的保留值；
- Control/Diagnostic 从 Payload offset 0 读取已登记的 16-bit code，并禁止互相冒充；
- Transfer 的首个 16-bit subtype 只允许 Fragment `1..0x7FFF` 或精确 SACK `0x8000`；
- Data 不猜测 Opcode，Payload 可以为空。

Interaction 非 OneWay 的 Operation Envelope，以及 Transfer 的 Parent/Setup/bitmap 语义由后续
Owner 在 raw decode 后、业务副作用前验证。

## 5. 共享 C/Rust Oracle

共享 fixture 不是由 Codec 生成。独立 Python Oracle 按冻结字段重新构造 10 个全局 Golden，并
额外核对共享文件中的 5 组 C0/C1 bytes：

- C0 A1 Bootstrap；
- C1 A0/A1/A2/A3 Data。

C 侧 `test_wire_c1.c` 与 Rust 侧 `conformance.rs` 都直接消费同一文件。共享 Negative 覆盖错误
Version、保留 Contract、错误 Contract、保留 Delivery、保留 Origin Security 和零 Hop Limit。
检查器还对 5 个共享向量逐一执行单字节变异，保证固定字节变化会被发现。

## 6. 分项自审与整改

### 6.1 类型/Registry 自审

- 发现 `PersistenceRecordGeneration` 初稿误用了 `C0TransactionId` 类型别名；这会允许跨 Owner
  错绑，已改为独立 64-bit 强类型。
- 发现动态 Group ID 的合法终值是 `0xFFFF_FFFE`，不能由通用 u32 后继进入保留全 1；已增加
  专用 `checked_next()`。
- Opcode Registry 全部 94 项执行 raw→enum→raw 检查，Suite 未登记值独立拒绝。

### 6.2 Codec 自审

- 发现 C0 Encoder 初稿在开始写输出后仍重新执行理论可失败的地址转换。虽然前置检查已保证它
  不会失败，仍不符合可审计的“首次写后无错误分支”；现已在写入前缓存双 raw 地址。
- 发现未绑定 C0 初稿只绑定了双零 Binding、Hop Limit 与 Opcode 域，没有精确约束保留地址；
  已补成 `Source=全 0 + Destination=全 1`，并以编码与 raw 解码双向反例钉住。
- C0/C1 所有 offset 由基础长度公式交叉核对；A1 使用冻结 Golden，不使用 round-trip 自证字节序。
- O1/O2/H1 与非法 Transfer subtype 均在写输出前拒绝，并检查完整输出哨兵不变。

## 7. 验证矩阵

| 门禁 | 结果 |
| --- | --- |
| `cargo fmt --all --check` | PASS |
| Clippy workspace/all-targets `-D warnings` | PASS |
| Host Debug tests | 13/13 PASS（3 types + 10 wire） |
| Host Release tests | 13/13 PASS |
| 声明 MSRV Rust 1.85.0 Host check/tests | 13/13 PASS |
| rustdoc `-D warnings` | PASS |
| `thumbv7em-none-eabi` workspace check | PASS |
| `thumbv7em-none-eabihf` workspace check | PASS |
| C/Rust shared Wire Oracle | 10 global + 5 shared + 6 shared Negative PASS |
| 固定种子 C1 round-trip | 4096/4096 PASS |
| C `ucn_wire_c1_tests` | PASS |
| Fresh C Full + Persistence CTest | 52/52 PASS |
| Cargo dependency tree | 仅两个本地 crate |
| 生产 Rust `std/alloc/unsafe` 扫描 | 0 |

Rust 分支因接入共享 C/Rust fixture 而更新了 C conformance test、Wire Oracle 和关联台账，故重新
生成当前 118 项 IMPL-02 候选清单并通过 118/118 变异门禁。它只证明当前分支文件自洽；C 版既有
外审 GO 仍精确绑定 `ffe39f4/6b38fa9`，不得把新清单误写成一次新的 C 外审签字。

## 8. 未完成和下一步

RUST-01 不证明以下能力：

- O1/O2/H1/H2/H3 密码保护与 Replay；
- Adapter Token、同步早到、回调重入和 Runtime；
- 动态 Admission、Route、Reliable、Transfer 完整状态机；
- Persistence、真实 Flash、掉电和 Witness；
- ESP32-S3、六板网络、物理 Bearer、性能或功耗。

下一项 RUST-02 只实现 Owner、Coordinator 与 Adapter Token，不得借机接入 Security、Route 或
业务 Runtime。
