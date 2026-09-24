#include "internal/ucn_group.h"

#include <stdio.h>
#include <string.h>

#define CHECK(expression_)                                                     \
    do {                                                                       \
        if (!(expression_)) {                                                  \
            fprintf(stderr, "check failed at %d: %s\n", __LINE__,            \
                    #expression_);                                             \
            return __LINE__;                                                   \
        }                                                                      \
    } while (0)

typedef struct test_lock { uint8_t held; } test_lock_t;
static test_lock_t lock_state;
static ucn_i_group_owner_t owner;

static ucn_result_t enter(void *context)
{
    test_lock_t *lock = context;
    if (lock == NULL || lock->held != 0U) return UCN_ERR_STATE;
    lock->held = 1U;
    return UCN_OK;
}
static void leave(void *context) { ((test_lock_t *)context)->held = 0U; }

static void fill_bytes(uint8_t *bytes, size_t count, uint8_t seed)
{
    size_t index;
    for (index = 0U; index < count; ++index) bytes[index] = (uint8_t)(seed + index);
}

static ucn_i_group_context_config_t make_group(uint32_t group_id,
                                                bool secure)
{
    ucn_i_group_context_config_t group;
    uint8_t index;
    memset(&group, 0, sizeof(group));
    group.realm_id = 1U;
    group.group_id = group_id;
    group.group_generation = 2U;
    group.policy_generation = 3U;
    group.member_generation = 4U;
    group.endpoint = 0x1001U;
    group.opcode = 0x2001U;
    group.mode = UCN_I_GROUP_STATIC;
    group.member_count = 3U;
    group.unicast_fanout_limit = 3U;
    group.quorum_weight = 2U;
    group.allowed_scope_mask =
        (uint8_t)((1U << UCN_I_GROUP_SCOPE_COUNT) - 1U);
    group.secure_required = secure ? 1U : 0U;
    group.public_static = secure ? 0U : 1U;
    if (secure) {
        group.security.runtime_instance = 1U;
        group.security.key_generation = 5U;
        group.security.owner_instance = 9U;
        group.security.slot = 2U;
        group.security.generation = 6U;
        group.security.valid = 1U;
        group.security.exact_principal = 1U;
        fill_bytes(group.security.context_digest,
                   sizeof(group.security.context_digest), 0x60U);
    }
    for (index = 0U; index < group.member_count; ++index) {
        group.members[index].address = (uint32_t)(200U + index);
        group.members[index].binding_generation = (uint32_t)(30U + index);
        group.members[index].sender_slot = (uint16_t)(index + 1U);
        group.members[index].weight = 1U;
        fill_bytes(group.members[index].principal,
                   sizeof(group.members[index].principal),
                   (uint8_t)(0x10U + 0x20U * index));
    }
    return group;
}

static int configure(void)
{
    ucn_i_group_config_t config;
    ucn_i_group_context_config_t secure = make_group(61U, true);
    ucn_i_group_context_config_t public_group = make_group(62U, false);
    ucn_handle_t handle;

    memset(&owner, 0, sizeof(owner));
    memset(&lock_state, 0, sizeof(lock_state));
    memset(&config, 0, sizeof(config));
    config.runtime_instance = 1U;
    config.realm_id = 1U;
    config.owner_instance = 8U;
    config.state_lock.struct_size = sizeof(config.state_lock);
    config.state_lock.api_version = UCN_I_LOCK_OPS_VERSION;
    config.state_lock.context = &lock_state;
    config.state_lock.enter = enter;
    config.state_lock.leave = leave;
    CHECK(ucn_i_group_owner_init(&owner, &config) == UCN_OK);
    CHECK(ucn_i_group_install_static(&owner, 0U, &secure, true, true,
                                     &handle) == UCN_OK);
#if UCN_I_GROUP_STATIC_COUNT > 1U
    CHECK(ucn_i_group_install_static(&owner, 1U, &public_group, true, true,
                                     &handle) == UCN_OK);
#else
    (void)public_group;
#endif
    return 0;
}

#if UCN_I_GROUP_STATIC_COUNT == 1U
static int configure_public_only(void)
{
    ucn_i_group_config_t config;
    ucn_i_group_context_config_t public_group = make_group(62U, false);
    ucn_handle_t handle;

    CHECK(ucn_i_group_owner_destroy(&owner) == UCN_OK);
    memset(&owner, 0, sizeof(owner));
    memset(&lock_state, 0, sizeof(lock_state));
    memset(&config, 0, sizeof(config));
    config.runtime_instance = 1U;
    config.realm_id = 1U;
    config.owner_instance = 8U;
    config.state_lock.struct_size = sizeof(config.state_lock);
    config.state_lock.api_version = UCN_I_LOCK_OPS_VERSION;
    config.state_lock.context = &lock_state;
    config.state_lock.enter = enter;
    config.state_lock.leave = leave;
    CHECK(ucn_i_group_owner_init(&owner, &config) == UCN_OK);
    CHECK(ucn_i_group_install_static(&owner, 0U, &public_group, true, true,
                                     &handle) == UCN_OK);
    return 0;
}
#endif

static ucn_i_group_receive_facts_t receive_facts(uint32_t group_id,
                                                  bool secure,
                                                  uint64_t send_id)
{
    ucn_i_group_receive_facts_t facts;
    memset(&facts, 0, sizeof(facts));
    facts.send_id = send_id;
    facts.absolute_deadline_us = 1000U;
    facts.group_id = group_id;
    facts.group_generation = 2U;
    facts.policy_generation = 3U;
    facts.member_generation = 4U;
    facts.key_generation = secure ? 5U : 0U;
    facts.source_address = 200U;
    facts.source_binding_generation = 30U;
    facts.relay_member_bitmap = UINT32_C(1) << 1U;
    facts.endpoint = 0x1001U;
    facts.opcode = 0x2001U;
    facts.sender_slot = 1U;
    fill_bytes(facts.source_principal, sizeof(facts.source_principal), 0x10U);
    fill_bytes(facts.payload_digest, sizeof(facts.payload_digest), 0x90U);
    facts.fresh_authenticated = secure ? 1U : 0U;
    facts.public_unsecured = secure ? 0U : 1U;
    facts.acl_authorized = secure ? 1U : 0U;
    facts.exact_principal = secure ? 1U : 0U;
    facts.local_member = 1U;
    facts.relay_egress_count = 1U;
    return facts;
}

static ucn_i_group_security_commit_t security_commit(
    const ucn_i_group_receive_facts_t *facts, bool secure)
{
    ucn_i_group_security_commit_t commit;
    memset(&commit, 0, sizeof(commit));
    commit.send_id = facts->send_id;
    commit.group_id = facts->group_id;
    commit.group_generation = facts->group_generation;
    commit.key_generation = facts->key_generation;
    commit.security_owner_instance = secure ? 9U : 0U;
    commit.replay_committed = secure ? 1U : 0U;
    memcpy(commit.payload_digest, facts->payload_digest,
           sizeof(commit.payload_digest));
    return commit;
}

static int complete_relay_attempt(void)
{
    ucn_i_group_attempt_view_t attempt;
    CHECK(ucn_i_group_attempt_peek(&owner, &attempt) == UCN_OK);
    CHECK(attempt.plan == UCN_I_GROUP_PLAN_UNICAST &&
          attempt.member_address == 201U &&
          attempt.member_binding_generation == 31U &&
          attempt.sender_slot == 2U);
    CHECK(ucn_i_group_attempt_note_submitted(&owner, attempt.attempt) == UCN_OK);
    CHECK(ucn_i_group_attempt_complete(&owner, attempt.attempt, UCN_OK) == UCN_OK);
    return 0;
}

static int test_secure_receive_and_replay(void)
{
    ucn_i_group_receive_facts_t facts = receive_facts(61U, true, 700U);
    ucn_i_group_security_commit_t commit;
    ucn_i_group_rx_view_t replay;
    ucn_handle_t reservation;
    ucn_handle_t empty;
    int line;

    facts.acl_authorized = 0U;
    CHECK(ucn_i_group_receive_preflight(&owner, &facts, 10U, &reservation,
                                        &replay) == UCN_ERR_SECURITY);
    facts.acl_authorized = 1U;
    CHECK(ucn_i_group_receive_preflight(&owner, &facts, 10U, &reservation,
                                        &replay) == UCN_OK);
    CHECK(ucn_i_group_attempt_peek(&owner, &(ucn_i_group_attempt_view_t){0}) ==
          UCN_ERR_NOT_FOUND);
    commit = security_commit(&facts, true);
    commit.key_generation++;
    CHECK(ucn_i_group_receive_publish(&owner, reservation, &commit) ==
          UCN_ERR_SECURITY);
    commit = security_commit(&facts, true);
    CHECK(ucn_i_group_receive_publish(&owner, reservation, &commit) == UCN_OK);
    line = complete_relay_attempt();
    if (line != 0) return line;
    CHECK(ucn_i_group_receive_complete(&owner, reservation, UCN_OK) == UCN_OK);

    facts.fresh_authenticated = 0U;
    facts.authenticated_replay_candidate = 1U;
    memset(&empty, 0, sizeof(empty));
    CHECK(ucn_i_group_receive_preflight(&owner, &facts, 20U, &empty,
                                        &replay) == UCN_OK);
    CHECK(replay.action == UCN_I_GROUP_RX_REPLAY_RECEIPT &&
          replay.stable_receipt == 1U && empty.runtime_instance == 0U);
    facts.payload_digest[0] ^= 1U;
    CHECK(ucn_i_group_receive_preflight(&owner, &facts, 20U, &empty,
                                        &replay) == UCN_ERR_REPLAY);
    facts.payload_digest[0] ^= 1U;
    CHECK(ucn_i_group_receive_retire(&owner, reservation) == UCN_OK);
    CHECK(ucn_i_group_receive_preflight(&owner, &facts, 20U, &empty,
                                        &replay) == UCN_ERR_REPLAY);
    return 0;
}

static int test_public_static_dedup(void)
{
    ucn_i_group_receive_facts_t facts = receive_facts(62U, false, 701U);
    ucn_i_group_security_commit_t commit = security_commit(&facts, false);
    ucn_i_group_rx_view_t replay;
    ucn_handle_t reservation;
    ucn_handle_t empty;
    int line;

    CHECK(ucn_i_group_receive_preflight(&owner, &facts, 10U, &reservation,
                                        &replay) == UCN_OK);
    CHECK(ucn_i_group_receive_publish(&owner, reservation, &commit) == UCN_OK);
    memset(&empty, 0xA5, sizeof(empty));
    memset(&replay, 0x5A, sizeof(replay));
    CHECK(ucn_i_group_receive_preflight(&owner, &facts, 11U, &empty,
                                        &replay) == UCN_ERR_STATE);
    CHECK(empty.runtime_instance == UINT32_C(0xA5A5A5A5));
    CHECK(replay.send_id == UINT64_C(0x5A5A5A5A5A5A5A5A));
    facts.payload_digest[0] ^= 1U;
    CHECK(ucn_i_group_receive_preflight(&owner, &facts, 11U, &empty,
                                        &replay) == UCN_ERR_REPLAY);
    facts.payload_digest[0] ^= 1U;
    line = complete_relay_attempt();
    if (line != 0) return line;
    CHECK(ucn_i_group_receive_complete(&owner, reservation, UCN_OK) == UCN_OK);
    memset(&empty, 0, sizeof(empty));
    CHECK(ucn_i_group_receive_preflight(&owner, &facts, 20U, &empty,
                                        &replay) == UCN_OK);
    CHECK(replay.action == UCN_I_GROUP_RX_REPLAY_RECEIPT &&
          empty.runtime_instance == 0U);
    CHECK(ucn_i_group_receive_retire(&owner, reservation) == UCN_OK);
    return 0;
}

int main(void)
{
    int result = configure();
    if (result != 0) return result;
    result = test_secure_receive_and_replay();
    if (result != 0) return result;
#if UCN_I_GROUP_STATIC_COUNT == 1U
    result = configure_public_only();
    if (result != 0) return result;
#endif
    result = test_public_static_dedup();
    if (result != 0) return result;
    printf("group_receive_tests=PASS receipts=%u\n",
           (unsigned)UCN_I_GROUP_RX_COUNT);
    return 0;
}
