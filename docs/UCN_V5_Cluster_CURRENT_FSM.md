# UCN V5 Cluster 当前实际状态机

> 分支：`codex/v5-adaptive-wire`
>
> 对齐日期：2026-08-25
>
> 事实源：`include/ucn/ucn_cluster.h`、`src/extended/ucn_cluster.c`、`src/extended/cluster/`
> 边界：本文描述当前默认产品与已编译组件；default-OFF 实验模型不等于生产接线。

## 1. 当前结论

当前 Cluster 已经是 **Phase 唯一生命周期状态源**。公开 Role 仅由 `ucn_cluster_phase_to_role()` 从 Phase 派生；应用不能直接访问内部 `ucn_cluster_t` 字段，也不能以旧 Role/bool 组合铸造状态。

```text
唯一 Protocol Owner
  -> ucn_cluster_step()/ucn_cluster_receive()
  -> cluster_transition()
  -> ucn_cluster_phase_t
  -> phase_to_role() 只读视图
```

内部仍有 deadline、计数器、快照游标和事务对象，但它们是 Phase 内数据，不是第二套生命周期状态源。

## 2. 冻结 Phase 编号

| 值 | Phase | 当前默认产品可达性 |
|---:|---|---|
| 0 | `DISABLED` | 可达 |
| 1 | `DETACHED_OBSERVE` | 可达 |
| 2 | `ELECTION` | 可达 |
| 3 | `JOIN_PENDING` | 可达 |
| 4 | `MEMBER_ACTIVE` | 可达 |
| 5 | `MEMBER_TAKEOVER_GRACE` | 可达 |
| 6 | `HEAD_NO_BACKUP` | 可达 |
| 7 | `HEAD_BACKUP_ASSIGNING` | 当前 v3 Authority RX 围栏下受限 |
| 8 | `HEAD_BACKUP_SYNCING` | 当前 v3 Authority RX 围栏下受限 |
| 9 | `HEAD_STABLE` | 可达 |
| 10 | `BACKUP_SYNCING` | 当前 v3 Authority RX 围栏下不可由生产 v3 建立 |
| 11 | `BACKUP_READY` | 同上 |
| 12 | `BACKUP_TAKEOVER` | 同上 |
| 13 | `STEPPING_DOWN` | 可达 |
| 14 | `RECOVERY_OBSERVE` | 可达 |
| 15 | `RECOVERY_ELECTION` | 可达 |
| 16 | `RECOVERY_HEAD` | 可达 |
| 17 | `TERM_CONFLICT_WAIT` | 可达，M11 Fence 与其构成双保险 |
| 18 | `HEAD_RECONFIGURING` | M07 default-OFF owner |
| 19 | `HEAD_QUORUM_GRACE` | M08 Authority owner |
| 20 | `HEAD_FENCED` | M08 Authority owner |
| 21 | `HEAD_REKEYING` | M13 default-OFF owner |

`UCN_CLUSTER_PHASE_COUNT` 固定为 22；新增 Phase 只能追加，禁止重编号。

## 3. 默认产品主路径

```mermaid
flowchart LR
  D[DETACHED_OBSERVE] --> E[ELECTION]
  E --> H[HEAD_NO_BACKUP/HEAD_STABLE]
  E --> J[JOIN_PENDING]
  J --> M[MEMBER_ACTIVE]
  M --> G[MEMBER_TAKEOVER_GRACE]
  G --> D
  H --> S[STEPPING_DOWN]
  S --> J
  M --> RO[RECOVERY_OBSERVE]
  RO --> RE[RECOVERY_ELECTION]
  RE --> RH[RECOVERY_HEAD]
```

同 Cluster Authority 比较使用完整 Epoch；foreign Cluster 不比较数值 Term。serial 均使用 checked-next/no-wrap，达到保留阈值后 fail-closed 或进入 Rekey/Recovery rotation 规划。

## 4. Wire 当前边界

- 默认生产 Cluster 仍使用 format v3、固定 32 B、Type 1..19；
- format v4 固定 40 B、Type 1..33，codec/semantic/dispatcher 已独立归档并经过受限测试；
- `UCN_CLUSTER_WIRE_V4_ENCODER_ENABLED` 默认 0；
- M05 顶层仍为 `AUDIT HOLD`，所以默认 RX/TX/FSM/Authority 不接入 v4；
- 生产 v3 对 Backup/Takeover Authority 类 Type 8、10..15、18、19 在状态写入前统一拒绝。

## 5. Persistence 当前边界

- Provider API v1，Cluster public API/storage layout v2；
- 当前 writer 为 Record schema v4、388 B；v1/v2 280 B、v3 292 B 只读迁移；
- REQUIRED 模式遵守 load-before-init 与 persist-before-promise；
- M04 软件合同通过不等于真实 Flash 双槽/断电实测；
- M07/M10/M13 的 Config/Takeover/Rekey owner 仍是 default-OFF 实验集成。

## 6. Public storage 与资源

`ucn_cluster_t` 在公共头中为 opaque。唯一 Owner 额外包含 `ucn_cluster_storage.h` 后，以完整的 `ucn_cluster_t` 静态分配；默认预算 2048 B，当前 Host Release object 为 1608 B。Cluster 源码禁止动态分配。

## 7. 已知未闭环项

1. M05 未放行 production v4 RX/TX/FSM/Authority；
2. M14-03、14-07 因 M05 只能部分完成；
3. M14-08 缺四板、真实 Flash 和可控断电证据；
4. 因上述边界，当前不能宣称 Target v2 已成为完整生产状态机，也不能建立 release tag。

## 8. 关联文档

- Target 规格：`UCN_V5_Cluster_FSM_Design_v2.md`
- Wire：`UCN_Cluster_Wire_v4.md`
- Persistence：`UCN_Cluster_Persistence_v4.md`
- Config：`UCN_Cluster_Config_v2.md`
- Timer：`UCN_Cluster_Timer_Algebra.md`
- API 迁移：`UCN_Cluster_API_v2_迁移指南.md`
- 自动契约：`UCN_Cluster_Code_Doc_Contract.md`
