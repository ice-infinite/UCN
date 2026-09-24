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

static int configure(ucn_handle_t *group_out, bool with_tree)
{
    ucn_i_group_config_t owner_config;
    ucn_i_group_context_config_t group;
    uint8_t index;

    memset(&owner, 0, sizeof(owner));
    memset(&lock_state, 0, sizeof(lock_state));
    memset(&owner_config, 0, sizeof(owner_config));
    owner_config.runtime_instance = 1U;
    owner_config.realm_id = 1U;
    owner_config.owner_instance = 8U;
    owner_config.state_lock.struct_size = sizeof(owner_config.state_lock);
    owner_config.state_lock.api_version = UCN_I_LOCK_OPS_VERSION;
    owner_config.state_lock.context = &lock_state;
    owner_config.state_lock.enter = enter;
    owner_config.state_lock.leave = leave;
    CHECK(ucn_i_group_owner_init(&owner, &owner_config) == UCN_OK);

    memset(&group, 0, sizeof(group));
    group.realm_id = 1U;
    group.group_id = 51U;
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
    group.secure_required = 1U;
    group.security.runtime_instance = 1U;
    group.security.key_generation = 5U;
    group.security.owner_instance = 9U;
    group.security.slot = 2U;
    group.security.generation = 6U;
    group.security.valid = 1U;
    group.security.exact_principal = 1U;
    fill_bytes(group.security.context_digest,
               sizeof(group.security.context_digest), 0x60U);
    if (with_tree) {
        group.tree.route_causal_id = 0x1122334455667788ULL;
        group.tree.route_generation = 7U;
        group.tree.tree_generation = 9U;
        group.tree.owner_instance = 10U;
        group.tree.slot = 1U;
        group.tree.generation = 2U;
        group.tree.valid = 1U;
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
    CHECK(ucn_i_group_install_static(&owner, 0U, &group, true, true,
                                     group_out) == UCN_OK);
    return 0;
}

static ucn_i_group_send_request_t request_for(uint64_t send_id,
                                               uint8_t scope)
{
    ucn_i_group_send_request_t request;
    memset(&request, 0, sizeof(request));
    request.send_id = send_id;
    request.absolute_deadline_us = 1000U;
    request.expected_group_generation = 2U;
    request.expected_policy_generation = 3U;
    request.expected_member_generation = 4U;
    request.expected_key_generation = 5U;
    request.success_scope = scope;
    request.reliable_receipts = scope == UCN_I_GROUP_SCOPE_LOCAL_ONLY ? 0U : 1U;
    request.authenticated_delivery = 1U;
    request.exact_principal_delivery = 1U;
    fill_bytes(request.payload_digest, sizeof(request.payload_digest), 0xA0U);
    return request;
}

static ucn_i_group_member_receipt_t receipt_for(
    const ucn_i_group_attempt_view_t *attempt,
    ucn_result_t terminal_result)
{
    ucn_i_group_member_receipt_t receipt;
    uint8_t member_index = (uint8_t)(attempt->member_address - 200U);
    memset(&receipt, 0, sizeof(receipt));
    receipt.send_id = attempt->send_id;
    receipt.group_id = attempt->group_id;
    receipt.group_generation = attempt->group_generation;
    receipt.member_address = attempt->member_address;
    receipt.member_binding_generation = attempt->member_binding_generation;
    fill_bytes(receipt.member_principal, sizeof(receipt.member_principal),
               (uint8_t)(0x10U + 0x20U * member_index));
    memcpy(receipt.payload_digest, attempt->payload_digest,
           sizeof(receipt.payload_digest));
    receipt.terminal_result = terminal_result;
    receipt.authenticated = 1U;
    receipt.terminal = 1U;
    return receipt;
}

static int complete_attempt(ucn_i_group_attempt_view_t *attempt)
{
    CHECK(ucn_i_group_attempt_note_submitted(&owner, attempt->attempt) == UCN_OK);
    CHECK(ucn_i_group_attempt_complete(&owner, attempt->attempt, UCN_OK) == UCN_OK);
    return 0;
}

static int test_quorum_and_frozen_plan(ucn_handle_t group)
{
    ucn_i_group_send_request_t request =
        request_for(100U, UCN_I_GROUP_SCOPE_QUORUM);
    ucn_i_group_attempt_view_t attempts[3];
    ucn_i_group_member_receipt_t receipt;
    ucn_i_group_send_view_t view;
    ucn_handle_t send;
    uint8_t index;
    int line;

    CHECK(ucn_i_group_send_begin(&owner, group, &request, 10U, &send) == UCN_OK);
    for (index = 0U; index < 3U; ++index) {
        CHECK(ucn_i_group_attempt_peek(&owner, &attempts[index]) == UCN_OK);
        CHECK(attempts[index].group_generation == 2U &&
              attempts[index].policy_generation == 3U &&
              attempts[index].member_generation == 4U &&
              attempts[index].endpoint == 0x1001U &&
              attempts[index].opcode == 0x2001U &&
              attempts[index].security.key_generation == 5U &&
              attempts[index].plan == UCN_I_GROUP_PLAN_UNICAST);
        line = complete_attempt(&attempts[index]);
        if (line != 0) return line;
    }
    CHECK(ucn_i_group_attempt_peek(&owner, &attempts[0]) == UCN_ERR_NOT_FOUND);
    receipt = receipt_for(&attempts[0], UCN_OK);
    CHECK(ucn_i_group_send_accept_receipt(&owner, send, &receipt) == UCN_OK);
    CHECK(ucn_i_group_send_view(&owner, send, &view) == UCN_OK);
    CHECK(view.completion_satisfied == 0U && view.success_bitmap != 0U);
    receipt = receipt_for(&attempts[1], UCN_OK);
    CHECK(ucn_i_group_send_accept_receipt(&owner, send, &receipt) == UCN_OK);
    CHECK(ucn_i_group_send_view(&owner, send, &view) == UCN_OK);
    CHECK(view.completion_satisfied == 1U &&
          view.phase == UCN_I_GROUP_SEND_COMPLETE);
    receipt.terminal_result = UCN_ERR_TIMEOUT;
    CHECK(ucn_i_group_send_accept_receipt(&owner, send, &receipt) ==
          UCN_ERR_REPLAY);
    CHECK(ucn_i_group_send_retire(&owner, send) == UCN_OK);
    return 0;
}

static int test_policy_and_deadline(ucn_handle_t group)
{
    ucn_i_group_send_request_t request =
        request_for(101U, UCN_I_GROUP_SCOPE_ALL_MEMBERS);
    ucn_i_group_send_view_t view;
    ucn_handle_t send;
    uint16_t changed = 0U;
    uint16_t inspected = 0U;

    request.authenticated_delivery = 0U;
    CHECK(ucn_i_group_send_begin(&owner, group, &request, 10U, &send) ==
          UCN_ERR_POLICY);
    request = request_for(102U, UCN_I_GROUP_SCOPE_ALL_MEMBERS);
    CHECK(ucn_i_group_send_begin(&owner, group, &request, 10U, &send) == UCN_OK);
    CHECK(ucn_i_group_step(&owner, request.absolute_deadline_us,
                           (uint16_t)(UCN_I_GROUP_SEND_COUNT +
                                      UCN_I_GROUP_RX_COUNT),
                           &inspected, &changed) == UCN_OK);
    CHECK(changed == 1U);
    CHECK(ucn_i_group_send_view(&owner, send, &view) == UCN_OK);
    CHECK(view.phase == UCN_I_GROUP_SEND_FAILED);
    return 0;
}

static int test_dependency_fence(ucn_handle_t group)
{
    ucn_i_group_dependency_event_t event;
    ucn_i_group_context_config_t config;
    uint8_t phase = 0U;

    memset(&event, 0, sizeof(event));
    event.group_id = 51U;
    event.group_generation = 2U;
    event.policy_generation = 3U;
    event.member_generation = 4U;
    event.key_generation = 5U;
    event.security_current = 1U;
    event.tree_current = 1U;
    event.authority_current = 1U;
    CHECK(ucn_i_group_dependency_change(&owner, &event) == UCN_OK);
    CHECK(ucn_i_group_context_view(&owner, group, &config, &phase) == UCN_OK);
    CHECK(phase == UCN_I_GROUP_ACTIVE);
    event.security_current = 0U;
    CHECK(ucn_i_group_dependency_change(&owner, &event) == UCN_OK);
    CHECK(ucn_i_group_context_view(&owner, group, &config, &phase) == UCN_OK);
    CHECK(phase == UCN_I_GROUP_FENCED);
    return 0;
}

static int test_tree_plan(void)
{
    ucn_i_group_send_request_t request;
    ucn_i_group_attempt_view_t attempt;
    ucn_handle_t group;
    ucn_handle_t send;

    CHECK(ucn_i_group_owner_destroy(&owner) == UCN_OK);
    CHECK(configure(&group, true) == 0);
    request = request_for(103U, UCN_I_GROUP_SCOPE_ALL_MEMBERS);
    request.expected_tree_generation = 9U;
    CHECK(ucn_i_group_send_begin(&owner, group, &request, 10U, &send) ==
          UCN_OK);
    CHECK(ucn_i_group_attempt_peek(&owner, &attempt) == UCN_OK);
    CHECK(attempt.plan == UCN_I_GROUP_PLAN_TREE &&
          attempt.tree.route_causal_id == 0x1122334455667788ULL &&
          attempt.tree.route_generation == 7U &&
          attempt.tree.tree_generation == 9U &&
          attempt.member_address == 0U);
    CHECK(ucn_i_group_attempt_note_submitted(&owner, attempt.attempt) ==
          UCN_OK);
    CHECK(ucn_i_group_attempt_complete(&owner, attempt.attempt, UCN_OK) ==
          UCN_OK);
    CHECK(ucn_i_group_attempt_peek(&owner, &attempt) == UCN_ERR_NOT_FOUND);
    return 0;
}

int main(void)
{
    ucn_handle_t group;
    int result = configure(&group, false);
    if (result != 0) return result;
    result = test_quorum_and_frozen_plan(group);
    if (result != 0) return result;
    result = test_policy_and_deadline(group);
    if (result != 0) return result;
    result = test_dependency_fence(group);
    if (result != 0) return result;
    result = test_tree_plan();
    if (result != 0) return result;
    CHECK(ucn_i_group_owner_destroy(&owner) == UCN_OK);
    printf("group_send_tests=PASS attempts=%u\n",
           (unsigned)UCN_I_GROUP_ATTEMPT_COUNT);
    return 0;
}
