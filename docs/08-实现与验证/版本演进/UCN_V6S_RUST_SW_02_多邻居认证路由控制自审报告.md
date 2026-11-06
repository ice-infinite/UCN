# RUST-SW-02 多邻居认证路由控制自审报告

## 1. 本段范围与证据层级

2026-10-18，`v6-simplified-rust@03eb7eb`加继承工作树。继续用户授权的离线软件推进，
逐小项自审并进行两轮整体自审。不操作COM/USB、烧录、复位、目标调试或实机，不提交、
推送或暂存；保留既有修改。知识库只读核对固定容量、MCU-first和非阻塞Adapter，源码优先。

规范见[共同合同14](../../10-理论与规划/建议方案/UCN_v6_C_Rust共用合同收口/14-多邻居认证路由控制与局部小MTU合同.md)。
这是**路由控制面**：真实三节点的认证RREQ/RREP/RERR及127 B局部分片。不是多跳业务交付、
Flow或整个RUST-SW-02完成；C只同步共同Codec、Registry及认证typed分派，不伪称C自动Root完成。
普通A1 C1数据头仍13 B，本段新增0 B。

## 2. 小项与即时自审

| 小项 | 实现 | 可区分错误实现的自审 |
| --- | --- | --- |
| MC-01 | 完整RREQ54/RREP77、共同发现键和报告方代际；C/Rust/Python独立raw向量 | 外层Source=B、正文Origin=A；不再接受9/29 B叶子作为完整网络消息 |
| MC-02 | 固定多Peer Coordinator，真实Identity/Foundation/Security/Capability及原子Adapter RX | B的两个Peer共享物理Link；错Tag、物理判别值、父域和实际上下文拒绝 |
| MC-03 | 同一内层发现、全新外层C0/H1的有界重试 | 整条RREP丢失后收敛，不通过关闭Replay换取重答 |
| MC-04 | 通过认证RREP回执映射远端Route Generation与本地Route/Link | 双方代际/Link编号不同仍正确处理RERR；不得跨节点比较本地编号 |
| MC-05 | 0206、14 B前缀、2～4片固定99 B重组 | 127 B实际Adapter完成三节点发现和RERR；不发布半条路由 |
| MC-06 | bundle高水位、精确认证上下文、原始绝对Deadline | 非首片换组拒绝；高首片全清旧字节但不续期；返回两段各丢首/末片仍可收敛 |
| MC-07 | 固定轮转、真实Token退休、反向转发完成义务 | NOT_SUBMITTED可重试；Unknown停止保留；反向记录到期不能抹除实际Driver义务 |
| MC-08 | Feature OFF、目标仅编译、MSRV/Clippy、C多Profile和源码指纹 | 最终矩阵与独立复算见结案段；不把Host sizeof称为MCU最坏栈 |

Runtime正式定向15项；Routing独立Codec包含字段/长度、输出零写、全部地址宽度及固定Seed
4096次变异。所有认证输入来自实际Security验证，不用`authenticated=true`或伪造OpenedPacket。
Host密码Provider为测试实现，不能替代生产Provider验收。

## 3. 第一轮整体自审：身份、域与长度

1. 逐跳Session只认证当前邻居。完整正文冻结原始发现键；Principal转述只是路由提示，
   不成为端到端业务授权。源Route域与逐Peer密码Session分离。
2. RREP报告者的本地代际需要映射。C在安装时原子保存，Rust保存完整回执；RERR按完整
   RouteDomain、发现因果、实际报告者及远端代际精确匹配，再转换本机编号。
3. 自审发现旧`minimum_payload_budget`名称与实际完整帧比较不一致，C/Rust统一改成
   `minimum_frame_mtu`，删除误导别名。100 B业务加A1 C1/O1/H1实际需要145 B，不能把100
   当完整帧要求。线上9 B叶子不变。
4. 逐Peer最大帧不能只看远端Capability，还要取本地Frame/Processing上界、实际Adapter和
   固定缓冲最小值。本地共同Capability精确冻结，后续更换不得继承原样本。
5. 分片不为普通数据增加字段；费用只在放不下的路由控制。只允许RREP/RERR，不接受任意
   大正文或无限片数，不把分片前缀误称为全部Wire开销。

## 4. 第二轮整体自审：丢包、容量和生命周期

1. 自审发现源端仅一个任务槽、RREQ实际Token未退休时，RREP不需要发送槽却被容量预检
   拦住。改为按消息实际下游工作预检，正式单槽回归能在原Holder仍占槽时接受回复。
2. 反向记录到期只撤销业务有效性，不能删除未完成ReplyForwardHandle。Driver真实终态、
   Token退休后才交还RouteOwner；重开/停止不是Unknown消失的证明。
3. 空闲访问也更新单调时间高水位，不能用一次无工作访问后再倒退时刻延长租约。
4. 第二次完整矩阵虽已全绿，仍进一步注入127 B返回链路丢片。旧实现拒绝所有组装替换，
   导致首片已到、末片丢失时占槽直到原发现过期，新的回复无法收敛。新正式回归先对旧
   实现稳定失败，再整改：同一实际认证上下文的更高bundle首片全量清旧字节/位图，但
   **保留原首片Deadline**。旧尾片、非首片换组、跨上下文和等值到期仍拒绝，不增加ACK。
5. 新增四种丢片顺序及临近到期替换回归，15项Runtime全部通过。前两份矩阵保存为历史
   pass1/pass2，不冒充包含这项语义整改的最终证据。
6. C的0206只登记/Codec。生产通用入口显式拒绝，防止“Registry有Opcode”被误认成已有
   认证重组Root。C/Rust运行时能力差距仍公开登记，不靠测试数遮盖。

## 5. 验证与继续边界

最终源码冻结后的[不可变矩阵](../软件证据/Rust/2026-10-18/software-matrix-sw02-multi-peer-route-control-final.json)
UTC为`2026-10-18T13:06:35.558333+00:00`，SHA256：

`D6C6BA58E8F3448568D80879662B5D0B82CF5991E15453A1F324C196FA5216C3`。

| 门禁 | 实际结果 |
| --- | --- |
| Rust Debug / Release / MSRV1.85 | 各348/348，通过 |
| 全矩阵 | 12配置exit0，source_stable=true |
| Clippy all-targets / fmt | 通过 |
| Cortex-M soft/hard及ESP32-S3 | 仅交叉编译检查通过，没有目标运行 |
| Optional/Feature OFF | 30组合通过 |
| C/Rust/Python独立raw Oracle | 91向量通过 |
| Admission共享Golden | 7项通过 |
| 最终依赖独立逐文件哈希复算 | 422项，0漂移 |
| C Full | 重建后108/108 |
| C Lite / Nano | 重建后各5/5路由/Flow/Control定向 |

Host RouteRoot小实例`P=2/Q=4/B=128`为4656 B，对应三节点测试实例`P=3/Q=12/B=256`
为11816 B，alignment8；这里只是该Root的sizeof，不含Identity/Security/Adapter等Owner，
也不是MCU整体RAM或最坏栈。前两轮快照保留为历史证据，不覆盖最终文件。

本段不能解除独立外审、生产密码、真实Flash/掉电、MCU栈/WCET、物理Bearer和实机门禁。
软件仍需C自动多Peer Root、独立Origin/Hop多跳业务、Flow真实控制接线、Group/Cluster及
统一Ingress和跨语言Runtime互通。RUST-SW-02维持IN PROGRESS。
