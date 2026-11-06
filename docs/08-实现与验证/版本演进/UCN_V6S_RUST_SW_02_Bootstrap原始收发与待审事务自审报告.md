# RUST-SW-02 第二段：Bootstrap 原始收发与待审事务自审

## 1. 范围与结论

日期：2026-10-15。分支 `v6-simplified-rust`，基线 `03eb7eb` 加完整工作树。
按用户授权补齐共同控制正文、同步 C/Rust，每小项自审后再做两轮整体回读。
没有操作实机、枚举 COM/USB、烧录、复位、提交或推送；保留原有工作树修改。

本段自审通过，**RUST-SW-02 仍 IN PROGRESS，整体外审/生产 HOLD**。当前闭环是
`实际原始 RX → Bootstrap Codec → Cookie 验证 → CookieVerified Pending`，不是
完整 JOIN。没有授予 Principal、Address Binding、ADMITTED、Session 或 Authority。

权威正文见[共同合同](../../10-理论与规划/建议方案/UCN_v6_C_Rust共用合同收口/07-Bootstrap原始收发Cookie与待审事务共同合同.md)。
本段不增加普通数据头，不增加 Opcode，不引入旧草案兼容路径。

## 2. 逐项实施与自审

| 小项 | 实施 | 自审与实际限制 |
| --- | --- | --- |
| BOOT-01 | Challenge 的 issuer-local Link 域 | 使用原8 B保留区中的6 B，Body仍40 B；双方Link 1/3与2/9实际握手；旧代际、非法ID/保留位拒绝 |
| BOOT-02 | 完整前序摘要与可信时间 | 完整32 B SHA-256绑定两份canonical外层帧；Owner把now传给Cookie verifier；三方独立字节/hash Oracle通过 |
| BOOT-03 | 实际原始收发 Client/Listener | 自己领取实际Adapter原子完整RX，再调用严格Codec和Admission；没有调用方伪造的opened/authenticated DTO |
| BOOT-04 | 固定Cookie/资源/密码预算 | Cookie独立最多16 B类型；当前Root固定16 B；单Pending、单回复缓存；签发与验证分别限流，验证每次最多一次Provider调用 |
| BOOT-05 | 丢包、背压、早到与生命周期 | Challenge丢失、在途重复HELLO、早到Challenge、NOT_SUBMITTED精确重试；不清在途Token、不刷新原deadline；取消失败/UNKNOWN保留Holder |
| BOOT-06 | reopen、停止与可裁剪边界 | reopen旧代际Echo在MAC前拒绝；旧Root不能停止新Link；Faulted当前Link仍可stop；排空后才quiescent；bootstrap-only/OFF纳入矩阵 |

## 3. 共同 Wire 与真实运行路径

[Rust Root](../../../rust/crates/ucn-runtime/src/bootstrap_runtime.rs) 使用专属单Link的
实际 Adapter，输入完整原始帧和物理metadata；Driver区分值只做去复用，不充当认证身份。
[帧合同](../../../rust/crates/ucn-runtime/src/bootstrap_wire.rs) 固定C0、Control、Q0、
BestEffort、O0/H0、HopLimit1、固定Realm、Source0、Link-local目的地、Binding0。
Body JOIN transaction与外层C0 transaction必须完全相同；禁止转发。

HELLO40 → Challenge40 → Echo98。在A1下完整帧69/69/127 B，首次无丢包合计265 B。
这是每次启动的认证前交换，不是逐数据包开销，也不是完整JOIN费用。
当前Root不做Bootstrap分片；A2/A3、128 B Link因最大Echo超MTU而在构造时拒绝。

旧合同存在跨节点命名空间错误：Echo需要接收端Link域，Challenge却没有提供它，C发送
入口还要求它等于发送端本地Link。现已同步C/Rust：Challenge携带issuer域，客户端原样
回显，接收端与自己实际当前Link核对。本地发送Link独立，不依赖两端编号恰好相同。
该修订仍使用原保留区，Challenge长度不增加；issuer域本身不是Authority认证。

完整前序摘要使用SHA256，输入为域分隔、两帧总长度和两份精确canonical帧。
C的完整SHA-256与既有128位截断共用一个后端，增加标准abc向量与部分别名零写回回归。
Cookie Provider必须验证Realm/宽度、nonce/transaction、issuer/密钥域、时间桶及完整前序
摘要；不能只看本机缓存。verify SPI现在显式接收Owner捕获的可信now。

测试Cookie参考格式为4 B bucket + 12 B HMAC；固定测试密钥不代表生产密码Provider。
时间桶采用checked floor(now/bucket)+1，启动首个非零时刻即可服务，不添加启动等待。
产品仍需定义密钥轮换、Cookie窗口及密码计算成本边界。

## 4. 两轮整体自审

第一轮按合同、因果与资源逐段回读：

- 分离客户端本地Link与签发方回显域；C/Rust及独立Python重建相同Challenge字段。
- Cookie不再占128 B通用Evidence容器；限制16 B，通用Evidence只留给后续认证阶段。
- 无HELLO缓存的合法Echo仍可无状态验证；错Link先拒绝，错摘要/过旧桶由Provider拒绝。
- CookieVerifier预算独立于签发预算，按配置rate累积且不超过burst，不把burst误当每秒速率。
- CookieVerified仅是私有待审元数据，没有转成准入成功或发布耐久证明的捷径。

第二轮按物理生命周期、裁剪与证据回读：

- 收到早到Challenge不能删除仍Submitted的HELLO Token；只有实际退休后才排队Echo。
- 缓存回复飞行中再次收到HELLO保留requeue闩锁，完成事件不能把重发义务覆盖掉。
- UNKNOWN/InDoubt不猜测完成；取消失败保持Token，后续重试，不能伪造quiescence。
- 当前Faulted Link允许停止隔离；已经reopen的旧Root不能禁用新Link的接纳。
- 原deadline不续；Pending过期由显式step推进，满槽/坏输入不隐式驱逐。
- 初期编译/Oracle fixture问题已修正后重新跑完整矩阵，没有关闭警告或跳过失败项。
  最终矩阵期间源快照稳定，未把早期局部成功当作最终结果。

## 5. 本轮最终验证

| 门禁 | 最终结果 |
| --- | --- |
| Rust workspace Debug / Release / MSRV 1.85 | 各269/269 |
| 全量Clippy / fmt | PASS，Clippy -D warnings |
| Cortex soft/hard no_std | 两目标check PASS |
| ESP32-S3 | compile-only PASS；未烧录/运行 |
| Optional/OFF/独立依赖 | 24组合PASS |
| C/Rust/Python独立Codec/摘要 | 52向量PASS，含4项Bootstrap完整帧/前序摘要 |
| Admission/Capability既有共享Golden | 7项PASS |
| 本轮新Root正式回归 | 5项PASS，均走实际Adapter/原子RX/Driver |
| C Windows GCC Full | 成功重建后全量108/108 |
| C Lite / Nano | 本轮Admission/Composition/Control Wire/Common定向各4/4 |
| 最终矩阵/来源快照 | 12配置全部exit0，400项依赖稳定，PowerShell独立复算0差异 |

这是本地Host回归和目标编译，不是fresh全工具链C矩阵。本轮未运行MSVC、WSL
ASan/TSan/Miri，也没有MCU最坏栈、Flash原子性或掉电证明。

Host x64测量：`BootstrapClient<'static,128>` sizeof536 B，Listener1176 B，alignment8；
小Cookie17 B、alignment1。均不含外置Adapter/Ingress/Driver及调用栈，不能相加后称为
MCU实际总RAM或任务栈。bootstrap-only不引入Service/Realtime/Group/Cluster和durable
业务模块；当前基础Runtime/Identity类型仍有Persistence依赖，未宣称此组合完全无持久化crate。

[不可变原始矩阵](../软件证据/Rust/2026-10-15/software-matrix-sw02-bootstrap-pending-final.json)
SHA256：`87555501B3B12DCC874DC333D0D667A4DF237443F171182DEBD22813E5C8E8A6`。
逐项独立复算包括未跟踪源码；没有覆盖之前的Capability或其他历史证据。

收口后文档门禁143篇/360链接、公共API索引318项通过；本段共同合同/导航/报告另核验
19个本地链接、5个新增手写文件尾随空白为0。真实Git暂存区为空；diff-check无空白错误，
仅有既有CRLF转换提示。机械通过不等于完整JOIN或安全外审签字。

## 6. 下一步和放行边界

下一软件任务是把双向身份认证、完整Authority/Device Evidence、Offer/Commit、实际
Identity/Foundation发布与reload证明、Final Commit及Session发布接入同一线上事务。
还包括已绑定设备的Peer Reauth、多Peer公平性和失败/重启；不能用bool跳过后半段。

C已同步共同Codec、Provider SPI和已有Bootstrap分派，**不是**完整C自动JOIN Root；
三方Oracle也不是完整C↔Rust Runtime互通证明。动态Route/Flow、其他可选模块控制及
全模块共同调度仍是软件待办，不是只剩实机。

本轮自审不能代替独立外审。真实Flash/Witness、掉电、生产密码、物理Bearer、精度、
性能功耗、长稳及目标栈仍独立HOLD；普通数据头维持0新增字段。
