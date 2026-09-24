#ifndef UCN_TYPES_H
#define UCN_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* EN: Public results have an exact 32-bit ABI. Enum constants provide names
 * without making compiler-selected enum width part of any function or DTO.
 * 中文：公共结果码固定为 32 位 ABI。枚举只提供常量名称，编译器自行选择的
 * enum 宽度不会进入函数签名或数据对象。 */
typedef int32_t ucn_result_t;

enum {
    UCN_OK = 0,
    UCN_ERR_ARGUMENT = -1,
    UCN_ERR_CONFIG = -2,
    UCN_ERR_NO_SPACE = -3,
    UCN_ERR_MALFORMED = -4,
    UCN_ERR_SECURITY = -5,
    UCN_ERR_REPLAY = -6,
    UCN_ERR_ACCESS = -7,
    UCN_ERR_STATE = -8,
    UCN_ERR_EXHAUSTED = -9,
    UCN_ERR_NOT_FOUND = -10,
    UCN_ERR_TIMEOUT = -11,
    UCN_ERR_CANCELLED = -12,
    UCN_ERR_POLICY = -13,
    UCN_ERR_UNSUPPORTED = -14,
    UCN_ERR_IN_DOUBT = -15
};

#define UCN_STATIC_ASSERT_JOIN_INNER(left_, right_) left_##right_
#define UCN_STATIC_ASSERT_JOIN(left_, right_) \
    UCN_STATIC_ASSERT_JOIN_INNER(left_, right_)
#define UCN_STATIC_ASSERT(condition_, name_) \
    typedef char UCN_STATIC_ASSERT_JOIN(ucn_static_assert_##name_, __LINE__)[ \
        (condition_) ? 1 : -1]

UCN_STATIC_ASSERT(sizeof(ucn_result_t) == 4U, result_must_be_i32);
UCN_STATIC_ASSERT(UCN_ERR_IN_DOUBT == -15, result_values_must_not_drift);

/* EN: Handles are process-local references, never Wire identity or Authority.
 * Every field participates in validation; all-zero and nonzero reserved data
 * are invalid.
 * 中文：Handle 只是在本次运行期内使用的本地引用，不是线上身份或 Authority。
 * 所有字段都参与校验；全零值和非零保留字段均非法。 */
typedef struct ucn_handle {
    uint32_t runtime_instance;
    uint16_t owner_instance;
    uint16_t slot;
    uint16_t generation;
    uint8_t object_kind;
    uint8_t reserved_zero;
} ucn_handle_t;

typedef ucn_handle_t ucn_send_handle_t;
typedef ucn_handle_t ucn_endpoint_handle_t;
typedef ucn_handle_t ucn_path_handle_t;
typedef ucn_handle_t ucn_group_handle_t;
typedef ucn_handle_t ucn_time_domain_handle_t;
typedef ucn_handle_t ucn_link_handle_t;
typedef ucn_handle_t ucn_driver_token_t;

typedef uint8_t ucn_object_kind_t;
enum {
    UCN_OBJECT_KIND_SEND = 1,
    UCN_OBJECT_KIND_ENDPOINT = 2,
    UCN_OBJECT_KIND_PATH = 3,
    UCN_OBJECT_KIND_GROUP = 4,
    UCN_OBJECT_KIND_TIME_DOMAIN = 5,
    UCN_OBJECT_KIND_LINK = 6,
    UCN_OBJECT_KIND_ADAPTER_TX = 7,
    UCN_OBJECT_KIND_ADAPTER_RX = 8,
    UCN_OBJECT_KIND_PERSISTENCE = 9,
    UCN_OBJECT_KIND_CLUSTER = 10
};

UCN_STATIC_ASSERT(sizeof(ucn_handle_t) == 12U, handle_must_be_12_bytes);
UCN_STATIC_ASSERT(offsetof(ucn_handle_t, object_kind) == 10U,
                  handle_field_order_must_not_drift);

#ifdef __cplusplus
}
#endif

#endif
