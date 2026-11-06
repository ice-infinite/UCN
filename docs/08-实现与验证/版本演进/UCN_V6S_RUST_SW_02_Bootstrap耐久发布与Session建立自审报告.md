# RUST-SW-02 Bootstrap耐久发布与Session建立自审报告

## 1. 本轮范围

- 日期：2026-10-15；`v6-simplified-rust@03eb7eb`加完整既有工作树。
- 用户授权：接通Offer/Commit/Final真实耐久发布与Session建立，统一C/Rust合同；每小项
  自审，最后整体自审；不操作实机。本轮无串口/USB访问、烧录、复位、提交、推送或Vault改写。
- 采用知识库MCU-first、固定容量、Host无协议特权原则；旧v5能力描述不当作当前证据。
- 结论范围：Rust专属单Peer JOIN的软件链；C共同Codec/Session种子与现有Proof接口。
  **不是完整自动Node、C自动JOIN Root、跨语言全链路互通、生产密码或实机放行。**

共同合同见[第09篇](../../10-理论与规划/建议方案/UCN_v6_C_Rust共用合同收口/09-Bootstrap耐久发布与Session建立共同合同.md)。

## 2. 分项实施与即时自审

| 小项 | 实施 | 自审与反例 | 结果 |
| --- | --- | --- | --- |
| JOIN-01 | 可裁剪bootstrap-join；Cookie阶段消费交换；Identity Challenge不可由Frame构造 | 迟捕获起点、外来Identity Gate拒绝；旧认证API仍不能发送Final | Host定向通过 |
| JOIN-02 | Device实际提交Authority Record并消费回读Proof | Provider PENDING不发布；回读损坏无Authority；保留原起点 | Host定向通过 |
| JOIN-03 | Authority先持久化精确地址预留，再分片发送Offer | 未提交/Proof未形成/回读损坏不得Offer；被放弃的代际不回退 | Host定向通过 |
| JOIN-04 | Device完成自己的Binding回读后才能Commit | 未耐久/损坏不得Commit；Offer和Commit各丢首片后整轮重传收敛 | Host定向通过 |
| JOIN-05 | Authority核对Commit及当前有效Binding后Final | 假Evidence拒绝且阶段不变；Driver UNKNOWN保留/隔离Holder | Host定向通过 |
| JOIN-06 | Final派生方向槽、Digest和本地租约，分别持久化Session192 | 外来Security Gate、替换Evidence、Key ID耗尽、Suite冲突、溢出拒绝；一端Active不伪造另一端Active | Host定向通过 |
| JOIN-07 | 停止Fence Session；已接纳Foundation请求保留到退休 | 停止后Proof只退休；reopen拒绝迟到Proof；重启恢复记录不恢复权限 | Host定向通过 |
| JOIN-08 | C同步纯Session seed，三方独立规范字节及边界测试 | C输出哨兵/别名/溢出；Python独立构造；不是Rust调用C FFI | 共同Oracle通过 |

正式入口为`rust/crates/ucn-runtime/tests/bootstrap_runtime.rs`及其`bootstrap_runtime/join_tests.rs`。
新增14项，连同既有6项共20项。Fake Media真实执行Foundation，不手工构造PersistenceProof
或BindingView。Host Verifier验证本轮HMAC及预期事实，不能替代产品PKI/密码审计。

## 3. 第一轮整体自审：权限、因果域、阶段

逐项回读Runtime、Identity、Admission、Security、Foundation及C Codec：

1. Challenge在最初锁存；Final不能提供新租约起点；双方uptime不混用。
2. Offer/Commit/Final严格分阶段；非地址字段冻结；Final必须经Admission实际耐久门。
3. Session还要自己的Foundation Proof；不把收到Frame或本地Driver成功当成耐久成功。
4. Evidence精确绑定已认证事件；Session候选由Final派生，不接受调用者任意Candidate。
5. 提交/完成/查询复核实际Link；Gate按对象身份比较，不是“各自有一个Gate”即可。
6. Foundation明确未接纳时撤销未发布候选；接纳后保留实际Handle到终态。

这轮补强了精确Evidence、共享Gate身份、失败时单调时刻高水位、迟到Proof的Link复验。
修正后重跑定向与Clippy，没有压低告警或删减失败测试。

## 4. 第二轮整体自审：故障、资源、共同合同

1. 下一事件先校验再换重组槽；丢失首片允许拒绝后续片，完整重传可收敛。
2. 新响应不能越过未退休的原TX Token；UNKNOWN和不确定取消保留隔离历史。
3. 停止请求完成后仅退休Proof；不复活Identity/Session，不回滚已发布的地址预留。
4. 重启重建Foundation恢复三个Record与高水位，业务Owner仍为空；重复提交旧Session
   Generation被拒绝。Host保留内存模拟不等于物理掉电实验。
5. C/Rust SessionDigest有专属域和精确长度；方向Key不同且不回绕；完整字节独立Oracle。
6. C scratch由调用方静态持有，Admission栈门限未放宽；Rust私有辅助减少大对象按值传参。
7. 关闭JOIN Feature不保留新增起点/Evidence缓存；Optional Owner不反向依赖Runtime。
8. Json、合同、Task和README明确C实际边界，不把C seed写成C自动JOIN已完成。

第二轮后冻结源码、Cargo、CMake、共同JSON、Oracle及工具，再运行完整矩阵。期间只补文档；
最终独立复算全部依赖哈希，不能将旧证据套用到新工作树。

最终回读还发现迟到Final原用例先命中握手超时，区分度不足。已把该例设备握手窗口放宽为
20000 us，而Binding仍在5020 us到期，明确断言Access拒绝、密码回调为0；正常链还验证
5019 us可用/5020 us拒绝。该测试修改后重新运行全部12配置，旧矩阵不当最终证据。

## 5. 验证与证据

已完成：Rust定向20/20；workspace Debug、Release、MSRV 1.85各286/286；Clippy、fmt与双
Cortex检查通过。Windows GCC C Full重建CTest108/108；Lite/Nano重建定向各7/7，包含
Admission栈、Identity与Security。最终矩阵、不可变快照和资源数字在结案段记录。

本轮不是fresh MSVC/Clang/ASan/TSan全矩阵，也不是MCU运行/任务栈证明；不借用历史数字
当本轮执行。新Root只借用并协调Owner，没有新增共享裸全局可写状态。

### 最终冻结快照

- Rust Debug/Release/MSRV各286/286；Bootstrap定向20/20。
- Clippy/fmt、双Cortex、ESP32-S3 **compile-only**、25裁剪组合通过。
- 三方Oracle69向量；既有Admission/Capability共同Golden7/7。
- 最终12配置全部exit0，`source_stable=true`；406项源码/合同/工具依赖独立复算，差异0。
- [不可变矩阵](../软件证据/Rust/2026-10-15/software-matrix-sw02-durable-join-final.json)
  SHA256：`D3B303CD8CA9A96515EAAB1652F134AED93015B0939D1D4D06A088AB43E08492`。
- Host sizeof/alignment：Client560/8、Listener1200/8、AuthExchange1792/8、Freshness120/8、
  BootstrapJoin2680/8；组件之间有借用，不得把某一项当成总节点RAM。
- GCC Full新C seed直接栈帧96 B；原Admission256 B门限仍通过。这不是目标MCU总调用链。

新增种子不改变公共C ABI或普通Wire头；C公共函数索引仍318项。最终文档门禁145篇/371链，
4份相关文档独立核对56链；4个新增手写文件尾随空白0；边界/Admission栈2/2；普通
`git diff --check` exit0（98条换行提示、无其他问题），真实暂存区为空。最终再复算406项
源码依赖仍0差异。本轮没有独立外审签字，RUST-SW-02和生产状态不升为整体GO。

## 6. 费用与后续

普通数据头新增0 B。A1、Evidence32、62 B分块、首次无丢包八类建链共4600 B；不含Carrier、
PHY、重传及Capability交换。Host sizeof只统计组件，还须另计Adapter/RX/Driver、三个
Foundation域、Identity/Security、Provider工作区及目标调用栈。

Peer Reauth、安全续期、多Peer、C自动Root、H1自动接入现有静态H0 Capability Root和
后续Route/Flow仍属软件工作，不是“没有实机”的阻塞。产品方向槽/KDF、证书Provider、
真实Flash原子性/Witness/掉电、目标资源和性能分别HOLD。本段自审不代签独立外审GO。
