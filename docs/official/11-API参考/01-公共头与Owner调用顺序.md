# 公共头与 Owner 调用顺序

## 1. 总入口

应用通常只包含 `<ucn/ucn.h>`。它总是引入 Config、Identity、Bootstrap、Wire、Message、Owner、
Security、Capability、Route、QoS 和 Transfer；Realtime、Cluster、Adapter 只在对应 Feature
启用时引入。需要最小依赖时可以直接包含 `<ucn/v6/ucn_v6_*.h>`。

所有返回 `ucn_v6_result_t` 的函数都必须检查。一般语义是：参数/枚举错误、状态不允许、容量
不足、认证失败、过期或 Provider 错误均显式返回；失败时输出和持久/运行状态按接口合同保持
不变。不要把任意非零都当成可重试，尤其认证、代际和 durable fault。

## 2. 基础 API 组

- Config：`ucn_v6_compiled_manifest()`、`ucn_v6_manifest_validate_exact()`、
  `ucn_v6_storage_validate()`；
- Identity：Principal/Binding 校验、serial checked-next、lease deadline、callback gate、
  Authority allocate/retire；
- Wire：`encoded_size()`、`encode()`、`decode()`、`write_canonical_aad()`、`crc32c()`；
- Message：`message_validate()`、Operation ID allocator 和 durable Journal 状态转换；
- Owner：`init_in_place()`、`post()`、`run()`、`copy_view()`。

## 3. 数据面 Owner

Capability Owner ingest 已认证 HELLO/ADVERTISE，再 derive/install Path。Route Owner 创建
Candidate、加入 Path、记录 Probe、冻结 Activate、记录发送并用 ACK 原子提交。Metric Owner
接收测量并生成 Cost；QoS Owner enqueue/select/complete/retire。Transfer Owner 负责 begin、
fragment、SACK、Credit、完成与失效。

这些 API 不是随意组合：例如 Route ACK 之前必须已有 matching activation send，QoS select
之后必须 complete，Transfer fragment submit 只能对应此前返回的 fragment，Session 变化后
旧 token 全部失效。

## 4. Security 与入网

Bootstrap Owner 只维护有限 pending 和流程顺序；Security Owner 写认证 transcript、commit
join、reauth、设置 ACL/Group、轮换/吊销 Key，并 protect/open frame。密码 Provider、持久化和
Identity Authority 必须由产品接入，测试 fake 不能用于生产。

## 5. 可选 Owner

Realtime Owner 设置 Endpoint Policy、绑定 Domain、ingest sample、读取 Clock、准备 Payload，
并在 receive/execution 两个时点 admit。Cluster Owner 提供 create/member/backup/joint/takeover/
handover/recovery/rekey/directory/tunnel/step；每个 Authority 入口前仍需当前 preflight。

Adapter Owner 注册 Link、发布/退休 RX、排队/服务/取消 TX、发布/退休 completion 和 reopen。
FreeRTOS Port 把通知与 Owner run 连接起来，不改变协议状态机语义。

## 6. 如何查参数

本文是调用顺序导航，不复制可能变化的完整声明。精确类型、参数 const 性、Storage 宏和返回
条件以 `include/ucn/v6/` 当前头文件为准；源码阅读顺序见
[源码阅读指南](../../源码阅读指南/README.md)。
