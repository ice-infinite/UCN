# CLV2-06-07 Public Summary 自审报告

日期：2026-08-22  
状态：代码完成，分项自审通过；外部审计并入 M06 final。

## 实现核对

- `ucn_cluster_member_summary_t` 新增 `status`、`voting`、`config_id`；前两项复制 owner record，`config_id` 只在 canonical `active_voter_set` 合法时返回其值，否则为零。
- 统一由 private `member_summary_fill()` 填充 compact-copy 与 indexed-copy 两个只读 API，避免两个 API 字段漂移。
- API 仍只返回值拷贝，不返回 `primary_members` 地址；任何 caller 对 summary 的修改不会写回 Cluster record 或 active voter set。
- 空 slot、NULL Cluster 等失败路径均在写 output 前返回，保持 caller 输出不变。

## 定向验证

```text
cmake --build build_c06_full --parallel                         PASS
ctest --test-dir build_c06_full --output-on-failure              4/4 PASS
```

模型建立 v3 provisional node 9 与 canonical config_id=44，验证两种 summary API 都报告 `PROVISIONAL/non-voting/44`；随后篡改 caller-side summary，验证 owner record 和 active voter set 未改变。空 slot/NULL 的 indexed-query 也验证输出完整保持哨兵值。

## 限制

`config_id` 是当前 active voter-set 的诊断投影，M06 尚未实现 M07 的真实 Config transaction；该字段不会授予 voter、Backup、Head 或 Authority。M05 production v4 RX/TX/FSM 与 encoder 隔离不变。
