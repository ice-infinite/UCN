# CLV2-M13 13-04 Wire 自审报告

## 结论

`CLV2-13-04` 为 **CODE COMPLETE / SELF-AUDIT PASS**，可进入 13-05。

## 字段绑定

- Type 30 PREPARE：header=predecessor Epoch；P0/P1=successor Cluster/Term；P2=txid；P3/P4=old/new Config ID；P5=nonce。
- Type 31 ACK：header=predecessor Epoch；P0/P1/P2/P3 与 transaction 精确一致；P4=已准入 profile 的 persistence generation；P5=member nonce。
- Type 32 COMMIT：与 PREPARE 同 identity，但只有 durable active/max=successor、committed Config/Rekey、terminal txid 和 old→new Tombstone 全部精确匹配时才能构造。

## Admission

- ACK 的 outer source 必须与 profile Node ID 相同，且不能冒充旧 Head。
- profile 必须为 format v4，且同时具备 Persistence/Rekey capability。
- frame 必须先通过 RFC4 raw structural gate；旧 txid、旧/foreign Epoch、新 ID/Config 或 persistence generation 错配均作为 replay 拒绝。
- 失败不写 typed ACK 或 output frame。

## 验证与边界

- 正向 PREPARE/ACK/COMMIT raw fields 和 structural validity 已逐字段断言。
- 错 txid、v3 profile、未 durable COMMIT 均覆盖 no-write。
- MSVC M13 定向测试通过。
- 本项没有启用 `UCN_CLUSTER_WIRE_V4_ENCODER_ENABLED`，没有调用 production RX/TX/FSM；它只消费/生成 decoded raw value。
