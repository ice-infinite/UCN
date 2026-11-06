# RUST-SW-02 Reauth能力交接与耐久分歧恢复自审报告

## 1. 本轮范围

2026-10-15，分支`v6-simplified-rust@03eb7eb`加继承工作树。用户要求保持C/Rust合同统一，
接通Reauth自动Capability交接及两端耐久提交不一致后的恢复，每小项完成自审，最后整体
自审，不操作实机。未操作COM/USB、烧录、复位、目标调试、Git提交/推送或暂存；保留
既有修改。Obsidian仅只读核对MCU-first、固定容量及Host无特权边界，旧功能状态不用作证据。

当前规范为[共同合同13](../../10-理论与规划/建议方案/UCN_v6_C_Rust共用合同收口/13-Reauth能力交接与耐久分歧恢复共同合同.md)。
C同步Codec和独立raw Oracle；Rust接实际Identity/Foundation/Security/Adapter/Capability。
不是C自动Root已经完成，也不是跨语言Runtime、生产或实机GO。

## 2. 小项和完成后自审

| 小项 | 实现与检查 | 即时自审 |
| --- | --- | --- |
| RC-01 | 双历史208 B V2、52 B局部扩展、225..273 B分片；C/Rust/Python独立逐字节Oracle | 长度、版本、保留位、非对称历史及零写入定向通过 |
| RC-02 | `max(双方Session/Origin/Hop)+1`，本端真实前驱精确检查；普通静态入口不放宽 | 双端2/1、1/2及3/1真实媒体恢复后收敛；普通入口跳代拒绝 |
| RC-03 | Security专用入口验证Challenge/Response双方完整身份签名及产品Session证明 | 任一签名坏、本端历史错、未来start和同代际分叉拒绝 |
| RC-04 | 自动构造Capability，唯一原子RX分派，保留ACCEPT缓存 | 实际127/128 B Adapter下成功；ACCEPT丢失后两lane重叠仍收敛 |
| RC-05 | 新Context精确ACL、双Binding/Authority逐次复验、Link生命周期 | 不继承旧ACL；父域截止和重开Link拒绝；回调重入零Driver/计数器推进 |
| RC-06 | 持续轮转发送/退休，单物理槽背压、UNKNOWN保留、统一stop | 自审发现槽争用并整改；实际Unknown停止后Token仍存在且不静止 |
| RC-07 | 裁剪、MSRV/Clippy/目标编译、C多Profile、不可变矩阵 | 最终冻结源码后执行，结果见结案段 |

本轮Runtime定向26项（上一轮16项，新增10项），Codec4项（上一轮3项，新增1项）。
恢复负向断言也集成在正式媒体恢复用例中，不用测试数掩盖多断言。Host身份和数据Provider
采用真实HMAC测试实现，未证明生产AEAD/Ed25519或硬件密钥材料。

## 3. 第一轮整体自审：耐久、信任与时间

1. 不能通过修改通用静态prepare允许任意跳代。保留其`+1`，新增窄入口，核对本端完整
   历史、双方身份签名、当前事实及真实Foundation Requirement/Proof。
2. 远端“耐久历史”是签名声明，不是本端Proof；它只能增大共同后继。同代际但不同摘要
   或Key历史拒绝，未知、零、耗尽和非单调组合失败关闭。
3. 签名绑定V2域、双方历史、Nonce、Binding、Link、角色和事务；只改摘要不重签不成立。
   未保留V1运行时fallback。先校验typed Record再派生候选，禁止Stage归一化隐藏非法输入。
4. 起点由constructor锁存；额外检查专用Security入口拒绝future start。重试不续租，
   新Session仍受双Binding/Authority实际截止约束。恢复历史不复活旧租约或Session。
5. 媒体测试从真实双槽/Marker/Witness恢复后构造新Security Owner；保留独立live Identity。
   前驱用实际双身份签名及Foundation提交制造一端已耐久状态，不声称整个设备断电链路
   已验证。连续单端写入也保留单调高水位，不保存无限历史集合。

## 4. 第二轮整体自审：交接和资源生命周期

1. 直接丢弃旧Root会丢ACCEPT重答义务。因此组合Root内保留原缓存，以实际认证Capability
   事实或原握手截止/预算停止重答。Established后网络重试耗尽不撤销耐久结果。
2. Adapter只领取一次原子RX；Coordinator私有分派到原签名验证或Capability实际Security/
   Replay/ACL入口。没有伪造OpenedPacket，没有把Driver物理Hint当Principal。
3. 小项自审复现单TX槽NO_SPACE，修成物理槽未退休时等待，再按持续游标轮转。停止时
   两lane分别退休，不丢Unknown或把缓存出队当作Driver已退休。
4. 后续自审发现stop可能先停止Capability再因旧Link拒绝，补全两lane变更前的物理代际
   预检；正式回归断言重开后stop拒绝且Capability阶段不变，旧Root不得关闭新Link。
5. 每次公开能力使用都先复验原Identity完整双Binding/Authority。没有向外暴露内部可变
   Capability Root来绕过父域；错ACL预检失败不发布半个交接结果。
6. 构造时检查既有定向Key槽和本轮角色一致，禁止在重认证中偷换方向槽或Suite。
7. 真实额外费用只在Reauth控制：V2比上段多52 B正文，32 B签名时每轮1716 B，非原1320 B。
   普通Data头新增0 B。Host Reauth-only2304 B；启用Capability-control后6840 B（即使
   未arm也内嵌预留）。不把编译通过或Host sizeof称为MCU最坏栈/RAM验收。
8. 最后交叉阅读共同JSON时发现Capability段仍写“Reauth交接待实现”，与当前Root矛盾。
   同步范围描述、补齐208 B完整layout后重新跑全部12配置；第一轮证据以pass1保留，不拿
   第一次源码快照冒充最终合同。没有因为机械门禁绿灯就跳过跨文档语义检查。

## 5. 最终验证与证据

最终源码冻结后两次完整矩阵均通过；第二次包含JSON范围表述的收口。正式结案证据为
[最终不可变矩阵](../软件证据/Rust/2026-10-15/software-matrix-sw02-reauth-capability-recovery-final.json)，
UTC `2026-10-15T08:14:34.757814+00:00`，SHA256：

`0B892D1918D71E3CC2E746702ADE121F62C9BC1348B3B50C348A7967854DB53A`。

| 门禁 | 本轮实际结果 |
| --- | --- |
| Rust Workspace Debug / Release / MSRV1.85 | 各329/329，通过 |
| 全矩阵 | 12配置exit0，source_stable=true |
| Clippy all-targets / fmt | 通过 |
| Cortex-M soft / hard float | 交叉编译检查通过，不是目标运行 |
| ESP32-S3 | compile-only通过，无开发板操作 |
| Optional/Feature OFF | 28组合通过，最小Core不拉可选Root |
| C/Rust/Python独立raw Oracle | 82向量通过，包括非对称历史及四片 |
| Admission共享Golden | 7项通过 |
| 最终源码依赖独立复算 | 417项全部匹配，0漂移 |
| C Full | 重建后108/108 |
| C Lite / Nano | 重建后各4/4定向：Security Session、Control Wire、Admission及Security栈门禁 |
| 文档 / API索引 | 最终结案后重验149篇/393本地链通过；318项公共函数索引最新 |
| Git / 未跟踪目标 | diff-check通过；本轮8个指定源码/文档目标尾随空白0；真实暂存区0路径 |

Reauth-only额外裁剪运行`bootstrap_runtime`39/39通过，Host资源分别2304 B与完整组合
6840 B/alignment8；Capability本身4392 B。C并未在本轮独立重跑所有MSVC/Clang/TSan配置，
不沿用历史数字冒充当前结果。第一轮[pass1快照](../软件证据/Rust/2026-10-15/software-matrix-sw02-reauth-capability-recovery-pass1.json)
只对应JSON范围描述修正前，不作为最终候选哈希。

RC-01..07及两轮整体自审在本段Host范围完成，等待独立外审；不自行签外审GO。
生产、真实Flash/掉电、物理Witness、生产密码、MCU最坏栈和实机仍HOLD。RUST-SW-02整体
仍IN PROGRESS，多Peer、C自动Root、跨语言Runtime互通和动态Route/Flow仍需软件实现。
