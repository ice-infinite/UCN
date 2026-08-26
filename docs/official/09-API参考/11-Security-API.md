# Security API

> 文档级别：`REFERENCE`
> 实现状态：`CURRENT`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：include/ucn 公共头、链接目标与公共 API 测试
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：API 合同不等同于各平台实机驱动完成

Security Policy 决定 Plain、E2E 或 Auto，以及接收、转发和 endpoint 授权；Provider ops 提供 seal/open/authorize、Session/Sequence 和产品上下文。

中间节点可以按路由头透明转发 E2E 密文，不需要密钥；目标节点验证 AAD、tag、Session 和 replay window 后交付业务。Provider 返回成功前必须完整验证，失败不写明文 output。

仓库提供的是接口和测试实现，不是生产密钥系统。产品必须自行提供审计通过的 AEAD、密钥存储、随机数、持久化序列和轮换流程。

## Policy 三个维度

| 维度 | 可选值 | 决定什么 |
| --- | --- | --- |
| TX | PLAIN / E2E / AUTO | 本节点发出 payload 是否保护 |
| RX | PLAIN_ONLY / ENCRYPTED_ONLY / BOTH | 终点允许接收哪些格式 |
| Forward | PLAIN_AND_OPAQUE / OPAQUE_ONLY / TERMINAL_ONLY | 中继是否转发明文/密文 |

它们是按 Node 默认并可被 Endpoint 覆盖的独立选择。例如本地传感器遥测可允许 Both，舵机命令 Endpoint 要求 E2E；某个网关可以只转发 opaque E2E，完全不拥有业务密钥。

## Provider 生命周期

初始化推荐顺序：

```text
加载/验证设备身份和密钥
  → load durable next_sequence
  → get non-zero session_id
  → 安装 ops/context
  → 安装 Node/Endpoint policy
  → 若产品要求安全，set_security_required(true)
  → 检查 ucn_node_security_ready()
  → 才开放 Link/业务流量
```

`security_ready` 只验证回调/策略/持久状态合同达到代码要求，不证明密码算法经过审计。

## TX 调用链

```text
业务 Frame 草案
  → authorize_tx()
  → select_tx_protection()（AUTO 时）
  → 分配并持久化 next sequence/session 状态
  → 生成固定 30 B AAD
  → seal(plaintext → ciphertext + 16 B tag)
  → Core encode/queue/send
```

若 store sequence 失败，不能发送可能在重启后复用 nonce 的帧。Provider 的 `seal()` 必须在 output capacity 内完成，失败时不留下可发送的半成品。

## RX 与透明中继

中继只解码未加密路由头，检查网络、重复、TTL、Path/Route 和 forward policy，然后原样转发 ciphertext/tag；它不调用 `open()`。

最终目标执行：

```text
authorize_rx(ingress, protected frame)
  → Session/Sequence replay admission
  → 生成同一 AAD
  → open() 验证 tag 并解密
  → Endpoint/Service handler
```

`open()` 返回 `UCN_OK` 前必须完成认证；认证失败时 plaintext output 不可被应用读取。Replay window 应在认证成功后按产品原子规则推进，避免伪造包消耗合法序列。

## Session 轮换

Sequence 达到阈值前调用 `rotate_session()`。Provider 必须先原子持久化/配置新 key、非零且不同的 session ID 和较小 next sequence，再返回成功。仅把 RAM sequence 清零会导致 nonce 重用。

Plain 模式也应通过 `ucn_node_set_plain_session_id()` 使用重启变化的 session，使 duplicate/replay 状态不把新启动与旧启动混为一谈。

## 安装示意

```c
static const ucn_security_ops_t sec_ops = {
    .load_next_sequence = product_load_seq,
    .store_next_sequence = product_store_seq,
    .get_session_id = product_get_session,
    .authorize_tx = product_authorize_tx,
    .authorize_rx = product_authorize_rx,
    .select_tx_protection = product_select,
    .seal = product_aead_seal,
    .open = product_aead_open,
    .rotate_session = product_rotate,
};

ucn_node_set_security(&node, &sec_ops, &security_context);
ucn_node_set_security_policy(&node, &default_policy);
ucn_node_set_endpoint_security_policy(&node, COMMAND_EP, &command_policy);
ucn_node_set_security_required(&node, true);
```

每一步都要检查返回值；required gate 未 ready 时不要把错误降级成明文。

## 生产缺口与验收

产品还必须解决 key provisioning、secure boot/固件签名、密钥撤销、RNG 健康、侧信道、调试口、Flash 防读、审计日志和多设备信任域。测试至少覆盖篡改每个 AAD 字段/tag/ciphertext、重复/乱序/重启、存储失败、轮换掉电、无 Provider 和 Endpoint policy 冲突。
