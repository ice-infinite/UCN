# UCN V6-11 Realtime 生产集成实现报告

## 1. 范围与边界

V6-11 把已经完成安全整改的实时语义重新接到 v6 Identity、Security、Capability 和固定
Path。实现位于独立 `ucn_v6_realtime` archive，不链接 Cluster，也不复用 v5 的公开
Realtime 类型。当前称“生产集成”是指 v6 对象和安全合同已经贯通；真实 MCU 时间戳精度、
Driver 与物理链路仍属于 V6-13/14，不在本报告中冒充完成。

## 2. 零开销与固定 Envelope

每个 Endpoint 独立选择 NONE、LOCAL_STAMP、SYNCED_STAMP 或 DEADLINE。NONE 直接复制业务
Payload，线上增加 0 B。其余模式在 E2E Payload 前增加固定 16 B Envelope：

```text
byte 0      version + mode
byte 1      uncertainty class + capture/domain/holdover flags
byte 2..3   clock domain ID
byte 4..7   domain generation
byte 8..15  capture domain time/us
```

对象不使用 packed struct；Codec 显式写网络序、检查保留位和语义组合，失败保持完整输出不变。
中继只处理外层 v6 Header/Hop Budget，不解析加密的 Envelope。

## 3. Capability 与固定 Path

Capability Record 调整为 68 B，末 8 B 认证 `realtime_mode_bits`、`clock_domain_id` 和
`clock_domain_generation`。声明 SYNCED/DEADLINE 时 Domain ID/Generation 必须非零且合法；
未声明 Realtime 时这些字段必须全零。

Path Budget 请求显式标记 `fixed_path`，导出的 `immutable_for_realtime` 进入 Capability
Path 和 Route Proposal Digest。Time Domain 只接受以下全部成立的 Path：

- Path 在事务期间不可变；
- 每跳共同支持 Realtime；
- 路径具有 RX/TX hardware timestamp；
- timestamp uncertainty 非零已知；
- Master Principal、Binding、Session、Route/Path Generation 与认证 Capability 完全一致。

普通动态 Route 可以继续承载业务，但不能推动有效同步或产生 SYNCED/DEADLINE Envelope。

## 4. Generation 与持久化

Domain Generation 使用统一的 1..`UCN_V6_SERIAL_ROTATION_THRESHOLD` no-wrap serial。Owner
要求产品提供按 `{Master Principal, Clock Domain}` 持久保存 high-water 的同步 Provider。
绑定新 Domain 前执行：

```text
load high-water
  -> reject rollback
  -> reserve presented generation
  -> reload exact generation
  -> install RAM domain
```

Provider 返回成功但没有真正写入时，reload 会失败并禁止 Domain 生效。整个回调动态范围由
V6-05 共享 callback gate 围栏，任何递归或跨 Owner Provider 调用都失败关闭。

## 5. uncertainty 与 Time Domain

有效样本必须提供六个独立、非零、已知上界：timer resolution、link timestamp capture、
filter residual、arithmetic rounding、sample capture 和 path asymmetry。任一 Unknown、零值
或加法溢出都不能进入滤波窗口。

Time Domain 使用固定 5 样本中值窗口和连续样本锁定门槛。样本必须来自 V6-07 已完成
Hop+E2E+精确 ACL 的 Master，并绑定固定 Route/Path。旧 sample time、过大 offset jump、
算术溢出或同 generation 的候选时间低于已发布 high-water 会直接进入 FAULT。
LOCKED 超时后进入 HOLDOVER，误差按 oscillator ppb 向上增长；超过最大 HOLDOVER 后清空采集
窗口但保留同 generation 的采样与发布时间 high-water，重新锁定不能倒退。

## 6. Endpoint 收发门禁

发送端把当前 Domain uncertainty 与本次 sample capture bound checked-add，再向上量化为二进制
class。REQUIRED 只允许 SYNCED/DEADLINE；硬件采样策略会拒绝 software capture。

接收端和执行端分别重新计算：

```text
U = source_uncertainty_class_upper + local_domain_uncertainty
age_upper = max(0, local_domain_time - capture_time) + U
accept iff U <= max_uncertainty AND age_upper < max_age
```

未来时间最多只能落在组合 uncertainty 内；边界 `age_upper == max_age` 明确拒绝。远端
HOLDOVER 在当前 v6 REQUIRED/PREFERRED 首版均拒绝，因为 Envelope 没有可认证 holdover age。
执行前再次门禁，避免消息在 Queue 中等待后变旧仍被执行。

## 7. 分项自审

| 小节 | 自审结论 |
|---|---|
| 11-01 Codec | 固定 16 B、网络序、保留位、语义组合与失败不写回已覆盖 |
| 11-02 Capability | 68 B 精确字段、Realtime/Domain 条件和 Path immutable 位已覆盖 |
| 11-03 Persistence | high-water rollback、Provider 假成功 reload、callback 重入已覆盖 |
| 11-04 Domain | 连续锁定、旧样本、跳变、单调 high-water、HOLDOVER 和溢出已覆盖 |
| 11-05 Endpoint | NONE 零开销、REQUIRED E2E+ACL、硬件采样和双 Deadline 已覆盖 |
| 11-06 隔离 | Realtime 不链接 Cluster；default-OFF Core 不含 v6 符号 |

## 8. 验证

| 门禁 | 结果 |
|---|---|
| Windows GCC Full | 70/70 |
| MSVC Release Realtime/Capability/Route | 3/3 |
| WSL ASan/UBSan | 3/3 |
| WSL `-fanalyzer -Werror` | 3/3 |
| default-OFF `ucn_core` v6 符号 | 0 |
| `git diff --check` | 无空白错误，仅行尾转换提示 |

当前状态：`V6-11 软件实现与分项自审完成 / FINAL EXTERNAL REVIEW DEFERRED`。

## 9. 未完成的硬件证明

Host 测试不能给出 UART、RS-485、ESP-NOW、CAN/CAN-FD 或 USB 的真实打点位置、ISR/DMA 延迟、
链路非对称、晶振漂移和最坏 uncertainty。V6-13 必须把统一 Timed Link 事件接到参考产品，
V6-14 必须保存不同 Bearer/负载/温度下的原始证据；在此之前不得宣称分布式时间精度或绝对
Deadline 的硬件验收完成。
