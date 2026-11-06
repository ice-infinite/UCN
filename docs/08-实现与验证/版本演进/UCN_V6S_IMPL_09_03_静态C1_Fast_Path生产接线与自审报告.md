# UCN V6 简化版 IMPL-09-03 静态 C1 Fast Path 生产接线与自审报告

## 1. 本阶段结论

`IMPL-09-03` 已把最小静态 C1 通信接入私有 Composition Runtime。接入方式不是再实现一套
Runtime，而是在 Composition 生命周期门后建立一个薄外观，继续使用唯一的 Foundation：

```text
Application/private product glue
  -> Composition lifecycle/reentry gate
  -> existing Foundation public operation
  -> existing fixed queue + C1 codec + Adapter token
  -> Driver

Driver fact
  -> Composition ingress gate
  -> existing Foundation driver ingress
  -> existing RX queue/attempt state
  -> Composition step
  -> Endpoint callback
```

因此，本阶段没有新增 Wire 字段、第二套队列、第二套 Replay/Sequence、第二个 Adapter 或隐藏的
高级 Owner。它完成的是“把已经验证过的静态 C1 Foundation 纳入统一装配和生命周期”，而不是
修改 C1 协议。

## 2. 为什么 Static C1 不经过业务 Dependency Slot

Coordinator 的 typed dependency slot 用于两个独立业务 Owner 之间的 Requirement/Event/Proof
事务。Static C1 的 Endpoint、Path、队列、Codec 与 Adapter 本来就共同属于 Foundation，强行把
一次普通单帧发送拆成跨 Owner 事务只会增加状态和调度延时。

本阶段采用以下边界：

- Composition 是产品级编排入口，负责生命周期、重入和 Owner 身份；
- Foundation 内部继续以原来的最短调用链完成静态 C1；
- Coordinator 的业务 binding/slot 在最小 C1 路径中必须保持全零；
- 未来 Security、Route、Service 等独立 Owner 的交互必须使用 typed dependency，不得借本阶段
  的 Foundation 快路绕开 Coordinator。

这既符合“跨 Owner 必须经 Coordinator”，也避免把同一 Owner 内部的普通调用人为拆长。

## 3. 精确 Owner 身份绑定

原 Foundation 公共初始化固定使用独立 Core Owner ID。Composition Registry 则按
`owner_instance_base + module_id` 分配实际 Owner。如果继续直接调用公共 `ucn_init()`，
Foundation 发出的 Endpoint、Link、Path、Send Handle 会携带错误 Owner ID，无法证明它们属于
当前 Registry 记录。

本阶段新增私有初始化入口：

```text
ucn_i_node_init_with_owner_instance(..., exact_registry_owner, ...)
```

规则如下：

1. 公共 `ucn_init()` 行为不变，独立 Runtime 仍使用 Core Owner 1；
2. Adapter 内部 Owner 2 继续保留；
3. Composition 的 `owner_instance_base` 必须大于 2；
4. `start_begin()` 只把 Registry 第 0 槽的精确 Owner 传给 Foundation；
5. Runtime 每次完整校验 Foundation 的 Runtime/Owner 是否与 Registry 第 0 槽一致；
6. Archive 门不允许 Composition 回退调用公共 `ucn_init()`。

测试逐项证明 Foundation 发出的 Endpoint、Path、Link 和 Send Handle 都绑定当前
Composition Runtime 与 Foundation Registry Owner；基址 1 或 2 均在首次写入前失败，输出指针
与 Storage 不变。

## 4. TX Fast Path

静态发送的唯一调用链为：

```text
composition_publish()
  -> composition gate
  -> ucn_publish()
  -> fixed request/attempt/traffic-class queue

composition_step(now, budget)
  -> composition gate
  -> ucn_step()
  -> existing C1 encode
  -> existing Adapter token reserve/submit
  -> Driver submit
```

Composition 不保存 Payload、Frame、Origin Sequence 或 Driver Token。输出 Handle 先写局部 staging，
只有 Foundation 操作与 Composition gate leave 均成功后才写回调用者。

### 4.1 Wire 开销证明

C1 Header 公式没有改变：

```text
C1 header bytes = 3 + 2 * address_width + 6
```

测试使用 A1 地址宽度，实际 Header 仍为 11 B，并直接对 Driver 捕获的真实 Frame 调用同一 C1
Decoder，核对 Payload 长度和逐字节内容。Composition 不在 Frame 前后附加任何 Header、Tag 或
内部句柄。

### 4.2 背压与同步早到 completion

- `NOT_SUBMITTED`：退休本次 Adapter Token，但保留同一 Request/Attempt、Origin Sequence 与
  编码 Frame；测试连续两次背压后第三次成功，三次字节完全一致；
- 同步早到：Fake Driver 在 `submit()` 尚未返回时通过 Composition ingress 上报 completion；
  Foundation 的既有 early-completion latch 接收该事实，最终只产生一个成功终态；
- 所有推进仍受 `ucn_step_budget_t` 约束，不在外观层添加隐藏循环。

## 5. RX Fast Path 与 callback 边界

真实双节点测试使用两个独立 Composition Runtime：发送端 Driver 把完整编码帧交给接收端的
Composition RX ingress，接收端随后在自己的 `composition_step()` 中完成 C1 解码和 Endpoint
投递。

Endpoint callback 内的规则为：

- 普通 Composition 读写入口因活动 step gate 返回 `UCN_ERR_STATE`；
- 只有携带 Foundation 签发的精确 callback scope，才可读取冻结 Link/Send/Stats View；
- scope 的 Runtime、Owner、nonce 和生命周期仍由 Foundation 校验；
- callback 不能删除 Endpoint 或启动新的写事务。

测试同时断言回调发生时的 Stats 是 callback 进入前冻结的快照，而不是回调执行到一半的可变
状态。

## 6. Driver ingress 与生命周期

Driver RX、TX completion 和 Link Event 可能异步到达，也可能在 `submit/cancel` 内同步回调。
Composition ingress 使用两种路径：

1. 无活动 Composition 操作时，先取得独立 ingress gate，再进入 Foundation；
2. 当前恰为同一 Runtime 的 `RUN_STEP` 或 `STOP_STEP` 时，允许嵌套 Driver fact 直接进入
   Foundation，以支持同步早到 completion/cancel；
3. 其他生命周期操作、PREPARED/QUIESCENT/FAULT 或不匹配 gate 一律拒绝。

Composition 不替代 Foundation 的 Adapter lock、Driver callback gate 和 Token/Link generation
检查。若 Foundation 进入 Fault，Composition 会撤销已发布 Owner 状态并同步进入 Fault，避免
外层继续显示 RUNNING。

## 7. Cancel、Stop 与失败原子性

测试覆盖两条 obligation 收口路径：

- 显式 `send_cancel()` 后继续 step，Driver cancel 只调用一次，Send 进入
  `UCN_ERR_CANCELLED` 终态；
- 在仍有 submitted Attempt 时执行 `stop_begin()`，反向 quiescence 驱动同一 Foundation
  cancel/drain，随后 Composition 才允许 destroy。

此外还验证：

- PREPARED 阶段调用 Fast Path 返回 `UCN_ERR_STATE`，输出哨兵不变；
- Config/DTO/Payload/output 与 Composition Storage 重叠时在首次写入前拒绝；
- 所有带输出的外观先写局部 staging；
- Static C1 Minimal Plan 的 `init_count=0`、durable mask=0；
- 完成收发后 Coordinator binding/slot 仍逐字节为零。

## 8. 分项自审

### 8.1 架构自审

- 只有一个 Foundation 状态机、一个 Adapter 和一组队列；
- Static C1 没有伪造跨 Owner dependency；
- Composition archive 对 Foundation 的公共依赖只允许明确列出的 API，私有初始化只为精确
  Owner 绑定开放一个具名例外；
- 公共 `ucn_init()` 不在 Composition allowlist，防止 Owner 身份回退；
- 业务高级 Owner 仍未取得生产 RX/TX 路径。

### 8.2 Wire 与资源自审

- C1 编解码实现和 Header 公式未改；
- Composition Runtime、Plan、Registry 和 Foundation 结构均未增加字段；
- Minimal Plan 没有隐式 Persistence/Security/Route/Flow；
- Full GCC 的 Composition 最大单函数 Host 栈仍为 176 B，新增外观最大值不超过 128 B；
- 没有动态内存、递归等待或无界扫描。

### 8.3 状态机自审

- 仅 RUNNING 接受普通 Fast Path；
- STOPPING 只接受为 quiescence 服务的 Driver fact；
- callback 中 mutation 关闭、精确 snapshot read 开放；
- 背压不伪造终态，同步早到不丢 completion；
- cancel/stop 均复用同一 Attempt/Token 生命周期。

### 8.4 负向自审

- 错 phase、Storage alias、保留 Owner ID 和非法 gate 均失败关闭；
- 输出 API 只有完整成功后写回；
- Adapter OFF 不生成 Static C1 Composition 测试，也不能形成虚假可用 Plan；
- Persistence OFF 不影响真正无 durable 承诺的 Static C1。

## 9. 验证证据

| 门禁 | 结果 |
| --- | --- |
| Windows GCC Full Debug 全量 | `105/105` |
| Windows GCC Lite 定向 | `6/6` |
| Windows GCC Nano 定向 | `6/6` |
| Windows GCC Persistence OFF 定向 | `6/6` |
| Windows GCC Adapter OFF 定向 | `3/3` |
| MSVC 19.29 Release `/W4 /WX` 定向 | `6/6` |
| WSL ASan/UBSan 定向 | `6/6` |
| WSL Clang 定向 | `6/6` |
| WSL `-fanalyzer -Werror` 构建与定向 | `5/5` |
| Archive dependency gate | PASS |
| `git diff --check` | PASS，仅既有换行提示 |

本轮 MSVC 首次运行发现测试中的两个局部变量只依赖循环必执行推导，`/W4 /WX` 正确拒绝；现已
显式初始化，并在相同门禁下通过。该修复不改变生产实现。

## 10. 当前边界与下一步

```text
IMPL-09-03                    = DONE / SELF-REVIEW PASS
Private Composition Static C1 = IMPLEMENTED
Wire overhead                 = UNCHANGED
Second queue/state machine    = NONE
Public Composition API        = NOT EXPOSED
Dynamic Route / Security      = NOT YET WIRED
MCU / physical bearer         = NOT PROVEN
External review               = HOLD
```

下一项是 `IMPL-09-04`：接入 Security、Identity/Admission、Capability、Route 与 Flow，并冻结
唯一的 RX/TX 校验顺序和 use-time generation 门禁。该阶段不得修改本轮 C1 Wire，也不得让动态
能力成为最小 Static C1 的隐式依赖。
