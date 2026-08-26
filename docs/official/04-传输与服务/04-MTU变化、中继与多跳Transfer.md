# MTU 变化、中继与多跳 Transfer

> 文档级别：`NORMATIVE`
> 实现状态：`CURRENT`；跨簇 Transfer 尚未闭环
> 最近核对：`a093862`，2026-08-25

## Fragment 大小

`fragment_data_limit=0` 表示选择当前构建和路径允许的最大本片数据。若运行期发送发现 MTU 更小，Transfer 可按规则缩小到固定 16 B 下限。

小于最低 Fragment 数据能力的路径失败，不生成零数据片或无限循环。

## 多跳中继

Fragment 和 ACK 都是普通 UCN routed frame。中间节点：

- 验证 Frame；
- 按 Route/Path 转发；
- 不解释完整业务消息；
- 不创建端到端重组缓冲。

终点才按 Transfer ID 重组并返回 ACK。

## Route 切换

后续重发片可以走恢复后的 Route，但 Source/Session/Transfer ID/CRC 仍保持同一逻辑事务。接收端以 Offset 和累计 ACK 去重。

若切换后的共同 MTU 变小，新的 Fragment 可以缩小；已经确认的 Offset 不重置。

## 当前限制

- 当前 Transfer 面向普通 Core Route/Path；Federation Tunnel 内的透明 T32～T8K 尚未形成完整生产闭环。
- Link/Route 切换可能改变时延和乱序；窗口/重试必须按真实 Bearer 标定。
- ACK 能证明重组完成，不能证明业务 callback 已执行完毕。

## Fragment 数据上限如何计算

每次发送片之前，可用数据空间由下式共同决定：

```text
fragment_data_max = logical_path_mtu
                  - Core Wire header
                  - optional security tag/fields
                  - 14 B Transfer fragment header
```

还要受 `UCN_TRANSFER_MAX_MESSAGE_BYTES`、配置的 fragment limit 和 16 B 最小数据下限约束。不能只用 UART/CAN 的名义 MTU减 14 B，因为 W0/W3、安全 Flags 的 Core Header 不同。

## 为什么允许后续 Fragment 变小

Transfer ID 和 Offset 定义逻辑字节位置，不要求每片长度恒定。路由从大 MTU Wi-Fi 切到较小 MTU CAN 后，发送端可从累计 ACK 的 offset 继续，用更小片覆盖剩余区间。

已经 ACK 的 `[0,N)` 不重发/重置；未确认窗口按新上限重新切片。接收端根据 Offset 写入，因此无需知道发送端当前片大小。

## 多跳中的 ACK 路径

Fragment 从 A 经 B 到 C，ACK 是 C 发回 A 的普通 UCN Frame，可以使用当前合法返回 Route，不必和数据逐跳完全同路。ACK 身份仍绑定 Endpoint/Transfer ID；Source/Session 和安全策略必须正确。

如果返回 Route 失效，C 可能已经完整重组但最终 ACK 到不了 A。Recent Completion 状态用于在 A 重传迟到 Fragment 时再次回答完成，而不是重复向业务交付整条消息。

## 中继压力来自哪里

B 不分配 8 KiB 缓冲，但仍要为每片执行 RX、CRC、安全边界、Route 查找、重新入队和 TX。多跳吞吐受以下因素影响：

- 每片固定 Header 占比；
- Owner 每次 Source/TX budget；
- 同一半双工介质重复占用；
- 中继 RX/TX Queue 水位；
- ACK 返回和重试；
- 路径最小 MTU 决定的片数。

所以“中继不重组”节约 RAM，不代表中继没有 CPU/带宽代价。

## MTU 突降的失败边界

若新 Route 连 16 B 数据加所有 Header 都放不下：

1. 该 Route 对当前 Transfer 不可用；
2. 不生成 data_length=0 的 Fragment；
3. 不无限在 step 中尝试同一个失败尺寸；
4. 等待其他 Route/Path 仍受 Deadline；
5. 最终返回明确失败，保留接收端 Slot 到其 timeout 后释放。

## 路由变化与安全

E2E 保护的 Fragment 可由中继透明转发。允许变化的逐跳字段与受保护的 AAD 字段必须按 Core Security 合同处理。切路不能顺便删除安全 Tag 或更改被绑定的 Path ID；新路径不满足安全/capability 时必须失败关闭。

## 跨簇边界

当前 Transfer 的成熟范围是普通 Core Route/Path。Cluster Federation Tunnel 若要透明承载 T32～T8K，还需要冻结分片身份、MTU 传播、ACK 返回、Authority/Fence 和跨簇安全边界。没有该闭环前，文档不能把普通多跳 Transfer 外推为生产跨簇大消息。

## 实机测试矩阵

- 单跳固定 MTU下九档消息；
- 1/2/3 跳相同介质的吞吐下降；
- 不同 MTU 的 UART→CAN→UART；
- 传输中切到更小 MTU；
- 最终 ACK 丢失与 recent completion；
- 中继 Queue 满、Link Down 和 Route 恢复；
- Window 1/2/4/8 与并发 1/2 的 RAM、CPU、P95；
- 安全开/关后的有效 Payload 与吞吐差异。

每组结果要记录 Payload bytes/s、Wire bytes/s、片数、重传、ACK、Hop、CPU、堆/栈和固件 commit。
