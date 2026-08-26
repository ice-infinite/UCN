# 自定义 Port、Adapter 与 Source 模板

> 文档级别：`GUIDE`
> 实现状态：基于当前公共 API
> 最近核对：`a093862`，2026-08-25

## 新 Port

实现内容：

- `now_ms`；
- 可选 random/counter persistence；
- Task critical pair；
- 需要 ISR enqueue 时的 ISR token critical pair；
- notify/wait 调度 glue；
- 唯一 Owner 的 step/run wrapper。

Port 不包含具体介质 Driver。

## 新 Adapter

实现内容：

- 每实例物理地址/context；
- Link ops 和逻辑 MTU；
- 固定 RX Queue 或 Source→Adapter submit；
- open/close/send/liveness；
- 通用 metrics；
- 全部错误和 overflow stats。

Adapter 不实现业务 Endpoint、Route Policy 或 RTOS task。

## 新 Source

实现内容：

- 调用者 storage 和固定 Ring；
- ISR/task push；
- `service()` 每次有界消费；
- Carrier framing/reassembly；
- 只提交完整 UCN Frame；
- malformed/overflow/timeout/health stats。

## 必测项

完整帧逐字节交付、截断/拼接/错位、Ring 满、连续消息、ISR/Task 竞态、重入、超时、Driver Down、MTU 边界和多实例隔离。

## 先判断你要扩展哪一层

| 需求 | 应实现 |
| --- | --- |
| 新 RTOS/调度方式 | Port/Owner scheduler glue |
| 新物理介质但一次给完整 Frame | Adapter + Link Driver |
| 字节流/小物理帧需要组帧 | Source/Carrier + Adapter |
| 新路由/业务语义 | 不是 Adapter，应进入对应 Core/Extended 设计 |

把新 SPI Radio 的组帧写进 FreeRTOS Port 会造成平台与介质耦合；正确做法是 SPI Radio Source/Adapter 可被裸机、FreeRTOS、Zephyr 共用。

## 自定义 Port 步骤

1. 选择唯一 Protocol Owner 上下文；
2. 提供单调 `now_ms`；
3. 提供 Task notify/wait，wait 必须有界；
4. 若直接 from_isr submit，提供独立 ISR critical token pair；
5. 映射 Event Runtime 的 scheduler hooks；
6. 定义 shutdown/restart；
7. 建 Host Fake 类似测试和目标平台 smoke；
8. 记录 SDK/BSP 不在 Core 的边界。

## 自定义 Adapter 步骤

1. 冻结物理 peer key 与 Node/Bearer 绑定；
2. 选择 logical MTU 和 Carrier；
3. 定义 send 的 ownership/complete/error；
4. 建固定 RX/TX storage；
5. 映射 open/close/up/down；
6. 归一 Metrics valid/timestamp；
7. 注册独立 Link instance；
8. 做多实例和能力变化测试。

## 自定义 Source 的 service 合同

`service()` 一次最多处理预算内 Carrier，输出只能是精确完整 UCN Frame。剩余输入保持 pending。malformed 记录应丢弃到可重新同步边界，不能污染下一帧；超时清理只清对应 slot。

## 输出不写回和原子初始化

公共 init/parse/resolve API 对非法参数应在修改 caller 对象前完成验证，或先构造临时对象再提交。测试用不同哨兵填充 output，失败后逐字节 `memcmp`，不能只检查首尾字段。

## 推荐目录

```text
include/ucn/adapters/<medium>/...   通用、无SDK公共合同
src/adapters/<medium>/...           Carrier/Source实现
product/bsp/...                     SDK/HAL Driver glue
tests/test_<medium>_source.c         Host确定性测试
docs/official/...                    接入与限制
```

如果实现只适用于一个产品 SDK，优先留在产品仓库；只有通用 Carrier/合同才进入 UCN Core。

## Review 清单

- API 命名/版本/对象大小稳定；
- 无动态分配或有明确产品层 pool；
- Task/ISR/reentrancy 合同完整；
- 所有长度、offset、DLC、padding 严格；
- 多实例无全局可写单例；
- Link hard state 与 soft metrics 分离；
- 默认 CMake 不因实验 Adapter 增加产品成本；
- Unit、Sanitizer、Analyzer、Release 和目标硬件证据齐全。
