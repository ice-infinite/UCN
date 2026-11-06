#ifndef UCN_OWNER_H
#define UCN_OWNER_H

#include "ucn/ucn_types.h"

#define UCN_I_LOCK_OPS_VERSION UINT16_C(1)
#define UCN_I_CALLBACK_GATE_MAGIC UINT32_C(0x55434754)
#define UCN_I_CALLBACK_GATE_SCHEMA UINT16_C(1)
#define UCN_I_OWNER_MAILBOX_MAGIC UINT32_C(0x55434D42)
#define UCN_I_OWNER_MAILBOX_SCHEMA UINT16_C(1)
#define UCN_I_OWNER_WORK_CLASS_LIMIT UINT8_C(5)

typedef ucn_result_t (*ucn_i_lock_enter_fn)(void *context);
typedef void (*ucn_i_lock_leave_fn)(void *context);

/* The product supplies a short, non-callbacking task/ISR/SMP-safe critical
 * section. The common layer never assumes volatile provides synchronization. */
typedef struct ucn_i_lock_ops {
    uint16_t struct_size;
    uint16_t api_version;
    void *context;
    ucn_i_lock_enter_fn enter;
    ucn_i_lock_leave_fn leave;
} ucn_i_lock_ops_t;

typedef struct ucn_i_callback_claim {
    uint32_t owner_instance;
    uint32_t operation_id;
    uint16_t operation_generation;
    uint16_t operation_kind;
} ucn_i_callback_claim_t;

typedef struct ucn_i_callback_gate {
    uint32_t magic;
    uint32_t gate_instance;
    uint16_t schema;
    uint16_t reserved_zero;
    ucn_i_callback_claim_t active_claim;
    ucn_i_lock_ops_t lock;
} ucn_i_callback_gate_t;

/* init/destroy require exclusive lifecycle ownership. Ordinary enter/leave/view
 * calls may run concurrently only when the supplied lock covers that domain. */
ucn_result_t ucn_i_callback_gate_init(ucn_i_callback_gate_t *gate,
                                      uint32_t gate_instance,
                                      const ucn_i_lock_ops_t *lock);
ucn_result_t ucn_i_callback_gate_enter(
    ucn_i_callback_gate_t *gate,
    const ucn_i_callback_claim_t *claim);
ucn_result_t ucn_i_callback_gate_leave(
    ucn_i_callback_gate_t *gate,
    const ucn_i_callback_claim_t *claim);
ucn_result_t ucn_i_callback_gate_view(
    ucn_i_callback_gate_t *gate,
    bool *active_out,
    ucn_i_callback_claim_t *claim_out);
ucn_result_t ucn_i_callback_gate_destroy(ucn_i_callback_gate_t *gate);

typedef uint8_t ucn_i_owner_work_class_t;
enum {
    UCN_I_OWNER_WORK_COMPLETION = 0,
    UCN_I_OWNER_WORK_INVALIDATION = 1,
    UCN_I_OWNER_WORK_CANCEL_RETIRE = 2,
    UCN_I_OWNER_WORK_TIMER = 3,
    UCN_I_OWNER_WORK_REQUEST = 4
};

typedef struct ucn_i_owner_work_hint {
    uint32_t occurrences;
    uint8_t work_class;
    uint8_t reserved_zero[3];
} ucn_i_owner_work_hint_t;

/* Mailbox entries are wakeup hints only. Correctness facts remain latched in
 * their owning token/object and are found again by bounded Owner scans. */
typedef struct ucn_i_owner_mailbox {
    uint32_t magic;
    uint16_t schema;
    uint8_t class_count;
    uint8_t cursor;
    uint32_t pending_mask;
    uint32_t occurrences[UCN_I_OWNER_WORK_CLASS_LIMIT];
    ucn_i_lock_ops_t lock;
} ucn_i_owner_mailbox_t;

/* EN: The caller must already hold mailbox->lock.  This form lets an Owner
 * publish its fact atomically with the state change that created it, without
 * recursively taking the same non-recursive lock.
 * 中文：调用方必须已持有 mailbox->lock。该形式允许 Owner 把事实提示与产生
 * 该事实的状态变化原子发布，且不会递归获取同一把非递归锁。 */
#if defined(_MSC_VER)
#define UCN_I_OWNER_HEADER_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define UCN_I_OWNER_HEADER_INLINE inline __attribute__((always_inline))
#else
#define UCN_I_OWNER_HEADER_INLINE inline
#endif
static UCN_I_OWNER_HEADER_INLINE ucn_result_t
ucn_i_owner_mailbox_publish_locked(
    ucn_i_owner_mailbox_t *mailbox,
    ucn_i_owner_work_class_t work_class)
{
    uint32_t bit;
    uint32_t valid_mask;
    uint8_t index;

    if (mailbox == NULL || mailbox->magic != UCN_I_OWNER_MAILBOX_MAGIC ||
        mailbox->schema != UCN_I_OWNER_MAILBOX_SCHEMA ||
        mailbox->class_count == 0U ||
        mailbox->class_count > UCN_I_OWNER_WORK_CLASS_LIMIT ||
        mailbox->cursor >= mailbox->class_count ||
        work_class >= mailbox->class_count) {
        return UCN_ERR_ARGUMENT;
    }
    valid_mask = (UINT32_C(1) << mailbox->class_count) - UINT32_C(1);
    if ((mailbox->pending_mask & ~valid_mask) != 0U) {
        return UCN_ERR_STATE;
    }
    for (index = 0U; index < UCN_I_OWNER_WORK_CLASS_LIMIT; ++index) {
        const bool pending =
            (mailbox->pending_mask & (UINT32_C(1) << index)) != 0U;

        if ((index < mailbox->class_count &&
             pending != (mailbox->occurrences[index] != 0U)) ||
            (index >= mailbox->class_count &&
             mailbox->occurrences[index] != 0U)) {
            return UCN_ERR_STATE;
        }
    }
    bit = UINT32_C(1) << work_class;
    mailbox->pending_mask |= bit;
    if (mailbox->occurrences[work_class] != UINT32_MAX) {
        ++mailbox->occurrences[work_class];
    }
    return UCN_OK;
}
#undef UCN_I_OWNER_HEADER_INLINE

/* init/destroy require exclusive lifecycle ownership. publish/take are safe in
 * every task/ISR/SMP context for which the supplied lock is safe. */
ucn_result_t ucn_i_owner_mailbox_init(ucn_i_owner_mailbox_t *mailbox,
                                      uint8_t class_count,
                                      const ucn_i_lock_ops_t *lock);
ucn_result_t ucn_i_owner_mailbox_publish(
    ucn_i_owner_mailbox_t *mailbox,
    ucn_i_owner_work_class_t work_class);
ucn_result_t ucn_i_owner_mailbox_take(
    ucn_i_owner_mailbox_t *mailbox,
    ucn_i_owner_work_hint_t *hint_out);
ucn_result_t ucn_i_owner_mailbox_destroy(ucn_i_owner_mailbox_t *mailbox);

#endif
