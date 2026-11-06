# RUST-SW-02 C1逐跳与Origin分离原语自审报告

## 1. 范围

2026-10-18，继续第九段后的离线软件工作。依据[共同合同15](../../10-理论与规划/建议方案/UCN_v6_C_Rust共用合同收口/15-C1不透明逐跳转发与计数器共同合同.md)，
新增Rust C1不透明Hop原语、实际动态Route/独立端到端Session三节点测试和内部计数器防重置。
未操作实机、COM/USB、烧录/复位、目标调试、暂存或提交/推送；保留继承工作树。

这不是自动业务Node、完整Flow或C/Rust运行时互通完成。C/Rust线上格式、AAD和信任合同
统一；**C1的C自动逐跳适配未实现**，本段没有把Rust代码能力追认给C。普通A1 C1数据头仍
13 B，不增加字段；O1/O2的16 B Origin Tag和H1的16 B尾部照常计入完整帧。

## 2. 小项完成后自审

| 小项 | 实现 | 区分错误实现的门禁 |
| --- | --- | --- |
| PH-01 | C1/Data、A0～A3、O1/O2/H1结构与固定caller-owned workspace | 短帧逐长度、零序号、不可见Control拒绝；不解析业务密文 |
| PH-02 | 私有pending Hop证明、整帧密码摘要及独立Replay | 换载荷后复用proof拒绝；TTL=1不转发；错ACL/坏Tag零输出 |
| PH-03 | A/C端到端Session与AB/BC邻居Session分离 | 真实Foundation回读发布两端Session；实际动态寻路、Driver及原子Adapter RX，B不执行Origin open |
| PH-04 | 终点独立Origin/Binding/Service ACL与Replay | 有效Hop包住损坏Origin，中继按权限转述，终点拒绝且明文哨兵不变 |
| PH-05 | C0/普通Origin/Hop独立内部高水位，Provider前consume | Hop reset反例先红；C0/O2 Provider失败后同事务也拒绝；不改变Flow自有Origin域 |
| PH-06 | 全矩阵、裁剪、MSRV、代码/合同和资源复核 | 结案后单独保存当前源码证据，不复用第九段最终哈希 |

## 3. 第一轮整体自审：信任与输出

1. Source包装必须使用本地地址；Relay必须回交实际open所得私有proof，不能改包再复用。
   出入事实必须同本地Binding、同now，TTL只减1，Origin内容和Tag逐字节保留。
2. 接收端仍分别验证相邻Peer和原始Peer。`C1HopOpened`只证明邻居，不授予Endpoint权限、
   调度Class提升或应用执行权；真实测试明确验证最终Principal仍为A，而非B。
3. 结构层允许O2不等于实测AEAD三节点通过。当前真正原语路径测试使用HMAC/O1；O2只做
   不透明结构检查，生产AEAD和硬件Provider不能据此签字。
4. 输出只有全部Provider与Gate退出成功后才写入。保留caller-owned scratch，未新增全
   Owner快照或整包自动栈数组。失败的Replay提交主动释放仍存在的reservation。
5. 全局唯一Ingress、业务队列、实际Driver Unknown退休和四级调度仍需在Node层统一，
   本段不以显式测试Coordinator替代尚未完成的自动Root。

## 4. 第二轮整体自审：计数器和负向测试真实性

1. 新增重置Hop counter回归，旧实现第二次返回成功66 B；修正后在外部burn/Provider前
   返回Replay且输出/外部counter不变。普通C0和Origin发送也统一增加内部高水位。
2. 高水位在外部burn成功后、Provider前写入；Provider失败不返还事务或序号。独立C0/O2
   测试同时覆盖Provider成功和失败，避免只验证成功路径。
3. 普通Origin按实际Session维护；经过精确Flow绑定的Origin由Flow Owner维护，不能把
   多个Flow误压到一个全Session序列。Hop始终是邻居Session共享序列。
4. 全量回归暴露旧RPC/Transfer负向fixture会重新封装不同内容却重复使用发送序号。
   修成合法新内容使用递增序号；网络乱序/原缓存重放仍使用原字节，不重新封装。
5. 需要验证恶意持钥Peer重复序号异内容时，用Host-only对抗Signer直接生成真实有效Tag，
   不从诚实发送API逃过高水位，也不绕过接收端验签或Replay。正式用例仍断言接收端拒绝。
6. 所有改变均在源码实际测试中验证，没有修改全局warning级别或用假认证布尔值换绿灯。

## 5. 最终证据与后续

最终[不可变矩阵](../软件证据/Rust/2026-10-18/software-matrix-sw02-c1-opaque-hop-final.json)，
UTC `2026-10-18T13:28:17.786186+00:00`，SHA256：

`A16FC001CE282EDA9B2358904A490BE673B7727D4426B4643DB35A9B2A503CA4`。

| 门禁 | 当前实际结果 |
| --- | --- |
| Rust Debug / Release / MSRV1.85 | 各354/354 |
| 全矩阵 / 源码稳定性 | 12配置exit0，source_stable=true |
| Clippy all-targets / fmt | 通过 |
| Cortex-M soft/hard与ESP32-S3 | compile-only通过，未运行实机 |
| Optional/Feature OFF | 30组合通过 |
| 共同C/Rust/Python raw / Admission Golden | 91向量 / 7项通过，未增加Wire字段 |
| 源码依赖独立逐文件复算 | 424项，0漂移 |
| C Full / Lite / Nano | Full再验108/108；Lite/Nano本轮重建路由相关各5/5 |
| Host Security sizeof | Nano2240 B、Lite10608 B、Full31152 B |
| 文档 / 公共C函数索引 | 151篇、404本地链通过；318项索引最新 |
| Git | diff-check无空白错误，仅既有CRLF提示；真实暂存区仍为空 |

上述sizeof只是Security Owner，不含其他Owner、workspace、Driver或最坏调用栈；workspace
256/512分别为1136/1904 B。新增live Session高水位每槽16 B，分别增加32/128/256 B；
没有把总协议RAM伪报成这些数字。C未在本段独立复跑全部MSVC/Clang/TSan，不沿用旧数字。

PH-01..06及两轮整体自审在上述分层Host范围完成；当前源码证据使用本篇的354项矩阵。
第九段348项及其哈希仍保留为路由控制历史切片，不因后续代码变化冒充当前完整候选。

RUST-SW-01..05仍有软件待办，尤其自动多跳业务Root、完整Flow、C自动Root、Group/Cluster
控制pipeline和跨语言Runtime互通。当前不是“软件全部完成，只剩实机”；独立外审、生产
密码、Flash/Witness掉电、物理Bearer及MCU资源仍分别HOLD。
