# UCN V5-03 控制帧压缩实现报告

> 日期：2026-08-11  
> 范围：记录 V5-03 当时的 HELLO/RREQ 迁移；RREQ/RREP 的**当前格式以 V5-14 为准**，其余控制载荷以 V5-15 为准。

## 结果

V5-03 最初把 HELLO Payload 从重复 Node ID 的 4 B 改为 0 B，节点身份只使用已经受 CRC/Link 准入检查的 Frame Source。V5-10 混档极限测试发现 TX 档不能代表 RX Ceiling，因此当前 HELLO 使用独立 1 B `max_receive_wire_profile`；它不重复 Source，旧 0 B/4 B 格式和非法档位都按坏长度/坏能力拒绝。

RREQ 删除重复 Origin，Origin 始终等于 Frame Source。V5-03 当时的载荷为：

```text
Target(Address Width) | Request ID(4) | Route Cost(2) | Hop Count(1) | Flags(1)
```

| Profile | 新 RREQ Payload | 新 RREQ 整帧 | 旧 16 B Payload 整帧 | 节省 |
| --- | ---: | ---: | ---: | ---: |
| W0 | 9 B | 26 B | 33 B | 7 B |
| W1 | 10 B | 31 B | 37 B | 6 B |
| W2 | 11 B | 37 B | 42 B | 5 B |
| W3 | 12 B | 42 B | 46 B | 4 B |

Request ID 保持 32 bit。这里的固定 16 bit Cost 已在 V5-14 被累计 32 bit 语义取代，V5-23 又把线上 Cost Width 修正为 W0/W1/W2/W3=`3/3/3/4 B`；当前 RREQ Payload 为 10/11/12/14 B，RREP 为 10/11/11/12 B。详见 [V5-14/V5-23 实现报告](UCN_V5_14_长距离Cost与RREQ_RREP实现报告.md)。协议 v5 + Wire Profile 共同确定唯一载荷版本，不做启发式解析。

## 验证

- 本节的 RREQ 9/10/11/12 B 是 V5-03 历史验证值；当前四档向量由 V5-23 的 10/11/12/14 B 覆盖。
- W0 A→B→C：路由发现、RREP、业务中继、RERR 与修复继续通过。
- 负向：0 B/旧 4 B/非法或小于发送档的 HELLO 能力、旧 16 B W0 RREQ 均返回 `UCN_ERR_MALFORMED`。
- 控制预算：W3 新偏移下的 RREQ Replay、Better Cost、Token 耗尽/恢复与来源隔离继续通过。
- Windows Debug：Full/Service ON 2/2、Lite/Service ON 2/2、Nano/Service OFF 1/1。

本项没有改变生产控制面认证边界；仅有 CRC 的 HELLO/RREQ 仍不能抵抗有意伪造，逐跳认证继续归 S02。
