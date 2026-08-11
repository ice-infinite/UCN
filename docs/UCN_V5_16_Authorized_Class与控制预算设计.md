# UCN V5-16 Authorized Class 与控制预算设计

> 状态：设计冻结；尚未实现生产身份、逐跳认证或密钥生命周期。
> 前置依赖：S02。Wire Profile 测试通过不代表本设计已经成为安全能力。
> 当前源码核对：截至 `codex/v5-adaptive-wire@f941ae9` 仍没有 C0～C3 执行层；保持阻塞是有意的安全边界，不是文档遗漏。

## 1. 为什么不能让 Wire Profile 代表权限

W0～W3 只回答“这一帧使用多宽的 Network/Node/Session/Path/Cost 字段”。它由发送端写入帧头，任何未认证设备都能自报 W3，所以它不能证明身份，也不能自动取得转发、诊断、Path 安装或 Gateway 权限。

必须同时保留三条互相正交的属性：

| 属性 | 来源 | 用途 |
| --- | --- | --- |
| Wire Profile | 当前帧编码 | 决定字段宽度与解析方式 |
| RX Ceiling | HELLO 能力声明和本地 Link 配置 | 判断双方是否能承载某种编码 |
| Authorized Class | 认证身份、产品 ACL 和当前 Session Generation | 决定允许执行的操作、预算和 Fanout |

因此，C0 身份发送 W3 帧仍然只是 C0；C3 身份发送 W0 帧也不会降成 C0。权限判断不得读取帧自报 Profile 后直接提升。

## 2. 建议的授权等级

等级是产品侧最大授权上限，不替代逐条 ACL。高等级也不自动绕过目标 Node 的 Authorizer。

| Class | 建议能力上限 | 默认禁止 |
| --- | --- | --- |
| C0 Participant | 已授权 Endpoint 数据、受限单播响应 | RREQ 泛洪、远端诊断、Path 写入、Gateway |
| C1 Edge Relay | C0 + 普通数据中继 + 受限 RREQ/RREP/RERR | Path 管理、全网 Snapshot、跨域 Gateway |
| C2 Mesh Operator | C1 + 经 ACL 允许的 Path/Trace/Snapshot，较高但仍固定的控制预算 | 密钥管理、身份签发、跨域目录管理 |
| C3 Backbone/Gateway | C2 + 经独立 ACL 允许的 Gateway/目录操作 | 任何未明确列入 ACL 的管理动作 |

产品可以把某个能力继续关闭。例如 C2 只是“最多可以申请 Path Install”，最终仍要同时通过消息类型 ACL、目标 Node Authorizer、Session 状态和令牌预算。

## 3. 状态绑定位置

Authorized Class 应绑定到已经认证的逻辑身份，而不是物理 MAC、临时 Link ID 或某一帧：

```text
Authenticated Principal
  ├─ Identity ID / Key ID
  ├─ Session ID + Generation
  ├─ Authorized Class
  ├─ Message/Endpoint ACL
  └─ Control Budget Profile
          ↓
Admitted Neighbor
  ├─ one or more Bearers
  └─ current authenticated generation
```

同一 Neighbor 的 WiFi、UART、CAN Bearer 可以共享身份授权，但每个 Bearer 仍保留自己的 Link 状态、RX Ceiling 和介质预算。未经认证的新 Bearer不能只凭相同 Node ID 继承旧 Bearer 的权限。

## 4. 接收处理顺序

生产实现必须按以下顺序失败关闭：

```text
Frame 长度/Profile/CRC 校验
  → Link 与已准入 Neighbor 绑定
  → S02 逐跳身份/完整性验证
  → Session Generation 与吊销状态检查
  → 查 Authorized Class 和消息 ACL
  → 查 Source/消息类型固定 Token Budget
  → 查 Fanout/表容量
  → 执行 RREQ、Path、Trace、Snapshot 或业务分发
```

权限失败不得先消耗 Path/Route/Reverse/Reply 表项；预算失败不得提前提交 RREQ Best Cost 或修改授权状态。现有 Path Authorizer 和控制令牌桶继续保留，未来只是把“已认证身份/Class”作为它们的可信输入。

## 5. 预算与 Fanout

每个 Class 对应产品可覆盖、编译期有上限的预算模板：

- 按 `(Identity, Session Generation, Control Type)` 分桶，避免一个来源耗尽全部控制资源。
- RREQ、Trace、Snapshot、Path Install/Revoke 分开计数，不能共用一个无限额度。
- Fanout 是每次控制操作允许发送到的最大健康 Link 数；仍受 Hop Limit、去重表和全局固定队列限制。
- C3 也必须有固定上限，禁止“管理员无限泛洪”。
- 表满时拒绝新来源；不能挤掉仍有效的更高优先级业务状态。

具体 Token、Refill 和 Fanout 数值必须由 S06 的目标 MCU/介质日志标定，本设计不虚构通用默认值。

## 6. 升级、降级和失效

- **首次加入**：认证完成前是 Untrusted，只允许产品定义的最小 Join/HELLO 流程。
- **升级**：只接受经过 S02 验证的新授权记录；帧自报 W2/W3、Node ID 变大或更换介质都不能升级。
- **降级/吊销**：立即阻止新的受限操作；清理该身份拥有的 Pending 控制状态。已安装 Path 是否立即撤销由产品安全策略决定，但必须有确定、可测试的规则。
- **Session 轮换**：新 Generation 重新绑定授权；旧 Generation 的预算、Replay Window 和管理 Pending 不得泄漏到新 Session。
- **重启**：没有持久化且验证通过的授权记录时回到 Untrusted，不能从 RAM 残留恢复 C2/C3。
- **多 Bearer**：新 Bearer 完成身份绑定后才能并入同一 Neighbor；仅相同 Node ID 不足以继承权限。

## 7. 与 Gateway 的关系

C3 只是允许产品启用 Gateway 能力的必要条件，不是充分条件。W0 多扁平域的 Alias、Directory、Session Slot 和外层/内层安全信封仍属于独立 `ucn_gateway_ext`（V5-06），不能塞进 MCU 小 Core，也不能在 S02 前实现未认证地址改写。

## 8. 后续实现门禁

S02 完成后，至少新增以下测试才允许把 V5-16 从“设计”改成“代码完成”：

1. C0 发送 W3 Path Install 仍被拒绝，C2 发送 W0 请求按 ACL 正常处理。
2. 未认证、错误 Key、旧 Session、已吊销 Generation 全部默认拒绝且不写表。
3. Class 升级必须有签名授权；自报 Profile、Node ID 或更换 Bearer不能升级。
4. Session 轮换、重启、授权降级和预算表满均不继承旧权限或旧 Token。
5. 不同身份/控制类型预算隔离；C3 也受固定 Fanout 和全局容量约束。
6. Path/Trace/Snapshot 继续同时通过现有目标 Authorizer，授权检查发生在资源分配前。
7. Nano/Lite/Full 的关闭能力仍返回 Config/Unsupported，不因高 Class 获得未编译 Feature。

在这些门禁完成前，对外只能表述为“UCN 已把 Wire 编码与未来身份授权分层”，不能表述为“Wire Class 已实现安全权限”。
