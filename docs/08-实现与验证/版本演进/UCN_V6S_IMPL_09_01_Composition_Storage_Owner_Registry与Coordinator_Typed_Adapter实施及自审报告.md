# UCN V6 简化版 IMPL-09-01 Composition Storage、Owner Registry 与 Coordinator Typed Adapter 实施及自审报告

> 状态：`DONE / SELF-REVIEW PASS / EXTERNAL REVIEW HOLD`
>
> 日期：2026-09-24
>
> 边界：本阶段建立私有 `PREPARED` 装配容器，不初始化业务 Owner，不执行
> Persistence Provider I/O，不启动 Adapter/Driver，不改变公共 API 或生产 Wire。

## 1. 本阶段解决什么问题

IMPL-09-00 已能把用户能力要求转换为确定性的最小模块闭包，但 Plan 只回答“需要什么”，
还没有回答以下运行时问题：

1. 所有私有 Owner 放在哪一块固定内存中；
2. 一个 Handle/Event 属于哪次 Runtime 生命周期、哪个 Owner 和哪个 generation；
3. 跨 Owner 依赖如何保证只经过 Coordinator；
4. 初始化中途失败时，如何避免向外暴露半初始化 Owner。

IMPL-09-01 因此只实现一个无业务副作用的 `PREPARED` 阶段：

```text
validated Composition Plan
    -> calculate exact caller-owned storage bytes
    -> validate exact size/alignment/zero state/alias rules
    -> register Foundation and Coordinator
    -> reserve selected Owner regions in deterministic order
    -> initialize Coordinator with fail-closed event sink
    -> publish Runtime magic last
    -> PREPARED
```

任何业务 Owner Storage 在此阶段都必须保持全零；只要提前写入一个字节，完整 Runtime
校验就失败。真正的 Owner 初始化、reload、publish 和 Adapter start 属于 IMPL-09-02。

## 2. 固定 Storage 布局

Storage 由调用方提供，要求：

- 地址按 8 B 对齐；
- 容量必须与 `ucn_i_composition_storage_required(plan)` 返回值精确相等；
- 首次 prepare 前整个有效区域逐字节为零；
- 不能与 Plan、Config、输出指针或 Coordinator lock context 重叠；
- 不使用 malloc/calloc/realloc/free/alloca。

布局固定为：

```text
+-----------------------------------------+
| Composition Runtime header             |
| - exact Plan                            |
| - 14-entry Owner Registry               |
| - embedded Coordinator                  |
+-----------------------------------------+
| alignment padding                       |
+-----------------------------------------+
| selected Owner region 0 (all zero)      |
+-----------------------------------------+
| selected Owner region 1 (all zero)      |
+-----------------------------------------+
| ... deterministic Plan init order       |
+-----------------------------------------+
```

当前全能力 Host 对象的精确 Composition Storage 为：

| Profile | IMPL-09-00 私有 Owner 合计 | IMPL-09-01 精确 Storage | 装配元数据增量 |
| --- | ---: | ---: | ---: |
| Nano | 28,104 B | 28,584 B | 480 B |
| Lite | 83,624 B | 84,104 B | 480 B |
| Full | 243,592 B | 244,072 B | 480 B |

最小 Static C1 的私有 Owner 合计为 840 B，精确 Composition Storage 为 1,320 B。
这些是 Host 当前 ABI 下的静态对象数字，不是目标 MCU 链接 RAM、任务栈、DMA 或堆栈水位结论。

## 3. Owner Registry

Registry 固定为 14 槽，每个槽包含：

```text
runtime_instance
owner_instance
generation
module_id
module_mask
storage_offset
storage_bytes
state
```

登记顺序唯一：

1. Foundation：`REGISTERED`，不单独占私有 Owner 区；
2. Coordinator：`ACTIVE`，位于 Runtime header 内；
3. Plan 的 `init_order[]`：全部为 `RESERVED`，Storage 必须全零。

Owner instance 使用 `owner_instance_base + module_id`；generation 在本 Runtime 生命周期初始为
1。`runtime_instance` 必须由调用方从不回绕、不复用的本地高水位分配。任何旧引用即使拥有相同
module ID，也不能跨 Runtime 生命周期重新获得授权。

完整校验会重建 Plan 和 Storage 布局，再逐项核对 Registry 的顺序、Ref、offset、size、state、
保留位、未用槽全零以及每个 RESERVED Owner 区全零，不把 digest 当作精确相等比较的替代品。

## 4. Coordinator Typed Adapter

typed adapter 是一条依赖边的不可变描述：

```text
composition_digest
requester { runtime, owner, generation, module }
target    { runtime, owner, generation, module }
dependency_kind
```

Dependency kind 到目标 Owner 的映射固定为：

| Dependency | Target Owner |
| --- | --- |
| Identity Binding | Identity |
| Security Session | Security |
| Capability Refresh | Capability |
| Soft Route | Route |
| Flow | Flow |
| Transfer | Transport |
| Group | Group |
| Time Domain | Realtime |
| Persistence | Persistence |

adapter 不含函数指针，不持有目标 Owner context，不能绕过 Coordinator。构建时必须在当前
Registry 中精确找到 requester 和 target；缺少目标模块返回 `UNSUPPORTED`，旧 generation、
错 runtime 或篡改 digest 失败且输出不写回。

`PREPARED` 阶段业务 Owner 仍为 RESERVED，因此 requirement/event route 必须返回状态错误，
且 Handle 输出不写回。这一门禁防止“先拿到 adapter，再绕过 reload/publish 提前发起副作用”。

## 5. 失败原子性

prepare 的全部可失败检查都位于首次写 Storage 之前。只有预检全部通过后才按确定顺序写入：

1. Runtime header（magic 仍为零）；
2. Foundation/Coordinator/RESERVED Owner 记录；
3. 初始化 Coordinator；
4. 最后写 Runtime magic；
5. 最后写 `runtime_out`。

若 Coordinator 初始化出现意外失败，已触及的精确 Storage 会整体清零，输出指针保持原哨兵。
destroy 则先要求完整 Runtime 有效，再由 Coordinator 完成可拒绝的销毁，最后清零整块 Storage。

## 6. 代码和门禁变更

- 扩展 `src/internal/ucn_composition.h`：固定 Registry、Runtime、Owner Ref/View、prepare
  config 与 typed adapter 私有合同；
- 扩展 `src/runtime/ucn_composition.c`：实现精确 Storage、prepare、完整验证、Owner View、
  typed adapter、PREPARED route gate 和 destroy；
- `ucn_v6s_composition` 只新增对 `ucn_coordinator` 的私有依赖，不进入安装导出和公共 umbrella；
- 新增 `tests/simplified/test_composition_storage.c`；
- 边界检查器明确 Composition 只能依赖 Common + Coordinator，其他业务模块继续只依赖 Common。

## 7. 对抗测试

定向测试覆盖：

1. 最小 Plan 的精确 Storage、Foundation/Coordinator 记录和 destroy 全清零；
2. size - 1、size + 1、错对齐、非零 Storage、非法 Runtime ID 的首次写入前拒绝；
3. 同一 Storage 重复 prepare 拒绝，原 Runtime 仍完整有效；
4. Registry generation 篡改、RESERVED Owner 区单字节污染使 Runtime 立即无效；
5. 缺失模块的 typed adapter 返回 `UNSUPPORTED` 且输出哨兵不变；
6. requester runtime/generation 篡改拒绝；
7. PREPARED 阶段 route requirement 失败且 Handle 哨兵不变；
8. Full/Lite/Nano 全能力下 9 类 dependency 的目标 Owner 映射逐项验证；
9. Persistence/Realtime/Group/Cluster OFF 和 Adapter OFF 保持有界行为。

## 8. 分项与全体自审

第一轮按 Storage 生命周期回读，发现“容量至少 required”会给同一调用方区域留下未归属尾部，
已改为精确容量合同并加入 `required + 1` 反例。

第二轮按身份域回读，确认每个 Owner Ref 同时绑定 Runtime、Owner instance、generation 和
module ID；typed adapter 同时绑定 composition digest 与 dependency kind，不允许仅凭 digest
或 module ID 复用。

第三轮按跨模块依赖回读，确认详细业务模块没有新增 Coordinator include/call；只有 Composition
和既有 Persistence Coordinator adapter 可以直接依赖 Coordinator，业务 Owner 之间仍无直连。

第四轮按启动副作用回读，确认本阶段没有调用任何业务 Owner init、Provider、Driver、Timer、
Wire encode/decode 或 callback；Coordinator sink 在 IMPL-09-02 接入正式分派前固定失败关闭。

第五轮按 Profile/Feature/编译器回读，关闭 MSVC 常量条件告警，并验证 Adapter OFF 时测试本身
不留下 unused 对象或虚假 Foundation。

## 9. 验证结果

| 门禁 | 结果 |
| --- | --- |
| Windows GCC Full | `103/103 PASS` |
| Windows GCC Lite | `103/103 PASS` |
| Windows GCC Nano | `103/103 PASS` |
| Nano，Persistence/Realtime/Group/Cluster OFF | `68/68 PASS` |
| Nano，Adapter/Persistence/Realtime/Group/Cluster OFF | `66/66 PASS` |
| MSVC 19.29 Release 定向 | Composition Storage `1/1 PASS`，`/W4 /WX` |
| WSL ASan/UBSan 定向 | Composition Plan/Storage `2/2 PASS` |
| WSL `-fanalyzer -Werror` 定向 | Composition Plan/Storage `2/2 PASS` |
| 边界检查 | `sources=51 / internal_headers=22 PASS` |
| Nano Composition 最大单函数栈 | `160 B` |
| `git diff --check` | 无空白错误；仅既有换行提示 |

## 10. 当前结论

```text
IMPL-09-01                = DONE / SELF-REVIEW PASS
Composition Runtime      = PRIVATE PREPARED CONTAINER IMPLEMENTED
Business Owner init      = NOT STARTED
Durable reload/publish   = NOT STARTED
Adapter/Driver start     = NOT STARTED
Public API               = UNCHANGED
Production / MCU release = NOT CLAIMED
```

下一项是 IMPL-09-02：在同一固定 Storage 和 Registry 上实现事务化生命周期，按
`init → durable reload → Owner publish → Adapter start` 正向推进，并按相反顺序完成
stop/quiescence。任何 durable Owner 在 reload proof 完成前都不能从 RESERVED 进入 ACTIVE。
