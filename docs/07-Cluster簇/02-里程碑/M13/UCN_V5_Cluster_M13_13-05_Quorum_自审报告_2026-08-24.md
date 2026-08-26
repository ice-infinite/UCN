# CLV2-M13 13-05 Quorum 自审报告

## 结论

`CLV2-13-05` 为 **CODE COMPLETE / SELF-AUDIT PASS**，可进入 13-06。

## 冻结域

- transaction 保存 predecessor Stable Config 与按 canonical voter 顺序排列的 profile。
- profile 数量/Node ID 必须与 old Stable Config 完全相同；每项必须 format v4、Persistence+Rekey capability、有效 persistence generation。
- Joint Config、Config/Rekey PREPARED、无实时 M08 quorum 都在 Provider ID 分配前拒绝。

## 计票

- durable PREPARED exact reload 后才允许进入 ACK collection。
- Head 的 self-vote 只在该 durable boundary 计入。
- ACK outer source 必须是 frozen old Config voter；使用 canonical bitmap 计票。
- member nonce 保存逐 voter high-water；更低值 replay，exact duplicate 幂等不写。
- 达到 old Stable Config majority 后单向进入 `QUORUM`。

## 测试

- Config 中一个 v3 voter、一个缺 Rekey capability voter：begin 失败，Provider 未调用、round 未消耗。
- PREPARED 前无 collection；精确 PREPARED 后只含 self bit。
- 非 voter、跨 txid ACK 不写；一个合法远端 voter 与 Head 形成 2/3 quorum。
- exact duplicate ACK 对象不变；低 nonce replay 对象不变。
- MSVC M13 定向测试通过。
