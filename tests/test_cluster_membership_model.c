#include <stdio.h>
#include <string.h>

#include "ucn/ucn_cluster_membership.h"

#include "ucn_cluster_internal.h"

#if defined(UCN_CLUSTER_ENABLE_TEST_HOOKS)
#error "membership model must exercise the production Cluster archive"
#endif

#define ASSERT_TRUE(condition) \
    do { \
        if (!(condition)) { \
            printf("ASSERT failed: %s (%s:%d)\\n", #condition, __FILE__, \
                   __LINE__); \
            return 1; \
        } \
    } while (0)

static uint32_t production_rx_now(void *context)
{
    return *(const uint32_t *)context;
}

static ucn_result_t production_rx_send(
    void *context,
    ucn_node_id_t destination,
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

static bool production_rx_encode_backup_control(
    ucn_cluster_message_t *message,
    ucn_cluster_message_type_t type,
    ucn_node_id_t source,
    uint8_t output[UCN_CLUSTER_MESSAGE_BYTES])
{
    (void)memset(message, 0, sizeof(*message));
    message->type = type;
    message->role = UCN_CLUSTER_ROLE_HEAD;
    message->cluster_id = 10U;
    message->term = 3U;
    message->head_node_id = 1U;
    message->head_score = 1U;
    message->available_capacity = 1U;
    message->lease_ms = 1000U;
    message->nonce = 1U;
    switch (type) {
    case UCN_CLUSTER_MSG_HEAD_TAKEOVER:
        message->head_node_id = source;
        message->term = 4U;
        message->backup_generation = 4U;
        break;
    case UCN_CLUSTER_MSG_BACKUP_ASSIGN:
        message->backup_generation = 4U;
        message->sync_token = 2U; /* self-assignment is tested first. */
        break;
    case UCN_CLUSTER_MSG_BACKUP_READY:
        message->role = UCN_CLUSTER_ROLE_BACKUP;
        message->backup_generation = 4U;
        break;
    case UCN_CLUSTER_MSG_BACKUP_MEMBER_SYNC:
        message->backup_generation = 4U;
        message->flags = UCN_CLUSTER_FLAG_SYNC_BEGIN;
        break;
    case UCN_CLUSTER_MSG_PRIMARY_HEARTBEAT:
        message->backup_generation = 4U;
        break;
    case UCN_CLUSTER_MSG_TAKEOVER_PREPARE:
        message->role = UCN_CLUSTER_ROLE_BACKUP;
        message->backup_generation = 4U;
        break;
    case UCN_CLUSTER_MSG_TAKEOVER_ACK:
        message->role = UCN_CLUSTER_ROLE_MEMBER;
        message->backup_generation = 4U;
        break;
    case UCN_CLUSTER_MSG_BACKUP_RESYNC_REQ:
    case UCN_CLUSTER_MSG_BACKUP_REJECT:
        message->role = UCN_CLUSTER_ROLE_BACKUP;
        message->backup_generation = 4U;
        break;
    default:
        return false;
    }
    return ucn_cluster_message_encode(message, output) == UCN_OK;
}

static int test_status_and_transition_contract(void)
{
    static const bool expected[4][4] = {
        { true, true, true, false },
        { true, true, true, true },
        { false, false, true, true },
        { true, false, true, true }
    };
    size_t previous;
    size_t next;

    ASSERT_TRUE(ucn_cluster_member_status_is_valid(
        UCN_CLUSTER_MEMBER_STATUS_NONE));
    ASSERT_TRUE(ucn_cluster_member_status_is_valid(
        UCN_CLUSTER_MEMBER_STATUS_PROVISIONAL));
    ASSERT_TRUE(ucn_cluster_member_status_is_valid(
        UCN_CLUSTER_MEMBER_STATUS_COMMITTED));
    ASSERT_TRUE(ucn_cluster_member_status_is_valid(
        UCN_CLUSTER_MEMBER_STATUS_REMOVING));
    ASSERT_TRUE(!ucn_cluster_member_status_is_valid(
        (ucn_cluster_member_status_t)4));

    for (previous = 0U; previous < 4U; ++previous) {
        for (next = 0U; next < 4U; ++next) {
            ASSERT_TRUE(ucn_cluster_member_transition_is_valid(
                (ucn_cluster_member_status_t)previous,
                (ucn_cluster_member_status_t)next) == expected[previous][next]);
        }
    }
    ASSERT_TRUE(!ucn_cluster_member_transition_is_valid(
        (ucn_cluster_member_status_t)4,
        UCN_CLUSTER_MEMBER_STATUS_COMMITTED));
    return 0;
}

static int test_record_contract(void)
{
    ucn_cluster_member_t member;

    (void)memset(&member, 0, sizeof(member));
    ASSERT_TRUE(ucn_cluster_member_record_is_valid(&member));

    member.occupied = true;
    member.node_id = 7U;
    member.status = (uint8_t)UCN_CLUSTER_MEMBER_STATUS_PROVISIONAL;
    member.wire_version = UCN_CLUSTER_MEMBER_WIRE_VERSION_V4;
    member.capabilities = 1U;
    member.provisional_deadline_armed = true;
    member.provisional_deadline_ms = 100U;
    ASSERT_TRUE(ucn_cluster_member_record_is_valid(&member));
    member.voting = true;
    ASSERT_TRUE(!ucn_cluster_member_record_is_valid(&member));
    member.voting = false;
    member.wire_version = UCN_CLUSTER_MEMBER_WIRE_VERSION_V3;
    ASSERT_TRUE(!ucn_cluster_member_record_is_valid(&member));
    member.capabilities = 0U;
    ASSERT_TRUE(ucn_cluster_member_record_is_valid(&member));

    member.status = (uint8_t)UCN_CLUSTER_MEMBER_STATUS_COMMITTED;
    member.voting = true;
    member.provisional_deadline_armed = false;
    member.provisional_deadline_ms = 0U;
    member.node_id = UCN_NODE_BROADCAST;
    ASSERT_TRUE(!ucn_cluster_member_record_is_valid(&member));
    member.node_id = 7U;
    member.wire_version = 5U;
    ASSERT_TRUE(!ucn_cluster_member_record_is_valid(&member));
    member.wire_version = UCN_CLUSTER_MEMBER_WIRE_VERSION_V3;
    member.capabilities = 1U;
    ASSERT_TRUE(!ucn_cluster_member_record_is_valid(&member));
    member.capabilities = 0U;
    ASSERT_TRUE(ucn_cluster_member_record_is_valid(&member));
    return 0;
}

static int test_legacy_v3_bridge(void)
{
    ucn_cluster_member_t member;
    ucn_cluster_member_t before_invalid;

    (void)memset(&member, 0xA5, sizeof(member));
    ASSERT_TRUE(member_initialize_legacy(&member, 9U, 123U, 50U));
    ASSERT_TRUE(member.occupied);
    ASSERT_TRUE(!member.voting);
    ASSERT_TRUE(member.status == UCN_CLUSTER_MEMBER_STATUS_PROVISIONAL);
    ASSERT_TRUE(member.wire_version == UCN_CLUSTER_MEMBER_WIRE_VERSION_V3);
    ASSERT_TRUE(member.capabilities == 0U);
    ASSERT_TRUE(member.node_id == 9U);
    ASSERT_TRUE(member.lease_expires_at_ms == 0U);
    ASSERT_TRUE(member.last_nonce == 0U);
    ASSERT_TRUE(member.joined_at_ms == 123U);
    ASSERT_TRUE(member.last_keepalive_at_ms == 123U);
    ASSERT_TRUE(member.provisional_deadline_armed);
    ASSERT_TRUE(member.provisional_deadline_ms == 173U);
    ASSERT_TRUE(ucn_cluster_member_record_is_valid(&member));
    member_note_legacy_keepalive(&member, 456U);
    ASSERT_TRUE(member.joined_at_ms == 123U);
    ASSERT_TRUE(member.last_keepalive_at_ms == 456U);
    ASSERT_TRUE(ucn_cluster_member_record_is_valid(&member));
    before_invalid = member;
    ASSERT_TRUE(!member_initialize_legacy(&member, 0U, 789U, 50U));
    ASSERT_TRUE(memcmp(&member, &before_invalid, sizeof(member)) == 0);
    ASSERT_TRUE(!member_initialize_legacy(&member, UCN_NODE_BROADCAST, 789U,
                                           50U));
    ASSERT_TRUE(memcmp(&member, &before_invalid, sizeof(member)) == 0);
    return 0;
}

static int test_primary_member_table_contract(void)
{
    ucn_cluster_member_table_t table;
    ucn_cluster_member_table_t invalid_table;

    (void)memset(&table, 0, sizeof(table));
    ASSERT_TRUE(ucn_cluster_member_table_is_valid(&table));
    ASSERT_TRUE(ucn_cluster_member_table_count(&table) == 0U);
    ASSERT_TRUE(member_initialize_legacy(&table.slots[0U], 17U, 321U, 50U));
    ASSERT_TRUE(ucn_cluster_member_table_is_valid(&table));
    ASSERT_TRUE(ucn_cluster_member_table_count(&table) == 1U);
    ASSERT_TRUE(table.slots[0U].node_id == 17U);
    ASSERT_TRUE(table.slots[0U].status ==
                UCN_CLUSTER_MEMBER_STATUS_PROVISIONAL);

    invalid_table = table;
    invalid_table.slots[0U].occupied = false;
    invalid_table.slots[0U].node_id = 99U;
    ASSERT_TRUE(!ucn_cluster_member_table_is_valid(&invalid_table));
    ASSERT_TRUE(ucn_cluster_member_table_count(&invalid_table) == 0U);
    ASSERT_TRUE(ucn_cluster_member_table_is_valid(&table));
    ASSERT_TRUE(ucn_cluster_member_table_count(&table) == 1U);
    ASSERT_TRUE(!ucn_cluster_member_table_is_valid(NULL));
    ASSERT_TRUE(ucn_cluster_member_table_count(NULL) == 0U);
    return 0;
}

static int test_voter_set_contract(void)
{
    static const ucn_node_id_t unordered_ids[] = { 71U, 3U, 42U, 19U };
    static const ucn_node_id_t duplicate_ids[] = { 3U, 42U, 42U };
    ucn_cluster_voter_set_t set;
    ucn_cluster_voter_set_t before_failure;
    ucn_cluster_voter_set_t malformed;
    ucn_cluster_voter_set_t maximum;
    ucn_node_id_t maximum_ids[UCN_CLUSTER_MAX_VOTERS];
    size_t index;
    uint64_t bitmap = 0U;

    (void)memset(&set, 0xA5, sizeof(set));
    ASSERT_TRUE(ucn_cluster_voter_set_build(&set, 77U, unordered_ids,
                                            sizeof(unordered_ids) /
                                            sizeof(unordered_ids[0U])));
    ASSERT_TRUE(ucn_cluster_voter_set_is_valid(&set));
    ASSERT_TRUE(set.config_id == 77U);
    ASSERT_TRUE(set.count == 4U);
    ASSERT_TRUE(set.node_ids[0U] == 3U);
    ASSERT_TRUE(set.node_ids[1U] == 19U);
    ASSERT_TRUE(set.node_ids[2U] == 42U);
    ASSERT_TRUE(set.node_ids[3U] == 71U);
    ASSERT_TRUE(set.node_ids[4U] == 0U);
    ASSERT_TRUE(set.hash == UINT32_C(0x63C30465));
    ASSERT_TRUE(ucn_cluster_voter_set_contains(&set, 42U));
    ASSERT_TRUE(!ucn_cluster_voter_set_contains(&set, 43U));
    ASSERT_TRUE(ucn_cluster_voter_set_quorum(&set) == 3U);
    ASSERT_TRUE(ucn_cluster_voter_set_bitmap_for_node(&set, 3U, &bitmap));
    ASSERT_TRUE(bitmap == UINT64_C(1));
    ASSERT_TRUE(ucn_cluster_voter_set_bitmap_for_node(&set, 71U, &bitmap));
    ASSERT_TRUE(bitmap == (UINT64_C(1) << 3U));
    ASSERT_TRUE(!ucn_cluster_voter_set_bitmap_for_node(&set, 99U, &bitmap));

    for (index = 0U; index < UCN_CLUSTER_MAX_VOTERS; ++index) {
        maximum_ids[index] = (ucn_node_id_t)(index + 1U);
    }
    ASSERT_TRUE(ucn_cluster_voter_set_build(&maximum, 0U, maximum_ids,
                                            UCN_CLUSTER_MAX_VOTERS));
    ASSERT_TRUE(maximum.count == UCN_CLUSTER_MAX_VOTERS);
    ASSERT_TRUE(ucn_cluster_voter_set_quorum(&maximum) ==
                (uint8_t)((UCN_CLUSTER_MAX_VOTERS / 2U) + 1U));
    ASSERT_TRUE(ucn_cluster_voter_set_bitmap_for_node(
        &maximum, maximum_ids[UCN_CLUSTER_MAX_VOTERS - 1U], &bitmap));
    ASSERT_TRUE(bitmap == (UINT64_C(1) << (UCN_CLUSTER_MAX_VOTERS - 1U)));
    ASSERT_TRUE(!ucn_cluster_voter_set_build(
        &maximum, 0U, maximum_ids, UCN_CLUSTER_MAX_VOTERS + 1U));

    before_failure = set;
    ASSERT_TRUE(!ucn_cluster_voter_set_build(&set, 77U, duplicate_ids,
                                             sizeof(duplicate_ids) /
                                             sizeof(duplicate_ids[0U])));
    ASSERT_TRUE(memcmp(&set, &before_failure, sizeof(set)) == 0);
    ASSERT_TRUE(!ucn_cluster_voter_set_build(&set, 77U, NULL, 1U));
    ASSERT_TRUE(memcmp(&set, &before_failure, sizeof(set)) == 0);

    malformed = set;
    malformed.node_ids[1U] = malformed.node_ids[0U];
    ASSERT_TRUE(!ucn_cluster_voter_set_is_valid(&malformed));
    malformed = set;
    malformed.node_ids[set.count] = 1U;
    ASSERT_TRUE(!ucn_cluster_voter_set_is_valid(&malformed));
    malformed = set;
    malformed.hash++;
    ASSERT_TRUE(!ucn_cluster_voter_set_is_valid(&malformed));
    ASSERT_TRUE(!ucn_cluster_voter_set_is_valid(NULL));
    ASSERT_TRUE(ucn_cluster_voter_set_quorum(NULL) == 0U);
    return 0;
}

static int test_verified_v4_join_is_provisional_only(void)
{
    static const ucn_node_id_t old_voters[] = { 2U, 7U, 11U };
    ucn_cluster_t cluster;
    ucn_cluster_t before_failure;
    const ucn_cluster_member_t *member;

    (void)memset(&cluster, 0, sizeof(cluster));
    cluster.role = UCN_CLUSTER_ROLE_HEAD;
    cluster.config.local_node_id = 2U;
    cluster.config.member_capacity = 2U;
    cluster.config.provisional_timeout_ms = 50U;
    ASSERT_TRUE(ucn_cluster_voter_set_build(&cluster.active_voter_set, 9U,
                                            old_voters,
                                            sizeof(old_voters) /
                                            sizeof(old_voters[0U])));
    ASSERT_TRUE(cluster_admit_verified_v4_provisional_member(
                    &cluster, 21U, UINT16_C(0x0005), 100U, NULL) == UCN_OK);
    ASSERT_TRUE(primary_member_count_u16(&cluster) == 1U);
    member = primary_member_find(&cluster, 21U);
    ASSERT_TRUE(member != NULL);
    ASSERT_TRUE(member->status == UCN_CLUSTER_MEMBER_STATUS_PROVISIONAL);
    ASSERT_TRUE(!member->voting);
    ASSERT_TRUE(member->wire_version == UCN_CLUSTER_MEMBER_WIRE_VERSION_V4);
    ASSERT_TRUE(member->capabilities == UINT16_C(0x0005));
    ASSERT_TRUE(member->joined_at_ms == 100U);
    ASSERT_TRUE(member->provisional_deadline_armed);
    ASSERT_TRUE(member->provisional_deadline_ms == 150U);
    /* A Head that fails immediately still leaves the new node outside the
     * old protected set: quorum remains 2 of {2,7,11}, never 3 of four. */
    ASSERT_TRUE(cluster.active_voter_set.count == 3U);
    ASSERT_TRUE(ucn_cluster_voter_set_quorum(&cluster.active_voter_set) == 2U);
    ASSERT_TRUE(!ucn_cluster_voter_set_contains(&cluster.active_voter_set,
                                                21U));
    ASSERT_TRUE(cluster_admit_verified_v4_provisional_member(
                    &cluster, 21U, UINT16_C(0xFFFF), 200U, NULL) == UCN_OK);
    ASSERT_TRUE(member->capabilities == UINT16_C(0x0005));
    ASSERT_TRUE(member->joined_at_ms == 100U);

    before_failure = cluster;
    cluster.role = UCN_CLUSTER_ROLE_MEMBER;
    ASSERT_TRUE(cluster_admit_verified_v4_provisional_member(
                    &cluster, 22U, 0U, 300U, NULL) == UCN_ERR_ACCESS);
    cluster.role = before_failure.role;
    ASSERT_TRUE(memcmp(&cluster, &before_failure, sizeof(cluster)) == 0);
    ASSERT_TRUE(cluster_admit_verified_v4_provisional_member(
                    &cluster, 0U, 0U, 300U, NULL) == UCN_ERR_ARGUMENT);
    ASSERT_TRUE(memcmp(&cluster, &before_failure, sizeof(cluster)) == 0);
    ASSERT_TRUE(cluster_admit_verified_v4_provisional_member(
                    &cluster, 22U, 0U, 300U, NULL) == UCN_OK);
    ASSERT_TRUE(cluster_admit_verified_v4_provisional_member(
                    &cluster, 23U, 0U, 300U, NULL) == UCN_ERR_NO_SPACE);
    return 0;
}

static int test_provisional_deadline_releases_runtime_capacity(void)
{
    ucn_cluster_t cluster;

    (void)memset(&cluster, 0, sizeof(cluster));
    cluster.role = UCN_CLUSTER_ROLE_HEAD;
    cluster.config.member_capacity = 1U;
    cluster.config.provisional_timeout_ms = 50U;
    ASSERT_TRUE(cluster_admit_verified_v4_provisional_member(
                    &cluster, 31U, 0U, 100U, NULL) == UCN_OK);
    ASSERT_TRUE(primary_member_count_u16(&cluster) == 1U);
    ASSERT_TRUE(primary_member_expire_provisionals(&cluster, 149U) == 0U);
    ASSERT_TRUE(primary_member_count_u16(&cluster) == 1U);
    ASSERT_TRUE(primary_member_expire_provisionals(&cluster, 150U) == 1U);
    ASSERT_TRUE(primary_member_count_u16(&cluster) == 0U);
    ASSERT_TRUE(cluster.stats.provisional_members_expired == 1U);
    ASSERT_TRUE(cluster_admit_verified_v4_provisional_member(
                    &cluster, 32U, 0U, 150U, NULL) == UCN_OK);
    ASSERT_TRUE(primary_member_count_u16(&cluster) == 1U);
    /* A committed record cannot be removed by the provisional expiry owner. */
    cluster.primary_members.slots[0U].status =
        (uint8_t)UCN_CLUSTER_MEMBER_STATUS_COMMITTED;
    cluster.primary_members.slots[0U].voting = true;
    cluster.primary_members.slots[0U].provisional_deadline_armed = false;
    cluster.primary_members.slots[0U].provisional_deadline_ms = 0U;
    ASSERT_TRUE(ucn_cluster_member_record_is_valid(
        &cluster.primary_members.slots[0U]));
    ASSERT_TRUE(primary_member_expire_provisionals(&cluster, 999U) == 0U);
    ASSERT_TRUE(primary_member_count_u16(&cluster) == 1U);
    return 0;
}

static int test_legacy_v3_never_becomes_production_backup(void)
{
    ucn_cluster_t cluster;
    ucn_cluster_member_t *member;

    (void)memset(&cluster, 0, sizeof(cluster));
    cluster.role = UCN_CLUSTER_ROLE_HEAD;
    cluster.shadow_phase = UCN_CLUSTER_PHASE_HEAD_NO_BACKUP;
    cluster.config.local_node_id = 2U;
    cluster.config.enabled = true;
    cluster.config.member_capacity = 2U;
    cluster.config.provisional_timeout_ms = 50U;
    member = primary_member_allocate(&cluster, 9U, 100U);
    ASSERT_TRUE(member != NULL);
    ASSERT_TRUE(member->wire_version == UCN_CLUSTER_MEMBER_WIRE_VERSION_V3);
    ASSERT_TRUE(member->provisional_deadline_ms == 150U);
    cluster.candidates[0U].occupied = true;
    cluster.candidates[0U].head_node_id = 9U;
    cluster.candidates[0U].head_score = 10U;
    ASSERT_TRUE(!primary_member_is_protected_voter(member));
    ASSERT_TRUE(primary_member_protected_voter_count_u16(&cluster) == 0U);
    assign_backup(&cluster, 100U);
    ASSERT_TRUE(cluster.backup_node_id == 0U);

    member->status = (uint8_t)UCN_CLUSTER_MEMBER_STATUS_COMMITTED;
    member->voting = true;
    member->wire_version = UCN_CLUSTER_MEMBER_WIRE_VERSION_V4;
    member->provisional_deadline_armed = false;
    member->provisional_deadline_ms = 0U;
    ASSERT_TRUE(ucn_cluster_member_record_is_valid(member));
    ASSERT_TRUE(primary_member_is_protected_voter(member));
    ASSERT_TRUE(primary_member_protected_voter_count_u16(&cluster) == 1U);
    assign_backup(&cluster, 100U);
    ASSERT_TRUE(cluster.backup_node_id == 9U);
    return 0;
}

static int test_member_summary_is_owner_context_read_only(void)
{
    static const ucn_node_id_t voters[] = { 2U, 9U };
    ucn_cluster_t cluster;
    ucn_cluster_member_summary_t summary;
    ucn_cluster_member_summary_t summaries[2U];
    ucn_cluster_member_summary_t before_failure;
    ucn_cluster_member_t *member;

    (void)memset(&cluster, 0, sizeof(cluster));
    cluster.config.member_capacity = 2U;
    cluster.config.provisional_timeout_ms = 50U;
    member = primary_member_allocate(&cluster, 9U, 100U);
    ASSERT_TRUE(member != NULL);
    member->lease_expires_at_ms = 500U;
    ASSERT_TRUE(ucn_cluster_voter_set_build(&cluster.active_voter_set, 44U,
                                            voters,
                                            sizeof(voters) /
                                            sizeof(voters[0U])));
    (void)memset(&summary, 0, sizeof(summary));
    ASSERT_TRUE(ucn_cluster_get_member_summary_at(&cluster, 0U, &summary) ==
                UCN_OK);
    ASSERT_TRUE(summary.node_id == 9U);
    ASSERT_TRUE(summary.lease_expires_at_ms == 500U);
    ASSERT_TRUE(summary.status == UCN_CLUSTER_MEMBER_STATUS_PROVISIONAL);
    ASSERT_TRUE(!summary.voting);
    ASSERT_TRUE(summary.config_id == 44U);
    summary.status = UCN_CLUSTER_MEMBER_STATUS_COMMITTED;
    summary.voting = true;
    summary.config_id = 999U;
    ASSERT_TRUE(member->status == UCN_CLUSTER_MEMBER_STATUS_PROVISIONAL);
    ASSERT_TRUE(!member->voting);
    ASSERT_TRUE(cluster.active_voter_set.config_id == 44U);

    (void)memset(summaries, 0, sizeof(summaries));
    ASSERT_TRUE(ucn_cluster_copy_member_summaries(&cluster, summaries, 2U) ==
                1U);
    ASSERT_TRUE(memcmp(&summaries[0U], &summary, sizeof(summary)) != 0);
    ASSERT_TRUE(summaries[0U].status == UCN_CLUSTER_MEMBER_STATUS_PROVISIONAL);
    ASSERT_TRUE(!summaries[0U].voting && summaries[0U].config_id == 44U);

    before_failure = summary;
    ASSERT_TRUE(ucn_cluster_get_member_summary_at(&cluster, 1U, &summary) ==
                UCN_ERR_NOT_FOUND);
    ASSERT_TRUE(memcmp(&summary, &before_failure, sizeof(summary)) == 0);
    ASSERT_TRUE(ucn_cluster_get_member_summary_at(NULL, 0U, &summary) ==
                UCN_ERR_ARGUMENT);
    ASSERT_TRUE(memcmp(&summary, &before_failure, sizeof(summary)) == 0);
    return 0;
}

static int test_runtime_and_voter_capacities_are_independent(void)
{
    static const ucn_node_id_t voters[] = { 2U, 7U };
    ucn_cluster_t cluster;
    ucn_cluster_member_capacity_view_t view;
    ucn_cluster_member_admission_reason_t reason;
    ucn_cluster_voter_set_t voters_before;

    (void)memset(&cluster, 0, sizeof(cluster));
    cluster.role = UCN_CLUSTER_ROLE_HEAD;
    cluster.config.member_capacity = 1U;
    cluster.config.voter_capacity = 2U; /* includes Head 2 and voter 7 */
    cluster.config.provisional_timeout_ms = 50U;
    ASSERT_TRUE(ucn_cluster_voter_set_build(&cluster.active_voter_set, 5U,
                                            voters,
                                            sizeof(voters) /
                                            sizeof(voters[0U])));
    voters_before = cluster.active_voter_set;
    reason = UCN_CLUSTER_MEMBER_ADMISSION_ARGUMENT;
    ASSERT_TRUE(cluster_admit_verified_v4_provisional_member(
                    &cluster, 9U, 0U, 100U, &reason) == UCN_OK);
    ASSERT_TRUE(reason == UCN_CLUSTER_MEMBER_ADMISSION_NONE);
    ASSERT_TRUE(memcmp(&cluster.active_voter_set, &voters_before,
                       sizeof(voters_before)) == 0);
    ASSERT_TRUE(ucn_cluster_get_member_capacity_view(&cluster, &view) ==
                UCN_OK);
    ASSERT_TRUE(view.runtime_capacity == 1U && view.runtime_used == 1U &&
                view.runtime_available == 0U);
    ASSERT_TRUE(view.voter_capacity == 2U && view.voter_used == 2U &&
                view.voter_available == 0U);

    reason = UCN_CLUSTER_MEMBER_ADMISSION_NONE;
    ASSERT_TRUE(cluster_admit_verified_v4_provisional_member(
                    &cluster, 10U, 0U, 100U, &reason) == UCN_ERR_NO_SPACE);
    ASSERT_TRUE(reason == UCN_CLUSTER_MEMBER_ADMISSION_RUNTIME_CAPACITY);
    reason = UCN_CLUSTER_MEMBER_ADMISSION_NONE;
    ASSERT_TRUE(cluster_preflight_provisional_voter_commit(&cluster, 9U,
                                                            &reason) ==
                UCN_ERR_NO_SPACE);
    ASSERT_TRUE(reason == UCN_CLUSTER_MEMBER_ADMISSION_VOTER_CAPACITY);
    ASSERT_TRUE(memcmp(&cluster.active_voter_set, &voters_before,
                       sizeof(voters_before)) == 0);
    ASSERT_TRUE(!cluster.primary_members.slots[0U].voting);

    cluster.config.voter_capacity = 3U;
    reason = UCN_CLUSTER_MEMBER_ADMISSION_ARGUMENT;
    ASSERT_TRUE(cluster_preflight_provisional_voter_commit(&cluster, 9U,
                                                            &reason) == UCN_OK);
    ASSERT_TRUE(reason == UCN_CLUSTER_MEMBER_ADMISSION_NONE);
    ASSERT_TRUE(!cluster.primary_members.slots[0U].voting);
    (void)memset(&view, 0xA5, sizeof(view));
    ASSERT_TRUE(ucn_cluster_get_member_capacity_view(NULL, &view) ==
                UCN_ERR_ARGUMENT);
    ASSERT_TRUE(view.runtime_capacity == UINT16_C(0xA5A5));
    return 0;
}

/* CLV2-06-R01: this test links the production ucn_cluster archive without
 * either target-private legacy bridge.  It drives only the public RX API:
 * valid, protected 32 B v3 Backup/Takeover control must be rejected before
 * it can change role, known Backup identity, mirror, deadline, Head or Term.
 */
static int test_production_v3_backup_authority_rx_is_fenced(void)
{
    static const ucn_cluster_message_type_t blocked_types[] = {
        UCN_CLUSTER_MSG_HEAD_TAKEOVER,
        UCN_CLUSTER_MSG_BACKUP_ASSIGN,
        UCN_CLUSTER_MSG_BACKUP_READY,
        UCN_CLUSTER_MSG_BACKUP_MEMBER_SYNC,
        UCN_CLUSTER_MSG_PRIMARY_HEARTBEAT,
        UCN_CLUSTER_MSG_TAKEOVER_PREPARE,
        UCN_CLUSTER_MSG_TAKEOVER_ACK,
        UCN_CLUSTER_MSG_BACKUP_RESYNC_REQ,
        UCN_CLUSTER_MSG_BACKUP_REJECT
    };
    ucn_cluster_t cluster;
    ucn_cluster_t before;
    ucn_cluster_config_t config;
    ucn_neighbor_summary_t neighbors[2U];
    ucn_cluster_message_t message;
    uint8_t encoded[UCN_CLUSTER_MESSAGE_BYTES];
    uint32_t now_ms = 100U;
    size_t index;

    (void)memset(&config, 0, sizeof(config));
    config.local_node_id = 2U;
    config.enabled = true;
    config.head_capable = true;
    config.head_score = 1U;
    config.member_capacity = 2U;
    config.persistence_mode = UCN_CLUSTER_PERSISTENCE_VOLATILE_TEST;
    config.now_ms = production_rx_now;
    config.now_context = &now_ms;
    config.send = production_rx_send;
    ASSERT_TRUE(ucn_cluster_config_apply_timing_profile(
                    &config, UCN_CLUSTER_TIMING_PROFILE_DEFAULT) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_init(&cluster, &config) == UCN_OK);

    (void)memset(neighbors, 0, sizeof(neighbors));
    neighbors[0U].state = UCN_NEIGHBOR_ADMITTED;
    neighbors[0U].peer_node_id = 1U;
    neighbors[1U].state = UCN_NEIGHBOR_ADMITTED;
    neighbors[1U].peer_node_id = 3U;
    ASSERT_TRUE(ucn_cluster_sync_neighbors(&cluster, neighbors, 2U) == UCN_OK);

    /* Establish an internally consistent ordinary Member so the RX wrapper's
     * shadow sync itself is a no-op.  The frames below are then rejected by
     * the production authority fence, not by an unrelated setup failure. */
    cluster.role = UCN_CLUSTER_ROLE_MEMBER;
    cluster.cluster_id = 10U;
    cluster.term = 3U;
    cluster.head_node_id = 1U;
    cluster.shadow_phase = UCN_CLUSTER_PHASE_MEMBER_ACTIVE;
    cluster.known_backup_node_id = 3U;
    cluster.known_backup_generation = 4U;

    for (index = 0U; index < sizeof(blocked_types) /
                                sizeof(blocked_types[0U]); ++index) {
        ucn_node_id_t source = blocked_types[index] ==
                                       UCN_CLUSTER_MSG_HEAD_TAKEOVER ||
                                   blocked_types[index] ==
                                       UCN_CLUSTER_MSG_BACKUP_READY ||
                                   blocked_types[index] ==
                                       UCN_CLUSTER_MSG_TAKEOVER_PREPARE ||
                                   blocked_types[index] ==
                                       UCN_CLUSTER_MSG_TAKEOVER_ACK ||
                                   blocked_types[index] ==
                                       UCN_CLUSTER_MSG_BACKUP_RESYNC_REQ ||
                                   blocked_types[index] ==
                                       UCN_CLUSTER_MSG_BACKUP_REJECT
                                   ? 3U : 1U;

        ASSERT_TRUE(production_rx_encode_backup_control(
            &message, blocked_types[index], source, encoded));
        before = cluster;
        ASSERT_TRUE(ucn_cluster_receive(&cluster, source, true, encoded,
                                        sizeof(encoded)) == UCN_ERR_ACCESS);
        ASSERT_TRUE(memcmp(&cluster, &before, sizeof(cluster)) == 0);
    }

    /* A non-self assignment must not install known_backup_* either. */
    ASSERT_TRUE(production_rx_encode_backup_control(
        &message, UCN_CLUSTER_MSG_BACKUP_ASSIGN, 1U, encoded));
    message.sync_token = 3U;
    ASSERT_TRUE(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    before = cluster;
    ASSERT_TRUE(ucn_cluster_receive(&cluster, 1U, true, encoded,
                                    sizeof(encoded)) == UCN_ERR_ACCESS);
    ASSERT_TRUE(memcmp(&cluster, &before, sizeof(cluster)) == 0);

    /* The public wrapper normally repairs a stale diagnostic shadow after
     * rejected RX.  An authority-fenced frame is stricter: even a deliberately
     * desynchronised shadow must remain untouched. */
    cluster.shadow_phase = UCN_CLUSTER_PHASE_DETACHED_OBSERVE;
    ASSERT_TRUE(production_rx_encode_backup_control(
        &message, UCN_CLUSTER_MSG_BACKUP_ASSIGN, 1U, encoded));
    before = cluster;
    ASSERT_TRUE(ucn_cluster_receive(&cluster, 1U, true, encoded,
                                    sizeof(encoded)) == UCN_ERR_ACCESS);
    ASSERT_TRUE(memcmp(&cluster, &before, sizeof(cluster)) == 0);
    cluster.shadow_phase = UCN_CLUSTER_PHASE_MEMBER_ACTIVE;

    /* A complete v3 Type12 BEGIN/data/END stream must never move this
     * production Member to BACKUP_READY or allocate its mirror. */
    for (index = 0U; index < 3U; ++index) {
        ASSERT_TRUE(production_rx_encode_backup_control(
            &message, UCN_CLUSTER_MSG_BACKUP_MEMBER_SYNC, 1U, encoded));
        if (index == 1U) {
            message.flags = 0U;
            message.member_node_id = 3U;
            message.member_nonce = 1U;
        } else if (index == 2U) {
            message.flags = UCN_CLUSTER_FLAG_SYNC_END;
        }
        ASSERT_TRUE(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
        before = cluster;
        ASSERT_TRUE(ucn_cluster_receive(&cluster, 1U, true, encoded,
                                        sizeof(encoded)) == UCN_ERR_ACCESS);
        ASSERT_TRUE(memcmp(&cluster, &before, sizeof(cluster)) == 0);
    }
    ASSERT_TRUE(cluster.role == UCN_CLUSTER_ROLE_MEMBER);
    ASSERT_TRUE(!cluster.backup_ready);
    ASSERT_TRUE(primary_member_count_u16(&cluster) == 0U);
    return 0;
}

/* CLV2-07-12: exercise the public v3 RX owner from the production archive.
 * JOIN_REQUEST is the only legacy admission route that remains available
 * before a v4 Config owner exists.  It may reserve bounded runtime capacity,
 * but it must not auto-commit a voter or make the member eligible as Backup.
 */
static int test_production_v3_join_request_stays_provisional(void)
{
    ucn_cluster_t cluster;
    ucn_cluster_config_t config;
    ucn_neighbor_summary_t neighbor;
    ucn_cluster_message_t message;
    const ucn_cluster_member_t *member;
    uint8_t encoded[UCN_CLUSTER_MESSAGE_BYTES];
    uint32_t now_ms = 100U;

    (void)memset(&config, 0, sizeof(config));
    config.local_node_id = 2U;
    config.enabled = true;
    config.head_capable = true;
    config.head_score = 1U;
    config.member_capacity = 2U;
    config.persistence_mode = UCN_CLUSTER_PERSISTENCE_VOLATILE_TEST;
    config.now_ms = production_rx_now;
    config.now_context = &now_ms;
    config.send = production_rx_send;
    ASSERT_TRUE(ucn_cluster_config_apply_timing_profile(
                    &config, UCN_CLUSTER_TIMING_PROFILE_DEFAULT) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_init(&cluster, &config) == UCN_OK);

    (void)memset(&neighbor, 0, sizeof(neighbor));
    neighbor.state = UCN_NEIGHBOR_ADMITTED;
    neighbor.peer_node_id = 1U;
    ASSERT_TRUE(ucn_cluster_sync_neighbors(&cluster, &neighbor, 1U) == UCN_OK);

    /* Establish a coherent Head-only fixture.  This is not a v4 authority
     * path: the test verifies that the remaining v3 join admission has no
     * authority side effect. */
    cluster.role = UCN_CLUSTER_ROLE_HEAD;
    cluster.cluster_id = 10U;
    cluster.term = 3U;
    cluster.head_node_id = config.local_node_id;
    cluster.shadow_phase = UCN_CLUSTER_PHASE_HEAD_NO_BACKUP;

    (void)memset(&message, 0, sizeof(message));
    message.type = UCN_CLUSTER_MSG_JOIN_REQUEST;
    message.role = UCN_CLUSTER_ROLE_JOIN_PENDING;
    message.cluster_id = cluster.cluster_id;
    message.term = cluster.term;
    message.head_node_id = config.local_node_id;
    message.head_score = config.head_score;
    message.available_capacity = 1U;
    message.lease_ms = config.lease_ms;
    message.nonce = 7U;
    ASSERT_TRUE(ucn_cluster_message_encode(&message, encoded) == UCN_OK);
    ASSERT_TRUE(ucn_cluster_receive(&cluster, 1U, true, encoded,
                                    sizeof(encoded)) == UCN_OK);

    member = primary_member_find(&cluster, 1U);
    ASSERT_TRUE(member != NULL);
    ASSERT_TRUE(member->status == UCN_CLUSTER_MEMBER_STATUS_PROVISIONAL);
    ASSERT_TRUE(!member->voting);
    ASSERT_TRUE(member->wire_version == UCN_CLUSTER_MEMBER_WIRE_VERSION_V3);
    ASSERT_TRUE(member->provisional_deadline_armed);
    ASSERT_TRUE(member->provisional_deadline_ms > now_ms);
    ASSERT_TRUE(!primary_member_is_protected_voter(member));
    ASSERT_TRUE(!ucn_cluster_voter_set_contains(&cluster.active_voter_set, 1U));
    ASSERT_TRUE(cluster.backup_node_id == 0U);
    return 0;
}

int main(void)
{
    int result = 0;

    result |= test_status_and_transition_contract();
    result |= test_record_contract();
    result |= test_legacy_v3_bridge();
    result |= test_primary_member_table_contract();
    result |= test_voter_set_contract();
    result |= test_verified_v4_join_is_provisional_only();
    result |= test_provisional_deadline_releases_runtime_capacity();
    result |= test_legacy_v3_never_becomes_production_backup();
    result |= test_member_summary_is_owner_context_read_only();
    result |= test_runtime_and_voter_capacities_are_independent();
    result |= test_production_v3_backup_authority_rx_is_fenced();
    result |= test_production_v3_join_request_stays_provisional();
    return result;
}
