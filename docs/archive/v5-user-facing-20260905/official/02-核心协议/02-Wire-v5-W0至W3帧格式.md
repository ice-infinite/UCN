# Wire v5 W0 至 W3 帧格式

> 文档级别：`NORMATIVE`
> 实现状态：`CURRENT`
> 适用版本：`UCN_PROTOCOL_VERSION=5`
> 事实源：`ucn_types.h`、`ucn_frame.h/.c`、Frame Golden tests
> 最近核对：`a093862`，2026-08-25

## 共同前缀

所有 Profile 从以下 6 B 开始：

| Offset | 字段 |
| ---: | --- |
| 0 | Magic `0x55` |
| 1 | Magic `0x43` |
| 2 | 高 2 bit 为 Wire code，低 6 bit 为 Protocol version |
| 3 | Message Type / 静态 Endpoint |
| 4 | Traffic Class + Flags |
| 5 | Hop Limit |

后续依次编码 Network ID、Source、Destination、Sequence、Session、可选 Route Epoch、可选 Path ID、Payload Length、Payload、可选 16 B E2E Tag 和 CRC16。整数采用大端序。

## 四档描述

| Profile | 地址 | Payload Len | Route Epoch | Path ID | Route Cost | 最大 Hop | 基础头 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| W0 Local | 1 B | 1 B | 1 B | 1 B | 3 B | 4 | 17 B |
| W1 Edge | 2 B | 1 B | 2 B | 2 B | 3 B | 16 | 21 B |
| W2 Mesh | 3 B | 2 B | 2 B | 3 B | 3 B | 64 | 26 B |
| W3 Backbone | 4 B | 2 B | 2 B | 4 B | 4 B | 254 | 30 B |

Route Extension 使头变为 18/23/28/32 B；Path ID 形式为 19/25/31/36 B。二者不是任意叠加字段：Codec 按冻结 Flags 和结构验证。

## Flags

- `ROUTE_EXTENSION`：存在 Route Epoch/Cost 控制域；
- `E2E_PROTECTED`：Payload 后有固定 16 B认证 Tag；
- `DIAGNOSTIC`：诊断语义；
- `PATH_ID`：帧绑定 Path ID。

未知 Flags 拒绝。受保护帧缺 Tag、明文帧携带不应有的 Tag、长度不一致、Profile code/版本非法均拒绝。

## Profile 选择

Standalone Frame 中 `UNSPECIFIED` 保守解析为 W3。Node TX 可以固定发送档，也可显式启用 route-aware 最小档自动选择。自动选择从 W0 向上查找第一个同时满足字段、Flags、Payload、MTU 和接收上限的档位。

接收端可默认允许到 W3，也可按产品 MTU/安全策略收窄。发送档和接收上限是两个独立配置。

## 1. 解码为什么先看 6 B 前缀

Stream/CAN Source 需要知道一段 carrier 是否可能是 UCN Frame。前 6 B 提供 Magic、Protocol version、Wire code、Type、Flags 和 Hop。解码器先检查这些固定字段，再根据 Wire code 选择对应字段宽度和总长度计算。

不能把输入直接强转成 C 结构体，因为结构体存在对齐、端序和 ABI 差异。Codec 显式按 offset 读写大端整数，并用 golden tests 冻结线上字节。

## 2. 基础字段的作用

| 字段 | 发送方填写 | 中继是否可改 | 目标如何使用 |
| --- | --- | --- | --- |
| Magic/Version/Profile | Codec | 否 | 选择严格 decoder |
| Message Type | Node/Endpoint | 否 | 控制分派或业务 Endpoint |
| Traffic/Flags | Node/Policy | 仅合同允许的路由字段 | 队列、安全、诊断语义 |
| Hop | 发送方初值 | 每跳递减 | 0 时禁止继续转发 |
| Network/Source/Destination | Node | 否 | 网络隔离、去重、路由/目标判断 |
| Sequence/Session | Source Node/Security | 否 | 重复与重放域 |
| Route Epoch/Cost | 路由控制 | 按控制 Type 更新 | Route/Candidate 判断 |
| Path ID | Path owner | 通常保持 | 查逐跳 Path 表 |
| Payload Length | Codec | 否 | 精确边界校验 |
| Tag | E2E Provider | 中继不得改 | 目标认证 |
| CRC16 | 每次线帧编码 | 转发重编码后重算 | 检测单帧传输错误 |

## 3. Header 变体

“基础头、Route Extension、Path ID”是冻结布局，不是任意 TLV。Flags 决定 decoder 预期哪些字段和头长；出现未允许组合时拒绝。这样可在 MCU 上用常数 offset 解码，代价是新增 Wire 字段通常需要明确协议升级。

## 4. 编码流程

概念步骤：

1. 校验 Frame view 的字段合法域；
2. 选择/确认 Wire Profile；
3. 计算 flags 对应头长、Tag 和总长；
4. 检查 output capacity 与 Link MTU；
5. 清零或完整写入输出，避免保留未初始化字节；
6. 按大端写字段和 Payload；
7. E2E Provider 写 Tag；
8. 计算并写 CRC16；
9. 返回精确 encoded length。

任何步骤失败时不应返回一段“可发送的半帧”。

## 5. 解码流程

1. 检查最小长度和 Magic；
2. 解析 version/profile code；
3. 检查本地接收上限；
4. 根据 flags 得到唯一预期布局；
5. 读取 Payload Length 并验证 `header + payload + tag == input length`；
6. 校验 CRC；
7. 校验字段范围、Hop、Type/Flag 组合；
8. 全部成功后写 output view。

这能拒绝 v5 头后附加垃圾、截断 Tag、伪造 Payload Length 和未知 Flag。

## 6. 具体长度示例

W1 基础明文头为 21 B。若 Payload 为 12 B，则总长为 33 B。启用 E2E 后再加 16 B Tag，总长 49 B。如果同一 Link MTU 只有 44 B，这个受保护 Frame 无法直接发送；应缩小 Payload、选择能够减少头的合法档位，或使用 Transfer/其他 Link，不能删除 Tag。

## 7. W0 的意义和限制

W0 适合小地址域和小 Hop 的局部 MCU 网络。它节省的头字节有利于 Classic CAN/低速串口，但最大普通 Node ID 254、Hop 4。超过范围时提升档位，不应为了继续用 W0 而复用 Node ID。

## 8. Forward 与 E2E AAD

E2E AAD 保护不可变身份/安全字段；Hop 等逐跳可变字段不能简单纳入必须保持不变的 AAD。中继可以按合同更新 Hop/路由外层并重算线帧 CRC，但不能修改受保护 Payload/Tag。

## 9. Codec 能力与业务能力

Frame decoder 能解析 W3 不表示 Node 有所有 Type 的 handler。Codec 只回答“字节是否合法”；Node/Profile/ACL 再回答“本产品是否允许并能执行该消息”。
