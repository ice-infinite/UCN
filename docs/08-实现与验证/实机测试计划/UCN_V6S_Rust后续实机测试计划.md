# UCN Rust 简化版后续实机测试计划

> 创建：2026-10-13。状态：WAIT USER HARDWARE NOTIFICATION。
> 本文是计划，不是已执行记录。没有用户通知不打开 COM、不烧录、不复位。

## 1. 先决条件

每个场景单独验证软件支持、Driver、固件 profile 和实际链接模块。历史 smoke/路由固件不含
新 Transport/Realtime/Group/Cluster，不能复用它的成功日志证明新模块。

主机上的 xtensa `cargo check` 只是编译，不等于固件 ELF 链接。开始实测前必须完成相关 BSP
固件构建、ELF/镜像哈希、目标段大小、模块列表和 Key/Flash 区划分检查。未知实际误差上界
时 Realtime 只允许诊断，不自行填入小数值让 Domain LOCKED。

## 2. 板卡与接线确认

历史记录出现过 B=COM53、C=COM58；用户更早也报过 B=COM52、C=COM55。端口编号可变化，
因此重新通知后必须按 eFuse/MAC 和固件角色确认 A～F，不把旧 COM 号当成实时事实。

最近已记录物理链为 `B—C—D—A—E—F`，不是字母顺序 A—B—C—D—E—F；必须以实际回读
确认。先跑单对 UART，再三节点，最后六节点。供电使用用户此前安排的独立供电，不加
人工错峰延时掩盖供电或 Driver 问题。所有节点同镜像，通过参数/MAC 配置角色。

## 3. 分阶段执行与退出条件

| 阶段 | 内容 | 必须保存的证据 | 失败行为 |
| --- | --- | --- | --- |
| HW-00 | 无通信 smoke、资源布局与任务栈水位 | ELF/镜像哈希、启动日志、模块/容量、栈峰值 | 不接下一级协议 |
| HW-01 | 两节点 UART Adapter | FIFO/ISR/notification/TX token/completion、RX CRC、背压 | 不以固定等待冒充 completion |
| HW-02 | O1/O2 Session 与真实密码 Provider | 双身份/Binding/Key/Suite、正确与篡改 Tag、旧 Replay | 未认证业务零副作用 |
| HW-03 | Reliable/Transfer/Service | 丢 ACK/片、重排/重复、有限等待、Request-ID、receipt、实际吞吐 | 未退休 holder 不复用；超时有限退出 |
| HW-04 | Flash/Witness 断电注入 | 每一步写入、重启后的 Slot/Witness/Body/Proof/ID | 不回退、不复用；不确定时 Fault |
| HW-05 | Realtime | GPIO/逻辑分析仪 T1～T4、timer read/resolution、asymmetry/capture bounds | 无可信界仅 Diagnostic，不 LOCKED |
| HW-06 | Group/Cluster（仅软件合同闭环后） | fanout/Tree/receipt、quorum、重准入、新票、Fence/reload | 不把本地模型 proof 当远端认证证书 |
| HW-07 | 三节点及六节点物理断线 | 拔线时刻、RERR/重发现、旧 Path/Generation 拒绝、C↔Rust | 不把软件失效注入当物理断链 |
| HW-08 | 性能及 24 h | 单源/多源、Q0～Q3、p50/p95/p99/max、goodput、CPU/栈/功耗 | 原始异常保留，不只报告平均值 |

物理断电/拔线等需要用户配合的步骤，在执行到该点时给出具体节点/接线/动作，不能把“有板子”
视为所有故障注入动作已经授权完成。Flash 擦写先确认目标分区及可恢复镜像。

## 4. 每完成一项的操作记录

记录日期、代码 commit/工作树候选哈希、固件 Hash、工具链、Profile/容量、真实 MAC/端口、
拓扑/波特率、供电、实验步骤、故障注入与恢复、原始日志位置、统计口径、PASS/FAIL 及
未覆盖边界。每项新建独立记录并在项目 OP 台账追加，不覆盖旧证据。

出现 Panic/WDT、资源泄漏、超时不退出、Witness/ID 回退、旧 Authority 恢复或 timestamp
误差低估时停止相关场景，先软件复现整改再继续，不用 Host PASS 覆盖实机 FAIL。

当前软件状态见 [RUST-10 报告](../版本演进/UCN_V6S_RUST_10_软件矩阵与后续门禁.md)。
