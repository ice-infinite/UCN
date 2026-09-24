# UCN V6 简化版 IMPL-06 Reliable 与 Transfer 离线模型实施及全体自审报告

## 1. 结论

`IMPL-06 = OFFLINE PRIVATE MODEL IMPLEMENTED / SELF-REVIEW PASS / AUDIT HOLD`。

本阶段在 C99、固定容量、零动态内存和单 Owner 写入约束下，完成了以下私有模型：

1. 单帧 Reliable：冻结发送字节、精确 ACK、绝对 Deadline、有界重试与接收侧 retained receipt；
2. Transfer：Setup、Fragment Prefix、SACK、固定 TX/RX 窗口、乱序重组、正文摘要验证、终态回执和显式 Abort；
3. C1 durable parent：Transfer ID 高水位只有在 Persistence Proof 精确通过后才推进并形成一次性 grant；
4. Foundation 集成：通过真实 Persistence Foundation 完成
   `prepare -> submit -> poll -> reload proof -> activate grant`。

这不是生产接线或协议发布。当前 target 不安装、不导出，也不进入 `UCN::simplified`。Flow Transfer
已经要求并冻结 typed Source/Destination Binding、Realm、Transport Policy、安全域和 Flow 指纹，
但这些事实仍由离线调用方提供，尚未消费 Coordinator 提供的“当前 Flow/Route/Capability/Security
Proof”。完整 Setup ACK、Abort、Terminal Receipt Wire Body、公共 Runtime、Adapter、Security、
物理 Bearer、真实 Flash 和 MCU 资源均保持开放。

## 2. 模块边界

| 对象 | 位置 | 当前职责 |
| --- | --- | --- |
| 私有类型与 API | `src/internal/ucn_transport.h` | 定义 fixed-capacity Owner、Reliable/Transfer/Parent DTO 与状态；不安装。 |
| 私有内部辅助 | `src/transport/ucn_transport_private.h` | 锁、Handle、精确查找与内部辅助；不跨模块发布。 |
| Reliable Owner | `src/transport/ucn_transport.c` | 单帧重试、ACK、RX receipt、维护和生命周期。 |
| Transfer Codec | `src/transport/ucn_transport_codec.c` | 已冻结 Setup、Fragment Prefix、SACK 和 Delivery ACK 编解码。 |
| Transfer Owner | `src/transport/ucn_transfer.c` | 固定窗口 TX/RX、分片、SACK、重组、摘要和终态回执。 |
| Durable Parent | `src/transport/ucn_transport_parent.c` | C1/Flow Parent Record、高水位、Requirement/Proof/Failure 与一次性 grant。 |

`ucn_v6s_transport` 只链接 `ucn_common`。它没有直接链接或持有 Route、Flow、Security、Admission、
Persistence、Adapter、Runtime 或 Coordinator Owner。跨 Owner 的 Requirement、Proof 与 Failure 都是
不可变 typed value；真实路由责任仍属于后续 Coordinator 接线。

## 3. Wire 子合同

### 3.1 已实现布局

| 对象 | 长度 | 说明 |
| --- | ---: | --- |
| Delivery ACK | 9 B | Service、Origin Sequence、状态、Credit；只允许冻结 subtype。 |
| One-way Setup | 43 B | Transfer identity、Parent、长度、分片、Deadline 和正文 Digest。 |
| Operation Setup | 52 B | One-way 基础上加入 Operation ID 与 Flags。 |
| Result Setup | 54 B | Operation 基础上加入 Result Code。 |
| C1 Fragment Prefix | 12 B | C1 Parent/Transfer identity、Fragment Index/Count。 |
| Flow Fragment Prefix | 8 B | Flow Label/Generation 与 Fragment Index/Count。 |
| C1 SACK | 20 B | C1 identity、累计/选择确认和 Credit。 |
| Flow SACK | 14 B | Flow identity、累计/选择确认和 Credit。 |
| Parent Record Body | 88 B | Parent context、Transfer high-water 与 Foundation Transaction。 |

所有多字节整数使用 big-endian；Decoder 只接受精确长度。Encoder/Decoder 在保留值、非法组合、
容量不足或任意输入/输出部分重叠时，在首次写入前失败，输出缓冲区和输出长度保持不变。

### 3.2 未冻结、因此未自行发明的布局

以下 Opcode 已保留语义，但本阶段不声称其完整 Wire Body 已冻结：

- `TRANSFER_SETUP_ACK`；
- `TRANSFER_ABORT`；
- `TRANSFER_TERMINAL_RECEIPT`；
- 生产路径中的完整 `SACK_CREDIT` Control Envelope。

测试只使用已冻结 Codec 或 typed facts 验证状态机，避免为了赶进度产生新的线上格式。

## 4. 单帧 Reliable

### 4.1 发送链

```text
begin(frozen key + security facts + aad/payload digest + sealed bytes)
  -> READY
  -> copy_attempt() 返回完全相同的 sealed bytes
  -> note_submit(submitted=true)
  -> WAIT_ACK
  -> exact ACK -> DONE
  -> retire()
```

- 重传复用首次保存的 sealed bytes、Origin Sequence、摘要和安全代际，不能重新编码或更换 Key；
- `NOT_SUBMITTED` 不增加已提交次数，状态返回 READY，受同一绝对 Deadline 与重试间隔约束；
- 每次真正提交推进 attempts；达到最大次数或 Deadline 后进入 FAILED；
- ACK 必须精确反向绑定 Source/Destination，并匹配 Realm、Service、Origin Sequence、Session/
  Key Generation 与安全模式；O0 还精确绑定 `link_id + link_generation + policy_generation`，
  不能复用旧 Link/Policy 的布尔批准；
- DONE、FAILED、CANCELLED 都是单向终态，错误或迟到事实不得复活事务。

### 4.2 接收与 retained receipt

接收端按完整 Reliable Key、Security facts、AAD/Payload Digest 判断重复。完全相同的重放返回原
Delivery ACK；相同 Key 但摘要或安全事实不同则失败关闭。receipt 在固定时限内保留，由有界
维护游标回收；表满不驱逐未过期记录。

## 5. Transfer

### 5.1 Setup 与资源原子性

RX Setup 只有在 TX/RX identity、Parent、Source/Destination Binding、Realm、Transport Policy、
安全域或 O0 Link/Policy、Flow 指纹、分片预算、总长度、Deadline 和正文 Digest 全部合法时
才预留固定 RX 槽与终态回执槽。任何一个槽不足都零写失败，不留下半个事务。精确重复 Setup
只有在 Security 把它分类为认证重放候选且本地仍保留精确证据时才幂等返回既有 Handle；冲突重复、
fresh 重复和证据清理后的旧重放均拒绝。O0 没有密码重放候选，但仍精确绑定当前 Link ID、Link
Generation 和 Policy Generation。

C1 Setup 额外要求有效、当前且一次性的 Parent grant。普通 Flow Setup 目前只验证冻结 Setup 自身，
不把它误称为生产 Flow current-proof 校验。

### 5.2 分片、SACK 与重组

- 分片允许乱序；
- O1/O2 完全相同的重复分片只有带认证重放分类且命中保留证据时幂等；O0 重复必须仍匹配精确
  Link/Policy 事实；
- 同一 Index 的长度或内容摘要冲突失败关闭；
- Offset、Length、Index、Count 和总长度都 checked；
- SACK 只推进已确认 bitmap，不能确认未发送或越界 Fragment；
- 全部分片到齐后重新计算完整正文 SHA-256/128，只有与 Setup Digest 完全相同才进入 COMPLETE；
- RX Copy Complete 与业务 Delivered 分离，避免“已复制”等同于“业务已消费”。

### 5.3 终态与回执生命周期

`COMPLETE`、`DELIVERED`、`ABORTED`、`FAILED` 不允许被重复 Setup、SACK、Fragment 或迟到事实
拉回中间态。`COMPLETE/DELIVERED` 的精确重复可按保留证据幂等查询；`ABORTED/FAILED` 的重复
Setup 明确拒绝。任何路径都不会改写正文、Deadline、回执或计数。

终态回执拥有独立槽和绝对保留期。回执过期前，关联 DELIVERED RX 不能单独消失；回执到期后由
`maintain()` 在同一 Owner 锁内原子退休回执与关联 RX，保留二者 slot generation，防止旧 Handle
命中新对象。表满时不靠机会式驱逐旧回执创建新事务。

## 6. C1 durable Parent 与 Persistence

### 6.1 高水位流程

```text
import/recover current parent
  -> prepare checked-next high-water
  -> emit immutable Persistence Requirement
  -> Coordinator submits to Persistence Foundation
  -> Provider durable commit and reload
  -> exact Proof returns through Coordinator
  -> activate high-water and publish one-shot Transfer-ID grant
  -> C1 TX begin atomically consumes the grant
```

未持久化时不会发布新 Transfer ID。Proof 精确绑定 Persistence Handle、Continuation、Domain、
Domain Generation、Foundation Transaction、Record Generation、Witness、Schema、Operation、Body
Digest、Transition Fingerprint、Runtime 和双方 Owner。任一字段不匹配都零写拒绝。

每个 Parent 同时最多持有一个 outstanding grant；在 grant 被成功消费或显式丢弃前，不允许准备
下一次高水位。C1 TX 参数不合法时不会消费 grant，修正参数后仍可使用同一 grant；成功创建 TX
后 grant 立即失效，不能重复使用。

### 6.2 失败与恢复

- Persistence 尚未接收前可 `abort_unsubmitted()`，恢复到原 Active 记录；
- 已绑定 Persistence 后，只接受精确 terminal Failure，伪造或旧 Failure 不解除 obligation；
- 重启只从已提交 Parent Record 导入，不从易失 pending/grant 猜测状态；
- Record 高水位和 Foundation Transaction 都 checked-next，耗尽后失败关闭，不回绕。

## 7. 固定资源与栈

| Profile | Transport Owner | Parent record object | Reliable TX/RX | 单帧字节 | Transfer TX/RX/Receipt | Transfer 字节 | Fragment 上限 | Parent |
| --- | ---: | ---: | --- | ---: | --- | ---: | ---: | ---: |
| Nano | 3,248 B | 360 B | 2/2 | 128 B | 1/1/1 | 512 B | 16 | 1 |
| Lite | 15,080 B | 360 B | 4/8 | 256 B | 2/2/2 | 2,048 B | 64 | 2 |
| Full | 53,656 B | 360 B | 8/24 | 512 B | 4/4/4 | 4,096 B | 128 | 4 |

实现不调用 `malloc/calloc/realloc/free`。GCC Debug `-fstack-usage` 扫描 Transport 的 114 个函数，
最大单函数自动栈为 272 B，低于 1,024 B 门限。该数字是 Host 编译器结果，不是 MCU 完整调用链
栈水位；后续 ESP32/FreeRTOS 接线后必须重新测量。

## 8. 分项与全体自审

| 自审项 | 发现 | 已采取的处理 |
| --- | --- | --- |
| Reliable 重传 | 重建 Frame 会改变 Sequence、Nonce 或密文。 | Owner 保存首次 sealed bytes，所有重试只复制该快照。 |
| ACK 错绑 | 只比较 Sequence 会跨 Source/Realm/Session 误确认。 | ACK 使用完整反向 identity、安全代际和 Service 精确比较。 |
| O0 策略错绑 | 单个 trusted 布尔值可被旧 Link/Policy 重用。 | O0 绑定 Link ID、Link Generation 和 Policy Generation，并禁止伪造 authenticated replay candidate。 |
| Transfer 域错绑 | 只保存 Parent ID/Generation 会跨 Binding、Realm、安全域或 Flow 复用 Setup/Fragment。 | 每个 TX/RX/receipt 冻结完整 typed facts；反馈精确反向匹配，Fragment/Setup 按完整域匹配。 |
| Transfer 重放复活 | receipt 回收后，旧认证 Setup 可能重新分配重组槽并再次交付。 | fresh 与 authenticated replay candidate 分开；后者只能查询仍保留的精确 Setup/Fragment/receipt，证据消失后拒绝。 |
| Setup 原子性 | RX 槽成功、回执槽失败会留下半事务。 | 写入前同时预检两个槽与 generation。 |
| 分片冲突 | 重复 Index 可用不同内容覆盖正文。 | 每片保存摘要和长度；冲突重复拒绝。 |
| Digest 完成 | 仅看 bitmap 会把损坏正文标成 COMPLETE。 | 完整重组后重新计算正文摘要再提交终态。 |
| Parent grant | 参数错误可能消耗唯一 durable ID。 | C1 begin 完成全部校验和槽预检后才消费 grant。 |
| Parent 并发 | 未消费 grant 时再 prepare 会产生悬空 ID。 | outstanding grant 阻断下一次 prepare。 |
| 终态倒退 | 超时后的重复 Setup/SACK 可能把 COMPLETE 拉回发送态。 | 先识别精确终态重复；Fragment 仅允许 SENDING。 |
| 回执悬挂 | 只回收 receipt 会留下不可达 DELIVERED RX。 | 维护步骤原子退休 receipt 与其关联 RX。 |
| Slot ABA | 退休后旧 Handle 可能命中新事务。 | 每类槽保留并 checked-next 推进 generation。 |
| 输出别名 | 输出覆盖 Owner/输入会在校验中途破坏状态。 | 所有入口在取锁和首次写入前执行双向 overlap 检查。 |
| 跨 Owner 依赖 | Transport 直接调 Persistence 会破坏唯一编排边界。 | target 只链接 Common；Persistence 通过 typed Requirement/Proof 集成测试。 |

第二轮全体自审专门从“终态不可逆、Deadline 边界、资源表满、失败零写、重启后 Proof、并发 Owner、
Feature/Archive 隔离”逆向检查，未发现尚未处理的软件 P0/P1；但这一结论只适用于当前私有 Host
模型，仍等待独立外审。

## 9. 验证矩阵

| 门禁 | 结果 |
| --- | --- |
| Windows GCC Full Debug | 71/71 PASS |
| Windows GCC Lite Debug | 71/71 PASS |
| Windows GCC Nano Debug | 71/71 PASS |
| MSVC 19.29 Full Release | Transport 定向 8/8 PASS |
| WSL GCC ASan/UBSan Nano | Transport 定向 8/8 PASS |
| WSL GCC `-fanalyzer -Werror` Nano | Transport 定向 6/6 PASS |
| WSL Clang 18 Release | Transport 定向 8/8 PASS |
| 普通 pthread 并发 | 双 Owner 固定轮次压力 PASS |
| Boundary / archive / stack | PASS；私有符号不进入安装公共 archive |

当前 WSL GCC TSan 运行时仍会在测试入口报 `ThreadSanitizer: unexpected memory mapping`，而现有
Clang 环境缺少可用 TSan runtime。因此不把 TSan 写成通过证据；并发证据限于共享锁合同、普通
pthread 压力及 ASan/UBSan。

## 10. 开放项与下一步

以下能力没有随 IMPL-06 离线模型获得放行：

1. Coordinator 向 Flow Transfer 注入并在使用点复核当前 Route/Flow/Capability/Security Proof；
2. Setup ACK、Abort、Terminal Receipt 和生产 SACK Control Body 的完整冻结布局与 Golden；
3. Reliable/Transfer 进入公共 Runtime、QoS、Adapter Token 与真实发送完成链；
4. Security protect/open、Replay、Path MTU 与 Carrier segmentation 的真实组合；
5. scatter/gather 或 zero-copy；当前实现明确为有界 copy path；
6. C 与 Rust 在线互操作；
7. ESP32-S3、真实 Flash/掉电、物理 Bearer、MCU RAM/Flash/完整任务栈、性能与长稳；
8. 可运行环境中的 TSan。

因此下一阶段若继续，只能开始 `IMPL-07 Service/Operation/高级 QoS` 的私有合同和离线模型；在
IMPL-06 外审完成前，不得把本阶段状态改成生产 GO，也不得把它接入安装 API。
