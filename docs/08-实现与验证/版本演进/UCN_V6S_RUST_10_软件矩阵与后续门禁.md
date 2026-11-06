# RUST-10｜软件矩阵、候选范围与下一步门禁

> 日期：2026-10-13；分支 `v6-simplified-rust`。
> 本轮没有访问 COM、烧录、复位或测试任何板子，没有提交或推送。
> `SOFTWARE SLICE SELF-REVIEW PASS / COMPLETE PROTOCOL HOLD`。

## 1. 对用户“推进到需要实机”为止的执行边界

本轮继续了 Rust 的 RUST-08/09，并建立 RUST-10 软件组合和自动矩阵。不是只创建空 crate：
代码有 fixed storage、正常及失败状态机、真实 Foundation Consumer、Codec 和可运行回归。

**但目前仍未达到“完整 Rust 协议仅剩实机”。** 全部线上模块接线所需的共同 Control Body、
Group Record 和安全上下文合同仍不完整；不能为了完成 Rust 任务擅自定义与 C 不同的协议，
也不能把剩余软件工作改名为硬件门禁。下面明确区分已完成切片、剩余软件和实际硬件。

## 2. 已完成的组合

```text
actual Security Session / Foundation proof
 -> protect packet using a test MAC Provider
 -> open / exact Source + Destination + ACL / Replay reservation
 -> Reliable receipt capacity preflight
 -> Replay commit
 -> one application publication or duplicate receipt

immutable Reliable origin bytes
 -> actual Adapter reservation / Driver submit disposition
 -> NOT_SUBMITTED retry or real holder
 -> stop/cancel/real retirement before slot recycle
```

`ucn-runtime::RuntimeBridge` 只提供直接 C1 Reliable One-Way 收包、C0 Delivery ACK 和真实
Adapter 发送/退休桥；不是所有模块的生产 Coordinator。默认最小 `ucn-core` 完全不引入它。
真实 Test MAC Provider 使用 HMAC-SHA256 并检查 Tag；篡改会失败。测试 Key/Provider 不发布为
生产 O1 Suite，O2 测试 Provider 明确 Unsupported，不冒充生产 AES-GCM/ChaCha20-Poly1305。

Security 增加私有字段 `PacketEvidence`；上层读取 Owner 已核对的身份/摘要，不拼装
`authenticated=true`。C0 transaction 与 C1 origin sequence 分属独立 Replay Window，
相同数值 1 不相互阻塞。新的 C0 window 也参加过期、Fence 和有预算维护。

为维持既有 Security 资源门禁，Pending Requirement 仅保存必要的精确基线，不保留握手完成
后已无用途的完整候选副本；没有删除 Record/Proof/Generation 的安全匹配字段。

## 3. 可复现验证

工作目录为仓库根目录；该脚本仅执行编译/测试，不打开串口：

```powershell
python tools/v6/run_rust_software_matrix.py --cargo C:\Users\y2389_4rq4ld9\.cargo\bin\cargo.exe
```

矩阵同时保存执行前后源码 SHA256，期间任何源文件变动使 source_stable 为 false 并失败。
Rust 所有 crate、测试/fixture、Workspace/Lock/toolchain、测试工具和影响 native C oracle 的
头文件/Codec 源码都在清单内。本轮为 **192 个源码依赖**，没有声称整个脏工作树被外审冻结。

| 验证 | 本轮结果 | 能证明什么 |
| --- | --- | --- |
| Windows Host Debug | 166/166 | 正常/错误路径及已有完整 Rust 回归 |
| Windows Host Release | 166/166 | 优化后的相同软件语义 |
| MSRV Rust 1.85.0 | 166/166 | 当前 Workspace 的最低版本测试 |
| Clippy all-targets -D warnings | PASS | 所有当前 Target 的静态 lint |
| rustfmt --check | PASS | 当前代码格式 |
| Cortex-M soft/hard float | 两个 target PASS | no_std 编译，不是 MCU 执行 |
| ESP32-S3 xtensa build-std core | PASS | esp 工具链编译，不是固件链接、烧录或运行 |
| 裁剪/依赖 | 20 crates、9 组合 PASS | 最小 Core 不带高级模块，Optional 不互链 |
| C/Rust/Python Oracle | 10/10 | 指定 Body 的逐字节对照，不是全协议互操作 |
| 固定 Seed raw 属性测试 | 8192 个输入 | 无 panic、被接受值重新编码 canonical |
| 源码快照 | 前后 192 项一致 | 此次矩阵没有混用执行期间变动的版本 |

原始结果在 `rust/target/software-validation/latest.json` 和各同名 `.log`，target 不提交。
本轮可审阅副本见 [software-matrix.json](../软件证据/Rust/2026-10-13/software-matrix.json)。
该副本 SHA256：`CA5657F0775EDB699EA3CBBE4020561415C269AB847CB447DB14801785121D3C`。
报告与日志的 PASS 必须绑定该副本列明的 source_sha256，不绑定仅有旧 HEAD 的远端内容。

Windows 线程并发回归实际执行；本轮没有运行 TSan/Miri，不挪用 C 的 TSan/ASan 结论。
额外试编 `riscv32imac` 通过；无 A 扩展的 `riscv32imc` 缺 CAS，当前 Owner/Adapter 不能直接
支持该 target。需要独立平台临界区/原子 Port，不能用“可运行所有 MCU”措辞掩盖这一限制。

## 4. 资源账本

下表为 Windows Host `size_of/align_of`，各项 alignment=8。不是 ESP32 `.bss`、调用栈、
整机 RAM、CPU 或 Flash 证明；实际装配还需要 Foundation、Driver、队列和 scratch。

| Owner | Nano | Lite | Full |
| --- | --- | --- | --- |
| Transfer | 2336 B | 15208 B | 58552 B |
| Service RPC | 712 B | 3288 B | 10744 B |
| Cluster | 1912 B | 3640 B | 6520 B |

另外：`RuntimeBridge<2,128>` 272 B、`SyncOwner<1,2>` 440 B、`TimeDomain<3>` 736 B。
Security 原有门禁继续通过：Nano 2208 B、Lite 10480 B、Full 30896 B。

大消息数组留在 caller-owned/static Owner 内；Transfer 有 const 初始化，处理一个分片不会
复制整份 Owner。仍不能据此证明所有 generic 实例的 MCU 最坏栈均达标：目标实例化、链接和
任务栈水位属于后续目标验证。Full 不是“几个 KB 的完整协议”，裁剪容量必须按业务选择。

## 5. 尚可不接实机完成的软件工作

| ID | 明确缺口 | 继续实施前需要的共同合同 |
| --- | --- | --- |
| RUST-SW-01 | Parent、Transfer、Service/Operation 的完整控制 ingress/反馈 | 精确 Opcode Body、Role/Source/Flags、重复/超时/ACK |
| RUST-SW-02 | 统一动态 Runtime；Route/Flow 对实际保护帧与 Adapter 的组合 | Identity/Capability/Origin/Hop 的当前 use-time 事实和生命周期 |
| RUST-SW-03 | Group 与 C5/H3、Tree；catalog 与 C Record 统一 | 共用 Group Record、allocator 所有权、Sender/Key/Tree/Replay 域 |
| RUST-SW-04 | Cluster 全部线上证书/Delta/Merge/Dissolve/Federation | voter persist-before-vote、认证证书、跨节点 lease/fence 与精确 Body |
| RUST-SW-05 | 全 Profile、全组合的运行时互操作及目标软件资源门禁 | 上述合同/实现完成后扩大独立 Fixture 与真实 ELF 实例化 |

这五项不是硬件依赖。当前 C 的 Registry 登记、某 Owner 有 typed API 或已有过期审计结论，
都不能代替新 Runtime 所需的规范 Body。尤其 `GRC1/0x0902` 仅为 Rust catalog 原型，不允许
悄悄变成两种“同一个 v6 Group Record”。

继续这些项涉及 C/Rust 共同合同与可能的 C 行为同步；本轮严格限于 Rust 推进，不修改 C
生产模块或已冻结合同。需要用户确认共同合同整改范围后继续，而不是假定已经到达硬件唯一门槛。

## 6. 确实需要实机的项目

详见 [Rust 后续实机计划](../实机测试计划/UCN_V6S_Rust后续实机测试计划.md)。先完成必要
软件门禁和对应固件构建，再根据用户通知测试：

1. 非阻塞 UART TX/RX/ISR completion、同步早到和 backpressure 的实际 Driver 语义。
2. 真 Flash Marker/Witness/双槽/撕裂写入及断电，重启不复用 Generation/ID。
3. 生产密码 Provider、Key storage、实际 AEAD/Tag/Replay 与受保护多跳。
4. 真实硬件 timestamp/quantization/asymmetry，GPIO/分析仪证明 uncertainty 不低估。
5. 六板物理断线/重连、可靠传输/分片/群发/簇接管与 C↔Rust 固件互操作。
6. 真实 Flash/RAM/栈水位/CPU/吞吐/尾延迟/功耗与 24 h 长稳。

本轮既有六板历史证据不重新标为新模块完成证据；没有实机输入时不运行这些步骤。

## 7. 结论

已列明的 Owner/Codec/桥接软件切片通过两轮自审和本轮矩阵；不代表统一 Rust 最终协议完成。
`RUST-08/09/10` 顶层保留 PARTIAL，外审/生产/RC/Tag 均 HOLD。源码/文档/工具的当前
未跟踪成员属于候选，未来用户要求提交时必须完整纳入，不能只提交旧 tracked 差异。
