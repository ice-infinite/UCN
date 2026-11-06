# UCN V6 简化版 IMPL-09-06 Realtime、Group、Cluster 独立接线及自审报告

## 1. 本阶段结论

`IMPL-09-06` 已完成三个 Optional Owner 到私有 Composition 的独立 typed 接线：

```text
Coordinator gate
  +-- Realtime Owner : LOCAL_STAMP policy / prepare
  +-- Group Owner    : static install / send begin / attempt peek
  +-- Cluster Owner  : member observe / authority preflight
```

这里的“接线”不是把三个模块互相调用，也不是把所有高级能力塞进每个数据包。它只是在产品 Plan
明确选择某个能力时，让 Coordinator 能调用该 Owner 的最小操作面。每次调用只验证并授权一个模块
和一项能力；未选择的模块既不能被调用，也不会因为另一个 Optional Feature 被动进入依赖闭包。

本阶段仍没有开放公共 Composition API，也没有接入 raw Wire、Security `open/protect`、Adapter
submit/RX dispatch 或生产 Control Opcode。以上工作属于 `IMPL-09-07`，因此本报告不能作为实机、
公共 SDK 或完整生产协议放行证据。

## 2. 统一进入门与失败原子性

三个模块复用 `composition_optional_enter()`。进入顺序固定为：

1. 校验 Composition Runtime 的完整结构和当前 `RUNNING` phase；
2. 校验当前 Plan 精确选择了目标模块；
3. 校验当前 Plan 精确选择了本次操作需要的能力；
4. 校验目标 Owner 仍为 `ACTIVE`；
5. 校验生命周期/Coordinator gate 当前空闲；
6. 以本操作唯一的 Operation ID 进入 Coordinator gate；
7. 调用目标 Owner；
8. 退出 gate 后，只有最终结果为 `UCN_OK` 才发布 caller output。

所有 caller 输入和输出还必须同时位于 Composition Storage 与独立 Foundation Storage 之外。这样既
防止调用方用输出覆盖 Owner/Registry，也防止通过 Foundation Node 区域别名破坏基础通信状态。
Handle、Prepared metadata、Attempt 和 Authority View 都先写入局部零初始化候选，失败不写回调用方。

## 3. Realtime：LOCAL_STAMP 独立路径

当前 Composition 只接入最小、无网络时间依赖的 LOCAL_STAMP：

- 安装 Endpoint 的 LOCAL_STAMP policy；
- 用本地 capture time、采样上界和硬件采样标志准备元数据；
- 成功结果必须 `metadata_present=1`；
- `mode` 必须仍是 `LOCAL_STAMP`；
- `domain_time_valid`、`domain_generation`、`clock_domain_id` 必须为零。

Realtime-only Plan 的断言为：

```text
selected = Foundation + Realtime
absent   = Flow + Persistence + Group + Cluster
```

这意味着只需要“附带本地采样时间”的用户不会承担路由 Flow、持久化或分布式时间域状态。DEADLINE、
SYNCED_STAMP 和网络四时间戳同步没有混入本入口。

## 4. Group：Static Group 独立路径

当前 Composition 只接入静态 Group 的三个必要步骤：

1. 从经过认证且具备 anti-rollback 证明的静态 Manifest 安装固定槽 Group Context；
2. 按精确 Group/Policy/Member Generation 建立静态发送；
3. 读取 Owner 冻结的下一项单播 Attempt。

Group-only Plan 的断言为：

```text
selected = Foundation + Group
absent   = Persistence + Realtime + Cluster
```

因此静态群发不是 Cluster 的别名，也不需要 Cluster Head、Quorum、Epoch 或成员接管。测试覆盖认证失败、
错误 Group Generation、成功的静态单播 fanout，以及失败后 Context/Send/Attempt 全部不变。

## 5. Cluster：Base Authority 独立路径

Cluster Base 当前接入两项 use-time 操作：

- 观察一个经过认证、当前有效并绑定 Binding/Session/Capability/Route/Link Generation 的成员事实；
- 以当前 `now_us` 执行 Authority preflight，只有当前配置和 live voter 仍满足要求时才发布 View。

Cluster 能力按冻结合同选择自身需要的基础闭包：

```text
Foundation + Persistence + Identity + Security + Capability
           + Route + Flow + Cluster
```

该闭包明确不选择 Realtime 或 Group。测试使用一个单 voter Stable Config 验证：未认证 Fact 不能写成员
表；合法 Fact 可建立当前 Authority；到半开租约边界后 preflight 返回拒绝，调用方 Authority View 哨兵
保持不变。

## 6. 物理归档依赖门禁

仅在 Plan 中“不选择”模块还不够。本轮扩展归档符号检查，对三个可选 archive 的未定义符号持续执行
denylist：

| Archive | 禁止依赖 |
| --- | --- |
| Realtime | Group、Cluster、Flow、Persistence |
| Group | Realtime、Cluster、Security、Route、Persistence |
| Cluster | Realtime、Group |

门禁同时识别 GCC/Clang `.a` 与 MSVC `.lib`。此外分别建立 Realtime-only、Group-only、Cluster-only
产品组合并真实链接测试，避免“全功能构建恰好补齐悬空符号”掩盖模块耦合。

## 7. 本轮逐点自审与整改

### 7.1 Realtime 自审

- 正常：LOCAL_STAMP policy 与 prepare 成功；
- 失败：未知 Endpoint 不写输出；DEADLINE policy 被本地专用入口拒绝；
- 解耦：Plan 和 archive 均不引入 Flow/Persistence/Group/Cluster；
- 结论：通过。

### 7.2 Group 自审

- 正常：认证 Static Group 安装、发送建立、Attempt 读取成功；
- 失败：Manifest 未认证、Generation 错误均零状态拒绝；
- 解耦：Plan 和 archive 均不引入 Cluster；
- 结论：通过。

### 7.3 Cluster 自审

- 正常：当前成员事实可形成 Authority View；
- 失败：未认证成员不写表，到期成员不能继续授权；
- 解耦：Plan 和 archive 均不引入 Realtime/Group；
- 结论：通过。

### 7.4 全局自审中发现并修复的问题

1. 最初的外部范围检查只排除了 Composition Storage。现已同时排除独立 Foundation Storage，并有
   Realtime 输出别名反例；
2. 单独开启 Realtime 时，测试中只供 Group/Cluster 使用的 helper 触发 `-Werror`。helper 现按精确
   Feature 条件编译，三个单模块构建均通过；
3. GCC 能推导下层成功后完整初始化候选，MSVC `/W4 /WX` 仍报告潜在未初始化。所有候选现在显式
   零初始化；这同时避免未来下层错误实现把未定义字节发布给调用方。

## 8. 栈与固定资源

普通 GCC `-fstack-usage` 对新增入口的结果：

| 入口 | 静态栈 |
| --- | ---: |
| Realtime policy | 96 B |
| Realtime prepare | 144 B |
| Group static install | 128 B |
| Group send begin | 144 B |
| Group attempt peek | 224 B |
| Cluster member observe | 96 B |
| Cluster authority preflight | 160 B |

最大值为 224 B，低于当前单函数 256 B 门限。Owner Storage、Context、成员表和 pending 数量仍全部来自
编译期 Profile；本轮没有动态分配、后台任务或隐式 Timer。

## 9. 验证矩阵

| 门禁 | 结果 |
| --- | --- |
| Windows GCC Full Debug 全量 | `107/107` |
| Windows GCC Lite Debug 全量 | `107/107` |
| Windows GCC Nano Debug 全量 | `107/107` |
| Windows GCC Persistence-OFF 全量 | `96/96` |
| Windows GCC 全 Optional-OFF 全量 | `71/71` |
| Windows GCC Adapter-OFF 全量 | `101/101` |
| Realtime-only：Archive + Composition | `2/2` |
| Group-only：Archive + Composition | `2/2` |
| Cluster-only：Archive + Composition | `2/2` |
| MSVC 19.29 Release `/W4 /WX` 全量 | `89/89` |
| WSL ASan/UBSan 适用矩阵 | `104/104` |
| WSL `-fanalyzer -Werror` 适用矩阵 | `117/117` |
| `git diff --check` | PASS，仅既有 CRLF 提示 |

ASan 排除了插桩后不再代表普通产品栈的 Common/Kernel `.su` 与未链接 sanitizer runtime 的简化版
安装消费者；Analyzer 排除了会把 analyzer dynamic frame 当成未知栈的三个 call-stack gate。普通 GCC
和 MSVC 全量构建继续负责栈、ABI 与安装消费者证据，未把不适用门禁伪记为通过。

## 10. 当前边界与下一步

```text
IMPL-09-06                         = DONE / SELF-REVIEW PASS
Realtime LOCAL_STAMP composition = IMPLEMENTED
Static Group composition          = IMPLEMENTED
Cluster Base composition          = IMPLEMENTED
Cross-optional archive dependency = DENIED BY GATE
Public Composition API            = NOT EXPOSED
Raw Wire / Security / RX-TX       = NOT YET WIRED
MCU / Flash / physical bearer     = NOT PROVEN
External review                   = HOLD
```

下一项是 `IMPL-09-07`：冻结并接入生产 Control Wire/Opcode/AAD/dispatch。它必须复用本轮三个 typed
入口，不能在 raw RX/TX 旁边再建立第二套 Optional Owner 授权链。
