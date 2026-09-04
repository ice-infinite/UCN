# UCN RT-01 Realtime Metadata Codec 自审报告

> 状态：`DONE / EXTERNAL REVIEW GO（受限实验软件范围）`
>
> 日期：2026-09-04
>
> 基线：`main@69901bf` 加当前未提交 RT-01 工作树
>
> 范围：仅独立 16 B Metadata v1 Codec；不包含 Endpoint Policy、Time Domain、同步报文、驱动时间戳或生产收发接线。

## 1. 本轮结论

RT-01 已形成一个可以独立编译、独立链接、无动态内存、无可变静态状态的 Codec 候选：

- 公共语义 API 位于 `include/ucn/ucn_realtime.h`；
- 实现位于 `src/extended/time/ucn_realtime.c`；
- CMake 目标为 `EXCLUDE_FROM_ALL` 的 `ucn_realtime` 静态库；
- 只有显式链接该库的产品才承担 Flash 成本；
- `ucn_core`、`ucn_node_t`、Adapter、Service、Port 和 Cluster 没有新增实时状态或调用；
- 当前阶段没有使用堆、锁、系统时钟、OS API 或 Linux 运行依赖。

这表示固定 16 B Envelope 的“值对象与字节编码边界”已经实现，不表示两个节点已经同步，也不表示当前 UCN 已具备同步时钟或 Deadline 执行业务能力。

## 2. 实现清单

| 文件 | 职责 |
| --- | --- |
| `include/ucn/ucn_realtime.h` | 常量、Mode、语义对象、严格编解码和 uncertainty 量化 API |
| `src/extended/time/ucn_realtime.c` | 显式大端读写、语义合法性、16 B 编解码、ceil-log2 uncertainty class |
| `tests/test_realtime_codec.c` | Golden、字段负向、长度、完整不写回、uncertainty 边界 |
| `CMakeLists.txt` | 建立默认不参与 all 的独立 archive 和定向 CTest |
| `tests/test_public_headers.c` | 证明公共头可与现有公共 API 一起编译 |

## 3. 合同到代码的反向映射

### 3.1 Wire 和模式

Codec 严格接受 16 B：

- Byte 0 高四位必须为版本 1；
- Byte 0 的 Bit 3..2 必须为零；
- Mode 只允许 LOCAL_STAMP、SYNCED_STAMP、DEADLINE；
- Domain ID、generation 和 capture time 均按网络大端序读写；
- `NONE` 表示不携带 Envelope，不能编码成 16 B 对象。

LOCAL_STAMP 必须使用 unknown uncertainty，Domain ID/generation 为零，且不能声明 Domain valid 或 Source holdover。SYNCED_STAMP/DEADLINE 必须使用已知 uncertainty、合法 Domain ID、非零且不越阈值的 generation，并声明 Domain valid。

### 3.2 不写回

Encode 先写入函数栈上的完整 16 B 临时数组，全部校验成功后才一次复制到调用方 output。Decode 先构造函数栈上的临时语义对象，结构和语义全部通过后才写回。因此：

- 空指针、错误长度、错误版本、保留位、非法模式和非法组合均不会部分修改 output；
- 测试使用完整哨兵副本和 `memcmp`，不只检查首尾字节；
- Decode 的不写回检查包含 Host 结构体全部字节，包括 padding。

### 3.3 uncertainty class

实现采用冻结候选公式：

```text
known=false                 -> class 31
known=true, S=0 or 1 us     -> class 0, 解码上界 1 us
known=true, 1 < S <= 2^30   -> ceil(log2(S))
known=true, S > 2^30        -> class 31
```

等级 0..30 解码为 `2^class us`；等级 31 返回 `known=false`。该值只表示发送端上界 `S`，接收端组合 `U` 不属于 RT-01。

## 4. 测试矩阵

| 配置 | 结果 |
| --- | --- |
| Windows GCC 14.2 Full Debug | 58/58 |
| Windows GCC 14.2 Lite Debug | 30/30 |
| Windows GCC 14.2 Nano Debug | 30/30 |
| Windows GCC 14.2 Full Release | 30/30 |
| Windows MSVC VS2019 Release | 30/30 |
| WSL Ubuntu 24.04 GCC ASan/UBSan | 30/30 |
| WSL GCC `-fanalyzer -Wall -Wextra -Wpedantic -Werror` | 30/30 |
| 默认产品：`UCN_BUILD_TESTS=OFF`、构建 all | `libucn_realtime.a` 不生成 |

定向用例覆盖：

- LOCAL、SYNCED、DEADLINE 三条 16 B Golden Vector 双向精确比对；
- 15/16/17 B 长度；
- 版本、两个保留位、NONE/非法 Mode；
- LOCAL 的 uncertainty、Domain valid、holdover、Domain ID 和 generation 禁止项；
- SYNCED 的 unknown uncertainty、Domain valid、Domain ID 和 generation 边界；
- uncertainty 的 unknown、0、1、2、1024、1025、`2^30`、`2^30+1` 和非法 class；
- 所有失败分支的完整 output 不写回。

## 5. 隔离与资源

Windows GCC Release 对单个 `ucn_realtime.c.obj` 的静态结果为：

```text
text = 1408 B
data = 0 B
bss  = 0 B
```

Release 静态 archive 文件大小为 2640 B；archive 文件大小不等于目标 MCU 的最终 Flash 增量。最终 Flash 仍需在具体交叉编译器、链接裁剪和产品固件下测量。

符号扫描确认：

- `libucn_realtime.a` 导出 5 个 `ucn_realtime_*` 公共符号；
- `libucn_core.a` 不含任何 `ucn_realtime` 符号；
- 生产 Node/Core/Routing/Transport/Service/Adapter/Port/Cluster 源码不引用实时 API；
- 源码未发现 `malloc/calloc/realloc/free` 或可变静态状态。

因此本轮没有为每个 Node 增加 RAM，也没有改变 Nano/Lite/Full 的 Node Storage Layout。

## 6. 自审中保留的限制

1. RT-00A R15 的文档整改和 RT-01～RT-07 软件候选均已完成全体自审，但仍等待外部审计；16 B ABI 还不是生产冻结版本。
2. RT-02～RT-07 已在各自独立 archive 和报告中实现；这些下游结果不扩大本报告对 RT-01 Codec 的签字范围。
3. 尚未接入生产 Endpoint/Service/Node，因此普通业务帧和默认产品资源占用不变。
4. 尚无 ESP32、STM32、CAN、UART、USB、Wi-Fi 或多跳时间精度实测。

## 7. 外审建议

外审应优先对抗以下边界：

- parser/builder 同时交换字段时 Golden 是否必然失败；
- 所有失败分支是否完整不写回；
- LOCAL 与共享 Domain 模式是否存在交叉放宽；
- class 30/31 是否可能发生移位未定义行为或低估；
- `EXCLUDE_FROM_ALL` 是否在默认产品构建中真正隔离；
- MSVC 对 `bool`、整数提升和公共头的行为是否一致。

在外部复审完成前，RT-01 保持候选状态。当前 MSVC、GCC、三 Profile、Sanitizer 和 Analyzer 软件矩阵已经补齐，但仍不自动授权生产路径或真实硬件实时性声明。

## 9. RT-A02 保守 uncertainty 聚合整改

外审指出“仅对最终数值量化”不能证明所有误差来源都已进入上界。整改后 RT-01
提供 `ucn_realtime_uncertainty_aggregate()`：调用方必须通过 known mask 同时证明
timer resolution、Link capture、filter residual、arithmetic rounding 四个固定分量，
并显式证明 Path asymmetry。任一分量未知、为零、checked-add 溢出或超过
`UINT32_MAX` 都失败关闭且不写 output。采样锁存误差保持为 RT-02 发送阶段的独立
增量，防止同步时钟误差和业务采样误差被混为一项。

定向回归覆盖完整聚合、缺失 bit、未知 Path asymmetry、零分量、溢出与失败输出
不写回；该修复不改变 16 B Envelope 布局。
