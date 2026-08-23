#ifndef UCN_CLUSTER_PERSIST_H
#define UCN_CLUSTER_PERSIST_H

/* CLV2-M04 (04-01): platform-neutral persistence contract.
 *
 * This header deliberately describes logical Cluster safety state, not a
 * Flash sector, file, RTOS object, or wear-leveling algorithm.  A product may
 * implement it with two Flash slots, FRAM, a secure element, a host fake, or
 * another atomic store.  The 04-02 codec below implements the on-media
 * magic/schema/generation/CRC format; providers may either use that
 * codec or prove an equivalent atomic, versioned representation.
 *
 * Safety contract (frozen for M04):
 *   1. load() completes before Cluster init may create an outward promise;
 *   2. submit() returns COMMITTED only after next_state is durable;
 *   3. PENDING has no outward protocol effect; the owner must poll() it;
 *   4. FAILED is fail-closed: no Advertise, ACK, Commit or new Term promise;
 *   5. an async provider copies request->next_state before submit() returns.
 */

#include "ucn/ucn_cluster.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UCN_CLUSTER_PERSIST_PROVIDER_API_VERSION ((uint16_t)1U)
#define UCN_CLUSTER_PERSIST_TOKEN_NONE UINT32_C(0)
#define UCN_CLUSTER_PERSIST_CONFIG_DIGEST_BYTES ((size_t)16U)
/* Physical record schemas: all fields are explicit big-endian bytes;
 * never persist a C struct image because alignment and bool layout vary by
 * toolchain. Schema v1/v2 are read-only 280 B legacy inputs. Schema v3 is
 * the only writer format: it keeps the first 280 B byte-for-byte compatible
 * with v2 and appends the three missing M10 VoteId fields. A product that
 * persists raw slots must therefore support a 292 B v3 slot (or explicitly
 * migrate its older 280 B slots before making a v3 write). */
#define UCN_CLUSTER_PERSIST_RECORD_MAGIC UINT32_C(0x55435052)
#define UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_LEGACY_V1 ((uint16_t)1U)
#define UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_LEGACY_V2 ((uint16_t)2U)
#define UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_CURRENT_V3 ((uint16_t)3U)
/* Kept as a source-level name for code that explicitly handles an on-media
 * v2 input. It is not a writer schema. */
#define UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_CURRENT_V2 \
    UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_LEGACY_V2
#define UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION \
    UCN_CLUSTER_PERSIST_RECORD_SCHEMA_VERSION_CURRENT_V3
#define UCN_CLUSTER_PERSIST_RECORD_HEADER_BYTES ((size_t)16U)
#define UCN_CLUSTER_PERSIST_RECORD_LEGACY_PAYLOAD_BYTES ((size_t)264U)
#define UCN_CLUSTER_PERSIST_RECORD_LEGACY_BYTES ((size_t)280U)
#define UCN_CLUSTER_PERSIST_RECORD_PAYLOAD_BYTES ((size_t)276U)
#define UCN_CLUSTER_PERSIST_RECORD_BYTES ((size_t)292U)

typedef uint32_t ucn_cluster_persist_token_t;

/* Schema-v3 can bind a vote to the complete M10 Takeover VoteId. Runtime v3
 * compatibility code may still create an older partial vote (all three M10
 * extension fields are zero); it is never accepted as M10 quorum proof.
 * This distinction lets the pre-existing, separately fenced v3 control path
 * remain readable while M10 refuses to turn it into a v4 takeover promise. */
typedef struct ucn_cluster_persist_vote {
    bool valid;
    ucn_cluster_epoch_t epoch;
    ucn_node_id_t voted_for_node_id;
    uint32_t backup_generation;
    uint32_t proposed_term;
    uint32_t config_id;
    uint32_t snapshot_id;
} ucn_cluster_persist_vote_t;

/* M07 owns the membership body.  M04 persists only its immutable committed
 * identity so a restart cannot claim a different configuration was committed.
 * config_id and generation are no-wrap serials: a new Config transaction
 * advances each one exactly once and never reuses an older identity.
 */
typedef struct ucn_cluster_persist_config_ref {
    bool valid;
    uint32_t config_id;
    uint32_t generation;
    uint8_t digest[UCN_CLUSTER_PERSIST_CONFIG_DIGEST_BYTES];
} ucn_cluster_persist_config_ref_t;

/* M13 owns Rekey wire/quorum details.  M04 nevertheless persists the whole
 * rekey identity now: a restart has to know both the old authority/config
 * binding and the exact successor Cluster/Epoch, not merely that a Rekey was
 * once prepared. */
typedef struct ucn_cluster_persist_rekey_ref {
    bool valid;
    uint32_t generation;
    uint32_t next_incarnation;
    ucn_cluster_epoch_t predecessor_epoch;
    ucn_cluster_persist_config_ref_t predecessor_config;
    ucn_cluster_epoch_t successor_epoch;
} ucn_cluster_persist_rekey_ref_t;

/* Transaction phase is invalid-zero-safe. NONE means no in-flight or
 * journaled transaction; PREPARED keeps C_new; COMMITTED retains both txid
 * and C_new. For CONFIG_COMMIT C_new equals committed_config; for
 * CONFIG_ABORT it is the strictly newer proposal that was atomically closed.
 * Retaining that identity binds terminal Abort replay after restart. */
typedef enum ucn_cluster_persist_transaction_phase {
    UCN_CLUSTER_PERSIST_TRANSACTION_INVALID = 0,
    UCN_CLUSTER_PERSIST_TRANSACTION_NONE = 1,
    UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED = 2,
    UCN_CLUSTER_PERSIST_TRANSACTION_COMMITTED = 3
} ucn_cluster_persist_transaction_phase_t;

typedef struct ucn_cluster_persist_config_transaction {
    ucn_cluster_persist_transaction_phase_t phase;
    uint32_t transaction_id;
    ucn_cluster_persist_config_ref_t staging_config;
} ucn_cluster_persist_config_transaction_t;

typedef struct ucn_cluster_persist_rekey_transaction {
    ucn_cluster_persist_transaction_phase_t phase;
    uint32_t transaction_id;
    ucn_cluster_persist_rekey_ref_t staging_rekey;
} ucn_cluster_persist_rekey_transaction_t;

/* A tombstone prevents a retired identity from becoming valid again after a
 * restart.  replacement_cluster_id is the non-zero successor Cluster ID from
 * the matching committed Rekey.  rekey_transaction_id binds a retired identity
 * to the committed Rekey
 * transaction that created it, so restart recovery cannot invent a pairing.
 */
typedef struct ucn_cluster_persist_tombstone {
    bool valid;
    ucn_cluster_epoch_t retired_epoch;
    uint32_t replacement_cluster_id;
    uint32_t rekey_transaction_id;
} ucn_cluster_persist_tombstone_t;

/* Full logical snapshot atomically supplied to the provider.  04-02 maps it
 * to a versioned CRC-protected physical record.  Fields with has_* false are
 * semantically absent, not a zero-valued valid Epoch.  When active_epoch is
 * present, max_epoch is present and byte-for-byte equal: a normal persisted
 * active identity may never lag its durable maximum. */
typedef struct ucn_cluster_persist_state {
    /* On-media schema provenance, populated by decode. It is not authority
     * state: v1/v2 exist only for controlled migration/readback. The public
     * writer and normal runtime writes emit v3. Its pre-release terminal
     * Config records retain C_new; older unpublished terminal records without
     * that identity are fail-closed. */
    uint16_t record_schema_version;
    bool has_active_epoch;
    ucn_cluster_epoch_t active_epoch;
    bool has_max_epoch;
    ucn_cluster_epoch_t max_epoch;
    ucn_cluster_persist_vote_t last_vote;
    ucn_cluster_persist_config_ref_t committed_config;
    ucn_cluster_persist_config_transaction_t config_transaction;
    ucn_cluster_persist_rekey_ref_t committed_rekey;
    ucn_cluster_persist_rekey_transaction_t rekey_transaction;
    ucn_cluster_persist_tombstone_t tombstone;
    /* A product allocates a fresh non-zero value once per controlled boot.
     * Together with volatile nonces this is the replay-incarnation boundary;
     * it is not a per-heartbeat Flash counter. */
    uint32_t boot_incarnation;
    /* Completed-operation journal.  It makes restart retry explicit:
     * same id + operation + canonical state is idempotent; any other reuse
     * of the id is rejected.  A zero id means that no operation was completed
     * in this persisted lineage. */
    uint32_t last_completed_operation_id;
    uint8_t last_completed_operation;
    uint32_t last_completed_operation_fingerprint;
} ucn_cluster_persist_state_t;

typedef enum ucn_cluster_persist_load_state {
    /* A zero-filled result is never a valid load outcome. */
    UCN_CLUSTER_PERSIST_LOAD_INVALID = 0,
    /* No valid record exists.  The returned snapshot must be zeroed. */
    UCN_CLUSTER_PERSIST_LOAD_FACTORY_EMPTY = 1,
    /* A complete logical snapshot was loaded and integrity-checked. */
    UCN_CLUSTER_PERSIST_LOAD_READY = 2
} ucn_cluster_persist_load_state_t;

typedef struct ucn_cluster_persist_load_result {
    ucn_cluster_persist_load_state_t state;
    ucn_cluster_persist_state_t snapshot;
} ucn_cluster_persist_load_result_t;

/* The operation class is an audit and policy boundary.  Every request still
 * carries the complete next snapshot so providers never expose torn logical
 * state between Epoch, Vote, Config, Rekey, Tombstone and incarnation writes.
 */
typedef enum ucn_cluster_persist_operation {
    UCN_CLUSTER_PERSIST_OPERATION_REPLAY_INCARNATION = 1,
    UCN_CLUSTER_PERSIST_OPERATION_EPOCH_COMMIT = 2,
    UCN_CLUSTER_PERSIST_OPERATION_VOTE_COMMIT = 3,
    UCN_CLUSTER_PERSIST_OPERATION_CONFIG_PREPARE = 4,
    UCN_CLUSTER_PERSIST_OPERATION_CONFIG_COMMIT = 5,
    UCN_CLUSTER_PERSIST_OPERATION_REKEY_PREPARE = 6,
    UCN_CLUSTER_PERSIST_OPERATION_REKEY_COMMIT = 7,
    /* A normal EPOCH_COMMIT never changes Cluster identity.  This explicit
     * operation is the only ordinary-election/recovery transition that may
     * replace an old Cluster with a newly allocated identity at Term 1.
     *
     * The owner may request it only after its runtime FSM has detached from
     * the old authority.  The durable operation preserves boot_incarnation,
     * clears only ordinary Vote/Config state, and writes the new Active/Max
     * Epoch atomically.  A state holding committed Rekey/Tombstone evidence is
     * deliberately rejected rather than erased: Record v1 cannot retain an
     * ancestor set, and CLV2-M12 owns the lineage representation required to
     * relax this fail-closed M04 boundary.  It does not by itself authorize any wire
     * promise; CLV2-04-05 remains responsible for that integration. */
    UCN_CLUSTER_PERSIST_OPERATION_CLUSTER_CREATE_COMMIT = 8,
    /* Reserved for diagnostics only.  A Tombstone has no standalone durable
     * transition: it is written only inside REKEY_COMMIT's next snapshot. */
    UCN_CLUSTER_PERSIST_OPERATION_TOMBSTONE_COMMIT = 9,
    /* Record-v1 snapshots created before the M04 public Hook fence may carry
     * one otherwise-valid CONFIG/REKEY PREPARED transaction.  M04 has no
     * public resume/commit owner for that transaction yet, so a controlled
     * REQUIRED boot may use this one-way migration only: atomically clear the
     * prepared transaction and advance boot_incarnation.  It is deliberately
     * not a public Config/Rekey abort API and may not change any authority,
     * Config, Rekey, Tombstone or Vote field. */
    UCN_CLUSTER_PERSIST_OPERATION_LEGACY_PREPARED_ABORT = 10,
    /* M07 timeout/owner Abort closes a v2 CONFIG_PREPARED transaction without
     * changing committed C_old. The completed transaction ID is retained as
     * COMMITTED journal state so the next Config transaction cannot reuse it. */
    UCN_CLUSTER_PERSIST_OPERATION_CONFIG_ABORT = 11,
    /* CONFIG_JOINT is a durable barrier between PREPARED(C_new) and
     * CONFIG_COMMIT.  It intentionally leaves the Config transaction in
     * PREPARED state, but journals that the exact txid/C_new pair entered the
     * Joint phase.  A reboot can therefore distinguish "prepared only" from
     * "joint was durably authorized" without granting an implicit commit. */
    UCN_CLUSTER_PERSIST_OPERATION_CONFIG_JOINT = 12,
    /* M10 persists an exact full VoteId before it can count the local vote,
     * then writes the proposed Epoch in a distinct atomic operation. Neither
     * operation has a production RX/TX/FSM call site while M05 is on hold. */
    UCN_CLUSTER_PERSIST_OPERATION_TAKEOVER_VOTE_COMMIT = 13,
    UCN_CLUSTER_PERSIST_OPERATION_TAKEOVER_EPOCH_COMMIT = 14,
    /* CLV2-M12.1 (MAJOR-1): a Recovery declaration creates a NEW identity
     * whose Epoch Term mirrors the captured parent Term (the published
     * authority term), so the durable promise and the wire promise can
     * never diverge.  Ordinary elections keep CLUSTER_CREATE_COMMIT and
     * its Term-1 rule.  Only the RECOVERY_DECLARE owner may request this
     * operation; its transition validator applies the same ordinary-state
     * clear policy as CLUSTER_CREATE_COMMIT. */
    UCN_CLUSTER_PERSIST_OPERATION_RECOVERY_CREATE_COMMIT = 15
} ucn_cluster_persist_operation_t;

typedef struct ucn_cluster_persist_request {
    /* Owner-generated, non-zero and not reused while a request is live.
     * Repeating an already committed ID is permitted only with byte-identical
     * operation and next_state, which lets a product recover after reset. */
    uint32_t operation_id;
    ucn_cluster_persist_operation_t operation;
    ucn_cluster_persist_state_t next_state;
} ucn_cluster_persist_request_t;

typedef enum ucn_cluster_persist_completion_state {
    /* A zero-filled completion is never a valid provider outcome. */
    UCN_CLUSTER_PERSIST_COMPLETION_INVALID = 0,
    UCN_CLUSTER_PERSIST_COMMITTED = 1,
    UCN_CLUSTER_PERSIST_PENDING = 2,
    UCN_CLUSTER_PERSIST_FAILED = 3
} ucn_cluster_persist_completion_state_t;

typedef struct ucn_cluster_persist_completion {
    ucn_cluster_persist_completion_state_t state;
    /* Only PENDING owns a non-zero token. */
    ucn_cluster_persist_token_t token;
    /* Only FAILED carries a non-UCN_OK cause.  Providers map device-specific
     * errors to the closest UCN error without exposing a Flash SDK here. */
    ucn_result_t failure;
} ucn_cluster_persist_completion_t;

/* load() is deliberately synchronous: init must not create or transmit any
 * Cluster authority before it knows whether durable safety state exists.
 * Return UCN_OK for FACTORY_EMPTY or READY; return an error for I/O, CRC,
 * version or policy failure. */
typedef ucn_result_t (*ucn_cluster_persist_load_fn)(
    void *context,
    ucn_cluster_persist_load_result_t *result);

/* submit() copies the full request before returning.  COMMITTED means durable
 * now; PENDING means poll() must later produce COMMITTED or FAILED; FAILED
 * means no part of next_state may be treated as a protocol promise. */
typedef ucn_cluster_persist_completion_t (*ucn_cluster_persist_submit_fn)(
    void *context,
    const ucn_cluster_persist_request_t *request);

typedef ucn_cluster_persist_completion_t (*ucn_cluster_persist_poll_fn)(
    void *context,
    ucn_cluster_persist_token_t token);

typedef struct ucn_cluster_persist_provider {
    /* Versioned prefix; a future incompatible layout increments api_version.
     * Products should use designated initializers because this API is still
     * pre-release and deliberately safety-sensitive. */
    uint16_t struct_size;
    uint16_t api_version;
    ucn_cluster_persist_load_fn load;
    ucn_cluster_persist_submit_fn submit;
    /* Optional only for a provider whose submit() never returns PENDING. */
    ucn_cluster_persist_poll_fn poll;
    void *context;
} ucn_cluster_persist_provider_t;

static inline bool ucn_cluster_persist_provider_is_compatible(
    const ucn_cluster_persist_provider_t *provider)
{
    return provider != NULL &&
           provider->struct_size >=
               (uint16_t)sizeof(ucn_cluster_persist_provider_t) &&
           provider->api_version == UCN_CLUSTER_PERSIST_PROVIDER_API_VERSION &&
           provider->load != NULL && provider->submit != NULL;
}

static inline bool ucn_cluster_persist_provider_supports_async(
    const ucn_cluster_persist_provider_t *provider)
{
    return ucn_cluster_persist_provider_is_compatible(provider) &&
           provider->poll != NULL;
}

static inline bool ucn_cluster_persist_completion_is_valid(
    const ucn_cluster_persist_completion_t *completion)
{
    if (completion == NULL) {
        return false;
    }
    if (completion->state == UCN_CLUSTER_PERSIST_COMMITTED) {
        return completion->token == UCN_CLUSTER_PERSIST_TOKEN_NONE &&
               completion->failure == UCN_OK;
    }
    if (completion->state == UCN_CLUSTER_PERSIST_PENDING) {
        return completion->token != UCN_CLUSTER_PERSIST_TOKEN_NONE &&
               completion->failure == UCN_OK;
    }
    return completion->state == UCN_CLUSTER_PERSIST_FAILED &&
           completion->token == UCN_CLUSTER_PERSIST_TOKEN_NONE &&
           completion->failure != UCN_OK;
}

/* An owner must reject an impossible asynchronous response before it can
 * become observable.  In particular, a synchronous provider without poll()
 * cannot return PENDING. */
static inline bool ucn_cluster_persist_provider_accepts_completion(
    const ucn_cluster_persist_provider_t *provider,
    const ucn_cluster_persist_completion_t *completion)
{
    return ucn_cluster_persist_completion_is_valid(completion) &&
           (completion->state != UCN_CLUSTER_PERSIST_PENDING ||
            ucn_cluster_persist_provider_supports_async(provider));
}

/* Initializes a logically empty but READY-valid snapshot.  This differs from
 * a Factory Empty load result, whose complete snapshot must be all zero. */
void ucn_cluster_persist_state_init_empty(ucn_cluster_persist_state_t *state);
bool ucn_cluster_persist_state_is_valid(
    const ucn_cluster_persist_state_t *state);
/* True only for the schema-v3 representation of M10's complete VoteId.
 * A valid partial legacy vote deliberately returns false. */
bool ucn_cluster_persist_vote_is_complete_takeover(
    const ucn_cluster_persist_vote_t *vote);
bool ucn_cluster_persist_load_result_is_valid(
    const ucn_cluster_persist_load_result_t *result);

/* Finalize copies the canonical request identity into next_state's completed
 * journal.  Providers and owners validate it before accepting a write.
 * request_admit() is deterministic across restart because both the completed
 * operation ID and its canonical fingerprint are persisted. */
ucn_result_t ucn_cluster_persist_request_finalize(
    ucn_cluster_persist_request_t *request);
bool ucn_cluster_persist_request_is_valid(
    const ucn_cluster_persist_request_t *request);
typedef enum ucn_cluster_persist_request_admission {
    UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_INVALID = 0,
    UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_NEW = 1,
    UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_IDEMPOTENT = 2,
    UCN_CLUSTER_PERSIST_REQUEST_ADMISSION_REJECTED = 3
} ucn_cluster_persist_request_admission_t;
ucn_cluster_persist_request_admission_t ucn_cluster_persist_request_admit(
    const ucn_cluster_persist_state_t *committed_state,
    const ucn_cluster_persist_request_t *request);

/* M04-07 owner hooks.  M07 (Config) and M13 (Rekey) call these instead of
 * issuing Provider requests themselves, so their future wire/FSM work cannot
 * bypass the same submit/poll/reload proof used by Election and Vote.
 *
 * committed=false with UCN_OK means the request is durably pending; the owner
 * must retain its subtransaction, call ucn_cluster_step(), and inspect its own
 * FSM only after the pending flag clears.  M04 deliberately does not invent a
 * Config/Rekey role transition or wire message.  These hooks require
 * REQUIRED mode: VOLATILE_TEST has no durable staging state and returns
 * UCN_ERR_CONFIG rather than falsely accepting a Prepare that cannot Commit.
 * Until M07/M13 provide the complete PREPARE recovery (resume/abort/commit)
 * and runtime continuations, **all four public Config/Rekey Hooks return
 * UCN_ERR_CONFIG before Provider I/O**.  M04 must never create an orphaned
 * durable PREPARED state, make a new durable contract, or resume an old RAM
 * FSM after ACTION_NONE.  Record-v1 transition helpers remain available to
 * the future owning milestones' dedicated tests and Provider implementation.
 */
ucn_result_t ucn_cluster_persist_config_prepare(
    ucn_cluster_t *cluster,
    uint32_t transaction_id,
    const ucn_cluster_persist_config_ref_t *staging_config,
    bool *committed);
ucn_result_t ucn_cluster_persist_config_commit(
    ucn_cluster_t *cluster,
    uint32_t transaction_id,
    bool *committed);
ucn_result_t ucn_cluster_persist_rekey_prepare(
    ucn_cluster_t *cluster,
    uint32_t transaction_id,
    const ucn_cluster_persist_rekey_ref_t *staging_rekey,
    bool *committed);
ucn_result_t ucn_cluster_persist_rekey_commit(
    ucn_cluster_t *cluster,
    uint32_t transaction_id,
    bool *committed);

/* M04-02 record codec.  A product's provider can use these helpers for each
 * physical slot, then choose the largest valid non-wrapping generation.  The
 * codec does no I/O and performs no allocation.  decode() distinguishes a
 * fully erased/zero factory slot (UCN_ERR_NOT_FOUND) from malformed records,
 * unsupported schema (UCN_ERR_VERSION), and CRC damage (UCN_ERR_CRC). */
bool ucn_cluster_persist_record_is_factory_empty(
    const uint8_t *record,
    size_t record_length);
ucn_result_t ucn_cluster_persist_record_generation_next(
    uint32_t current_generation,
    uint32_t *next_generation);
bool ucn_cluster_persist_record_generation_is_newer(
    uint32_t candidate_generation,
    uint32_t baseline_generation);
ucn_result_t ucn_cluster_persist_record_encode(
    const ucn_cluster_persist_state_t *state,
    uint32_t record_generation,
    uint8_t *output,
    size_t output_capacity);
ucn_result_t ucn_cluster_persist_record_decode(
    const uint8_t *record,
    size_t record_length,
    uint32_t *record_generation,
    ucn_cluster_persist_state_t *state);

#ifdef __cplusplus
}
#endif

#endif /* UCN_CLUSTER_PERSIST_H */
