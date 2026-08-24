# UCN Cluster Config / Joint Config 当前合同

## 状态链

```text
Stable C_old
  -> CONFIG_PREPARE(txid, C_new)
  -> durable PREPARED
  -> durable JOINT(C_old, C_new)
  -> old quorum AND new quorum AND exact Backup gate
  -> durable CONFIG_COMMIT
  -> Stable C_new
```

禁止 `PREPARED + quorum` 直接 Commit。Commit 前必须精确绑定 txid、C_new、Backup ID/ACK source；Abort 同时绑定 txid 与 staging C_new。

## Voter 与成员

- CommittedVoterSet 与 provisional Member 分离；
- Joint quorum 对 old/new voter set分别计算 majority；
- v3 provisional member 不得成为 Voter、Backup 或 Authority；
- Config ID、generation、txid 均使用 checked-serial/no-wrap。

## 当前边界

M07 owner 与测试 archive 完成受限软件验证，但 production v4 Config Type 20..25 尚未接入默认 RX/TX/FSM。M05 `AUDIT HOLD` 未解除。
