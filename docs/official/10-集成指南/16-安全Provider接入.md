# 安全 Provider 接入

> 文档级别：`GUIDE`
> 实现状态：`PARTIAL（通用合同已实现，产品 BSP/SDK 需接入）`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：公共 API、Port/Adapter/Source 实现与测试
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：部分 ESP32-S3 UART/ESP-NOW；其他平台按正文标注

产品 Provider 至少实现：身份/密钥查找、AEAD seal/open、AAD 校验、Session/Sequence 持久化、replay window、Endpoint authorize 和安全错误审计。

密钥不得硬编码在公共源码或日志中。随机数来自硬件/审计 DRBG；Flash 中的密钥和序列状态使用访问保护与原子更新。

先用已知向量和掉电测试验证 Provider，再允许无线/外部链路接受受保护业务。测试 Provider 不得进入发布固件。

## 威胁与密钥模型

产品先明确攻击者能否监听、注入、控制中继、读取Flash或获得某个合法节点。决定是全网共享key、每节点pairwise key、group key还是PKI；UCN Provider接口不替产品做这个选择。

至少区分：设备身份密钥、会话AEAD key、固件签名key、调试/维护凭据。不要让一个泄漏的业务key同时能签固件。

## Provider上下文

```c
typedef struct product_security {
    key_handle_t identity;
    key_handle_t session_key;
    uint32_t session_id;
    uint32_t next_sequence;
    replay_table_t replay;
    secure_store_t *store;
} product_security_t;
```

上下文持有key handle而非在普通RAM长期保存明文key更好。具体Secure Element/Flash方案由硬件决定。

## 启动流程

1. 验证secure boot和配置完整性；
2. 读取两槽sequence/session记录，拒绝CRC/generation冲突；
3. 若首次启动，使用审计RNG创建身份/会话；
4. 安装Provider ops；
5. 安装Node与Endpoint policy；
6. 设置required并检查ready；
7. 才启用无线RX和发送业务。

任何一步失败都保持Link离网或只允许受限恢复/诊断。

## Sequence持久化策略

逐帧Flash写磨损大，可预留sequence区间：先原子持久化“已保留到N”，RAM只使用该区间；掉电后跳过未用值，不复用nonce。区间大小是磨损与浪费的权衡，必须证明最大寿命和no-wrap轮换。

## AEAD合同

使用固定30 B AAD和16 B tag；nonce构造必须唯一绑定session/sequence/方向或key domain。`seal/open`不能修改Frame identity；open失败时output保持不可用。算法、key/tag长度和nonce规则写入产品安全规范并用已知向量验证。

## ACL与命令

`authorize_tx/rx`检查Node/Endpoint/Role/Link/产品状态。远端Q0 Service还要业务validator和Command Guard。中继只允许opaque forwarding不代表它能绕过终点ACL。

## 轮换与撤销

达到sequence阈值、key到期或设备撤销时建立新session/key并先持久化，再切换发送。网络应允许一段受控双key接收窗口或明确同步流程；撤销旧key后Replay状态不可让旧session重新进入。

## 测试

- 官方/产品AEAD向量；
- AAD每字段、ciphertext和tag单bit篡改；
- replay、乱序、跨Endpoint/Path替换；
- sequence存储失败、每个掉电点、磨损/坏块；
- session轮换前后、回滚旧固件；
- key缺失/RNG失败/debug解锁；
- 性能：每包CPU、stack、延迟和功耗。

完成这些之前只能称“安全接口已接入”，不能称“产品安全已完成”。
