#ifndef UCN_CLUSTER_INVARIANT_H
#define UCN_CLUSTER_INVARIANT_H

/* CLV2-14-04: read-only Target Safety diagnostic engine.
 *
 * The checker never repairs or advances protocol state.  Debug Cluster Step
 * calls it after the Owner has completed the current state cycle; tests and
 * diagnostics may call it directly to identify one or more violated Safety
 * categories.  A zero mask means that every locally observable invariant
 * holds. Cross-node Single Authority is checked by the bounded network API.
 */

#include "ucn/ucn_cluster.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ucn_cluster_invariant_violation {
    UCN_CLUSTER_INVARIANT_SAFETY_1_SINGLE_AUTHORITY = UINT32_C(1) << 0,
    UCN_CLUSTER_INVARIANT_SAFETY_2_AUTHORITY_QUORUM = UINT32_C(1) << 1,
    UCN_CLUSTER_INVARIANT_SAFETY_3_TAKEOVER_MAJORITY = UINT32_C(1) << 2,
    UCN_CLUSTER_INVARIANT_SAFETY_4_RECOVERY_ISOLATION = UINT32_C(1) << 3,
    UCN_CLUSTER_INVARIANT_SAFETY_5_PERSISTENT_VOTE = UINT32_C(1) << 4,
    UCN_CLUSTER_INVARIANT_SAFETY_6_CONFIG_SAFETY = UINT32_C(1) << 5,
    UCN_CLUSTER_INVARIANT_SAFETY_7_REPLAY_ISOLATION = UINT32_C(1) << 6,
    UCN_CLUSTER_INVARIANT_SAFETY_8_FENCE_ORDERING = UINT32_C(1) << 7,
    UCN_CLUSTER_INVARIANT_SAFETY_9_NO_SERIAL_REUSE = UINT32_C(1) << 8,
    UCN_CLUSTER_INVARIANT_SAFETY_10_PERSIST_BEFORE_PROMISE = UINT32_C(1) << 9
} ucn_cluster_invariant_violation_t;

#define UCN_CLUSTER_INVARIANT_ALL_SAFETY \
    ((uint32_t)((UINT32_C(1) << 10) - UINT32_C(1)))

/* On invalid arguments the output is unchanged.  now_ms must be the most
 * recent Owner-cycle time, not an independently sampled wall clock. */
ucn_result_t ucn_cluster_invariant_check(
    const ucn_cluster_t *cluster,
    uint32_t now_ms,
    uint32_t *violation_mask);

/* Bounded cross-node check. It combines every local result and verifies that
 * no two nodes claim writable Authority for the same stable cluster_id.
 * The caller owns the pointer array; no allocation is performed. */
ucn_result_t ucn_cluster_invariant_check_network(
    const ucn_cluster_t *const *clusters,
    size_t cluster_count,
    uint32_t now_ms,
    uint32_t *violation_mask);

#ifdef __cplusplus
}
#endif

#endif
