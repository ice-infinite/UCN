# CLV2-06-04 Provisional Admission 自审报告

日期：2026-08-22  
状态：代码完成，分项自审通过；外部审计并入 M06 final。

## 实现核对

- 新增内部 `cluster_admit_verified_v4_provisional_member()`：其输入不是 raw wire frame；调用前置条件明确为未来 RX Owner 已完成 v4 Type、role、source、epoch 与 capability 验证。
- 该 helper 仅允许当前 `HEAD` 写入空 Runtime slot，写入 `occupied + PROVISIONAL + non-voting + wire=v4`；不会修改 `active_voter_set`、Backup、takeover 或 Authority。
- 对已经存在的相同 v4 provisional member 为幂等成功且不覆盖已有 capability/time；冲突的 existing record、非 Head、无效 ID、容量耗尽均失败且不写新状态。
- 现有 `ucn_cluster_receive()`、v3 JOIN path、v4 codec、encoder 和 production FSM 都没有调用该 helper。也就是说这是 M06 admission 数据语义，不是 v4 production RX 接线。

## 定向验证

```text
cmake --build build_c06_full --parallel                         PASS
ctest --test-dir build_c06_full -R "ucn_cluster_membership_model_tests|ucn_tests" --output-on-failure
  ucn_tests                              PASS
  ucn_cluster_membership_model_tests     PASS
```

模型验证建立旧 voter set `{2,7,11}` 后接纳 node `21`：Runtime count 增加、record 为 v4 provisional/non-voting，而 voter count 仍为 3、quorum 仍为 2、node 21 不在旧 voter set。因此 Head 立即失效时，新节点不会被算入旧 protected quorum。另覆盖幂等、非 Head、非法 ID、运行时容量耗尽与失败无写回。

## 隔离核对

- `src/extended/ucn_cluster.c` 没有 `ucn_cluster_wire_v4` 或 `UCN_CLUSTER_WIRE_V4` 引用。
- 新 helper 只出现在 membership module、internal header 与 model test；不包含 private v4 semantic header。
- Adapter/include Adapter 无 Cluster 引用；encoder 默认关闭未变。
- `git diff --check` 对本项文件无空白错误；仅既有 CRLF 提示。

结论：06-04 满足“验证完成后的 Join 先成为 provisional”的数据安全边界；真实 wire 接收、Config Commit、voter 变更、持久化与 Authority 仍由 M05/M07/M10 后续任务负责。
