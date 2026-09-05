# Cluster 角色、Authority 与 Persistence 排查

> 文档级别：`GUIDE`
> 实现状态：`CURRENT（诊断方法）`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：状态视图、统计 API、测试与现有实测记录
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：部分实测；未测项不得推断

按顺序读取 Role、Phase、Epoch、Config/Joint、quorum/lease、Authority、Fence、Backup coverage、persistence pending/fault 和 operation journal。

“Role=HEAD 但不能发送”通常是 Authority/Fence 门禁，不应手动强制角色。初始化失败先检查 persistence mode、Provider API/record size、load result、CRC/schema 和 legacy migration。

## 必须按层解释状态

```text
Role/Phase：节点正在扮演/推进什么
Epoch：属于哪个Cluster/Term/Head身份域
Config：哪些voter有权形成quorum
Lease/Quorum：当前证据是否新鲜且达到多数
Authority：现在是否允许权威副作用
Fence：为什么被撤权/阻断
Persistence：承诺是否durable
```

前一层看起来正常，不代表后一层成立。

## Head不能发送

读取view：`authority_active`、authority phase/fence reason、persistence pending/fault、Active/Max Epoch。然后看Authority runtime的old/new quorum、voter lease age和Owner budget。

常见原因：刚成为Head但Config未安装；quorum丢失进入Grace；Lease到期且RX-first preflight撤权；Config切入Joint但新集合无多数；Provider仍PENDING；观察到higher term/authority；已Stepdown/Rekey retired。

正确修复是恢复证据或走Recovery/Handover，不是写role字段。

## 初始化失败

```text
persistence_mode是REQUIRED?
  → provider API version/record_size/load/submit完整?
  → load返回值不是全零伪READY?
  → record magic/schema/CRC/generation合法?
  → v1/v2/v3迁移是否匹配?
  → REPLAY_INCARNATION/legacy PREPARED abort能提交?
```

Provider回调重入会返回STATE；Flash I/O失败会清对象/fail-closed。不要把生产配置改成VOLATILE_TEST来绕过。

## Persistence pending/fault

PENDING时所有Cluster promise/RX副作用/推进受Fence，Owner调用poll。COMMITTED后必须reload+journal验证。若durable Vote后的ACK发送`NO_SPACE/LINK_DOWN`，应进入发送重试/传输错误，而不是persistence fault；查看retry pending即可区分。

## Backup问题

Backup READY需要Snapshot完整、sequence连续、Epoch/Config一致和protected voter coverage。明确SUSPECT才有grace；REMOVED或缺条目使本轮assignment永久ineligible。新ADMITTED不能复活旧assignment，需Head重新assign/snapshot。

## Recovery/Rekey

Recovery先观察稳定Authority；lineage/backoff/round决定何时创建新Cluster。Rekey接近serial阈值时建立successor identity和Tombstone；旧Cluster ID不得通过普通create重新使用。回滚旧Storage可能破坏该历史。

## 采集快照

同时保存Cluster view、member summaries/capacity、stats、Persistence record header/generation（不含密钥）、Config/Authority/Backup实验对象和最近控制消息。多节点问题按同一时间线对齐，才能判断是否曾有双Authority。

## 当前版本边界

默认v3与实验v4/M07～M13不可混为一体。若日志来自实验target，必须记录CMake开关；生产v3 Backup/Takeover帧返回ACCESS是安全围栏，不是待修的“无法接管”。
