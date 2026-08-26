# UCN V5 Cluster M07-08 Durable Joint 自审报告

日期：2026-08-23  
范围：`CLV2-07-08`；受控实验 runtime 的 Joint identity install。

## 进入条件

`enter_joint()` 必须同时验证：

1. caller 当前 active Config 是 transaction 的 exact Stable C_old；
2. transaction 的 old/new bitmap 都达到 quorum；
3. M04 Record v2 中存在 `CONFIG_PREPARED`，transaction id 一致，staging Config digest 与 C_new 完全一致。

条件全部满足才将 caller-owned runtime 改为 exact Joint C_new，并将 transaction 本地阶段标为 `JOINT_DURABLE`。任一条件缺失，runtime 保持逐字节不变。

## 反例与验证

- 仅 Head self ACK、未达 dual quorum：拒绝，无写回。
- 已完成 C_new Prepare、再补齐 old/new ACK：可进入 Joint。
- Joint runtime 的 active_config 必须与 proposed_config 字段逐项一致。
- Windows GCC Full/Lite/Nano 与 WSL Full ASan/UBSan 均为 `11/11` CTest 通过。

## 范围限制

当前 M04 Record 可持久化 committed C_old ref、staging C_new ref 与 txid；这足以在本项锁住 Joint identity pair，却还没有存放完整 Config body 的双槽 Provider。完整 body 的 Crash/restart/torn-write 实现和验证仍归 `CLV2-07-11`，在其之前不能宣称 Backup 已可恢复全部 Joint membership body。

`CLV2-07-08`：**CODE COMPLETE / SELF-AUDIT PASS**。没有 Head Authority、Takeover、v4 production RX/TX/FSM 或 Adapter 接线；M05 顶层 `AUDIT HOLD` 不变。
