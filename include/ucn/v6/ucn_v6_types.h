#ifndef UCN_V6_TYPES_H
#define UCN_V6_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* EN: Canonical 16-bit identifier domains. Zero means absent/unbound and
 * UINT16_MAX is reserved as the invalid/exhausted sentinel. Keeping the
 * usable upper bound here prevents Wire, Adapter and higher owners from
 * accepting identifiers that another v6 layer can never consume.
 * 中文：16 位标识符的统一合法域。0 表示缺省/未绑定，UINT16_MAX 保留为
 * 无效/耗尽哨兵。统一在基础类型中声明可用上限，防止 Wire、Adapter 与上层
 * Owner 对同一标识符给出互相矛盾的准入结论。 */
#define UCN_V6_ENDPOINT_ID_MAX UINT16_C(65534)
#define UCN_V6_LINK_ID_MAX UINT16_C(65534)
#define UCN_V6_PATH_ID_MAX UINT16_C(65534)

/* EN: Canonical cumulative path-hop domain shared by Capability, Route and
 * Metric. Zero means no valid path and UINT16_MAX is a reserved invalid /
 * exhausted sentinel; neither value may become an installed path cost.
 * 中文：Capability、Route 与 Metric 共享的规范累计跳数域。0 表示无有效
 * 路径，UINT16_MAX 保留为无效/耗尽哨兵，二者均不得成为已安装路径代价。 */
#define UCN_V6_HOP_COUNT_MAX UINT16_C(65534)

/* EN: Dependency-free public result codes shared by Config and every v6
 * module. Keeping this type outside Identity breaks the former Config <-
 * Identity <- Config include cycle, so profile capacity macros are resolved
 * identically in library and application translation units.
 * 中文：Config 与所有 v6 模块共享的无依赖公共结果码。把该类型从 Identity
 * 中拆出，可消除原来的 Config <- Identity <- Config 循环包含，确保库与应用
 * 翻译单元使用完全相同的 Profile 容量宏。 */
typedef enum ucn_v6_result {
    UCN_V6_OK = 0,
    UCN_V6_ERR_ARGUMENT = -1,
    UCN_V6_ERR_CONFIG = -2,
    UCN_V6_ERR_NO_SPACE = -3,
    UCN_V6_ERR_MALFORMED = -4,
    UCN_V6_ERR_SECURITY = -5,
    UCN_V6_ERR_REPLAY = -6,
    UCN_V6_ERR_ACCESS = -7,
    UCN_V6_ERR_STATE = -8,
    UCN_V6_ERR_EXHAUSTED = -9,
    UCN_V6_ERR_NOT_FOUND = -10,
    UCN_V6_ERR_TIMEOUT = -11,
    UCN_V6_ERR_CANCELLED = -12
} ucn_v6_result_t;

#endif
