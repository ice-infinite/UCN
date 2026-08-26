# Federation API

> 文档级别：`REFERENCE`
> 实现状态：`CURRENT`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：include/ucn 公共头、链接目标与公共 API 测试
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：API 合同不等同于各平台实机驱动完成

Federation API 提供 Locator 发布/查询、Directory 固定表、跨簇 Tunnel、过期维护和可选 Security Provider。调用者提供静态 storage、时钟和发送回调。

发布 Authority 信息前必须传入或触发当前时间 preflight；Directory 更新按完整 identity/Epoch 单调规则执行。查询完成、超时和容量不足分别报告。

Federation 不负责大消息分片，也不替代 Core route。生产 Cluster v4 Authority 未放行时只能按受限 archive 使用。

## 作用与数据对象

Federation 解决“远端 Node 属于哪个 Cluster、该 Cluster 当前 Head 是谁”，不是逐跳链路寻路。主要固定表包括本地 Locator、Directory records、Locator cache、Next-Cluster、pending query、seen transaction 和 ClusterHeadLease。

Locator 应绑定 Node、Cluster ID、Head、Term、Config/Nonce 和 lease；同一 identity 只能按 serial/nonce 单调更新。缓存过期后必须重新查询，不能继续用旧 Head 转发权威数据。

## 初始化配置

```c
ucn_cluster_federation_config_t cfg = {0};
cfg.local_node_id = local_id;
cfg.cluster = cluster;
cfg.enabled = true;
cfg.directory_authority = false;
cfg.require_protected_control = true;
cfg.enable_tunnel = true;
cfg.inner_security_mode = UCN_CLUSTER_FED_INNER_SECURITY_REQUIRED;
cfg.default_hop_limit = 8U;
cfg.directory_authorities = directory_ids;
cfg.directory_authority_count = directory_count;
cfg.now_ms = product_now_ms;
cfg.send = product_send_endpoint;
cfg.seal_inner = product_seal_inner;
cfg.open_inner = product_open_inner;
cfg.deliver = product_deliver;
```

Directory Authority 还必须配置 `authorize_head`；受保护 Handover 需要 authorize/build proof。Tunnel 默认 inner security REQUIRED，只有明确的实验诊断配置才能选择 protected-outer-only。

## Locator 发布与查询

Head step 根据 Cluster view/Authority 发布本机和成员 Locator，刷新 lease；退位/Term 变化先 Withdraw。Directory 验证 protected outer、Head authorization 和 identity 后写固定 record。

查询流程：

```text
find_locator(target)
  ├─命中未过期 cache → 返回
  └─miss → query_locator()
        → 依次询问固定 Directory Authorities
        → receive Reply/Error
        → 写 cache 或 timeout
```

`query_locator()` 成功只表示已有 cache 或查询被接受；调用者之后通过 `find_locator()` 观察完成。

## 单帧 Tunnel

`ucn_cluster_federation_send()` 是显式 API，不劫持普通 `ucn_node_send_endpoint()`：

```text
源 Member → 本簇 Head
  → 查目标 Locator
  → Head-to-Head Tunnel Data
  → 目标 Head → 最终 Member Deliver
```

内层 AAD 绑定 transaction、origin/final Node、origin/destination Cluster、Endpoint 和 traffic class。中间 Head 不应改变这些字段。Seen table 去重，hop limit 防环；错误按原事务返回上游。

当前只支持其固定 payload 上限内的小消息；跨簇 T32～T8K Transfer 尚不能从 API 名称推断已完成。

## Handover 与 Authority preflight

新 Head 发布 ClusterHeadLease Handover，Directory 通过 proof/backup generation/term 验证后原子替换 `cluster_id→head`。`federation_step()` 和显式 publish 入口必须先以当前时间刷新 Cluster Authority；Lease/quorum 已过期时不得继续写 Directory。

## 失败处理

- Directory list 空/全不可达：query timeout/error；
- cache/record/pending 满：`NO_SPACE`，不覆盖权威新记录；
- stale Term/Nonce、错误 source、未保护：拒绝并计数；
- tunnel 找不到 Locator：启动有界 query，源稍后用新 transaction 重试；
- Head/Route 断开：返回 tunnel error，不让普通中继学习所有远端 member route。

## 线程与资源

所有 API 在唯一 Federation/Protocol Owner 上调用；callbacks 不得递归进入对象。诊断读取 stats 和 const find 结果，后续 step 可能使指针过期，应用不要长期保存内部 entry 指针。
