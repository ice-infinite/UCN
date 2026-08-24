#ifndef UCN_CLUSTER_REKEY_H
#define UCN_CLUSTER_REKEY_H

/* CLV2-M13: default-OFF quorum Rekey / no-wrap transaction owner.
 *
 * This API is intentionally unavailable unless the product explicitly links
 * ucn_cluster_rekey_experimental.  It consumes the M08 Authority Owner and an
 * M04 durable snapshot; it does not parse production RX, enable the v4
 * encoder, send a frame, mutate the Current FSM, or grant successor
 * Authority. */

#if !defined(UCN_CLUSTER_REKEY_EXPERIMENTAL_ENABLED) || \
    UCN_CLUSTER_REKEY_EXPERIMENTAL_ENABLED != 1
#error "ucn_cluster_rekey.h requires ucn_cluster_rekey_experimental"
#endif

#include "ucn/ucn_cluster_authority.h"
#include "ucn/ucn_cluster_config_persistence.h"
#include "ucn/ucn_cluster_wire_v4.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ucn_cluster_rekey_state {
    UCN_CLUSTER_REKEY_STATE_INVALID = 0,
    UCN_CLUSTER_REKEY_STATE_ID_HISTORY_DURABLE_REQUIRED = 1,
    UCN_CLUSTER_REKEY_STATE_PREPARE_REQUIRED = 2,
    UCN_CLUSTER_REKEY_STATE_PREPARE_PENDING = 3,
    UCN_CLUSTER_REKEY_STATE_PREPARED_DURABLE = 4,
    UCN_CLUSTER_REKEY_STATE_COLLECTING_ACKS = 5,
    UCN_CLUSTER_REKEY_STATE_QUORUM = 6,
    UCN_CLUSTER_REKEY_STATE_COMMIT_PENDING = 7,
    UCN_CLUSTER_REKEY_STATE_EPOCH_DURABLE = 8,
    UCN_CLUSTER_REKEY_STATE_COMMITTED = 9,
    UCN_CLUSTER_REKEY_STATE_FENCED = 10,
    UCN_CLUSTER_REKEY_STATE_ABORTED = 11
} ucn_cluster_rekey_state_t;

typedef enum ucn_cluster_rekey_trigger {
    UCN_CLUSTER_REKEY_TRIGGER_NONE = 0,
    UCN_CLUSTER_REKEY_TRIGGER_TERM = 1U << 0,
    UCN_CLUSTER_REKEY_TRIGGER_BACKUP_GENERATION = 1U << 1,
    UCN_CLUSTER_REKEY_TRIGGER_CONFIG_ID = 1U << 2
} ucn_cluster_rekey_trigger_t;

/* Snapshot ID exhaustion does not itself require a Cluster Rekey: it first
 * requests a new Backup generation and a full snapshot at ID 1.  If the
 * Backup generation has reached the same reserved boundary, Rekey wins and
 * snapshot rotation is not advertised as an available continuation. */
typedef struct ucn_cluster_rekey_serial_view {
    uint32_t term;
    uint32_t config_id;
    uint32_t backup_generation;
    uint32_t snapshot_id;
    bool has_backup;
    bool has_snapshot;
} ucn_cluster_rekey_serial_view_t;

typedef struct ucn_cluster_rekey_threshold_decision {
    uint8_t trigger_mask;
    bool rekey_required;
    bool snapshot_generation_rotation_required;
} ucn_cluster_rekey_threshold_decision_t;

#define UCN_CLUSTER_REKEY_CAPABILITY_PERSISTENCE ((uint16_t)0x0008U)
#define UCN_CLUSTER_REKEY_CAPABILITY_REKEY ((uint16_t)0x0020U)
#define UCN_CLUSTER_REKEY_REQUIRED_CAPABILITIES \
    ((uint16_t)(UCN_CLUSTER_REKEY_CAPABILITY_PERSISTENCE | \
                UCN_CLUSTER_REKEY_CAPABILITY_REKEY))
#define UCN_CLUSTER_REKEY_ACK_TIMEOUT_MS UINT32_C(1000)

/* CLV2-13-12: bounded durable allocation history. The 32-bit Cluster ID is
 * not assumed collision-free: a product Provider answer becomes usable only
 * after this history admits the complete allocation identity. */
#define UCN_CLUSTER_ID_HISTORY_CAPACITY ((uint8_t)8U)
#define UCN_CLUSTER_ID_HISTORY_RECORD_BYTES ((size_t)280U)

typedef struct ucn_cluster_id_history_entry {
    uint32_t cluster_id;
    ucn_cluster_id_request_t identity;
} ucn_cluster_id_history_entry_t;

typedef struct ucn_cluster_id_history {
    ucn_cluster_id_history_entry_t entries[UCN_CLUSTER_ID_HISTORY_CAPACITY];
    uint8_t count;
} ucn_cluster_id_history_t;

typedef struct ucn_cluster_rekey_voter_profile {
    ucn_node_id_t node_id;
    uint8_t wire_format;
    uint16_t capabilities;
    uint32_t persistence_generation;
} ucn_cluster_rekey_voter_profile_t;

typedef struct ucn_cluster_rekey_ack {
    ucn_node_id_t source_node_id;
    ucn_cluster_role_t source_role;
    uint32_t persistence_generation;
    uint32_t member_nonce;
} ucn_cluster_rekey_ack_t;

/* 13-01 starts with the predecessor identity only.  13-03 atomically fills
 * successor_epoch and successor_config through the ID Provider before a
 * PREPARE can be persisted.  Keeping those fields in the bounded owner now
 * avoids a second authority/state object later. */
typedef struct ucn_cluster_rekey_transaction {
    ucn_cluster_rekey_state_t state;
    ucn_cluster_phase_t phase;
    ucn_cluster_epoch_t predecessor_epoch;
    ucn_cluster_config_state_t predecessor_config;
    ucn_cluster_persist_config_ref_t predecessor_config_ref;
    ucn_cluster_epoch_t successor_epoch;
    ucn_cluster_config_state_t successor_config;
    ucn_cluster_persist_config_ref_t successor_config_ref;
    ucn_cluster_persist_rekey_ref_t durable_rekey_ref;
    ucn_cluster_id_request_t allocation_identity;
    ucn_cluster_rekey_voter_profile_t voter_profiles[UCN_CLUSTER_MAX_VOTERS];
    uint32_t ack_nonce_high_water[UCN_CLUSTER_MAX_VOTERS];
    uint64_t ack_bitmap;
    uint32_t transaction_id;
    uint32_t nonce;
    uint32_t allocation_history_generation;
    uint32_t allocation_history_fingerprint;
    uint32_t started_ms;
    uint32_t deadline_ms;
    ucn_node_id_t successor_backup_node_id;
    uint8_t voter_count;
    bool authority_revoked;
} ucn_cluster_rekey_transaction_t;

/* Atomic value emitted only after durable successor proof. It is not the
 * production ucn_cluster_t and cannot itself send or grant Authority; it
 * gives the future integration owner one complete, non-split state to apply. */
typedef struct ucn_cluster_rekey_successor_state {
    ucn_cluster_epoch_t epoch;
    ucn_cluster_config_state_t config;
    ucn_cluster_persist_config_ref_t config_ref;
    ucn_cluster_member_table_t members;
    ucn_cluster_voter_set_t voters;
    ucn_node_id_t backup_node_id;
    uint32_t backup_generation;
    uint32_t membership_sequence;
    uint32_t snapshot_generation;
    uint32_t snapshot_id;
} ucn_cluster_rekey_successor_state_t;

typedef enum ucn_cluster_rekey_persist_action {
    UCN_CLUSTER_REKEY_PERSIST_ACTION_NONE = 0,
    UCN_CLUSTER_REKEY_PERSIST_ACTION_PREPARE = 1,
    UCN_CLUSTER_REKEY_PERSIST_ACTION_COMMIT = 2,
    UCN_CLUSTER_REKEY_PERSIST_ACTION_ABORT = 3
} ucn_cluster_rekey_persist_action_t;

/* Isolated M13 persistence owner. Provider callbacks are foreign code, so
 * io_active is established before load/submit/poll and pending remains live
 * until an exact reload proves the requested journal. Any persistence error
 * permanently fences the transaction and revokes the attached M08 owner. */
typedef struct ucn_cluster_rekey_persist_owner {
    const ucn_cluster_persist_provider_t *provider;
    ucn_cluster_authority_runtime_t *authority;
    ucn_cluster_persist_state_t durable_state;
    ucn_cluster_persist_request_t pending_request;
    ucn_cluster_persist_token_t pending_token;
    uint8_t pending_action;
    bool io_active;
    bool pending;
    bool faulted;
} ucn_cluster_rekey_persist_owner_t;

/* Starts only from a byte-zero transaction object.  The caller must retain
 * the object for the transaction lifetime; there is deliberately no runtime
 * reset API that could erase a Fence or resurrect retired Authority.
 *
 * The function executes an M08 preflight at now_ms, then requires:
 *   - a live Stable Head Authority and live Stable Config quorum;
 *   - exact active/max Epoch and committed Config in a current-schema Record;
 *   - no Config/Rekey PREPARED state, Tombstone or persistence fault;
 *   - a product Cluster-ID Provider; default best-effort mixing is not a
 *     uniqueness proof for Rekey;
 *   - an exact history loaded from generation `loaded_history_generation`
 *     (zero is accepted only for the factory-empty history).
 *   - non-zero no-wrap transaction_id and nonce.
 * A successful call only reaches ID_HISTORY_DURABLE_REQUIRED. The caller must
 * encode, atomically store and reload the changed history, then call
 * confirm_id_history_durable(); PREPARE remains impossible before that proof.
 * On every rejection the transaction output is byte-for-byte unchanged. */
ucn_result_t ucn_cluster_rekey_transaction_begin(
    ucn_cluster_rekey_transaction_t *transaction,
    ucn_cluster_authority_runtime_t *authority,
    const ucn_cluster_persist_state_t *durable_state,
    const ucn_cluster_rekey_voter_profile_t *voter_profiles,
    size_t voter_profile_count,
    uint32_t transaction_id,
    uint32_t nonce,
    uint32_t now_ms,
    ucn_cluster_id_history_t *allocation_history,
    uint32_t loaded_history_generation);

void ucn_cluster_id_history_init(ucn_cluster_id_history_t *history);
ucn_result_t ucn_cluster_id_history_admit(
    ucn_cluster_id_history_t *history,
    const ucn_cluster_id_request_t *identity,
    uint32_t cluster_id);
ucn_result_t ucn_cluster_id_history_record_encode(
    const ucn_cluster_id_history_t *history,
    uint32_t generation,
    uint8_t *output,
    size_t output_capacity);
ucn_result_t ucn_cluster_id_history_record_decode(
    const uint8_t *record,
    size_t record_length,
    uint32_t *generation,
    ucn_cluster_id_history_t *history);
uint32_t ucn_cluster_id_history_fingerprint(
    const ucn_cluster_id_history_t *history,
    uint32_t generation);

/* The ID-history record must be committed and reloaded before Rekey PREPARE
 * can touch the Cluster persistence Provider. The proof is exact for the
 * complete canonical bounded history and its generation. */
ucn_result_t ucn_cluster_rekey_transaction_confirm_id_history_durable(
    ucn_cluster_rekey_transaction_t *transaction,
    const ucn_cluster_id_history_t *durable_history,
    uint32_t durable_generation);

bool ucn_cluster_rekey_transaction_is_valid(
    const ucn_cluster_rekey_transaction_t *transaction);

/* Pure no-wrap routing decision.  All present serials must be non-zero and
 * <= the reserved rotation threshold. Output is untouched on failure. */
ucn_result_t ucn_cluster_rekey_threshold_evaluate(
    const ucn_cluster_rekey_serial_view_t *view,
    ucn_cluster_rekey_threshold_decision_t *decision);

/* Raw RFC4 bridges for the isolated owner.  They construct/consume the
 * already-decoded 40-byte frame object only; they neither enable the byte
 * encoder nor register a production RX/TX/FSM path. */
ucn_result_t ucn_cluster_rekey_prepare_frame_build(
    const ucn_cluster_rekey_transaction_t *transaction,
    const ucn_cluster_persist_state_t *durable_state,
    uint32_t now_ms,
    ucn_cluster_wire_v4_frame_t *output);
ucn_result_t ucn_cluster_rekey_ack_frame_admit(
    const ucn_cluster_rekey_transaction_t *transaction,
    const ucn_cluster_wire_v4_frame_t *frame,
    ucn_node_id_t outer_source,
    ucn_cluster_rekey_ack_t *output);
ucn_result_t ucn_cluster_rekey_commit_frame_build(
    const ucn_cluster_rekey_transaction_t *transaction,
    const ucn_cluster_persist_state_t *durable_state,
    ucn_cluster_wire_v4_frame_t *output);

ucn_result_t ucn_cluster_rekey_transaction_begin_collection(
    ucn_cluster_rekey_transaction_t *transaction,
    const ucn_cluster_persist_state_t *durable_state);
/* Reconstructs the exact durable PREPARED transaction after a controlled
 * restart. ACK progress is intentionally not recovered: collection restarts
 * with the local Head vote only. The current Stable Authority/Config and
 * caller-supplied voter profiles are revalidated, while txid, nonce,
 * successor Epoch/Config, frozen Backup, allocation-history generation and
 * its complete fingerprint come only from Record v4. `durable_history` must
 * be the exact separately reloaded record for that generation. */
ucn_result_t ucn_cluster_rekey_transaction_resume_prepared(
    ucn_cluster_rekey_transaction_t *transaction,
    ucn_cluster_authority_runtime_t *authority,
    const ucn_cluster_persist_state_t *durable_state,
    const ucn_cluster_rekey_voter_profile_t *voter_profiles,
    size_t voter_profile_count,
    const ucn_cluster_id_history_t *durable_history,
    uint32_t durable_history_generation,
    uint32_t now_ms);
ucn_result_t ucn_cluster_rekey_transaction_note_ack(
    ucn_cluster_rekey_transaction_t *transaction,
    const ucn_cluster_persist_state_t *durable_state,
    const ucn_cluster_wire_v4_frame_t *frame,
    ucn_node_id_t outer_source,
    uint32_t now_ms);
bool ucn_cluster_rekey_transaction_quorum_reached(
    const ucn_cluster_rekey_transaction_t *transaction);
ucn_result_t ucn_cluster_rekey_transaction_step(
    ucn_cluster_rekey_transaction_t *transaction,
    uint32_t now_ms);

ucn_result_t ucn_cluster_rekey_persist_owner_init(
    ucn_cluster_rekey_persist_owner_t *owner,
    const ucn_cluster_persist_provider_t *provider,
    ucn_cluster_authority_runtime_t *authority);
const ucn_cluster_persist_state_t *ucn_cluster_rekey_persist_owner_state(
    const ucn_cluster_rekey_persist_owner_t *owner);
ucn_result_t ucn_cluster_rekey_persist_begin_prepare(
    ucn_cluster_rekey_persist_owner_t *owner,
    uint32_t operation_id,
    ucn_cluster_rekey_transaction_t *transaction,
    uint32_t now_ms,
    bool *durable);
ucn_result_t ucn_cluster_rekey_persist_begin_commit(
    ucn_cluster_rekey_persist_owner_t *owner,
    uint32_t operation_id,
    ucn_cluster_rekey_transaction_t *transaction,
    uint32_t now_ms,
    bool *durable);
ucn_result_t ucn_cluster_rekey_persist_begin_abort(
    ucn_cluster_rekey_persist_owner_t *owner,
    uint32_t operation_id,
    ucn_cluster_rekey_transaction_t *transaction,
    uint32_t now_ms,
    bool *durable);
ucn_result_t ucn_cluster_rekey_persist_poll(
    ucn_cluster_rekey_persist_owner_t *owner,
    ucn_cluster_rekey_transaction_t *transaction,
    uint32_t now_ms,
    bool *durable,
    ucn_cluster_rekey_persist_action_t *completed_action);

/* Returns UCN_ERR_REPLAY for every frame in the retired Cluster ID domain,
 * including frames with a numerically larger Term. A retired Cluster ID is
 * never reusable after Rekey. */
ucn_result_t ucn_cluster_rekey_tombstone_admit_frame(
    const ucn_cluster_persist_state_t *durable_state,
    const ucn_cluster_wire_v4_frame_t *frame);

ucn_result_t ucn_cluster_rekey_successor_materialize(
    const ucn_cluster_rekey_transaction_t *transaction,
    const ucn_cluster_persist_state_t *durable_state,
    ucn_cluster_rekey_successor_state_t *output);

#ifdef __cplusplus
}
#endif

#endif
