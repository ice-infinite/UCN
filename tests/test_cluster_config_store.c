#include <stdio.h>
#include <string.h>

#include "ucn/ucn_cluster_config_persistence.h"
#include "ucn/ucn_cluster_config_store.h"

#define ASSERT_TRUE(condition) \
    do { \
        if (!(condition)) { \
            printf("ASSERT failed: %s (%s:%d)\\n", #condition, __FILE__, \
                   __LINE__); \
            return 1; \
        } \
    } while (0)

static bool make_states(ucn_cluster_config_state_t *old_stable,
                        ucn_cluster_config_state_t *new_stable)
{
    static const ucn_node_id_t old_voters[] = { 1U, 4U, 9U };
    static const ucn_node_id_t new_voters[] = { 1U, 4U, 9U, 21U };
    ucn_cluster_config_state_t joint;

    return ucn_cluster_config_state_init_stable(
               old_stable, 1U, old_voters,
               sizeof(old_voters) / sizeof(old_voters[0U])) &&
           ucn_cluster_config_state_init_joint(
               &joint, old_stable, new_voters,
               sizeof(new_voters) / sizeof(new_voters[0U])) &&
           ucn_cluster_config_state_promote_joint(new_stable, &joint);
}

static bool make_durable_state(ucn_cluster_persist_state_t *state,
                               const ucn_cluster_persist_config_ref_t *base_ref)
{
    ucn_cluster_persist_state_init_empty(state);
    state->boot_incarnation = 1U;
    state->committed_config = *base_ref;
    return ucn_cluster_persist_state_is_valid(state);
}

static bool mark_prepared(ucn_cluster_persist_state_t *state,
                          const ucn_cluster_persist_config_ref_t *staging_ref)
{
    state->config_transaction.phase = UCN_CLUSTER_PERSIST_TRANSACTION_PREPARED;
    state->config_transaction.transaction_id = 1U;
    state->config_transaction.staging_config = *staging_ref;
    return ucn_cluster_persist_state_is_valid(state);
}

static bool mark_committed(ucn_cluster_persist_state_t *state,
                           const ucn_cluster_persist_config_ref_t *new_ref)
{
    state->committed_config = *new_ref;
    state->config_transaction.phase = UCN_CLUSTER_PERSIST_TRANSACTION_COMMITTED;
    state->config_transaction.transaction_id = 1U;
    state->config_transaction.staging_config = *new_ref;
    return ucn_cluster_persist_state_is_valid(state);
}

static int test_prepare_restart_has_one_active_and_one_staged_body(void)
{
    ucn_cluster_config_state_t old_stable;
    ucn_cluster_config_state_t new_stable;
    ucn_cluster_persist_config_ref_t old_ref;
    ucn_cluster_persist_config_ref_t new_ref;
    ucn_cluster_persist_state_t durable;
    ucn_cluster_config_store_t store;
    ucn_cluster_config_store_recovery_t recovery;

    ASSERT_TRUE(make_states(&old_stable, &new_stable) &&
                ucn_cluster_config_persist_ref_from_state(&old_stable,
                                                          &old_ref) == UCN_OK &&
                ucn_cluster_config_persist_ref_from_state(&new_stable,
                                                          &new_ref) == UCN_OK &&
                make_durable_state(&durable, &old_ref));
    ucn_cluster_config_store_init_empty(&store);
    ASSERT_TRUE(ucn_cluster_config_store_write_stable(&store, &old_stable,
                                                       &old_ref) == UCN_OK &&
                ucn_cluster_config_store_write_stable(&store, &new_stable,
                                                       &new_ref) == UCN_OK &&
                mark_prepared(&durable, &new_ref) &&
                ucn_cluster_config_store_recover(&store, &durable, &recovery) ==
                    UCN_OK && recovery.has_staged_config &&
                memcmp(&recovery.active_config, &old_stable,
                       sizeof(old_stable)) == 0 &&
                memcmp(&recovery.staged_config, &new_stable,
                       sizeof(new_stable)) == 0 &&
                recovery.active_generation != 0U &&
                recovery.staged_generation > recovery.active_generation);
    return 0;
}

static int test_commit_restart_selects_only_new_active_body(void)
{
    ucn_cluster_config_state_t old_stable;
    ucn_cluster_config_state_t new_stable;
    ucn_cluster_persist_config_ref_t old_ref;
    ucn_cluster_persist_config_ref_t new_ref;
    ucn_cluster_persist_state_t durable;
    ucn_cluster_config_store_t store;
    ucn_cluster_config_store_recovery_t recovery;

    ASSERT_TRUE(make_states(&old_stable, &new_stable) &&
                ucn_cluster_config_persist_ref_from_state(&old_stable,
                                                          &old_ref) == UCN_OK &&
                ucn_cluster_config_persist_ref_from_state(&new_stable,
                                                          &new_ref) == UCN_OK &&
                make_durable_state(&durable, &old_ref));
    ucn_cluster_config_store_init_empty(&store);
    ASSERT_TRUE(ucn_cluster_config_store_write_stable(&store, &old_stable,
                                                       &old_ref) == UCN_OK &&
                ucn_cluster_config_store_write_stable(&store, &new_stable,
                                                       &new_ref) == UCN_OK &&
                mark_committed(&durable, &new_ref) &&
                ucn_cluster_config_store_recover(&store, &durable, &recovery) ==
                    UCN_OK && !recovery.has_staged_config &&
                memcmp(&recovery.active_config, &new_stable,
                       sizeof(new_stable)) == 0);
    return 0;
}

static int test_torn_staged_slot_never_changes_active_config(void)
{
    ucn_cluster_config_state_t old_stable;
    ucn_cluster_config_state_t new_stable;
    ucn_cluster_persist_config_ref_t old_ref;
    ucn_cluster_persist_config_ref_t new_ref;
    ucn_cluster_persist_state_t durable;
    ucn_cluster_config_store_t store;
    ucn_cluster_config_store_recovery_t recovery;
    ucn_cluster_config_store_recovery_t before;

    ASSERT_TRUE(make_states(&old_stable, &new_stable) &&
                ucn_cluster_config_persist_ref_from_state(&old_stable,
                                                          &old_ref) == UCN_OK &&
                ucn_cluster_config_persist_ref_from_state(&new_stable,
                                                          &new_ref) == UCN_OK &&
                make_durable_state(&durable, &old_ref));
    ucn_cluster_config_store_init_empty(&store);
    ASSERT_TRUE(ucn_cluster_config_store_write_stable(&store, &old_stable,
                                                       &old_ref) == UCN_OK &&
                ucn_cluster_config_store_write_stable(&store, &new_stable,
                                                       &new_ref) == UCN_OK &&
                mark_prepared(&durable, &new_ref));
    (void)memset(&before, 0xA5, sizeof(before));
    recovery = before;
    store.slots[1U][UCN_CLUSTER_CONFIG_STORE_RECORD_HEADER_BYTES] ^= 1U;
    ASSERT_TRUE(ucn_cluster_config_store_recover(&store, &durable, &recovery) ==
                    UCN_ERR_STATE &&
                memcmp(&recovery, &before, sizeof(recovery)) == 0);
    durable.config_transaction.phase = UCN_CLUSTER_PERSIST_TRANSACTION_NONE;
    durable.config_transaction.transaction_id = 0U;
    (void)memset(&durable.config_transaction.staging_config, 0,
                 sizeof(durable.config_transaction.staging_config));
    ASSERT_TRUE(ucn_cluster_persist_state_is_valid(&durable) &&
                ucn_cluster_config_store_recover(&store, &durable, &recovery) ==
                    UCN_OK && !recovery.has_staged_config &&
                memcmp(&recovery.active_config, &old_stable,
                       sizeof(old_stable)) == 0);
    return 0;
}

int main(void)
{
    int result = 0;

    result |= test_prepare_restart_has_one_active_and_one_staged_body();
    result |= test_commit_restart_selects_only_new_active_body();
    result |= test_torn_staged_slot_never_changes_active_config();
    if (result == 0) {
        printf("Cluster config store tests passed.\n");
    }
    return result;
}
