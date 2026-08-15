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

- CLV2-00-03：Golden Transition Trace（tests/test_cluster.c 内观测层 + tests/golden/cluster_golden_trace.txt，34 行；M01 重构必须保持字节级一致）。
- CLV2-00-07：故障注入网络（partition 矩阵、deliver budget、drop_one_in、节点重启），新增 3 个故障测试（分区多数派 takeover、重启无旧 term、丢包最终收敛）。
- CLV2-00-04：测试夹具 `tests/cluster_test_fixture.[ch]`（set_epoch/set_role/set_backup/set_vote），代表性测试已迁移。

## 8. M00 完整重跑证据（审计版，2026-08-17 干净矩阵）

在 WSL Ubuntu-24.04-ROS（gcc 13.3.0 / cmake 3.28.3 / ninja 1.11.1）用全新构建目录重跑完整门禁，5 路并行：

| # | 构建 | 配置 | 结果 | 日志 |
|---|------|------|------|------|
| 1 | build-m00-full | FULL Debug, Ninja | **22/22 通过**（3.00 s） | `docs/results/m00_matrix/m00-full.log` |
| 2 | build-m00-lite | LITE Debug, Ninja | **22/22 通过**（2.56 s） | `docs/results/m00_matrix/m00-lite.log` |
| 3 | build-m00-nano | NANO Debug, Ninja | **12/12 通过**（2.05 s） | `docs/results/m00_matrix/m00-nano.log` |
| 4 | build-m00-asan | FULL + `-fsanitize=address,undefined -fno-omit-frame-pointer` | **22/22 通过**（12.37 s） | `docs/results/m00_matrix/m00-asan.log` |
| 5 | build-m00-analyzer | FULL + `-fanalyzer` | **22/22 通过**；编译 0 告警（-Werror 下告警即构建失败） | `docs/results/m00_matrix/m00-analyzer.log` |

- `ucn_tests` 直跑输出：`UCN_PROFILE name=Full value=3 service=1 node_bytes=10080 link_bytes=40 event_runtime_bytes=432 stream_source_bytes=240 stream_default_storage_bytes=771 can_source_bytes=256 can_default_storage_bytes=1184 transfer_bytes=8840 transfer_rx_bytes=8192 cluster_bytes=1080 federation_bytes=3328` + `All UCN tests passed.`
- Golden Trace 复核：`tests/golden/cluster_golden_trace.txt` 与测试运行时生成的 `cluster_golden_trace_actual.txt` 逐字节 **IDENTICAL**（各 34 行），M01 门禁就绪。
- 测试清单（22 项）：ucn_tests（含全部 Cluster 故障注入、golden trace、夹具迁移用例）、cluster sim 64/256/1000 clean+impaired、64 head-failover/mobility/score-shift、64 fast head-failover/fast impaired、scale w0/w1/w2/w3/mixed smoke、tree capacity contract、w0/mixed address-limit reject（WILL_FAIL）。
- 复跑脚本：`tools/m00_matrix.sh`（rm -rf 全新目录 → configure → build → ctest → 汇总），可随时重放本证据。

记录时间：2026-08-17（M00 审计前完整重跑）。

---

记录时间：2026-08-17（含审计前完整重跑）。由 CLV2-M00 任务生成；每个后续里程碑增量更新。