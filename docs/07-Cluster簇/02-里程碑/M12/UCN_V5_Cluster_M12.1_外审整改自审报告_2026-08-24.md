# UCN V5 Cluster M12.1 外审整改分项报告（2026-08-24）

## 1. 结论

针对 b2fcde8 外审 4 MAJOR + 1 MINOR 的收口整改。结论: SELF-AUDIT PASS / WAIT EXTERNAL RE-REVIEW。

## 2. 逐项整改证据

| 项 | 修复 | 对抗测试 |
|---|---|---|
| MAJOR-1 durable Recovery Term | RECOVERY_CREATE_COMMIT operation 类 + 期望 epoch 逐字段校验 + declare 采纳 durable epoch | persist 测试: T9 同步/异步/重启一致 + Term 失配 fail-closed |
| MAJOR-2 成员当前赢家 fencing | recovery_candidate_outranks_current_head（term DESC, node ASC vs 当前 Head） | winner_fence 4 组（H1/H2 双向 + T9/T8 双向） |
| MAJOR-3 config 前向刷新 | 同 parent 分支 binding>当前值才升级，round 保留 | lineage 测试 c4（C12->C13 升级、stale C9 不回退） |
| MAJOR-4 12-09 scope | 正式降级 PARTIAL；重启入站 replay 归 M13 | 报告第 0 节 + OP-368 补正 |
| MINOR 12-08 措辞 | 可见远端多数（保守隔离），非 Config-majority | 注释更新 |

## 3. 门禁

FULL/ASan/LITE/NANO 全绿；OBSERVED 30 零违例；Golden 8b80b08、mismatch 0；cluster_bytes 1616；-Werror、diff --check 干净。
