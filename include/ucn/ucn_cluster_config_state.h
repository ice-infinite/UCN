#ifndef UCN_CLUSTER_CONFIG_STATE_H
#define UCN_CLUSTER_CONFIG_STATE_H

/* CLV2-07-01: bounded, canonical Config State value model.
 *
 * This header describes only C_old/C_joint/C_new membership identities. It
 * deliberately does not include the v4 wire header, create authority, modify
 * Runtime member records, or send a frame. M07 runtime/FSM integration stays
 * in explicitly named test/experiment targets until M05 is released. */

#include "ucn/ucn_cluster.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ucn_cluster_config_phase {
    UCN_CLUSTER_CONFIG_PHASE_INVALID = 0,
    UCN_CLUSTER_CONFIG_PHASE_STABLE = 1,
    UCN_CLUSTER_CONFIG_PHASE_JOINT = 2
} ucn_cluster_config_phase_t;

/* Fixed canonical serialization stores both voter-set identities, their
 * hashes and all bounded Node IDs. It is a local persistence/test value, not
 * an RFC4 frame or a quorum certificate. */
#define UCN_CLUSTER_CONFIG_STATE_SERIALIZED_BYTES \
    ((size_t)24U + ((size_t)8U * UCN_CLUSTER_MAX_VOTERS))

typedef struct ucn_cluster_config_state {
    /* The active/configuration target identity. STABLE: equals both set IDs.
     * JOINT: equals C_new while old_set remains C_old. */
    uint32_t config_id;
    uint32_t old_set_hash;
    uint32_t new_set_hash;
    uint8_t phase;
    ucn_cluster_voter_set_t old_set;
    ucn_cluster_voter_set_t new_set;
} ucn_cluster_config_state_t;

bool ucn_cluster_config_phase_is_valid(ucn_cluster_config_phase_t phase);
bool ucn_cluster_config_state_is_valid(
    const ucn_cluster_config_state_t *state);

/* M13 hand-off predicate.  A Stable Config at the reserved serial boundary
 * remains readable, but no next Config may be created under the same Cluster
 * identity.  Until M13 provides a quorum-persisted Rekey, callers fail
 * closed rather than wrap config_id back to one. */
bool ucn_cluster_config_state_rekey_required(
    const ucn_cluster_config_state_t *state);

/* Builds C_old == C_new. Output is untouched on failure. */
bool ucn_cluster_config_state_init_stable(
    ucn_cluster_config_state_t *output,
    uint32_t config_id,
    const ucn_node_id_t *voter_node_ids,
    size_t voter_count);

/* Builds C_old -> C_new from an existing canonical Stable state. C_new is
 * checked-next only; reaching the serial rotation threshold fails closed. */
bool ucn_cluster_config_state_init_joint(
    ucn_cluster_config_state_t *output,
    const ucn_cluster_config_state_t *stable_old,
    const ucn_node_id_t *new_voter_node_ids,
    size_t new_voter_count);

/* Stable promotion is intentionally a pure value operation. It does not
 * persist, alter member tables or authorize a CONFIG_COMMIT wire action. */
bool ucn_cluster_config_state_promote_joint(
    ucn_cluster_config_state_t *output,
    const ucn_cluster_config_state_t *joint);

/* Returns zero for an invalid state. The hash covers phase, Config IDs,
 * stored set hashes/counts and canonical Node IDs in network byte order. */
uint32_t ucn_cluster_config_state_hash(
    const ucn_cluster_config_state_t *state);

/* Output is untouched on invalid input or insufficient capacity. */
ucn_result_t ucn_cluster_config_state_serialize(
    const ucn_cluster_config_state_t *state,
    uint8_t *output,
    size_t output_capacity);

/* Decodes only the exact canonical serialization emitted above. Reserved and
 * unused bytes must be zero; output stays untouched on every failure. */
ucn_result_t ucn_cluster_config_state_deserialize(
    const uint8_t *input,
    size_t input_length,
    ucn_cluster_config_state_t *output);

#ifdef __cplusplus
}
#endif

#endif
