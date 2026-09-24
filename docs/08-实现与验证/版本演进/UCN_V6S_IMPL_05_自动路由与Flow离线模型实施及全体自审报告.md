# UCN V6 简化版 IMPL-05 自动路由与 Flow 离线模型实施及全体自审报告

## 1. 结论

`IMPL-05 = IMPLEMENTED / SELF-REVIEW PASS / AUDIT HOLD`。

本阶段已经形成两个互相隔离的 C99 私有实现：

1. 基础 Route Owner：实现冻结的 RREQ/RREP/RERR、Discovery、Reverse obligation、易失
   SoftRoute、静态 fallback、精确失效和有界维护；
2. 高级 Flow Origin Owner：实现已经冻结的 Label/Prefix Codec，以及
   `Candidate → Probed → Staged → Committed → Active` 的离线状态机、InDoubt、Use-time
   preflight、Sequence 生命周期、Fence 和 Drain。

这不是生产放行。完整 Stage/Commit Wire Body、Relay/Target Flow Owner、公共 Runtime、Adapter、
Security、Persistence 接线，真实 MCU、多跳 Bearer、C↔Rust 互操作和性能长稳均不在本阶段签字范围。

## 2. 物理边界

| 对象 | 位置 | 边界 |
| --- | --- | --- |
| Route 私有类型/API | `src/internal/ucn_route.h` | 只允许仓库内部测试和后续 Coordinator 集成使用，不安装。 |
| Route 实现 | `src/routing/ucn_route.c` | 不持有 Flow、Security、Admission 或 Runtime Owner 指针。 |
| Flow 私有类型/API | `src/internal/ucn_flow.h` | 只描述 Origin 侧离线状态，不冒充完整网络事务。 |
| Flow 实现 | `src/routing/ucn_flow.c` | 只消费调用方提供的不可变当前事实；不直接调用其他 Owner。 |
| 定向测试 | `tests/simplified/test_route.c`、`test_flow.c` | 覆盖 Codec、状态机、失败副作用和 ABA。 |
| 并发/资源 | `test_routing_concurrency.c`、`test_routing_resources.c` | 验证共享锁边界、固定容量和 Profile 资源。 |

两个 archive 均为测试期私有 target，不进入 `UCN::simplified`、安装包、导出 target 或生产公共
头。`UCN_BUILD_TESTS=OFF` 时不生成本阶段私有 archive。

## 3. 基础 Route 实现

### 3.1 冻结 Payload Codec

| 消息 | 长度 | 关键绑定 |
| --- | ---: | --- |
| RREQ | 9 B | Transaction、Origin、Target、Hop Count、Cost。 |
| RREP | 29 B | 原始 RREQ、Route Generation、Causal ID、返回 Path/Link 事实。 |
| RERR | 99 B | 完整 RouteDomain、Route Generation、Route Causal ID、Failed Link Generation。 |

所有多字节字段使用 big-endian。Decoder 要求精确长度；Encoder/Decoder 在参数、容量、保留值或
任意部分别名错误时均在首次输出写入前失败，输出长度和缓冲区保持调用前字节。

### 3.2 Discovery 与 RREP

- 同一个 RouteDomain 的并发请求合并到同一 Discovery，但不得刷新既有 txid 和绝对 deadline；
- RREQ 重复只能命中逐字段相同的已接受请求，不能借相同 txid 改写 Origin、Target、Cost 或 Link；
- Reverse obligation 使用固定槽，表满时不驱逐旧事务；
- RREP 必须精确证明已经接受的 RREQ，并沿 Reverse obligation 返回；
- 本地产生的 Discovery 到期后，迟到 RREP 必须拒绝，不能复活过期路径；
- Route 安装完成后，RREP 转发义务和本地 Route 发布保持明确顺序，失败不留下半提交状态。

### 3.3 Route 查找与失效

查找顺序固定为“精确动态 Route 优先，静态 fallback 最后”。静态 Route 不拥有动态 Route
Generation，也不会被普通动态 RERR 删除。

RERR 只有在 RouteDomain、Route Generation、Causal ID、Failed Link ID 和 Failed Link
Generation 全部精确匹配时才失效动态 Route。Link 失效只处理精确 Link 实例；同 ID 不同
Generation、其他 Origin、其他 Realm 和静态 fallback 均不受影响。

维护使用持久化游标和每次调用预算，不在单次 step 中扫描全部表项。

## 4. 高级 Flow 离线模型

### 4.1 已冻结的 Wire 子集

本阶段只实现已有稳定合同的以下 Codec：

- 16 B Label Setup；
- 9 B C2 Prefix；
- 7 B C3 Prefix；
- 11 B C4 Prefix。

完整 Probe、Stage、Commit、Ack、Abort、Receipt Wire Body 尚未冻结，因此本阶段只使用 typed
input/output 建模，不自行发明线上布局。

### 4.2 Proposal 与状态推进

Proposal 摘要和精确字段同时绑定：Route/Link 实例、Route Generation/Causal ID、Capability、
Session、Security/Policy、Profile、MTU、Deadline 和业务 Requirements。摘要只用于预筛选，
状态复用前仍逐字段精确比较。

状态推进为：

```text
Candidate
  -> Probe sent
  -> Probe acknowledged and proposal frozen
  -> resources reserved with zero-write preflight
  -> Stage submitted / acknowledged
  -> Commit submitted / acknowledged
  -> Active published
```

只有精确 Commit ACK 才发布 Active Flow。Commit 结果在 deadline 后仍不明确时进入 `IN_DOUBT`；
不能把超时、迟到成功或调用者猜测写成 Active。新 Active 发布后，旧 Active 先进入 Draining，
再由有界维护退休。

### 4.3 使用点重验与 ABA

每次使用 Active Flow 都重新验证 Route、Link、Capability、Session、Security/Policy、Profile、
MTU 和 Deadline。任何父事实漂移都先 Fence，再拒绝发送。

所有槽 Handle 包含 slot generation。槽释放再复用后，旧 Handle 无法读取、确认、提交或取消新
事务。Origin Sequence 使用 reserve/commit/abort，达到终值后耗尽，不回绕复用。

## 5. 固定资源

| Profile | Route Owner | Flow Owner | 合计 | Discovery/Reverse/Dynamic/Static | Candidate/Activation/Active/Receipt |
| --- | ---: | ---: | ---: | --- | --- |
| Nano | 1,744 B | 2,656 B | 4,400 B | 2/4/4/4 | 2/2/2/2 |
| Lite | 5,056 B | 9,856 B | 14,912 B | 4/12/16/8 | 8/8/8/8 |
| Full | 13,376 B | 32,960 B | 46,336 B | 8/32/48/16 | 24/24/32/32 |

实现不调用 `malloc/calloc/realloc/free`。正常 GCC `-fstack-usage` 的本阶段最大单函数结果为：

- Route：656 B；
- Flow：784 B。

因此 IMPL-05 私有 target 使用 1,024 B 单函数门限。这个值是 Host 编译器结果，不是 MCU 任务栈
水位；接入目标 RTOS 后必须重新测量完整调用链。

## 6. 分项自审与发现的整改

| 自审方向 | 发现 | 整改 |
| --- | --- | --- |
| Codec 独立性 | 编解码互相同错可能掩盖字段序问题。 | 测试直接消费冻结 Golden，并运行独立 C/Rust Oracle。 |
| RREQ 重放 | 相同事务号的字段变异可能改写 Reverse 语义。 | 已接受 RREQ 只允许逐字段完全相同的重复。 |
| RREP 新鲜度 | 迟到 RREP 可能在 Discovery deadline 后安装 Route。 | 本地 Origin 在安装前复核 Discovery 绝对 deadline。 |
| Flow Proposal | 只绑定部分 Route/Capability 字段会继承旧 Probe。 | 摘要及 canonical Proposal 纳入完整父事实与 Requirements。 |
| 状态超时 | Stage/Commit 发送结果晚于 deadline 可能伪成功。 | Stage 转 Abort；Commit 转 InDoubt；均不得发布 Active。 |
| Slot ABA | 释放后的旧下标可能命中新事务。 | Handle 加 slot generation，释放时保留并推进 generation。 |
| 输出别名 | Owner、输入和输出重叠可能覆盖状态。 | 所有公开私有入口在取锁和写状态前执行双向范围重叠检查。 |
| 资源回收 | 发布 Active 后 Candidate 仍占槽会造成固定表耗尽。 | Commit ACK 成功后释放 Candidate；旧 Active 走 Drain。 |

## 7. 验证结果

| 门禁 | 结果 |
| --- | --- |
| Windows GCC Full/Lite/Nano | 各 65/65 PASS |
| Windows GCC Nano Feature-OFF | 54/54 PASS |
| MSVC 19.29 Release `/W4 /WX` | 55/55 PASS |
| WSL ASan/UBSan 定向 | Route/Flow/Resource/Concurrency 4/4 PASS |
| WSL `-fanalyzer -Werror` | Route/Flow/Concurrency 3/3 PASS |
| Clang 18 Release `-Werror` | Route/Flow/Concurrency 3/3 PASS |
| 普通 pthread 压力 | 双 Route/Flow Owner 并发 200 轮 PASS |
| C/Rust Frozen Oracle | 7/7 PASS |
| Boundary / archive / stack gate | PASS |

GCC TSan 二进制完成编译，但当前 WSL 6.6 运行时在测试入口报
`ThreadSanitizer: unexpected memory mapping`；Clang 18 环境又缺少
`libclang_rt.tsan-x86_64.a`。因此本报告不把 TSan 写成通过证据，只记录普通 pthread 压力与
锁合同；换到可运行 TSan 的 Linux 主机后仍需补跑。

ASan/UBSan 的全量 CTest 中，插桩会使既有固定栈门禁超限，安装 consumer 也未自动链接
sanitizer runtime；因此 Sanitizer 结论严格限定为本阶段四个定向目标，不把这些工具组合限制
写成协议失败。

## 8. 未完成与下一步

以下项目保持开放：

1. 完整 Flow Probe/Stage/Commit/Abort/Receipt Wire Body 与独立 Golden；
2. Relay/Target Flow Owner 和三方原子资源预留；
3. Coordinator、Security、Adapter、Persistence、公共 Runtime 的真实接线；
4. C↔Rust 在线互操作；
5. ESP32-S3 真实多跳、断链 RERR、Route 重发现和 Flow 切换；
6. MCU RAM/Flash/完整任务栈、功耗、吞吐和长稳；
7. 可运行环境中的 TSan。

下一项可以开始 C `IMPL-06` Reliable/Transfer，但不得把本阶段私有模型称为生产自动路由或
最终 Flow 协议，也不得继承 IMPL-04 或历史 Rust 实现的签字。
