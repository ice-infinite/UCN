# Endpoint ACL、Service 命令保护与管理诊断

> 文档级别：`NORMATIVE`
> 实现状态：接口与门禁 `CURRENT`；产品规则需配置
> 最近核对：`a093862`，2026-08-25

## Endpoint ACL

产品可为 Endpoint 设置独立安全 Policy，并在 Provider `authorize_tx/rx` 中根据 Source/Destination、Ingress Link、Session、Endpoint 和保护状态决定。

Endpoint 号只表示业务类型，不是权限。

## Service Q0

远端高风险 Q0 Binding 可要求 Validator。若配置要求 Validator 但产品没有安装，Bridge handler 初始化/投递失败关闭。

推荐校验：Source 身份、角色、Command ID、issued time、有效期、参数范围、当前设备状态和幂等历史。

## Result

远端任务用显式 Result Endpoint 回报 Accepted/Succeeded/Rejected/Failed/Expired。攻击者不能仅通过伪造 Link Queue ACK 让源节点认为舵机动作已完成。

## 管理诊断

Node Snapshot、Path Trace、Path Control、Policy Diagnostic 都有独立 authorizer。Node Snapshot 默认拒绝远端请求，防止任意节点低成本枚举网络状态。

管理节点本身也必须认证；“固定 Node ID 是管理端”若没有密码身份，仍可被伪造。

## Cluster/Federation

Cluster Authority、Config、Takeover、Handover、Rekey 和 Federation Directory/Tunnel 具有更强控制效果，必须同时满足保护帧、来源角色、Epoch/Config/Lease/Persistence 等门禁。普通 Endpoint ACL 不能替代这些状态机证明。

## ACL 决策需要哪些上下文

一个可用的授权决策不应只检查 `source == 1`。Provider/Validator 可综合：

- 认证设备身份与当前 Node ID；
- Source Session/Key generation；
- Destination 与 Endpoint；
- Traffic Class 和保护状态；
- Ingress Link/Bearer 安全级别；
- 当前产品模式/角色/租约；
- Payload Guard、Command ID 和参数；
- 是否本机任务或远端来源。

其中网络层负责可稳定提供的字段，业务层负责设备状态和 Payload 语义。

## Endpoint 最小权限示例

| Endpoint | 允许来源 | 要求 |
| --- | --- | --- |
| IMU telemetry | 已认证监控节点 | 可 Q1，允许读取 |
| Servo command | 主控节点/安全 CAN Path | E2E、Q0、Validator、Command ID、短有效期 |
| Parameter read | 管理节点 | 认证、限频 |
| Parameter write | 管理节点 | 认证、签名/版本、持久结果 |
| Node snapshot | 诊断节点 | 默认拒绝、显式 authorizer、分页限频 |

“管理 Node ID=1”只能作为地址筛选，必须再绑定认证身份。

## Service Q0 的两次检查

Bridge Validator 在进入 Inbox 前检查远端来源和 Guard；任务取出后还要检查当前机械/业务状态。两次检查不是重复：网络条件和设备执行条件属于不同时间点。

Validator 未安装而 Binding 要求它时，Handler 安装/投递 fail-closed；不能为了“先跑起来”自动允许。

## 诊断为什么默认拒绝

Path Trace、Node Snapshot、Policy Diagnostic 会暴露拓扑、Node ID、Link、Cost、Policy 和资源状态，也会消耗固定事务槽与控制带宽。默认远端拒绝可以防止任意普通节点扫描网络或制造诊断 DoS。

Authorizer 应限制 Source/Session、查询类型、分页索引、频率和当前维护模式。结果也可能含敏感元数据，需要保护帧/安全返回路径。

## Path Control 与普通业务 ACL

能够向一个 Endpoint 发数据不意味着能安装 Path。Path install/revoke 改变中继行为，必须有独立管理 authorizer，并绑定 Owner Session/Path ID。Endpoint ACL 不能被复用为“所有管理操作都允许”。

## Cluster 权威控制

Cluster 消息除了身份认证，还需满足 Phase、Role、Epoch、Config、quorum、Lease、Persistence/Fence。一个合法 Head 的旧 Epoch 消息也必须拒绝。密码认证只证明“谁发的/没被改”，不证明状态机当前允许。

## 容量与拒绝

ACL/Validator/Replay/诊断表满时应拒绝新请求并保留旧有效安全状态。不能覆盖当前幂等记录后执行重复命令，也不能覆盖正在进行的管理事务。所有拒绝要有饱和统计，避免计数溢出回 0。

## 测试矩阵

- 正确/错误 Source、Session、Ingress Link；
- Plain/Protected 与 Endpoint Policy 组合；
- Guard 过期、ID 重复、ID 冲突 Payload；
- Validator 缺失和 Task Not Ready；
- Node Snapshot/Trace/Policy 默认拒绝和授权分页；
- 管理请求洪泛、表满和统计饱和；
- Cluster 旧 Epoch/错误 Role/无 durable promise；
- 重启后 ACL、Command ID 和持久状态恢复。
