# UCN V6-10 Transfer Selective Repeat 与 Credit 实现报告

## 1. 目标、依赖与边界

V6-10 在 V6-04 的正交 Message/Operation、V6-06 的精确 Path Budget、V6-07 的
Security Session、V6-08 的 RouteSet/Path 以及 V6-09 的 QoS 上实现大消息传输。
本阶段仍是 `UCN_BUILD_V6_EXPERIMENTAL=ON` 下的隔离 archive，不接 v5 Node、Adapter
或生产 RX/TX。Transfer 不重新定义路由、安全或业务执行语义：它只负责把一个已经通过
Endpoint 合同的可靠 Q2/Q3 Operation 分成可选择确认的 Fragment，并将重组完成与业务
Result 分开。

## 2. 九档 Message Class 与精确数据预算

Message Class 固定为 T32、T64、T128、T256、T512、T1K、T2K、T4K、T8K。它是
接收能力桶，不是 Wire 帧长度；实际 Payload 可以小于桶上限。产品通过
`UCN_V6_CONFIG_TRANSFER_MAX_CLASS` 在编译期裁掉大桶，因此 Nano 不必为 T8K 付出 RAM。

Fragment 的数据预算必须来自 V6-06 `path.fragment_data_budget`。V6-06 已按以下顺序做
checked subtraction：

```text
Path Frame MTU
- 当前 Address Class 基础头
- 精确启用的 Wire extensions
- Hop/E2E tags
- CRC32C
- Transfer Fragment Header
= fragment_data_budget
```

Transfer 再要求请求预算非零且不超过该值。24 B Fragment Header 固定携带 Message Class、
64-bit Message/Operation ID、总长度、分片序号、分片总数、冻结预算和整消息 CRC32C。
Message ID 必须等于已经持久分配的 Operation ID，避免重启后出现第二套 RAM-only ID 域。

## 3. 固定窗口 Selective Repeat

每个 TX 槽只保存当前固定窗口的状态；Payload 始终由调用方持有，直到终态后
`retire_tx()` 返回 `buffer_token`。状态流程为：

```text
SEND_BEGIN -> SENDING -> REMOTE_REASSEMBLED -> RETIRE
                    \-> FAILED -------------> RETIRE
```

窗口中未发送的 Fragment 可连续提交，不需要逐片等待 ACK。SACK 使用累计 Base 加 32-bit
位图；接收方可以先收到 0、2、3，发送方只会在 Retry 到期后重发缺失的 1，同时继续把
窗口中新开放的 4 发出。测试明确在收到首个 SACK 前提交四片，并验证单片丢失时统计只有
一次重传，因而不是每跳/每片 Stop-and-Wait。

`next_fragment()` 是 peek，只有 `record_fragment_submit()` 才改变 attempts 和发送时刻。
Driver 背压不会被误记成物理发送。每片重试次数、间隔与 Path 总 Deadline 都有上限；总
Deadline 到达半开边界时 TX 进入可退休的 FAILED，而不是永久占槽。

## 4. RX 重组与 Recent Completion

RX 使用编译期固定槽和固定最大 Message 缓冲，不动态分配。槽主键为：

```text
{Origin Principal, Origin Binding, Origin Session,
 Operation ID == Message ID}
```

第一片冻结 Class、总长度、分片数、数据预算和 CRC；后续任一字段变化均按重放拒绝。
乱序分片写到确定偏移，重复分片只有字节完全相同时才幂等，内容冲突返回 Security 错误。
全部分片齐全后校验整消息 CRC32C，随后才允许业务复制。

应用退休完成槽时写入固定 Recent Completion。Recent 项保存完整 Message 身份而非只保存
CRC；最后一个 SACK 丢失时，完全相同的末片无论在业务退休前还是 Recent 租期内到达，均可
重新生成完整 SACK，但不再次交付业务。Recent 满时不驱逐其他 Operation，完成槽保留并
失败关闭。RX 完成槽和 Recent 都有半开 Deadline，防止应用或对端永久耗尽静态资源。

## 5. 逐跳 Credit

Credit 只用于 Q2/Q3，归属精确的：

```text
{Authenticated Ingress Peer Session,
 Link ID, Link Generation, Traffic Class,
 Credit Generation, Update Sequence}
```

Credit Frame 必须先通过 Hop Authentication。新 Link Generation 和 Credit Generation
只能 checked-next；Sequence 同代际只能 checked-next，exact replay 不续租。额度预留采用
`reserve -> physical submit -> finish`：未提交可退回，已提交永久消费。存在未完成预留时，
新的绝对 Credit Update 失败关闭，避免更新覆盖本地尚未结算的扣减。租期、表容量和预留
容量全部固定，满载不驱逐。

## 6. Path 切换、Session 失效与中继流水

Path 重绑定只允许同一目标 Principal/Binding/Session，并要求新 Path 的 Message Class、
Fragment Budget、Window 和 Feature 不低于已冻结事务；Route/Path Generation 必须合法且
新 Deadline 尚未到期。已经 SACK 的 Fragment 不重发，未确认片改在新 Path 上重试；旧
Path 的迟到 SACK 因 Route/Path Context 不匹配而拒绝。

Security REAUTH/撤销通过 `invalidate_session()` 原子回收目标 Session 的 TX Buffer Token、
RX、Recent、Credit 和 Credit Reservation。调用方输出容量不足时整个对象零写，避免只清
状态却丢失 Buffer 所有权。

中继不建立整消息 RX 槽，也不计算整消息 CRC；它只对单片完成 Hop Security、Credit 与
V6-09 QoS 调度后转发。因此多跳可以形成 Fragment Pipeline。只有最终 Endpoint 执行 E2E
准入和重组，业务 Result 使用独立固定 Payload，不能把 Remote Reassembled 冒充成
Application Result。

## 7. 分项自审

| 小节 | 自审结论 |
|---|---|
| 10-01 Class/Codec | 九档、精确长度、零保留位、失败 output 不写回和 Result 分层已固定 |
| 10-02 TX Window | peek/submit、四片流水、Selective SACK、缺片重传和次数上限已覆盖 |
| 10-03 RX Reassembly | 乱序、重复、冲突、整消息 CRC、完成槽与 Recent replay 已覆盖 |
| 10-04 Credit | Peer/Link/Class/代际主键、checked-next、exact replay 不续租与 reservation 已覆盖 |
| 10-05 Path/Session | 窄 Path 拒绝、合法重绑定、Deadline、并发和原子 Session Fence 已覆盖 |
| 10-06 固定资源 | TX/RX/Recent/Credit/Reservation 全部进入 Manifest/Layout Hash，满载不驱逐 |

自审期间额外关闭了：Message ID 与 Operation ID 分裂、Path 并发上限未执行、完成 RX 永久
占槽、Recent 身份字段不足、Credit struct padding 比较、Credit 代际回绕、更新覆盖未结算
Reservation、发送 Deadline 后不可退休、以及 Session 撤销遗漏 Transfer 资源等问题。

## 8. 验证结果

| 门禁 | 结果 |
|---|---|
| Windows GCC Full | 69/69 |
| MSVC Release Config/Security/Capability/QoS/Transfer | Transfer 等 5 项通过；未构建的旧 Route target 不计失败 |
| WSL ASan/UBSan Transfer | 1/1 |
| WSL `-fanalyzer -Werror` Transfer | 1/1 |
| `git diff --check` | 无空白错误，仅行尾转换提示 |

## 9. 未完成与硬件边界

本报告不证明真实 UART/CAN/CAN-FD/USB/ESP-NOW 的吞吐、DMA Buffer 生命周期、物理 Credit
精度或多跳带宽。Q0/Q1 压力下的最坏等待、不同 MTU 的实际 Pipeline 增益、丢包曲线和 Path
切换性能属于 V6-13/14 实机与系统验证。当前也没有把 Transfer 接入生产 Node/Adapter。

当前状态：`V6-10 软件实现与分项自审完成 / FINAL EXTERNAL REVIEW DEFERRED`。
