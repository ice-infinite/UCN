# UCN Realtime Metadata v1 编解码 RFC

> 状态：`EXTERNAL REVIEW GO / LIMITED EXPERIMENTAL SOFTWARE SCOPE`
>
> 适用基线：`main@69901bf`，UCN Core Wire v5
>
> 边界：本 RFC 只描述独立 16 B Realtime Envelope Codec。RT-A01～A11 已整改并通过第四轮全体自审与受限软件范围外部复审；所有实现仍位于默认不链接的实验 Extended archive，不得据此接入生产 Node RX/TX 或宣称硬件时间戳能力。

## 1. 目的

Realtime Metadata v1 为明确选择实时 ABI 的 Endpoint 提供固定、可验证的时间前缀。没有选择实时 ABI 的普通 Endpoint 不携带该前缀，Core Header、W0～W3 Frame、Route、Path 和中继行为保持不变。

本阶段只解决三个问题：

1. 将一个语义对象严格编码成固定 16 B 网络大端序；
2. 将 16 B 输入严格解码为语义对象；
3. 将发送端 uncertainty 上界向上量化为不会低估的 5-bit class。

本阶段不判断 Endpoint Requirement、不计算接收端组合 `U`、不判断 Deadline、不维护同步状态，也不发送任何时间同步控制帧。

## 2. 模块和链接边界

公共头文件为 `include/ucn/ucn_realtime.h`，实现为 `src/extended/time/ucn_realtime.c`。CMake 目标为独立静态库 `ucn_realtime`，使用 `EXCLUDE_FROM_ALL`：

- `ucn_core` 不包含实时模块对象或符号；
- Nano、Lite、Full 的 `ucn_node_t` 和所有 Core 静态存储布局不变；
- 产品只有显式链接 `ucn_realtime` 才承担 Codec Flash 成本；
- Codec 不持有堆、静态可变状态、Node 指针或 OS 对象；
- 同一份纯 C99 实现供裸机、RTOS 和 Host 使用。

`ucn/ucn.h` 不隐式包含可选模块；调用方显式包含 `ucn/ucn_realtime.h`。

## 3. 固定 Wire 布局

Envelope 固定为 16 B，所有多字节整数使用网络大端序：

| Offset | 长度 | 字段 | 编码规则 |
| ---: | ---: | --- | --- |
| 0 | 1 B | `version_mode` | bits 7..4=`1`；bits 3..2=`0`；bits 1..0=`1/2/3` |
| 1 | 1 B | `quality_flags` | bits 7..3=uncertainty class；bit2=`SAMPLE_CAPTURE_HW`；bit1=`DOMAIN_TIME_VALID`；bit0=`SOURCE_HOLDOVER` |
| 2 | 2 B | `clock_domain_id` | 网络大端序 |
| 4 | 4 B | `domain_generation` | 网络大端序 |
| 8 | 8 B | `capture_time_us` | 网络大端序，允许值 0 |

常量：

```text
wire bytes              = 16
format version          = 1
domain ID valid range   = 1..0xFFFE
generation valid range  = 1..0x7FFFFFFF
uncertainty class       = 0..30 known, 31 unknown
```

`mode=0/NONE` 表示该 Endpoint 根本不携带 Envelope，不能编码成一个 16 B 全零对象。

## 4. 语义合法组合

### 4.1 LOCAL_STAMP

必须同时满足：

- `mode=LOCAL_STAMP`；
- `uncertainty_class=31`；
- `clock_domain_id=0`；
- `domain_generation=0`；
- `DOMAIN_TIME_VALID=0`；
- `SOURCE_HOLDOVER=0`；
- `SAMPLE_CAPTURE_HW` 可为 0 或 1。

LOCAL_STAMP 只表达来源节点自己的本地采样顺序，不能被解释为跨节点时间。

### 4.2 SYNCED_STAMP 和 DEADLINE

两种模式的 Codec 结构门禁相同：

- mode 分别为 2 或 3；
- uncertainty class 必须在 `0..30`；
- `clock_domain_id` 必须在 `1..0xFFFE`；
- `domain_generation` 必须在 `1..0x7FFFFFFF`；
- `DOMAIN_TIME_VALID=1`；
- `SOURCE_HOLDOVER` 和 `SAMPLE_CAPTURE_HW` 均可为 0 或 1。

Codec 接受 `SOURCE_HOLDOVER=1` 只表示结构合法。未来 RT-02 Policy 必须继续执行：REQUIRED 接收固定拒绝远端 HOLDOVER，PREFERRED 也默认拒绝，不能把 Codec 成功当作业务准入成功。

### 4.3 必须拒绝

- version 不等于 1；
- `version_mode.bits[3:2]` 非零；
- mode 为 0 或大于 3；
- uncertainty class 大于 31；
- LOCAL_STAMP 携带 Domain、generation、Domain valid 或 source holdover；
- LOCAL_STAMP 使用 known uncertainty class；
- SYNCED/DEADLINE 使用 domain 0/0xFFFF、generation 0/大于阈值、unknown uncertainty 或没有 `DOMAIN_TIME_VALID`。

## 5. uncertainty class 唯一算法

输入是发送端已经用检查算术得到的上界 `S`，不是端到端组合 `U`：

该“已经得到”不是允许调用方随意填写一个数。RT-A02 后的候选实现要求同步层
先通过 known mask 聚合 timer resolution、Link capture、filter residual、integer
rounding 和本事务 Path asymmetry；Policy 再加入已证明的 sample-capture bound。
任一必需分量未知、为零或求和溢出时，都不得把结果标记为 known。

```text
if known == false or S > 2^30 us:
    class = 31
else:
    S = max(S, 1 us)
    class = minimum k in [0,30] where 2^k >= S
```

因此：

| 输入 | 输出 class |
| ---: | ---: |
| unknown | 31 |
| 0 或 1 us | 0 |
| 2 us | 1 |
| 1024 us | 10 |
| 1025 us | 11 |
| `2^30 us` | 30 |
| 大于 `2^30 us` | 31 |

解码 class 31 时必须返回 `known=false`；class 0..30 返回 `known=true` 和精确的 `2^class` 上界。

## 6. 公共 API 合同

```c
bool ucn_realtime_envelope_is_valid(
    const ucn_realtime_envelope_t *envelope);

ucn_result_t ucn_realtime_envelope_encode(
    const ucn_realtime_envelope_t *envelope,
    uint8_t output[UCN_REALTIME_ENVELOPE_WIRE_BYTES]);

ucn_result_t ucn_realtime_envelope_decode(
    const uint8_t *input,
    size_t input_length,
    ucn_realtime_envelope_t *output);

ucn_result_t ucn_realtime_uncertainty_class_encode(
    bool known,
    uint64_t upper_bound_us,
    uint8_t *output_class);

ucn_result_t ucn_realtime_uncertainty_class_decode(
    uint8_t uncertainty_class,
    bool *known,
    uint32_t *upper_bound_us);

ucn_result_t ucn_realtime_uncertainty_aggregate(
    const ucn_realtime_uncertainty_components_t *components,
    uint32_t path_asymmetry_bound_us,
    bool path_asymmetry_known,
    uint32_t *upper_bound_us);
```

错误和写回规则：

| 情况 | 返回 | output |
| --- | --- | --- |
| NULL 参数 | `UCN_ERR_ARGUMENT` | 不写 |
| encode 语义对象非法 | `UCN_ERR_ARGUMENT` | 16 B 完整不写 |
| decode 长度不是 16 B | `UCN_ERR_MALFORMED` | 对象完整不写 |
| decode version 不是 1 | `UCN_ERR_VERSION` | 对象完整不写 |
| decode 字段/组合非法 | `UCN_ERR_MALFORMED` | 对象完整不写 |
| uncertainty class 大于 31 | `UCN_ERR_ARGUMENT` | 所有输出完整不写 |

Encoder 必须先在局部临时缓冲区完成验证和编码，最后一次性复制到调用方 output。Decoder 必须先解码到局部对象，全部验证通过后才赋值给调用方 output。

## 7. Golden Vector

### 7.1 LOCAL_STAMP + hardware capture

```text
Object:
mode=1, class=31, SAMPLE_CAPTURE_HW=1,
domain=0, generation=0, capture=0x0102030405060708

Wire:
11 FC 00 00 00 00 00 00 01 02 03 04 05 06 07 08
```

### 7.2 SYNCED_STAMP

```text
Object:
mode=2, class=10, SAMPLE_CAPTURE_HW=1, DOMAIN_TIME_VALID=1,
domain=0x1234, generation=0x01020304,
capture=0x1122334455667788

Wire:
12 56 12 34 01 02 03 04 11 22 33 44 55 66 77 88
```

### 7.3 DEADLINE + source holdover

```text
Object:
mode=3, class=11, DOMAIN_TIME_VALID=1, SOURCE_HOLDOVER=1,
domain=0xFFFE, generation=0x7FFFFFFF,
capture=0xFFFFFFFFFFFFFFFF

Wire:
13 5B FF FE 7F FF FF FF FF FF FF FF FF FF FF FF
```

## 8. 本阶段验收

RT-01 只有同时满足以下条件才可进入外审：

- 三条 Golden Vector 逐字节 encode/decode；
- 所有字段和组合逐项负向测试；
- 15/16/17 B 长度测试；
- 所有失败路径完整 output 不写回；
- uncertainty 的 0/1/2/1024/1025/`2^30`/超界/unknown 边界；
- 固定 uncertainty 分量的完整 known mask、零值、Path unknown 和 checked-add 溢出；
- GCC/MSVC Debug、GCC Release、Full/Lite/Nano；
- WSL ASan/UBSan 与 `-fanalyzer -Werror`；
- `ucn_core` 不包含 realtime symbol，`ucn_node_t` 大小不变；
- 没有生产 Node、Adapter、Service、Cluster 或 Port 调用该 API。

RT-01 通过仍只代表 Codec 软件闭环，不代表 RT-02 Endpoint Policy、RT-03 Domain、RT-04 Timed Link、RT-05 四报文同步或任何硬件精度已经完成。

当前实现、自审证据和未验证边界见 [UCN RT-01 Realtime Metadata Codec 自审报告](UCN_RT01_Realtime_Metadata_Codec_自审报告.md)。
