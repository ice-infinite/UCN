# UCN 全局公共配置说明

> 对应任务：V5-09
> 目标：把可调编译期参数集中到一个公开入口，同时保留原头文件默认值作为回退，不把运行期身份、密钥和板级配置写死进 Core。

## 1. 配置入口

公共配置文件是：

```text
include/ucn/ucn_config.h
```

V5-09 建立时它集中收录了 103 个分散默认项；此后新增的 Link 存活档和 Transfer 配置也继续进入同一入口。当前覆盖 Build Profile、Frame/MTU、静态容量、路由/邻居/控制预算、诊断、Path/Policy、Service 和可选 Transfer。各功能头中的 `#ifndef` 回退仍保留。

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
#ifndef UCN_TRANSFER_RX_SLOTS
#define UCN_TRANSFER_RX_SLOTS ((size_t)1U)
#endif
#ifndef UCN_TRANSFER_MAX_WINDOW
#define UCN_TRANSFER_MAX_WINDOW ((uint8_t)2U)
#endif

#endif
```

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
| 固定 RAM 容量 | Link、Neighbor、Bearer、Route、Queue、Endpoint、Path、Policy、Service、`UCN_EVENT_RUNTIME_MAX_SOURCES`，以及 `UCN_TRANSFER_MAX_MESSAGE_BYTES`/TX/RX Slot 深度/`UCN_TRANSFER_MAX_WINDOW` |
| 时间与预算 | Heartbeat、Route 生命周期、Token、Step 上限、Probe、诊断超时、`UCN_EVENT_RUNTIME_DEFAULT_DRAIN_ROUNDS`/`SOURCE_BUDGET` |
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

很多宏会改变 `ucn_node_t`、Adapter Queue、Service Router、`ucn_transfer_t` 等对象布局。同一个固件中的 Core、Adapter、Service、Transfer 和业务 Translation Unit 必须看到完全相同的配置；不能只给某一个 `.c` 单独定义容量宏。

原有静态编译门禁继续生效，例如：

- Frame/Payload 不足以容纳当前 Feature 的控制帧会编译失败；
- `UCN_MAX_HOPS` 必须在 1～254；
- Maintenance 上界必须早于 Neighbor Suspect；
- Security Required 不能用于未编译 Security 的 Nano；
- Service Binding/Validator 和 Policy 容量必须满足依赖关系。

`UCN_CONFIG_NO_DEFAULTS` 只用于回退测试或特殊集成验证，不建议作为产品日常配置开关。

## 6. 软件证据

- V5-09 自动盘点确认原公共头 103 个分散默认项为 103/103；后续 Transfer 新增项同样同时具备全局默认和 `ucn_transfer.h` 本地回退。
- `ucn_config_defaults_test`：统一默认值生效。
- `ucn_config_fallback_test`：定义 `UCN_CONFIG_NO_DEFAULTS` 后，原头文件默认值仍可独立构建并保持一致。
- `ucn_config_override_test`：产品头将 Transfer 最大消息覆盖为 512 B、RX Slot/编译窗口覆盖为 2，将 Event Runtime Source/Round/Source Budget 覆盖为 3/5/2，把 Stream Ring/Byte Budget/Error Budget/Read Chunk 覆盖为 128/64/2/16 B，并把 CAN Frame Ring/Reassembly Slot/Timeout 覆盖为 4/1/75 ms；其余参数继续使用统一默认。公共 Transfer 编译最大窗口为 8，运行默认仍为 1。
- Stream 默认宏为 `UCN_STREAM_SOURCE_DEFAULT_RING_BYTES=512`、`UCN_STREAM_SOURCE_DEFAULT_BYTE_BUDGET=512`、`UCN_STREAM_SOURCE_DEFAULT_ERROR_BUDGET=4`、`UCN_STREAM_SOURCE_READ_CHUNK_BYTES=32`。它们只决定便利 Storage/默认服务预算；产品也可给每个 Source 传入自己的静态 Ring/Frame 数组和运行预算，但同一固件的 ABI 宏仍必须全翻译单元一致。
- CAN 默认宏为 `UCN_CAN_SOURCE_DEFAULT_RING_FRAMES=8`、`UCN_CAN_SOURCE_DEFAULT_REASSEMBLY_SLOTS=2`、`UCN_CAN_SOURCE_DEFAULT_REASSEMBLY_TIMEOUT_MS=250`。前两项会改变 `ucn_can_source_default_storage_t` 布局，必须全工程一致；产品也可不使用便利 Storage，改传自己的固定 Frame Ring、Slot Descriptor 和扁平重组区。
- 独立 Full/Service ON 产品配置头构建通过；Nano/Lite/Full Debug/Release 和 Full ASan+UBSan 回归通过。
