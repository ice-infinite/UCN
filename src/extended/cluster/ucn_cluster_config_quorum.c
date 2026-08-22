#include "ucn/ucn_cluster_config_quorum.h"

static uint64_t bitmap_mask_for_count(uint8_t count)
{
    return count >= 64U ? UINT64_MAX : ((UINT64_C(1) << count) - UINT64_C(1));
}

static uint8_t bitmap_popcount(uint64_t value)
{
    uint8_t count = 0U;

    while (value != 0U) {
        value &= value - UINT64_C(1);
        ++count;
    }
    return count;
}

bool ucn_cluster_config_bitmap_reaches_quorum(
    const ucn_cluster_voter_set_t *set,
    uint64_t bitmap)
{
    uint8_t quorum;

    if (!ucn_cluster_voter_set_is_valid(set) ||
        (bitmap & ~bitmap_mask_for_count(set->count)) != 0U) {
        return false;
    }
    quorum = ucn_cluster_voter_set_quorum(set);
    return quorum != 0U && bitmap_popcount(bitmap) >= quorum;
}

bool ucn_cluster_config_joint_quorum_reached(
    const ucn_cluster_config_tx_t *tx)
{
    return ucn_cluster_config_tx_is_active(tx) &&
           ucn_cluster_config_bitmap_reaches_quorum(
               &tx->proposed_config.old_set, tx->old_ack_bitmap) &&
           ucn_cluster_config_bitmap_reaches_quorum(
               &tx->proposed_config.new_set, tx->new_ack_bitmap);
}
