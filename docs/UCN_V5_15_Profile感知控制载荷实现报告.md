# UCN V5-15 Profile 感知控制载荷实现报告

> 日期：2026-08-11
> 范围：RERR、PATH_INSTALL/REVOKE、Path Trace、Node Snapshot 中的 Node/Path 字段。

## 1. 设计规则

控制帧不再把 `ucn_node_id_t` 或 `ucn_path_id_t` 的 Host 宽度直接固定写入 Payload。编码和解析都先取得 Frame 的 Wire Profile 描述符，再用对应的 Address/Path/Session 宽度读写。

共同规则：

1. 固定发送模式继续使用 Node 的固定 TX Profile；Auto 模式选择能表达本帧全部字段且满足 Link MTU、Peer RX Ceiling 的最小 Profile。
2. 转发和回复保持请求的 Wire Profile，不在中继处静默升降档。
3. 长度必须与 Profile 推导值精确一致；旧固定宽度落入窄档时拒绝。
4. 授权顺序不因压缩改变：解析/可表达性 → Security/authorizer → Source/Session token → 写表或 Fanout。

## 2. 载荷布局

设 `A=AddressWidth`、`P=PathWidth`、`S=SessionWidth`。

| 控制类型 | Payload 布局 | W0/W1/W2/W3 大小 |
| --- | --- | --- |
| 普通 RERR | Unreachable(A) | 1 / 2 / 3 / 4 B |
| Path RERR | Unreachable(A) + OwnerSession(S) + PathID(P)；Owner Node 使用 Header Destination | 3 / 6 / 9 / 12 B |
| PATH_INSTALL | PathID(P) + Destination(A) + NextHop(A) + Lease(4) | 7 / 10 / 13 / 16 B |
| PATH_REVOKE | PathID(P) + Destination(A) | 2 / 4 / 6 / 8 B |
| Path Trace | Trace 固定域(8) + N×NodeID(A) | 请求含 1 节点为 9 / 10 / 11 / 12 B |
| Node Snapshot Request | Query/Flags 固定域 | 四档均 8 B |
| Node Snapshot Reply | Query(4) + NodeID(A) + NeighborCount(1) + Flags(1) + Reserved(2) | 9 / 10 / 11 / 12 B |

Path Trace 的回复随记录节点数增长，例如两个节点为 10/12/14/16 B；若下一跳 Node ID 不能由当前 Profile 表达，返回 `TRUNCATED`，不截断地址。

## 3. 自动选档和回复继承

- PATH_INSTALL/REVOKE 会同时检查控制目标、Path ID、Destination、Next Hop、Session、Hop、MTU 和对端接收上限。
- Path Trace 发起时选择能表达目标、源节点和记录上限的最小档；中继追加前再次验证本 Node ID。
- Node Snapshot 请求没有可缩窄的 Node/Path Payload 字段，但回复会继承请求 Profile，并按该 Profile 编码应答 Node ID。
- RERR 保持触发它的业务/路径帧 Profile，使上游能按同一域解释失效对象。

## 4. 验证

- RERR：四档长度/Profile、普通与 Path-scoped 语义、错误长度拒绝。
- PATH_INSTALL/REVOKE：四档 Golden、精确最小 MTU、坏长度、W0 旧 16 B 安装格式拒绝、真实 W0 安装/撤销、W3 Auto→W0 缩窄。
- Path Trace：四档请求/回复、精确 MTU、坏长度、W3 Auto→W0、记录截断。
- Node Snapshot：四档请求/回复、精确 MTU、坏长度、W3 Auto→W0、回复保持请求 Profile。
- Path 控制 authorizer 与 token 调用次数保持原顺序，非法帧不会提前消耗授权预算或写入表。
- Full Debug 单测通过，当前 Host Full `ucn_node_t=9464 B`、`ucn_link_t=40 B`；增加量来自累计 Cost/诊断待发状态的静态字段，不使用动态内存。

## 5. 经评估保持不变的 Schema

Candidate `PATH_PROBE/ACK/ACTIVATE` 的 Candidate ID/Epoch 是该控制状态机自己的固定标识，不是通用 Node/Path 地址字段；Policy Diagnostic 是独立、固定 32 B、分页/Section 化的诊断 Schema。两者不为追求表面压缩而在 V5-15 改线格式，未来若升级必须单独版本化和测试。

## 6. 安全边界

Wire Profile 只决定编码宽度，不授予管理权限。当前 Path/诊断 authorizer 和控制预算仍是接口与状态机门禁；真正的身份、密钥、逐跳控制认证、撤销与持久 Replay 继续归 S02。V5-16 只冻结 Authorized Class 设计，不能把 W3 或“大帧能力”解释为高权限。
