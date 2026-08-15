# UCN 全局公共配置说明

> 对应任务：V5-09
> 目标：把可调编译期参数集中到一个公开入口，同时保留原头文件默认值作为回退，不把运行期身份、密钥和板级配置写死进 Core。

## 1. 配置入口

公共配置文件是：

```text
include/ucn/ucn_config.h
```

V5-09 建立时它集中收录了 103 个分散默认项；此后新增的 Link 存活档、Transfer、Cluster 和 C06 Federation 配置也继续进入同一入口。当前覆盖 Build Profile、Frame/MTU、静态容量、路由/邻居/控制预算、诊断、Path/Policy、Service、可选 Transfer、可选单层 Cluster 和可选 Locator Directory。各功能头中的 `#ifndef` 回退仍保留。

加载顺序是：

```text
编译器 / CMake -D
        ↓
可选产品配置头 UCN_USER_CONFIG_HEADER
        ↓
ucn_config.h 中的统一默认值
        ↓
原头文件中的 #ifndef 默认值兜底
```

因此产品只定义自己需要修改的项目即可。某项没有写进产品头时使用全局默认；测试或特殊集成关闭全局默认后，原文件默认仍能独立编译。产品头建议也使用 `#ifndef`，这样显式的 CMake/编译器 `-D` 仍保持最高优先级。

## 2. 推荐使用方式

不要直接维护一份被改乱的 UCN 源码副本。推荐在产品工程创建自己的文件，例如：

```c
/* product/ucn_product_config.h */
#ifndef PRODUCT_UCN_CONFIG_H
#define PRODUCT_UCN_CONFIG_H

#ifndef UCN_MAX_FRAME_BYTES
#define UCN_MAX_FRAME_BYTES ((size_t)128U)
#endif
#ifndef UCN_MAX_PAYLOAD_BYTES
#define UCN_MAX_PAYLOAD_BYTES ((size_t)64U)
#endif
#ifndef UCN_MAX_LINKS
#define UCN_MAX_LINKS ((size_t)3U)
#endif
#ifndef UCN_MAX_NEIGHBORS
#define UCN_MAX_NEIGHBORS ((size_t)4U)
#endif
#ifndef UCN_MAX_BEARERS_PER_NEIGHBOR
#define UCN_MAX_BEARERS_PER_NEIGHBOR ((size_t)2U)
#endif
#ifndef UCN_ADAPTER_RX_QUEUE_DEPTH
#define UCN_ADAPTER_RX_QUEUE_DEPTH 3U
#endif
/* 一个 UART/CAN 控制器/USB Endpoint/无线 Adapter 各占一个 Source。 */
#ifndef UCN_EVENT_RUNTIME_MAX_SOURCES
#define UCN_EVENT_RUNTIME_MAX_SOURCES 4U
#endif
#ifndef UCN_EVENT_RUNTIME_DEFAULT_DRAIN_ROUNDS
#define UCN_EVENT_RUNTIME_DEFAULT_DRAIN_ROUNDS 8U
#endif
#ifndef UCN_EVENT_RUNTIME_DEFAULT_SOURCE_BUDGET
#define UCN_EVENT_RUNTIME_DEFAULT_SOURCE_BUDGET 4U
#endif
/* 只有链接并创建 ucn_transfer_t 的产品才会使用这些 RAM 上限。 */
#ifndef UCN_TRANSFER_MAX_MESSAGE_BYTES
#define UCN_TRANSFER_MAX_MESSAGE_BYTES ((size_t)512U)
#endif
#ifndef UCN_TRANSFER_TX_SLOTS
#define UCN_TRANSFER_TX_SLOTS ((size_t)1U)
#endif
#ifndef UCN_TRANSFER_RX_SLOTS
#define UCN_TRANSFER_RX_SLOTS ((size_t)1U)
#endif
#ifndef UCN_TRANSFER_MAX_WINDOW
#define UCN_TRANSFER_MAX_WINDOW ((uint8_t)2U)
#endif
/* 只有链接并创建 ucn_cluster_t 的产品才使用这些表。 */
#ifndef UCN_CLUSTER_MAX_PEERS
#define UCN_CLUSTER_MAX_PEERS ((size_t)8U)
#endif
#ifndef UCN_CLUSTER_MAX_CANDIDATES
#define UCN_CLUSTER_MAX_CANDIDATES ((size_t)8U)
#endif
#ifndef UCN_CLUSTER_MAX_MEMBERS
#define UCN_CLUSTER_MAX_MEMBERS ((size_t)16U)
#endif
/* 只有链接并创建 ucn_cluster_federation_t 的 Head/Directory 产品使用。 */
#ifndef UCN_CLUSTER_FED_MAX_DIRECTORY_AUTHORITIES
#define UCN_CLUSTER_FED_MAX_DIRECTORY_AUTHORITIES ((size_t)2U)
#endif
#ifndef UCN_CLUSTER_FED_MAX_LOCAL_LOCATORS
#define UCN_CLUSTER_FED_MAX_LOCAL_LOCATORS ((size_t)17U)
#endif
#ifndef UCN_CLUSTER_FED_MAX_DIRECTORY_RECORDS
#define UCN_CLUSTER_FED_MAX_DIRECTORY_RECORDS ((size_t)32U)
#endif
#ifndef UCN_CLUSTER_FED_MAX_LOCATOR_CACHE
#define UCN_CLUSTER_FED_MAX_LOCATOR_CACHE ((size_t)16U)
#endif
#ifndef UCN_CLUSTER_FED_MAX_NEXT_CLUSTERS
#define UCN_CLUSTER_FED_MAX_NEXT_CLUSTERS ((size_t)8U)
#endif
#ifndef UCN_CLUSTER_FED_MAX_PENDING
#define UCN_CLUSTER_FED_MAX_PENDING ((size_t)2U)
#endif
#ifndef UCN_CLUSTER_FED_MAX_SEEN_TRANSACTIONS
#define UCN_CLUSTER_FED_MAX_SEEN_TRANSACTIONS ((size_t)8U)
#endif
#ifndef UCN_CLUSTER_FED_DIRECTORY_LEASE_MS
#define UCN_CLUSTER_FED_DIRECTORY_LEASE_MS UINT32_C(8000)
#endif
#ifndef UCN_CLUSTER_FED_LOCATOR_REFRESH_MS
#define UCN_CLUSTER_FED_LOCATOR_REFRESH_MS UINT32_C(2000)
#endif
#ifndef UCN_CLUSTER_FED_QUERY_TIMEOUT_MS
#define UCN_CLUSTER_FED_QUERY_TIMEOUT_MS UINT32_C(1000)
#endif
#ifndef UCN_CLUSTER_FED_TRANSACTION_LEASE_MS
#define UCN_CLUSTER_FED_TRANSACTION_LEASE_MS UINT32_C(3000)
#endif

#endif
```

`UCN_TRANSFER_TX_SLOTS` 与 `UCN_TRANSFER_RX_SLOTS` 决定能够同时存在的独立
逻辑消息数，也直接改变 `ucn_transfer_t` 的静态 RAM；
`UCN_TRANSFER_MAX_WINDOW` 只决定同一条逻辑消息能够同时在途的 Fragment 数。
三者不是同一个维度。默认 TX/RX Slot 和 Peer 消息并发都为 1；只有产品同时
增加固定 Slot，并通过 `ucn_transfer_set_peer_concurrency_capability()` 显式声明
对端能力后，才允许同一目标存在多条分片消息。增加接收 Slot 的 RAM 代价约为
`UCN_TRANSFER_RX_SLOTS * UCN_TRANSFER_MAX_MESSAGE_BYTES`，必须根据目标 MCU
实际 RAM 裁剪；普通中继不创建 Transfer 对象，不承担这些完整消息缓冲。

PowerShell/CMake 配置时，带 `.h` 的参数必须整体加引号：

```powershell
cmake -S . -B build-product `
  -DUCN_PROFILE=LITE `
  -DUCN_FEATURE_SERVICE=ON `
  "-DUCN_USER_CONFIG_HEADER=ucn_product_config.h" `
  "-DUCN_USER_CONFIG_INCLUDE_DIR=E:/Product/include"
```

直接把 C 源码加入其他构建系统时，所有 UCN 源文件和所有包含公共 UCN 对象布局的产品文件必须同时带上：

```text
-DUCN_USER_CONFIG_HEADER=\"ucn_product_config.h\" -IE:/Product/include
```

也可以直接修改 `include/ucn/ucn_config.h`，但这会让产品配置与上游库混在一起，不利于升级和多产品共用，因此只建议用于快速实验。

## 3. 哪些内容放在这里

| 类别 | 示例 |
| --- | --- |
| 构建裁剪 | `UCN_PROFILE`、`UCN_FEATURE_SERVICE` |
| 帧与范围 | `UCN_MAX_FRAME_BYTES`、`UCN_MAX_PAYLOAD_BYTES`、`UCN_MAX_HOPS` |
| 固定 RAM 容量 | Link、Neighbor、Bearer、Route、Queue、Endpoint、Path、Policy、Service、`UCN_EVENT_RUNTIME_MAX_SOURCES`，Transfer 消息/Slot/窗口，Cluster Peer/Candidate/Member 表，以及 Federation 的 Directory/Cache/Next-Cluster/Pending/Seen 表 |
| 时间与预算 | Heartbeat、Route 生命周期、Token、Step 上限、Probe、诊断超时、Cluster 冷启动观察/故障恢复观察/选举/广告/保活/租约，Federation Directory Lease/刷新/Query 超时/Transaction Seen Lease，以及显式固定有线快速档 `UCN_CLUSTER_FAST_*`、`UCN_EVENT_RUNTIME_DEFAULT_DRAIN_ROUNDS`/`SOURCE_BUDGET` |
| 产品安全门禁 | `UCN_SECURITY_REQUIRED_BY_DEFAULT` |

协议版本、Magic、线上字段宽度、消息编号、CRC/AAD 布局、广播保留值等协议不变量不是产品配置，不能通过全局头随意改变。

## 4. 哪些内容不能放在 Core 全局头

以下属于每块设备或每次启动的运行配置：

- `network_id`、`node_id`、默认 Hop 的产品实例值；
- Flash/NVS 中的逻辑地址、Session/Boot Counter；
- 身份证书、密钥、ACL 和 Endpoint 权限表；
- UART/CAN/Wi-Fi 引脚、MAC、波特率和驱动句柄；
- 运行时选择的固定 TX/最大 RX Wire Profile、Link 和路由策略。

这些仍由 `ucn_config_t`、Node 配置 API、Security Provider、Adapter 和产品 Flash 管理。把 Node ID 或密钥编进公共库头会导致同固件设备冲突或泄密。

## 5. ABI 与失败关闭

很多宏会改变 `ucn_node_t`、Adapter Queue、Service Router、`ucn_transfer_t`、`ucn_cluster_t`、`ucn_cluster_federation_t` 等对象布局。同一个固件中的 Core、Adapter、Service、Transfer、Cluster、Federation 和业务 Translation Unit 必须看到完全相同的配置；不能只给某一个 `.c` 单独定义容量宏。

原有静态编译门禁继续生效，例如：

- Frame/Payload 不足以容纳当前 Feature 的控制帧会编译失败；
- `UCN_MAX_HOPS` 必须在 1～254；
- Maintenance 上界必须早于 Neighbor Suspect；
- Security Required 不能用于未编译 Security 的 Nano；
- Service Binding/Validator 和 Policy 容量必须满足依赖关系。

`UCN_CONFIG_NO_DEFAULTS` 只用于回退测试或特殊集成验证，不建议作为产品日常配置开关。

## 6. 软件证据

- V5-09 自动盘点确认原公共头 103 个分散默认项为 103/103；后续 Transfer 与 Cluster 新增项同样同时具备全局默认和各功能头本地回退。
- `ucn_config_defaults_test`：统一默认值生效。
- `ucn_config_fallback_test`：定义 `UCN_CONFIG_NO_DEFAULTS` 后，原头文件默认值仍可独立构建并保持一致。
- C05.1 增加 `UCN_CLUSTER_RECOVERY_OBSERVATION_MS` 和 `UCN_CLUSTER_FAST_*`；默认档仍为稳定优先，只有产品主动调用 `ucn_cluster_config_apply_timing_profile(...FAST_FIXED)` 才使用快速固定有线值。Cluster 初始化还校验 Advertisement/Keepalive 周期均不大于租约三分之一，避免不安全的手工时间组合。
- C06.2/C06.3 使用 `UCN_CLUSTER_FED_*` 固定表与时间宏。默认 `17` 个本地 Locator 槽等于一个 Head 加 `16` 个成员，Seen 事务槽为 `8`；`DIRECTORY_LEASE/LOCATOR_REFRESH/QUERY_TIMEOUT/TRANSACTION_LEASE=8000/2000/1000/3000 ms`。启用 Federation 时初始化会拒绝空/重复 Directory Authority、无时钟/发送回调、刷新超过租约三分之一，或 Directory Authority 没有出现在其自身副本列表中。`enable_tunnel=true` 时还要求 Head 授权、最终 `deliver` 回调和默认的 `seal_inner/open_inner`；仅外层保护必须显式选择诊断模式。C07 Handover 的首轮重试为 `UCN_CLUSTER_FED_HANDOVER_RETRY_MS=250 ms`、最多 `3` 次，随后按 `LOCATOR_REFRESH` 幂等续发；受保护模式还强制 Handover proof Builder 与 Authorizer，不能用 NULL 回调静默放行。
- C07 Cluster Control 为 v3：`UCN_CLUSTER_MAX_MEMBERS` 受 32 bit ACK bitmap 限制为 `≤32`，产品 Head 的 `member_capacity` 还不得超过 `UCN_CLUSTER_MAX_PEERS`；Backup 成员快照的帧间隔上限为 `UCN_CLUSTER_BACKUP_SYNC_MAX_SPACING_MS=250 ms`。这些是控制面静态容量/时间上限，不替代产品对 Bearer 时延、丢包与功耗的标定。
- `ucn_config_override_test`：产品头将 Transfer 最大消息覆盖为 512 B、RX Slot/编译窗口覆盖为 2，将 Cluster Peer/Candidate/Member/Lease 覆盖为 4/3/6/6000 ms，将 Event Runtime Source/Round/Source Budget 覆盖为 3/5/2，把 Stream Ring/Byte Budget/Error Budget/Read Chunk 覆盖为 128/64/2/16 B，并把 CAN Frame Ring/Reassembly Slot/Timeout 覆盖为 4/1/75 ms；其余参数继续使用统一默认。公共 Transfer TX/RX Slot 默认均为 1、编译最大窗口为 8，运行默认窗口与 Peer 消息并发仍为 1。
- Stream 默认宏为 `UCN_STREAM_SOURCE_DEFAULT_RING_BYTES=512`、`UCN_STREAM_SOURCE_DEFAULT_BYTE_BUDGET=512`、`UCN_STREAM_SOURCE_DEFAULT_ERROR_BUDGET=4`、`UCN_STREAM_SOURCE_READ_CHUNK_BYTES=32`。它们只决定便利 Storage/默认服务预算；产品也可给每个 Source 传入自己的静态 Ring/Frame 数组和运行预算，但同一固件的 ABI 宏仍必须全翻译单元一致。
- CAN 默认宏为 `UCN_CAN_SOURCE_DEFAULT_RING_FRAMES=8`、`UCN_CAN_SOURCE_DEFAULT_REASSEMBLY_SLOTS=2`、`UCN_CAN_SOURCE_DEFAULT_REASSEMBLY_TIMEOUT_MS=250`。前两项会改变 `ucn_can_source_default_storage_t` 布局，必须全工程一致；产品也可不使用便利 Storage，改传自己的固定 Frame Ring、Slot Descriptor 和扁平重组区。
- 独立 Full/Service ON 产品配置头构建通过；Nano/Lite/Full Debug/Release 和 Full ASan+UBSan 回归通过。
