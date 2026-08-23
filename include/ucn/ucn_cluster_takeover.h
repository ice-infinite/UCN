#ifndef UCN_CLUSTER_TAKEOVER_H
#define UCN_CLUSTER_TAKEOVER_H

/* This is an opt-in experimental API, not a default product Cluster API.
 * CMake's ucn_cluster_takeover_experimental target exports this definition to
 * its sources and consumers. Reject a bare include early instead of letting a
 * default-product link discover a missing optional archive later. */
#if !defined(UCN_CLUSTER_TAKEOVER_EXPERIMENTAL_ENABLED)
#error "Include ucn_cluster_takeover.h only through ucn_cluster_takeover_experimental"
#endif

/* CLV2-M10: frozen-majority Takeover value model.
 *
 * This header is intentionally caller-owned and wire-agnostic.  It consumes
 * an already verified M09 committed Backup snapshot and an M07 canonical
 * Config State, then records an exact VoteId and deterministic certificate.
 * It neither parses/sends a v4 frame nor modifies ucn_cluster_t, Authority or
 * Adapter state.  A later, separately audited RX/TX owner may bridge the
 * model to RFC4 Type 8/14/15/33.
 */

#include "ucn/ucn_cluster_backup_sync.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UCN_CLUSTER_TAKEOVER_SET_OLD ((uint8_t)0x01U)
#define UCN_CLUSTER_TAKEOVER_SET_NEW ((uint8_t)0x02U)
#define UCN_CLUSTER_TAKEOVER_MAX_CERTIFICATE_WORDS \
    ((UCN_CLUSTER_MAX_VOTERS + (size_t)31U) / (size_t)32U)

typedef char ucn_cluster_takeover_certificate_words_are_bounded[
    UCN_CLUSTER_TAKEOVER_MAX_CERTIFICATE_WORDS >= (size_t)1U &&
            UCN_CLUSTER_TAKEOVER_MAX_CERTIFICATE_WORDS <= (size_t)2U ?
        1 :
        -1];

/* Exact durable promise identity.  `old_term` is the frozen Backup Epoch
 * term; `proposed_term` is stored explicitly even though it must be its exact
 * checked successor.  A Config/snapshot/generation change therefore cannot
 * be mistaken for an idempotent vote in the same numeric Term. */
typedef struct ucn_cluster_takeover_vote_id {
    uint32_t cluster_id;
    uint32_t old_term;
    uint32_t proposed_term;
    uint32_t config_id;
    ucn_node_id_t backup_node_id;
    uint32_t backup_generation;
    uint32_t snapshot_id;
} ucn_cluster_takeover_vote_id_t;

typedef enum ucn_cluster_takeover_state {
    UCN_CLUSTER_TAKEOVER_STATE_NONE = 0,
    UCN_CLUSTER_TAKEOVER_STATE_COLLECTING = 1,
    UCN_CLUSTER_TAKEOVER_STATE_QUORUM = 2,
    UCN_CLUSTER_TAKEOVER_STATE_EPOCH_DURABLE = 3,
    UCN_CLUSTER_TAKEOVER_STATE_ABORTED = 4
} ucn_cluster_takeover_state_t;

/* One fragment maps directly to an RFC4 Type 33 semantic payload.  The
 * caller determines its raw frame/flags only in a separately gated test or
 * production owner; this value itself has no wire side effect. */
typedef struct ucn_cluster_takeover_certificate_fragment {
    uint8_t set_mask;
    uint8_t fragment_index;
    uint8_t fragment_count;
    uint32_t config_id;
    uint32_t config_hash;
    uint32_t vote_bitmap_word;
} ucn_cluster_takeover_certificate_fragment_t;

typedef struct ucn_cluster_takeover_certificate {
    ucn_cluster_takeover_vote_id_t vote_id;
    uint32_t certificate_anchor_config_id;
    uint8_t required_set_mask;
    uint32_t old_vote_words[UCN_CLUSTER_TAKEOVER_MAX_CERTIFICATE_WORDS];
    uint32_t new_vote_words[UCN_CLUSTER_TAKEOVER_MAX_CERTIFICATE_WORDS];
    uint32_t canonical_crc32;
} ucn_cluster_takeover_certificate_t;

/* The transaction owns no mutable Runtime member table.  All denominator and
 * bit positions are frozen copies of the M09/M07 proof at begin time. */
typedef struct ucn_cluster_takeover_transaction {
    ucn_cluster_snapshot_epoch_t frozen_snapshot_epoch;
    ucn_cluster_config_state_t frozen_config;
    ucn_cluster_takeover_vote_id_t vote_id;
    ucn_cluster_epoch_t proposed_epoch;
    uint32_t deadline_ms;
    /* Exact M10 operation identity.  Persistent anti-replay is owned by the
     * M10 persistence journal; this caller-owned value model deliberately
     * never reads a previous transaction object before it has constructed a
     * complete candidate. */
    uint32_t transaction_id;
    uint32_t old_vote_words[UCN_CLUSTER_TAKEOVER_MAX_CERTIFICATE_WORDS];
    uint32_t new_vote_words[UCN_CLUSTER_TAKEOVER_MAX_CERTIFICATE_WORDS];
    uint32_t old_unreachable_words[UCN_CLUSTER_TAKEOVER_MAX_CERTIFICATE_WORDS];
    uint32_t new_unreachable_words[UCN_CLUSTER_TAKEOVER_MAX_CERTIFICATE_WORDS];
    bool self_vote_durable;
    bool active;
    bool recovery_required;
    bool proposed_epoch_durable;
    uint8_t state;
} ucn_cluster_takeover_transaction_t;

/* The future RX owner supplies these facts only after its Member grace and
 * local record gate.  This value model never derives them from role/source or
 * a v3 message. */
typedef struct ucn_cluster_takeover_member_vote_context {
    ucn_node_id_t voter_node_id;
    bool member_takeover_grace;
    bool old_head_lease_expired;
    bool committed_v4_voter;
} ucn_cluster_takeover_member_vote_context_t;

/* Remote Vote evidence is deliberately stronger than a raw source Node ID.
 * The future RX owner has to establish all fields before the frozen bitmap can
 * be updated.  This prevents a v3/provisional/ACTIVE member from becoming a
 * silent quorum voter merely because it used a numerically valid Node ID. */
typedef struct ucn_cluster_takeover_remote_vote_proof {
    ucn_cluster_takeover_member_vote_context_t member;
    bool exact_vote_durable;
} ucn_cluster_takeover_remote_vote_proof_t;

typedef struct ucn_cluster_takeover_old_primary_fence {
    ucn_cluster_epoch_t accepted_epoch;
    uint32_t accepted_certificate_crc32;
    bool fenced;
    bool join_required;
} ucn_cluster_takeover_old_primary_fence_t;

void ucn_cluster_takeover_transaction_reset(
    ucn_cluster_takeover_transaction_t *transaction);
bool ucn_cluster_takeover_vote_id_is_valid(
    const ucn_cluster_takeover_vote_id_t *vote_id);
bool ucn_cluster_takeover_vote_id_is_exact(
    const ucn_cluster_takeover_vote_id_t *left,
    const ucn_cluster_takeover_vote_id_t *right);
bool ucn_cluster_takeover_transaction_is_active(
    const ucn_cluster_takeover_transaction_t *transaction);

/* Starts only from M09's exact committed snapshot.  A staging refresh may be
 * open, but it never replaces this frozen proof.  `takeover_window_ms` must
 * be a non-zero bounded modular-clock duration.  It intentionally does not
 * inspect previous storage, so a fresh caller-owned object is safe; M10's
 * durable operation journal, not this transient object, prevents txid reuse
 * across reset/restart. */
ucn_result_t ucn_cluster_takeover_transaction_begin(
    ucn_cluster_takeover_transaction_t *transaction,
    const ucn_cluster_backup_sync_owner_t *backup_owner,
    uint32_t takeover_transaction_id,
    uint32_t now_ms,
    uint32_t takeover_window_ms);

/* Remote votes are accepted only when their exact durable VoteId and the
 * complete M10 member-grace proof have already been verified by the caller.
 * A duplicate same voter is idempotent; an out-of-set, v3/provisional/ACTIVE,
 * expired-proof or mismatching VoteId never changes the transaction. */
ucn_result_t ucn_cluster_takeover_transaction_note_durable_vote(
    ucn_cluster_takeover_transaction_t *transaction,
    const ucn_cluster_takeover_vote_id_t *vote_id,
    const ucn_cluster_takeover_remote_vote_proof_t *proof);

/* Rejects MEMBER_ACTIVE, unexpired Head lease, provisional and v3 voters.
 * This is a pure gate and deliberately does not persist or record a vote. */
ucn_result_t ucn_cluster_takeover_member_vote_gate(
    const ucn_cluster_takeover_transaction_t *transaction,
    const ucn_cluster_takeover_member_vote_context_t *context);

bool ucn_cluster_takeover_transaction_quorum_reached(
    const ucn_cluster_takeover_transaction_t *transaction);
bool ucn_cluster_takeover_transaction_quorum_possible(
    const ucn_cluster_takeover_transaction_t *transaction);

/* Marks a known frozen voter unreachable without changing the denominator.
 * If one required set can no longer reach quorum, this atomically aborts the
 * transaction and exposes `recovery_required`. */
ucn_result_t ucn_cluster_takeover_transaction_note_voter_unreachable(
    ucn_cluster_takeover_transaction_t *transaction,
    ucn_node_id_t voter_node_id);

/* Time-driven abort.  At deadline or after an impossible quorum the frozen
 * denominator remains intact; callers must create a new transaction/recovery
 * lineage rather than reuse a reduced quorum. */
ucn_result_t ucn_cluster_takeover_transaction_step(
    ucn_cluster_takeover_transaction_t *transaction,
    uint32_t now_ms);

/* Builds/verifies the complete canonical certificate.  It is intentionally
 * independent of the M05 pending cache; a later RX owner must still collect
 * and validate RFC4 Type 33 fragments before calling this proof. */
ucn_result_t ucn_cluster_takeover_certificate_build(
    const ucn_cluster_takeover_transaction_t *transaction,
    ucn_cluster_takeover_certificate_t *output);
bool ucn_cluster_takeover_certificate_is_valid(
    const ucn_cluster_takeover_transaction_t *transaction,
    const ucn_cluster_takeover_certificate_t *certificate);
ucn_result_t ucn_cluster_takeover_certificate_fragment_get(
    const ucn_cluster_takeover_transaction_t *transaction,
    const ucn_cluster_takeover_certificate_t *certificate,
    uint8_t set_mask,
    uint8_t fragment_index,
    ucn_cluster_takeover_certificate_fragment_t *output);
bool ucn_cluster_takeover_certificate_fragment_is_valid(
    const ucn_cluster_takeover_transaction_t *transaction,
    const ucn_cluster_takeover_certificate_t *certificate,
    const ucn_cluster_takeover_certificate_fragment_t *fragment);

bool ucn_cluster_takeover_transaction_head_result_ready(
    const ucn_cluster_takeover_transaction_t *transaction);

void ucn_cluster_takeover_old_primary_fence_reset(
    ucn_cluster_takeover_old_primary_fence_t *fence);
/* A valid higher same-Cluster certificate permanently fences the exact old
 * Primary Epoch and requests Join.  score/node preference cannot clear this
 * fence.  A certificate for another Cluster or a non-successor Term is never
 * enough to fence a Primary. */
ucn_result_t ucn_cluster_takeover_old_primary_fence_accept(
    ucn_cluster_takeover_old_primary_fence_t *fence,
    const ucn_cluster_epoch_t *old_primary_epoch,
    const ucn_cluster_takeover_transaction_t *transaction,
    const ucn_cluster_takeover_certificate_t *certificate);

#ifdef __cplusplus
}
#endif

#endif
