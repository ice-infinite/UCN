#include <stdio.h>
#include <string.h>

#include "ucn/ucn_cluster_config_state.h"

#define ASSERT_TRUE(condition) \
    do { \
        if (!(condition)) { \
            printf("ASSERT failed: %s (%s:%d)\\n", #condition, __FILE__, \
                   __LINE__); \
            return 1; \
        } \
    } while (0)

static int test_stable_canonicalization(void)
{
    static const ucn_node_id_t unordered[] = { 17U, 1U, 9U, 4U };
    static const ucn_node_id_t reordered[] = { 4U, 9U, 17U, 1U };
    ucn_cluster_config_state_t first;
    ucn_cluster_config_state_t second;
    uint8_t first_bytes[UCN_CLUSTER_CONFIG_STATE_SERIALIZED_BYTES];
    uint8_t second_bytes[UCN_CLUSTER_CONFIG_STATE_SERIALIZED_BYTES];

    (void)memset(&first, 0xA5, sizeof(first));
    (void)memset(&second, 0xA5, sizeof(second));
    ASSERT_TRUE(ucn_cluster_config_state_init_stable(
        &first, 7U, unordered, sizeof(unordered) / sizeof(unordered[0U])));
    ASSERT_TRUE(ucn_cluster_config_state_init_stable(
        &second, 7U, reordered, sizeof(reordered) / sizeof(reordered[0U])));
    ASSERT_TRUE(ucn_cluster_config_state_is_valid(&first));
    ASSERT_TRUE(first.phase == (uint8_t)UCN_CLUSTER_CONFIG_PHASE_STABLE &&
                first.config_id == 7U && first.old_set.count == 4U &&
                first.old_set.node_ids[0U] == 1U &&
                first.old_set.node_ids[1U] == 4U &&
                first.old_set.node_ids[2U] == 9U &&
                first.old_set.node_ids[3U] == 17U &&
                memcmp(&first.old_set, &first.new_set,
                       sizeof(first.old_set)) == 0);
    ASSERT_TRUE(ucn_cluster_config_state_hash(&first) ==
                ucn_cluster_config_state_hash(&second));
    ASSERT_TRUE(ucn_cluster_config_state_serialize(
                    &first, first_bytes, sizeof(first_bytes)) == UCN_OK &&
                ucn_cluster_config_state_serialize(
                    &second, second_bytes, sizeof(second_bytes)) == UCN_OK &&
                memcmp(first_bytes, second_bytes, sizeof(first_bytes)) == 0);
    return 0;
}

static int test_joint_and_promotion(void)
{
    static const ucn_node_id_t old_nodes[] = { 1U, 4U, 9U };
    static const ucn_node_id_t new_nodes[] = { 1U, 4U, 9U, 21U };
    ucn_cluster_config_state_t stable;
    ucn_cluster_config_state_t joint;
    ucn_cluster_config_state_t promoted;

    ASSERT_TRUE(ucn_cluster_config_state_init_stable(
        &stable, 41U, old_nodes, sizeof(old_nodes) / sizeof(old_nodes[0U])));
    ASSERT_TRUE(ucn_cluster_config_state_init_joint(
        &joint, &stable, new_nodes, sizeof(new_nodes) / sizeof(new_nodes[0U])));
    ASSERT_TRUE(ucn_cluster_config_state_is_valid(&joint));
    ASSERT_TRUE(joint.phase == (uint8_t)UCN_CLUSTER_CONFIG_PHASE_JOINT &&
                joint.config_id == 42U && joint.old_set.config_id == 41U &&
                joint.new_set.config_id == 42U && joint.old_set.count == 3U &&
                joint.new_set.count == 4U &&
                ucn_cluster_voter_set_contains(&joint.old_set, 9U) &&
                !ucn_cluster_voter_set_contains(&joint.old_set, 21U) &&
                ucn_cluster_voter_set_contains(&joint.new_set, 21U));
    ASSERT_TRUE(ucn_cluster_config_state_promote_joint(&promoted, &joint));
    ASSERT_TRUE(ucn_cluster_config_state_is_valid(&promoted) &&
                promoted.phase == (uint8_t)UCN_CLUSTER_CONFIG_PHASE_STABLE &&
                promoted.config_id == 42U && promoted.old_set.count == 4U &&
                memcmp(&promoted.old_set, &promoted.new_set,
                       sizeof(promoted.old_set)) == 0);
    return 0;
}

static int test_failure_no_write(void)
{
    static const ucn_node_id_t nodes[] = { 1U, 4U, 9U };
    ucn_cluster_config_state_t stable;
    ucn_cluster_config_state_t before;
    uint8_t output[UCN_CLUSTER_CONFIG_STATE_SERIALIZED_BYTES];
    uint8_t output_before[UCN_CLUSTER_CONFIG_STATE_SERIALIZED_BYTES];

    (void)memset(&stable, 0x5A, sizeof(stable));
    before = stable;
    ASSERT_TRUE(!ucn_cluster_config_state_init_stable(&stable, 0U, nodes,
                                                       3U));
    ASSERT_TRUE(memcmp(&stable, &before, sizeof(stable)) == 0);
    ASSERT_TRUE(ucn_cluster_config_state_init_stable(&stable,
        UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD, nodes, 3U));
    ASSERT_TRUE(ucn_cluster_config_state_rekey_required(&stable));
    before = stable;
    ASSERT_TRUE(!ucn_cluster_config_state_init_joint(&stable, &before, nodes,
                                                      3U));
    ASSERT_TRUE(memcmp(&stable, &before, sizeof(stable)) == 0);
    stable.old_set_hash ^= 1U;
    (void)memset(output, 0xA5, sizeof(output));
    (void)memcpy(output_before, output, sizeof(output));
    ASSERT_TRUE(!ucn_cluster_config_state_is_valid(&stable));
    ASSERT_TRUE(ucn_cluster_config_state_hash(&stable) == 0U);
    ASSERT_TRUE(ucn_cluster_config_state_serialize(
                    &stable, output, sizeof(output)) == UCN_ERR_CONFIG &&
                memcmp(output, output_before, sizeof(output)) == 0);
    return 0;
}

static int test_config_id_stops_before_wrap(void)
{
    static const ucn_node_id_t nodes[] = { 1U, 4U, 9U };
    ucn_cluster_config_state_t stable;
    ucn_cluster_config_state_t joint;
    ucn_cluster_config_state_t promoted;

    ASSERT_TRUE(ucn_cluster_config_state_init_stable(
                    &stable, UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD - 1U,
                    nodes, sizeof(nodes) / sizeof(nodes[0U])) &&
                !ucn_cluster_config_state_rekey_required(&stable) &&
                ucn_cluster_config_state_init_joint(
                    &joint, &stable, nodes,
                    sizeof(nodes) / sizeof(nodes[0U])) &&
                joint.config_id == UCN_CLUSTER_SERIAL_ROTATION_THRESHOLD &&
                ucn_cluster_config_state_promote_joint(&promoted, &joint) &&
                ucn_cluster_config_state_rekey_required(&promoted));
    return 0;
}

static int test_canonical_deserialize_rejects_torn_or_dirty_body(void)
{
    static const ucn_node_id_t nodes[] = { 1U, 4U, 9U };
    ucn_cluster_config_state_t stable;
    ucn_cluster_config_state_t decoded;
    ucn_cluster_config_state_t before;
    uint8_t bytes[UCN_CLUSTER_CONFIG_STATE_SERIALIZED_BYTES];

    ASSERT_TRUE(ucn_cluster_config_state_init_stable(
                    &stable, 7U, nodes,
                    sizeof(nodes) / sizeof(nodes[0U])) &&
                ucn_cluster_config_state_serialize(&stable, bytes,
                                                   sizeof(bytes)) == UCN_OK &&
                ucn_cluster_config_state_deserialize(bytes, sizeof(bytes),
                                                     &decoded) == UCN_OK &&
                memcmp(&stable, &decoded, sizeof(stable)) == 0);
    before = decoded;
    bytes[7U] = 1U;
    ASSERT_TRUE(ucn_cluster_config_state_deserialize(bytes, sizeof(bytes),
                                                     &decoded) == UCN_ERR_ARGUMENT &&
                memcmp(&decoded, &before, sizeof(decoded)) == 0);
    return 0;
}

int main(void)
{
    int result = 0;

    result |= test_stable_canonicalization();
    result |= test_joint_and_promotion();
    result |= test_failure_no_write();
    result |= test_config_id_stops_before_wrap();
    result |= test_canonical_deserialize_rejects_torn_or_dirty_body();
    if (result == 0) {
        printf("Cluster config state tests passed.\n");
    }
    return result;
}
