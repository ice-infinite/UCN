#ifndef UCN_CLUSTER_EPOCH_H
#define UCN_CLUSTER_EPOCH_H

/* UCN CLV2-M03 (03-01): Cluster Epoch value type and pure comparator.
 *
 * PURE INFRASTRUCTURE (M03.1 audit scope): this module adds the Epoch
 * relation mathematics ONLY - it does NOT change any production
 * decision path yet.  Current business code keeps working; the Golden
 * trace must stay byte-identical.  03-02+ will wire active_epoch into
 * Head Offer / Merge / Higher-Authority decisions.
 *
 * RELATION SEMANTICS (human auditor, frozen):
 *   same cluster:            terms ARE comparable
 *   same cluster + same term: head equal      -> SAME
 *                            head different  -> CONFLICT
 *   foreign cluster:         terms are NEVER comparable -> FOREIGN
 *
 * THE ORDER MATTERS: foreign MUST truncate the comparison domain
 * FIRST.  A comparator that compares terms first and only then checks
 * cluster_id is WRONG (Cluster A term 2 must not defer to Cluster B
 * term 100 - that is 03-03's semantic fix, but the comparator must
 * already forbid the cross-cluster term comparison).
 */

#include "ucn/ucn_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A point-in-time cluster generation: which cluster, at which term,
 * led by which Head.  Value type - copied by value, never heap. */
typedef struct ucn_cluster_epoch {
    uint32_t cluster_id;
    uint32_t term;
    ucn_node_id_t head_node_id;
} ucn_cluster_epoch_t;

/* The pairwise relation between two Epochs.  FOREIGN is returned for
 * any pair whose cluster_id differs - terms are not compared across
 * clusters (M03 milestone gate). */
typedef enum ucn_cluster_epoch_relation {
    UCN_CLUSTER_EPOCH_RELATION_UNKNOWN = 0,
    UCN_CLUSTER_EPOCH_RELATION_SAME = 1,
    UCN_CLUSTER_EPOCH_RELATION_LOWER = 2,
    UCN_CLUSTER_EPOCH_RELATION_HIGHER = 3,
    UCN_CLUSTER_EPOCH_RELATION_CONFLICT = 4,
    UCN_CLUSTER_EPOCH_RELATION_FOREIGN = 5
} ucn_cluster_epoch_relation_t;

/* Compare two Epochs.  Pure function - no state, no I/O.
 *   - different cluster_id                     -> FOREIGN
 *   - same cluster, a.term < b.term            -> LOWER
 *   - same cluster, a.term > b.term            -> HIGHER
 *   - same cluster, same term, same head       -> SAME
 *   - same cluster, same term, different head  -> CONFLICT
 * Returns UNKNOWN only for a NULL argument. */
ucn_cluster_epoch_relation_t ucn_cluster_epoch_compare(
    const ucn_cluster_epoch_t *a,
    const ucn_cluster_epoch_t *b);

/* Convenience predicates over the relation (all NULL-safe). */
bool ucn_cluster_epoch_is_same_cluster(
    const ucn_cluster_epoch_t *a, const ucn_cluster_epoch_t *b);
bool ucn_cluster_epoch_is_foreign(
    const ucn_cluster_epoch_t *a, const ucn_cluster_epoch_t *b);

#ifdef __cplusplus
}
#endif

#endif /* UCN_CLUSTER_EPOCH_H */
