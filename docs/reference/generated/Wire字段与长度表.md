# Wire 字段与长度表

| 协议 | 版本/格式 | 固定或范围 | 当前状态 |
| --- | --- | --- | --- |
| Core | Wire v5 W0～W3 | Class 决定头长，总帧最大 256 B 默认 | 当前 |
| Cluster Current | v3 | 固定 32 B，Type 1～19 | 默认 FSM |
| Cluster Target Codec | v4 | 固定 40 B，Type 1～33 | encoder 默认关 |
| Stream Carrier | COBS | 变长字节流载体 | 当前 Source |
| CAN-FD Carrier | fixed carrier/padding | 完整 frame 承载 | 当前 Source |
| Classic CAN Carrier | START/CONTINUE | 多物理帧重组 | 当前 Source |
