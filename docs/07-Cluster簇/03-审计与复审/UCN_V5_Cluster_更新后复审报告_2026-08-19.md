# UCN v5：2026-08-19 更新后复审报告

> 基线：`codex/v5-adaptive-wire@0fecd7f`（包含 `01-04f` 最新迁移）  
> 审计范围：`src/extended/ucn_cluster.c`、`src/extended/ucn_cluster_federation.c`、`tests/test_cluster.c`  
> 结论：**关键回归仍存在，尚未形成“可发布”结论**。

## 1. 先看结论（是否修复）

- ✅ 你这次更新后，部分早期缺陷已修复（见“已修复或缓解”）。
- ⚠️ 仍有 2 个高优先级功能缺陷和 1 个构建阻塞问题在 `DEBUG`/CI 可见路径仍在，**不能视为已修复**。
- ℹ️ 本地工作区当前 `git status` 显示源码是 clean 的，只有 untracked 文档文件。

## 2. 已修复或缓解（Evidence）

| 项目 | 文件/位置 | 说明 |
|---|---|---|
| token bucket 冷启动补偿 | [src/extended/ucn_cluster.c](../../../src/extended/ucn_cluster.c) | `token_bucket_refill()` 增加了 `last_refill_ms==0` 的冷启动分支，避免启动即触发“虚假的满量补发”导致速率失真。 |
| 邻居列表同步原子化 | [src/extended/ucn_cluster.c](../../../src/extended/ucn_cluster.c) | `ucn_cluster_sync_neighbors()` 改为先 stage 后 memcpy，避免溢出中途留下半写状态。 |
| `RECOVERY_HEAD -> MEMBER_ACTIVE` 迁移 | [src/extended/ucn_cluster.c](../../../src/extended/ucn_cluster.c) | `handle_head_takeover()` 的 Recovery Head 入站边先做 transition 后做 site-write，且失败时 fail-closed。 |
| 备份接管回放/状态更新顺序 | [src/extended/ucn_cluster.c](../../../src/extended/ucn_cluster.c) | `handle_backup_assign()` 的主链路已增加明显的角色路径约束与 post-commit derive 断言，降低了回放重入窗口的非一致行为。 |
| 成员/备份位宽边界约束 | [include/ucn/ucn_cluster.h](../../../include/ucn/ucn_cluster.h) | 在头文件加入 `UCN_CLUSTER_MAX_MEMBERS <= 32` 的静态断言，规避位移 UB。 |

## 3. 仍未修复（高优先级）

### P1-1: 主备同步回归仍可在快照仿真触发断言
- **现象**：`ucn_cluster_sim` 快速场景仍可直接触发断言崩溃。  
- **命令复现**：
  - `& .\build-v567-cluster-gcc\ucn_cluster_sim.exe --nodes 64 --scenario impaired --profile fast-fixed --seed 1592594996 --group 2`
  - `& .\build-v567-cluster-gcc\ucn_cluster_sim.exe --nodes 64 --scenario impaired --profile fast-fixed --seed 1592594996 --group 8`
- **证据**：
  - `--group 2`：`Assertion failed: cluster_phase_from_legacy_state(cluster, now_ms) == UCN_CLUSTER_PHASE_BACKUP_SYNCING, src/extended/ucn_cluster.c:3085`
  - `--group 8`：`Assertion failed: derived == UCN_CLUSTER_PHASE_HEAD_BACKUP_ASSIGNING, src/extended/ucn_cluster.c:4916`
- **风险**：在 `backup_assign` 与 `start_backup_assignment_cycle` 关键路径出现状态派生与站点写入不一致，可能导致 fast-profile 下主备恢复链抖动、阶段错位，影响实际网络收敛。
- **优先级**：P1（集群收敛与恢复正确性直接失效）

### P1-2: `BACKUP_ASSIGN` 仍先写共享快照再做回放转换校验
- **文件**：[src/extended/ucn_cluster.c](../../../src/extended/ucn_cluster.c)
- **现象**：在 `handle_backup_assign()` 中，`known_backup_node_id` 与 `known_backup_generation` 在 `cluster_transition()` 之前即被写入。若 transition 因 shadow-mismatch 被拒绝，字段已污染。
- **证据**：
  - 行内写入在 `cluster_transition()` 之前：`known_backup_node_id = message->sync_token;`
  - 返回 `UCN_ERR_STATE` 前未回滚该共享元数据（注释也明确写为“not rolled back”）。
- **风险**：在重放、乱序与角色回退场景下，可能引入“先污染再拒绝”的错误镜像，影响下一次 takeover/清理判断。
- **优先级**：P1（握手一致性核心路径，影响恢复正确性）

### P1-3: 跨簇缓存写入缺少时序幂等保护
- **文件**：[src/extended/ucn_cluster_federation.c](../../../src/extended/ucn_cluster_federation.c)
- **现象**：`cache_locator()` 无视旧记录 `term/record_nonce`，每次直接覆盖 cache 和 next-cluster，可能在分叉/延迟窗口把新鲜/陈旧条目互相覆盖。
- **风险**：导致目录查询缓存回退，路径解析到错误 head、错误 lease，影响簇间路由寻址一致性。
- **优先级**：P1（跨簇寻址正确性是 federation 基本契约）

### P2-1: MSVC 构建阻塞（可编译性）
- **文件/错误**：[src/extended/ucn_cluster.c:1049](../../../src/extended/ucn_cluster.c)
- **现象**：`cmake --build build-v567-cluster-full --target ucn_cluster_sim` 在 MSVC 下报 `error C2099`，同分支 GCC 可编译通过。
- **说明**：这类错误会让 Windows/VS 工程链路无法产出 sim 可执行与同源验证，发布自动化不完整。
- **优先级**：P2（工具链阻塞 + 回归覆盖缺失）

## 4. 回归验证清单（已执行）

| 场景 | 命令 | 结果 |
|---|---|---|
| 单元测试 | `ctest --output-on-failure --test-dir build-v567-cluster-gcc -R ucn_tests` | 通过 |
| 全量集成（GCC） | `ctest --output-on-failure --test-dir build-v567-cluster-gcc` | 只 `ucn_cluster_64_fast_impaired` 失败 |
| fast impaired 原复现 | `ucn_cluster_sim` 上述 `--group 2 / 8` 命令 | 均可复现断言 |
| MSVC 编译验证 | `cmake --build build-v567-cluster-full --target ucn_cluster_sim` | 失败：C2099 |

## 5. 下一步建议（按优先级）

1. 优先修复 `handle_backup_assign` 的“先转移再写入共享身份”次序，并补充 `tests/test_cluster.c` 里对应“过期/重放失败不污染镜像”的回归用例。  
2. 在 `cache_locator` 引入 `cluster_id/term/record_nonce` 的单向时序保护（拒绝旧 epoch/旧 nonce 覆盖）。  
3. 用同一组 `seed+group` 组合把 `ucn_cluster_64_fast_impaired` 固定到可复现的断言场景并补充成回归用例。  
4. 修复 MSVC 的 `C2099`（建议优先最小化变更范围到数组定义初始化语义），避免工具链阻塞。  
