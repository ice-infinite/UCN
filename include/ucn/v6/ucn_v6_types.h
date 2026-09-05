#ifndef UCN_V6_TYPES_H
#define UCN_V6_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
