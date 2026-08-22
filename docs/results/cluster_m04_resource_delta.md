# Cluster M04 资源增量记录

> 范围：M04 Persistence 的当前 Host x64 Debug `sizeof(ucn_cluster_t)` 观测。
> 这是结构体大小趋势，不替代目标 MCU 的编译器、ABI、Profile 和链接结果。

| 阶段 | `cluster_bytes` | 相对前一阶段 | 变更原因 |
|---|---:|---:|---|
| M03 独立审计基线 | 1136 B | — | M03 Epoch 分类完成后的 Host Full 基线。 |
| M04-03 独立复审 PASS | 1144 B | +8 B | `persistence_mode`、Provider 指针及诊断可见性。 |
| M04-04 代码候选 | 1152 B | +8 B | 恢复结果枚举在 view/stats 的诊断投影。 |
| M04-05..10 初次自审 | 1192 B | +40 B | 一个有界 pending-operation 描述符、failure/统计诊断与 Provider 完成后的 continuation 元数据；完整 Record 仍不放入对象。 |
| M04 R17..R22 整改候选 | 1200 B | +8 B | Provider I/O 动态重入门、durable ACK retry descriptor 与诊断统计；仍不把完整 Record 放入对象。 |
| M04 R23 legacy PREPARED 迁移 | 1200 B | +0 B | 仅新增 Record operation 与启动期临时状态变换；不在 `ucn_cluster_t` 增加字段，也不缓存完整 Record。 |
| M06-01 成员状态值模型 | 1392 B | +192 B | 默认 16 个成员槽从旧 `occupied/node/lease/nonce` 扩展为状态、voting、wire/capability 与两个时间戳；每槽 Host x64 Debug 增加 12 B。 |
| M06-02 Primary Member Table | 1392 B | +0 B | 把裸 `members[]` 封装为固定 `primary_members.slots[]`，不增加槽、计数、指针或 staging storage。 |
| M06-03..09 完整自审基线 | 1552 B | +160 B | Voter set、每成员 provisional deadline、只读容量/摘要诊断及少量运行配置/统计字段。真实 Config Commit、Joint staging table、证书和完整 Record 仍未加入对象。 |

## 当前结论

- 累计相对 M03：`+416 B`。
- M06-03..09 当前测量来源：`build_c06_release` 的 64-node clean simulator 输出，`UCN_PROFILE=Full`、Service=ON、Host x64 Release；同轮 Service-OFF Debug simulator 也观测到 `1552 B`。这仍是 Host ABI 观测，不可替代 MCU 测量。
- Core-only 不链接 Cluster 库，仍不承担 Cluster RAM；Lite/Nano 的最终目标资源仍需在对应 MCU 工具链下单独测量。
- M04 R17..R23 仍没有把 280 B Record v1 或 Config/Rekey/Tombstone 复制进 `ucn_cluster_t`；它们继续由 Provider 持有，避免此阶段制造不完整且可被旧 FSM 覆写的 RAM mirror。R23 只在受控启动调用栈中构造 `next_state`，并由 Provider 原子保存。
- **栈边界需明确处理**：Bridge 的 `submit`/`poll`/hook 调用会在调用栈暂存一个或多个 280 B Record 状态，以完成 `load → validate → journal match`；当前 Host CTest 证明功能和内存安全，**不等价于 MCU 任务栈已测量**。板级 Provider 接入前必须在目标编译器/RTOS 栈预算中测量最深调用路径，并把结果纳入实机 M04 验证。
- M06-01 的 `1392 B` 来自 `build-v567-cluster-gcc` 新编译后的 `ucn_tests` 与 `ucn_cluster_sim` 输出（Full、Service ON、Host x64 Debug）；Lite/Nano 在相同 Host ABI 下也观测到 `1392 B`，不代表目标 MCU ABI 已测量。
- M06-02 在 Full/Lite/Nano、Service OFF 和 64-node simulation 中仍观测为 `1392 B`；该项只引入 table wrapper 类型和字段命名，故相对 M06-01 为 `+0 B`。
- M06-03..09 的 `+160 B` 是整段的聚合变化，未将 Host ABI 的 padding 伪精确地拆分到每个小项；目标 MCU 必须在其编译器、Profile 与链接配置下复测静态 RAM、栈和 Flash。
