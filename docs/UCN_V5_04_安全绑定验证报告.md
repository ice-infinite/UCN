# UCN V5-04 安全绑定验证报告

> 日期：2026-08-11  
> 范围：Core/AAD/透明转发契约的软件验证；不宣称测试 Provider 是生产密码。

## 已冻结的同域规则

v5 E2E AAD 绑定 Version/Profile、Message、Traffic/不可变 Flags、Network、Source、Destination、Sequence、Session、Payload Length 和 Path ID。Hop Limit 与 Route Epoch 允许中继修改，因此必须由未来的逐跳 Link 认证保护。

受保护帧在普通中继上不得改变 Wire Profile、规范地址、Session、Sequence、Length、Path ID、Ciphertext 或 Tag。需要 W0↔W3 转换时，只允许：

1. 安全终止 Gateway 解密、授权、重新编码与重新加密；或
2. 未来 Extended 的可重封装 Outer Wire Envelope 包住不可改写的 Canonical Inner E2E Envelope。

当前小 Core 没有实现第二种双层信封，因而不会把普通 Profile 改写冒充透明 Gateway。

## 专项软件证据

- W0 A→B→C：A/C 安装可验证 AAD 的测试 Provider，B 只安装授权/透明转发 Provider。连续两帧到达 C，B 的 `open_calls=0`，透明密文转发计数为 2。
- W0→W1 Profile 重编码后复用旧 Tag：Open 返回 `UCN_ERR_SECURITY`。
- 修改 Destination 或 Payload Length 后复用旧 Tag：Open 返回 `UCN_ERR_SECURITY`。
- 修改 Ciphertext 并重新编码 CRC：目标 Open 返回 `UCN_ERR_SECURITY`；未重算 CRC 的线上篡改先由 Decoder 返回 CRC 错误。
- Provider/明文策略失败关闭、生产 Security Ready 门禁和旧版本拒绝继续通过。

Windows Full Debug/Service ON CTest 为 2/2。测试 Provider 是可逆的确定性 Fixture，只证明 Core 合约；生产身份、AEAD、密钥持久化/轮换、Replay Window 和逐跳控制面认证继续由 S02 验收。
