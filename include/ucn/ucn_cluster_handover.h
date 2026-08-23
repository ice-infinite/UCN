#ifndef UCN_CLUSTER_HANDOVER_H
#define UCN_CLUSTER_HANDOVER_H

/* CLV2-M11: opt-in Merge/Handover value model.
 *
 * This API is deliberately available only from the separately linked
 * `ucn_cluster_handover_experimental` archive.  It does not parse or emit a
 * v4 frame, mutate `ucn_cluster_t`, grant Authority, or touch an Adapter.
 * A future production owner must separately prove RFC4 RX/TX, persistence,
 * quorum and Authority gates before it can bridge this model.
 */
#if !defined(UCN_CLUSTER_HANDOVER_EXPERIMENTAL_ENABLED)
#error "UCN M11 API is default-OFF; include it only through ucn_cluster_handover_experimental"
#endif

#include "ucn/ucn_cluster_config_state.h"
#include "ucn/ucn_cluster_epoch.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UCN_CLUSTER_HANDOVER_MAX_CANDIDATES ((size_t)4U)
#define UCN_CLUSTER_HANDOVER_WIRE_FORMAT_V4 ((uint8_t)4U)
#define UCN_CLUSTER_HANDOVER_CAP_BACKUP ((uint16_t)0x0001U)
#define UCN_CLUSTER_HANDOVER_CAP_JOINT_CONFIG ((uint16_t)0x0004U)
#define UCN_CLUSTER_HANDOVER_CAP_PERSISTENCE ((uint16_t)0x0008U)
#define UCN_CLUSTER_HANDOVER_REQUIRED_CAPABILITIES \
    (UCN_CLUSTER_HANDOVER_CAP_BACKUP | UCN_CLUSTER_HANDOVER_CAP_JOINT_CONFIG | \
     UCN_CLUSTER_HANDOVER_CAP_PERSISTENCE)
#define UCN_CLUSTER_HANDOVER_TRACE_CAPACITY ((size_t)8U)

typedef enum ucn_cluster_handover_offer_class {
    UCN_CLUSTER_HANDOVER_OFFER_INVALID = 0,
    UCN_CLUSTER_HANDOVER_OFFER_SAME_CLUSTER_AUTHORITY = 1,
    UCN_CLUSTER_HANDOVER_OFFER_FOREIGN_MERGE = 2
} ucn_cluster_handover_offer_class_t;

typedef enum ucn_cluster_handover_role {
    UCN_CLUSTER_HANDOVER_ROLE_INVALID = 0,
    UCN_CLUSTER_HANDOVER_ROLE_MEMBER = 1,
    UCN_CLUSTER_HANDOVER_ROLE_BACKUP = 2,
    UCN_CLUSTER_HANDOVER_ROLE_HEAD = 3,
    UCN_CLUSTER_HANDOVER_ROLE_PROVISIONAL = 4
} ucn_cluster_handover_role_t;

typedef enum ucn_cluster_handover_mode {
    UCN_CLUSTER_HANDOVER_MODE_INVALID = 0,
    UCN_CLUSTER_HANDOVER_MODE_FOREIGN_MERGE = 1,
    UCN_CLUSTER_HANDOVER_MODE_SAME_CLUSTER_PLANNED = 2
} ucn_cluster_handover_mode_t;

typedef enum ucn_cluster_handover_message_type {
    UCN_CLUSTER_HANDOVER_MESSAGE_INVALID = 0,
    UCN_CLUSTER_HANDOVER_MESSAGE_PREPARE = 26,
    UCN_CLUSTER_HANDOVER_MESSAGE_READY = 27,
    UCN_CLUSTER_HANDOVER_MESSAGE_COMMIT = 28,
    UCN_CLUSTER_HANDOVER_MESSAGE_WITHDRAW = 29,
    UCN_CLUSTER_HANDOVER_MESSAGE_STEPDOWN = 9
} ucn_cluster_handover_message_type_t;

typedef enum ucn_cluster_handover_state {
    UCN_CLUSTER_HANDOVER_STATE_IDLE = 0,
    UCN_CLUSTER_HANDOVER_STATE_PREPARE_SENT = 1,
    UCN_CLUSTER_HANDOVER_STATE_READY_SENT = 2,
    UCN_CLUSTER_HANDOVER_STATE_READY_VERIFIED = 3,
    UCN_CLUSTER_HANDOVER_STATE_AUTHORITY_REVOKED = 4,
    UCN_CLUSTER_HANDOVER_STATE_STEPDOWN_SENT = 5,
    UCN_CLUSTER_HANDOVER_STATE_COMMIT_SENT = 6,
    UCN_CLUSTER_HANDOVER_STATE_TARGET_COMMITTED = 7,
    UCN_CLUSTER_HANDOVER_STATE_TARGET_DURABLE = 8,
    UCN_CLUSTER_HANDOVER_STATE_ABORTED = 9
} ucn_cluster_handover_state_t;

typedef enum ucn_cluster_handover_trace_event {
    UCN_CLUSTER_HANDOVER_TRACE_NONE = 0,
    UCN_CLUSTER_HANDOVER_TRACE_READY_VERIFIED = 1,
    UCN_CLUSTER_HANDOVER_TRACE_AUTHORITY_REVOKED = 2,
    UCN_CLUSTER_HANDOVER_TRACE_STEPDOWN_EMITTED = 3,
    UCN_CLUSTER_HANDOVER_TRACE_COMMIT_EMITTED = 4,
    UCN_CLUSTER_HANDOVER_TRACE_TARGET_COMMITTED = 5,
    UCN_CLUSTER_HANDOVER_TRACE_TARGET_DURABLE = 6,
    UCN_CLUSTER_HANDOVER_TRACE_MEMBER_JOIN_TARGET = 7,
    UCN_CLUSTER_HANDOVER_TRACE_MEMBER_OBSERVE = 8,
    UCN_CLUSTER_HANDOVER_TRACE_ABORTED = 9
} ucn_cluster_handover_trace_event_t;

typedef enum ucn_cluster_handover_feasibility_reason {
    UCN_CLUSTER_HANDOVER_FEASIBILITY_OK = 0,
    UCN_CLUSTER_HANDOVER_FEASIBILITY_ARGUMENT = 1,
    UCN_CLUSTER_HANDOVER_FEASIBILITY_CAPACITY = 2,
    UCN_CLUSTER_HANDOVER_FEASIBILITY_WIRE = 3,
    UCN_CLUSTER_HANDOVER_FEASIBILITY_CAPABILITIES = 4,
    UCN_CLUSTER_HANDOVER_FEASIBILITY_CONFIG = 5,
    UCN_CLUSTER_HANDOVER_FEASIBILITY_BACKUP_POLICY = 6
} ucn_cluster_handover_feasibility_reason_t;

typedef struct ucn_cluster_handover_offer {
    ucn_cluster_epoch_t epoch;
    uint32_t config_id;
    uint32_t config_hash;
    uint32_t nonce;
    uint16_t head_score;
    uint16_t cluster_size;
    uint16_t available_capacity;
    uint16_t capabilities;
    uint8_t wire_format;
    bool backup_policy_compatible;
} ucn_cluster_handover_offer_t;

typedef struct ucn_cluster_handover_policy {
    uint8_t improvement_percent;
    uint8_t required_samples;
    uint16_t required_capabilities;
    uint32_t head_min_tenure_ms;
    uint32_t merge_hold_down_ms;
    uint32_t retry_interval_ms;
    uint32_t transaction_timeout_ms;
} ucn_cluster_handover_policy_t;

typedef struct ucn_cluster_handover_candidate {
    bool occupied;
    ucn_cluster_handover_offer_t offer;
    /* score_samples is a consecutive run, never a fresh-packet count.  The
     * context records the exact local threshold/policy that qualified it. */
    uint8_t score_samples;
    uint8_t qualification_improvement_percent;
    uint8_t qualification_required_samples;
    uint16_t qualification_local_head_score;
    uint16_t qualification_required_capabilities;
    uint32_t first_seen_ms;
    uint32_t last_seen_ms;
    uint32_t hold_down_until_ms;
} ucn_cluster_handover_candidate_t;

typedef struct ucn_cluster_handover_candidate_table {
    ucn_cluster_handover_candidate_t slots[UCN_CLUSTER_HANDOVER_MAX_CANDIDATES];
} ucn_cluster_handover_candidate_table_t;

typedef struct ucn_cluster_handover_feasibility {
    bool admitted;
    uint8_t reason;
} ucn_cluster_handover_feasibility_t;

typedef struct ucn_cluster_handover_message {
    uint8_t type;
    uint8_t sender_role;
    uint8_t mode;
    ucn_node_id_t source_node_id;
    ucn_cluster_epoch_t old_epoch;
    ucn_cluster_epoch_t target_epoch;
    uint32_t transaction_id;
    /* PREPARE/READY/COMMIT bind the target Config.  Their stepdown_nonce is
     * always zero: RFC4 Type 26..28 have no nonce field.  STEPDOWN (Type 9)
     * and WITHDRAW (Type 29) deliberately carry no target Config, while
     * their nonce is an old-Authority fence only. */
    uint32_t target_config_id;
    uint32_t target_config_hash;
    uint32_t stepdown_nonce;
} ucn_cluster_handover_message_t;

typedef struct ucn_cluster_handover_receiver_context {
    /* local_epoch is the receiver's actually installed Epoch.  For a
     * same-Cluster Planned Transfer a Backup remains in old_epoch until
     * Commit/persist succeeds; expected_target_epoch is only the proposed
     * successor it has admitted for this transaction. */
    ucn_cluster_epoch_t local_epoch;
    ucn_cluster_epoch_t expected_target_epoch;
    uint32_t active_config_id;
    uint32_t active_config_hash;
    uint16_t available_capacity;
    uint16_t capabilities;
    uint8_t wire_format;
    uint8_t local_role;
    bool confirmed_backup;
    bool backup_policy_compatible;
} ucn_cluster_handover_receiver_context_t;

typedef struct ucn_cluster_handover_transaction {
    bool active;
    bool local_authority_active;
    /* Implementation-owned, irreversible once revoke_authority() succeeds.
     * transaction_reset() must preserve this fence so a caller cannot clear a
     * revoked old-Head transaction and begin it again. */
    uint32_t authority_reentry_fence;
    bool recovery_observe_required;
    bool target_epoch_durable;
    uint8_t mode;
    uint8_t state;
    uint8_t retry_count;
    uint8_t trace_count;
    ucn_cluster_epoch_t old_epoch;
    ucn_cluster_epoch_t target_epoch;
    uint32_t old_config_id;
    uint32_t old_config_hash;
    uint32_t target_config_id;
    uint32_t target_config_hash;
    uint32_t transaction_id;
    /* Set only after READY by revoke_authority().  It is emitted only on
     * Type 9/29 and is never part of Type 26..28 transaction identity. */
    uint32_t stepdown_nonce;
    uint32_t retry_deadline_ms;
    uint32_t deadline_ms;
    ucn_cluster_handover_trace_event_t trace[UCN_CLUSTER_HANDOVER_TRACE_CAPACITY];
} ucn_cluster_handover_transaction_t;

typedef struct ucn_cluster_handover_member_result {
    bool join_target;
    bool observe_target;
    ucn_cluster_epoch_t target_epoch;
    uint32_t transaction_id;
    uint32_t stepdown_nonce;
} ucn_cluster_handover_member_result_t;

/* The caller must zero-initialize transaction storage before first use.  This
 * helper clears only zero-fence caller-owned transaction before local begin or
 * target-side PREPARE admission.  revoke_authority() irreversibly seals the
 * object: any nonzero fence makes later reset calls no-ops, so Begin cannot
 * recreate local Authority after revoke/Stepdown/Commit.  This API is not a lifecycle
 * release; a fenced object remains Observe/Recovery-only. */
void ucn_cluster_handover_transaction_reset(
    ucn_cluster_handover_transaction_t *transaction);

/* Classifies only the Cluster identity.  Foreign Terms are intentionally never
 * inspected: A/term2 and B/term100 are both FOREIGN_MERGE. */
ucn_cluster_handover_offer_class_t ucn_cluster_handover_offer_classify(
    const ucn_cluster_epoch_t *local_epoch,
    const ucn_cluster_handover_offer_t *offer);

void ucn_cluster_handover_candidate_table_reset(
    ucn_cluster_handover_candidate_table_t *table);
void ucn_cluster_handover_candidate_expire(
    ucn_cluster_handover_candidate_table_t *table,
    uint32_t now_ms,
    uint32_t expiry_ms);
ucn_result_t ucn_cluster_handover_candidate_observe(
    ucn_cluster_handover_candidate_table_t *table,
    const ucn_cluster_epoch_t *local_epoch,
    uint16_t local_head_score,
    const ucn_cluster_handover_offer_t *offer,
    const ucn_cluster_handover_policy_t *policy,
    uint32_t now_ms,
    ucn_cluster_handover_candidate_t **output);
bool ucn_cluster_handover_candidate_is_eligible(
    const ucn_cluster_handover_candidate_t *candidate,
    uint16_t local_head_score,
    uint32_t local_head_since_ms,
    const ucn_cluster_handover_policy_t *policy,
    uint32_t now_ms);
void ucn_cluster_handover_candidate_note_result(
    ucn_cluster_handover_candidate_t *candidate,
    bool handover_started,
    const ucn_cluster_handover_policy_t *policy,
    uint32_t now_ms);

ucn_result_t ucn_cluster_handover_feasibility_evaluate(
    const ucn_cluster_handover_offer_t *target,
    uint16_t losing_cluster_size,
    const ucn_cluster_handover_policy_t *policy,
    ucn_cluster_handover_feasibility_t *output);

ucn_result_t ucn_cluster_handover_transaction_begin(
    ucn_cluster_handover_transaction_t *transaction,
    const ucn_cluster_epoch_t *old_epoch,
    const ucn_cluster_handover_offer_t *target,
    const ucn_cluster_handover_policy_t *policy,
    uint16_t losing_cluster_size,
    uint32_t old_config_id,
    uint32_t old_config_hash,
    uint32_t transaction_id,
    uint32_t now_ms);
ucn_result_t ucn_cluster_handover_transaction_build_prepare(
    const ucn_cluster_handover_transaction_t *transaction,
    ucn_cluster_handover_message_t *output);
ucn_result_t ucn_cluster_handover_transaction_accept_prepare(
    ucn_cluster_handover_transaction_t *transaction,
    const ucn_cluster_handover_receiver_context_t *receiver,
    const ucn_cluster_handover_message_t *prepare,
    const ucn_cluster_handover_policy_t *policy,
    uint16_t losing_cluster_size,
    uint32_t now_ms,
    ucn_cluster_handover_message_t *ready_output);
ucn_result_t ucn_cluster_handover_transaction_accept_ready(
    ucn_cluster_handover_transaction_t *transaction,
    const ucn_cluster_handover_message_t *ready);
ucn_result_t ucn_cluster_handover_transaction_revoke_authority(
    ucn_cluster_handover_transaction_t *transaction,
    uint32_t stepdown_nonce);
ucn_result_t ucn_cluster_handover_transaction_build_stepdown(
    ucn_cluster_handover_transaction_t *transaction,
    ucn_cluster_handover_message_t *output);
ucn_result_t ucn_cluster_handover_transaction_build_commit(
    ucn_cluster_handover_transaction_t *transaction,
    ucn_cluster_handover_message_t *output);
ucn_result_t ucn_cluster_handover_transaction_accept_commit(
    ucn_cluster_handover_transaction_t *transaction,
    const ucn_cluster_handover_message_t *commit,
    uint32_t now_ms);
/* M11 has no Provider submit+reload proof.  This API therefore remains
 * fail-closed and returns UCN_ERR_UNSUPPORTED while preserving transaction
 * state.  A future persistence owner must replace it with a receipt/reload
 * proof, not a caller-supplied Epoch equality claim. */
ucn_result_t ucn_cluster_handover_transaction_mark_target_epoch_durable(
    ucn_cluster_handover_transaction_t *transaction,
    const ucn_cluster_epoch_t *durable_epoch,
    uint32_t now_ms);
bool ucn_cluster_handover_transaction_target_authority_ready(
    const ucn_cluster_handover_transaction_t *transaction);
bool ucn_cluster_handover_transaction_retry_due(
    const ucn_cluster_handover_transaction_t *transaction,
    uint32_t now_ms);
ucn_result_t ucn_cluster_handover_transaction_note_prepare_retransmitted(
    ucn_cluster_handover_transaction_t *transaction,
    const ucn_cluster_handover_policy_t *policy,
    uint32_t now_ms);
ucn_result_t ucn_cluster_handover_transaction_step(
    ucn_cluster_handover_transaction_t *transaction,
    uint32_t now_ms);

ucn_result_t ucn_cluster_handover_member_accept_stepdown(
    const ucn_cluster_epoch_t *current_epoch,
    uint32_t active_config_id,
    uint32_t active_config_hash,
    ucn_node_id_t expected_old_head,
    uint8_t local_role,
    const ucn_cluster_handover_message_t *stepdown,
    ucn_cluster_handover_member_result_t *output);
void ucn_cluster_handover_member_note_target_lost(
    ucn_cluster_handover_member_result_t *member);
bool ucn_cluster_handover_trace_order_is_valid(
    const ucn_cluster_handover_transaction_t *transaction);

#ifdef __cplusplus
}
#endif

#endif /* UCN_CLUSTER_HANDOVER_H */
