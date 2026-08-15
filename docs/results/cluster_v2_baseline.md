# UCN V5 Cluster v2 迁移基线（CLV2-M00-01）

> 文档性质：不可回退基线记录。M00 门禁：所有现有测试、规模模拟、静态分析结果可重复；生成基线清单和状态迁移 Trace。

## 1. 基线 Hash

- 代码语义基线：`a5718534ef4014f240bbe8b1640dde0328eb8669`（32B Cluster Wire v3、Type 1-19、全部 C07.7 收尾修复）。
- 基线建立时工作树 HEAD：`adc1b10`（含文档类提交：迁移方案入仓、头文件 28B→32B 注释修正、UniLink 品牌改名；均不影响代码语义）。
- 后续提交必须能回到此基线重放全部证据；协议语义变更前必须先在此基线跑通 Gate。

## 2. 构建命令（WSL Ubuntu-24.04-ROS）

```bash
cmake -B build-c07-wsl -DUCN_PROFILE=FULL -DUCN_FEATURE_SERVICE=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-c07-wsl -j8
ctest --test-dir build-c07-wsl --output-on-failure
```

## 3. 测试证据（FULL）

- `ctest`：22/22 通过（含 ucn_tests 全部 Cluster 故障注入、64 节点 clean/impaired/fast_head_failover、scale 系列）。
- `-fanalyzer`（gcc -std=c99 -Wall -Wextra -Werror -fanalyzer src/extended/ucn_cluster.c）：0 告警。

## 4. 资源基线（FULL，Host x64 Debug）

- `sizeof(ucn_cluster_t)`：1080 B（`UCN_PROFILE` 运行时输出 cluster_bytes=1080；含 C07.7 收尾新增字段：join txid、stepdown nonce、vote identity (cluster/term/generation)、delta cursor、reject cooldown 等；`ucn_cluster_federation` 3328 B）。
- Core-only 构建（不链接 ucn_cluster）：Core RAM 不因 Cluster 增加（Cluster 为 `EXCLUDE_FROM_ALL` 按需库）。

## 5. 构建矩阵门禁（CLV2-00-02）

| Profile | Service | Cluster | 状态 |
|---|---|---|---|
| FULL | ON | ON | 通过（22/22） |
| LITE | ON | ON | 通过（22/22） |
| NANO | ON | ON | 通过（12/12，Profile 裁剪） |
| Core-only | - | OFF | 通过（ucn_core 独立编译，Cluster 为 EXCLUDE_FROM_ALL 按需库） |

## 6. Sanitizer 门禁（CLV2-00-06）

- ASan/UBSan（FULL）：22/22 通过（`-fsanitize=address,undefined -fno-omit-frame-pointer`，ASAN_OPTIONS=detect_leaks=0）。

## 7. M00 新增观测/基建（不影响协议行为）

- CLV2-00-03：Golden Transition Trace（tests/test_cluster.c 内观测层 + tests/golden/cluster_golden_trace.txt，37 行 old/event/new 三态格式；M01 重构必须保持字节级一致；reference 缺失 fail-closed，仅 `UCN_UPDATE_CLUSTER_GOLDEN=1` 可重生成）。
- CLV2-00-07：故障注入网络（partition 矩阵/heal、deliver budget、drop/dup/delay/reorder、节点重启、Owner step skip、neighbor state override、xorshift32 seed、storage-failure M04 placeholder），8 个故障测试（分区多数派 takeover、重启无旧 term、丢包最终收敛、deterministic replay、dup/delay/reorder 收敛、skip owner step、neighbor flap、partition heal）。
- CLV2-00-04：测试夹具 `tests/cluster_test_fixture.[ch]`（set_epoch/set_role/set_backup/set_vote），代表性测试已迁移。
- CLV2-00-06：Codec fuzz smoke `tests/test_cluster_fuzz.c`（Type1-19 合法 seed × xorshift32 固定种子变异 20000 cases，decode 返回值集合 + decode-OK 不变量 + 长度损坏用例 + live receive 集合；在 ASan/UBSan 路下运行）。
- CLV2-00-05：资源基线 `tools/cluster_size_report.sh` 输出 `docs/results/m00_matrix/size_report.md`（各 Profile sizeof/.text/.rodata/.data/.bss/max static stack + CORE_ONLY cluster-absent 断言）。

## 8. M00 完整重跑证据（审计版，5 路 → 6 路）

在 WSL Ubuntu-24.04-ROS（gcc 13.3.0 / cmake 3.28.3 / ninja 1.11.1）用全新构建目录重跑完整门禁：

| # | 构建 | 配置 | 结果 | 日志 |
|---|------|------|------|------|
| 1 | build-m00-full | FULL Debug, Ninja | **22/22 通过** | `docs/results/m00_matrix/m00-full.log` |
| 2 | build-m00-lite | LITE Debug, Ninja | **22/22 通过** | `docs/results/m00_matrix/m00-lite.log` |
| 3 | build-m00-nano | NANO Debug, Ninja | **12/12 通过** | `docs/results/m00_matrix/m00-nano.log` |
| 4 | build-m00-asan | FULL + `-fsanitize=address,undefined -fno-omit-frame-pointer` | **22/22 通过** | `docs/results/m00_matrix/m00-asan.log` |
| 5 | build-m00-analyzer | FULL + `-fanalyzer` | **22/22 通过**；编译 0 告警（-Werror 下告警即构建失败） | `docs/results/m00_matrix/m00-analyzer.log` |
| 6 | build-m00-core_only | Core-only（TESTS/SCALE/CLUSTER_SIM/SERVICE 全 OFF，仅 ucn_core） | **通过**：`libucn_cluster.a` 不存在（Cluster OFF 不付 RAM/Flash） | `docs/results/m00_matrix/core_only.log` |

- 门禁脚本 `tools/m00_matrix.sh` 已 fail-closed：6 路逐 PID `wait` 收集状态，任何一路 configure/build/ctest 失败或 core-only 断言失败 → 整体退出非 0；负向验证（人为注入 `-DUCN_PROFILE=BOGUS` 使 nano 路失败）→ 脚本非 0 退出、`MATRIX: FAILURE`（见 `tools/m00_negative_gate_check.sh`）。
- `ucn_tests` 直跑输出：`cluster_bytes=1080 federation_bytes=3328 node_bytes=10080 ...` + `All UCN tests passed.`
- Golden Gate（M00.1 后）：reference 只读（源树 `tests/golden/`）、actual 写各自 build 目录（5 路并行无文件竞争）；**reference 缺失 = 测试 FAIL**，仅 `UCN_UPDATE_CLUSTER_GOLDEN=1` 允许显式重生成并提交；负向验证（临时移走 golden）→ 测试失败、进程非 0 退出。
- Trace 升级为 `old/event/new` 三态行（SYNC / STEP / RX:<type> 事件），37 行覆盖选举→入簇→Backup 指派→主断接管完整生命周期。

记录时间：2026-08-15（M00.1 审计闭环比；早期记录曾因 WSL 时钟漂移误写 08-17，已更正）。


## 9. M00 任务逐项审计表（CLV2-00-01 ~ 08，M00.1 闭环比）

| 任务 | 状态 | 证据位置 |
|---|---|---|
| CLV2-00-01 基线冻结 | ✅ PASS | 语义基线 `a5718534…`（本文件 §1）；`6bea852` 测试基建；生产 `src/extended/ucn_cluster.c` 在 M00/M00.1 零语义改动（diff 仅注释，见提交序列） |
| CLV2-00-02 构建矩阵 | ✅ PASS | §8 六路表：FULL/LITE 22/22、NANO 12/12、ASan/UBSan 22/22、fanalyzer 0 告警、CORE_ONLY cluster-absent 断言 |
| CLV2-00-03 Golden Trace | ✅ PASS（M00.1 封死 fail-open） | 37 行 old/event/new；reference/actual 目录分离（无并行写竞争）；missing→FAIL；`UCN_UPDATE_CLUSTER_GOLDEN=1` 显式重生成 |
| CLV2-00-04 Test Fixture | ✅ PASS | `tests/cluster_test_fixture.[ch]`；代表性测试已迁移（任务为逐步迁移，不要求一次清空） |
| CLV2-00-05 资源基线 | ✅ PASS（M00.1 补齐） | `docs/results/m00_matrix/size_report.md`：FULL/LITE/NANO sizeof=1080、.text=30780、.rodata=320、max static stack=208；CORE_ONLY ucn_core .text=137360、cluster 未链接 |
| CLV2-00-06 Sanitizer/Analyzer/Fuzz | ✅ PASS（M00.1 补 fuzz smoke） | ASan/UBSan 22/22；-Werror 严格告警；-fanalyzer 0 告警；Type1-19 codec fuzz smoke 20000 cases（`tests/test_cluster_fuzz.c`）在 ASan/UBSan 下运行 |
| CLV2-00-07 Fault Model | ✅ PASS（M00.1 补齐注入面） | §7：partition/heal、drop/dup/delay/reorder、restart、skip-step、neighbor override、xorshift32 seed、storage placeholder（M04 连接）；8 个故障测试含 deterministic replay |
| CLV2-00-08 任务跟踪 | ✅ PASS（M00.1 逐项绑定） | 本表 + 方案文档里程碑状态表（AUDIT HOLD→用户复签）+ OP-185/186 操作记录 |

## 10. 日期更正说明

- 早期记录曾写「2026-08-17」：系 WSL 时钟漂移所致（实际 Host/WSL/Git 三者现均为 2026-08-15 +0800）。本文件已统一为 2026-08-15。

---

记录时间：2026-08-15（M00.1 审计闭环比）。由 CLV2-M00 任务生成；每个后续里程碑增量更新。