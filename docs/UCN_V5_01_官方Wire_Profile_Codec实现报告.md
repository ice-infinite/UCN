# UCN V5-01 官方 Wire Profile Codec 实现报告

> 日期：2026-08-11
>
> 范围：官方 W0～W3 帧编解码、软件测试、Host 资源与版本基线；不包含 Node 固定域配置、自动选档、Gateway 或实机。

## 1. 结论

V5-00 和 V5-01 已完成。v4 最终底座已由 Git 提交、远端标签、本地 ZIP 和解压目录四种方式固化；v5 分支从同一个标签创建。当前源码版本为 `5.0.0`、协议版本为 5，W0/W1/W2/W3 使用唯一官方格式，用户不能自定义字段位数。

V5-01 只让 Codec 具备四档能力。`ucn_config_t` 还没有 Wire Profile 字段，Node 内部零初始化的新发帧按兼容规则落到 W3；因此不能把本项表述成“产品已经自动使用 W0 小帧”。该能力属于 V5-02/V5-05。

## 2. v4 可追溯底座

| 项目 | 证据 |
| --- | --- |
| 最终提交 | `main@25bcd22` |
| 远端标签 | `v4.0.0-final-before-v5` |
| 本地目录 | `E:\File\MESH\UCN_Backups\ucn-v4.0.0-final-before-v5-20260811` |
| ZIP | 同目录名 `.zip`，SHA-256 `B37D95AA0EA70C5C14DCEE5128CD135C4E0D8A707B8F593E99F69F5743B8C5CD` |
| 解压核对 | 249 文件、4,872,248 B；CMake 4.0.0、协议版本 4 |
| v5 分支 | `codex/v5-adaptive-wire`，从上述标签创建并已推送 GitHub |

## 3. 线上格式

Version/Profile 字节：高 2 bit 是 W0～W3 Code，低 6 bit 是协议版本 5。Traffic/Flags 字节：高 2 bit 是 Q0～Q3，低 6 bit 是协议 Flag；未定义 Flag 必须为 0。

编码顺序为：

```text
Magic(2) | Version+Profile(1) | Message(1) | Traffic+Flags(1) | Hop(1)
Network(W) | Source(W) | Destination(W) | Sequence(4) | Session(W)
Payload Length(L) | [Route Epoch(R)] | [Path ID(P)] | CRC-16(2)
Payload | [E2E Tag(16)]
```

| Profile | W | L | R | P | 最大 Hop | Base | Route | Path |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| W0 Local | 1 | 1 | 1 | 1 | 4 | 17 | 18 | 19 |
| W1 Edge | 2 | 1 | 2 | 2 | 16 | 21 | 23 | 25 |
| W2 Mesh | 3 | 2 | 2 | 3 | 64 | 26 | 28 | 31 |
| W3 Backbone | 4 | 2 | 2 | 4 | 254 | 30 | 32 | 36 |

各档全 1 Node 值保留为 Broadcast，所以 W0 可用单播 Node ID 是 1～254。Network/Session 可使用该宽度完整数值；Source 和单播 Destination 不能使用 0 或广播保留值。超范围返回错误，不做截断或 Alias。

CRC 位于实际 Header 最后 2 B，覆盖 CRC 之前的 Header、Payload 和可选 Tag。30 B 规范 AAD 新增 Profile/Version 绑定；Hop 和 Route Epoch 仍允许中继修改，Network/Source/Destination/Sequence/Session/Length/Path ID 保持不可变绑定。

## 4. 公共接口

- `ucn_wire_profile_get_descriptor()`：取得只读官方描述符。
- `ucn_frame_header_size_for_profile()`：按 Profile 与 Flag 推导唯一 Header 长度。
- `ucn_frame_max_payload_for_profile()`：同时受 Length 宽度、最大帧、Tag 和静态 Payload 上限约束。
- `ucn_frame_max_payload()`：为旧调用保留的 W3 查询。
- `ucn_frame_t.wire_profile`：0 表示 V5-01 的未指定兼容模式，编码时解析为 W3；解码后总是得到明确 W0～W3。

压缩 Header 不自动扩大 `UCN_MAX_PAYLOAD_BYTES`，默认静态 Payload 缓冲仍为 224 B。

## 5. 测试证据

| 门禁 | 结果 |
| --- | --- |
| Windows Full Debug / Service ON | CTest 2/2 |
| Windows Full Release / Service OFF | CTest 2/2 |
| Windows Lite Release / Service OFF | CTest 2/2 |
| Windows Nano Release / Service OFF | CTest 1/1 |
| WSL GCC 13.3 ASan+UBSan Full/OFF | CTest 2/2 |
| Feature Profile 最小帧门禁 | 本阶段历史值为 33/50/64 B；V5-14 后当前值为 33/46/64 B，32/45/63 B 编译拒绝 |
| 256 Node、M4、80‰ Duplicate | 102,400/102,400，重复业务 0，Route Loop 0，Harness 背压 0 |

`test_wire_profile.c` 固定四个完整 Golden Vector，并覆盖四档描述符、Base/Route/Path 长度、最大 Hop、广播映射、字段/Route Epoch/Path ID 溢出、坏 Flag、坏 CRC、AAD Profile 差异和 v4 显式拒绝。全量旧 Node/路由/安全/Service 测试同时回归。

同一规模模拟的 32 B Payload Wire Efficiency 为 50.383%；v4 固化记录为 48.785%。这是 Host 虚拟链路中默认 W3 从 32 B Base Header 降为 30 B 的软件结果，不代表 Wi-Fi/UART/CAN 实机吞吐。

## 6. Host 资源

| Profile | `sizeof(ucn_node_t)` | Core `.text` |
| --- | ---: | ---: |
| Nano | 2,648 B | 17,228 B |
| Lite | 5,888 B | 56,216 B |
| Full | 9,400 B | 113,996 B |

Node 对象与 v4 相同；三档静态库 `.text` 均比 v4 增加 4,096 B。该结果来自 Windows x64 GCC Release/Service OFF，只证明当前 Host 对象与代码增量；MCU Flash、RAM、栈、CPU 和功耗必须在目标 ELF/实机测量。

## 7. 下一项

V5-02 将把固定本地域 Wire Profile 和接收上限加入 Node 配置，并覆盖 Node 初始化、所有新发控制/业务帧、转发、HELLO/准入、MTU 与字段范围。完成前继续遵守：Node 新发帧默认 W3；应用若手工使用 W0～W2 Codec，必须自行保证同域字段全部可表示。
