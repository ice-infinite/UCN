# UCN V5 Cluster M12 12-02 Recovery ID 分项自审报告（2026-08-24）

## 1. 范围与结论

本报告只处理 CLV2-12-02（P0）：Recovery ID 经 cluster ID provider 派生并绑定完整 lineage。M12.3 全体复审后，结论收敛为：**Provider contract PASS；默认 32-bit mix 为 best-effort，不能宣称数学无碰撞。**

## 2. 完成定义逐条对照（任务表 CLV2-12-02）

| 要求 | 证据 | 结论 |
|---|---|---|
| 通过 provider 生成 hash(parent,term,config,round,node,boot_incarnation) 或等价唯一 ID | 请求结构扩展 parent_config_id/recovery_round（ucn_cluster.h 请求 struct）；默认生成器链式 mix 全部 8 个输入（ucn_cluster.c cluster_default_next_id） | PASS |
| 禁止等于 parent/0 | cluster_id_is_valid() 拒绝 0/broadcast/parent 复用，失败返回 UCN_ERR_CONFIG 且不消耗对象轮 | PASS |
| 同节点不同 round/boot ID 不复用 | 产品 Provider 必须维护分配历史/唯一性；现有测试只证明固定短序列互异，不能推广成全空间证明 | PROVIDER CONTRACT PASS / DEFAULT PARTIAL |

## 3. 自审期间发现并修复的真实缺陷

- 缺陷：首版把 lineage 字段并列 XOR 进候选值。测试 (d) 立即暴露：recovery_round 0→1 与对象轮 2→3 的位差同为 bit0，XOR 抵消 → 连续恢复轮派生相同 ID（违反唯一性门禁）。
- 修复：改为链式 `cluster_id_mix()`，消除已知的直接 XOR 抵消；并新增连续 8 轮 TTL 失败模式钉桩。该测试是回归样本，不是 32-bit 全空间唯一性证明。
- 影响面：仅默认生成器（无 Provider 产品）；有 Provider 的产品不受影响（Provider 自行负责唯一性，核心只做 validity 门）。

M12.3 额外对抗探针按当前默认算法扫描固定 lineage 下的 65534 轮，确认某些输入存在真实生日碰撞，例如 `local=1,parent=101,term=8,config=4,incarnation=1` 时 object round `16459` 与 `29522` 都得到 `3258608038`。因此源码和文档已删除“绝不碰撞”措辞；观察到同 Recovery ID 的不同 Head/Term/parent/nonce 时运行期 fail-closed，但自动换号与持久 allocation history 归 `CLV2-13-12`。

## 4. 测试与门禁证据

- 新测试 cluster_test_recovery_id_uniqueness（5 组）+ 白盒钩子；既有 03-08 Provider 全路径测试不受影响。
- FULL Debug All UCN tests passed；ASan/UBSan 零报错；OBSERVED-PAIRS 30 零违例；Golden 8b80b08、trace mismatch 0；cluster_bytes 1608（无新增字段）；-Werror 干净；diff --check 干净。

## 5. 边界

- 跨重启的短序列去相关依赖 incarnation；硬不复用仍要求产品 Provider/持久 allocation history。M04 boot incarnation 不能把 32-bit hash 变成无碰撞分配器。
- REKEY purpose 仍只是接口预留（03-08 边界不变）。
