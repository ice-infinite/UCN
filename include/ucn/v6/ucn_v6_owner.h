#ifndef UCN_V6_OWNER_H
#define UCN_V6_OWNER_H

#include "ucn/v6/ucn_v6_config.h"
#include "ucn/v6/ucn_v6_identity.h"

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

/* EN: Owner notifications are five coalescing latches, not a counted event
 * queue. The manifest depth is therefore fixed to UCN_V6_OWNER_EVENT_COUNT in
 * every Profile; repeated posts of one event do not consume more storage.
 * 中文：Owner 通知是五个合并锁存位，不是计数事件队列。因此所有 Profile 的
 * Manifest 深度固定等于 UCN_V6_OWNER_EVENT_COUNT；同类重复通知不额外占内存。 */

typedef struct ucn_v6_owner_lock_ops {
    void *context;
    /* Task context must acquire this lock before returning. */
    void (*lock_task)(void *context);
    bool (*try_lock_from_isr)(void *context);
    void (*unlock_task)(void *context);
    void (*unlock_from_isr)(void *context);
    void (*notify)(void *context, bool from_isr);
} ucn_v6_owner_lock_ops_t;

/* The event mailbox is deliberately private.  Products can only enter the
 * protocol through the canonical fixed-stage Owner below, so an RTOS port
 * cannot replace the safety order with an arbitrary event callback. */
typedef enum ucn_v6_stack_phase {
    UCN_V6_STACK_PHASE_RX_INGRESS = 0,
    UCN_V6_STACK_PHASE_TX_COMPLETION = 1,
    UCN_V6_STACK_PHASE_TIMER_EXPIRY = 2,
    UCN_V6_STACK_PHASE_PERSISTENCE = 3,
    UCN_V6_STACK_PHASE_HOP_SECURITY = 4,
    UCN_V6_STACK_PHASE_E2E_SECURITY = 5,
    UCN_V6_STACK_PHASE_CAPABILITY = 6,
    UCN_V6_STACK_PHASE_ROUTE_AUTHORITY = 7,
    UCN_V6_STACK_PHASE_REALTIME = 8,
    UCN_V6_STACK_PHASE_OPERATION = 9,
    UCN_V6_STACK_PHASE_ENDPOINT = 10,
    UCN_V6_STACK_PHASE_CLUSTER = 11,
    UCN_V6_STACK_PHASE_QOS_TX = 12,
    UCN_V6_STACK_PHASE_COUNT = 13
} ucn_v6_stack_phase_t;

typedef enum ucn_v6_stack_invalidation_type {
    UCN_V6_STACK_INVALIDATE_LINK = 1,
    UCN_V6_STACK_INVALIDATE_SESSION = 2,
    UCN_V6_STACK_INVALIDATE_CAPABILITY = 3,
    UCN_V6_STACK_INVALIDATE_PATH = 4
} ucn_v6_stack_invalidation_type_t;

/* Every invalidation carries the complete parent chain.  Fields below the
 * selected type must be canonical zero, which prevents a caller from applying
 * an event to an ambiguous generation domain. */
typedef struct ucn_v6_stack_invalidation {
    ucn_v6_stack_invalidation_type_t type;
    uint16_t link_id;
    uint32_t link_generation;
    ucn_v6_session_key_t session;
    uint32_t capability_generation;
    uint16_t path_id;
    uint32_t path_generation;
} ucn_v6_stack_invalidation_t;

/* EN: Validates a complete parent-bound dependency invalidation key.
 * 中文：校验携带完整父代际链的依赖失效键。 */
bool ucn_v6_stack_invalidation_is_valid(
    const ucn_v6_stack_invalidation_t *invalidation);

typedef struct ucn_v6_stack_phase_result {
    uint16_t work_done;
    /* EN: has_more is valid only after making observable progress in this run.
     * 中文：has_more 仅在本轮已产生可观察进度时有效。 */
    bool has_more;
    bool has_deadline;
    uint64_t next_deadline_us;
    bool has_invalidation;
    ucn_v6_stack_invalidation_t invalidation;
} ucn_v6_stack_phase_result_t;

typedef ucn_v6_result_t (*ucn_v6_stack_phase_hook_t)(
    void *context,
    uint64_t now_us,
    uint16_t budget,
    ucn_v6_stack_phase_result_t *result);
typedef ucn_v6_result_t (*ucn_v6_stack_invalidation_hook_t)(
    void *context,
    const ucn_v6_stack_invalidation_t *invalidation);

/* These are named dependencies, not an extensible callback array.  The Owner
 * validates the exact compiled Feature Manifest and invokes them only in the
 * enum order above. */
typedef struct ucn_v6_stack_hooks {
    void *context;
    ucn_v6_stack_phase_hook_t rx_ingress;
    ucn_v6_stack_phase_hook_t tx_completion;
    ucn_v6_stack_phase_hook_t timer_expiry;
    ucn_v6_stack_phase_hook_t persistence;
    ucn_v6_stack_phase_hook_t hop_security;
    ucn_v6_stack_phase_hook_t e2e_security;
    ucn_v6_stack_phase_hook_t capability;
    ucn_v6_stack_phase_hook_t route_authority;
    ucn_v6_stack_phase_hook_t realtime;
    ucn_v6_stack_phase_hook_t operation;
    ucn_v6_stack_phase_hook_t endpoint;
    ucn_v6_stack_phase_hook_t cluster;
    ucn_v6_stack_phase_hook_t qos_tx;
    ucn_v6_stack_invalidation_hook_t invalidate_adapter;
    ucn_v6_stack_invalidation_hook_t invalidate_security;
    ucn_v6_stack_invalidation_hook_t invalidate_capability;
    ucn_v6_stack_invalidation_hook_t invalidate_realtime;
    ucn_v6_stack_invalidation_hook_t invalidate_cluster;
    ucn_v6_stack_invalidation_hook_t invalidate_transfer;
    ucn_v6_stack_invalidation_hook_t invalidate_route;
    ucn_v6_stack_invalidation_hook_t invalidate_qos;
    ucn_v6_stack_invalidation_hook_t invalidate_endpoint;
} ucn_v6_stack_hooks_t;

typedef struct ucn_v6_stack_budget {
    uint16_t max_total_work;
    uint16_t phase_work[UCN_V6_STACK_PHASE_COUNT];
} ucn_v6_stack_budget_t;

/* EN: Validates the one canonical per-run budget against the exact compiled
 * Feature Manifest. Enabled phases require nonzero budgets, disabled phases
 * require zero, and the phase sum must fit within max_total_work.
 * 中文：根据精确的编译期 Feature Manifest 校验单轮标准预算；已启用阶段
 * 必须非零，未启用阶段必须为零，各阶段之和不得超过总预算。 */
bool ucn_v6_stack_budget_is_valid(
    const ucn_v6_feature_manifest_t *manifest,
    const ucn_v6_stack_budget_t *budget);

typedef struct ucn_v6_stack_run_result {
    uint16_t work_done;
    uint32_t phases_run_mask;
    uint32_t phases_backlogged_mask;
    uint32_t pending_event_mask;
    bool more_work;
    bool has_next_deadline;
    uint64_t next_deadline_us;
    ucn_v6_result_t last_error;
} ucn_v6_stack_run_result_t;

typedef struct ucn_v6_stack_owner_view {
    uint64_t last_now_us;
    uint64_t next_deadline_us;
    uint32_t pending_event_mask;
    uint32_t invalidations_applied;
    bool has_time;
    bool has_next_deadline;
    bool rerun_pending;
    bool running;
    bool faulted;
    ucn_v6_result_t last_error;
} ucn_v6_stack_owner_view_t;

typedef struct ucn_v6_stack_owner ucn_v6_stack_owner_t;
#ifndef UCN_V6_STACK_OWNER_STORAGE_BYTES
#define UCN_V6_STACK_OWNER_STORAGE_BYTES ((size_t)2048U)
#endif
typedef union ucn_v6_stack_owner_storage {
    uint64_t alignment_u64;
    void *alignment_pointer;
    uint8_t bytes[UCN_V6_STACK_OWNER_STORAGE_BYTES];
} ucn_v6_stack_owner_storage_t;

/* EN: Initializes the canonical fixed-stage stack runtime.
 * 中文：初始化具有固定阶段顺序的标准协议栈运行时。 */
ucn_v6_result_t ucn_v6_stack_owner_init_in_place(
    void *storage,
    size_t storage_bytes,
    const ucn_v6_feature_manifest_t *manifest,
    const ucn_v6_owner_lock_ops_t *lock_ops,
    const ucn_v6_stack_hooks_t *hooks,
    ucn_v6_stack_owner_t **owner);

/* EN: Coalesces a wake reason. ISR callers never run a protocol phase.
 * 中文：合并一个唤醒原因；ISR 调用方永不执行协议阶段。 */
ucn_v6_result_t ucn_v6_stack_owner_post(
    ucn_v6_stack_owner_t *owner,
    ucn_v6_owner_event_t event,
    bool from_isr);

/* EN: Executes the immutable stage graph with one monotonic time and bounded
 * per-stage budgets. Invalidation fan-out completes before a later stage runs.
 * 中文：使用同一单调时间和分阶段预算执行不可变阶段图；失效传播完成后才会
 * 进入后续阶段。 */
ucn_v6_result_t ucn_v6_stack_owner_run(
    ucn_v6_stack_owner_t *owner,
    uint64_t now_us,
    const ucn_v6_stack_budget_t *budget,
    ucn_v6_stack_run_result_t *result);

ucn_v6_result_t ucn_v6_stack_owner_copy_view(
    ucn_v6_stack_owner_t *owner,
    ucn_v6_stack_owner_view_t *view);

#ifdef __cplusplus
}
#endif

#endif
