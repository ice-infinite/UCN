# CLV2-06-06 Legacy v3 Member 自审报告

日期：2026-08-22  
状态：代码完成，分项自审通过；外部审计并入 M06 final。

## 生产语义

- 产品 archive 中，`primary_member_allocate()` 的既有 v3 Join producer 通过 `member_initialize_legacy()` 生成 `wire=v3 + PROVISIONAL + non-voting`，并建立 bounded provisional deadline。
- `primary_member_is_protected_voter()` 在产品构建只接受 `COMMITTED + voting + wire=v4`。因此 v3 legacy/provisional record 不能被 `assign_backup()` 选为 Backup，也不能被 v3 takeover ACK、self vote、prepare fan-out 或 Recovery mirror quorum 当作 protected voter。
- 这不是“把 v3 解释成 v4”：没有新增 v4 raw RX/TX、capability negotiation、certificate、Config Commit 或 Authority。

## 过渡测试桥

`ucn_tests` 仍需要保持当前 v3 Current-FSM 基线，所以只有它自行编译的 membership 副本带 `UCN_CLUSTER_ENABLE_TEST_HOOKS=1`。该副本把 legacy fixture 临时初始化为 committed/voting，保留旧回归；正常 `ucn_cluster` archive 和 membership model target 不带该宏，实际验证产品分支。

这只是 M06 到 M07 的测试桥，不能由产品配置开启；06-09 还会补齐其 CMake、符号和严格生产反例的最终隔离门禁，并由 M07-12 删除。

## 定向验证

```text
cmake --build build_c06_full --parallel                         PASS
ctest --test-dir build_c06_full -R "ucn_cluster_membership_model_tests|ucn_tests" --output-on-failure
  ucn_tests                              PASS (test-hook bridge)
  ucn_cluster_membership_model_tests     PASS (production archive)
```

production-archive model 直接调用 `primary_member_allocate()`，验证 v3 record 为 provisional/non-voting、deadline=150；同一 record 虽有 Candidate metadata 也无法被 `assign_backup()` 选择。将其显式转换为 canonical committed/voting/v4 后，才可被选择，证明筛选条件不是仅检查 slot occupied。

## 边界核对

- CMake 中 `UCN_CLUSTER_ENABLE_TEST_HOOKS=1` 只属于 `ucn_tests` 的自编译 test copy；生产 `ucn_cluster` target 无该定义。
- `src/extended/ucn_cluster.c` 的 v4 搜索为空，Adapter 搜索为空；encoder 默认关闭。
- `git diff --check` 对本项文件无空白错误；仅既有 CRLF 提示。

结论：产品 v3 legacy Join 已不具备 Backup/protected-voter 资格。真实 v4 Join 接线、Config Commit 与 protected quorum/certificate 仍不在本项范围。
