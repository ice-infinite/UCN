#include <stdio.h>
#include <string.h>

#include "ucn/ucn_cluster_authority.h"
#include "ucn/ucn_cluster_invariant.h"
#include "ucn/ucn_cluster_storage.h"
#include "ucn/ucn_cluster_takeover.h"

#include "ucn_cluster_takeover_internal.h"

#define PROPERTY_ASSERT(condition) \
    do { \
        if (!(condition)) { \
            (void)fprintf(stderr, "PROPERTY failed: %s (%s:%d)\n", \
                          #condition, __FILE__, __LINE__); \
            return 1; \
        } \
    } while (0)

typedef struct property_authority_node {
    uint32_t now_ms;
    ucn_cluster_t cluster;
    ucn_cluster_authority_runtime_t runtime;
    ucn_cluster_config_state_t config;
    ucn_cluster_authority_timing_t timing;
} property_authority_node_t;

static uint32_t property_now(void *context)
{
    const property_authority_node_t *node = context;

    return node == NULL ? 0U : node->now_ms;
}

static ucn_result_t property_send(void *context, ucn_node_id_t destination,
                                  ucn_endpoint_t endpoint,
                                  const uint8_t *payload,
                                  uint16_t payload_length)
{
    (void)context;
    (void)destination;
    (void)endpoint;
    (void)payload;
    (void)payload_length;
    return UCN_OK;
}

static bool authority_node_init(property_authority_node_t *node,
                                ucn_node_id_t local_node_id)
{
    static const ucn_node_id_t voters[] = {1U, 2U, 3U, 4U, 5U};
    const ucn_cluster_timing_budget_t budget = {5U, 5U, 5U, 5U, 5U, 5U};
    ucn_cluster_config_t config;

    (void)memset(node, 0, sizeof(*node));
    (void)memset(&config, 0, sizeof(config));
    config.local_node_id = local_node_id;
    config.enabled = false;
    config.head_capable = true;
    config.member_capacity = 8U;
    config.now_ms = property_now;
    config.now_context = node;
    config.send = property_send;
    config.send_context = node;
    config.persistence_mode = UCN_CLUSTER_PERSISTENCE_VOLATILE_TEST;
    if (ucn_cluster_init(&node->cluster, &config) != UCN_OK) {
        return false;
    }
    node->cluster.config.enabled = true;
    node->cluster.phase = UCN_CLUSTER_PHASE_HEAD_NO_BACKUP;
    node->cluster.cluster_id = 700U;
    node->cluster.term = 9U;
    node->cluster.head_node_id = local_node_id;
    return ucn_cluster_config_state_init_stable(
               &node->config, 12U, voters,
               sizeof(voters) / sizeof(voters[0U])) &&
           ucn_cluster_authority_timing_derive(&budget, &node->timing) == UCN_OK &&
           ucn_cluster_authority_runtime_init(
               &node->runtime, &node->cluster, &node->config, &node->timing,
               node->now_ms) == UCN_OK;
}

/* Exhaust all 3^3 exclusive assignments of neutral voters 3..5.  Each Head
 * counts its local self-vote; a majority therefore needs two of those three
 * neutral voters.  Exclusivity and quorum intersection make dual Authority
 * impossible, which is checked after every Owner tick rather than only at
 * the scenario endpoint. */
static int property_single_authority_all_partitions(void)
{
    uint32_t assignment;

    for (assignment = 0U; assignment < 27U; ++assignment) {
        property_authority_node_t first;
        property_authority_node_t second;
        const ucn_cluster_t *network[2];
        uint32_t choices = assignment;
        uint8_t owners[3];
        uint32_t now_ms;
        size_t voter;

        PROPERTY_ASSERT(authority_node_init(&first, 1U));
        PROPERTY_ASSERT(authority_node_init(&second, 2U));
        for (voter = 0U; voter < 3U; ++voter) {
            owners[voter] = (uint8_t)(choices % 3U);
            choices /= 3U;
        }
        network[0] = &first.cluster;
        network[1] = &second.cluster;
        for (now_ms = 0U; now_ms <= 180U; ++now_ms) {
            uint32_t violations = UINT32_MAX;

            first.now_ms = now_ms;
            second.now_ms = now_ms;
            if ((now_ms % 15U) == 0U) {
                for (voter = 0U; voter < 3U; ++voter) {
                    ucn_node_id_t voter_id = (ucn_node_id_t)(voter + 3U);

                    if (owners[voter] == 1U) {
                        PROPERTY_ASSERT(
                            ucn_cluster_authority_runtime_note_voter_keepalive(
                                &first.runtime, voter_id, now_ms) == UCN_OK);
                    } else if (owners[voter] == 2U) {
                        PROPERTY_ASSERT(
                            ucn_cluster_authority_runtime_note_voter_keepalive(
                                &second.runtime, voter_id, now_ms) == UCN_OK);
                    }
                }
            }
            PROPERTY_ASSERT(ucn_cluster_authority_runtime_step(
                                &first.runtime, now_ms) == UCN_OK);
            PROPERTY_ASSERT(ucn_cluster_authority_runtime_step(
                                &second.runtime, now_ms) == UCN_OK);
            PROPERTY_ASSERT(ucn_cluster_invariant_check_network(
                                network, 2U, now_ms, &violations) == UCN_OK);
            PROPERTY_ASSERT(violations == 0U);
            PROPERTY_ASSERT(!(ucn_cluster_authority_active(&first.cluster) &&
                              ucn_cluster_authority_active(&second.cluster)));
        }
    }
    return 0;
}

static bool make_takeover_owner(ucn_cluster_backup_sync_owner_t *owner,
                                bool joint)
{
    static const ucn_node_id_t stable_voters[] = {1U, 2U, 3U};
    static const ucn_node_id_t joint_voters[] = {2U, 3U, 4U};
    ucn_cluster_backup_epoch_t epoch;
    ucn_cluster_config_state_t stable;
    ucn_cluster_config_state_t active;
    ucn_cluster_snapshot_epoch_t snapshot;

    (void)memset(owner, 0, sizeof(*owner));
    (void)memset(&epoch, 0, sizeof(epoch));
    epoch.cluster_id = 21U;
    epoch.term = 9U;
    epoch.head_node_id = 1U;
    epoch.backup_node_id = 2U;
    epoch.backup_generation = 4U;
    if (!ucn_cluster_config_state_init_stable(
            &stable, 6U, stable_voters,
            sizeof(stable_voters) / sizeof(stable_voters[0U]))) {
        return false;
    }
    if (joint) {
        if (!ucn_cluster_config_state_init_joint(
                &active, &stable, joint_voters,
                sizeof(joint_voters) / sizeof(joint_voters[0U]))) {
            return false;
        }
    } else {
        active = stable;
    }
    if (ucn_cluster_backup_sync_owner_init(owner, &epoch, &active) != UCN_OK ||
        !ucn_cluster_snapshot_epoch_from_config(&snapshot, &epoch, 7U,
                                                &active)) {
        return false;
    }
    owner->mirror.committed_epoch = snapshot;
    owner->mirror.committed_valid = true;
    return ucn_cluster_backup_sync_owner_is_valid(owner) &&
           ucn_cluster_backup_sync_owner_takeover_eligible(owner);
}

static ucn_result_t note_vote(ucn_cluster_takeover_transaction_t *transaction,
                              ucn_node_id_t voter_node_id)
{
    ucn_cluster_takeover_remote_vote_proof_t proof;

    (void)memset(&proof, 0, sizeof(proof));
    proof.member.voter_node_id = voter_node_id;
    proof.member.member_takeover_grace = true;
    proof.member.old_head_lease_expired = true;
    proof.member.committed_v4_voter = true;
    proof.exact_vote_durable = true;
    return ucn_cluster_takeover_transaction_note_durable_vote(
        transaction, &transaction->vote_id, &proof);
}

static bool reference_majority(uint8_t votes, uint8_t voter_count)
{
    uint8_t count = 0U;

    while (votes != 0U) {
        votes &= (uint8_t)(votes - 1U);
        ++count;
    }
    return count >= (uint8_t)(voter_count / 2U + 1U);
}

/* Enumerate every remote-vote subset for Stable and Joint takeover.  The
 * independent bitmap oracle is deliberately simpler than the implementation
 * and proves that EPOCH_DURABLE can be reached only after all required frozen
 * set majorities. */
static int property_takeover_majority_and_durable_terminal(void)
{
    uint8_t joint;

    for (joint = 0U; joint <= 1U; ++joint) {
        uint8_t subset;

        for (subset = 0U; subset < 8U; ++subset) {
            ucn_cluster_backup_sync_owner_t owner;
            ucn_cluster_takeover_transaction_t transaction;
            bool old_quorum;
            bool new_quorum;

            PROPERTY_ASSERT(make_takeover_owner(&owner, joint != 0U));
            (void)memset(&transaction, 0, sizeof(transaction));
            PROPERTY_ASSERT(ucn_cluster_takeover_transaction_begin(
                                &transaction, &owner, 31U, 10U, 100U) == UCN_OK);
            PROPERTY_ASSERT(
                ucn_cluster_takeover_transaction_mark_self_vote_durable_internal(
                    &transaction, &transaction.vote_id) == UCN_OK);
            if ((subset & 1U) != 0U) {
                PROPERTY_ASSERT(note_vote(&transaction, 1U) == UCN_OK);
            }
            if ((subset & 2U) != 0U) {
                PROPERTY_ASSERT(note_vote(&transaction, 3U) == UCN_OK);
            }
            if ((subset & 4U) != 0U && joint != 0U) {
                PROPERTY_ASSERT(note_vote(&transaction, 4U) == UCN_OK);
            }

            /* Backup 2 is the durable self vote. Stable old={1,2,3}; Joint
             * new={2,3,4}. */
            old_quorum = reference_majority(
                (uint8_t)(2U | ((subset & 1U) != 0U ? 1U : 0U) |
                          ((subset & 2U) != 0U ? 4U : 0U)),
                3U);
            new_quorum = joint == 0U || reference_majority(
                (uint8_t)(1U | ((subset & 2U) != 0U ? 2U : 0U) |
                          ((subset & 4U) != 0U ? 4U : 0U)),
                3U);
            PROPERTY_ASSERT(ucn_cluster_takeover_transaction_quorum_reached(
                                &transaction) == (old_quorum && new_quorum));
            if (old_quorum && new_quorum) {
                ucn_cluster_takeover_certificate_t certificate;
                ucn_cluster_takeover_old_primary_fence_t fence;
                ucn_cluster_epoch_t old_epoch;

                PROPERTY_ASSERT(
                    ucn_cluster_takeover_transaction_mark_epoch_durable_internal(
                        &transaction, &transaction.vote_id) == UCN_OK);
                PROPERTY_ASSERT(
                    ucn_cluster_takeover_transaction_head_result_ready(&transaction));
                PROPERTY_ASSERT(ucn_cluster_takeover_certificate_build(
                                    &transaction, &certificate) == UCN_OK);
                (void)memset(&fence, 0, sizeof(fence));
                old_epoch.cluster_id =
                    transaction.frozen_snapshot_epoch.backup_epoch.cluster_id;
                old_epoch.term =
                    transaction.frozen_snapshot_epoch.backup_epoch.term;
                old_epoch.head_node_id =
                    transaction.frozen_snapshot_epoch.backup_epoch.head_node_id;
                PROPERTY_ASSERT(ucn_cluster_takeover_old_primary_fence_accept(
                                    &fence, &old_epoch, &transaction,
                                    &certificate) == UCN_OK);
                PROPERTY_ASSERT(fence.fenced && fence.join_required);
                PROPERTY_ASSERT(ucn_cluster_takeover_transaction_step(
                                    &transaction, 1000U) == UCN_OK);
                PROPERTY_ASSERT(
                    ucn_cluster_takeover_transaction_head_result_ready(&transaction));
            } else {
                PROPERTY_ASSERT(
                    ucn_cluster_takeover_transaction_mark_epoch_durable_internal(
                        &transaction, &transaction.vote_id) == UCN_ERR_STATE);
                PROPERTY_ASSERT(
                    !ucn_cluster_takeover_transaction_head_result_ready(&transaction));
            }
        }
    }
    return 0;
}

static uint32_t property_random_next(uint32_t *state)
{
    uint32_t value = *state;

    value ^= value << 13U;
    value ^= value >> 17U;
    value ^= value << 5U;
    *state = value;
    return value;
}

/* Stateful deterministic property run over the real M10 transaction.  The
 * fixed seed is printed on failure by the assertion line and retained here
 * so every ordering is exactly reproducible. */
static int property_takeover_random_sequences(void)
{
    uint32_t random_state = UINT32_C(0x14A05EED);
    uint32_t sequence;

    for (sequence = 0U; sequence < 4096U; ++sequence) {
        ucn_cluster_backup_sync_owner_t owner;
        ucn_cluster_takeover_transaction_t transaction;
        uint32_t event_index;
        bool joint = (property_random_next(&random_state) & 1U) != 0U;

        PROPERTY_ASSERT(make_takeover_owner(&owner, joint));
        (void)memset(&transaction, 0, sizeof(transaction));
        PROPERTY_ASSERT(ucn_cluster_takeover_transaction_begin(
                            &transaction, &owner, sequence + 1U, 10U, 100U) ==
                        UCN_OK);
        for (event_index = 0U; event_index < 32U; ++event_index) {
            ucn_cluster_takeover_transaction_t before = transaction;
            uint32_t event = property_random_next(&random_state) % 7U;
            ucn_result_t result;
            bool time_step = false;

            if (event == 0U) {
                result =
                    ucn_cluster_takeover_transaction_mark_self_vote_durable_internal(
                        &transaction, &transaction.vote_id);
            } else if (event == 1U) {
                result = note_vote(&transaction, 1U);
            } else if (event == 2U) {
                result = note_vote(&transaction, 3U);
            } else if (event == 3U && joint) {
                result = note_vote(&transaction, 4U);
            } else if (event == 4U) {
                ucn_cluster_takeover_vote_id_t wrong = transaction.vote_id;
                ucn_cluster_takeover_remote_vote_proof_t proof;

                wrong.snapshot_id++;
                (void)memset(&proof, 0, sizeof(proof));
                proof.member.voter_node_id = 3U;
                proof.member.member_takeover_grace = true;
                proof.member.old_head_lease_expired = true;
                proof.member.committed_v4_voter = true;
                proof.exact_vote_durable = true;
                result = ucn_cluster_takeover_transaction_note_durable_vote(
                    &transaction, &wrong, &proof);
                PROPERTY_ASSERT(result == UCN_ERR_REPLAY);
                PROPERTY_ASSERT(memcmp(&transaction, &before,
                                       sizeof(transaction)) == 0);
            } else if (event == 5U) {
                bool quorum =
                    ucn_cluster_takeover_transaction_quorum_reached(&transaction);

                result =
                    ucn_cluster_takeover_transaction_mark_epoch_durable_internal(
                        &transaction, &transaction.vote_id);
                if (quorum) {
                    PROPERTY_ASSERT(result == UCN_OK);
                }
            } else {
                uint32_t now_ms = 10U +
                                  (property_random_next(&random_state) % 140U);

                time_step = true;
                result = ucn_cluster_takeover_transaction_step(&transaction,
                                                                now_ms);
            }
            if (event != 4U && result != UCN_OK) {
                if (time_step) {
                    PROPERTY_ASSERT(transaction.state ==
                                    (uint8_t)UCN_CLUSTER_TAKEOVER_STATE_ABORTED);
                    PROPERTY_ASSERT(transaction.recovery_required &&
                                    !transaction.active);
                } else {
                    PROPERTY_ASSERT(memcmp(&transaction, &before,
                                           sizeof(transaction)) == 0);
                }
            }
            if (ucn_cluster_takeover_transaction_head_result_ready(
                    &transaction)) {
                ucn_cluster_takeover_transaction_t terminal = transaction;

                PROPERTY_ASSERT(transaction.proposed_epoch_durable);
                PROPERTY_ASSERT(transaction.state ==
                                (uint8_t)UCN_CLUSTER_TAKEOVER_STATE_EPOCH_DURABLE);
                PROPERTY_ASSERT(ucn_cluster_takeover_transaction_quorum_reached(
                                    &transaction));
                PROPERTY_ASSERT(ucn_cluster_takeover_transaction_step(
                                    &transaction, UINT32_C(100000)) == UCN_OK);
                PROPERTY_ASSERT(memcmp(&transaction, &terminal,
                                       sizeof(transaction)) == 0);
            }
        }
    }
    return 0;
}

int main(void)
{
    int result = 0;

    result |= property_single_authority_all_partitions();
    result |= property_takeover_majority_and_durable_terminal();
    result |= property_takeover_random_sequences();
    if (result == 0) {
        (void)printf("M14 target property model passed: 27 partitions, "
                     "16 takeover subsets, 131072 fixed-seed events\n");
    }
    return result;
}
