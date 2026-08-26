# UCN V5 Cluster M12 12-01 Recovery Lineage State 分项自审报告（2026-08-24）

## 1. 范围与结论

本报告只处理 CLV2-12-01（P0）：恢复域 lineage 状态与离簇前捕获。结论：**SELF-AUDIT PASS**。
未改退避/仲裁/权威/线协议；M05 AUDIT HOLD、M08 WAIT EXTERNAL 等边界不变；不提交、不推送。

## 2. 完成定义逐条对照（任务表 CLV2-12-01）

| 要求 | 证据 | 结论 |
|---|---|---|
| 新增 parent_cluster_id/term/config_id/recovery_round | include/ucn/ucn_cluster.h 恢复域 5 字段 + view 4 字段 | PASS |
| 在 Member/Backup/Head Fence 离开旧簇前捕获 lineage | ucn_cluster.c：MEMBER 宽限超时站点 `cluster_lineage_capture()` 在 `set_detached()` 前（约 L1813）；Backup missed-heartbeat 站点在 `backup_clear_sync()` 前 | PASS（Head 侧当前无离簇路径：v3 Head 不退化为 recovery，注释说明） |
| Detach 不丢 parent 信息 | 测试 (a)/(b) 在 set_detached 后断言 parent 字段存活；capture 与 03-06 历史互补 | PASS |
| （附加）同 parent 保留 round、异 parent 归零 | capture 同 parent 分支只升级 term；异 parent 分支 round=0 | PASS |
| （附加）恢复域不成为 parent | capture 条件 `cluster_id != recovery_cluster_id`；测试 (c3) | PASS |

## 3. 关键设计说明

- **与 03-06 历史的关系**：`last_cluster_id/max_seen_term` 是"最近稳定域防旧 Term 重入"的运行时记忆；12-01 的 parent_* 是"恢复域自身血缘"——捕获时优先读活身份、Detach 循环后回退历史。两者不合并：历史是防重放护栏，lineage 是恢复域身份输入。
- **parent_config_id 绑定**：v3 默认产品无 Config owner → 恒 0（rank 比较中的最小值，12-04）；`ucn_cluster_lineage_bind_config()` 是 M07 实验 owner 的桥，绑定值仅在异 parent 捕获时消费。
- **round 语义边界**：12-01 只保存/保留 round；递增（TTL/仲裁失败）与稳定重置属 12-03。
- **资源**：cluster_bytes 1584→1608（+24 B：5×u32 + stats 前 4 B 对齐）。Host x64 Debug 观测，MCU 待测。

## 4. 测试证据

- 新测试 `cluster_test_recovery_lineage_capture()` 5 组场景 + 白盒钩子（test_cluster.c，注册于 03-06 历史测试后）。
- FULL Debug：`All UCN tests passed`；OBSERVED-PAIRS 30 零违例；ASan/UBSan 零报错；Golden 8b80b08、trace mismatch 0；-Werror 干净；diff --check 干净。

## 5. 已知边界（留给后续节点）

- 12-02 将把 parent_term/config_id/recovery_round 传入 provider 请求参与 ID 派生。
- 12-03 将实现 round++（TTL/仲裁失败）与稳定加入后的重置（届时 parent_* 与 round 一并清零）。
- 12-09 将持久化 lineage/round（当前仅 RAM）。
