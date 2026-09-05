# 启用 Realtime 与 Cluster

## 1. Realtime

仅在确实需要同步时间或 Deadline 的产品启用 `UCN_FEATURE_REALTIME=ON`。提供认证固定 Path、
硬件/软件 timestamp、asymmetry 上界、振荡器误差和计时器分辨率；任何未知分量会阻止有效
LOCKED/Envelope。

为每个 Endpoint 设置 NONE、PREFERRED 或 REQUIRED 策略。REQUIRED 的消息需要 E2E、ACL、
正确 generation、组合 uncertainty 和两次 Deadline 检查。普通消息不需要 Envelope，也不会
因为启用模块自动增加开销。

## 2. Cluster

仅需分簇、Head/Backup、配置共识或跨簇管理时启用 `UCN_FEATURE_CLUSTER=ON`。产品必须提供
持久 Store、单调时钟、身份安全和选举/容量策略。创建 Cluster 后，成员只有通过认证且写入
committed Config 才能投票。

Config 变更走 Joint 双 quorum；Backup 要先同步覆盖并 ACK；Takeover/Handover/Recovery/
Rekey 都必须 durable 后才开放 Authority。应用不要直接把“role==HEAD”当成可发送，始终使用
当前 Authority view/preflight。

## 3. 二者独立

Cluster 不需要 Time Domain 才能选举，Realtime 也不需要 Cluster。产品可只启用其中一个，
或同时启用但保持独立 Storage、Owner 和失败域。一个模块 Fault 不应通过隐藏链接破坏另一个。

## 4. 当前限制

Host 模型通过不代表真实 Flash、无线分区或硬件 timestamp 已验证。没有完整硬件证据时，
Realtime/Cluster 只能作为开发功能，不能成为安全关键产品的唯一保护层。
