# UCN 按需加密与透明转发建议

> 状态：**v4 Core 已实现线格式、Provider、策略与透明转发；生产密码/密钥/身份体系仍待接入**。  
> 日期：2026-08-07  
> 适用范围：`UCN-Core` 的生产安全演进；保持 MCU-first、固定资源、Linux 非必需和按产品配置启用的原则。

## 1. 结论

UCN 不应规定“无线一定加密”或“所有帧一定加密”。每个产品可按 **Node 默认策略 + Endpoint 覆盖策略 + 对端身份/密钥表** 决定一帧是否加密、目标是否允许明文、以及中继是否需要解密。

默认建议的业务加密是**端到端 Payload AEAD**：路由头保持可读，中继只按目的 Node 和 Hop Limit 转发密文，不持有业务解密密钥；最终目标 Node 完成 Tag 校验、解密、重放检查和 Endpoint ACL 后才分发给应用。

这与可选的“逐跳 Link AEAD”并不冲突：前者保护原始业务来源到最终目标，后者保护某一段物理链路。两者都由配置显式启用，不根据 WiFi、CAN、RS485、BLE 或 Linux 身份隐式决定。

## 2. 当前实现边界

| 项目 | 当前 v4 Core 实现 | 仍待产品接入 |
| --- | --- | --- |
| 帧 | 32 B 基础头、36 B Route Header 或 40 B Path Header；`E2E_PROTECTED=0x02` 时追加固定 16 B Tag；CRC 覆盖密文和 Tag。 | 逐跳 Link AEAD 封装。 |
| Security Provider | 会话 ID、持久化发送序号、TX/RX 授权、可选 `select_tx_protection`、固定缓冲 `seal/open` 回调。 | 密钥选择/供应、生产重放窗口和 Endpoint ACL 表。 |
| 加密 | Core 不内置密码算法；测试使用明确标识的非生产 Provider 验证调用链。 | 用经审计的 AEAD Provider 替换测试 Provider，并测资源。 |
| 转发 | 中继按 Node/Endpoint `forward_mode` 可透明转发密文，不调用 `open`。 | 逐跳 Link AEAD 的 Adapter 实现。 |

因此，`E2E_PROTECTED`、Tag、策略、AAD 与透明转发是当前 v4 Core 能力；**AEAD 算法、密钥、真实身份、ACL、轮换和撤销不是**。没有提供经审计 Provider 的固件不得宣称拥有机密性或抗篡改能力。

## 3. 配置模型：Node 默认，Endpoint 覆盖

不能只设置一个全局“加密开关”。同一 Node 既可能发布低风险温度，也可能接收高风险电机命令；必须允许 Endpoint 覆盖默认值。

### 3.1 Node 默认策略

| 配置 | 枚举 | 含义 |
| --- | --- | --- |
| `tx_mode` | `PLAIN` / `ENCRYPTED` / `AUTO` | 本 Node 发出的业务默认明文、默认端到端加密，或由目标/Endpoint 表选择。 |
| `rx_mode` | `PLAIN_ONLY` / `ENCRYPTED_ONLY` / `BOTH` | 本 Node 的默认接收能力与接收要求。`BOTH` 是显式允许，不得在认证失败后自动降级。 |
| `forward_mode` | `PLAIN_AND_OPAQUE_E2E` / `OPAQUE_E2E_ONLY` / `TERMINAL_ONLY` | 中继可转发明文及端到端密文、仅转发端到端密文，或不转发业务。透明转发不等于解密。 |
| `peer_policy` | 固定白名单 / 产品 CA / 配网 PSK | 决定允许与哪些真实身份建立会话，不能只相信可伪造的 Node ID 或 MAC。 |

`AUTO` 在当前 Core 中调用可选 `select_tx_protection` Provider 回调，默认不保护；它不能理解为“先试加密，失败再发明文”。对 `ENCRYPTED_ONLY` Endpoint，发送端没有可用密钥时必须返回安全错误，而不是降级。

### 3.2 Endpoint 覆盖策略

Endpoint ACL 继续使用 `(source_node, destination_node, endpoint, operation)`；在此基础上增加最小安全等级。

```text
MOTOR0_COMMAND_V1
  tx_mode            = ENCRYPTED
  rx_mode            = ENCRYPTED_ONLY
  allow_source        = FLIGHT_CONTROLLER_A
  forward_mode        = OPAQUE_E2E_ONLY
  e2e_key_slot        = MOTOR_A_TO_C

TEMPERATURE0_V1
  tx_mode            = AUTO 或 PLAIN
  rx_mode            = BOTH
```

最终接收节点必须先完成 AEAD 验证和解密，再检查 Endpoint ACL 并交给业务。任何 Node 都不能凭“这帧头写着 source=A”就相信它来自 A。

## 4. 透明中继的端到端流程

以 A → B → C 为例，A 与 C 持有同一个端到端会话密钥，B 没有：

```text
A：构造清晰可路由的 UCN 头
   Payload 用 A→C 的 E2E Key 加密，附 Tag
        ↓
B：只读取目标、TTL、路由扩展
   验证本地准入和转发策略，TTL 减一、重算 CRC
   不解密 Payload，也不能伪造 A→C 业务
        ↓
C：按 (source=A, destination=C, endpoint, session) 找到 E2E Key
   验 Tag、查重放窗口、解密 Payload、检查 Endpoint ACL、业务分发
```

这正是“发送端按配置加密、某些中继只转发不解密、指定目标节点解密”的模式。它能防止没有密钥的监听者和中继读取或修改业务 Payload；若中继篡改不可变身份/Endpoint 字段，最终 Tag 校验也必须失败。

### 4.1 可变与不可变字段

中继必须改写 `Hop Limit`，路径切换期还可能处理 `route_epoch`；这些字段不能直接放进端到端 AEAD 的不可变 AAD。v4 已冻结为：

| 字段类别 | 处理原则 |
| --- | --- |
| 不可变 AAD | Version、`E2E_PROTECTED`/Path 标志、Network ID、Source、Destination、Message Type/Endpoint、Traffic Class、Session ID、Sequence、Payload Length，以及存在时的 Path ID。 |
| 可变转发元数据 | Hop Limit、Route Extension、CRC。中继可按协议改写；它们不能作为“原始来源身份”的证明。 |
| 受保护内容 | 整个业务 Payload 和 AEAD Tag。 |

最终目标以不可变 AAD 和密文共同验 Tag。即使 B 能修改 TTL 或重算 CRC，也不能把 IMU 密文改成电机命令、不能把来源 A 改成另一控制节点，更不能构造有效 Tag。

## 5. v4 业务线格式（已实现）

### 5.1 明文业务帧

```text
[UCN Header 32 B / Route Header 36 B / Path Header 40 B][Plain Business Payload P]
```

### 5.2 端到端受保护业务帧

```text
[UCN Header 32 B / Route Header 36 B / Path Header 40 B, E2E_PROTECTED=1]
[Ciphertext Business Payload P]
[AEAD Tag 16 B]
```

`Payload Length` 仍只表示 `P`，不包含 Tag；解码器依据 `E2E_PROTECTED` 计算实际帧长。CRC 覆盖密文和 Tag，用于快速误码丢弃，但 Tag 才是安全校验。

已冻结标志位：`ROUTE_EXTENSION = 0x01`、`E2E_PROTECTED = 0x02`、`DIAGNOSTIC = 0x04`、`PATH_ID = 0x08`；Path 业务帧必须同时具备 Route Extension 和非零 Path ID。仅诊断控制帧可使用 `DIAGNOSTIC`，控制帧不允许 E2E/Path ID，其他未知值由解码器拒绝。v3/v4 不能在同一 Network ID 内静默混用。

在默认 256 B 最大帧下：

| 帧 | 总长度 | 最大业务 Payload |
| --- | ---: | ---: |
| 正常明文 | `32 + P` | 224 B |
| 正常端到端受保护 | `32 + P + 16` | 208 B |
| 带路径扩展的明文 | `36 + P` | 220 B |
| 带路径扩展的端到端受保护 | `36 + P + 16` | 204 B |
| 带 Path ID 的明文 | `40 + P` | 216 B |
| 带 Path ID 的端到端受保护 | `40 + P + 16` | 200 B |

Nonce 不额外放入每一帧；由方向、Source Node、Session ID、Sequence 和密钥 Epoch 按已冻结算法确定。前提是同一密钥下绝不能重用该组合：发送 Counter 需持久化或在重启前后换出新会话密钥，Sequence 到上限前必须换密钥。

### 5.3 可选逐跳 Link AEAD

当产品希望连路由头也不暴露给某段物理链路的监听者时，可由该 Link Adapter 再包一层，而不是强迫整个网络改变业务策略：

```text
[Link Sec Flag 1 B][Key Epoch 1 B][Hop Counter 4 B]
[Encrypted complete UCN frame]
[Link AEAD Tag 16 B]
```

该封装约增加 22 B，仅在该 Link 的策略明确启用时出现。中继在入站 Adapter 解开逐跳封装，Core 再按普通或端到端受保护的 UCN 帧转发；出站 Adapter 是否再次封装由下一条 Link 配置决定。它可与端到端 Payload AEAD 同时启用。

## 6. 密钥、入网与控制帧

1. `HELLO` 只做物理 Candidate 与 Node ID 发现，不能作为身份认证。
2. `JOIN_REQ / JOIN_CHALLENGE / JOIN_ACCEPT` 完成真实身份校验、挑战应答、会话密钥建立和权限下发；Coordinator 可以是 MCU，Linux 不参与必经安全路径。
3. 端到端业务密钥只给发送 Endpoint 与最终接收 Endpoint；透明中继不保存该 Key。
4. 路由、入网、Heartbeat、Probe 等控制帧不能套用“不解密透明业务转发”语义：它们需要各自的逐跳认证/会话保护，否则中继无法安全处理控制 Payload。
5. 节点撤销、密钥 Epoch 轮换和重放窗口均为固定容量表项；表满、计数器回退、Tag 失败和 ACL 拒绝应有计数与退避，不能无限占用队列。

## 7. 安全覆盖范围与限制

| 威胁 | 端到端 Payload AEAD 的结果 |
| --- | --- |
| 空口监听业务数据 | 无端到端密钥时无法读取 Payload。 |
| 空口修改业务数据 | Tag 校验失败，目标节点丢弃。 |
| 伪造来源/Endpoint | 不可变 AAD 与 Endpoint ACL 共同拒绝。 |
| 重放旧业务帧 | 目标的 `(source, session, sequence)` 重放窗口拒绝。 |
| 已授权但被攻破的透明中继 | 不能读取或伪造端到端 Payload；仍可能丢弃、延迟或干扰转发。 |
| 无线干扰、总线阻塞 | 密码不能消除；仍需 T17/T18 的限流、切路和本地失效安全。 |
| 配置为明文的 Endpoint | 不提供机密性或抗恶意篡改，只有 CRC 和已启用的准入策略。 |

## 8. T15 实施与验收顺序

1. 已完成 v4 Flag、Tag 长度、AAD 和 v3 拒绝；Nonce/Key Epoch 由生产 Provider 与密钥模型冻结。
2. 已完成固定缓冲 `seal/open` 接口和带 Tag 的精确长度/CRC。
3. 已完成 Node 默认策略、固定 Endpoint 覆盖表、`PLAIN_ONLY/ENCRYPTED_ONLY/BOTH` 和透明中继策略；密钥槽/ACL 归 Provider/产品表。
4. 后续完成真实 JOIN 身份/密钥建立、Counter/重放窗口、密钥轮换与撤销。
5. 单元测试：明文/密文同 Node 共存、`ENCRYPTED_ONLY` 拒绝明文、坏 Tag、改头、重放、无 Key 透明中继、ACL 拒绝、Counter 回退、表满。
6. 模拟测试：A→B→C 的 B 无端到端 Key 透明转发；明文温度与加密电机命令并发；可选 Link AEAD 与端到端 AEAD 叠加。
7. T14 实机测试：测量 Tag/加解密的 Flash、RAM、栈、CPU、最大帧、时延、丢包、重放与干扰下的失败行为。

当前 UCN 已是 v4 Core；只有在第 4、7 步完成并在目标硬件复核后，产品才可宣称使用了生产级端到端保护。
