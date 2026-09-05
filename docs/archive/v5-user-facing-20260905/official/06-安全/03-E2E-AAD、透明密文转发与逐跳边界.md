# E2E AAD、透明密文转发与逐跳边界

> 文档级别：`NORMATIVE`
> 实现状态：`CURRENT`
> 事实源：`ucn_frame_write_e2e_aad()`、Security/Path tests
> 最近核对：`a093862`，2026-08-25

## E2E 数据流

```text
源节点：authorize_tx → build AAD → seal → Frame Encode
中继：验证外层 → 递减 Hop/更新允许字段 → 不解密转发
目标：authorize_rx → build 同一 AAD → open → Endpoint/Service
```

E2E Tag 固定 16 B。中继不需要拥有业务解密密钥，适合 A→B→C 中 B 只负责转发密文。

## AAD 绑定

AAD 绑定不可由中继任意修改的路由身份字段。存在 Path 扩展时还绑定 Path ID，防止攻击者把受保护业务从已授权 Path 改绑到另一条 Path。

Hop Limit、某些 Route Epoch 等逐跳必须变化的字段不进入不变 AAD，避免每个中继都需要重新加密。产品仍可在 Link 层额外提供逐跳认证。

## 逐跳与端到端

- E2E：中继不可读取/修改业务 Payload；目标验证源到目标完整性。
- Hop-by-hop：每段 Link 可另有 MAC/加密，保护本地介质和邻居身份。

两者可同时使用。仅 E2E 不隐藏外层地址、Traffic、长度和路径模式；需要流量分析防护属于产品/介质层。

## Gateway

Gateway 只要保持受保护身份和 Payload 不变即可转发密文。若要终止安全域、改 Endpoint 或解包再分发，它就成为安全端点，必须显式 open、authorize、重新 seal，并承担密钥和审计责任。

## 当前 30 B AAD 的精确内容

`ucn_frame_write_e2e_aad()` 当前按固定大端格式写入：

| Offset | 字段 |
| ---: | --- |
| 0 | Version + Wire Profile |
| 1 | Message Type / Endpoint |
| 2 | Traffic Class |
| 3 | 仅 `E2E_PROTECTED` 与 `PATH_ID` Flags |
| 4..7 | Network ID |
| 8..11 | Source Node ID |
| 12..15 | Destination Node ID |
| 16..19 | Sequence |
| 20..23 | Session ID |
| 24..25 | Payload Length |
| 26..29 | Path ID；不存在时为 0 |

AAD 固定按 32-bit 规范表示关键身份，不随 W0～W3 地址宽度缩短。Provider 不应自行重新拼一份“差不多”的 AAD，必须调用公共函数。

## 为什么 Hop/Route 可变字段不进入 AAD

中继每跳都要递减 Hop、更新允许的路由状态。如果这些字段被 E2E Tag 绑定，中继必须拥有端到端 Key 重新 seal，违背透明密文转发。AAD 只绑定端到端不可变身份；逐跳字段的真实性可由 Link 层 MAC/邻居认证保护。

这不是“Hop 可被攻击者随便改也没关系”。恶意修改会影响可用性/路由，产品若要求保护必须增加逐跳安全；E2E 保证的是 Payload 不被无钥中继改成另一个源、目标、Endpoint 或 Path 身份。

## 密文长度与 Frame 开销

当前 seal 产生与 plaintext 等长 ciphertext，额外固定 16 B Tag。选择 Wire Profile/MTU 时必须先为 Tag 留空间。一个原本恰好装下的明文 Frame 开启保护后可能需要更高 Profile、更小 Payload 或 Transfer Fragment。

禁止为了适配 MTU 删除 Tag 或自动转 Plain。

## 中继处理顺序

中继看到 protected Frame 时：

1. 校验外层 Wire/CRC/Network/TTL；
2. 检查 Forward Policy 和路由/Path；
3. 不调用 `open()`、不修改 ciphertext/Tag/AAD 字段；
4. 只修改允许逐跳字段；
5. 重新计算外层 Frame CRC并发送。

中继可以看到 Source、Destination、Endpoint、长度和时序，不能看到业务明文。需要隐藏这些元数据要使用隧道、流量填充或更外层网络方案。

## Path ID 绑定示例

控制命令被允许只走安全 CAN Path 7。源节点 seal 时 AAD 包含 Path ID 7。攻击者/中继把 Frame 改成 Path 9，即使 ciphertext 未变，目标重建 AAD 后 Tag 验证失败。

没有 Path 扩展时 AAD 对应字段为 0；不能在中途无认证地“补一个 Path ID”。

## Gateway 两种模式

| 模式 | 行为 | 安全责任 |
| --- | --- | --- |
| Opaque relay | 不解密，保持身份/Payload/Tag | 只负责外层转发/可用性 |
| Security terminator | open 后转换协议/Endpoint，再重新 seal | 拥有两侧 Key、ACL、审计和数据保护责任 |

第二种不是普通中继，必须在威胁模型中列为可信边界。

## 验证清单

- [ ] AAD 30 B golden vector 和每字段篡改测试；
- [ ] W0～W3 都生成相同规范宽度的身份表示；
- [ ] Hop/允许 Route 字段变化后目标仍可 open；
- [ ] Source/Destination/Endpoint/Traffic/Sequence/Session/Length/Path 任一变化 open 失败；
- [ ] 中继无 Key 也能转发且不能读 Payload；
- [ ] Protected MTU 计算包含 16 B Tag；
- [ ] Gateway terminator 与 opaque relay 权限完全分开。
