# CLV2-M13 13-12 Allocation History 自审报告

## 结论

`CLV2-13-12` 为 **CODE COMPLETE / SELF-AUDIT PASS（软件 Provider 合同）**。

## 实现

- 默认 32-bit mix 不再被当作 Rekey 唯一性证明；Rekey 必须使用产品 `make_cluster_id`。
- 新增 8-entry bounded collision history 与 280 B CRC record；完整 allocation identity 为 `{purpose, local, parent Epoch/Config, recovery_round, incarnation, round}`。
- ID 与 identity 建立一一映射：同 ID/同 identity 幂等；同 ID/不同 identity 或同 identity/不同 ID 均 `UCN_ERR_REPLAY`。
- history encode 只接受 canonical 状态；decode 检查 magic/version/generation/count/CRC/零尾和逐 entry 合法性。
- Rekey begin 只进入 `ID_HISTORY_DURABLE_REQUIRED`。只有候选 history 按 checked generation 写入并精确回读，且完整 fingerprint 与 M04 Record v4 Rekey ref 一致，才进入 `PREPARE_REQUIRED`。
- PREPARED 重启恢复必须重新加载同 generation history，并匹配 successor ID、父 Epoch/Config、incarnation 与 Record fingerprint。

## 回归

- 固定 ID collision 消耗当前 allocation round，下一 round/新候选才可继续。
- exact duplicate、反向冲突、CRC 错、非法手工 history、空记录、重启加载、满 8-entry 后 fail-closed。
- 未 durable/错 generation/错完整 body 时 PREPARE 为零 Provider I/O。

## 边界

8-entry 满载选择安全停机，不做淘汰。产品需用自己的原子存储保存 history record；本项验证 codec、generation、reload/fingerprint 门禁，不把 Host 内存 fake 当作真实 Flash 证明。容量调优和硬件掉电属于 M14。
