# CLV2-M13 13-01 Phase / State 自审报告

## 结论

`CLV2-13-01` 为 **CODE COMPLETE / SELF-AUDIT PASS**，可进入 13-02。

## 实现核对

- `UCN_CLUSTER_PHASE_HEAD_REKEYING=21` 仅作为 M13 事务 Phase；Current FSM direct matrix 没有任何入边或出边。
- `ucn_cluster_rekey_experimental` 默认 OFF，头文件带编译 guard；默认 `ucn_cluster` 不包含 Rekey object。
- `rekey_tx` 是 caller-owned bounded 对象，不嵌入 `ucn_cluster_t`，没有动态内存，也没有运行期 reset API。
- `begin()` 使用真实 `ucn_cluster_authority_runtime_t` 做当前时钟 preflight，并要求 `HEAD_STABLE + authority_active + Stable Config + quorum`。
- durable predecessor 必须是当前 writer schema、active=max=运行时 Epoch、committed Config digest 精确匹配，且无 Config/Rekey PREPARED、committed Rekey 或 Tombstone。
- transaction id、nonce 必须是非零且不超过 rotation threshold 的 serial。

## 对抗测试

- lease 过期后直接 begin：preflight 当场撤权，事务保持全零。
- `HEAD_RECONFIGURING`、legacy schema、Config PREPARED：均 `UCN_ERR_STATE`，事务逐字节不写。
- txid 为 0、超过 threshold、同一对象二次 begin：均在写入前拒绝。
- 正向 begin 冻结 predecessor Epoch/Config；M13 全体收口后首先停在 `ID_HISTORY_DURABLE_REQUIRED`，只有 13-12 的 exact history reload 证明才进入 `PREPARE_REQUIRED`，Phase 始终为 `HEAD_REKEYING`。

## 验证

- MSVC Debug：`ucn_tests + ucn_cluster_rekey_tests = 2/2`。
- M13 OFF：默认 `ucn_cluster` 独立构建通过。
- 生产 `ucn_cluster.c`、codec、Adapter 无 M13 API/Phase 接线。
- `git diff --check` 无空白错误，仅既有 CRLF 提示。

## 限制

本报告保留 13-01 的分项边界；successor ID、history durable gate、PREPARED、Wire/quorum/Commit 后续已由 13-03..12 补齐。它不解除 M05 `AUDIT HOLD`。
