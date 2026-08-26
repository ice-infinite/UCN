# CLV2-06-03 Voter Set 自审报告

日期：2026-08-22  
状态：代码完成，分项自审通过；外部审计并入 M06 final。

## 实现核对

- `ucn_cluster_voter_set_t` 与 Runtime `primary_members` 物理分离；容量固定为 `UCN_CLUSTER_MAX_MEMBERS + 1`，明确包含 Head。
- 公共静态约束要求该容量不超过 64；因此 `uint64_t` logical bitmap 可覆盖产品允许的最大 32 个 remote member 加 1 个 Head。旧 v3 的 32-bit takeover bitmap 没有被改写或误称为新 quorum。
- build 过程无堆分配：先写局部 candidate、插入排序、拒绝 0/broadcast/重复 ID、写入 canonical hash，最后一次性赋值 output；失败时 output 不写回。
- valid 检查同时锁定 count、严格升序、unused zero tail 与 hash；contains、quorum、bitmap 不接受 malformed set。
- `active_voter_set` 只新增为独立存储，尚无生产资格、Backup、takeover、certificate 或 Authority consumer；M07/M10 才拥有这些语义。

## 定向验证

```text
cmake --build build_c06_full --parallel                         PASS
ctest --test-dir build_c06_full -R "ucn_cluster_membership_model_tests|ucn_tests" --output-on-failure
  ucn_tests                              PASS
  ucn_cluster_membership_model_tests     PASS
```

模型覆盖：排序、确定性 `0x63C30465` hash、contains、4-member quorum、最大容量 bitmap 的最高位、重复/NULL/overflow 输入无写回、canonical tail/排序/hash 篡改拒绝。

## 隔离与限制核对

- `src/extended/ucn_cluster.c` 中 `ucn_cluster_wire_v4` / `UCN_CLUSTER_WIRE_V4` 搜索结果为空。
- `src/adapters` 与 `include/ucn/adapters` 中无 Cluster/v4 引用。
- `git diff --check` 对本项文件无空白错误；仅报告仓库既有 CRLF 转换提示。

结论：06-03 的数据合同可供后续 M06 使用；它不等于 M10 quorum、证书验证、v4 RX/TX/FSM 接线或 M05 顶层放行。
