# CLV2-06-09 Test-only Current Behavior Bridge 自审报告

日期：2026-08-22  
状态：代码完成，分项自审通过；进入 M06 final self-audit。

## 隔离设计

| 构建对象 | `UCN_CLUSTER_ENABLE_TEST_HOOKS` | legacy v3 初始化 | 用途 |
|---|---:|---|---|
| `ucn_cluster` production archive | 未定义 | `PROVISIONAL + non-voting + deadline` | 产品语义 |
| `ucn_cluster_membership_model_tests` | 未定义，链接 production archive | 验证产品语义 | 防止 bridge 泄漏 |
| `ucn_tests` 自编译 membership copy | `UCN_CLUSTER_ENABLE_TEST_HOOKS=1` | `COMMITTED + voting + v3` | 仅维持旧 Current-FSM 回归 |
| `ucn_cluster_sim` Host copy | `UCN_CLUSTER_LEGACY_V3_TEST_BRIDGE=1` | `COMMITTED + voting + v3` | 仅维持历史 scale/failover 仿真模型 |

- 两个宏都只在专用 Host 测试 target 的 private compile definition 中出现；没有产品 CMake option、产品配置字段或公共 API 可打开它。
- 测试 target 才拥有 `src/extended/cluster` internal include path，用于断言 bridge 分支；production target 不接收该 target-only include/define。
- 新增 `cluster_test_m06_legacy_auto_commit_bridge()` 明确证明测试副本产生 committed/voting/v3、deadline canonical zero。
- 独立 membership model 已证明 production archive 的 `primary_member_allocate()` 恰好相反：v3 record 为 provisional/non-voting 且 bounded deadline；并且不具 Backup protected-voter 资格。

## 定向验证

```text
cmake -S . -B build_c06_full -G Ninja -DCMAKE_BUILD_TYPE=Debug  PASS
cmake --build build_c06_full --parallel                         PASS
ctest --test-dir build_c06_full --output-on-failure              4/4 PASS
```

同时审阅 CMake：`UCN_CLUSTER_ENABLE_TEST_HOOKS=1` 仅定义给 `ucn_tests`，`UCN_CLUSTER_LEGACY_V3_TEST_BRIDGE=1` 仅定义给 Host simulator；`ucn_cluster` 仍独立生成 archive。源码扫描确认没有产品配置、Adapter 或 `ucn_cluster.c` v4 production 接线。

## 删除约束

该 bridge 不是兼容性承诺。M07 `CLV2-07-12` 必须删除它，并通过 CI/source scan 证明没有 `JOIN_ACCEPT -> voting=true` 的直接路径；任何正式 v4 voter 变更只能由经过持久化的 Config transaction 完成。
