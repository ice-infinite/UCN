# Cluster API、Storage 与 Record 迁移

> 文档级别：`NORMATIVE`
> 实现状态：`CURRENT（规则）；RELEASE NO-GO`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：版本宏、Wire/API/Storage 合同、CMake 与发布门禁
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：完整发布实机门禁未完成

Cluster API v2 使用版本化 storage/配置合同。Persistence 当前 writer 为 Record v4/388 B，并兼容读取既有 v1/v2 280 B、v3 292 B。

读取兼容不等于可以安全回滚 writer。legacy PREPARED 有专用迁移；未来开放新 PREPARED 前必须区分来源。升级包应声明双槽尺寸、schema、迁移是否原子及何时需要擦除。

## 三类Storage

1. `ucn_cluster_t`运行storage：opaque，随API/容量变化，不落Flash；
2. Persistence Record：固定canonical字节，跨重启恢复；
3. 产品Provider双槽/介质元数据：commit marker、erase block、坏块等。

不能把运行对象`memcpy`进Flash替代Record。

## 当前Record口径

- writer：schema v4，388 B；
- reader：v1/v2 280 B、v3 292 B、v4；
-内容包括Active/Max Epoch、VoteId、Config/Rekey事务、boot incarnation、Tombstone、operation journal等；
- encode canonical清零无效字段，decode严格验证ID/serial/CRC。

精确常量以`ucn_cluster_persist.h`和codec为准。

## 升级迁移

```text
双槽选择最新有效旧Record
  → strict decode旧schema
  → 映射canonical state
  → 处理允许的legacy PREPARED abort
  → 增加boot incarnation
  → encode v4到另一槽
  → reload+journal验证
  → 才初始化Runtime
```

任一步失败都不发送Cluster promise。

## PREPARED风险

早期v1允许合法PREPARED但当时M07/M13 continuation未开放，启动可能永久失败，因此引入受限legacy abort。未来新事务真正使用PREPARED前，Record必须有schema/来源标记，防止把新事务误当legacy清除；这是M07/M13前置门禁。

## Tombstone与回滚

Rekey后Tombstone记录retired A→successor B。普通新簇创建不得删除或复用A。旧固件若不理解Tombstone，回滚可能恢复已退休身份；必须拒绝回滚、向下迁移history，或受控擦除并创建全新identity。

## 双槽尺寸

Provider声明record size/API version并为每槽预留至少当前writer长度。升级到更大record时先确认分区空间；不能让新writer越界覆盖相邻配置。旧固件看到大schema必须fail-closed。

## 验证

保存各历史版本真实golden Record，执行decode→migrate→restart；每个erase/program/marker断电；向下回滚或明确拒绝；损坏CRC/脏缺省/serial回退；多轮generation和磨损。
