# V6S-00-04 Golden、Negative、Fuzz 与属性测试合同

> 状态：`DONE / SELF-REVIEW PASS / FINAL EXTERNAL REVIEW PENDING`
>
> 本文冻结测试 oracle，不使用未来 Codec 的 builder 生成 decoder fixture，也不以随机 fuzz
> 代替字段级负向矩阵。

## 1. 三类证据互不替代

| 证据 | 负责发现 | 不能替代 |
| --- | --- | --- |
| Golden | 字节 offset、端序、固定值、Tag 输入漂移 | 全部非法字段 |
| Deterministic Negative | 每条规范门禁被放宽、输出被部分写回 | 未知组合探索 |
| Fuzz/Property | 组合、长度和状态空间中的意外交互 | 独立规范 oracle |

所有 fixture 均存完整输入和期望输出。测试失败时必须报告 Contract、字段、规则和 seed，不能只
报告“decode false”。

## 2. 十条初始 Golden

以下十六进制不含 Carrier framing。A1 地址宽度为 2 B。

| 名称 | 长度 | 精确 Core Packet bytes |
| --- | ---: | --- |
| C0 A1 O0/H0 Bootstrap | 31 B | `600101010203040000FFFF000000000000000001020304050607080001AA55` |
| C1 A1 O0/H0 Data | 17 B | `61400312345678010201020304DEADBEEF` |
| C2 O0/H0 structural Control | 12 B | `622501123401020304040100` |
| C3 O0/H0 Data | 10 B | `63800211112222AABBCC` |
| C4 O0/H0 Latest Data | 13 B | `6450041111222201020304CAFE` |
| C5 public-discovery O0/H0 | 15 B | `65000311112222000101020304BEEF` |
| C2 O1/H2 Auth-only | 29 B | `620041123400000001010203048D2206580D6E913E15F14DE32AC68EF9` |
| C3 H1 Hop-auth | 26 B | `63800211112222AABBCC00000001B61FCD2ED841CF792777F4F1` |
| C0 A1 O2/H0 AES-128-GCM | 49 B | `602581010203041234567800000001000000020102030405060708001358B2377998A3E0C198106C043A0DBCC6BE150D6C` |
| C2 O2/H2 ChaCha20-Poly1305 | 29 B | `62008112340000000181052B38E079A5ACC9BDDBE6A1E43903DC3541C2` |

前六条用于 raw Codec。后四条必须由独立密码 oracle 重算完整 bytes，不能只比较手写 Tag。
C2 structural Control 的 Payload 不是完整业务 Request，只证明 Core
字段布局；semantic parser 必须因缺少完整 Operation Envelope 拒绝它。C5/O0 只有产品 Manifest
明确开启 public discovery 才可通过 semantic policy，默认安全 Group 必须拒绝。

## 3. 受保护 Golden 输入

C2 O1/H2 使用：

```text
origin key = 000102030405060708090A0B0C0D0E0F
flow fingerprint = 202122232425262728292A2B2C2D2E2F
direct fingerprint = 303132333435363738393A3B3C3D3E3F
origin sequence = 00000001
payload = 01020304
canonical AAD =
55434E362D4F524947494E2D56310000002C62004002
202122232425262728292A2B2C2D2E2F00000001
303132333435363738393A3B3C3D3E3F00000004
expected HMAC-SHA-256-128 tag = 8D2206580D6E913E15F14DE32AC68EF9
```

C3 H1 使用：

```text
hop key = 101112131415161718191A1B1C1D1E1F
hop context fingerprint = 404142434445464748494A4B4C4D4E4F
hop sequence = 00000001
canonical Hop AAD =
55434E362D484F502D56310000001E
63800211112222AABBCC00000001
404142434445464748494A4B4C4D4E4F
expected HMAC-SHA-256-96 tag = B61FCD2ED841CF792777F4F1
```

C0 A1 O2/H0 使用：

```text
AEAD suite = AES-128-GCM
AEAD key = 808182838485868788898A8B8C8D8E8F
directional nonce key = 606162636465666768696A6B6C6D6E6F
                        707172737475767778797A7B7C7D7E7F
C0 context fingerprint = 505152535455565758595A5B5C5D5E5F
transaction id = 0102030405060708
protocol opcode = 0013
direction = 00
interaction role = 01
derived nonce = 23543C789589E4D6067570A4
plaintext payload = DEADBEEF
canonical AAD =
55434E362D4F524947494E2D56310000002260258000
010203041234567800000001000000020102030405060708001300000004
expected ciphertext = 58B23779
expected tag = 98A3E0C198106C043A0DBCC6BE150D6C
```

C2 O2/H2 使用：

```text
AEAD suite = ChaCha20-Poly1305
AEAD key = B0B1B2B3B4B5B6B7B8B9BABBBCBDBEBF
           C0C1C2C3C4C5C6C7C8C9CACBCCCDCECF
directional nonce key = 909192939495969798999A9B9C9D9E9F
                        A0A1A2A3A4A5A6A7A8A9AAABACADAEAF
flow fingerprint = 202122232425262728292A2B2C2D2E2F
origin sequence = 00000001
derived nonce = 1E606C8E60B022EAE3BC7931
plaintext payload = 01020304
canonical AAD =
55434E362D4F524947494E2D56310000002C62008002
202122232425262728292A2B2C2D2E2F00000001
303132333435363738393A3B3C3D3E3F00000004
expected ciphertext = 81052B38
expected tag = E079A5ACC9BDDBE6A1E43903DC3541C2
```

Origin AAD 中 Common Header byte 2 必须先执行 `byte2 & 0xC0`；因此上述 C2 Wire
Header 的 byte 2 为 `0x41/0x81`，而 canonical AAD 中分别为 `0x40/0x80`。这条规则防止
逐跳递减 HopLimit 破坏端到端认证，同时禁止 oracle 直接复制原始 Header。

[独立 Wire oracle](../../../../tools/v6/check_v6s_wire_contract.py)从逐字段输入重建这些字节，
并与固定常量比较；未来 Codec 测试不得调用被测 Encoder 生成这些期望值。

## 4. raw decode 负向矩阵

每个 Contract 都必须逐项覆盖：

| 规则族 | 必测变异 |
| --- | --- |
| 版本/Contract | version 0/5/7/15；contract 6/15；不降级猜旧协议 |
| Traffic/Delivery | 四个 Traffic；Delivery 保留值 3；逐 Contract 枚举 2.1 矩阵中的允许/禁止组合 |
| Interaction/Kind | 四角色、四 Kind；C0～C5 逐项覆盖，不使用“其他由 Policy 决定”替代结构矩阵 |
| Origin Security | O3；O1/O2 缺 Tag；多余 Tag；H Profile 不唯一 |
| Hop Limit | 0、1 边界、63、逐跳从 1 再转发 |
| 长度 | base-1、base、base+1、精确 payload、Tag-1/Tag+1、算术下溢 |
| 地址 | 0、全 1、宽度错配、Realm 未知、多 Realm 候选 |
| ID/Generation | 0/保留值；最大合法终值必须接受；在最大值上再次分配、跨域/错父域/旧代际必须由状态层拒绝 |
| Opcode | 0、未登记、错模块范围、experimental 在 release |
| Context | 缺失、多个活动映射、Link reopen、错 Flow/Group/Sender |
| Padding/Carrier | 非零 Carrier padding、无可信 frame boundary、H0/O0 无 FCS |

对于 C0～C5，每个实际字段都必须在独立 registry fixture 中标注 `required_nonzero`、
`reserved`、`range`、`context_bound` 或 `opaque_payload`，逐规则构造非法值。
固定宽度字段无法编码 `TYPE_MAX+1`，因此“最大值”不能作为通用 raw 负例。分配器必须另测
`current=TYPE_MAX -> checked_next=EXHAUSTED/FAULT`，并断言高水位和输出不写回。

## 5. semantic policy 负向矩阵

raw 合法不表示业务可接受。至少覆盖：

- C3 携带 Latest/Reliable/Request/Result/Error/O1/O2/Control/Transfer；
- C5/O0 未被 Manifest 明确授权 public discovery；
- Endpoint 要求 O1/O2，Frame 或 Context 只提供 O0；
- Payload Kind=Control/Diagnostic 但缺 2 B Opcode，或 Opcode 不在 ACL；
- Request/Result/Error 缺 Operation ID/Flags/Result Code；
- Transfer Fragment 与 Setup 的 Interaction/Service/Parent 不同；
- C2 下一跳不是最终目标，H2 缺 Direct Context 或存在中继；
- C1 地址在当前 Realm 映射为零个或多个 Principal；
- Capability、Lease、Route/Path、Session 在 raw decode 后、业务副作用前过期；
- Feature OFF 收到对应 Opcode/Envelope，必须在分配模块资源前拒绝。

## 6. builder 负向矩阵

typed→raw 必须使用与 raw fixture 不同的数据源。每个失败测试先填充完整输出和 output-length
哨兵，然后断言逐字节不变：

```text
sentinel_before = output[0..capacity] + output_length + owner_state
result = encode(invalid_typed_input, output, output_length)
require result != OK
require byte_equal(sentinel_before, output + output_length + owner_state)
```

输入/输出任意完整或部分重叠都在首次写入前拒绝。不能只检查首尾字节，也不能允许先分配
Sequence、再因容量不足失败。

## 7. 必须冻结的属性

| 属性 | Oracle |
| --- | --- |
| 合法 typed round-trip | `decode(encode(x)) == x`，比较具名字段 |
| 合法 raw round-trip | `encode(decode(raw)) == raw`，逐字节比较 |
| 失败原子性 | output、length、Sequence、Replay、Context、统计均不变 |
| 中继不变域 | 只允许 HopLimit/Label/raw Context/Hop trailer 变化；Origin 域不变 |
| MTU 守恒 | `header+envelopes+payload+tags == exact core length <= path MTU` |
| 安全单调 | 安全要求只能保持或失败，不能自动降级 |
| 父域隔离 | 数值相同但父 Generation 不同不得命中 |
| Feature OFF | 专用输入明确拒绝，基础 C1 不受影响 |

## 8. Fuzz 合同

初始固定 seed 为 `0x56300004`，每次至少 16384 个 0～`MAX_FRAME+1` B 输入，覆盖：

1. 全随机 bytes；
2. 从十条 Golden 单 bit/单 byte/长度变异；
3. 合法 typed object 的边界值组合；
4. Context 表、Replay 窗口、MTU 和 Feature 组合变异。

Fuzzer 的成功 oracle 必须再经过独立 registry/semantic validator；失败则检查完整不写回。
任何 crash、越界、未初始化读取、无限循环、状态污染或实现与独立 oracle 分歧均失败。

## 9. V6S-00-04 自审

| 检查 | 结果 |
| --- | --- |
| 十条 Golden 由独立脚本逐字段重建 | PASS |
| O1/H2、H1、AES-GCM 与 ChaCha20-Poly1305 已由独立 oracle 重算 | PASS |
| raw、semantic、builder 三层负向门禁分开 | PASS |
| 全字段确定性测试不依赖随机命中 | PASS |
| fuzz seed、次数、输入族和 oracle 已冻结 | PASS |
| 失败输出、状态、Sequence/Replay 完全不写回 | PASS |

结论：`V6S-00-04 = DONE / SELF-REVIEW PASS`。当前 oracle 是合同工具，不是生产 Codec 测试
通过证据；实现后必须把同一向量接入 CTest、Sanitizer 和目标构建。
