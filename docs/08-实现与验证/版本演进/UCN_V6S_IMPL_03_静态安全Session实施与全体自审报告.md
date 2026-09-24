# UCN V6 简化版 IMPL-03 静态安全 Session 实施与全体自审报告

> 日期：2026-09-20  
> 状态：`IMPLEMENTED / SELF-REVIEW PASS / EXTERNAL REVIEW HOLD`  
> 证据范围：C 简化版私有 Host 软件实现；不代表生产密码、MCU、Flash 掉电或产品发布通过。

## 1. 本阶段解决什么问题

IMPL-01 已能在静态 Path 上发送 C1 明文单帧，IMPL-02 已提供所有 durable consumer 共用的
Persistence Foundation。但二者之间仍缺少一个安全所有者来回答四个问题：

1. 当前 Peer 身份、地址绑定、Link、Key 和策略是否仍属于同一个有效 Session；
2. 这个 Session 是否已经完成 persist-before-use，而不是只在 RAM 中自称有效；
3. 某个 Endpoint/Service/Opcode/方向是否被精确授权；
4. 帧在交给业务前是否已认证、可选解密并通过 Replay 门禁。

本阶段新增独立 Security Owner 来承担上述职责。它不把密码算法写死在协议内，而是固定
Provider 输入、状态机、AAD、Nonce、Replay 和失败副作用；产品按硬件与认证要求提供真正的
HMAC/AEAD 实现。

## 2. 实施范围与明确后置项

本阶段实现：

- 静态 Peer Session 和 Key Selector；
- C1 `O1 Auth-only` 与 `O2 Confidential`；
- C1 `H0` 与逐跳 `H1`；
- exact ACL、Origin/Hop Replay、Origin/Hop Sequence；
- Security Record、Persistence Requirement 和 reload Proof 消费；
- Nano/Lite/Full 固定容量、共享 callback Gate 和 caller-owned Workspace。

本阶段不实现：

- Dynamic Admission、JOIN/REAUTH Wire FSM 与 Capability 协商；
- C2/H2 Direct Flow、C5/H3 Group 或其他 Contract；
- 生产 HMAC-SHA-256、AES-GCM、ChaCha20-Poly1305 与密钥安全存储；
- 公共用户 API 或 `ucn_node` Runtime 接线；
- 真实 Flash/Witness 掉电、MCU 栈水位、硬件密码外设和实机性能。

因此测试中的密码 Provider 只是可判定的状态机夹具，不能作为生产密码证据。

## 3. 物理模块与依赖

```text
tests / future Runtime Coordinator
              |
              v
     ucn_v6s_security_session        private, EXCLUDE_FROM_ALL
              |
              +----> ucn_common      checked arithmetic / lock / gate types

Security Requirement/Event
              |
              v
     Runtime Coordinator             future composition owner
              |
              v
     Persistence Foundation          no direct Security link dependency
```

Security target 不链接 Persistence，不 include Persistence Owner 私有结构，也不持有其指针。
它只输出 immutable Requirement View，并消费由 Coordinator 路由的 Durability Proof 投影。

## 4. Session 状态机

```text
EMPTY
  |
  | validate candidate + verify Provider proof + reserve fixed slot
  v
AWAITING_DURABILITY
  |
  | bind exact Persistence Handle/digest
  | consume exact reload Proof + recheck current facts
  v
ACTIVE --------------------------+
  |                               |
  | binding/link/policy/key       | higher valid Session becomes ACTIVE
  | expires or explicit fence     | old Session is fenced first
  v                               |
FENCED <-------------------------+
  |
  | no pending TX/Replay/Provider callback
  v
EMPTY
```

只有 `ACTIVE` 可以授权、预留发送序列或接收 Replay。`FENCED` 不能 Reset 回 ACTIVE；只有资源
全部退休后槽位才回到 `EMPTY`，且槽代际递增，旧 Handle 不能复活。

## 5. Persist-before-use 合同

### 5.1 记录

Security Record 固定 192 B、big-endian，包含：

- Record magic/schema/长度和 Realm；
- Local/Peer Principal、Address 与 Binding Generation；
- Link/Session/Policy Generation 与绝对过期时间；
- Origin TX/RX、Hop TX/RX Suite/Key ID/Key Generation；
- Origin/Hop Context Fingerprint；
- Transcript Digest、Origin Level、Hop Profile 与 Address Width；
- 全部保留字节必须为零。

Decode 使用 caller-owned 小型 Workspace；字段、reserved、selector、identity 或长度任一非法均
返回错误，输出保持不变。

### 5.2 Requirement 与 Proof

Requirement 精确携带 Domain、Foundation Transaction ID、期望 Record Generation、绝对
Deadline、Transition Fingerprint、Runtime/Owner/Domain Generation、Schema/Version、Operation
Kind、Body Digest 与 immutable canonical Body。

`activate()` 必须再次比对：

- exact Persistence Handle；
- exact Domain/Schema/Operation/Owner；
- committed generation 与 witness generation；
- Foundation Transaction ID、Transition Fingerprint、Body bytes/digest；
- 当前 Local/Peer Binding、Link Generation、Policy Generation 与未过期 Deadline。

任一不一致都不能发布 Session。Proof 只证明记录已提交并 reload，不替代当前 Identity、Policy、
Link 或 ACL 授权。

## 6. C1 安全数据面

### 6.1 Origin AAD

C1 Origin AAD 固定 72 B：

```text
"UCN6-ORIGIN-V1"
u32be(canonical_length=54)
byte0 Version+Contract
byte1 MessageMeta
byte2 OriginSecurity + zeroed HopLimit
u8 contract=1
Source Principal + u32be(Source Binding Generation)
Destination Principal + u32be(Destination Binding Generation)
u16be(Service ID)
u32be(Origin Sequence)
u32be(Payload Length)
```

真实 Wire Header 不被修改；只在 AAD 副本中清零 HopLimit。C1 本阶段只接受 BestEffort、
OneWay、Data，Traffic Class 可为 Q0～Q3。地址宽度由 Session/Realm 唯一决定，不按帧长度猜测。

### 6.2 O1/O2

- O1：Provider 必须保持 Payload 明文逐字节不变，并生成 16 B Origin Tag；若 Provider 篡改
  Payload，即使返回成功也转为 `UCN_ERR_SECURITY`。
- O2：Provider 使用 frozen Selector、72 B AAD 与 37 B Nonce 输入产生 ciphertext 和 16 B
  Tag；协议层不允许 Provider 自行选择另一个 Suite/Key。

Sequence Nonce 输入为：

```text
"UCN6-NONCE-SEQ-V1"
16 B Origin Context Fingerprint
u32be(Origin Sequence)
```

Origin Sequence 在调用 Provider 前已从当前 Session 单调预留；Provider 一旦被调用，失败也烧掉
该 Sequence，避免重试复用 AEAD Nonce。

### 6.3 H1

H1 Trailer 固定为：

```text
u32be(Hop Sequence)
12 B Hop Tag
```

Hop AAD 覆盖当前完整 Header、Payload、Origin Tag、Hop Sequence 和 Hop Context Fingerprint。
Origin 和 Hop 使用独立 selector、独立 sequence、独立 replay window。H0 不分配 Hop 状态。

## 7. Replay 与提交顺序

RX 顺序固定为：

```text
结构/容量/别名预检
  -> 当前 Session/Facts/ACL 重验
  -> H1 verify（如启用）
  -> O1/O2 open
  -> reserve Origin Replay
  -> reserve Hop Replay（如启用）
  -> 返回 plaintext + combined Replay Handle
  -> 上层业务预检
  -> commit 或 abort
```

Replay Window 保存最高已提交 Sequence、64-bit bitmap 和固定 reservation 槽。组合 Handle 必须
同时精确匹配 Origin 与 Hop reservation，才能一次提交；Hop reserve 失败会撤销刚建立的 Origin
reservation。过期、重复、错 Handle、旧 Session、事实漂移或槽满均失败关闭。

## 8. 回调、并发与内存所有权

- 每个 Owner 有独立状态锁；外部 Provider 调用期间不持有 Owner 锁；
- 所有 Security Owner 共享调用方持有的 callback Gate，跨 Owner/跨线程重入 fail-closed；
- Provider 调用前冻结 Candidate/Proof/Request，返回后重新获取状态锁并逐字节复核；
- Packet/Plaintext/AAD/Nonce/Provider Request/Replay 临时对象位于 caller-owned Workspace；
- owner、输入、Workspace、输出和长度之间任意部分重叠在 Sequence 预留或 Provider 调用前拒绝。

普通 GCC 下最大 Security 单函数栈为 240 B，自动门禁上限为 256 B。Sanitizer 插桩会放大 `.su`
结果，因此 Sanitizer 配置只运行内存安全测试，不拿插桩后栈值冒充生产栈。

## 9. 固定资源

| Profile | Session 槽 | 每窗口 Replay reservation | Owner | Packet Workspace | Slot |
| --- | ---: | ---: | ---: | ---: | ---: |
| Nano | 2 | 2 | 2032 B | 1480 B | 776 B |
| Lite | 8 | 4 | 8480 B | 1480 B | 1000 B |
| Full | 16 | 8 | 23648 B | 2248 B | 1448 B |

这些是当前 Host ABI 的 `sizeof`，不是目标 MCU 的最终 `.bss`/对齐证明。Workspace 由调用方复用，
不得为每个 Session 分配一份。

## 10. 测试覆盖

### 10.1 正向

- Record raw↔typed roundtrip；
- 静态 Session prepare→Requirement→bind→reload Proof→ACTIVE；
- exact ACL、TX Sequence、AAD 与 Nonce 字节；
- C1 `O1/H0`、`O2/H0`、`O1/H1`、`O2/H1` 双端 roundtrip；
- Replay reserve→commit、reserve→abort 和有界 maintenance；
- 更高 Session 激活后旧 Session Fence/retire；
- Nano/Lite/Full 容量边界与资源输出。

### 10.2 负向与零写

- 错 Principal/Domain/Binding/Link/Policy/Deadline/ACL/Opcode/方向；
- 错 Persistence Handle/Tx/Generation/Witness/Fingerprint/Digest/Body；
- 非 ACTIVE、旧 Handle、重复或过旧 Sequence、Replay in-flight 冲突；
- Tag/Hop Tag 篡改、O1 Provider 非法改写 Payload；
- 输出容量少 1、输入输出部分重叠、Session/Replay 槽满；
- callback 内递归控制调用和两个 Owner 同时进入同一个 Provider Gate；
- Provider 已调用后的失败必须烧掉 Origin/Hop Sequence。

## 11. 验证矩阵

| 环境 | 结果 |
| --- | --- |
| Windows GCC 14.2 Full Debug | 全量 CTest 55/55 |
| Windows GCC 14.2 Lite Debug | 全量 CTest 55/55 |
| Windows GCC 14.2 Nano Debug | 全量 CTest 55/55 |
| MSVC 19.29 Full Release `/W4 /WX` | Security/资源定向 2/2 |
| WSL GCC Nano | Security/资源/并发/栈 4/4 |
| WSL Clang 18 Nano | Security/资源/并发 3/3 |
| WSL ASan/UBSan Nano | Security/资源/并发 3/3 |
| WSL GCC `-fanalyzer -Werror` Nano | Security/资源/并发/栈 4/4 |
| Nano `UCN_BUILD_TESTS=OFF`, Persistence OFF | 产品构建成功；简化 Security archive 0 |

当前主机的 WSL TSan 在测试进程启动前报告 `FATAL: ThreadSanitizer: unexpected memory mapping`，
因此本报告不把 TSan 写成通过。共享 Gate 的并发行为由普通 pthread 测试覆盖，仍须在可运行的
TSan/目标 RTOS/SMP 环境复验。

## 12. 两轮全体自审

### 12.1 正向：合同→代码→测试

逐项从状态机、Persist-before-use、当前事实、ACL、AAD/Nonce、O1/O2、H1、Replay、容量和
Feature-OFF 合同追到实现入口，再追到正式测试。没有发现由测试替代生产判断、由 Provider
返回值直接授予 ACTIVE、由 digest 代替 exact 字段比较或由 H1 代替 Origin 认证的路径。

### 12.2 反向：故障→副作用→恢复

从坏 Proof、坏 Tag、Replay、容量满、Provider 失败/重入、状态锁重获、过期和事实变化反向
检查：所有失败均发生在应用输出写回前；TX 在 Provider 调用后只允许烧号不允许复用；RX 在
combined Replay commit 前不发布业务副作用；Fence 清理 reservation 后禁止继续使用旧 Handle。

### 12.3 自审发现并修复的问题

首次实现把 Candidate、Origin/Hop Provider request、Replay Handle 和 digest 放在公开入口的
自动对象中，`open()` 的栈接近 Nano 768 B 调用链门限。现已全部移到 caller-owned Workspace，
`protect/open` 均降到 240 B，并新增自动栈门禁，避免后续回归。

反向检查还发现接收端虽然用 Wire Service ID 做 ACL，却没有要求调用方的 Access Context
Service ID 与 Wire 值相同。在存在多个合法 Service 时可能发生上下文错绑。现已在密码 Provider
和 Replay 之前做精确相等检查，并增加“只改变调用上下文 Service、原始认证帧不变”的零写拒绝
回归。

最后一轮 Handle 规范性审计发现：`has_hop=0` 时，调用方仍可能在组合 Replay Handle 中夹带
非零 Hop Sequence、Hop reservation generation 或 Hop slot。旧实现虽然不会提交 Hop Window，
但接受非 canonical Handle 会扩大后续维护和跨实现歧义。当前实现要求无 H1 的三个 Hop 字段
全部为零；变异 Handle 的 commit/abort 均原子拒绝，原始 Handle 随后仍可正常提交。

同轮生命周期审计发现，最初的 `owner_init()` 会直接清零非零 Owner，递归初始化可借此抹掉
ACTIVE/FENCED 状态。修复后，调用方必须提供完整清零的 Owner 存储；初始化先取得外部
`state_lock`，再在锁内检查全零并写入，任何 live Owner 都以 `UCN_ERR_STATE` 零写拒绝。正式回归
覆盖 Provider callback 内递归 init、ACTIVE 后 init、FENCED 后 init，以及 destroy 清零后合法重建。

## 13. 结论

```text
IMPL-03 implementation        = COMPLETE
IMPL-03 self-review           = PASS
External review               = HOLD / NOT YET PERFORMED
Production crypto             = NOT IMPLEMENTED
Public Runtime integration    = NOT IMPLEMENTED
MCU / Flash / power-loss      = NOT PROVEN
IMPL-04                       = BLOCKED BY IMPL-03 EXTERNAL REVIEW
```

本结论只证明静态 Security Session 私有 C 模块已经形成可复审的软件候选；不能据此宣称整个
V6 简化协议、安全产品或实机链路已经完成。
