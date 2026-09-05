#ifndef UCN_V6_OWNER_H
#define UCN_V6_OWNER_H

#include "ucn/v6/ucn_v6_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ucn_v6_owner_event {
    UCN_V6_OWNER_EVENT_RX = 1,
    UCN_V6_OWNER_EVENT_TX = 2,
    UCN_V6_OWNER_EVENT_COMPLETION = 3,
    UCN_V6_OWNER_EVENT_TIMER = 4,
    UCN_V6_OWNER_EVENT_PROVIDER = 5,
    UCN_V6_OWNER_EVENT_COUNT = 5
} ucn_v6_owner_event_t;

typedef struct ucn_v6_owner_lock_ops {
    void *context;
    /* Task context must acquire this lock before returning. */
    void (*lock_task)(void *context);
    bool (*try_lock_from_isr)(void *context);
    void (*unlock_task)(void *context);
    void (*unlock_from_isr)(void *context);
    void (*notify)(void *context, bool from_isr);
} ucn_v6_owner_lock_ops_t;

typedef ucn_v6_result_t (*ucn_v6_owner_event_handler_t)(
    void *context,
    ucn_v6_owner_event_t event);

typedef struct ucn_v6_protocol_owner ucn_v6_protocol_owner_t;
#ifndef UCN_V6_PROTOCOL_OWNER_STORAGE_BYTES
#define UCN_V6_PROTOCOL_OWNER_STORAGE_BYTES ((size_t)1024U)
#endif
typedef union ucn_v6_protocol_owner_storage {
    uint64_t alignment_u64;
    void *alignment_pointer;
    uint8_t bytes[UCN_V6_PROTOCOL_OWNER_STORAGE_BYTES];
} ucn_v6_protocol_owner_storage_t;

typedef struct ucn_v6_protocol_owner_view {
    uint16_t pending_total;
    uint16_t pending_by_event[UCN_V6_OWNER_EVENT_COUNT];
    uint8_t next_event_index;
    bool running;
    bool faulted;
} ucn_v6_protocol_owner_view_t;

/* EN: Initializes the unique protocol owner in caller-provided static storage.
 * 中文：在调用方提供的静态存储中初始化唯一协议 Owner。 */
ucn_v6_result_t ucn_v6_protocol_owner_init_in_place(
    void *storage,
    size_t storage_bytes,
    const ucn_v6_feature_manifest_t *manifest,
    const ucn_v6_owner_lock_ops_t *lock_ops,
    ucn_v6_protocol_owner_t **owner);

/* EN: Queues one bounded event; ISR callers only queue and notify.
 * 中文：投递一个有界事件；ISR 调用方只入队并通知。 */
ucn_v6_result_t ucn_v6_protocol_owner_post(
    ucn_v6_protocol_owner_t *owner,
    ucn_v6_owner_event_t event,
    bool from_isr);

/* EN: Advances at most budget events from the sole Owner context.
 * 中文：仅在 Owner 上下文中最多推进 budget 个事件。 */
ucn_v6_result_t ucn_v6_protocol_owner_run(
    ucn_v6_protocol_owner_t *owner,
    uint16_t budget,
    ucn_v6_owner_event_handler_t handler,
    void *handler_context,
    uint16_t *processed);

/* EN: Copies diagnostics without exposing writable protocol state.
 * 中文：复制诊断视图，不暴露可写协议状态。 */
ucn_v6_result_t ucn_v6_protocol_owner_copy_view(
    ucn_v6_protocol_owner_t *owner,
    ucn_v6_protocol_owner_view_t *view);

#ifdef __cplusplus
}
#endif

#endif
