# UCN V6-05 C99 Opaque、Config 与 Protocol Owner 实现报告

> 状态：隔离实现与分项自审完成；最终统一外审延期。
> 生产状态：`UCN_BUILD_V6_EXPERIMENTAL=OFF` 时不编译或链接本阶段对象。

## 1. 为什么需要本阶段

v6 面向固定内存 MCU，同时又不允许应用代码依赖协议私有字段。只返回运行期
`storage_required()` 不能用于 C99 文件作用域数组；直接公开状态结构体又会让应用修改字段、跨版本
复制布局或绕过 Owner。V6-05 因而同时冻结三层合同：

1. 产品在编译期得到容量、`*_STORAGE_BYTES` 和 `*_STORAGE_ALIGNMENT`；
2. 应用只声明对齐 storage union，协议库在 `init_in_place()` 中建立 opaque 对象；
3. ISR/驱动只投递事件，唯一 Protocol Owner 才能推进协议状态。

Opaque 不能物理阻止同一地址空间的越界写或 `memset()`。它只禁止合法调用方依赖字段布局，并通过
magic、Schema、Layout Hash 与末尾 canary 在后续 API 入口检测明显损坏、随后失败关闭。需要抵抗恶意
同地址空间写入的产品仍应使用 MPU、TrustZone 或进程隔离。

## 2. 编译期配置与 Manifest

`ucn_v6_config.h` 是 v6 唯一容量合同。当前包含 Binding、Active/Dynamic Group、Bootstrap pending、
Bootstrap Link budget、Operation、Principal high-water、Static Group、Group Key 与 Owner event depth。
每项都有编译期合法域；数组容量和 storage bytes 从同一宏生成，不存在运行时堆分配。

编译后的 Manifest 精确保存：

```text
api_version + storage_layout + feature_bits + layout_hash
+ every capacity that changes state/layout/resource semantics
```

应用必须把自己编译时看到的 Manifest 传给初始化。任一字段不同都返回 `UCN_V6_ERR_CONFIG`；测试用
另一个翻译单元把 Binding 容量改为 15，确认它不能初始化按 16 编译的库。Layout Hash 不是安全
MAC，而是快速布局标识；精确字段比较才是最终裁决。

## 3. C99 opaque in-place 对象

以下已有 v6 活状态已完成私有化：

| 对象 | 公共持有方式 | 只读观察 |
| --- | --- | --- |
| Identity Authority | `ucn_v6_identity_authority_storage_t` | Authority view |
| Bootstrap Owner | `ucn_v6_bootstrap_owner_storage_t` | Pending copy |
| Operation-ID Allocator | `ucn_v6_operation_id_allocator_storage_t` | Allocator view |
| Durable Operation Journal | `ucn_v6_operation_journal_storage_t` | Slot copy / Journal view |
| Protocol Owner | `ucn_v6_protocol_owner_storage_t` | Owner view |

调用顺序固定为：

```text
declare aligned storage union
    -> obtain exact compiled Manifest
    -> init_in_place(storage, bytes, manifest, config/ops, &opaque_handle)
    -> use opaque handle only through public API
```

初始化先验证全部参数、Manifest、容量和对齐，再获取必要的共享门、调用 Provider 或写入 storage。
因此错误 Manifest、少一个字节、非对齐地址、活动 Provider 门和非法配置均不会部分初始化，也不会
修改输出 handle。Provider load 后发现非法 Record 时同样不发布对象。

未来 V6-08～12 新增的 Route/Node、QoS、Transfer、Realtime 与 Cluster 活状态必须使用相同模式；
那些对象尚未实现，不能把本阶段描述成它们已经完成。

## 4. 唯一 Protocol Owner

Owner 接受五类事件：RX、TX、Completion、Timer 与 Provider。任务上下文与 ISR 使用两个明确
不同的锁合同：`lock_task()` 必须阻塞或以其他方式保证返回时已经持锁，只有
`try_lock_from_isr()` 可以失败并要求调用方重试。公开 `post()` 只在对应锁下增加固定计数并执行
通知，不调用 Codec、Route、业务或 Provider。每个产品的通知实现可以对应裸机 pending flag、
FreeRTOS task notification、Zephyr semaphore 等。

协议任务调用：

```text
ucn_v6_protocol_owner_run(owner, budget, handler, context, &processed)
```

Owner 在五类事件间 round-robin，单次最多处理 `budget` 个事件。Handler 返回成功后才确认出队；
失败则事件保持 pending，供下一次唤醒重试。运行期间第二次 `run()` 返回 STATE，避免两个执行上下文
同时推进协议。任务锁不可失败，因此 `run()` 的所有退出路径都能重新持锁并清除 `running`；不会因
尾部 try-lock 失败留下永久忙状态。总 pending 达到 Manifest 固定深度时返回 NO_SPACE，不覆盖旧事件。

本阶段只冻结 Owner 模型，不声称 Host fake lock 等同真实 RTOS/ISR 锁。V6-13 必须为每个 Port
验证相应的 task/ISR/SMP 临界区与通知实现。

## 5. 分项自审

### 5.1 Identity / Bootstrap

- Authority 和 Bootstrap 的结构体正文不再出现在 `include/`；
- Authority Provider 回调仍受 caller-owned 共享 gate 保护；回调内重新初始化同一对象零写拒绝；
- Binding/Group persist-before-publish 和 Bootstrap 固定 pending、配额、超时语义保持不变；
- Manifest 错配、非对齐 storage 与 magic 损坏均有不写回回归。

### 5.2 Message / Durable Journal

- Operation Allocator 与 Journal 只暴露 handle/view，不允许测试或应用直接改 `next_id/faulted`；
- 初始化中的 load/migration 在临时对象完成，验证成功后才一次发布到 caller storage；
- Provider 失败仍只设置内部 Fence，不修改 committed snapshot；
- 定向测试通过实际耗尽预留区触发下一次持久化，不再直接改私有字段。

### 5.3 Protocol Owner

- task 与 ISR 通知计数分别验证；
- RX 热点与 Timer 事件按 cursor 轮转，不由固定槽 0 长期饥饿其他类别；
- Handler 首次失败时 pending 数量和类别保持；后续成功可继续处理；
- Handler 内递归 `run()` 被拒绝且不改外层结果；
- 深度耗尽零覆盖，magic 损坏后所有操作失败关闭。
- 任务锁采用 acquire-before-return 合同，ISR 独占 try-lock 合同；FreeRTOS Port API 已随破坏性
  合同升级，Host 回归验证 Handler 成功、失败和递归路径都能恢复 `running=false`。

### 5.4 隔离与工具链

- 公共 include 树中不存在五个 opaque 对象的结构体正文；
- 仓库中不存在旧的非 in-place 初始化调用；
- `UCN_BUILD_V6_EXPERIMENTAL=OFF` 的 `libucn_core.a` 不包含 `ucn_v6_*` 符号；
- GCC/MSVC 严格告警、ASan/UBSan 和 GCC Analyzer 均通过定向门禁。

## 6. 验证结果

| 门禁 | 结果 |
| --- | --- |
| Windows GCC Full Debug 全量 | 64/64 |
| Config/Identity/Message/Owner 定向 | 5/5（含两个 Config 合同目标） |
| MSVC 19.29 Release `/W4 /WX` | 5/5 |
| WSL ASan/UBSan | 5/5 |
| WSL `-fanalyzer -Werror` | 5/5 |
| default-OFF archive | `ucn_v6_*` symbols = 0 |
| `git diff --check` | 无空白错误，仅行尾格式提示 |

以上是 Host 软件证明。真实 Flash 双槽/撕裂恢复、真实任务与 ISR 并发、缓存一致性、SMP、各 RTOS
通知延迟和 MCU RAM/栈高水位，分别留给 V6-07、V6-13 和 V6-14。
