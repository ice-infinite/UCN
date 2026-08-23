# UCN V5 Cluster M12 12-02 Recovery ID 分项自审报告（2026-08-24）

## 1. 范围与结论

本报告只处理 CLV2-12-02（P0）：Recovery ID 经 cluster ID provider 派生，绑定完整 lineage，保证同节点不同 round/boot 不复用。结论：SELF-AUDIT PASS。

## 2. 完成定义逐条对照（任务表 CLV2-12-02）

| 要求 | 证据 | 结论 |
|---|---|---|
| 通过 provider 生成 hash(parent,term,config,round,node,boot_incarnation) 或等价唯一 ID | 请求结构扩展 parent_config_id/recovery_round（ucn_cluster.h 请求 struct）；默认生成器链式 mix 全部 8 个输入（ucn_cluster.c cluster_default_next_id） | PASS |
| 禁止等于 parent/0 | cluster_id_is_valid() 拒绝 0/broadcast/parent 复用，失败返回 UCN_ERR_CONFIG 且不消耗对象轮 | PASS |
| 同节点不同 round/boot ID 不复用 | 测试 (d)(e)：recovery_round、incarnation、对象轮三维互异 + 连续 8 轮全互异 | PASS |

## 3. 自审期间发现并修复的真实缺陷

- 缺陷：首版把 lineage 字段并列 XOR 进候选值。测试 (d) 立即暴露：recovery_round 0→1 与对象轮 2→3 的位差同为 bit0，XOR 抵消 → 连续恢复轮派生相同 ID（违反唯一性门禁）。
- 修复：改为链式 cluster_id_mix()——每个输入先雪崩再链入，位差不可能跨字段抵消；并新增 (e) 连续 8 轮 TTL 失败模式钉桩（全对互异断言）。
- 影响面：仅默认生成器（无 Provider 产品）；有 Provider 的产品不受影响（Provider 自行负责唯一性，核心只做 validity 门）。

## 4. 测试与门禁证据

- 新测试 cluster_test_recovery_id_uniqueness（5 组）+ 白盒钩子；既有 03-08 Provider 全路径测试不受影响。
- FULL Debug All UCN tests passed；ASan/UBSan 零报错；OBSERVED-PAIRS 30 零违例；Golden 8b80b08、trace mismatch 0；cluster_bytes 1608（无新增字段）；-Werror 干净；diff --check 干净。

## 5. 边界

- 跨重启唯一性依赖 incarnation：无 Provider 产品仍需 M04 受控启动 incarnation（04-09）或产品 boot counter；12-09 将持久化 round/lineage。
- REKEY purpose 仍只是接口预留（03-08 边界不变）。
