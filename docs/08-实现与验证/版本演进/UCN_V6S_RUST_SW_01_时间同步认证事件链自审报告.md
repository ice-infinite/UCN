# RUST-SW-01 第十二段：时间同步认证事件链自审

## 1. 本轮范围

2026-10-14；`v6-simplified-rust@03eb7eb` 加完整工作树。
按用户授权继续共同合同与 C/Rust 软件实施；不操作实机、COM/USB、烧录/复位或 Git 提交推送。
状态是固定直连 Host 子集 SELF-REVIEW PASS，不是顶层/外审/生产 GO。

[共同合同](../../10-理论与规划/建议方案/UCN_v6_C_Rust共用合同收口/05-时间同步认证报文与事件链共同合同.md)
保留既有四项 Opcode、12/24 B Body、128 B Domain Record、16 B 可选 Envelope，
普通数据头新增字节 **0**。A1/H0/O1 的四报文费用为 57+69+57+69=252 B，不含 Carrier。

## 2. 小项自审

| 小项 | 实现、回归与范围 |
| --- | --- |
| 当前 Domain 证明 | 实际 Foundation Consumer/Record/schema/generation/digest/正文核对；Pending Proof 拒绝，另一真实 Domain 同名配置也拒绝。Root 冻结完整 binding，不收 bool/View 冒充证明。 |
| 线性序号 | 实际 TimeDomain 许可；非 Clone，UNSYNCED 不重置，同代际不回绕，实际耐久换代才重置。 |
| 认证收帧 | Runtime 自己领取同一原子完整帧/真实 Timestamp，再接实际 Security、Opcode ACL、Replay；没有调用方 supplied opened/T2/T4 入口。 |
| 实际发送 | 内部构造四种 C0 正文并保护，实际 TimedLink reserve/submit/completion；T3 只属于内部 DELAY_REQ，不接受任意 TX；过小 MTU 在 counter/token 前拒绝。 |
| 样本生成 | 真实 T1～T4、固定 Peer/Path/域/代际/sequence；连续两轮进入 LOCKED。已知正 uncertainty、asymmetry 上界和交换处理期双钟漂移。 |
| 生命周期 | 原始截止；背压或 UNKNOWN 放弃样本且保留 token；缺 T1 不发 FOLLOW_UP，旧/future completion 拒绝；reopen 不继续旧路径。 |
| 退休 | peek→真实 retire→保留 receipt→Queue release→Sync ACK；失败保留 key；固定6 holder、持续游标和显式 stop。 |
| C 同步 | Domain validator 拒绝 aggregate.asymmetry 小于 Path 上界，定向回归验证拒绝输出不写；C 编解码和布局不改。C 完整认证 Root 尚未接通。 |

新增 Rust Runtime 正式5组集成测试；Realtime新增2组误差回归，并扩大原回归以验证
同代际序号不因 UNSYNCED 清零。Fake Flash、Crypto、TimedDriver 只提供测试证据，
真实 Security Owner、Foundation Owner、TimedLink、SyncOwner、TimeDomain 都实际参与。

## 3. 两轮整体自审

第一轮沿实际四报文与所有失败返回回读：修正预留 T3 的提交/完成区分，缺 completion
不得改变阶段；完整定向身份与当前 Session、Key、Policy、Origin、Link 必须匹配。
认证失败仍保留该 RX 的退休义务；状态拒绝不会发布 TimeSample。

第二轮按回调和媒体边界回读：Root 保存实际共享 CallbackGate，而非依赖调用方临时
给出一枚门；step/stop/take_outcome/retire 均拒绝回调重入。证明还绑定完整 Domain/
Consumer/Record，不让另一合法 Domain 替代。Driver 成功但 Queue/ACK 未完的 receipt
保留，Queue 回收后清 claim，避免失败重试重复退休或重复回收。

发现并处理的保守上界问题：C/Rust aggregate 的 asymmetry 分量不得低于实际冻结 Path；
Rust 样本要覆盖从 T2 到响应处理的双钟漂移，不把老 Offset 当刚采样。
已完成的 Domain 采集清理继续保留单调输出/接纳与分配高水位，未恢复旧 ABA 行为。

## 4. 验证证据

最终11项矩阵全部退出0：Debug/Release/MSRV各 **257/257**，Clippy/fmt、22裁剪组合、
41组三方字节Oracle、双Cortex与ESP32-S3 compile-only通过。393项源码/共同合同前后
稳定，独立SHA复算差异0。
[完整原始证据](../软件证据/Rust/2026-10-14/software-matrix-sw01-time-runtime-final.json)，
SHA256：`D1F019A7F4B878EB9A0C874DA3CF05D7E33C26F800DAAD772EF18299F0AA6B1C`。
此快照绑定本次全部393项依赖；后续改动必须重跑，不能继承当前数字或签字。
收口后文档门禁141 documents/348 links、C公共API索引318函数、diff-check通过。
四个本轮新增源码/测试/合同/报告另查尾随空白通过；真实Git暂存区保持为空。

原第十/十一段250/250快照是历史证据，不能作为新增 TimeRuntime 的签字依据。
成功重新构建的 Windows GCC Full C Realtime 定向 **9/9**，同一构建全量 **108/108**。
这是当前现有Full构建，不冒充所有Profile、全新工具链或真实MCU证据。

Host `TimeRuntime<128>` sizeof/align 为 **2600/8 B**，包含内部Sync/Control和六个holder，
不含外部Security/Foundation/TimedLink/Ingress/Queue/Driver存储与调用栈。
它是按MCU-first固定容量约束得到的Host数字，不能当作MCU总RAM或任务栈水位。
首轮矩阵曾在 time-only 裁剪的Clippy分支失败；已改为显式拒绝DeliveryAck、按Feature
编译其他不支持Body分支，并完整重跑，不引用失败轮的部分通过结果。

## 5. 未完成及放行边界

本子集是单一配置 STATIC_MASTER、固定安全直连 H0 Peer、专属 Timed RX Queue。
Path/Capability 来自可信静态产品配置，不冒充当前动态 Owner 的发布证明。
四报文是尽力同步，丢失/乱序可放弃本轮，不承诺可靠投递或任意网络下 LOCKED。
诊断语义模型保持有效，但诊断 Root 本轮不提供，不会去掉未知分量自动放行。

C 完整认证四报文 Root、组合数值算法互通、跨语言整链、动态/多 Peer/切路、
全部模块统一调度以及 Parent/Group/Cluster 尚未完成的软件工作继续开放。
没有新做 TSan/Miri、C 全工具链矩阵、实际 MCU 栈、真实 Flash/Witness、
硬件时间戳精度、无线非对称校准或性能长稳验证。保持外审与实机/生产 HOLD。
