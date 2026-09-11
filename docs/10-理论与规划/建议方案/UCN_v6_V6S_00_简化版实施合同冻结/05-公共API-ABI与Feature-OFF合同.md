# V6S-00-05 公共 API、ABI 与 Feature OFF 合同

> 状态：`DONE / SELF-REVIEW PASS / EXTERNAL REVIEW REQUIRED`
>
> 本文冻结简化版的用户入口、静态存储、错误语义、生命周期和发布边界；不表示这些名称已经由当前源码实现。

## 1. 冻结目标

普通用户只表达“发给谁、做什么、需要何种结果”，不得选择 C0～C5、O0～O2、H0～H3、
Route Candidate、Security Session 或 Persistence Operation。产品集成者配置能力、容量和
Port；Driver 只发布事实；模块 Owner 只接受 Coordinator 路由的 typed requirement。

四个边界如下：

| 边界 | 调用者 | 稳定输入 | 禁止暴露 |
| --- | --- | --- | --- |
| Public Intent API | 应用 | Node、Endpoint、Target、Payload、Options | Wire Contract、Route/Key/FSM |
| Product Setup API | 产品集成层 | Storage、Manifest、Policy、Port/Provider | 可写 Owner 字段 |
| Driver Fact API | Driver/ISR | RX/TX/Link/时间戳事实 | 解码、ACL、路由决策 |
| Internal Owner SPI | Coordinator | canonical requirement/event/handle | 跨 Owner 直调、裸 Provider/Adapter |

## 2. 头文件与命名空间

最终公共发布面分为：

```text
include/ucn/ucn.h                 始终安装的最小用户 API
include/ucn/ucn_config.h          生成后的 Composition/Profile/Storage 常量
include/ucn/ucn_product.h         产品 Setup、Manifest、Port/Provider 合同
include/ucn/features/*.h          仅安装已编译 Feature 的产品配置与 View
```

`ucn.h` 不包含模块 private 类型，不要求应用 include Wire/Security/Route 头。内部 SPI 放在
`src/internal/`，不得安装。所有公共符号使用 `ucn_`；内部符号使用 `ucn_i_`，模块 private
符号必须为 `static` 或通过 hidden visibility 隐藏。

## 3. 稳定结果码

结果对象冻结为精确的 `int32_t`；C99 的 `enum` 宽度由实现决定，且 `-fshort-enums` 会改变 ABI，
所以 enum 只能提供常量，不能作为函数返回类型或公共结构字段类型：

```c
typedef int32_t ucn_result_t;

enum {
    UCN_OK              =  0,
    UCN_ERR_ARGUMENT    = -1,
    UCN_ERR_CONFIG      = -2,
    UCN_ERR_NO_SPACE    = -3,
    UCN_ERR_MALFORMED   = -4,
    UCN_ERR_SECURITY    = -5,
    UCN_ERR_REPLAY      = -6,
    UCN_ERR_ACCESS      = -7,
    UCN_ERR_STATE       = -8,
    UCN_ERR_EXHAUSTED   = -9,
    UCN_ERR_NOT_FOUND   = -10,
    UCN_ERR_TIMEOUT     = -11,
    UCN_ERR_CANCELLED   = -12,
    UCN_ERR_POLICY      = -13,
    UCN_ERR_UNSUPPORTED = -14,
    UCN_ERR_IN_DOUBT    = -15
};

UCN_STATIC_ASSERT(sizeof(ucn_result_t) == 4U, result_must_be_i32);
UCN_STATIC_ASSERT(UCN_ERR_IN_DOUBT == -15, result_values_must_not_drift);
```

所有公共函数、回调和结构字段统一使用 `ucn_result_t`，不得暴露匿名/具名 enum 对象。C/C++
安装 consumer 必须分别在默认编译参数和等价 `-fshort-enums` 配置下证明
`sizeof(ucn_result_t)==4` 且常量值一致。

同步 `UCN_OK` 只表示输入已被本地原子接受。它不表示已提交物理链路、已到达远端、已执行业务
或已经持久化；这些状态只能从 Completion View 读取。错误发生时，所有输出参数和调用者
Payload 必须逐字节不变。

## 4. 公共结构 ABI 规则

所有可扩展输入/输出结构以以下字段开头：

```c
uint16_t struct_size;
uint16_t api_version;
```

规则：

1. `api_version` 必须精确等于本构建的 `UCN_API_VERSION`；不做旧版本兼容解析。
2. `struct_size` 必须精确等于本版本类型大小；简化版 1.0 不接受前缀兼容。
3. 所有 `reserved_zero`、未使用 bit 和 padding 构造前清零，入口逐项验证。
4. 公共结构不得包含 `bool`、编译器位域、`enum` 存储字段、`long`、函数局部结构指针或
   private Owner 指针；线上/ABI 字段使用 `uint*_t`。
5. 公共结构的 `sizeof`、`offsetof` 和对齐由 C99 compile-contract target 在 GCC/Clang/MSVC
   分别断言；不得使用 `#pragma pack` 掩盖布局差异。
6. Callback vtable 也执行 exact size/version，尾部不能被忽略。

## 5. Opaque Storage

C99 文件作用域不能用运行时函数返回值声明数组，因此每个已装配 Owner 都必须由生成配置给出
整数常量表达式：

```c
#define UCN_STORAGE_BYTES             ...
#define UCN_STORAGE_ALIGNMENT         ...
#define UCN_DECLARE_STORAGE(name_)    ...
#define UCN_COMPILED_MANIFEST_HASH    UINT64_C(...)
#define UCN_STORAGE_LAYOUT            UINT16_C(...)

UCN_DECLARE_STORAGE(app_ucn_storage);
```

`UCN_DECLARE_STORAGE` 是唯一受支持的可移植声明形式。生成器必须为当前编译器提供满足对齐的
union/attribute/declspec 实现；不支持所需对齐的编译器在编译期失败。不得要求用户手写
`uint8_t bytes[N]` 后猜测对齐。

运行时 `ucn_storage_required()` 只用于诊断和交叉核对，不能替代编译期常量。`ucn_init()` 在
首次写 Storage 前按顺序验证：指针、容量、地址对齐、API Version、Storage Layout、完整
Manifest Hash、Port/Provider vtable、所有容量公式。任一失败时 Storage 与 `out_node` 都不写。

Opaque 只保证合法应用不依赖内部布局；它不能物理阻止恶意调用者 `memset`。每次公共入口仍需
校验 magic/layout/runtime generation/fence，损坏时失败关闭。

## 6. Handle

统一 Handle 的规范字段为：

```c
typedef struct ucn_handle {
    uint32_t runtime_instance;
    uint16_t owner_instance;
    uint16_t slot;
    uint16_t generation;
    uint8_t  object_kind;
    uint8_t  reserved_zero;
} ucn_handle_t;
```

`sizeof(ucn_handle_t)==12`，自然对齐不大于 4。全零 Handle 无效。验证顺序固定为
`reserved/kind → runtime → owner → slot → generation → fence/lifecycle`。Handle 代际不回绕；
到顶后对应槽/Owner Fault，不能因 remove/reset 复用旧值。Handle 不是 Authority，不得保存裸
指针或作为网络身份发送。

## 7. 最小公共函数面

### 7.1 生命周期

```c
ucn_result_t ucn_init(void *storage, size_t storage_bytes,
                      const ucn_config_t *config,
                      const ucn_ports_t *ports,
                      ucn_node_t **out_node);
ucn_result_t ucn_start(ucn_node_t *node, uint64_t now_us);
ucn_result_t ucn_step(ucn_node_t *node, uint64_t now_us,
                      const ucn_step_budget_t *budget,
                      ucn_step_result_t *out_result);
ucn_result_t ucn_stop(ucn_node_t *node);
ucn_result_t ucn_deinit(ucn_node_t *node);
```

生命周期为：

```text
UNINITIALIZED -> INITIALIZED -> STARTING -> RUNNING
RUNNING -> STOPPING -> QUIESCENT -> UNINITIALIZED
                         \-> LOCAL_FAULT -> STOPPING/diagnostic
```

`start/stop/deinit` 不能在 Driver、Provider、Endpoint callback 或 `ucn_step()` 动态范围内调用。
`ucn_step()` 单次最多消费调用方给定预算；零预算、溢出时间和倒退的单调时间失败关闭。

### 7.2 Endpoint 与静态 Path

```c
ucn_result_t ucn_endpoint_add(ucn_node_t *, const ucn_endpoint_config_t *,
                              ucn_endpoint_handle_t *);
ucn_result_t ucn_endpoint_remove(ucn_node_t *, ucn_endpoint_handle_t);
ucn_result_t ucn_static_path_add(ucn_node_t *, const ucn_static_path_t *,
                                ucn_path_handle_t *);
ucn_result_t ucn_static_path_remove(ucn_node_t *, ucn_path_handle_t);
```

Endpoint ID/Service/Opcode ACL 属于配置；用户 callback 只接收已经完成 Wire、Security、ACL 和
本地目标裁决的数据。静态 Path 仍由 Route Owner 持有，应用获得的仅是稳定 Handle。

### 7.3 统一意图发送面

```c
ucn_result_t ucn_publish(ucn_node_t *, const ucn_target_t *,
                         const void *, size_t,
                         const ucn_send_options_t *, ucn_send_handle_t *);
ucn_result_t ucn_request(ucn_node_t *, const ucn_target_t *,
                         const void *, size_t,
                         const ucn_send_options_t *, ucn_send_handle_t *);
ucn_result_t ucn_respond(ucn_node_t *, ucn_request_context_t,
                         ucn_result_kind_t, const void *, size_t,
                         const ucn_send_options_t *, ucn_send_handle_t *);
ucn_result_t ucn_publish_group(ucn_node_t *, ucn_group_handle_t, uint16_t,
                               const void *, size_t,
                               const ucn_send_options_t *, ucn_send_handle_t *);
```

四个函数必须进入同一个内部 `submit_intent()`，不得分别实现路由、安全或分片选择。
`ucn_send_options_t` 只表达用户需求：可靠性、交互角色、实时性、可信/加密、群发、指定 Path、
Payload ownership 和 Completion；不含 Wire Contract 选择字段。

只有 `ONE_WAY + BEST_EFFORT + COPY + 单帧 + 无 operation_id + 无 callback + out_handle==NULL`
且无需高级 Feature 时，Resolver 可选未跟踪最小路径。其他情况必须在返回成功前原子预留
Request/Receipt/Buffer obligation 并返回 Handle；资源不足不得降级语义。

### 7.4 查询、取消与释放

```c
ucn_result_t ucn_send_query(const ucn_node_t *, ucn_send_handle_t,
                            ucn_send_view_t *);
ucn_result_t ucn_send_cancel(ucn_node_t *, ucn_send_handle_t);
ucn_result_t ucn_send_forget(ucn_node_t *, ucn_send_handle_t);
ucn_result_t ucn_buffer_release(ucn_node_t *, ucn_buffer_claim_t);
ucn_result_t ucn_get_stats(const ucn_node_t *, ucn_stats_t *);
ucn_result_t ucn_plan_preview(const ucn_node_t *, const ucn_target_t *,
                              size_t, const ucn_send_options_t *,
                              ucn_plan_view_t *);
```

Completion 必须分开表示 local admission、link submit、remote acceptance、application result、
execution unknown、buffer release 与 receipt retirement。`cancel` 不证明远端没收到；物理提交
不确定时结果只能是 `IN_DOUBT`。诊断 View 不是可执行 Authority。

## 8. Driver Fact 与 Port 回调

Driver 只允许：

```c
ucn_driver_rx_publish(... exact_link, bytes, length, rx_meta ...);
ucn_driver_tx_complete(... exact_token, result, tx_meta ...);
ucn_driver_link_event(... exact_link, event, event_meta ...);
```

Driver 不解码 Wire、不做 ACL、不建立 Session、不调 Endpoint。Port `submit/cancel` 必须非阻塞，
每次 callback 前 Runtime 已发布 exact continuation/token。同步早到 completion 进入预留 latch，
返回后在同一合并点处理。

## 9. Callback、并发与重入

| 动态范围 | 允许调用 | 必须拒绝 |
| --- | --- | --- |
| Driver callback | 专用 ISR-safe fact/latch | 生命周期、发送、重开 Adapter、Provider I/O |
| Provider callback | 对当前 token 的同步 completion latch | init、再次 submit/poll/load、业务发送 |
| Endpoint callback | 只读 View、显式延迟投递 API | stop/deinit/reopen、直接 Owner/SPI |
| Owner step | 当前 Coordinator 路由的一个 work item | 第二 task/ISR 并发写同 Owner |

共享 Gate 必须由调用方存储持有，并由目标平台提供 task/ISR/SMP 安全原子或临界区；普通静态指针
或 `volatile` 不构成同步。失败路径不得消耗 token、推进 generation、写统计或触发 callback。

## 10. Feature OFF

Composition 与 Profile 正交。Feature OFF 的冻结行为：

1. 对应源码不编译进任何 production archive，`nm` denylist 中符号数必须为零。
2. 对应产品配置头不安装，umbrella header 不 include；应用若显式 include，编译期找不到或由
   生成配置明确 `#error`，不能延迟到链接失败。
3. `UCN_STORAGE_BYTES`、Manifest Hash 和 Layout 不包含该模块 Owner、队列或业务 Record Body。
4. 稳定 Public Intent API 可以存在；请求依赖未编译 Feature 时在任何预留/写入前返回
   `UCN_ERR_UNSUPPORTED`。不得通过 stub 返回成功或暗中降级安全/可靠/实时语义。
5. Core、Identity、基础 Wire、基础 Queue 和静态 Path 是最小 Composition；Realtime、Group、
   Cluster、Dynamic Admission、Advanced Route、Reliable Transfer 等按依赖矩阵独立裁剪。

## 11. ABI 与负向验收

每个公共入口至少验证：NULL、未对齐、size/version 错、负枚举、保留位、stale/wrong-kind Handle、
完整/部分 buffer overlap、callback 重入、Feature OFF、容量边界和输出哨兵。ABI target 还必须：

- 在 GCC/Clang/MSVC 对公共 `sizeof/offsetof/alignment` 做编译期断言；
- 以 C 和 C++ consumer 分别 include/install/link/run；
- 直接执行 consumer 程序并检查退出码，不以空 CTest 代替；
- 对 Nano/Lite/Full 和 Feature OFF 安装树执行 header/symbol/storage denylist；
- 确认公共头没有 private include、可变长度数组或动态内存依赖。

## 12. 本项自审

- C99 静态分配不依赖运行时 `storage_required()`；
- 用户发送面只有四个意图包装，并统一进入 Resolver/Coordinator；
- 错误值、Handle 验证、生命周期、回调与失败不写回均有唯一解释；
- Feature OFF 同时覆盖编译、安装、符号、Storage 与运行时意图拒绝；
- 本文没有宣称当前完整 V6 API 已实现，也没有放行 `IMPL-00`。

结论：`V6S-00-05 = DONE / SELF-REVIEW PASS / EXTERNAL REVIEW REQUIRED`。
