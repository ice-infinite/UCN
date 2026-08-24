# CLV2-M13 13-03 New Cluster ID 自审报告

## 结论

`CLV2-13-03` 为 **CODE COMPLETE / SELF-AUDIT PASS**，可进入 13-04。

## 实现

- Rekey 必须配置产品 `make_cluster_id`；不使用默认 32-bit mix 伪装唯一性。
- Provider request 精确携带 `purpose=REKEY`、local Head、parent Cluster/Term/Config、durable boot incarnation 和 checked object round。
- Provider 成功后构造完整 successor：新 Cluster ID、Term 1、原 Head、Stable Config ID 1、原 voter set。
- transaction 保存 predecessor/successor Config ref 与 M04 `rekey_ref`；`next_incarnation` 严格前进。
- candidate 完整验证后才提交 `cluster_id_round` 和 transaction。

## 对抗测试

- Provider 返回 0、广播或 parent ID：`UCN_ERR_CONFIG`，round/transaction 不写。
- Provider 缺失：拒绝，不退回 best-effort mix。
- `cluster_id_round == threshold`：`UCN_ERR_EXHAUSTED`，Provider 不产生安全 continuation。
- 正向路径验证 request 所有域、successor Term/Config 初值、round 消耗和 durable Rekey ref。

## 限制

本项只证明“只能由产品 Provider 分配”。后续 13-12 已增加持久化 allocation history、固定碰撞/满历史门禁和 durable-before-PREPARE；真实产品 Flash 与跨设备唯一分配仍归 M14 验证。
