# Stream Source 与 COBS 载体

> 文档级别：`NORMATIVE`
> 实现状态：`CURRENT`
> 适用介质：UART、RS-485、USB CDC 或其他可靠字节流
> 最近核对：`a093862`，2026-08-25

Stream Source 使用调用者存储的固定 Byte Ring。Driver/ISR 将收到的字节写入 Ring；Owner `service()` 从 Ring 读取 `COBS encoded bytes + 0 delimiter`，恢复一个完整 UCN Frame，再提交 Adapter Queue。

## 为什么用 COBS

- 0 可以作为明确 Frame 边界；
- Payload 任意字节不会与 delimiter 混淆；
- 丢字节后可在下一个 delimiter 重新同步；
- 不需要动态内存。

## 产品责任

- 初始化 UART/USB、波特率、数据位、DMA、中断和引脚；
- RS-485 控制 DE/RE 和半双工 turnaround；
- 把 RX bytes 推入 Source；
- 把 Link send 的完整 Frame 做 COBS encode 后写 Driver；
- 根据 Ring overflow/COBS error/Frame length stats 调优。

## 线程边界

ISR 只 push bytes 和 signal；COBS decode、Frame peek 和 Adapter submit 在 Owner 中执行。

## 多 Stream

每个串口/USB 实例拥有独立 `ucn_stream_source_t` 和 storage。不能共用一个 Byte Ring 区分多个物理流。

## TX 编码路径

Link send 收到完整 UCN Frame 后：

1. 检查 Frame 长度不超过本 Link logical MTU；
2. COBS 编码到固定 TX scratch/slot；
3. 追加单字节 `0x00` delimiter；
4. 把编码字节交给 UART/USB Driver；
5. Driver 同步复制或持有到 TX complete，所有权必须明确；
6. 失败返回 NO_SPACE/LINK_DOWN/TOO_LARGE 并更新对应统计。

不能把未编码的任意 UCN Payload 直接写串口，也不能每次 `printf` 文本混入同一二进制流。

## RX 解码与重新同步

Owner 从 Byte Ring 累积到 delimiter：

- 空包、非法 COBS、解码超过 Frame 缓冲：丢弃当前记录；
- 合法解码：先 peek 精确 Wire Profile/长度，再交 Adapter；
- 缺字节/插入脏字节：当前记录 CRC/COBS 失败，但下一个 delimiter 重新同步；
- 长时间没有 delimiter 且超过 scratch 能力：丢弃到下一个 delimiter并计 oversize。

COBS 只提供边界恢复，不代替 Frame CRC 和安全认证。

## Ring 容量如何估算

Ring 要覆盖 Driver/ISR 到 Owner 最坏唤醒期间可能收到的字节：

```text
ring_min > byte_rate × worst_owner_latency
           + max_encoded_frame
           + safety_margin
```

3 Mbit/s UART 8N1 约 300 kB/s；Owner 最坏晚 2 ms 就可能积累约 600 B，显著大于一个 256 B Frame。仅按“最大帧 256 B”配置 Ring 可能在调度抖动时溢出。

## DMA 与中断建议

- DMA circular/idle-line 可批量提交新字节范围；
- ISR 只更新 write index/复制必要数据并 signal Source；
- Owner 做 COBS 和 CRC；
- TX 使用 DMA 时必须处理 busy/complete/error；
- USB CDC callback 同样只是字节流来源，不能假设一次 callback 就等于一个 Frame。

## RS-485 半双工

产品 Driver 还要处理 DE/RE、TX drain、turnaround 和冲突。Link send 返回 queued 时不能立刻拉低 DE；要等最后 stop bit 真正发完。接收窗口和 Heartbeat/ACK 定时需考虑 turnaround。

## 串口调试输出冲突

若同一 UART 同时输出人类日志和 COBS UCN，日志中的任意字节会破坏 Frame。产品应使用不同 UART/USB interface，或在更高层定义严格复用通道；官方 Source 不解析混合文本。

## 验证清单

- [ ] 0～255 字节值的随机 Frame COBS round-trip 精确一致；
- [ ] 连续 40/256 B Frame 无丢尾/拼接；
- [ ] 丢字节、插字节、非法 code 后能在下一个 delimiter 恢复；
- [ ] Ring wrap/满载/from_isr 竞态无越界；
- [ ] UART/USB callback 分块方式不影响 Frame 边界；
- [ ] RS-485 DE 时序在真实波特率下验证；
- [ ] 业务吞吐报告包含 COBS 与 8N1 开销。
