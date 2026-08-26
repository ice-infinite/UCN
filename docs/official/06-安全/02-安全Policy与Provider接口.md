# 安全 Policy 与 Provider 接口

> 文档级别：`NORMATIVE`
> 实现状态：Lite/Full `CURRENT`；Nano 无 Security Feature
> 事实源：`ucn_security.h`、Node security APIs/tests
> 最近核对：`a093862`，2026-08-25

## 三组 Policy

| 方向 | 取值 | 含义 |
| --- | --- | --- |
| TX | Plain / E2E Protected / Auto | 发送明文、必须保护或由 Provider 选择 |
| RX | Plain Only / Encrypted Only / Both | 接受哪些线帧 |
| Forward | Plain+Opaque / Opaque Only / Terminal Only | 中继可转发什么 |

Policy 可设置为 Node 默认，并对具体 Endpoint 覆盖。某节点可以按配置加密或不加密，不强制“无线一定加密、本地一定明文”。

## Provider Ops

- `load_next_sequence` / `store_next_sequence`：持久化下一发送 Sequence；
- `get_session_id`：返回当前非零安全 Session；
- `authorize_tx/rx`：在 seal/open 前做权限判断；
- `select_tx_protection`：Auto 模式选择；
- `seal/open`：产品 AEAD；
- `rotate_session`：原子建立新 Session/Key/next Sequence。

## 初始化门禁

产品可开启 `UCN_SECURITY_REQUIRED_BY_DEFAULT` 或运行期 required gate。要求安全时，Provider/Session/Sequence 未就绪必须让 Node 初始化或发送失败关闭，不能回退明文。

## 中继

中继处理外层路由，不需要调用 `open()`。只有目标节点解密业务 Payload。中继若配置 `OPAQUE_E2E_ONLY` 会拒绝明文转发；`TERMINAL_ONLY` 不转发任何业务。

## 当前不内置

仓库不选择 AES-GCM、ChaCha20-Poly1305、证书或密钥存储实现。测试 Provider 只验证协议调用顺序和门禁。

## Policy 的匹配层级

Node 有默认 Policy，静态 Endpoint 可覆盖。发送/接收时先找到最具体 Endpoint Policy，没有覆盖才使用默认。Policy 只表达允许模式，不保存 Key 或身份数据库。

例如：

```text
默认：TX Auto, RX Both, Forward Plain+Opaque
Servo Endpoint：TX Protected, RX Encrypted Only
Public Telemetry：TX Plain, RX Both
Gateway Node：Forward Opaque Only
```

产品可以让有线也加密、无线也明文测试，但生产决定必须来自威胁模型，而不是介质名字硬编码。

## 发送调用顺序

受保护发送的概念顺序：

1. 构造完整 Frame 身份字段和 Endpoint；
2. 解析 Endpoint/默认 TX Policy；
3. Auto 时调用 `select_tx_protection()`；
4. `authorize_tx()` 检查 Destination/Endpoint/会话/业务权限；
5. 获取/持久推进可用 Sequence 与 Session；
6. 用 `ucn_frame_write_e2e_aad()` 构造 AAD；
7. `seal()` 产生等长 ciphertext 和 16 B Tag；
8. 编码 Frame 并提交 Link；
9. 任一步失败都不自动重新发明文。

Sequence 持久化与发送的精确顺序必须保证掉电不重复 nonce。只在“物理发送成功后”才 store 往往不安全，因为掉电可能发生在上线后、store 前。

## 接收调用顺序

1. 严格 Wire/长度/Network 检查；
2. 根据保护 Flag 与 RX Policy 判定是否允许；
3. `authorize_rx()` 检查 Source、Ingress Link、Session、Endpoint；
4. 对受保护帧用相同 AAD 调 `open()`；
5. 认证成功后才提交可信 replay/业务状态；
6. 明文按 Plain/Both 规则进入 Endpoint；
7. Encrypted Only 收到明文直接拒绝。

认证失败不能更新 Neighbor 权威、Command ID 或可信 Sequence Window。

## Forward Policy 的具体含义

- `PLAIN_AND_OPAQUE_E2E`：可转发合法明文和不解密密文；
- `OPAQUE_E2E_ONLY`：只转发有 E2E 保护标志的业务；
- `TERMINAL_ONLY`：本节点不做业务中继。

Forward Policy 不等于目标 RX Policy。中继可以只转密文，而自身目标 Endpoint 同时允许特定明文本地业务。

## Provider 生命周期

初始化时 Provider 必须先能 load Session/next Sequence，再让 Node 安全 Ready。轮换中应 Fence 新发送，原子 provision 新 Key/Session/Sequence，reload/验证后恢复。Provider context 寿命覆盖 Node，回调不得同步重入 Node/Owner 破坏状态。

## REQUIRED gate

安全 required 时以下任一情况都要失败：缺 ops、Session=0、Sequence 无效、store/load/rotate 失败、Endpoint required protection 无 Key。测试/兼容模式必须由产品显式关闭 required；不能收到密文错误后自动切 Plain。

## 错误分类

- 权限拒绝：`ACCESS`；
- Tag/认证失败：`SECURITY`；
- 重放：`REPLAY`；
- Provider 缺失/配置错误：`CONFIG/STATE`；
- Sequence 到阈值且轮换失败：`EXHAUSTED/STATE`；
- Queue/Link 错误仍是传输错误，不应记为密码失败。

## Provider 对抗测试

- 每个回调失败点都验证无明文降级；
- seal/open 修改任一 AAD/Payload/Tag 字节必失败；
- store 撕裂/回滚/掉电；
- rotate 同步/异步失败和重启；
- 回调同步重入 send/receive/step；
- 多 Endpoint Policy 冲突；
- Forward Opaque/Terminal 的正负矩阵；
- Lite/Full/Nano Feature 边界。
