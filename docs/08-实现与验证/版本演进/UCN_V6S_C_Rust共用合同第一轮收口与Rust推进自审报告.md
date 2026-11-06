# C/Rust 共用合同第一轮收口与 Rust 推进自审报告

> 2026-10-13；分支 `v6-simplified-rust`，`03eb7eb` 加完整未提交工作树。
> 用户已授权 C/Rust 共用合同整改，禁止实机操作。本轮无 COM、烧录、复位、提交、推送。
> **本轮软件切片自审通过；完整协议仍 PARTIAL/HOLD，不宣称仅剩实机。**

## 1. 完成的改动与每项自审

| 小项 | 实现及自审证据 | 范围结论 |
| --- | --- | --- |
| SH-01a 条件字段 | 两语言共享 Flags：mode 0/1/2、FINAL4、reserved `0xF8`；Request无FINAL，Result/Error有FINAL；1024种 Role/Flags 组合独立比对 | PASS（字段合同） |
| SH-01b 完整 Control Body | Rust 补29B Transfer Key、33B ACK/Abort、32B Terminal、28/48/32B Operation Body、9/11B应用 RPC Envelope；精确长度/保留位/零写回 | PASS（Codec，非全业务接线） |
| SH-01c 规范摘要 | C 裸SHA、Rust BLAKE摘要分歧改为共同 domain+length+SHA256；AAD、Payload、Setup、完整Message独立三方比较；Foundation BLAKE2s不变 | PASS（列明域） |
| SH-01d C0重放域 | C的C0 bitmap与C1+ Origin分离，共用有限reservation pool；同数值1先提交C1、再认证C0仍正常；Handle带域并精确检查 | PASS（软件回归） |
| SH-01e C0 nonce | C原用本地IN/OUT、Rust原用事务角色，改为认证Principal字典序；双端派生输入逐字节相同；错误方向在burn/Provider前拒绝 | PASS（派生合同；非物理密码Provider放行） |
| SH-02a 认证命令入口 | 实际Security verify/ACL，强类型解码和队列容量通过后才提交Replay；opaque Handle、stop保留义务、错来源/错Service/篡改/满载/重放回归 | PASS（接纳队列切片） |
| SH-03a 共同Context正文 | C/Rust同`0x0802/v2`、64B头+28B成员；移除易失Handle；C恢复Fenced；独立Python固定512B向量比对 | PASS（共同Codec、恢复门禁） |
| SH-03b Rust Realm allocator | 独立16B GID1、唯一Realm Domain、真实Foundation proof后线性Permit；Context失败/提交前崩溃只烧号；退休不复用；恢复不发旧Permit | PASS（Rust消费者） |
| SH-03c 固定资源 | Nano/Lite/Full Context Body为176/288/512B；不再使用超过Full正文上限的目录；无已用ID无限集合；错误Domain/容量拒绝 | PASS（静态/Host上界） |

普通 Common Header仍3B，C1与其他已冻结短Header没有添加字段；One-Way不携带RPC Envelope。
变化属于规范摘要/KDF输入和业务Record，不通过复制旧Wire字段制造第二套协议。
C Storage Layout升级为4，拒绝旧Layout/Manifest组合；Group旧v1和Rust-only GRC1不保留解码兼容。

## 2. 两轮全体回读

第一轮按 `类型→Codec→认证→Replay→队列→Owner→Foundation→恢复` 顺序核对。
发现并修正：负/保留Service、Flags任意值、发送摘要与C不一致、C0与C1数值域冲突、C0 nonce
双端输入不一致、C1 primitive曾错误拒绝合法Reliable One-Way Metadata。

第二轮按 `失败/超时/重复→资源/义务→权限→恢复→正文` 反向核对。
修正了：重放Setup ACK重新补充已花Credit、冲突Terminal改写DONE、正文Service扩大ACL、
在C0中猜测14B Flow SACK的隐含Parent（现在拒绝，完整C1 SACK仍接受）、
Group目录超过Profile Body限制、恢复磁盘Handle、allocator与Context Domain混用。
新同步测试必须使用递增的物理采样时间，不能为了通过用例放宽单调门禁。

当前结论只针对上述已实现切片。未对尚不存在的完整 dispatcher、Group Tree、Cluster
线上证书作“P0/P1为0”声明。C/Rust普通内存和可信Coordinator/SPIs也不是恶意调用方或
任意原始内存破坏的防护边界。

## 3. 软件验证

- Rust Workspace Debug/Release/MSRV1.85，各171/171。
- Clippy `-D warnings`、fmt、Cortex软/硬浮点no_std、ESP32-S3 compile-only通过。
- 9个裁剪组合通过；Group/Realtime/Cluster不互相链接，普通Core不拉入可选模块。
- 独立 C/Rust/Python Oracle 24/24：包含完整O1/H0 C1实际Security保护/打开与HMAC向量、
  Group Context/allocator、完整响应字段、Flags、规范摘要与时间正文。
- Windows GCC C Full/Lite/Nano各108/108、Persistence-OFF97/97、全Optional及Persistence-OFF72/72。
  四项增量重建基于已核对的对应Profile/Feature缓存，不称为本轮fresh clean rebuild。

Native受保护C1向量使用C的Session生命周期Host fixture和实际Security函数，HMAC为独立实现。
它不把fixture中的模拟durability证明冒充真实Flash；Rust Foundation消费者另有真实软件
Provider→双槽/Witness→reload回归。本轮没有运行TSan/Miri，也不复用旧ASan数字。

原始Rust结果见[software-matrix-shared-01.json](../软件证据/Rust/2026-10-13/software-matrix-shared-01.json)。
该副本SHA256：`2881F6DCF9CF47B2F52D2B62457B887290ACB6222B0066BC56C5742DEC8A2794`。
矩阵开始/结束对364个源码及JSON合同依赖逐一核对，要求`source_stable=true`。
旧166测试的报告只对应旧快照，不能用旧签字覆盖本轮共用合同改动。

## 4. 资源边界

Host Transfer：Nano2352、Lite15240、Full58616B；RPC：712/3288/10744B；Cluster：1912/3640/6520B。
RuntimeBridge272B，ControlCoordinator552B；逐槽GroupContext856/1336/2296B。
GroupAllocator264B，包括独立高水位与未消费线性许可。
这些是Host `sizeof`，不是整机RAM、MCU最坏调用栈、性能或功耗。Full需要按业务裁剪容量。
大Owner应静态装配，不能把返回大型对象的普通局部初始化误作目标任务栈已验收。

## 5. 尚未完成的软件工作

| 工作 | 必须补齐，不能改成WAIT HARDWARE |
| --- | --- |
| 完整业务dispatcher | Parent/Transfer/Service/Operation/Time精确查找与业务提交、重复请求缓存回复、回复丢失和Driver退休义务；队列接纳不是完成 |
| 动态Runtime | Identity/Admission/Capability/Route/Flow实际保护帧与Adapter ingress；多跳必须分开终端Origin与每跳邻居Context |
| 完整Group | C独立allocator proof编排、C5/H3、Tree Stage/Commit、换钥/成员变更；共享Group MAC不能伪称唯一Source |
| 完整Cluster | voter-side durable vote/certificate、Snapshot Delta、Merge/Dissolve/Federation认证线上Pipeline；现有Owner/typedSPI不是完整Wire闭环 |
| 最终软件候选 | 以上全部完成后再做运行时双实现互通、真实目标ELF资源/栈、全模块自审与独立外审 |

保留MCU-first、固定容量、模块物理解耦；Linux不是成网、选主或恢复的必需条件。
只有软件项关闭后，才进入用户另行通知的ESP32、真实Flash/掉电、物理Bearer/时间戳、
生产密码Provider、性能与长稳验收。本轮不是外审GO或1.0 RC。
