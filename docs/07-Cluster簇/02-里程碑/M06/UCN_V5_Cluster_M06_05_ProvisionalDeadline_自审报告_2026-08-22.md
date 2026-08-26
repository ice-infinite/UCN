# CLV2-06-05 Provisional Deadline 自审报告

日期：2026-08-22  
状态：代码完成，分项自审通过；外部审计并入 M06 final。

## 实现核对

- `ucn_cluster_config_t.provisional_timeout_ms` 位于结构体尾部，零值使用 profile default；没有破坏旧位置初始化。默认/fast profile 分别为 10000 ms / 3000 ms，初始化时采用 `ucn_duration_is_valid()` 约束。
- Runtime record 增加 `provisional_deadline_armed + provisional_deadline_ms`。`PROVISIONAL` 必须 armed 且 non-voting；`NONE/COMMITTED/REMOVING` 必须 deadline 字段为 canonical zero，避免 stale deadline 混入其他状态。
- `cluster_admit_verified_v4_provisional_member()` 在首次成功接纳时建立绝对 deadline；同一 provisional 的重复接纳不延期，避免无限刷新容量占用。
- `primary_member_expire_provisionals()` 仅由 Head 处理 `PROVISIONAL + armed + expired` record，逐项清零并计数；不会删除 committed record、修改 `active_voter_set` 或使 provisional 成为 Backup。
- 常规 `expire_members()` 先回收 provisional，再按既有 lease 规则处理非-provisional 成员；provisional 不进入 Backup-expiry 判断。

## 定向验证

```text
cmake --build build_c06_full --parallel                         PASS
ctest --test-dir build_c06_full -R "ucn_cluster_membership_model_tests|ucn_tests" --output-on-failure
  ucn_tests                              PASS
  ucn_cluster_membership_model_tests     PASS
```

模型覆盖 100 ms 接纳、150 ms deadline：149 ms 未回收、150 ms 精确回收、Runtime capacity 立即允许新 provisional 加入；同时验证 committed record 不会由 provisional owner 删除。record 合法域测试覆盖 PROVISIONAL deadline 的 canonical 条件。

## 限制与边界

- 此 deadline 只处理 M06 Runtime provisional 生命周期；它不是 M07 `CONFIG_COMMIT` 或持久化 transaction 的替代品。
- `ucn_cluster.c` 仍没有 v4 RX/TX/FSM 引用，Adapter 无 Cluster v4 引用，encoder 默认关闭未变。
- `git diff --check` 对本项文件无空白错误；仅既有 CRLF 提示。

结论：丢失后续 Config 消息不会让 provisional Runtime entry 永久占用容量；它仍不进入 voter、Backup、takeover 或 Authority。
