#ifndef UCN_CLUSTER_CONFIG_PROPOSAL_H
#define UCN_CLUSTER_CONFIG_PROPOSAL_H

/* CLV2-07-03: bounded Config-add proposal planner.
 *
 * This is deliberately a test/experiment value-layer coordinator. It turns
 * one already-admitted v4 PROVISIONAL record into a C_new candidate and
 * records the Head self ACK in a config_tx. It never mutates that Runtime
 * record, sends CONFIG_BEGIN/MEMBER, invokes a Provider, or grants voting.
 */

#include "ucn/ucn_cluster_config_tx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Begins a single add transaction for a valid v4 PROVISIONAL remote member.
 * `head_node_id` must be part of C_old. `voter_capacity` includes the Head.
 * Outputs remain untouched on any failure. */
ucn_result_t ucn_cluster_config_tx_begin_add_provisional(
    ucn_cluster_config_tx_t *tx,
    uint32_t transaction_id,
    ucn_node_id_t head_node_id,
    const ucn_cluster_member_t *provisional_member,
    const ucn_cluster_config_state_t *stable_old,
    uint8_t voter_capacity,
    uint32_t deadline_ms);

/* Marks one v4 committed voter as REMOVING without changing voting, the
 * active voter set, or any transaction. It remains in C_old until a later
 * durable Config commit. */
ucn_result_t ucn_cluster_config_member_mark_removing(
    ucn_cluster_member_t *member);

/* Builds C_new without an already-marked REMOVING voter, while keeping that
 * voter in C_old and recording the retained Head self ACK. The Head itself
 * cannot be removed by this planner. Inputs are unchanged on failure. */
ucn_result_t ucn_cluster_config_tx_begin_remove_marked(
    ucn_cluster_config_tx_t *tx,
    uint32_t transaction_id,
    ucn_node_id_t head_node_id,
    const ucn_cluster_member_t *removing_member,
    const ucn_cluster_config_state_t *stable_old,
    uint32_t deadline_ms);

#ifdef __cplusplus
}
#endif

#endif
