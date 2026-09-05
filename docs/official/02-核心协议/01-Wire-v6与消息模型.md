# Wire v6 与消息模型

## 1. 单一 Wire

v6 不提供 v4/v5 decoder、encoder 或协商降级。接收器先检查 Magic、Version、Address Class、
Flags、Header/Extension 长度、保留位和总长度；任一条件错误时返回失败且不写输出。旧帧仍应
作为负向 fixture 被拒绝，但旧 codec 不属于发布产物。

A0～A3 是地址编码档，不是节点权限和算力等级。四档共享同一 canonical semantic frame；
所有 Profile 都必须解析四档。当前基础长度分别为 `40/42/44/46 B`，之后再叠加已声明的
扩展、Payload 与 Tag。应用仍应通过 Wire 长度 API 计算，不能把该表或历史 32 B/40 B 写死
到 Driver。

## 2. 可见字段与扩展

公共头携带版本、Flags、地址类、Frame Type、`origin_sequence`、`hop_sequence` 和长度。
`origin_sequence` 由 E2E/Group 原始发送安全上下文分配，进入 E2E canonical AAD，中继不改；
`hop_sequence` 由当前 Peer Session 分配，每一跳重新生成并用于该跳 Replay。Route Context、
Path、Hop Scheduling
Budget、Endpoint、Message 与安全 selector 只在相应 Flag/Type 要求时出现。解析器按规范
顺序读取并拒绝未知组合，不能靠结构体直接映射 Wire。

Hop Security Context 与 Group Context 互斥；E2E Context 独立存在。Security Suite、Key
ID/Generation、Binding Generation、Session Generation、Protocol Opcode 和 Route Context
进入相应认证域，防止中继或重放者把同一 Payload 改解释为另一操作。

## 3. CRC 与认证

CRC32C 用于发现传输损坏，不代替认证。Hop Tag 保护每跳可变和逐跳消费字段；E2E Tag 保护
端到端不可变字段和 Payload。中继必须先验证原始入站 Hop Tag/Replay，再递减 Hop Budget、
分配新的 Hop Sequence 并为下一跳重新计算 Hop Tag；不得修改被 E2E AAD 绑定的 Principal、
Origin Sequence、Endpoint、Operation、Delivery、密文或 E2E Tag。

若 Endpoint ACL 依据原始 Device Principal 授权，则消息必须通过 E2E Auth；不需要保密时
使用 auth-only suite，而不是取消认证。Group HELLO 使用独立 Group selector、保留组播地址
和受限权限，只能产生有界提示，不能直接获得 Peer Session 或业务 Authority。

## 4. 消息语义

一条消息分别声明：

- `traffic_class`：Q0～Q3 调度类别；
- `delivery_guarantee`：Datagram、Latest、Reliable、Durable At-Most-Once 等传输/执行保证；
- `interaction_role`：Event、Request、Result 等业务关系；
- `operation_id`：跨重试和重启识别同一业务操作；
- `protocol_opcode`：Control/Transfer 子协议内的精确操作。

这些维度互不推导：Q0 不自动可靠，Reliable Request 也不会自动获得高优先级。校验器先拒绝
非法枚举、零值和不允许组合，再允许进入 Journal、Transfer 或发送路径。

## 5. Durable At-Most-Once

Journal 使用 PREPARED、EXECUTING、COMMITTED_RESULT、IN_DOUBT、TOMBSTONED 等显式状态。
外部执行已经发生但结果未能持久化就掉电时，只能返回 IN_DOUBT；除非外部执行器支持按
Operation ID 查询对账或与 Journal 原子提交，否则不能承诺重放结果，也不能再次执行。

表满时拒绝新操作；只有符合保留期和确认条件的 Tombstone 可回收。相同 Operation ID 只有
在 Principal、Endpoint、Opcode 和请求摘要一致时才是幂等重放，任何错绑都必须拒绝。
