# UCN V6 简化版 C 后续模块架构骨架与自审报告

> 日期：2026-09-20  
> 状态：`ARCHITECTURE SCAFFOLD DONE / SELF-REVIEW PASS`  
> 实现边界：只覆盖 C 简化版 IMPL-03～IMPL-08 的私有物理边界，不代表任何模块业务功能完成。

> 后续状态：IMPL-03 已在本骨架之上完成私有静态 Security Session 实现与全体自审，见
> [IMPL-03 报告](UCN_V6S_IMPL_03_静态安全Session实施与全体自审报告.md)。本报告以下“详细实现
> 未开始”属于骨架阶段快照，不应再用于判断当前 IMPL-03～06 进度。IMPL-04
> Dynamic Admission/Capability、IMPL-05 Route/Flow 和 IMPL-06 Reliable/Transfer 也已分别
> 形成私有 Host C 实现与独立自审报告；IMPL-07 Service/Operation/QoS、IMPL-08A Realtime、
> IMPL-08B Group、IMPL-08C Cluster 本地控制核心及 IMPL-08D Cluster 镜像/Lineage/跨簇
> 私有模型也已完成分项实现与自审。最新 Cluster 结论见
> [IMPL-08D 报告](UCN_V6S_IMPL_08D_Cluster镜像Lineage与跨簇私有模型实施及全体自审报告.md)。
> 不得从本历史骨架快照推导当前业务完成度或生产接线状态。

## 1. 为什么先搭骨架

IMPL-00～02 已经提供 Common、Coordinator、最小静态通信和统一 Persistence Foundation。
如果后续直接在 Runtime 或一个大文件中依次添加安全、准入、路由、可靠传输、服务和 Cluster，
会重新形成旧版本中“一个模块隐式拥有另一个模块状态”的耦合。此次先建立编译目标和依赖方向，
目的是让后续每个模块都只能在自己的目录、target 和 Owner 边界内生长。

本阶段刻意不做以下事情：

- 不公开新用户 API；
- 不冻结尚未逐模块复审的 ABI；
- 不实现 Wire Opcode 或 Codec；
- 不创建 Session、Route、Transfer、Operation、Time、Group 或 Cluster 业务状态；
- 不将任何骨架链接进 `UCN::simplified`；
- 不从旧 `src/v6/` 或 Rust crate 复制状态机。

## 2. 物理结构

| 实施阶段 | 私有 target | 源目录 | 后续唯一职责 |
| --- | --- | --- | --- |
| IMPL-03 | `ucn_v6s_security_scaffold` | `src/security/` | Session、认证、加密、Replay、ACL |
| IMPL-04 | `ucn_v6s_admission_scaffold` | `src/admission/` | Bootstrap、JOIN、Lease、Binding、Capability |
| IMPL-05 | `ucn_v6s_routing_scaffold` | `src/routing/` | Discovery、SoftRoute、RERR、Forwarder、Flow activation |
| IMPL-06 | `ucn_v6s_transport_scaffold` | `src/transport/` | Reliable、Receipt、Flow data、Fragment/Transfer |
| IMPL-07 | `ucn_v6s_service_scaffold` | `src/service/` | Request/Result、Operation、Latest、Advanced QoS |
| IMPL-08 | `ucn_v6s_realtime_scaffold` | `src/realtime/` | Local stamp、Time Domain、可选网络同步 |
| IMPL-08 | `ucn_v6s_group_scaffold` | `src/group/` | Static/Dynamic Group 与成员完成范围 |
| IMPL-08 | `ucn_v6s_cluster_scaffold` | `src/cluster/` | Config、Authority、Backup、Takeover、Recovery |

共享的 `ucn_module_scaffold` 只拥有 16 字节内部描述符校验，不拥有业务对象。所有 target 都是
私有 `EXCLUDE_FROM_ALL`：测试关闭时不会被默认产品构建生成，测试打开且骨架测试引用时才编译。

## 3. 依赖规则

每个模块的直接编译依赖固定为：

```text
Module Scaffold Target
    -> ucn_common
    -> ucn_coordinator
        -> ucn_common
```

`ucn_module_scaffold` 是测试/验证侧的共享描述符校验库，由骨架测试单独链接；业务模块 target
不会把这套验证工具当作运行依赖。

模块描述符的 `coordinator_visible_mask` 只回答“该 Owner 将来可能经 Coordinator 消费哪些
不可变事实或 typed requirement”。它不是硬依赖列表，也不允许直接函数调用。描述符同时把相同
集合写入 `forbidden_direct_owner_mask`，使“看得见”与“禁止直连”成对出现。

例如 Security 可经 Coordinator 请求 Persistence proof，但 Security target 不链接
Persistence；Admission 可消费 Security/Persistence 事实，但不直接调用两个 Owner。Realtime、
Group、Cluster 仍为三个独立分支：Realtime 看不到 Group/Cluster，Group 看不到
Realtime/Cluster，Cluster 也看不到 Realtime。Cluster 对 Group 的位只表示未来可选的非权威
加速事实，不表示 Cluster 依赖 Group 才能运行。

## 4. 失败关闭规则

骨架不是空成功 stub。统一探针具有以下合同：

1. 描述符尺寸、Schema、模块 ID、实施阶段、直接依赖、Coordinator 可见集合、禁止直连集合、
   flags 或保留字段任一错误，返回 `UCN_ERR_CONFIG`；
2. 输出为空返回 `UCN_ERR_ARGUMENT`；
3. 合法骨架的业务探针返回 `UCN_ERR_UNSUPPORTED`；
4. 所有失败路径保持输出逐字节不变；
5. 骨架不安装公共头，因此产品无法误用内部探针。

## 5. 测试与机器门禁

新增 `ucn_v6s_module_scaffold_tests`，覆盖：

- 八个模块 ID 唯一且没有遗漏；
- IMPL-03、04、05、06、07、08 阶段映射精确；
- 直接依赖只能为 Common/Coordinator；
- 模块不能把自身登记为 Coordinator 依赖；
- Coordinator 可见集合必须与禁止直连集合完全一致；
- 业务探针一律 UNSUPPORTED 且不改输出；
- 非法直接依赖和空输出失败关闭；
- Realtime/Group/Cluster 的独立性不变量。

`check_v6s_impl_boundaries.py` 已登记全部新源文件、内部头、CMake target 和链接方向，并继续
扫描旧 v6 include、动态内存、安装/导出泄漏。`check_v6_archives.py` 只允许这些私有 archive
出现 `ucn_i_*` 符号。

本轮实际验证结果：

| 配置 | 结果 |
| --- | --- |
| Windows GCC 14.2 Full、Persistence OFF | 全量 CTest `48/48` |
| Windows GCC 14.2 Lite、Persistence OFF | 骨架、边界与符号定向测试 `3/3` |
| Windows GCC 14.2 Nano、Persistence ON | 全量 CTest `52/52` |
| MSVC 19.29 Full Release | 骨架编译、边界、符号与定向测试 `3/3` |
| Nano、`UCN_BUILD_TESTS=OFF`、Persistence OFF | 默认产品构建成功；骨架 archive `0` |
| 空白与静态扫描 | `git diff --check` 无空白错误；骨架无动态内存和旧 v6 include |

## 6. 历史外审候选边界

IMPL-02 的 118 项摘要是提交 `ffe39f4` 的历史签字快照。进入后续阶段后，任务表、CMake、
内部头和测试必然变化，因此不能继续把旧摘要当成当前工作树证明，也不能通过重生成同名摘要来
伪造“仍是原签字内容”。CMake 现在只有在显式设置
`UCN_VERIFY_IMPL02_SIGNED_CANDIDATE=ON` 时才复现旧候选门禁；当前后续工作树报告 stale 是正确
结果。新阶段完成后应建立自己的候选清单和外审基线。

## 7. 自审结论与下一步

正向回读结果：目录、target、描述符、测试和机器登记一致，没有公共 API、Wire 或运行时接线。
反向回读结果：从“错误模块直连、骨架伪成功、可选分支互相依赖、安装面泄漏、旧 v6/Rust
复用”五类故障出发，均存在静态或运行门禁。

本阶段只能得出：

```text
C module architecture scaffolds = DONE / SELF-REVIEW PASS
IMPL-03 detailed implementation = NOT STARTED / NEXT
IMPL-04..08 business functions  = NOT STARTED
Rust implementation             = UNCHANGED
Production / MCU readiness       = NOT CLAIMED
```

骨架阶段的原定下一步为 IMPL-03：先冻结静态 Session 的 C 私有对象和密码 Provider 边界，再实现
canonical AAD、O1/O2/H1、Replay、ACL 和 Persistence proof 接线；H2 依赖 C2 Direct Flow，
留给后续 Flow 阶段。完成定向/负向/容量/并发/
Feature-OFF 测试后，才允许 Admission 使用 Security View。
