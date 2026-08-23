#include <stdio.h>
#include <string.h>

#include "ucn/ucn_cluster_authority.h"

#define ASSERT_TRUE(condition) \
    do { \
        if (!(condition)) { \
            printf("ASSERT failed: %s (%s:%d)\\n", #condition, __FILE__, \
                   __LINE__); \
            return 1; \
        } \
    } while (0)

typedef struct authority_fixture {
    uint32_t now_ms;
    uint32_t send_calls;
    ucn_cluster_t cluster;
    ucn_cluster_authority_runtime_t runtime;
    ucn_cluster_config_state_t stable;
    ucn_cluster_authority_timing_t timing;
} authority_fixture_t;

static uint32_t fixture_now_ms(void *context)
{
    const authority_fixture_t *fixture = context;

    return fixture == NULL ? 0U : fixture->now_ms;
}

static ucn_result_t fixture_send(void *context, ucn_node_id_t destination,
                                 ucn_endpoint_t endpoint,
                                 const uint8_t *payload,
                                 uint16_t payload_length)
{
    authority_fixture_t *fixture = context;

    (void)destination;
    (void)endpoint;
    (void)payload;
    (void)payload_length;
    if (fixture != NULL) {
        fixture->send_calls++;
    }
    return UCN_OK;
}

static int fixture_init(authority_fixture_t *fixture)
{
    static const ucn_node_id_t voters[] = { 1U, 2U, 3U };
    const ucn_cluster_timing_budget_t budget = {
        5U, 5U, 5U, 5U, 5U, 5U
    };
    ucn_cluster_config_t config;

    (void)memset(fixture, 0, sizeof(*fixture));
    (void)memset(&config, 0, sizeof(config));
    config.local_node_id = 1U;
    config.enabled = false;
    config.head_capable = true;
    config.member_capacity = 4U;
    config.now_ms = fixture_now_ms;
    config.now_context = fixture;
    config.send = fixture_send;
    config.send_context = fixture;
    config.persistence_mode = UCN_CLUSTER_PERSISTENCE_VOLATILE_TEST;
    ASSERT_TRUE(ucn_cluster_init(&fixture->cluster, &config) == UCN_OK);
    fixture->cluster.config.enabled = true;
    fixture->cluster.role = UCN_CLUSTER_ROLE_HEAD;
    fixture->cluster.cluster_id = 77U;
    fixture->cluster.term = 5U;
    fixture->cluster.head_node_id = 1U;
    ASSERT_TRUE(ucn_cluster_config_state_init_stable(
        &fixture->stable, 10U, voters, sizeof(voters) / sizeof(voters[0U])));
    ASSERT_TRUE(ucn_cluster_authority_timing_derive(&budget,
                                                     &fixture->timing) == UCN_OK);
    ASSERT_TRUE(fixture->timing.voter_lease_ms == 90U &&
                fixture->timing.owner_step_budget_ms == 5U &&
                fixture->timing.control_window_ms == 30U &&
                fixture->timing.authority_grace_ms == 60U &&
                fixture->timing.quorum_restore_hold_ms == 30U &&
                fixture->timing.member_takeover_grace_ms == 60U &&
                fixture->timing.fenced_dissolve_ms == 90U);
    ASSERT_TRUE(ucn_cluster_authority_runtime_init(
        &fixture->runtime, &fixture->cluster, &fixture->stable,
        &fixture->timing, fixture->now_ms) == UCN_OK);
    return 0;
}

static bool authority_tx_matrix_is_denied(
    const ucn_cluster_authority_runtime_t *runtime)
{
    static const struct {
        ucn_cluster_message_type_t type;
        ucn_cluster_role_t role;
    } authority_messages[] = {
        { UCN_CLUSTER_MSG_ADVERTISE, UCN_CLUSTER_ROLE_HEAD },
        { UCN_CLUSTER_MSG_HEAD_DECLARE, UCN_CLUSTER_ROLE_HEAD },
        { UCN_CLUSTER_MSG_JOIN_ACCEPT, UCN_CLUSTER_ROLE_HEAD },
        { UCN_CLUSTER_MSG_JOIN_REJECT, UCN_CLUSTER_ROLE_HEAD },
        { UCN_CLUSTER_MSG_HEAD_TAKEOVER, UCN_CLUSTER_ROLE_HEAD },
        { UCN_CLUSTER_MSG_HEAD_STEPDOWN, UCN_CLUSTER_ROLE_HEAD },
        { UCN_CLUSTER_MSG_BACKUP_ASSIGN, UCN_CLUSTER_ROLE_HEAD },
        { UCN_CLUSTER_MSG_BACKUP_MEMBER_SYNC, UCN_CLUSTER_ROLE_HEAD },
        { UCN_CLUSTER_MSG_PRIMARY_HEARTBEAT, UCN_CLUSTER_ROLE_HEAD },
        { UCN_CLUSTER_MSG_BACKUP_READY, UCN_CLUSTER_ROLE_BACKUP },
        { UCN_CLUSTER_MSG_TAKEOVER_PREPARE, UCN_CLUSTER_ROLE_BACKUP },
        { UCN_CLUSTER_MSG_BACKUP_RESYNC_REQ, UCN_CLUSTER_ROLE_BACKUP }
    };
    size_t index;

    for (index = 0U; index < sizeof(authority_messages) /
                               sizeof(authority_messages[0U]); ++index) {
        if (ucn_cluster_authority_runtime_tx_allowed(
                runtime, authority_messages[index].type,
                authority_messages[index].role)) {
            return false;
        }
    }
    return true;
}

static int test_authority_stable_quorum_and_same_step_fence(void)
{
    static const struct {
        ucn_cluster_message_type_t type;
        ucn_cluster_role_t role;
    } authority_messages[] = {
        { UCN_CLUSTER_MSG_ADVERTISE, UCN_CLUSTER_ROLE_HEAD },
        { UCN_CLUSTER_MSG_HEAD_DECLARE, UCN_CLUSTER_ROLE_HEAD },
        { UCN_CLUSTER_MSG_JOIN_ACCEPT, UCN_CLUSTER_ROLE_HEAD },
        { UCN_CLUSTER_MSG_JOIN_REJECT, UCN_CLUSTER_ROLE_HEAD },
        { UCN_CLUSTER_MSG_HEAD_TAKEOVER, UCN_CLUSTER_ROLE_HEAD },
        { UCN_CLUSTER_MSG_HEAD_STEPDOWN, UCN_CLUSTER_ROLE_HEAD },
        { UCN_CLUSTER_MSG_BACKUP_ASSIGN, UCN_CLUSTER_ROLE_HEAD },
        { UCN_CLUSTER_MSG_BACKUP_MEMBER_SYNC, UCN_CLUSTER_ROLE_HEAD },
        { UCN_CLUSTER_MSG_PRIMARY_HEARTBEAT, UCN_CLUSTER_ROLE_HEAD },
        { UCN_CLUSTER_MSG_BACKUP_READY, UCN_CLUSTER_ROLE_BACKUP },
        { UCN_CLUSTER_MSG_TAKEOVER_PREPARE, UCN_CLUSTER_ROLE_BACKUP },
        { UCN_CLUSTER_MSG_BACKUP_RESYNC_REQ, UCN_CLUSTER_ROLE_BACKUP }
    };
    authority_fixture_t fixture;
    ucn_cluster_view_t view;
    size_t index;

    if (fixture_init(&fixture) != 0) {
        return 1;
    }
    ASSERT_TRUE(!ucn_cluster_authority_active(&fixture.cluster));
    ASSERT_TRUE(ucn_cluster_authority_runtime_note_voter_keepalive(
        &fixture.runtime, 2U, 0U) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_authority_runtime_step(&fixture.runtime, 0U) ==
                UCN_OK);
    ASSERT_TRUE(ucn_cluster_authority_active(&fixture.cluster) &&
                fixture.cluster.authority_phase ==
                    UCN_CLUSTER_PHASE_HEAD_NO_BACKUP &&
                ucn_cluster_authority_runtime_tx_allowed(
                    &fixture.runtime, UCN_CLUSTER_MSG_ADVERTISE,
                    UCN_CLUSTER_ROLE_HEAD));
    /* Neighbor state is not a voter lease evidence source. */
    fixture.cluster.peers[0].occupied = true;
    fixture.cluster.peers[0].node_id = 2U;
    fixture.cluster.peers[0].neighbor_state = UCN_NEIGHBOR_SUSPECT;
    ASSERT_TRUE(ucn_cluster_authority_runtime_quorum_met(&fixture.runtime, 1U));
    for (fixture.now_ms = 1U; fixture.now_ms <= 91U; ++fixture.now_ms) {
        ASSERT_TRUE(ucn_cluster_authority_runtime_step(&fixture.runtime,
                                                        fixture.now_ms) == UCN_OK);
    }
    ASSERT_TRUE(!ucn_cluster_authority_active(&fixture.cluster) &&
                fixture.cluster.authority_phase ==
                    UCN_CLUSTER_PHASE_HEAD_QUORUM_GRACE &&
                fixture.cluster.authority_fence_reason ==
                    UCN_CLUSTER_AUTHORITY_FENCE_QUORUM_LOST &&
                !ucn_cluster_authority_runtime_tx_allowed(
                    &fixture.runtime, UCN_CLUSTER_MSG_ADVERTISE,
                    UCN_CLUSTER_ROLE_HEAD) &&
                ucn_cluster_authority_runtime_tx_allowed(
                    &fixture.runtime, UCN_CLUSTER_MSG_KEEPALIVE,
                    UCN_CLUSTER_ROLE_MEMBER));
    for (index = 0U; index < sizeof(authority_messages) /
                               sizeof(authority_messages[0U]); ++index) {
        ASSERT_TRUE(!ucn_cluster_authority_runtime_tx_allowed(
            &fixture.runtime, authority_messages[index].type,
            authority_messages[index].role));
    }
    ASSERT_TRUE(ucn_cluster_get_view(&fixture.cluster, &view) == UCN_OK &&
                !view.authority_active &&
                view.authority_phase == UCN_CLUSTER_PHASE_HEAD_QUORUM_GRACE);
    ASSERT_TRUE(ucn_cluster_authority_runtime_note_voter_keepalive(
        &fixture.runtime, 2U, 92U) == UCN_OK);
    for (fixture.now_ms = 92U; fixture.now_ms <= 122U; ++fixture.now_ms) {
        ASSERT_TRUE(ucn_cluster_authority_runtime_step(&fixture.runtime,
                                                        fixture.now_ms) == UCN_OK);
    }
    ASSERT_TRUE(ucn_cluster_authority_active(&fixture.cluster) &&
                fixture.cluster.authority_phase ==
                    UCN_CLUSTER_PHASE_HEAD_NO_BACKUP);
    return 0;
}

static int test_joint_quorum_and_permanent_fence(void)
{
    static const ucn_node_id_t joint_voters[] = { 1U, 3U, 4U };
    authority_fixture_t fixture;
    ucn_cluster_config_state_t joint;

    if (fixture_init(&fixture) != 0) {
        return 1;
    }
    ASSERT_TRUE(ucn_cluster_authority_runtime_note_voter_keepalive(
        &fixture.runtime, 2U, 0U) == UCN_OK &&
                ucn_cluster_authority_runtime_step(&fixture.runtime, 0U) ==
                    UCN_OK && ucn_cluster_authority_active(&fixture.cluster));
    ASSERT_TRUE(ucn_cluster_config_state_init_joint(
        &joint, &fixture.stable, joint_voters,
        sizeof(joint_voters) / sizeof(joint_voters[0U])));
    ASSERT_TRUE(ucn_cluster_authority_runtime_install_config(&fixture.runtime,
                                                              &joint, 1U) == UCN_OK &&
                !ucn_cluster_authority_active(&fixture.cluster) &&
                fixture.cluster.authority_phase ==
                    UCN_CLUSTER_PHASE_HEAD_QUORUM_GRACE &&
                !ucn_cluster_authority_runtime_tx_allowed(
                    &fixture.runtime, UCN_CLUSTER_MSG_ADVERTISE,
                    UCN_CLUSTER_ROLE_HEAD) &&
                authority_tx_matrix_is_denied(&fixture.runtime));
    ASSERT_TRUE(!ucn_cluster_authority_runtime_quorum_met(&fixture.runtime, 1U));
    ASSERT_TRUE(ucn_cluster_authority_runtime_note_voter_keepalive(
        &fixture.runtime, 3U, 1U) == UCN_OK &&
                ucn_cluster_authority_runtime_quorum_met(&fixture.runtime, 1U));
    ASSERT_TRUE(ucn_cluster_authority_runtime_note_term_conflict(
        &fixture.runtime, 77U, 5U, 9U, 2U) == UCN_OK &&
                !ucn_cluster_authority_active(&fixture.cluster) &&
                fixture.cluster.authority_phase == UCN_CLUSTER_PHASE_HEAD_FENCED &&
                fixture.cluster.authority_fence_reason ==
                    UCN_CLUSTER_AUTHORITY_FENCE_TERM_CONFLICT);
    ASSERT_TRUE(ucn_cluster_authority_runtime_note_voter_keepalive(
        &fixture.runtime, 2U, 3U) == UCN_OK &&
                ucn_cluster_authority_runtime_step(&fixture.runtime, 3U) ==
                    UCN_OK && !ucn_cluster_authority_active(&fixture.cluster));
    ASSERT_TRUE(ucn_cluster_authority_runtime_step(&fixture.runtime, 92U) ==
                UCN_OK && fixture.cluster.authority_phase ==
                    UCN_CLUSTER_PHASE_RECOVERY_OBSERVE);
    ASSERT_TRUE(!ucn_cluster_authority_runtime_tx_allowed(
        &fixture.runtime, UCN_CLUSTER_MSG_PRIMARY_HEARTBEAT,
        UCN_CLUSTER_ROLE_HEAD));
    return 0;
}

static int test_config_install_immediately_revokes_for_unquorate_stable(void)
{
    static const ucn_node_id_t replacement_voters[] = { 1U, 3U, 4U };
    authority_fixture_t fixture;
    ucn_cluster_config_state_t replacement;

    if (fixture_init(&fixture) != 0) {
        return 1;
    }
    ASSERT_TRUE(ucn_cluster_authority_runtime_note_voter_keepalive(
                    &fixture.runtime, 2U, 0U) == UCN_OK &&
                ucn_cluster_authority_runtime_step(&fixture.runtime, 0U) ==
                    UCN_OK && ucn_cluster_authority_active(&fixture.cluster));
    ASSERT_TRUE(ucn_cluster_config_state_init_stable(
                    &replacement, 11U, replacement_voters,
                    sizeof(replacement_voters) /
                        sizeof(replacement_voters[0U])) &&
                ucn_cluster_authority_runtime_install_config(
                    &fixture.runtime, &replacement, 1U) == UCN_OK &&
                !ucn_cluster_authority_active(&fixture.cluster) &&
                fixture.cluster.authority_phase ==
                    UCN_CLUSTER_PHASE_HEAD_QUORUM_GRACE &&
                !ucn_cluster_authority_runtime_quorum_met(&fixture.runtime,
                                                           1U) &&
                authority_tx_matrix_is_denied(&fixture.runtime));
    return 0;
}

static int test_preflight_blocks_stale_tx_and_head_rx(void)
{
    authority_fixture_t fixture;
    ucn_cluster_message_t message;
    uint8_t payload[UCN_CLUSTER_MESSAGE_BYTES];
    ucn_cluster_member_table_t members_before;
    ucn_cluster_token_bucket_t bucket_before;

    if (fixture_init(&fixture) != 0) {
        return 1;
    }
    fixture.cluster.peers[0].occupied = true;
    fixture.cluster.peers[0].node_id = 4U;
    fixture.cluster.peers[0].neighbor_state = UCN_NEIGHBOR_ADMITTED;
    ASSERT_TRUE(ucn_cluster_authority_runtime_note_voter_keepalive(
                    &fixture.runtime, 2U, 0U) == UCN_OK &&
                ucn_cluster_authority_runtime_step(&fixture.runtime, 0U) ==
                    UCN_OK && ucn_cluster_authority_active(&fixture.cluster));
    bucket_before = fixture.cluster.token_bucket;
    fixture.now_ms = 91U;
    ASSERT_TRUE(ucn_cluster_step(&fixture.cluster) == UCN_OK &&
                !ucn_cluster_authority_active(&fixture.cluster) &&
                fixture.cluster.authority_fence_reason ==
                    UCN_CLUSTER_AUTHORITY_FENCE_OWNER_STEP_BUDGET &&
                fixture.send_calls == 0U &&
                memcmp(&fixture.cluster.token_bucket, &bucket_before,
                       sizeof(bucket_before)) == 0);

    if (fixture_init(&fixture) != 0) {
        return 1;
    }
    fixture.cluster.peers[0].occupied = true;
    fixture.cluster.peers[0].node_id = 4U;
    fixture.cluster.peers[0].neighbor_state = UCN_NEIGHBOR_ADMITTED;
    ASSERT_TRUE(ucn_cluster_authority_runtime_note_voter_keepalive(
                    &fixture.runtime, 2U, 0U) == UCN_OK &&
                ucn_cluster_authority_runtime_step(&fixture.runtime, 0U) ==
                    UCN_OK && ucn_cluster_authority_active(&fixture.cluster));
    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_JOIN_REQUEST;
    message.role = UCN_CLUSTER_ROLE_MEMBER;
    message.cluster_id = fixture.cluster.cluster_id;
    message.term = fixture.cluster.term;
    message.head_node_id = fixture.cluster.config.local_node_id;
    message.lease_ms = 1U;
    message.nonce = 1U;
    ASSERT_TRUE(ucn_cluster_message_encode(&message, payload) == UCN_OK);
    members_before = fixture.cluster.primary_members;
    bucket_before = fixture.cluster.token_bucket;
    fixture.now_ms = 91U;
    ASSERT_TRUE(ucn_cluster_receive(&fixture.cluster, 4U, false, payload,
                                    sizeof(payload)) == UCN_ERR_ACCESS &&
                !ucn_cluster_authority_active(&fixture.cluster) &&
                memcmp(&fixture.cluster.primary_members, &members_before,
                       sizeof(members_before)) == 0 &&
                memcmp(&fixture.cluster.token_bucket, &bucket_before,
                       sizeof(bucket_before)) == 0 &&
                fixture.send_calls == 0U);
    return 0;
}

static int test_higher_authority_fences_before_next_owner_step(void)
{
    authority_fixture_t fixture;

    if (fixture_init(&fixture) != 0) {
        return 1;
    }
    ASSERT_TRUE(ucn_cluster_authority_runtime_note_voter_keepalive(
        &fixture.runtime, 2U, 0U) == UCN_OK &&
                ucn_cluster_authority_runtime_step(&fixture.runtime, 0U) ==
                    UCN_OK && ucn_cluster_authority_active(&fixture.cluster));
    ASSERT_TRUE(ucn_cluster_authority_runtime_note_higher_authority(
        &fixture.runtime, 77U, 6U, 2U, 1U) == UCN_OK &&
                !ucn_cluster_authority_active(&fixture.cluster) &&
                fixture.cluster.authority_phase == UCN_CLUSTER_PHASE_HEAD_FENCED &&
                !ucn_cluster_authority_runtime_tx_allowed(
                    &fixture.runtime, UCN_CLUSTER_MSG_ADVERTISE,
                    UCN_CLUSTER_ROLE_HEAD));
    ASSERT_TRUE(ucn_cluster_authority_runtime_step(&fixture.runtime, 1U) ==
                UCN_OK && fixture.cluster.authority_phase ==
                    UCN_CLUSTER_PHASE_JOIN_PENDING &&
                !ucn_cluster_authority_active(&fixture.cluster));
    return 0;
}

static int test_observed_higher_term_never_selects_join_pending(void)
{
    authority_fixture_t fixture;

    if (fixture_init(&fixture) != 0) {
        return 1;
    }
    ASSERT_TRUE(ucn_cluster_authority_runtime_note_voter_keepalive(
        &fixture.runtime, 2U, 0U) == UCN_OK &&
                ucn_cluster_authority_runtime_step(&fixture.runtime, 0U) ==
                    UCN_OK && ucn_cluster_authority_active(&fixture.cluster));
    ASSERT_TRUE(ucn_cluster_authority_runtime_note_higher_term_observed(
        &fixture.runtime, 77U, 6U, 2U, 1U) == UCN_OK &&
                fixture.cluster.authority_phase == UCN_CLUSTER_PHASE_HEAD_FENCED &&
                !ucn_cluster_authority_active(&fixture.cluster));
    ASSERT_TRUE(ucn_cluster_authority_runtime_step(&fixture.runtime, 91U) ==
                    UCN_OK &&
                fixture.cluster.authority_phase ==
                    UCN_CLUSTER_PHASE_RECOVERY_OBSERVE);
    return 0;
}

static int test_timing_invalid_no_write(void)
{
    const ucn_cluster_timing_budget_t invalid = { 0U, 5U, 5U, 5U, 5U, 5U };
    ucn_cluster_authority_timing_t output;
    ucn_cluster_authority_timing_t before;

    (void)memset(&output, 0xA5, sizeof(output));
    before = output;
    ASSERT_TRUE(ucn_cluster_authority_timing_derive(&invalid, &output) ==
                    UCN_ERR_ARGUMENT &&
                memcmp(&output, &before, sizeof(output)) == 0);
    return 0;
}

static int test_member_takeover_grace_and_owner_budget(void)
{
    authority_fixture_t fixture;
    uint32_t grace = UINT32_C(0xA5A5A5A5);
    uint32_t before = grace;

    if (fixture_init(&fixture) != 0) {
        return 1;
    }
    ASSERT_TRUE(ucn_cluster_authority_member_takeover_grace_derive(
                    &fixture.timing, 120U, 90U, 40U, &grace) == UCN_OK &&
                grace == 100U);
    ASSERT_TRUE(ucn_cluster_authority_member_takeover_grace_derive(
                    &fixture.timing, 0U, 90U, 40U, &grace) == UCN_ERR_ARGUMENT &&
                grace == 100U);
    grace = before;
    ASSERT_TRUE(ucn_cluster_authority_runtime_note_voter_keepalive(
                    &fixture.runtime, 2U, 0U) == UCN_OK &&
                ucn_cluster_authority_runtime_step(&fixture.runtime, 0U) ==
                    UCN_OK && ucn_cluster_authority_active(&fixture.cluster));
    ASSERT_TRUE(ucn_cluster_authority_runtime_step(&fixture.runtime, 6U) ==
                    UCN_OK && !ucn_cluster_authority_active(&fixture.cluster) &&
                fixture.cluster.authority_phase == UCN_CLUSTER_PHASE_HEAD_FENCED &&
                fixture.cluster.authority_fence_reason ==
                    UCN_CLUSTER_AUTHORITY_FENCE_OWNER_STEP_BUDGET);
    return 0;
}

static int test_fence_causes_and_invalid_profile_no_write(void)
{
    static const ucn_node_id_t voters[] = { 1U, 2U, 3U };
    authority_fixture_t fixture;
    ucn_cluster_t unbound_cluster;
    ucn_cluster_config_t config;
    ucn_cluster_authority_runtime_t unbound_runtime;
    ucn_cluster_authority_runtime_t unbound_before;

    if (fixture_init(&fixture) != 0) {
        return 1;
    }
    ASSERT_TRUE(ucn_cluster_authority_runtime_step(&fixture.runtime, 0U) ==
                    UCN_OK &&
                fixture.cluster.authority_phase ==
                    UCN_CLUSTER_PHASE_HEAD_QUORUM_GRACE);
    for (fixture.now_ms = 1U;
         fixture.now_ms <= fixture.timing.authority_grace_ms;
         ++fixture.now_ms) {
        ASSERT_TRUE(ucn_cluster_authority_runtime_step(&fixture.runtime,
                                                        fixture.now_ms) == UCN_OK);
    }
    ASSERT_TRUE(fixture.cluster.authority_phase == UCN_CLUSTER_PHASE_HEAD_FENCED &&
                fixture.cluster.authority_fence_reason ==
                    UCN_CLUSTER_AUTHORITY_FENCE_GRACE_EXPIRED &&
                !ucn_cluster_authority_active(&fixture.cluster));

    if (fixture_init(&fixture) != 0) {
        return 1;
    }
    ASSERT_TRUE(ucn_cluster_authority_runtime_note_voter_keepalive(
                    &fixture.runtime, 2U, 0U) == UCN_OK &&
                ucn_cluster_authority_runtime_step(&fixture.runtime, 0U) ==
                    UCN_OK && ucn_cluster_authority_active(&fixture.cluster));
    fixture.cluster.persistence_faulted = true;
    ASSERT_TRUE(ucn_cluster_authority_runtime_step(&fixture.runtime, 1U) ==
                    UCN_OK && fixture.cluster.authority_phase ==
                        UCN_CLUSTER_PHASE_HEAD_FENCED &&
                fixture.cluster.authority_fence_reason ==
                    UCN_CLUSTER_AUTHORITY_FENCE_PERSISTENCE_FAULT &&
                !ucn_cluster_authority_active(&fixture.cluster));

    (void)memset(&unbound_cluster, 0, sizeof(unbound_cluster));
    (void)memset(&config, 0, sizeof(config));
    config.local_node_id = 1U;
    config.enabled = false;
    config.head_capable = true;
    config.member_capacity = 4U;
    config.now_ms = fixture_now_ms;
    config.now_context = &fixture;
    config.send = fixture_send;
    config.send_context = &fixture;
    config.persistence_mode = UCN_CLUSTER_PERSISTENCE_VOLATILE_TEST;
    ASSERT_TRUE(ucn_cluster_init(&unbound_cluster, &config) == UCN_OK);
    unbound_cluster.config.enabled = true;
    unbound_cluster.config.lease_ms = fixture.timing.voter_lease_ms - 1U;
    unbound_cluster.role = UCN_CLUSTER_ROLE_HEAD;
    unbound_cluster.cluster_id = 77U;
    unbound_cluster.term = 5U;
    unbound_cluster.head_node_id = 1U;
    ASSERT_TRUE(ucn_cluster_config_state_init_stable(
                    &fixture.stable, 10U, voters,
                    sizeof(voters) / sizeof(voters[0U])));
    (void)memset(&unbound_runtime, 0xA5, sizeof(unbound_runtime));
    unbound_before = unbound_runtime;
    ASSERT_TRUE(ucn_cluster_authority_runtime_init(
                    &unbound_runtime, &unbound_cluster, &fixture.stable,
                    &fixture.timing, 0U) == UCN_ERR_CONFIG &&
                memcmp(&unbound_runtime, &unbound_before,
                       sizeof(unbound_runtime)) == 0 &&
                unbound_cluster.authority_runtime == NULL &&
                !unbound_cluster.authority_active);
    return 0;
}

static int test_partition_quorum_property(void)
{
    const ucn_cluster_timing_budget_t budget = {
        5U, 5U, 5U, 5U, 5U, 5U
    };
    uint8_t voter_count;

    /* Exhaust every Head-containing partition for 3..6 voters.  This is a
     * bounded property test: the Head sees exactly the remote voter leases
     * represented by mask, while all other neighbor state is irrelevant. */
    for (voter_count = 3U; voter_count <= 6U; ++voter_count) {
        uint32_t mask_limit = UINT32_C(1) << (voter_count - 1U);
        uint32_t mask;

        for (mask = 0U; mask < mask_limit; ++mask) {
            ucn_node_id_t voters[6] = { 1U, 2U, 3U, 4U, 5U, 6U };
            ucn_cluster_t cluster;
            ucn_cluster_config_t config;
            ucn_cluster_authority_runtime_t runtime;
            ucn_cluster_config_state_t state;
            ucn_cluster_authority_timing_t timing;
            uint8_t index;
            uint8_t live_count = 1U;
            bool expected;

            (void)memset(&cluster, 0, sizeof(cluster));
            (void)memset(&config, 0, sizeof(config));
            config.local_node_id = 1U;
            config.enabled = false;
            config.head_capable = true;
            config.member_capacity = voter_count;
            config.persistence_mode = UCN_CLUSTER_PERSISTENCE_VOLATILE_TEST;
            ASSERT_TRUE(ucn_cluster_init(&cluster, &config) == UCN_OK);
            cluster.config.enabled = true;
            cluster.role = UCN_CLUSTER_ROLE_HEAD;
            cluster.cluster_id = 500U + voter_count;
            cluster.term = 1U;
            cluster.head_node_id = 1U;
            ASSERT_TRUE(ucn_cluster_config_state_init_stable(
                            &state, 1U, voters, voter_count) &&
                        ucn_cluster_authority_timing_derive(&budget, &timing) ==
                            UCN_OK &&
                        ucn_cluster_authority_runtime_init(
                            &runtime, &cluster, &state, &timing, 0U) == UCN_OK);
            for (index = 1U; index < voter_count; ++index) {
                if ((mask & (UINT32_C(1) << (index - 1U))) != 0U) {
                    ++live_count;
                    ASSERT_TRUE(ucn_cluster_authority_runtime_note_voter_keepalive(
                                    &runtime, voters[index], 0U) == UCN_OK);
                }
            }
            expected = live_count >= (uint8_t)(voter_count / 2U + 1U);
            ASSERT_TRUE(ucn_cluster_authority_runtime_quorum_met(&runtime, 0U) ==
                            expected &&
                        ucn_cluster_authority_runtime_step(&runtime, 0U) ==
                            UCN_OK &&
                        ucn_cluster_authority_active(&cluster) == expected);
            if (!expected) {
                ASSERT_TRUE(cluster.authority_phase ==
                                UCN_CLUSTER_PHASE_HEAD_QUORUM_GRACE &&
                            !ucn_cluster_authority_runtime_tx_allowed(
                                &runtime, UCN_CLUSTER_MSG_ADVERTISE,
                                UCN_CLUSTER_ROLE_HEAD));
            }
        }
    }
    return 0;
}

static int test_recovery_scoped_cluster_never_gets_authority(void)
{
    authority_fixture_t fixture;
    ucn_result_t result;

    ASSERT_TRUE(fixture_init(&fixture) == 0);
    /* Detach the fixture's original bind so the recovery staging looks
     * like an unattached recovery control domain. */
    fixture.cluster.authority_runtime = NULL;
    fixture.cluster.authority_active = false;

    /* A recovery control domain: role RECOVERY_HEAD, active identity is
     * the recovery domain ID. */
    fixture.cluster.role = UCN_CLUSTER_ROLE_RECOVERY_HEAD;
    fixture.cluster.cluster_id = 77U;
    fixture.cluster.recovery_cluster_id = 77U;
    fixture.cluster.parent_cluster_id = 5U;
    ASSERT_TRUE(ucn_cluster_recovery_scoped(&fixture.cluster));
    result = ucn_cluster_authority_runtime_init(
        &fixture.runtime, &fixture.cluster, &fixture.stable,
        &fixture.timing, fixture.now_ms);
    ASSERT_TRUE(result == UCN_ERR_STATE);
    /* The scope exclusion runs FIRST: the Owner never re-binds and no
     * authority-active permission appears. */
    ASSERT_TRUE(fixture.cluster.authority_runtime == NULL);
    ASSERT_TRUE(fixture.cluster.authority_active == false);

    /* A recovery-domain MEMBER is equally excluded. */
    fixture.cluster.role = UCN_CLUSTER_ROLE_MEMBER;
    fixture.cluster.cluster_id = 77U;
    fixture.cluster.recovery_cluster_id = 77U;
    ASSERT_TRUE(ucn_cluster_recovery_scoped(&fixture.cluster));
    result = ucn_cluster_authority_runtime_init(
        &fixture.runtime, &fixture.cluster, &fixture.stable,
        &fixture.timing, fixture.now_ms);
    ASSERT_TRUE(result == UCN_ERR_STATE);
    ASSERT_TRUE(fixture.cluster.authority_runtime == NULL);
    ASSERT_TRUE(fixture.cluster.authority_active == false);

    /* The same cluster identity is NOT recovery-scoped once the domain
     * dissolves (recovery_cluster_id cleared) - the normal stable bind
     * path stays available for a future stable identity. */
    fixture.cluster.role = UCN_CLUSTER_ROLE_HEAD;
    fixture.cluster.recovery_cluster_id = 0U;
    ASSERT_TRUE(!ucn_cluster_recovery_scoped(&fixture.cluster));
    return 0;
}

int main(void)
{
    int result = 0;

    result |= test_authority_stable_quorum_and_same_step_fence();
    result |= test_joint_quorum_and_permanent_fence();
    result |= test_config_install_immediately_revokes_for_unquorate_stable();
    result |= test_preflight_blocks_stale_tx_and_head_rx();
    result |= test_higher_authority_fences_before_next_owner_step();
    result |= test_observed_higher_term_never_selects_join_pending();
    result |= test_timing_invalid_no_write();
    result |= test_member_takeover_grace_and_owner_budget();
    result |= test_fence_causes_and_invalid_profile_no_write();
    result |= test_partition_quorum_property();
    result |= test_recovery_scoped_cluster_never_gets_authority();
    if (result == 0) {
        printf("Cluster Authority tests passed.\\n");
    }
    return result;
}
