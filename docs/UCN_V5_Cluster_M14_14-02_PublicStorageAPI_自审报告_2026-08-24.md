# UCN V5 Cluster M14 / 14-02 Public Storage API 自审报告

> 日期：2026-08-24  
> 状态：`CODE COMPLETE / SELF-AUDIT PASS / WAIT M14 EXTERNAL REVIEW`

## 1. API v2 边界

- `ucn_cluster.h` 只声明 opaque `ucn_cluster_t` 和公共配置、只读 View、操作 API；不再包含运行时 member、backup、epoch、persistence 或 Authority storage。
- 唯一 Protocol Owner 翻译单元包含 `ucn_cluster_storage.h`，静态分配 `ucn_cluster_t`；仍无堆、无动态内存、无运行期尺寸查询。
- `UCN_CLUSTER_API_VERSION=2`，`UCN_CLUSTER_STORAGE_LAYOUT_VERSION=2`。这是源码/ABI 破坏性升级，所有 Cluster 用户必须全量重编译；Wire 版本和字节不变。
- 应用、Adapter、Federation 声明只持有 `ucn_cluster_t *`；状态读取使用 `get_view/get_phase/get_role/active_epoch_get/member summary`。

## 2. 门禁与测试

- `test_public_headers.c` 不包含 storage 头，证明 pointer-only API 可独立编译。
- `test_cluster_storage_header.c` 模拟唯一 Owner，证明静态分配及 API/layout 版本。
- 新增 `check_cluster_storage_boundary.py`：拒绝公共 struct 定义、公共头传递包含 storage、版本遗漏和 Owner allocation proof 缺失。
- 自审中发现 `ucn_cluster_sim` 属于 `EXCLUDE_FROM_ALL`，仍残留已删除的 `.role` 字段读取；已改为 `ucn_cluster_get_role()`，并让 Phase source gate 同时扫描 tools，避免普通构建假绿。

## 3. 验证

| 配置 | 结果 |
|---|---:|
| GCC Full | 22/22 |
| GCC Lite | 20/20 |
| GCC Nano | 20/20 |
| GCC Full / Service OFF | 20/20 |
| MSVC Release（含显式构建 Cluster sim） | 40/40 |
| Public/storage source gate | PASS |
| Phase/no-wrap source gate | PASS |

MSVC 保留仓库既有 C4819 代码页警告。`git diff --check` 无空白错误，仅既有 CRLF 提示。

## 4. 迁移摘要

旧代码：

```c
#include "ucn/ucn_cluster.h"
ucn_cluster_t cluster;
```

新 Owner：

```c
#include "ucn/ucn_cluster_storage.h"
static ucn_cluster_t cluster;
```

非 Owner 继续只包含 `ucn_cluster.h` 并传递指针。禁止跨任务读写 storage 字段。

## 5. 结论

14-02 软件范围自审通过。该拆分没有启用生产 v4，也没有解除 M05 `AUDIT HOLD`；进入 14-03。
