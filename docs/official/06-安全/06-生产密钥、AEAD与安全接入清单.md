# 生产密钥、AEAD 与安全接入清单

> 文档级别：`NORMATIVE CHECKLIST`
> 实现状态：`PRODUCT REQUIRED`
> 最近核对：`a093862`，2026-08-25

发布前至少完成：

## 身份与密钥

- 每个 Node 有不可伪造的产品身份；
- Node ID 分配与身份绑定有冲突/更换流程；
- 使用经过评审的 AEAD 库，不自制密码算法；
- Key/Session 分域、版本、轮换、吊销和恢复规则明确；
- Flash/NVS 受回滚、读取和撕裂写保护；
- 开发测试密钥不会进入生产。

## Provider

- `seal/open` 正确使用 UCN AAD 和固定 16 B Tag；
- Sequence 在掉电和并发下不重复；
- `rotate_session` 原子且失败关闭；
- TX/RX authorize 覆盖管理、控制和普通业务；
- Provider 回调不可重入破坏 Node/Cluster Owner；
- 错误不会自动降级明文。

## 业务

- 高风险命令有 Validator、Command ID、有效期和幂等；
- 任务执行失败有显式 Result；
- 安全失效进入机械/控制安全状态；
- OTA/参数写入另有签名、版本和防回滚。

## 测试

- 篡改 Header/AAD/Payload/Tag 全部拒绝；
- 重放、乱序、Session 更换、掉电恢复和计数器撕裂写；
- 未授权 Source/Endpoint/诊断/Cluster 控制；
- 表满、Queue 满、Link Down 和 Provider I/O 故障；
- 真实 MCU 时序、RAM、CPU、功耗和密钥生命周期。

完成清单仍不等同外部密码审计或产品安全认证，应按产品风险等级执行独立评审。

## 1. 先形成安全架构文件

发布前不应只留一张勾选表。至少产出：威胁模型、身份/Key hierarchy、nonce 构造、持久化格式、轮换/吊销、制造注入、维修/恢复、日志/告警和测试证据索引。每项绑定代码版本与硬件型号。

## 2. Key hierarchy

不要让所有设备永久共用一个硬编码 Key。产品应区分制造根、设备身份、网络/组 Key、端到端业务 Key、固件签名 Key，并定义泄露一个节点后的影响范围。Key derivation 要有 context/domain separation，测试 Key 与生产 Key 的构建/注入路径物理隔离。

## 3. AEAD 选型与实现

选择平台有成熟实现且适合 MCU 的 AEAD，核对 Key/nonce/tag 长度、硬件加速、常量时间、错误返回和许可。Provider 使用固定 16 B UCN Tag 时，所选算法/库必须匹配；不能截短一个不同合同的 Tag 而无安全评估。

Golden vector 覆盖 AAD、plaintext、ciphertext、tag、nonce；篡改任一 bit 必须失败且 plaintext output 不写回/不交业务。

## 4. 持久化与掉电

在真实 Flash/NVS 上做：每个写阶段断电、双槽恢复、CRC/generation、旧镜像回滚、写寿命、掉电电压斜率、并发 reset。软件模拟的 Provider PENDING/FAILED 不能代替这些物理测试。

Sequence 批次预留、Session/Key commit、Cluster promise 都要遵守 persist-before-use/promise，且 Provider callback 重入被 Fence。

## 5. 制造与运维

- 每台设备身份如何注入并审计；
- RMA/维修是否可读取 Key；
- Debug/JTAG/UART boot 如何锁定；
- Key 吊销列表如何分发；
- Node ID 变化如何重新绑定；
- 丢失所有持久状态如何安全恢复；
- 日志不输出 Key、plaintext 或认证材料；
- 时间/证书过期如何在无 RTC MCU 上处理。

## 6. 故障安全

安全模块失败时，舵机/电机/电源系统进入产品定义安全状态，而不是只返回一个错误码。需要明确：停止接受远端命令、保持还是回中、允许哪些本地手动控制、如何提示和恢复。

## 7. 测试层级

1. 算法/library 官方 vector；
2. Provider unit + negative + fuzz；
3. Node/Endpoint/Service 集成；
4. 掉电/回滚/轮换；
5. 多跳 opaque forwarding 和恶意中继；
6. 无线/有线注入、重放、DoS；
7. 固件/调试/物理攻击面；
8. 独立安全审计与产品认证（若需要）。

## 8. 签字角色

协议维护者、产品固件、硬件/制造、安全评审和系统安全负责人分别签字。不能由“测试全绿”的同一个开发者单独宣称密码系统生产安全。

## 9. 发布输出

- 支持的安全模式和禁用的不安全兼容模式；
- Key/Session/Sequence 生命周期；
- 哪些 Endpoint/管理/Cluster 功能强制保护；
- 已测 MCU/Flash/SDK/拓扑；
- 残余风险和运维响应；
- 外部审计/认证版本；
- 回滚和紧急吊销步骤。

缺少任一关键闭环时，版本状态应保持“可集成安全 Provider / 生产安全未完成”。
