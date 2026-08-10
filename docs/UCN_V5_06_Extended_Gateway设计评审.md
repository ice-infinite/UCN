# UCN V5-06 Extended Gateway 设计评审

> 日期：2026-08-11  
> 决议：架构可行，但不进入 v5 小 Core；后续以独立 `ucn_gateway_ext` 组件实现，并以前置安全与资源门禁为条件。

## 1. 要解决的问题

W0 的单播 Node ID 只有 1～254。它适合一个小型本地域，但不能靠截断地址容纳成千上万节点。Extended Gateway 的目标是让多个短地址域互联，同时保持：

- 普通 MCU 域内仍运行现有固定内存 UCN Core；
- Linux 只是可选 Gateway/管理端，不是组网前提；
- 跨域地址、Session 和安全映射有明确租约、代际、表满和恢复语义；
- 普通中继无需解密端到端业务，也不能改写受认证字段后复用旧 Tag。

本 RFC 不实现互联网路由、云目录、无限节点、动态内存或任意格式字段。它也不把一个 W0 地址宣传成全局唯一身份。

## 2. 身份与地址分层

跨域时必须区分四个值：

| 名称 | 作用 | 生命周期 |
| --- | --- | --- |
| Global Identity | 设备/服务的稳定全局身份，建议 128 bit | 设备生命周期，由身份系统签发 |
| Domain ID | 标识一个 UCN 本地域，32 bit | 网络部署生命周期 |
| Local Node ID | W0～W3 线上短地址 | 当前域内租约期 |
| Alias Generation | 防止旧短地址租约复用后误投 | 每次分配单调增加 |

规范键为 `(Global Identity, Endpoint)`；域内转发键仍是 `(Network ID, Local Node ID)`。Gateway 不得把 Local Node ID 当作跨域身份，也不得在缺少 Generation/Lease 的情况下缓存 Alias。

## 3. 独立组件边界

```text
Application / Service Router
          |
Canonical Cross-Domain API
          |
ucn_gateway_ext
  Directory | Alias Lease | Session Map | Outer/Inner Envelope
          |
existing ucn_node / Adapter / Link
```

`ucn_gateway_ext` 持有自己的固定表和工作缓冲；`ucn_node_t` 不增加 Alias/Directory/Session Slot 表。Nano 默认不编译 Gateway；Lite/Full 只有显式 Feature 开关才链接该组件。Linux Host Adapter 与高资源 MCU 使用同一 C API。

## 4. 两种 Gateway 模式

### 4.1 安全终止模式（第一实现候选）

```text
Domain A 解密/认证 → ACL/Endpoint 检查 → 地址解析
→ Domain B 重新编码 → 使用 Domain B Key/Session 加密
```

优点是普通终端不需要新 Inner Envelope，容易在 MCU Gateway 落地。代价是 Gateway 可见明文，必须进入信任边界并具备安全存储、审计和密钥轮换。

### 4.2 透明 E2E 模式（后续候选）

```text
Outer Wire Envelope（逐跳可读、每域可重封装）
  Domain / Local Alias / Hop / Outer Session / Outer Auth

Inner Canonical Envelope（Gateway 不可改写）
  Global Source / Global Destination / Endpoint
  Sequence / E2E Session / Length / Ciphertext / E2E Tag
```

普通 v5 受保护帧不能直接由 W0 改成 W3并保留旧 Tag，因为 Profile 和规范地址已在 AAD 中。透明模式必须新增明确的 Inner 版本和独立 E2E AAD。未支持 Inner Envelope 的小节点只能使用安全终止模式或停留在域内。

## 5. 固定容量数据结构

建议后续实现采用编译期上限，不使用 malloc：

| 表 | 主键 | 必需字段 | 表满行为 |
| --- | --- | --- | --- |
| Alias Lease | Domain + Local ID | Global ID、Generation、到期、状态 | 拒绝新租约，不淘汰活动项 |
| Directory | Global ID | Home Domain、Gateway、Metric、到期 | 返回不可达，不做广播扩散 |
| Session Map | Global ID + E2E Session | Local Slot、Key Epoch、Replay 状态、到期 | 拒绝新安全会话 |
| Pending Resolve | Request ID | 目标、来源、deadline、回调 | 合并同目标；满时失败 |
| Gateway Route | Domain ID | Primary/Backup、Cost、状态 | 只保留固定工作集 |

建议对象预算公式而不是虚构单一节点上限：

```text
RAM_gateway = base
            + alias_count   * sizeof(alias_entry)
            + directory_count * sizeof(directory_entry)
            + session_count * sizeof(session_entry)
            + pending_count * sizeof(pending_entry)
            + fixed envelope buffers
```

初步 C99 结构估算每个 Alias/Directory/Session 项约 24～48 B。若各 32/32/16 项，仅表项大约 2.0～3.5 KiB；加 Replay Window、双缓冲和安全 Provider 后会更高。最终数值必须由实际结构 `sizeof`、目标 ELF 和栈高水位给出，不能把这一区间写成产品规格。

## 6. Alias 租约状态机

```text
EMPTY → OFFERED → ACTIVE → RENEWING → EXPIRED → QUARANTINE → EMPTY
                    |          |
                    +→ REVOKED-+
```

- 分配时生成非零 Generation，返回 `(Local ID, Generation, Lease)`。
- 业务映射必须同时匹配 Generation；只匹配 Local ID 的旧包拒绝。
- 到期先进入 Quarantine，至少等待最大包寿命、重放窗口和路由缓存寿命后才复用。
- 重启后若无安全持久化租约，Gateway 不得假装恢复旧 Generation；应重新 Join/Resolve。
- 活动表满不允许 LRU 淘汰，避免把仍在通信的节点映射给新身份。

## 7. Session Slot 与 Replay

Session Slot 不是简单把 32 bit Session 截成 8 bit。每项至少绑定：Global Identity、完整 Session/Key Epoch、Local Slot、方向、Replay Window、Lease 和撤销状态。

- Slot 复用必须先撤销并等待 Quarantine。
- Gateway 重启不能让 Sequence/Session 回退；需要安全持久单调状态或重新建立 Session。
- E2E Session 与 Outer Link Session 分离；逐跳重封装不得改变 Inner E2E Session。
- 表满返回安全错误，不退回明文、不复用随机旧 Slot。

在 S02 的身份、审计 AEAD、持久计数器与密钥生命周期完成前，不实施生产 Session Slot。

## 8. Directory 与寻路

Directory 只回答“全局目标在哪个 Domain/Gateway”，域内下一跳仍由现有 UCN Route/Path 决定：

```text
Global Destination
  → Directory 命中 Home Domain / Gateway
  → Gateway Route 选择 Primary/Backup
  → 当前域 UCN Route
  → Outer Envelope 发往出口
```

Directory Miss 使用固定 Pending Resolve 合并同目标请求并设置 deadline；禁止全网无限 Flood。正缓存和负缓存都必须到期，撤销消息需要认证。Cost 只用于 Gateway Route，不与域内 Link Cost 直接相加，除非以后冻结单位和归一化规则。

## 9. 双 Gateway 一致性

MCU-first 默认采用 Primary/Standby 或预分配不重叠 Alias 范围，不在小设备中实现分布式共识：

- Primary 签发租约和 Generation，Standby 只复制已认证日志；
- 网络分区时 Standby 默认不分配新 Alias；若产品要求继续分配，必须使用预先不重叠的地址段和独立 Epoch；
- 恢复合并时以 `(Authority Epoch, Generation)` 判定新旧，冲突项全部撤销并重新 Join，不能“猜一个赢家”；
- 两个 Gateway 同时宣告 Primary 视为安全故障，停止新租约但保持可验证的已有业务到期。

Linux 可承担管理与日志角色，但相同的静态 Primary/Standby 逻辑也可由两个高资源 MCU 实现。

## 10. MTU 与分片边界

Canonical Inner Envelope 会增加额外头和 Tag。Gateway 必须在封装前计算 Outer Header + Inner Envelope + Payload + Tag 是否适合选中 Link MTU。现有小 Core没有通用分片；超 MTU 必须返回 `UCN_ERR_TOO_LARGE`。

若以后需要分片，应作为独立可靠性/DoS 任务，定义 Fragment ID、总长、固定重组槽、deadline、重复片、认证顺序和表满行为。未完成前禁止在 Gateway 内偷偷切片。

## 11. 威胁模型与失败关闭

必须覆盖：伪造 Directory、Alias 抢占、旧 Generation 重放、Session Slot 混淆、Gateway 降级为明文、双 Primary、租约表耗尽、Resolve Flood、Outer Hop 篡改、Inner Tag 篡改、持久计数器回退和密钥撤销失联。

共同规则：

- CRC 只检错，Directory/Alias/控制消息必须认证；
- ACL 在 Global Identity + Endpoint 上执行，不能只看 Local Alias；
- 安全终止 Gateway 必须记录身份、规则版本和重加密结果，不记录明文 Payload；
- 任何认证、容量、版本、MTU 或持久状态错误都显式失败，不降级到无认证转发。

## 12. 迁移顺序与验收

1. 完成 S02 生产身份、AEAD、持久 Counter/Replay 与逐跳认证。
2. 建立独立 `ucn_gateway_ext` 头/源和 Feature 开关，保持 Core ABI/行为可单独构建。
3. 先实现静态 Directory + 安全终止 Gateway，不实现动态 Alias。
4. 加入固定 Alias Lease/Generation/Quarantine 和断电恢复测试。
5. 加入 Session Map，再做密钥轮换、表满和 Replay 故障注入。
6. 只有终端确有透明跨域需求时才实现 Canonical Inner Envelope。
7. 最后评审双 Gateway 与可选分片；没有真实需求则不增加复杂度。

验收至少包括两域/三域、多 Gateway、冲突/分区/重启/表满、旧租约重放、MTU、密钥轮换、持续高负载、目标 MCU RAM/Flash/栈和长时间实机。Host 模拟不能替代这些门禁。

## 13. 最终决议

V5-06 通过“独立 Extended 组件”的架构方向，但当前不写 Gateway 运行代码，原因不是不可行，而是它依赖尚未完成的 S02 生产安全，并且 Alias/Session/Directory 会显著扩大普通 MCU Core 的资源与故障面。

当前 v5 发布继续定位为：一个 Wire Domain 内的 MCU-first 自组网与统一传输。跨域 Gateway 是明确的后续扩展，不影响无 Linux 的域内独立运行。
