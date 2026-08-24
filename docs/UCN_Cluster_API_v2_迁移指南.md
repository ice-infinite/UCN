# UCN Cluster API / Storage v2 迁移指南

## 破坏性变化

- `UCN_CLUSTER_API_VERSION = 2`；
- `UCN_CLUSTER_STORAGE_LAYOUT_VERSION = 2`；
- `ucn_cluster_t` 变为 opaque，应用不得 `sizeof` 或读写字段；
- 所有调用方必须全量重编译。

## 旧写法

```c
ucn_cluster_t cluster;
if (cluster.role == UCN_CLUSTER_ROLE_HEAD) { /* ... */ }
```

## 新写法

```c
#include "ucn/ucn_cluster.h"
#include "ucn/ucn_cluster_storage.h" /* 仅唯一 Owner */

static ucn_cluster_t cluster_storage;
ucn_cluster_t *cluster = &cluster_storage;

ucn_cluster_view_t view;
if (ucn_cluster_get_view(cluster, &view) == UCN_OK &&
    view.role == UCN_CLUSTER_ROLE_HEAD) {
    /* read-only application decision */
}
```

Pointer-only 模块只包含 `ucn_cluster.h`；只有唯一 Protocol Owner 可以包含 storage header。不要缓存内部指针，不要依赖 object layout。

## Wire 与介质不是同一版本

- API/storage v2：C 源码与 ABI 边界；
- Cluster Wire v3/v4：网络控制载荷；
- Persistence schema v4：介质格式。

三者必须分别迁移，不能因为 API v2 就自动启用 Wire v4，也不能把 Record schema v4 当成 production v4 RX 已放行。
