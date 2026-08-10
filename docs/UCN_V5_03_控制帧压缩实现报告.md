# UCN V5-03 控制帧压缩实现报告

> 日期：2026-08-11  
> 范围：HELLO 与 RREQ v5 载荷；RREP 和其他控制帧不在本项改写。

## 结果

HELLO Payload 从 4 B 改为 0 B，节点身份只使用已经受 CRC/Link 准入检查的 Frame Source。旧 4 B HELLO 不再被猜测兼容，必须按坏长度拒绝。

RREQ 删除重复 Origin，Origin 始终等于 Frame Source。v5 载荷为：

```text
Target(Address Width) | Request ID(4) | Route Cost(2) | Hop Count(1) | Flags(1)
```

| Profile | 新 RREQ Payload | 新 RREQ 整帧 | 旧 16 B Payload 整帧 | 节省 |
| --- | ---: | ---: | ---: | ---: |
| W0 | 9 B | 26 B | 33 B | 7 B |
| W1 | 10 B | 31 B | 37 B | 6 B |
| W2 | 11 B | 37 B | 42 B | 5 B |
| W3 | 12 B | 42 B | 46 B | 4 B |

Request ID 和 Cost 按设计继续保持 32/16 bit，不因短地址牺牲并发去重与跨介质 Cost。目标地址按官方 Profile 宽度编码，旧 16 B RREQ 精确长度不匹配时拒绝；协议 v5 + Wire Profile 共同确定唯一载荷版本，不做启发式解析。

## 验证

- 四档控制向量：HELLO 零载荷及 RREQ 9/10/11/12 B、目标地址与非零 Request ID。
- W0 A→B→C：路由发现、RREP、业务中继、RERR 与修复继续通过。
- 负向：旧 4 B HELLO、旧 16 B W0 RREQ 均返回 `UCN_ERR_MALFORMED`。
- 控制预算：W3 新偏移下的 RREQ Replay、Better Cost、Token 耗尽/恢复与来源隔离继续通过。
- Windows Debug：Full/Service ON 2/2、Lite/Service ON 2/2、Nano/Service OFF 1/1。

本项没有改变生产控制面认证边界；仅有 CRC 的 HELLO/RREQ 仍不能抵抗有意伪造，逐跳认证继续归 S02。
