# Session、Sequence、持久化与重放窗口

> 文档级别：`NORMATIVE`
> 实现状态：接口和 RAM Window `CURRENT`；生产持久化由产品实现
> 最近核对：`a093862`，2026-08-25

## 发送域

安全 nonce/重放域至少绑定 Source、Session 和 Sequence。Sequence 必须在同一 Session 内单调，Session 轮换后不得重用旧 Key/nonce 组合。

## 持久化顺序

产品 Provider 必须保证下一可用 Sequence 在发送前或按安全批次策略原子持久化。掉电后恢复值不能小于已经可能上线的序号。

当序号接近轮换阈值：

1. 停止在旧 Session 接受新业务；
2. 原子 provision/persist 新 Session/Key/next Sequence；
3. 返回非零且不同的新 Session；
4. 重新进入 Ready；
5. 旧 Session 按产品窗口淘汰。

轮换失败必须停止安全发送，不能回绕到 0。

## 接收窗口

Core 的 `(Source, Session)` 位图 Window 处理近期乱序和重复。超出窗口的旧 Sequence 拒绝，窗口内重复拒绝。

固定 Window 会回收旧 Source，因此生产 Provider/身份层还应按威胁模型管理长期重放、重启和 Session 过期。

## 明文 Session

明文网络也可配置 plain Session 以避免节点重启后短期重复缓存混淆，但它不提供密码认证。安全要求不能因有 Session ID 就视为满足。

## Nonce 唯一性必须由具体 AEAD 方案冻结

UCN 提供 Source/Session/Sequence 身份，但 Provider 必须定义如何从它们和 Key 构造 nonce，并证明同一 Key 下永不重复。不能直接把 32-bit Sequence 当完整 nonce 而忽略 Source/Session/方向。

若 TX/RX 共用 Key，还需域分离，防止同一身份在相反方向产生 nonce 碰撞。具体构造应进入产品安全规范和 golden vectors。

## 预留批次的安全持久化

每帧都擦写 Flash 成本高。可使用批次预留：

1. 持久化“下一批上界”例如 2048；
2. RAM 只使用 `[1024,2048)`；
3. 用尽前先原子持久化 3072；
4. 掉电后从已持久上界 2048 开始，宁可跳过未用序号，也绝不复用；
5. 撕裂写恢复到旧完整记录时，旧记录必须仍大于所有可能已上线序号，否则方案不安全。

双槽、CRC 和 generation 只能发现撕裂；防恶意回滚还需要受保护 monotonic storage/secure element 或产品信任机制。

## 启动状态机

```text
Provider load Session/next Sequence
  ├─ 空厂状态：安全provision并持久化
  ├─ 完整Ready：验证合法域，建立Node
  ├─ pending/迁移：按冻结恢复合同处理
  └─ 损坏/回滚：fail-closed，不猜值
```

Node 在 Provider 未 Ready 前不能发送 required-protected 业务，也不能临时用 plain Session 继续。

## 接收窗口与认证顺序

攻击者可伪造超大 Sequence。如果接收端先推进 Window 再验证 Tag，合法后续帧会被锁死。因此受保护帧必须在认证成功后才把 Sequence 作为可信接受提交。

为了抗 DoS，可以在 open 前做不写状态的粗窗口检查，但最终 commit 必须认证后原子执行，且并发/重入由唯一 Owner 序列化。

## Session 轮换的双端问题

发送端切新 Session 后，接收端第一次看到 `(Source,new Session)` 建新 Window。产品需定义旧 Session 接受多久：立即拒绝、短 overlap 或根据 Key generation。Overlap 太长增加重放面，立即切换则要求控制面/密钥分发可靠。

轮换消息本身也必须认证和防重放。`rotate_session()` 只是本地 Provider 原子合同，不自动完成多节点 Key Distribution。

## Node ID 变化

Node ID 可配置/重分配时，身份、Key、Session/Sequence 域要明确迁移。不能只改地址而继续使用可能与另一个历史 Node 相同的 nonce/授权。管理系统应维护不可变设备身份→当前 Node ID 的绑定和冲突处理。

## 长期业务重放

RAM Window 只覆盖近期 Frame。一个月前的“开舵机”命令可以被重新封装成新 Frame，网络层无法识别其业务重复。Command ID/generation、有效期和任务状态机必须提供更长业务防重放。

## 验证清单

- [ ] 同 Key 下 nonce 唯一性有形式/测试依据；
- [ ] 每帧或批次预留掉电后不复用；
- [ ] 双槽撕裂、旧槽回退和完全损坏均测试；
- [ ] 认证失败/伪造大 Sequence 不推进 Window；
- [ ] Session 轮换失败停止发送；
- [ ] 新旧 Session overlap/淘汰规则冻结；
- [ ] Node ID 重分配不复用旧安全域；
- [ ] 业务 Command ID 与 Frame replay window 分层。
