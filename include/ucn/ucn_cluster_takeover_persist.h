#ifndef UCN_CLUSTER_TAKEOVER_PERSIST_H
#define UCN_CLUSTER_TAKEOVER_PERSIST_H

/* See ucn_cluster_takeover.h: this API exists only in the explicit M10
 * experimental target and must never be consumed by the default product. */
#if !defined(UCN_CLUSTER_TAKEOVER_EXPERIMENTAL_ENABLED)
#error "Include ucn_cluster_takeover_persist.h only through ucn_cluster_takeover_experimental"
#endif

/* CLV2-M10 (10-02/10-06): persistence request builder for the isolated
 * frozen-majority takeover model.  It owns no Provider callback, wire frame,
 * ucn_cluster_t or Authority state.  A controlled test/experiment owner must
 * submit the returned request, reload the durable snapshot, prove the exact
 * result through the match helpers, and only then advance the RAM model. */

#include "ucn/ucn_cluster_persist.h"
#include "ucn/ucn_cluster_takeover.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ucn_cluster_takeover_persist_phase {
    UCN_CLUSTER_TAKEOVER_PERSIST_PHASE_NONE = 0,
    UCN_CLUSTER_TAKEOVER_PERSIST_PHASE_VOTE = 1,
    UCN_CLUSTER_TAKEOVER_PERSIST_PHASE_EPOCH = 2
} ucn_cluster_takeover_persist_phase_t;

/* Controlled M10 Provider owner. It is intentionally separate from
 * ucn_cluster_t: it performs only submit/poll/reload proof for a frozen M10
 * transaction. `faulted` means a Provider/record contract failure, not an
 * ordinary transport backpressure condition. */
typedef struct ucn_cluster_takeover_persist_owner {
    const ucn_cluster_persist_provider_t *provider;
    ucn_cluster_persist_state_t durable_state;
    ucn_cluster_persist_request_t pending_request;
    ucn_cluster_persist_token_t pending_token;
    uint32_t persistence_failures;
    bool initialized;
    bool pending;
    bool io_active;
    bool faulted;
    uint8_t phase;
} ucn_cluster_takeover_persist_owner_t;

/* Performs synchronous Provider load before publishing an initialized owner.
 * Only READY snapshots are accepted: factory-empty storage must first finish
 * the existing M04 incarnation/Cluster creation contract. Output is untouched
 * on failure. */
ucn_result_t ucn_cluster_takeover_persist_owner_init(
    ucn_cluster_takeover_persist_owner_t *owner,
    const ucn_cluster_persist_provider_t *provider);
bool ucn_cluster_takeover_persist_owner_is_valid(
    const ucn_cluster_takeover_persist_owner_t *owner);

/* Begin either atomic M10 write. `committed=false` means Provider PENDING;
 * neither function changes the transaction or creates a Head result. Call
 * step() until pending clears, then use the exact durable-proof helpers. */
ucn_result_t ucn_cluster_takeover_persist_owner_begin_vote(
    ucn_cluster_takeover_persist_owner_t *owner,
    const ucn_cluster_takeover_transaction_t *transaction,
    uint32_t operation_id,
    bool *committed);
ucn_result_t ucn_cluster_takeover_persist_owner_begin_epoch(
    ucn_cluster_takeover_persist_owner_t *owner,
    const ucn_cluster_takeover_transaction_t *transaction,
    uint32_t operation_id,
    bool *committed);
ucn_result_t ucn_cluster_takeover_persist_owner_step(
    ucn_cluster_takeover_persist_owner_t *owner,
    const ucn_cluster_takeover_transaction_t *transaction);
bool ucn_cluster_takeover_persist_owner_vote_is_durable(
    const ucn_cluster_takeover_persist_owner_t *owner,
    const ucn_cluster_takeover_transaction_t *transaction);
bool ucn_cluster_takeover_persist_owner_epoch_is_durable(
    const ucn_cluster_takeover_persist_owner_t *owner,
    const ucn_cluster_takeover_transaction_t *transaction);

/* The only public progression bridge. Each function rechecks the owner's
 * reload-proven durable Record before advancing the transaction. A failed or
 * pending Provider operation leaves the transaction unchanged. */
ucn_result_t ucn_cluster_takeover_persist_owner_apply_durable_vote(
    const ucn_cluster_takeover_persist_owner_t *owner,
    ucn_cluster_takeover_transaction_t *transaction);
ucn_result_t ucn_cluster_takeover_persist_owner_apply_durable_epoch(
    const ucn_cluster_takeover_persist_owner_t *owner,
    ucn_cluster_takeover_transaction_t *transaction);

/* Builds TAKEOVER_VOTE_COMMIT.  The request atomically records the complete
 * VoteId while retaining the frozen old Active/Max Epoch.  Output is never
 * written on failure. */
ucn_result_t ucn_cluster_takeover_persist_vote_request_build(
    const ucn_cluster_persist_state_t *committed_state,
    const ucn_cluster_takeover_transaction_t *transaction,
    uint32_t operation_id,
    ucn_cluster_persist_request_t *output);

/* True only after a Provider reload returns the exact full VoteId associated
 * with this transaction; a partial v3 legacy Vote deliberately does not
 * match. */
bool ucn_cluster_takeover_persist_vote_matches(
    const ucn_cluster_persist_state_t *durable_state,
    const ucn_cluster_takeover_transaction_t *transaction);

/* Builds TAKEOVER_EPOCH_COMMIT only after the caller-owned transaction has a
 * quorum and the current durable snapshot proves the exact full VoteId.
 * This is the persist-before-Head-result barrier. */
ucn_result_t ucn_cluster_takeover_persist_epoch_request_build(
    const ucn_cluster_persist_state_t *committed_state,
    const ucn_cluster_takeover_transaction_t *transaction,
    uint32_t operation_id,
    ucn_cluster_persist_request_t *output);

bool ucn_cluster_takeover_persist_epoch_matches(
    const ucn_cluster_persist_state_t *durable_state,
    const ucn_cluster_takeover_transaction_t *transaction);

#ifdef __cplusplus
}
#endif

#endif
