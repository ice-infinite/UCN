#ifndef UCN_OWNER_H
#define UCN_OWNER_H

#include "ucn/ucn_types.h"

#define UCN_I_LOCK_OPS_VERSION UINT16_C(1)
#define UCN_I_CALLBACK_GATE_SCHEMA UINT16_C(1)
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
