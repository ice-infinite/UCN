# Federation、Locator、Directory 与 Tunnel

> 文档级别：`NORMATIVE`
> 实现状态：`PARTIAL（Current 与默认关闭实验层级见正文）`
> 适用版本：UCN 5.0.0 / Core Wire v5
> 事实源：Cluster 公共头、生产源码、独立 Archive、CMake 与测试
> 最近核对：`codex/v5-adaptive-wire@a093862`，2026-08-25
> 硬件状态：部分 ESP32-S3 实测；v4/掉电/完整多 Bearer 未发布验收

Federation 是独立可选组件，用于跨 Cluster 发布 Locator、维护 Directory，并通过小消息 Tunnel 连接不同簇。它位于 Cluster 之上，不修改 Core 的基本寻路机制。

## Locator 与 Directory

Locator 绑定节点/服务身份、Cluster Epoch、Head、路径提示和有效期。Directory 使用固定容量、单调更新和过期清理；同一 identity 的版本关系必须按 Epoch domain 判断，foreign Cluster 不比较 Term。

## Tunnel

Tunnel 外层负责跨簇转送，内层仍保持端到端身份和安全语义。中间 Federation 节点不得把解包能力等同于业务授权；超过小消息上限的数据应交给 Transfer/Service 设计，而非无限扩大控制帧。

## Authority 前置检查

Head 发布 Locator、Handover 或 Directory 权威信息前必须执行 current-time Authority preflight。租约刚过期但尚未运行常规 `step()` 时也不能使用旧缓存写 Directory。

## 当前限制

Federation 是可选 archive，生产 v4 Cluster Authority 未放行，因此不能声称跨簇自动漫游、全网目录或大规模 Tunnel 已达到产品成熟度。部署必须对目录容量、过期时间、授权和路由泄漏做独立设计。

## 为什么 Core Route 不等于 Federation Directory

Core Route 回答当前节点去某 Node 的下一跳；Directory 回答某身份/服务当前属于哪个 Cluster/Head，以及跨簇入口提示。它们生命周期和可信来源不同，不能把 Directory 项直接写成无验证 Core Route。

## Locator 身份和更新

Locator 需要绑定发布者 Authority、目标 identity、Cluster Epoch/Head、路径提示、版本/nonce 和 TTL。更新规则先比较 identity domain：同 Cluster 才看 term/nonce 单调，foreign 进入替换/冲突策略。旧 locator 到期清理，不成为永久全网表。

## Directory 固定容量

Directory 满时采用冻结的拒绝/回收规则；不能让未授权新条目淘汰仍有效的关键 Locator。按需查询/缓存比让每个 MCU保存全网所有节点更符合 MCU-first。

## Tunnel 数据流

```text
Cluster A member
→ A Head/Federation gateway封装外层Tunnel
→ Core Route 到 Cluster B gateway
→ 验证外层Authority/Locator
→ 解封或透明转发内层UCN身份
→ B内Core Route到目标
```

内层 E2E 保护可保持，使 gateway 不读业务 Payload；若 gateway 改 Endpoint/解密，则成为安全端点。

## 小消息上限

Federation control/tunnel 当前不应无限扩大 Payload。大消息需要明确与 Transfer 集成：Class/Fragment身份、跨簇 MTU、ACK 返回、重路由和 Authority 切换。该闭环尚未生产放行，所以不能把普通 T8K 直接塞进 Locator/Tunnel 控制帧。

## Authority 与缓存时序

Head Lease 在 t=90 到期、t=91 先调用 Federation publish 时，也必须 preflight 撤权；不能等待 cluster_step 后才刷新。Config/Epoch 切换时旧 Locator 发布 Fence，Directory 已有旧条目按 version/TTL 失效。

## 隐私与路由泄漏

Directory 暴露哪些 Node/Service 属于哪个簇，可能是敏感拓扑。管理授权、最小字段、保护帧、查询限频和日志脱敏必须设计。Tunnel 外层地址/长度仍可被观察。

## 验证清单

- [ ] Locator 同域单调/foreign 规则；
- [ ] current-time Authority preflight 覆盖所有发布入口；
- [ ] Directory 满载、到期和恶意插入；
- [ ] Tunnel 内外身份、安全和 MTU；
- [ ] Head/Config 切换后旧 Locator 不继续写；
- [ ] 大 Transfer 未接线时明确拒绝；
- [ ] 默认 archive/产品成熟度边界保持。
