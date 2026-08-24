# UCN V5 Cluster M14 / 14-05 Target Property Model 自审报告

> 日期：2026-08-24  
> 状态：`CODE COMPLETE / SELF-AUDIT PASS / WAIT M14 EXTERNAL REVIEW`

## 1. 模型范围

新增 default-OFF `ucn_cluster_target_property_tests`，直接组合真实 M08 Authority Owner、14-04 invariant engine 与 M10 frozen takeover transaction；不建立一套与代码脱离的“自证正确”模拟器。

覆盖三层状态空间：

1. 穷举两个候选 Head 对中立 voter 3..5 的全部 `3^3=27` 个独占分区，每个分区逐毫秒推进 181 个 Owner cycle；每一步检查 local Safety 和跨节点 Single Authority。
2. 穷举 Stable/Joint Config 的 16 个远端 vote subset，使用独立 popcount-majority oracle 对照生产 quorum helper；只有 old/new 必需集合均过半才允许 Epoch durable、Certificate 和旧 Primary Fence。
3. 固定 seed `0x14A05EED` 执行 4096 条、每条 32 event 的真实 M10 随机序列，共 131072 event：self durable、远端 durable vote、duplicate、错 VoteId replay、deadline abort、quorum durable 与 terminal replay。

## 2. 自审发现

第一次运行时，property oracle 错误假设“所有非 OK 操作必须零写”。真实 M10 `step()` 在 deadline/quorum impossible 时返回错误并原子进入 `ABORTED`，这是协议规定的安全状态转换。测试已改为：普通拒绝必须 no-write；time-driven abort 必须精确进入 `ABORTED + recovery_required + inactive`。没有为让测试变绿而修改协议实现。

## 3. 核心断言

- 同一 Cluster 永不同时出现两个 `authority_active`；
- writable Authority 必须满足当前 canonical Config quorum；
- Joint Config 必须 old/new 双 quorum；
- Takeover 未达冻结 quorum 时不得 durable；
- durable terminal 经 late `step()` 保持逐字节不变；
- 错 VoteId replay 不写 transaction；
- Certificate 接受后旧 Primary Fence 已建立并要求 Join。

## 4. 验证

- 定向 property CTest：PASS（27 partitions、16 vote subsets、131072 fixed-seed events）。
- M08 Authority 与 M10 Takeover 定向测试：3/3 PASS。
- 模型只在 `UCN_BUILD_CLUSTER_TAKEOVER_EXPERIMENTAL=ON` 时构建；默认产品不新增 M10/M14 property 符号。

## 5. 结论

14-05 软件范围自审通过。该结果是 bounded executable model evidence，不是无限状态形式化证明，也不解除 M05/M10 既有外审状态；下一项进入 14-06 codec/stateful replay fuzz。
