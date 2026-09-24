# UCN V6 简化版 IMPL-09-00 能力驱动生产装配合同与 Composition Plan 实施及自审报告

> 状态：`DONE / SELF-REVIEW PASS / EXTERNAL REVIEW HOLD`
>
> 日期：2026-09-24
>
> 边界：本阶段只实现无副作用的生产装配预检，没有初始化私有 Owner，没有对外公开新 API，没有将私有模块链入 `UCN::simplified`。

## 1. 为什么先做 Composition Plan

IMPL-01～08D 已经建立了基础通信、Persistence、Security、Admission、
Capability、Route/Flow、Transport、Service、Realtime、Group 和 Cluster 的私有实现。
如果直接将这些 archive 链入公共 Runtime，会同时引入三个问题：

1. 用户被迫知道 Owner、Wire Contract 和 C/H/O 组合；
2. 可选模块可能在用户没有请求对应能力时仍占用 RAM 或启动 Timer；
3. 一次链接把 Owner 之间的依赖顺序藏在 CMake 或初始化函数里，后续无法审计。

因此本阶段先建立一个纯函数：

```text
available modules + user capability requirements
    -> validate compiled Feature availability
    -> compute minimum selected-module closure
    -> compute durable Owner set
    -> compute deterministic init order
    -> compute fixed private-owner RAM ledger
    -> canonical composition digest
```

它不创建 Owner、不调用 Provider/Driver、不发帧，因此失败只能是输出不写回的预检失败。

## 2. 用户能力与最小模块闭包

| 用户需求 | 最小模块闭包 | 不会隐式引入 |
| --- | --- | --- |
| 静态 C1 单帧 | Foundation | Persistence/Security/Route/可选模块 |
| 受保护 C1 | Foundation + Persistence + Security | Admission/Route/Cluster |
| 动态入网 | Foundation + Persistence + Identity + Security + Admission + Capability | Route/Flow |
| 自动 SoftRoute | Foundation + Route | Security/Persistence；是否允许 O0 仍由 Policy 决定 |
| Advanced Flow | Foundation + Persistence + Security + Capability + Route + Flow | Realtime/Group/Cluster |
| Reliable | Foundation + Transport | Persistence；普通 Reliable 是易失的 |
| Transfer | Foundation + Persistence + Transport | Security/Service |
| 普通 Service | Foundation + Service | Persistence |
| Durable Operation | Foundation + Persistence + Service | Cluster |
| LOCAL_STAMP | Foundation + Realtime | Flow/Persistence |
| Network Time | Foundation + Persistence + Security + Capability + Route + Flow + Realtime | Cluster/Group |
| Static Group | Foundation + Group | Cluster/Persistence |
| Dynamic Group | Foundation + Persistence + Identity + Security + Group | Cluster |
| Cluster Base | Foundation + Persistence + Identity + Security + Capability + Route + Flow + Cluster | Realtime/Group |
| Cluster Group Acceleration | Cluster Base + Group | Realtime |

上表是当前没有等价硬件单调 Witness 时的默认闭包。未来若支持安全元件或硬件计数器，
必须作为新的明确合同引入，不得用一个布尔值临时绕过 Persistence。

## 3. 编译可用性和运行时选择分离

`available_module_mask` 表示当前产品允许使用的模块；
`compiled_module_mask` 由真实 CMake Feature 宏生成，其中 Foundation 还必须受
`UCN_FEATURE_ADAPTER` 约束；`selected_module_mask` 则是根据 capability 计算的最小
运行集合。三者不得合并：

- available 中出现未编译模块：`UCN_ERR_UNSUPPORTED`；
- capability 需要不在 available 中的模块：`UCN_ERR_CONFIG`；
- available 但未被 capability 选中：不初始化、不计入 Owner RAM、不创建 Timer。
- Adapter OFF 时 Foundation 不可用，最小 Static C1 计划也必须零写拒绝，不能生成
  一个实际上没有物理提交路径的虚假计划。

这使默认 API 可以围绕“是否要安全/可靠/实时/群发/簇”表达，而不是让用户选择
Security Owner 或 Flow Owner。

## 4. 确定性启动顺序

Plan 中的模块顺序固定为：

```text
Persistence -> Identity -> Security -> Admission -> Capability
-> Route -> Flow -> Transport -> Service -> Realtime -> Group -> Cluster
```

这只是 IMPL-09-01/02 的装配输入，不代表本阶段已经执行初始化。
Adapter 必须等 durable reload 和必要 Owner 发布完成后再启动，具体事务化退出顺序属于
IMPL-09-02。

## 5. 固定资源账本

`private_owner_bytes` 使用当前 Profile 下真实 `sizeof(Owner)` 相加，并包含
Coordinator。它不包含现有公共 Runtime/Adapter Storage、Provider 私有内存、Driver DMA、
RTOS 任务栈或中断栈，因此只是“新私有 Owner 增量 RAM”，不是 MCU 总 RAM 结论。

| Profile | 只有 Static C1/Coordinator | 所有当前能力全开的私有 Owner 增量 |
| --- | ---: | ---: |
| Nano | 840 B | 28,104 B |
| Lite | 840 B | 83,624 B |
| Full | 840 B | 243,592 B |

这组数字也证明不能将所有模块默认初始化：最小 Static C1 路径只承担 840 B
Coordinator，而所有管理面和可选模块全开时必须由产品显式接受对应 RAM 成本。

Plan 还绑定 Profile、available/compiled/selected/durable masks、capabilities、启动顺序和
资源字节数，以 FNV-1a-64 生成 `composition_digest`。Digest 是配置一致性证据，
不是安全签名，也不取代 canonical 字段精确比较。

## 6. 实施变更

- 新增 `src/internal/ucn_composition.h`；
- 新增 `src/runtime/ucn_composition.c`；
- 新增私有 `ucn_v6s_composition` target，不安装、不导出、不进入公共 umbrella；
- 新增 `tests/simplified/test_composition.c`；
- 边界检查器登记第 51 个源文件和第 22 个内部头；
- 将 Capability Resolver 的内部类型重命名为
  `ucn_i_resolve_dependency_kind_t`，与 Coordinator routing kind 解除同名冲突。

## 7. 对抗测试

定向测试覆盖：

1. 最小 Static C1 只选 Foundation；
2. 14 类高级能力各自的最小模块闭包；
3. 从每个闭包中删除任一必要模块时失败且输出哨兵不变；
4. Adapter/Persistence/Realtime/Group/Cluster Feature OFF 下不得声称对应模块可用；
5. Cluster Base 不选 Group/Realtime，Static Group 不选 Cluster/Realtime；
6. Plan 重建逐字节相同，任一 digest/顺序/资源字节篡改都使自验失败；
7. input/output 内存别名在首次写入前拒绝。

## 8. 四轮自审

第一轮按用户能力→模块闭包正向回读，确认 LOCAL_STAMP、Static Group、
Cluster Base 没有被错误绑到无关模块。

第二轮按 Feature OFF、缺依赖、错 Profile、保留位、别名、篡改和资源溢出反向回读，
确认所有失败位于 Owner 初始化和 Provider/Driver 调用之前。

第三轮按用户视角回读，发现初稿仍让上层选模块，随后改为
`available modules + capabilities -> minimum selected modules`。同时组合编译首次暴露
Capability/Coordinator 的同名 typedef，已以独立语义命名域修复，而不是依赖 include 顺序。

第四轮按真实 Feature 物理边界回读，发现 Foundation 初稿没有受 Adapter 编译开关约束；
现已把 Adapter 纳入 compiled mask，并增加 Adapter OFF 的零写拒绝回归。该轮同时把
digest 的四字节遍历改为显式固定循环，避免依赖无符号回绕形成难审计控制流。

## 9. 验证结果

| 门禁 | 结果 |
| --- | --- |
| Windows GCC Full | `102/102 PASS` |
| Windows GCC Lite | `102/102 PASS` |
| Windows GCC Nano | `102/102 PASS` |
| Nano，Persistence/Realtime/Group/Cluster OFF | `67/67 PASS` |
| Nano，Adapter/Persistence/Realtime/Group/Cluster OFF | `65/65 PASS` |
| Composition 定向五配置 | `5/5 PASS` |
| 实现边界检查 | `sources=51 / internal_headers=22 PASS` |
| Nano Composition 最大单函数栈 | `144 B` |
| `git diff --check` | 无空白错误，仅既有换行提示 |

上述结果是 Host 软件证据，不等于 MCU 链接、静态总 RAM、RTOS 任务栈、Driver DMA、
物理 Bearer 或实机时序证据。

## 10. 当前结论

```text
IMPL-09-00                 = DONE / SELF-REVIEW PASS
Composition Plan          = PRIVATE PURE PREFLIGHT IMPLEMENTED
Owner/Runtime wiring      = NOT STARTED
Public API                = UNCHANGED
Production / MCU release  = NOT CLAIMED
```

下一项是 IMPL-09-01：使用 Plan 预留固定 Composition Storage，建立具体 Owner
实例注册和 Coordinator typed adapter。该阶段仍先保持私有，直到生命周期、底层 Wire
和 Feature OFF 门禁同时成立，才允许进入公共发布面。
