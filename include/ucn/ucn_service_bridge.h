#ifndef UCN_SERVICE_BRIDGE_H
#define UCN_SERVICE_BRIDGE_H

#include "ucn/ucn_node.h"
#include "ucn/ucn_service.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef UCN_SERVICE_BRIDGE_MAX_VALIDATORS
#define UCN_SERVICE_BRIDGE_MAX_VALIDATORS UCN_SERVICE_MAX_Q0_BINDINGS
#endif

#ifndef UCN_SERVICE_BRIDGE_REPLAY_DEPTH
#define UCN_SERVICE_BRIDGE_REPLAY_DEPTH ((uint8_t)4U)
#endif

typedef char ucn_service_bridge_validator_limit_must_fit[
    UCN_SERVICE_BRIDGE_MAX_VALIDATORS <= UCN_SERVICE_MAX_BINDINGS ? 1 : -1];
typedef char ucn_service_bridge_replay_depth_must_be_positive[
    UCN_SERVICE_BRIDGE_REPLAY_DEPTH > 0U ? 1 : -1];

/* T25.2 is a product/Protocol-Task adapter, not part of the Router or Node
 * state machine.  It contains no RTOS object and must only be called by the
 * one context that owns ucn_node_t. */
typedef void (*ucn_service_bridge_lock_fn)(void *context);
typedef void (*ucn_service_bridge_inbound_observer_fn)(
    void *context,
    const ucn_frame_t *frame,
    ucn_result_t result);
typedef void (*ucn_service_bridge_outbound_observer_fn)(
    void *context,
    const ucn_service_message_t *message,
    ucn_result_t final_result);

/* Structured local-final outcome for S20.  This remains a local observation:
 * only LINK_QUEUE_ACCEPTED has a stage, while failures have stage NONE. */
typedef enum ucn_service_bridge_outbound_outcome {
    UCN_SERVICE_OUTBOUND_LINK_QUEUE_ACCEPTED = 0,
    UCN_SERVICE_OUTBOUND_BACKPRESSURE_REJECTED = 1,
    UCN_SERVICE_OUTBOUND_BACKPRESSURE_EXHAUSTED = 2,
    UCN_SERVICE_OUTBOUND_EXPIRED = 3,
    UCN_SERVICE_OUTBOUND_TERMINAL_FAILED = 4
} ucn_service_bridge_outbound_outcome_t;

typedef struct ucn_service_bridge_outbound_event {
    ucn_service_async_stage_t stage;
    ucn_service_bridge_outbound_outcome_t outcome;
    ucn_result_t result;
} ucn_service_bridge_outbound_event_t;

typedef void (*ucn_service_bridge_outbound_event_observer_fn)(
    void *context,
    const ucn_service_message_t *message,
    const ucn_service_bridge_outbound_event_t *event);

/* Product-defined admission check for a remote Service frame.  The callback
 * runs in the Protocol Task after Core security/decryption and before the
 * Router lock/copy, so a rejected command never enters an Inbox.  Explicit
 * fields are supplied in addition to the complete Frame to keep the ABI
 * usable by small products without reconstructing them. */
typedef ucn_result_t (*ucn_service_bridge_validator_fn)(
    void *context,
    const ucn_frame_t *frame,
    ucn_node_id_t source_node_id,
    ucn_session_id_t source_session_id,
    ucn_endpoint_t endpoint,
    const uint8_t *payload,
    uint16_t payload_length,
    uint32_t now_ms);

typedef struct ucn_service_bridge_validator_entry {
    bool occupied;
    ucn_endpoint_t endpoint;
    ucn_service_bridge_validator_fn validator;
    void *context;
} ucn_service_bridge_validator_entry_t;

/* Optional product-owned anti-replay table.  One slot retains the active
 * (source, session, endpoint) tuple and the newest command ID.  A different
 * Session is fail-closed until trusted product/security code explicitly
 * rotates it; this prevents an old Session from becoming current again. */
typedef struct ucn_service_bridge_replay_entry {
    bool occupied;
    bool has_last_command_id;
    ucn_node_id_t source_node_id;
    ucn_session_id_t source_session_id;
    ucn_endpoint_t endpoint;
    uint32_t last_command_id;
} ucn_service_bridge_replay_entry_t;

typedef struct ucn_service_bridge_replay_state {
    ucn_service_bridge_replay_entry_t entries[UCN_SERVICE_BRIDGE_REPLAY_DEPTH];
} ucn_service_bridge_replay_state_t;

/* The optional hooks are still platform-neutral: T25.3 uses them to guard
 * short Router copies and wake a FreeRTOS consumer, without placing any RTOS
 * type or dependency in the C99 Core.  lock/unlock must either both be set or
 * both be NULL.  observer runs after unlock in the Protocol Task context. */
typedef struct ucn_service_bridge_inbound_hooks {
    void *context;
    ucn_service_bridge_lock_fn lock;
    ucn_service_bridge_lock_fn unlock;
    ucn_service_bridge_inbound_observer_fn observer;
} ucn_service_bridge_inbound_hooks_t;

/* Optional S15 policy for one fixed Bridge-owned Q0 Pending slot.  The policy
 * retries only local Adapter admission failure (UCN_ERR_NO_SPACE).  It does
 * not create a remote ACK, retransmit an accepted frame, or affect Q1. */
typedef struct ucn_service_bridge_q0_backpressure_policy {
    uint8_t max_retries;
    uint32_t retry_interval_ms;
    uint32_t timeout_ms;
} ucn_service_bridge_q0_backpressure_policy_t;

typedef struct ucn_service_bridge_q0_pending {
    bool occupied;
    uint8_t retries;
    uint32_t next_attempt_ms;
    uint32_t deadline_ms;
    ucn_service_message_t message;
} ucn_service_bridge_q0_pending_t;

typedef struct ucn_service_bridge_stats {
    uint32_t endpoint_handlers_installed;
    uint32_t remote_tx_attempted;
    uint32_t remote_tx_accepted;
    uint32_t remote_tx_failed;
    uint32_t q0_backpressure_retries;
    uint32_t q0_backpressure_exhausted;
    uint32_t q0_backpressure_expired;
    uint32_t q0_backpressure_terminal_failed;
    uint32_t inbound_delivered;
    uint32_t inbound_rejected;
    uint32_t inbound_validator_checked;
    uint32_t inbound_validator_accepted;
    uint32_t inbound_validator_rejected;
    uint32_t inbound_validator_missing;
    ucn_result_t last_tx_result;
    ucn_result_t last_inbound_result;
} ucn_service_bridge_stats_t;

typedef struct ucn_service_protocol_bridge {
    ucn_service_router_t *router;
    ucn_node_t *node;
    bool endpoint_handlers_installed;
    ucn_service_bridge_validator_entry_t
        validators[UCN_SERVICE_BRIDGE_MAX_VALIDATORS];
    ucn_service_bridge_inbound_hooks_t inbound_hooks;
    bool q0_backpressure_retry_enabled;
    ucn_service_bridge_q0_backpressure_policy_t q0_backpressure_policy;
    ucn_service_bridge_q0_pending_t q0_pending;
    ucn_service_bridge_outbound_observer_fn outbound_observer;
    void *outbound_observer_context;
    ucn_service_bridge_outbound_event_observer_fn outbound_event_observer;
    void *outbound_event_observer_context;
    ucn_service_bridge_stats_t stats;
} ucn_service_protocol_bridge_t;

/* Both objects must already be initialized and represent the same local Node
 * ID.  This function neither registers Link objects nor sends a frame. */
ucn_result_t ucn_service_protocol_bridge_init(
    ucn_service_protocol_bridge_t *bridge,
    ucn_service_router_t *router,
    ucn_node_t *node);

/* Registers or replaces one Endpoint Validator before handlers are installed.
 * Passing validator=NULL removes an existing registration.  High-risk
 * Bindings marked require_remote_q0_validator cannot be installed without a
 * matching non-NULL entry. */
ucn_result_t ucn_service_protocol_bridge_set_validator(
    ucn_service_protocol_bridge_t *bridge,
    ucn_endpoint_t endpoint,
    ucn_service_bridge_validator_fn validator,
    void *context);

/* Claims every static Endpoint in the Router binding table.  Existing
 * handlers owned by another component are rejected rather than overwritten. */
ucn_result_t ucn_service_protocol_bridge_install_endpoint_handlers(
    ucn_service_protocol_bridge_t *bridge);

/* Installs optional Router critical-section and post-delivery hooks for a
 * product Port.  Passing NULL clears every hook.  This API does not create a
 * Task, Queue, or timer and has no wire-format effect. */
ucn_result_t ucn_service_protocol_bridge_set_inbound_hooks(
    ucn_service_protocol_bridge_t *bridge,
    const ucn_service_bridge_inbound_hooks_t *hooks);

/* NULL disables Q0 Bridge retry and preserves the original best-effort
 * dequeue/submit behavior.  Policy changes are rejected while the fixed
 * Pending slot owns a message. */
ucn_result_t ucn_service_protocol_bridge_set_q0_backpressure_policy(
    ucn_service_protocol_bridge_t *bridge,
    const ucn_service_bridge_q0_backpressure_policy_t *policy);

/* Called exactly once after a dequeued message reaches a final local submit
 * result.  Intermediate UCN_ERR_NO_SPACE retries are not reported.  UCN_OK
 * means only local Core/Link acceptance, never remote inbox or execution.
 * The message pointer is valid only for the duration of the callback; copy
 * any identifiers that the product needs to retain. */
ucn_result_t ucn_service_protocol_bridge_set_outbound_observer(
    ucn_service_protocol_bridge_t *bridge,
    ucn_service_bridge_outbound_observer_fn observer,
    void *context);

/* Structured S20 form.  It may be installed together with the compatibility
 * observer; each installed callback fires exactly once for the same final
 * local outcome.  Intermediate backpressure retries never fire either one. */
ucn_result_t ucn_service_protocol_bridge_set_outbound_event_observer(
    ucn_service_protocol_bridge_t *bridge,
    ucn_service_bridge_outbound_event_observer_fn observer,
    void *context);

/* Protocol Task only: take/attempt at most max_requests items (Q0 before Q1).
 * With the optional policy disabled, a rejected item remains final as before.
 * With it enabled, one Q0 item may remain Bridge-owned across calls while
 * UCN_ERR_NO_SPACE is retried within the configured limits.  processed counts
 * attempts or an expired Pending completion; it may be zero while waiting. */
ucn_result_t ucn_service_protocol_bridge_step(
    ucn_service_protocol_bridge_t *bridge,
    uint8_t max_requests,
    uint8_t *processed);

/* Explicit-time variant for bare-metal/RTOS loops.  The compatibility wrapper
 * above uses node->now_ms, i.e. the latest ucn_node_step() time. */
ucn_result_t ucn_service_protocol_bridge_step_at(
    ucn_service_protocol_bridge_t *bridge,
    uint32_t now_ms,
    uint8_t max_requests,
    uint8_t *processed);

const ucn_service_bridge_stats_t *ucn_service_protocol_bridge_get_stats(
    const ucn_service_protocol_bridge_t *bridge);

/* Fixed-memory helper for product Validators.  accept_command mutates state
 * only on success.  A new tuple consumes one fixed slot; table exhaustion is
 * UCN_ERR_NO_SPACE and must remain fail-closed. */
void ucn_service_bridge_replay_init(
    ucn_service_bridge_replay_state_t *state);
ucn_result_t ucn_service_bridge_replay_accept_command(
    ucn_service_bridge_replay_state_t *state,
    ucn_node_id_t source_node_id,
    ucn_session_id_t source_session_id,
    ucn_endpoint_t endpoint,
    uint32_t command_id);
/* Call only after the product Security Provider has authenticated the Session
 * transition.  Rotation clears the command-ID baseline for the new Session;
 * an old Session is subsequently rejected. */
ucn_result_t ucn_service_bridge_replay_rotate_session(
    ucn_service_bridge_replay_state_t *state,
    ucn_node_id_t source_node_id,
    ucn_endpoint_t endpoint,
    ucn_session_id_t old_session_id,
    ucn_session_id_t new_session_id);

#ifdef __cplusplus
}
#endif

#endif
