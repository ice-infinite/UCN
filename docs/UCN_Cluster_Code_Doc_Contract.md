# UCN Cluster 代码—文档冻结契约

本文件是 `tools/check_cluster_docs_contract.py` 的机器核对输入。表项必须与公共头枚举严格一致。

## Phase

| 值 | 名称 |
|---:|---|
| 0 | DISABLED |
| 1 | DETACHED_OBSERVE |
| 2 | ELECTION |
| 3 | JOIN_PENDING |
| 4 | MEMBER_ACTIVE |
| 5 | MEMBER_TAKEOVER_GRACE |
| 6 | HEAD_NO_BACKUP |
| 7 | HEAD_BACKUP_ASSIGNING |
| 8 | HEAD_BACKUP_SYNCING |
| 9 | HEAD_STABLE |
| 10 | BACKUP_SYNCING |
| 11 | BACKUP_READY |
| 12 | BACKUP_TAKEOVER |
| 13 | STEPPING_DOWN |
| 14 | RECOVERY_OBSERVE |
| 15 | RECOVERY_ELECTION |
| 16 | RECOVERY_HEAD |
| 17 | TERM_CONFLICT_WAIT |
| 18 | HEAD_RECONFIGURING |
| 19 | HEAD_QUORUM_GRACE |
| 20 | HEAD_FENCED |
| 21 | HEAD_REKEYING |

## Wire v4 Type

| 值 | 名称 |
|---:|---|
| 1 | ADVERTISE |
| 2 | JOIN_REQUEST |
| 3 | JOIN_ACCEPT |
| 4 | JOIN_REJECT |
| 5 | KEEPALIVE |
| 6 | LEAVE |
| 7 | HEAD_DECLARE |
| 8 | HEAD_TAKEOVER |
| 9 | HEAD_STEPDOWN |
| 10 | BACKUP_ASSIGN |
| 11 | BACKUP_READY |
| 12 | BACKUP_MEMBER_SYNC |
| 13 | PRIMARY_HEARTBEAT |
| 14 | TAKEOVER_PREPARE |
| 15 | TAKEOVER_ACK |
| 16 | RECOVERY_DECLARE |
| 17 | RECOVERY_ACK |
| 18 | BACKUP_RESYNC_REQ |
| 19 | BACKUP_REJECT |
| 20 | CONFIG_BEGIN |
| 21 | CONFIG_MEMBER |
| 22 | CONFIG_PREPARE |
| 23 | CONFIG_ACK |
| 24 | CONFIG_COMMIT |
| 25 | CONFIG_ABORT |
| 26 | HANDOVER_PREPARE |
| 27 | HANDOVER_READY |
| 28 | HANDOVER_COMMIT |
| 29 | HEAD_WITHDRAW |
| 30 | REKEY_PREPARE |
| 31 | REKEY_ACK |
| 32 | REKEY_COMMIT |
| 33 | TAKEOVER_CERTIFICATE |

## 版本常量

| 项 | 值 |
|---|---:|
| Cluster API | 2 |
| Storage layout | 2 |
| production Cluster format | 3 |
| recommended Cluster format | 4 |
| Wire v3 bytes | 32 |
| Wire v4 bytes | 40 |
| Persistence writer schema | 4 |
| Persistence record bytes | 388 |

## 状态边界

- production Wire v4 RX/TX/FSM/Authority：`AUDIT HOLD`；
- M14-03、M14-07：`PARTIAL / BLOCKED BY M05`；
- M14-08：`BLOCKED / REAL HARDWARE REQUIRED`；
- M14-12：在上述阻断解除前不得建立 tag。
