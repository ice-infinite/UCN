# Link 与 Adapter 公共契约

> 文档级别：`NORMATIVE`
> 实现状态：`CURRENT`
> 事实源：`ucn_link.h`、`ucn_adapter.h/.c`、Link/Adapter tests
> 最近核对：`a093862`，2026-08-25

## Link 提供什么

每条 `ucn_link_t` 向 Core 提供：本地 ID、MTU、稳定基础 route cost、状态/metrics，以及 open/close/send 等 ops。Core 不读取串口寄存器、RSSI API 或 CAN 错误寄存器。

## Adapter 提供什么

Adapter 负责：

- 产品物理地址与逻辑 Peer/Link 绑定；
- 固定 RX Queue；
- 将完整 UCN Frame 交给 Node；
- 维护 queue/overflow/bad length 等统计；
- 可选 HELLO scheduler 与动态发现 Candidate Link；
- 将 Driver 事实转换为通用 metrics。

## Driver 提供什么

产品 Driver 负责引脚、时钟、DMA、中断、过滤器、peer 配置、Bus-Off/重连和实际发送。Driver 回调不得绕过 Link/Adapter 直接修改 Node 状态。

## 多实例

每条物理通道有独立 Link、地址绑定、Queue、Source、stats 和 context。多个 Link 可以指向同一 Neighbor Node ID。

## 错误

- `NO_SPACE`：固定 RX/TX Queue 满；
- `TOO_LARGE`：Frame 超过逻辑 MTU/缓冲；
- `LINK_DOWN`：Driver/Link 当前不可用；
- `CONFIG`：ops、MTU、ID 或 Port 版本不合法。

Adapter 接受一个 Frame 不代表远端发送成功；它只把 ownership 推进到下一层。

## 四层边界为什么不能合并

```mermaid
flowchart LR
    D[Driver/BSP] --> S[Source/Carrier]
    S --> A[Adapter Queue/Peer binding]
    A --> L[UCN Link]
    L --> N[Node Core]
```

- Driver 知道 GPIO、DMA、CAN mailbox、Wi-Fi peer handle；
- Source 知道如何从字节/物理帧还原一个完整 UCN Frame；
- Adapter 知道物理 peer 对应哪条 Link、如何有界排队；
- Link 向 Core 暴露统一 send/MTU/Cost/状态；
- Node 不应包含任何 ESP-IDF、HAL、Zephyr 或 Linux SDK 类型。

合并这些层会导致一套 UART 驱动直接改 Route 表，下一次接 CAN/RTOS 时只能复制整套协议。

## TX 所有权链

Node 调用 Link send 时传入一个已编码完整 Frame。Link/Adapter/Driver 必须明确是同步复制还是借用到 TX complete。当前产品实现不能在 send 返回后仍异步读取已经失效的栈缓冲。

典型安全选择：

1. Driver 在 send 内复制到固定 TX Ring，返回表示本地拥有副本；或
2. Adapter 保存固定 slot，收到 TX complete 后释放；
3. 队列无空间返回 `NO_SPACE`，Link Down 返回 `LINK_DOWN`；
4. 不能把“SDK 接口返回 queued”报告成远端收到。

## RX 所有权链

ISR/SDK callback 只把 Carrier/字节复制到固定 Source storage。Owner `service()` 解码出完整 Frame，调用 Adapter submit；Adapter Queue 接管副本后，Source 才可复用临时缓冲。Owner 再把 Frame 交给 `ucn_node_receive()`。

每一步都要有明确容量和错误统计，禁止把 SDK buffer 的指针跨 callback 保存而不确认其寿命。

## Link 状态与 Metrics

Link `up` 是硬可用门；Metrics 是软质量信息。Driver 报 Bus-Off/peer deleted 时应立即标 Down，不能仅把 failure rate 设高等待滞回。Queue、RTT、failure、busy、quality 都要带 valid 和 timestamp；缺失保持 Invalid。

MTU 是本 Adapter 能交付的完整逻辑 UCN Frame 上限，不等于一次物理 Carrier 长度。Classic CAN 可用多个 8 B Carrier 承载一个更大逻辑 Frame。

## Open/Close 与重配置

推荐生命周期：Driver 初始化→Source/Adapter 初始化→Link 注册→open/up→发现/通信→down→停止 RX/TX→close。重新配置波特率、CAN filter 或 Wi-Fi peer 时，应先 Fence 旧 Link，排空/撤销相关状态，再以新能力恢复，不能在线改 MTU 后保留旧 Path 承诺。

## 多实例隔离

UART1 和 UART2 即使使用同一驱动函数，也必须传不同 context、Ring、stats 和 Link ID。一个实例 overflow 不应清空另一个实例；CAN1 Bus-Off 不应让 CAN2 Down。唯一可共享的是只读 ops 表和同一 Protocol Owner/Event Runtime。

## Adapter 实现验收

- [ ] send 的 copy/borrow 生命周期明确；
- [ ] RX callback buffer 在返回后不被悬空引用；
- [ ] 只提交完整、精确长度的 UCN Frame；
- [ ] Queue/Ring/MTU 满载返回确定错误；
- [ ] Link Down 与软 Metrics 分开；
- [ ] 多实例 context 和统计互不污染；
- [ ] SDK 类型没有进入 Core 公共头；
- [ ] TX queued、物理 complete、远端收到三个阶段没有混淆。
