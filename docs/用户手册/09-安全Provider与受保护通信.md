# 安全 Provider 与受保护通信

Lite/Full提供安全Policy和Provider调用合同，但不自带生产密钥、AEAD算法、硬件随机数或安全存储。用户必须把经过审计的产品安全实现接入Provider。

## 1. 三个独立策略维度

```c
ucn_security_policy_t policy = {
    .tx_mode = UCN_SECURITY_TX_E2E_PROTECTED,
    .rx_mode = UCN_SECURITY_RX_ENCRYPTED_ONLY,
    .forward_mode = UCN_SECURITY_FORWARD_PLAIN_AND_OPAQUE_E2E
};
```

| 维度 | 可选项 | 含义 |
| --- | --- | --- |
| TX | Plain/E2E/Auto | 本节点产生的帧怎样保护 |
| RX | Plain only/Encrypted only/Both | 终点接受哪类帧 |
| Forward | Plain+Opaque/Opaque only/Terminal only | 中继允许转发什么 |

中间节点可以在不解密Payload的情况下转发E2E密文。只有目标端Provider执行open。

## 2. Provider必须实现什么

`ucn_security_ops_t`包括：

- 加载/持久化下一个Sequence；
- 获取认证Session ID；
- TX/RX授权；
- 决定当前Frame是否保护；
- AEAD seal/open；
- 可选Session轮换。

产品上下文通常保存key handle、Session、Sequence预留区和Replay表，而不是把明文主密钥长期放在普通RAM。

## 3. 初始化顺序

```text
Secure Boot/产品配置校验
 → 读取安全存储双槽记录
 → 建立身份/Session/Sequence预留
 → ucn_node_init
 → set_security_required(true)
 → set_security(ops, context)
 → set_security_policy
 → Endpoint policy覆盖
 → security_ready检查
 → 最后开放外部RX
```

调用示例：

```c
check(ucn_node_set_security_required(node, true));
check(ucn_node_set_security(node, &PRODUCT_SECURITY_OPS,
                            &g_security_context));
check(ucn_node_set_security_policy(node, &default_policy));

if (!ucn_node_security_ready(node)) {
    product_keep_external_links_disabled();
    return UCN_ERR_SECURITY;
}
```

required开启后任何缺失Provider/Policy都应失败关闭，不能自动退回明文。

## 4. Endpoint覆盖

不同业务可使用不同Policy：

```c
ucn_security_policy_t command_policy = {
    .tx_mode = UCN_SECURITY_TX_E2E_PROTECTED,
    .rx_mode = UCN_SECURITY_RX_ENCRYPTED_ONLY,
    .forward_mode = UCN_SECURITY_FORWARD_TERMINAL_ONLY
};

check(ucn_node_set_endpoint_security_policy(
    node, PRODUCT_EP_SERVO_COMMAND, &command_policy));
```

例如：

- 本地公开温度广播允许明文；
- 舵机命令必须加密认证；
- 无线管理诊断必须E2E；
- 中继网关只透明转发密文，不拥有业务密钥。

## 5. AEAD合同

Provider seal/open使用Core生成的固定AAD和16 B Tag。产品必须确保：

- Nonce在同一Key域绝不重复；
- Session+Sequence+方向/Key域构造明确；
- AAD任何字段变化都会认证失败；
- open失败不留下可用明文；
- 算法、Key长度、Nonce规则和Tag长度写入产品规范；
- 使用已知向量和篡改测试验证。

## 6. Sequence持久化

逐帧写Flash会磨损，推荐预留区间：

```text
先原子持久化：已保留到 N
 → RAM使用当前区间
 → 掉电重启跳过未使用值
 → 永不回退复用Nonce
```

达到 `UCN_SEQUENCE_ROTATION_THRESHOLD` 前必须轮换Session/Key。不能让32位Sequence自然回绕后继续使用旧Key。

## 7. Authorization和ACL

Provider的 `authorize_tx/rx` 应至少检查：

- Source/Destination/Endpoint；
- Link/Neighbor状态；
- 当前设备角色和产品安全状态；
- 管理帧/Path控制是否来自授权控制器；
- 是否允许广播；
- 是否处于调试、恢复或锁定模式。

远端Q0命令还要经过Service Validator、Command Guard和业务状态检查。Core Security通过不等于命令一定安全可执行。

## 8. 密钥模型

产品必须明确选择：

- 全网共享Key；
- 每节点pairwise Key；
- Group Key；
- PKI/证书。

至少区分设备身份、会话AEAD、固件签名和调试凭据。一个业务Key泄漏不应允许签署固件。

## 9. 无线与本地链路

安全不应简单等同于“无线才加密”：

- Node/Endpoint按策略选择加密或明文；
- 有线外部接口也可能需要保护；
- 本机Service Fast Path不走Wire AEAD，但仍要本机ACL和业务Validator；
- Wi-Fi/BLE链路层加密不能替代端到端保护。

## 10. 测试清单

- AEAD标准/产品已知向量；
- AAD每字段、Ciphertext和Tag逐位篡改；
- Replay、乱序、旧Session回放；
- Sequence存储每个失败/掉电点；
- Session轮换、Key撤销、旧固件回滚；
- RNG失败、Key缺失、Secure Store损坏；
- 明文到加密Endpoint、密文到plain-only Endpoint；
- 中继Opaque转发且无法解密；
- 每包CPU、Stack、延迟、功耗。

这些通过以前只能称“安全接口已接入”，不能称“产品安全已经完成”。
