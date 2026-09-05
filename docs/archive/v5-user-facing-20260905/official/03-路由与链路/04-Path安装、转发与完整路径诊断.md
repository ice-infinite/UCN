# Path 安装、转发与完整路径诊断

> 文档级别：`NORMATIVE`
> 实现状态：Full `CURRENT`；非 Full API 失败关闭
> 事实源：`ucn_path.h`、Path Control/Trace tests
> 最近核对：`a093862`，2026-08-25

## 显式 Path

Path 是受授权安装的端到端转发身份。每个中继保存固定表项：Wire Path ID、目标、下一跳/egress Link、能力/MTU 等必要约束。

业务可通过 Policy 的本地 Path handle 引用它。Local handle 和 Wire Path ID 分离，产品可以替换一条 Path 而不修改所有业务规则。

## 安装与撤销

Path Control 需要产品 authorizer。处理顺序遵守：验证来源/权限/字段→确认容量与能力→提交表项→回送结果。中途失败不能留下半安装状态。

v5 基础 `PATH_INSTALL` 保持兼容格式；扩展能力信息只有调用 capability-aware API 且目标明确支持时才发送。未知扩展长度拒绝。

Path revoke 会删除相关转发表并通知 Policy 刷新/降级。

## 完整路径查询

Path Trace 是按需诊断控制帧。请求从当前节点沿实际下一跳传播，每跳追加受限信息并由目标/终止条件回送结果。

它用于调试“经过哪些节点”，不会随每个普通业务帧携带完整节点列表，也不要求所有中间节点保存全路径。

## 安全

E2E AAD 在 Path 扩展存在时绑定 Path ID；中继可修改 Hop/Route Epoch 等允许的逐跳字段，但不能在不破坏认证的情况下把受保护业务换到另一个 Path ID。

## 限制

Path/Policy 只在 Full 编译。表项固定；诊断结果会按 Payload 能力截断并给出状态，而不是越界写入。

## Path 身份由哪些部分构成

一个 Path 不能只靠“目标 Node + 下一跳”识别。逐跳安装至少需要区分：

- Path Owner 及其 Session；
- Wire Path ID；
- 最终 Destination；
- 本跳 Next Hop 与 Egress Link；
- 剩余 Hop/Lease；
- 能力感知格式中的最大 Wire Profile、逻辑 MTU 等约束。

Owner Session 防止拥有者重启后旧 Path ID 被新会话误继承。Wire Path ID 是线上身份；本地 Policy handle 是应用侧稳定引用，两者分离后可以原子替换路径实现而不改变业务规则。

## 一条三跳 Path 如何安装

以 A→B→C→D 为例，控制面先确定每一跳的 next hop，再逐跳提交：

```text
A: path P, destination D, next B
B: path P, destination D, next C
C: path P, destination D, next D
```

每个接收节点在写表前依次验证控制来源、Owner/Session、Path ID、目标、Hop、Link、容量和 capability。若 C 无法满足 MTU，不能让 A/B 保留一条看似完整的 Path；控制事务要返回失败并撤销已建立的局部状态，或由上层执行明确回滚。

当前 Path Control 的精确事务边界以 API/测试为准。产品不能在外部直接写内部 `path_forward` 数组来跳过 authorizer。

## 转发时的决策

带 Path ID 的业务帧到达中继 B 后：

1. 验证帧、Network、Destination、Session/Sequence 和安全边界；
2. 根据 Owner/Session/Path ID/Destination 查找唯一表项；
3. 检查表项 Lease、剩余 Hop、Egress Link、Profile/MTU；
4. 只修改协议允许的逐跳字段；
5. 提交给该表项指定 Link；
6. 查不到、过期或能力不符时拒绝并产生相应 Path/RERR 处理。

中继不需要解密 E2E Payload，也不根据传感器类型重新选路。Path ID 被 AAD 绑定时，私自换成另一条 Path 会破坏端到端认证。

## 撤销与故障切换

撤销可能来自 Owner 主动 revoke、Lease 到期、Link Down、Neighbor 离线或能力变化。撤销后：

- 本地逐跳表项立即不可用于新发送；
- 相关 Policy handle 重新解析时必须看到 Path 不可用；
- `PINNED_STRICT` 返回失败；
- `PINNED_FAILOVER` 可切到已验证备 Path；
- 旧 Driver 中已经提交的 Frame 不能被撤回；
- 可靠消息由 Transfer/业务幂等恢复。

## Path Trace 的请求与返回

Trace 是一次有界诊断事务，而不是路由协议的常驻数据：

1. 请求方指定 Destination、最大记录数和回调；
2. 请求沿当前实际 Route/Path 前进；
3. 每跳只在容量允许时追加本 Node/Link 等受限记录；
4. 目标、TTL、无路或记录上限触发终止；
5. 结果沿可用返回路径交给请求方回调；
6. 槽和 Deadline 到期后释放。

返回“截断”表示路径可能比报告更长，并不等于最后记录的节点就是目标。诊断调用应低频、鉴权、受控制预算限制。

## 完整路径信息何时有用

- 验证 A→B→C 是否确实经过预期安全网关；
- 定位哪个中继使 MTU/Profile 降级；
- 比较自动 Route 与 Pinned Path；
- 检查负载均衡后的 Flow 实际绑定；
- 实机测试每跳速率损失和故障切换。

正常业务转发不需要完整路径，避免每帧增加节点列表开销和泄露拓扑。

## 验证清单

- [ ] 未授权 install/revoke 不修改任何表项；
- [ ] 容量、MTU、Profile、Hop 任一失败不留下半安装状态；
- [ ] Owner 重启/Session 变化后旧 Path 不可复用；
- [ ] 转发严格匹配 Path 身份和 Destination；
- [ ] Path ID 的 E2E AAD 绑定有正负向测试；
- [ ] Trace 的记录上限、TTL、超时和截断状态确定；
- [ ] Lite/Nano API 符号存在但明确失败关闭。
