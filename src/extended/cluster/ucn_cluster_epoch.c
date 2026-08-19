/* UCN CLV2-M03 (03-01): Cluster Epoch comparator implementation.
 *
 * PURE INFRASTRUCTURE - see ucn_cluster_epoch.h for the frozen relation
 * semantics and the ordering rule (foreign truncates the comparison
 * domain before any term comparison).
 */

#include "ucn/ucn_cluster_epoch.h"

ucn_cluster_epoch_relation_t ucn_cluster_epoch_compare(
    const ucn_cluster_epoch_t *a,
    const ucn_cluster_epoch_t *b)
{
    if (a == NULL || b == NULL) {
        return UCN_CLUSTER_EPOCH_RELATION_UNKNOWN;
    }
    /* CLV2-M03.1: FOREIGN is decided FIRST - different cluster_id
     * truncates the comparison domain, terms are NEVER compared across
     * clusters (M03 milestone gate: different cluster_id terms are
     * never directly compared).  A same-cluster pair proceeds to the
     * term comparison. */
    if (a->cluster_id != b->cluster_id) {
        return UCN_CLUSTER_EPOCH_RELATION_FOREIGN;
    }
    if (a->term < b->term) {
        return UCN_CLUSTER_EPOCH_RELATION_LOWER;
    }
    if (a->term > b->term) {
        return UCN_CLUSTER_EPOCH_RELATION_HIGHER;
    }
    /* Same cluster, same term: the Head decides SAME vs CONFLICT. */
    if (a->head_node_id == b->head_node_id) {
        return UCN_CLUSTER_EPOCH_RELATION_SAME;
    }
    return UCN_CLUSTER_EPOCH_RELATION_CONFLICT;
}

bool ucn_cluster_epoch_is_same_cluster(
    const ucn_cluster_epoch_t *a, const ucn_cluster_epoch_t *b)
{
    return a != NULL && b != NULL && a->cluster_id == b->cluster_id;
}

bool ucn_cluster_epoch_is_foreign(
    const ucn_cluster_epoch_t *a, const ucn_cluster_epoch_t *b)
{
    return ucn_cluster_epoch_compare(a, b) ==
           UCN_CLUSTER_EPOCH_RELATION_FOREIGN;
}
