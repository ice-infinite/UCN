# UCN V6 简化版 IMPL-09-02 事务化生命周期实施及自审报告

## 1. 本阶段结论

`IMPL-09-02` 已在私有 Composition Runtime 内实现以下唯一生命周期顺序：

```text
PREPARED
  -> Foundation init
  -> selected Owner init（确定性 init order）
  -> Persistence Foundation recovery（如选中）
  -> Owner publish
  -> Foundation/Adapter start
  -> RUNNING

RUNNING / RELOADING / OWNERS_PUBLISHED / FAULT
  -> Foundation stop
  -> Adapter/协议 obligation drain
  -> Persistence recovery quiescence
  -> selected Owner reverse destroy
  -> Foundation deinit
  -> QUIESCENT
```

本阶段没有把 Composition 变成公共产品 API，也没有把各业务 Owner 的 RX/TX、
Persistence Body 语义导入或控制 Wire 接入生产路径。这里的 durable reload 是
Persistence Foundation 对所有 Manifest Domain 的 Slot、Marker、Witness、Record 和
required/fault 状态完成恢复；各业务模块如何把恢复正文导入自己的状态机，仍由
`IMPL-09-04～09-06` 在模块接线时逐域完成。在这些接线完成前，Coordinator 保持无业务
binding，因而 Registry 中已发布的 Owner 不能从外部取得业务请求，也不能据此宣称产品 Ready。

## 2. 生命周期状态与有界推进

Composition Runtime 使用固定状态：

| Phase | 含义 | 允许动作 |
| --- | --- | --- |
| `PREPARED` | Storage、Registry、Coordinator 已建立，业务 Owner 区仍为零 | `start_begin()` |
| `RELOADING` | Owner 已初始化，Persistence 正在有界恢复 | `start_step()` 或 `stop_begin()` |
| `OWNERS_PUBLISHED` | 必需恢复门已通过，但 Foundation 尚未启动 RX | `start_step()` 或 `stop_begin()` |
| `RUNNING` | Foundation/Adapter 已启动 | `stop_begin()` |
| `STOPPING` | 禁止新生命周期事务，按预算反向退出 | `stop_step()` |
| `QUIESCENT` | Foundation 与全部业务 Owner 已销毁，Composition 本身仍可检查 | `destroy()` |
| `FAULT` | 启动或恢复发生不可安全继续的错误 | 只允许受控 `stop_begin()` |

`start_step()` 和 `stop_step()` 都要求非零 `operation_budget`。一次调用只推进有限数量的
Provider、Foundation 或 Owner 操作，不能用一个公开调用隐藏无界扫描或阻塞等待。

## 3. 初始化前的精确绑定

`start_begin()` 在首次写 Foundation/Owner 前完成以下检查：

1. Start Config 的 schema、保留位、Foundation Storage 大小与指针均正确；
2. Foundation Config 的 `runtime_instance` 与 Composition 精确一致；
3. 每个 Plan 选中模块必须有且只有对应 Config，未选模块 Config 必须为 `NULL`；
4. 每个模块 Config 的 `runtime_instance + owner_instance` 必须精确匹配 Registry；
5. Config、Foundation Storage、Ports、锁 context 与 Composition Storage 不得重叠；
6. Foundation Storage 必须完整为零；
7. 每个业务 Owner Storage 必须仍完整为零。

这关闭了“拿另一个 Runtime/Owner 的合法 Config 初始化当前槽”的错绑路径。测试覆盖了
错误 Route Owner Instance，并逐字节断言 Composition 与 Foundation 输出不变。

## 4. Owner 初始化与失败回滚

Composition 按冻结 Plan 的 init order 调用真实私有 Owner 初始化函数，而不是继续使用
scaffold：

```text
Persistence -> Identity -> Security -> Admission -> Capability
-> Route -> Flow -> Transport -> Service -> Realtime -> Group -> Cluster
```

任何一个 Owner 初始化失败时：

- 尚未初始化的后续 Owner 不会被调用；
- 已初始化 Owner 逆序 destroy/reset；
- Foundation 执行 stop/deinit；
- Registry 回到 `REGISTERED/RESERVED`；
- Owner Storage 和 Foundation Storage 回到全零；
- Composition 回到 `PREPARED`，除非回滚本身失败，此时进入 `FAULT`。

定向测试使用合法绑定但非法 `Route realm=0` 制造“后段 Owner 初始化失败”，证明前段状态
不会泄漏，也不会留下半初始化对象。

## 5. durable reload 与发布门

选中 Persistence 时，`start_begin()` 只启动 Foundation recovery，并把 Composition 置为
`RELOADING`。在 Persistence Owner 报告 Ready 之前：

- `persistence_ready == 0`；
- Foundation 未启动，Adapter RX 未开放；
- 业务 Owner 保持 `INITIALIZED`；
- Registry 只有 Coordinator 为 `ACTIVE`；
- `start_step()` 每次只消耗调用者给出的预算。

任一 required Domain fault 会使 Composition 进入 `FAULT`，不会继续发布 Owner。全部 Domain
通过 Foundation 恢复后，才把已初始化 Owner 发布并进入 `OWNERS_PUBLISHED`。随后仍需另一个
有预算的 step 才调用 `ucn_start()`，因此“恢复通过”和“Adapter 开始接收”不是一个不可审计的
复合副作用。

本阶段还修正了两个交叉构建问题：

- `ucn_kernel` 与 Composition 现在使用同一 `UCN_FEATURE_PERSISTENCE_ENABLED` 编译 Manifest，
  防止 Foundation 正确拒绝但 Composition 测试误以为可启动；
- Persistence 槽按 `ucn_persistence_storage_t` 的完整固定 Storage 分配，不再只按不透明
  Owner 头大小分配。

## 6. Adapter 启动失败的有界退出

Foundation/Adapter 启动只发生在 `OWNERS_PUBLISHED`。若 `ucn_start()` 因状态锁或 Driver
门竞争失败：

- 本次调用立即返回，不自旋、不重试到成功；
- 已发布业务 Owner 退回 `INITIALIZED`；
- Foundation 进入 STOPPING；
- Composition 进入 `STOPPING`，等待调用者继续按预算清理；
- 竞争解除后可以完成反向退出到 `QUIESCENT`。

测试通过持有真实 Foundation state lock 制造启动竞争，并验证失败是有界的，随后可恢复性
停止，不把临时竞争伪装成成功，也不遗留无法销毁的 Owner。

## 7. callback、重入与零写拒绝

Composition 生命周期拥有独立 callback gate；Foundation 公共 API 继续拥有自己的 gate。
测试在真实 Foundation callback claim 活跃时调用 `composition_stop_begin()`：

- 返回 `UCN_ERR_STATE`；
- Composition Storage 逐字节不变；
- Foundation Storage 逐字节不变；
- callback 退出后正常 stop 仍可完成。

Start/Stop Config、View 和 Composition Storage 的所有重叠输入也在首次写入前拒绝。生命周期
API 不把 Provider/Driver callback 内的重入转换成部分状态提交。

## 8. 反向 stop 与恢复期 quiescence

停止顺序不是简单地清零 Owner：

1. `ucn_stop()` 先关闭 Adapter RX，并保留已提交 Driver/协议 obligation；
2. `ucn_step()` 按预算 drain，直到 Foundation `QUIESCENT`；
3. 若 Persistence 仍为 `RECOVERING`，继续按预算推进恢复/取消收口；
4. **只要 Persistence 仍在 RECOVERING，就禁止销毁任何业务 Owner**；
5. 恢复已退出后，按 init order 的严格逆序 destroy Owner；
6. 最后执行 Foundation deinit，并进入 Composition `QUIESCENT`。

第 4 条是本轮最后一次语义自审发现的缺口：旧实现可能在某个 Persistence step 返回零操作
但仍未离开 RECOVERING 时开始销毁下游 Owner。现已增加显式 `recovery_idle` 门，并新增
“RELOADING 中立即 stop”回归；若恢复仍活跃，`initialized_count` 必须保持不变。

## 9. 分项自审

### 9.1 正向顺序

- Minimal Static C1：`prepare -> start_begin -> start_step -> RUNNING`；
- Persistence + Transport：Persistence 未 Ready 前 Transport 不发布，恢复完成后才启动；
- Persistence OFF：无虚假的 durable 阶段，仍能完成最小生命周期。

### 9.2 失败原子性

- 错 Owner Instance 在 init 前零写拒绝；
- 后段 Owner init 失败完整逆序回滚；
- Adapter start 锁竞争有界返回并可继续清理；
- callback 内 stop 对 Composition/Foundation 均逐字节零写。

### 9.3 停止与恢复

- RUNNING 可 drain 到 QUIESCENT；
- RELOADING 可直接请求停止；
- Persistence 尚未离开 RECOVERING 时不销毁下游 Owner；
- destroy 后 Composition Storage 全零，Foundation Storage 全零。

### 9.4 Profile、Feature 与编译器

- Full/Lite/Nano 的实际 Storage 均由同一 Plan 计算；
- Persistence OFF 不引用 Persistence 符号；
- Adapter OFF 不生成依赖真实 Foundation 的生命周期测试；
- GCC 与 MSVC `/W4 /WX` 均编译实际 Composition；
- Archive 门只允许 Composition 对冻结生命周期 API 的精确依赖，不开放任意跨模块符号。

## 10. 验证证据

本阶段最终证据：

| 门禁 | 结果 |
| --- | --- |
| Windows GCC Full，Persistence/可选模块 ON | `104/104` |
| Windows GCC Lite，Persistence/可选模块 ON | `104/104` |
| Windows GCC Nano，Persistence/可选模块 ON | `104/104` |
| Windows GCC Full，Persistence OFF | `94/94` |
| MSVC 19.29 Release `/W4 /WX` 定向 | `5/5` |
| WSL ASan/UBSan 定向 | `5/5` |
| WSL `-fanalyzer -Werror` 构建 + 定向 | `6/6` |
| Composition 生命周期/Storage/Plan 定向 | 全部通过 |

`-fanalyzer` 构建下不运行 call-graph gate：GCC analyzer 会使若干既有间接调用的静态
callgraph 信息变为 unknown；同一 call-stack gate 已在普通 GCC Full/Lite/Nano 构建中通过。

当前全能力 Composition Storage（包含完整 Persistence Foundation Storage）为：

| Profile | Bytes |
| --- | ---: |
| Nano | 31,000 |
| Lite | 88,008 |
| Full | 253,000 |

Nano Composition 单函数最大栈为 `176 B`。以上均为 Host 编译证据，不是目标 MCU 的实际
Flash/RAM/任务栈水位。

## 11. 已知边界与下一步

```text
IMPL-09-02                  = DONE / SELF-REVIEW PASS
Private lifecycle           = IMPLEMENTED
Persistence Foundation gate = IMPLEMENTED
Business semantic imports   = DEFERRED TO IMPL-09-04..09-06
Static C1 production path   = NOT YET WIRED
Public Composition API      = NOT EXPOSED
MCU / real Flash / powercut = NOT PROVEN
External review             = HOLD
```

下一项是 `IMPL-09-03`：只接最小 Static C1 Fast Path，让普通单帧通信经新 Composition/
Coordinator 工作，同时保持无高级能力时不引入 Persistence、Security、Route、Flow 或可选
Owner 的状态与开销。
