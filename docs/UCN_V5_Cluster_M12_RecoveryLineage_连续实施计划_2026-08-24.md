# UCN V5 Cluster M12 RecoveryLineage 连续实施计划（2026-08-24）

## 1. 授权与范围

- 用户指令：接替完成 M12；**每完成一个小节点即自审，自审通过才开始下一个节点**；全部节点完成后做 M12 全量自审，通过后整理外部审计材料并停在外部审计边界（不提交、不推送）。
- 里程碑目标（任务表 M12 段 L747-772）：把当前临时岛升级为**可追溯、可排序、不会自旋且不继承旧 Authority** 的 Recovery 控制域。
- 里程碑门禁：① Recovery 使用新 cluster_id；② 同 lineage 先比较 parent term/config；③ 连续失败退避升级。
- 禁止事项：① 禁止 Recovery 使用 parent cluster_id；② 禁止忽略 parent term/config；③ 禁止 TTL 后固定间隔自旋。

## 2. 现场核对（HEAD=9386dca）

- M05 顶层仍 AUDIT HOLD；M08 仍 WAIT EXTERNAL；M10 待外审复审；M09 AUDIT HOLD；M11 已外审 GO（受限实验）。M12 依赖 M11，且**默认产品不接入 v4 RX/TX/FSM/Authority/encoder**。
- 既有可复用资产：M03-06 运行时历史 `last_cluster_id/max_seen_term/last_stable_head`（set_detached 不清）；M03-08 cluster ID provider（request 已携带 purpose/parent cluster/term/round/incarnation，默认生成器已保证恢复域新 ID）；M04 persist-before-promise + 受控启动 incarnation；M08 Authority/Fence（12-05 衔接）；M01 时代 zero-backoff 退化自旋（OP-210 明示留待 M12 修复）。
- 现状缺口（逐节点对应）：lineage 未显式成域（12-01）；Recovery ID 未绑 config/round 语义（12-02）；退避为 node_id%max 线性且零值退化（12-03）；仲裁仍是 (nonce,node_id) 而非 lineage rank（12-04）；Recovery Head 无权威范围限制（12-05）；Declare/ACK 未绑 round（12-06）；Recovery Member 无 Stable 优先让位路径（12-07）；min_recovery_peers 不可配（12-08）；round/lineage 未持久化（12-09）；缺端到端场景套件（12-10）。

## 3. 逐节点验收标准（完成定义与测试）

| 节点 | 优先级 | 依赖 | 完成定义（逐条可测） |
|---|---|---|---|
| CLV2-12-01 | P0 | M11 | `ucn_cluster_t` 新增 `parent_cluster_id/parent_term/parent_config_id/recovery_round`（`recovery_cluster_id` 已有）；Member 宽限超时、Backup missed-heartbeat 两个 Fence 离簇点在**清除 Active/Pending 前**捕获 lineage；Detach 不丢 parent；重新进入恢复域时同 parent 保留 round、异 parent 归零；view 暴露只读快照。 |
| CLV2-12-02 | P0 | 12-01 | Recovery ID 经 cluster ID provider 生成，请求携带 parent cluster/term/**config_id**/**recovery_round**/node/incarnation；结果禁止等于 parent、禁止 0/broadcast；同节点同 parent 不同 round 得不同 ID；不同 boot incarnation 得不同 ID。 |
| CLV2-12-03 | P1 | 12-01 | 退避 = min(max, base<<attempts) + 确定性抖动（由 parent/round/node 派生）；TTL 到期/仲裁失败 round++；**消除零值退化自旋**（M01 OP-210 遗留）；稳定加入 Stable Cluster 持续 `recovery_lineage_reset_ms` 后 lineage+round 重置。 |
| CLV2-12-04 | P0 | 12-01 | 纯函数 lineage rank：同 parent → parent_term DESC、parent_config_id DESC、score DESC、node_id ASC；异 parent 不可比（走 Merge）；`handle_recovery_declare` 仲裁从 (nonce,node_id) 换成 rank；T9 island 不被 T8 高 score 压制（定向测试）。 |
| CLV2-12-05 | P0 | 12-01 | Recovery 域只拥有 recovery-local authority：不得发布为 parent Stable Authority；禁止对 parent cluster_id 执行 takeover/config commit；Federation/Directory 标记 Recovery scope；无 v4/Authority 生产接线违规。 |
| CLV2-12-06 | P1 | 12-01 | RECOVERY_DECLARE 绑定 recovery cluster_id + 当轮 recovery_nonce；ACK 必须回显当前轮 (cluster_id,nonce)；旧轮 declare/ack 以 REPLAY 拒绝且零写；重复 ACK 幂等（recovery_acked 位图）；成员 lease 仅当轮刷新。 |
| CLV2-12-07 | P0 | 12-01 | 任意合法 Stable Head 优先：RECOVERY_HEAD 有序让位（既有路径）；Recovery 域 Member 收到 parent lineage 的合法 Stable offer 时**不经 score/外簇判定**直接 JOIN_PENDING（新 reason STABLE_RECLAIM）；Stable reclaim 定向测试。 |
| CLV2-12-08 | P1 | 12-01 | 配置 `min_recovery_peers`（0=默认）；普通 Member 门槛 = max(1, min_recovery_peers)；带 mirror Backup 门槛 = 镜像多数派（不变，注释明确区分）；1/2/多节点 island 测试。 |
| CLV2-12-09 | P0 | 12-01 | 持久化 recovery round/lineage（或至少 boot incarnation+tombstone）：重启后不复用旧 Recovery ID/nonce；旧恢复域 cluster_id 成为 tombstone，其帧被 replay 拒绝；重启 replay 测试。 |
| CLV2-12-10 | P0 | 12-01..09 | Recovery 套件：Primary+Backup 同死、多候选仲裁、两个同/异 lineage island、TTL 循环（退避递增不自旋）、Stable reclaim、节点重启（ID 不复用）。 |

## 4. 门禁与节奏

- 每节点自审：FULL Debug 构建 + ucn_tests 全绿（含既有全部回归）+ 该节点定向测试逐条 PASS；P0 节点另跑 ASan/UBSan；OP 记录（OP-351 起）+ 分项自审报告（`docs/UCN_V5_Cluster_M12_<节点>_自审报告_2026-08-24.md`）。
- 全量自审（12-10 后）：FULL/ASan/LITE/NANO 四 profile 全绿；OBSERVED-PAIRS 零违例；Golden `8b80b08` 不变（或差异逐行归因）；cluster_bytes 资源账；`-Werror` 干净；`git diff --check` 干净；三条禁止事项 + 三条里程碑门禁逐条核对；M12 全量自审报告 + 外部审计材料。
- 全部工作留在工作树 `ucn-wt-m35`（分支 wt/m35），不提交、不推送；不接入 v4/Authority/Adapter 生产路径。
