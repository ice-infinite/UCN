# UCN V6S RUST-05 Security Session 实施及自审报告

## 1. 结论

```ini
RUST-05 = DONE / SELF-REVIEW PASS
RUST-06 = ALLOWED TO START
EXTERNAL_REVIEW = DEFERRED UNTIL THE COMPLETE RUST WORKSTREAM
```

本阶段完成 Rust 独立 Security Session：静态 Peer 握手证明、Session/Key Generation、持久化后
发布、O1/O2/H1/H2 Packet 保护、精确 ACL、固定 Replay Window 和两阶段 Replay mutation 均由
`ucn-security` 自己实现。它不调用 C 安全库，也不通过 FFI 复用 C 状态机。

结论只适用于 Host 软件模型、独立密码 Wire Oracle 与两个 Cortex-M `no_std` 编译目标。测试用
Provider 不是生产密码实现；当前没有证明真实 Flash 防回退、安全元件、随机数质量、侧信道、
ESP32-S3 任务栈、物理链路或长期运行。

## 2. 权威输入与任务边界

实现逐项映射：

1. V6S Core Wire 3.0 的 C0～C5、O0～O2、H0～H3 布局、AAD、Nonce 与 Tag 合同；
2. Identity、Security、Admission 的 Owner 分离、握手与 Replay 两阶段提交合同；
3. Persistence Foundation 的 typed requirement、reload-before-proof 和 Domain 隔离；
4. 全局 Owner/Coordinator、固定容量、Feature OFF 与不回绕 Generation 规则；
5. Rust 独立实现的 `no_std`、禁止 `unsafe`、无第三方依赖与共享 Oracle 规则。

RUST-05 只负责已绑定静态 Peer 的新鲜 Session。它不负责陌生设备的 Bootstrap/JOIN、动态地址
分配、Capability 协商、Route/Flow、Reliable receipt、Group/Cluster 或 Realtime。

## 3. crate 与依赖边界

```text
ucn-types ─────┐
ucn-wire ──────┼──> ucn-security
ucn-owner ─────┤        │
ucn-persistence┘        └── typed requirement/proof only
```

`ucn-security` 不依赖 `ucn-core`、Adapter、Runtime、Route 或 Admission。它从 Persistence 读取
DTO 和 proof 类型，但没有 `PersistenceOwner` 实例、Provider 或 I/O 调用路径。Coordinator 负责：

```text
Security.prepare_static_session()
    -> SessionDurabilityRequirement
    -> Coordinator
    -> Persistence.submit()/step()/proof_get()
    -> Coordinator
    -> Security.activate(exact handle, exact proof)
```

密码 Provider 只持有产品密钥和算法实现；协议状态只保存 Suite、Key ID、Key Generation 与
canonical fingerprint，不复制密钥字节。

## 4. Session 状态机与持久化

```text
EMPTY
  │ verify complete HandshakeCandidate + proof
  ▼
AWAITING_DURABILITY
  │ bind exact PersistenceHandle
  │ reload proof matches Domain/Owner/Record/TxID/Body/continuation/deadline
  ▼
ACTIVE ── expiry/fact mismatch/revoke ──> FENCED ──> EMPTY with higher slot generation
```

`HandshakeCandidate` 精确包含：

- 本地和对端 Address、Binding Generation、Principal；
- Link Instance Generation、Peer Session Generation、Policy Generation 与绝对到期时间；
- Origin 等级、Hop Profile、双向 Origin/Hop Key selector；
- Origin/Hop Context fingerprint 与完整握手 transcript digest。

`CryptoProvider::verify_session_proof()` 的合同要求验证上述每个字段，不能只看 Principal 或单一
transcript 摘要。正式回归把候选到期时间改 1 后复用同一 proof，必须返回 `Security` 且不占槽。

Security Session Record 固定 192 B。前 180 B 保存完整活动候选，180～187 B 保存双向 Hop Key
Generation 高水位，188～191 B 保留为零。H1 暂时关闭为 H0 时，高水位不回退；以后重新启用 H1
必须使用严格更高代际。这避免同 Key/Context/Hop Sequence 域在 Profile 往返后发生 ABA。

Session 发布还要求当前本地/对端 Binding、Link、Policy 和可信时间全部与候选相等。更高代新
Session 发布时，旧同 Principal 活动 Session 先转入 Fenced 并清理其易失 Replay 状态。

## 5. 发送保护与字段所有权

公开 `protect_packet()` 的处理顺序固定为：

```text
validate handle/current facts/contract/profile/context/ACL
validate output and caller workspace capacity
validate payload opcode/service and exact canonical identity
preview Origin/C0/Hop counters
enter shared Crypto callback gate
burn exact counters (irreversible)
invoke one exact Key selector and one Profile only
build private sealed packet
leave callback gate
copy to caller output once
```

纯结构、Binding、Profile、ACL 或容量错误发生在 burn 前；一旦 Provider 能观察 Key/Nonce，已经
burn 的值永不复用，即使 Provider 失败或最终没有发送。Caller 输出只在全部密码运算和 Gate leave
成功后一次复制，失败保持逐字节不变。

Origin 字段所有权没有被 Security 泛化夺走：

| Contract | 唯一 Origin 字段 Owner | Security 行为 |
| --- | --- | --- |
| C0 | C0 Transaction Owner | 核对并 burn Transaction ID |
| C1 | Core Message Sequence Owner | 核对并 burn Origin Sequence |
| C2 | Flow/Direct Context Owner | 核对 Flow fingerprint、Sequence、Direct fingerprint |
| C3 | 无 Origin 序号 | 只处理 H1 Hop Sequence |
| C4 | 后续 RUST-07 Flow Owner | 当前明确 `Unsupported` |
| C5 | 后续 RUST-09 Group Sender Owner | 当前明确 `Unsupported` |

H3 密码原语保留在 Provider trait，但 Peer Session 候选拒绝 H3；H3 只能由后续 C5 Group/Tree
Security Context 发布。H2 只允许 C2+O1/O2，复用 Origin Tag 绑定 Direct Context，不追加第二个
Hop trailer。

## 6. 接收、ACL 与 Replay

`open_packet()` 先从当前 Session 取得唯一 Profile/Key，再验证 Hop Tag、Origin Tag 或 AEAD；它
不会按长度依次尝试 H0/H1/H2，也不会尝试 previous/current 两把 Key。认证后的明文只复制到私有
workspace，随后执行：

1. canonical Payload kind、Protocol Opcode 和 C1 Service 检查；
2. 精确 `{Principal,Binding,Context,Service,Opcode,Direction}` ACL；
3. Origin/Hop Replay 只读分类；
4. Fresh 输入 reserve 独占 mutation；已提交重复只形成只读 evidence；
5. Coordinator 预留完业务资源后调用 `replay_commit()`，否则 `replay_abort()`。

`ReplayEvidence` 精确携带 Peer Session Generation、Key Generation、Context fingerprint、
canonical AAD digest 和认证后明文 Payload digest。它只供 Reliable Owner 查询 retained receipt；
Security 不把 bitmap 命中直接称为业务 exact duplicate，也不为重复帧再次暴露明文。

Origin 与 Hop 同时存在时，commit 先对两个 Window 和 Session 做完整预检；只有全部可提交才顺序
执行不可失败的两个固定写入。Deadline 使用半开区间，`now == deadline` 拒绝并释放本组合。

遗失或被上层放弃的 reservation 不靠错误输入触发清理。`maintain_replay_reservations()` 使用跨
Session、Origin/Hop Window 的持久游标，每个预算单位只检查一个固定槽；只在自己的半开 Deadline
到期时释放，不刷新或驱逐其他合法 reservation。

## 7. Wire 与独立 Oracle

RUST-05 对以下四条冻结受保护 Golden 逐字节匹配：

| Profile | 完整 Core Packet |
| --- | --- |
| C2 O1/H2 | `620041123400000001010203048D2206580D6E913E15F14DE32AC68EF9` |
| C3 H1 | `63800211112222AABBCC00000001B61FCD2ED841CF792777F4F1` |
| C0 O2/H0 AES-128-GCM | `602581010203041234567800000001000000020102030405060708001358B2377998A3E0C198106C043A0DBCC6BE150D6C` |
| C2 O2/H2 ChaCha20-Poly1305 | `62008112340000000181052B38E079A5ACC9BDDBE6A1E43903DC3541C2` |

Python Wire Oracle 独立重算 HMAC、AES-GCM、ChaCha20-Poly1305、AAD、Nonce 与完整 Packet，不能由
Rust 测试 helper 反向生成期望值。另有 C1/O1 合法静态直连正向测试，以及错误 Binding、Context、
Tag、Key、Profile、ACL、Deadline、Replay、输出哨兵和代际轮换负向矩阵。

## 8. 固定资源

| 类型 | Host `size_of` | 固定容量 |
| --- | ---: | --- |
| `NanoSecurityOwner` | 2,368 B | 2 Session，每 Session 2 Replay reservation/Window |
| `LiteSecurityOwner` | 11,120 B | 8 Session，每 Session 4 Replay reservation/Window |
| `FullSecurityOwner` | 32,176 B | 16 Session，每 Session 8 Replay reservation/Window |
| `SecurityPacketWorkspace<256>` | 984 B | 3 个固定 256 B 数组 + Codec scratch |
| `SecurityPacketWorkspace<512>` | 1,752 B | 3 个固定 512 B 数组 + Codec scratch |

配置要求 Session Domain 数小于 Session 槽数，为同一 Peer 的新代会话保留一个切换槽。全部容量
在编译期确定，满载返回 `NoSpace`，不驱逐已认证 Session 或 Replay reservation。

Full Security Owner 已接近当前 32 KiB 自审上限；RUST-06 之前不扩大其槽数，后续需要结合完整
Runtime RAM 与 ESP32-S3 实测决定是否压缩 Record cache 或拆分 Replay 存储。Host 布局不能当作目标
ABI、任务栈或 Flash 证明。

## 9. 分项与三轮全体自审

| 编号 | 自审发现 | 整改 |
| --- | --- | --- |
| RUST05-S01 | Provider 合同可能只验证部分握手字段 | 明确要求完整候选逐字段绑定，并用单字段改变复用 proof 的反例钉住 |
| RUST05-S02 | C2 只检查 Context ID 非零，未精确核对 Flow/Sequence/Direct identity | 在 counter burn 前逐字段比较 canonical identity 与 Session/Packet 事实 |
| RUST05-S03 | Fresh Replay evidence 把内部 slot generation 误当 Peer Session Generation | reservation 分开保存两类代际，非相等 `slot=7/session=2` 回归证明 |
| RUST05-S04 | H1→H0→H1 可因 optional selector 清空而复用旧 Hop Key Generation | Record 保留双向高水位；禁用不回退，重新启用严格递增 |
| RUST05-S05 | 遗失 Replay handle 后到期槽没有独立清理入口 | 增加持久游标、显式预算和 deadline-1/deadline 回归 |
| RUST05-S06 | C1 只有拒绝测试，没有合法 O1 静态发送正向证据 | 补正确 Binding/Service/Sequence 的 sealed packet 回归 |

三轮审查顺序分别为：合同到代码、状态/代际到故障路径、公开 API/资源到跨模块边界。最终扫描确认
生产 crate 无 `std/alloc/Vec/Box/String/unsafe`、无动态容器、无 `TODO/FIXME`、无 Core/Adapter/
Coordinator 直连，也没有第三方正常依赖。

## 10. 最终验证结果

| 门禁 | 结果 |
| --- | --- |
| `cargo fmt --all --check` | PASS |
| `cargo clippy --workspace --all-targets -- -D warnings` | PASS |
| Host Debug `cargo test --workspace` | 80/80 PASS |
| Host Release `cargo test --workspace --release` | 80/80 PASS |
| Rust 1.85 MSRV | 80/80 PASS |
| `RUSTDOCFLAGS=-D warnings cargo doc --workspace --no-deps` | PASS |
| `thumbv7em-none-eabi` / `thumbv7em-none-eabihf` | PASS |
| 独立 Wire Oracle | 10/10 PASS |
| C GCC Full fresh | 47/47 PASS |
| 文档门禁 / V6S Freeze | PASS |
| `git diff --check` | PASS，仅既有 CRLF 提示 |

80 项由 Adapter 14、Core 13、Owner 7、Persistence 20、Security 13、Types 3、Wire 10 组成。
Security 13 项包括 Replay 单元测试 4、Profile 资源测试 1、Session/Persistence/Wire 集成测试 8。

## 11. 未完成与下一步

本报告不能证明：

- 生产 HMAC、AES-GCM、ChaCha20-Poly1305 实现、密钥安全存储、TRNG 或侧信道安全；
- 真实 Flash/安全元件中的 Session/Key 高水位和物理 anti-rollback；
- C4 Routed Origin 的 Flow Context、C5/H3 Group Security；
- Dynamic Admission 的 Cookie、Bootstrap、JOIN、Lease、Address Binding 和 Capability；
- RUST-03 Core 与 Security 的最终 Runtime Coordinator 装配；
- ESP32-S3 编译、真实 RAM/Flash/任务栈、物理 Bearer、性能、功耗或长稳。

下一项为 RUST-06 Admission 与 Capability。它只能消费现有 Security 握手/证明能力，不能在
Admission 内复制 Session、Replay、密码 Provider 或 Persistence I/O；动态 Binding 必须
persist-before-promise，且静态已绑定节点的普通 Session 重建不能依赖 Admission。
