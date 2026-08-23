#ifndef UCN_CLUSTER_TAKEOVER_INTERNAL_H
#define UCN_CLUSTER_TAKEOVER_INTERNAL_H

/* M10 private bridge between the frozen transaction model and its persistence
 * owner. These transitions are deliberately absent from the public header:
 * a normal caller cannot turn a RAM vote/quorum into an experimental Head
 * result without the owner first reload-proving the exact durable Record.
 *
 * Test code may include this header to model an already-proven Provider
 * result; production Cluster/Adapter code must not include it. */

#include "ucn/ucn_cluster_takeover.h"

ucn_result_t ucn_cluster_takeover_transaction_mark_self_vote_durable_internal(
    ucn_cluster_takeover_transaction_t *transaction,
    const ucn_cluster_takeover_vote_id_t *vote_id);
ucn_result_t ucn_cluster_takeover_transaction_mark_epoch_durable_internal(
    ucn_cluster_takeover_transaction_t *transaction,
    const ucn_cluster_takeover_vote_id_t *vote_id);

#endif
