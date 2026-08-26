# CLV2-M13 13-11 Epoch Scope / Recovery Tombstone 自审报告

## 结论

`CLV2-13-11` 为 **CODE COMPLETE / SELF-AUDIT PASS（软件持久化合同）**。

## 实现

- Record writer 升级为 append-only schema v4，当前固定 388 B；v1/v2 280 B 与 v3 292 B 只读兼容。
- v4 显式保存 `STABLE/RECOVERY` Epoch scope、Recovery Epoch、parent Cluster/Term/Config、Recovery round/nonce、Cluster-ID round，以及 Recovery retired-ID Tombstone。
- REQUIRED 启动恢复时，Stable scope 才能恢复 `last_stable_*`；Recovery scope 只恢复 Recovery identity/serial，不把 Recovery Epoch 投影为 Stable 历史。
- Recovery identity admission 在重启后按 exact Epoch/round/nonce 与退休 ID 拒绝 replay。
- Rekey ref 同时绑定 allocation-history generation 和完整 fingerprint，避免 PREPARED 重启时换用另一份 history body。

## 回归

- Stable 与 Recovery Record v4 encode/decode。
- Recovery create → restart 不污染 Stable history。
- Recovery Tombstone 在重启后仍拒绝旧身份。
- 新槽撕裂/CRC 错误不可解释为合法新状态，旧完整槽仍可单独恢复。

## 边界

Record codec/双槽 fake 行为已验证；真实 Flash 原子写、断电和磨损仍归 M14 硬件门禁。
