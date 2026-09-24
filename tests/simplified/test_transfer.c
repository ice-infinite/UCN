#include "internal/ucn_transport.h"

#include <stdio.h>
#include <string.h>

#define CHECK(expr_)                                                         \
    do {                                                                     \
        if (!(expr_)) {                                                      \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,       \
                    __LINE__, #expr_);                                       \
            return 1;                                                        \
        }                                                                    \
    } while (0)

typedef struct test_lock {
    uint8_t held;
} test_lock_t;

static ucn_i_transport_owner_t owner;
static test_lock_t test_lock;

static ucn_result_t lock_enter(void *context)
{
    test_lock_t *lock = context;
    if (lock == NULL || lock->held != 0U) {
        return UCN_ERR_STATE;
    }
    lock->held = 1U;
    return UCN_OK;
}

static void lock_leave(void *context)
{
    ((test_lock_t *)context)->held = 0U;
}

static int start_owner(void)
{
    ucn_i_transport_config_t config;
    ucn_i_lock_ops_t ops;

    memset(&owner, 0, sizeof(owner));
    memset(&test_lock, 0, sizeof(test_lock));
    memset(&ops, 0, sizeof(ops));
    ops.struct_size = sizeof(ops);
    ops.api_version = UCN_I_LOCK_OPS_VERSION;
    ops.context = &test_lock;
    ops.enter = lock_enter;
    ops.leave = lock_leave;
    memset(&config, 0, sizeof(config));
    config.reliable_lifetime_us = 1000U;
    config.reliable_retry_us = 100U;
    config.receipt_lifetime_us = 500U;
    config.runtime_instance = 21U;
    config.owner_instance = 22U;
    config.reliable_max_attempts = 3U;
    config.state_lock = ops;
    return ucn_i_transport_owner_init(&owner, &config) == UCN_OK ? 0 : 1;
}

static ucn_i_transport_transfer_setup_t setup_for(
    const uint8_t *message,
    uint32_t bytes,
    uint8_t parent_kind)
{
    ucn_i_transport_transfer_setup_t setup;
    ucn_i_sha256_workspace_t workspace;

    memset(&setup, 0, sizeof(setup));
    memset(&workspace, 0, sizeof(workspace));
    setup.parent_kind = parent_kind;
    setup.parent_id = parent_kind == UCN_I_TRANSPORT_PARENT_KIND_C1 ? 0U : 7U;
    setup.parent_generation = 9U;
    setup.transfer_id = 10U;
    setup.service_id = 11U;
    setup.total_length = bytes;
    setup.delivery = UCN_DELIVERY_RELIABLE;
    setup.interaction = UCN_INTERACTION_ONE_WAY;
    setup.fragment_budget = 4U;
    setup.fragment_count = (uint16_t)((bytes + 3U) / 4U);
    setup.lifetime_ms = 1000U;
    (void)ucn_i_sha256_128(message, bytes, setup.message_digest, &workspace);
    return setup;
}

static void fill_principal(uint8_t principal[16], uint8_t seed)
{
    size_t index;

    for (index = 0U; index < 16U; ++index) {
        principal[index] = (uint8_t)(seed + index);
    }
}

static ucn_i_transport_transfer_facts_t flow_facts(bool reverse)
{
    ucn_i_transport_transfer_facts_t facts;

    memset(&facts, 0, sizeof(facts));
    facts.source.address = reverse ? 3U : 1U;
    facts.source.generation = reverse ? 4U : 2U;
    fill_principal(facts.source.principal, reverse ? 0x30U : 0x10U);
    facts.destination.address = reverse ? 1U : 3U;
    facts.destination.generation = reverse ? 2U : 4U;
    fill_principal(facts.destination.principal,
                   reverse ? 0x10U : 0x30U);
    facts.security.session_generation = 7U;
    facts.security.key_generation = 8U;
    facts.security.origin_security = 1U;
    memset(facts.parent_fingerprint, 0x5AU,
           sizeof(facts.parent_fingerprint));
    facts.realm = 5U;
    facts.transport_policy_generation = 6U;
    facts.parent_kind = UCN_I_TRANSPORT_PARENT_KIND_FLOW;
    return facts;
}

static ucn_i_transport_transfer_facts_t flow_o0_facts(void)
{
    ucn_i_transport_transfer_facts_t facts = flow_facts(false);

    memset(&facts.security, 0, sizeof(facts.security));
    facts.security.link_generation = 12U;
    facts.security.policy_generation = 13U;
    facts.security.link_id = 14U;
    facts.security.endpoint_public_unauthenticated = 1U;
    facts.security.trusted_link_policy = 1U;
    return facts;
}

static int test_flow_prefixes(void)
{
    static const uint8_t fragment_golden[
        UCN_I_TRANSPORT_FLOW_FRAGMENT_PREFIX_BYTES] = {
        0x01U, 0x02U, 0x03U, 0x04U, 0x00U, 0x01U, 0x00U, 0x02U
    };
    static const uint8_t sack_golden[UCN_I_TRANSPORT_FLOW_SACK_BYTES] = {
        0x01U, 0x02U, 0x03U, 0x04U, 0x80U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x03U, 0x00U, 0x04U
    };
    ucn_i_transport_fragment_prefix_t fragment;
    ucn_i_transport_sack_t sack;
    uint8_t bytes[UCN_I_TRANSPORT_C1_SACK_BYTES];
    size_t output_bytes;

    memset(&fragment, 0, sizeof(fragment));
    fragment.parent_kind = UCN_I_TRANSPORT_PARENT_KIND_FLOW;
    fragment.transfer_id = UINT32_C(0x01020304);
    fragment.fragment_index = 1U;
    fragment.fragment_count = 2U;
    CHECK(ucn_i_transport_fragment_prefix_encode(
              &fragment, bytes, sizeof(bytes), &output_bytes) == UCN_OK);
    CHECK(output_bytes == sizeof(fragment_golden));
    CHECK(memcmp(bytes, fragment_golden, sizeof(fragment_golden)) == 0);
    memset(&fragment, 0, sizeof(fragment));
    CHECK(ucn_i_transport_fragment_prefix_decode(
              fragment_golden, sizeof(fragment_golden),
              UCN_I_TRANSPORT_PARENT_KIND_FLOW, &fragment) == UCN_OK);
    CHECK(fragment.parent_generation == 0U);

    memset(&sack, 0, sizeof(sack));
    sack.parent_kind = UCN_I_TRANSPORT_PARENT_KIND_FLOW;
    sack.transfer_id = UINT32_C(0x01020304);
    sack.bitmap = 3U;
    sack.receive_credit = 4U;
    CHECK(ucn_i_transport_sack_encode(
              &sack, bytes, sizeof(bytes), &output_bytes) == UCN_OK);
    CHECK(output_bytes == sizeof(sack_golden));
    CHECK(memcmp(bytes, sack_golden, sizeof(sack_golden)) == 0);
    memset(&sack, 0, sizeof(sack));
    CHECK(ucn_i_transport_sack_decode(
              sack_golden, sizeof(sack_golden),
              UCN_I_TRANSPORT_PARENT_KIND_FLOW, &sack) == UCN_OK);
    CHECK(sack.bitmap == 3U && sack.parent_generation == 0U);
    return 0;
}

static int test_transfer_roundtrip(void)
{
    static const uint8_t message[] = {
        0x10U, 0x11U, 0x12U, 0x13U, 0x14U, 0x15U, 0x16U, 0x17U
    };
    uint8_t output[sizeof(message)];
    uint8_t fragment_bytes[4];
    uint8_t aad[16];
    size_t output_bytes;
    size_t copied_bytes;
    ucn_i_transport_transfer_setup_t setup = setup_for(
        message, sizeof(message), UCN_I_TRANSPORT_PARENT_KIND_FLOW);
    ucn_i_transport_transfer_facts_t facts = flow_facts(false);
    ucn_i_transport_transfer_facts_t response_facts = flow_facts(true);
    ucn_i_transport_transfer_facts_t replay_facts = facts;
    ucn_i_transport_transfer_facts_t replay_response = response_facts;
    ucn_i_transport_transfer_facts_t wrong_facts = facts;
    ucn_i_transport_transfer_facts_t wrong_response = response_facts;
    ucn_i_transport_transfer_setup_t conflicting;
    ucn_i_transport_fragment_prefix_t prefix;
    ucn_i_transport_sack_t sack;
    ucn_i_transport_transfer_view_t view;
    ucn_i_transport_terminal_receipt_view_t receipt_view;
    ucn_handle_t tx;
    ucn_handle_t rx;
    ucn_handle_t duplicate;
    uint16_t inspected;
    uint16_t changed;
    bool exact;

    memset(aad, 0xA1, sizeof(aad));
    replay_facts.security.authenticated_replay_candidate = 1U;
    replay_response.security.authenticated_replay_candidate = 1U;
    wrong_facts.parent_fingerprint[0] ^= 1U;
    ++wrong_response.security.key_generation;
    setup.lifetime_ms = 1U;
    CHECK(ucn_i_transport_transfer_tx_begin(
              &owner, &setup, &facts, message, sizeof(message), 100U,
              &tx) == UCN_OK);
    CHECK(ucn_i_transport_transfer_tx_accept_setup(
              &owner, tx, &setup, &wrong_response, 101U) == UCN_ERR_STATE);
    CHECK(ucn_i_transport_transfer_view(&owner, tx, &view) == UCN_OK);
    CHECK(view.phase == UCN_I_TRANSPORT_TRANSFER_SETUP_PENDING);
    CHECK(ucn_i_transport_transfer_tx_accept_setup(
              &owner, tx, &setup, &response_facts, 101U) == UCN_OK);
    CHECK(ucn_i_transport_transfer_tx_accept_setup(
              &owner, tx, &setup, &replay_response, 102U) == UCN_OK);
    CHECK(ucn_i_transport_transfer_rx_setup(
              &owner, &setup, &facts, 100U, &rx, &exact) == UCN_OK);
    CHECK(!exact);
    CHECK(ucn_i_transport_transfer_view(&owner, rx, &view) == UCN_OK);
    CHECK(view.deadline_us == UINT64_C(1100));
    CHECK(ucn_i_transport_transfer_rx_setup(
              &owner, &setup, &facts, 499U, &duplicate,
              &exact) == UCN_ERR_REPLAY);
    CHECK(ucn_i_transport_transfer_rx_setup(
              &owner, &setup, &wrong_facts, 499U, &duplicate,
              &exact) == UCN_ERR_REPLAY);
    CHECK(ucn_i_transport_transfer_rx_setup(
              &owner, &setup, &replay_facts, 500U, &duplicate,
              &exact) == UCN_OK);
    CHECK(exact && memcmp(&rx, &duplicate, sizeof(rx)) == 0);
    CHECK(ucn_i_transport_transfer_view(&owner, rx, &view) == UCN_OK);
    CHECK(view.deadline_us == UINT64_C(1100));

    conflicting = setup;
    conflicting.message_digest[0] ^= 1U;
    CHECK(ucn_i_transport_transfer_rx_setup(
              &owner, &conflicting, &replay_facts, 501U, &duplicate,
              &exact) ==
          UCN_ERR_REPLAY);

    CHECK(ucn_i_transport_transfer_tx_fragment_copy(
              &owner, tx, 0U, 600U, &prefix, fragment_bytes,
              sizeof(fragment_bytes), &copied_bytes) == UCN_OK);
    CHECK(copied_bytes == 4U);
    CHECK(ucn_i_transport_transfer_rx_fragment(
              &owner, rx, &prefix, &wrong_facts, fragment_bytes,
              (uint16_t)copied_bytes, aad, 600U, &exact) == UCN_ERR_STATE);
    CHECK(ucn_i_transport_transfer_rx_fragment(
              &owner, rx, &prefix, &facts, fragment_bytes,
              (uint16_t)copied_bytes, aad, 601U, &exact) == UCN_OK);
    CHECK(!exact);
    CHECK(ucn_i_transport_transfer_rx_fragment(
              &owner, rx, &prefix, &facts, fragment_bytes,
              (uint16_t)copied_bytes, aad, 602U, &exact) == UCN_ERR_REPLAY);
    CHECK(ucn_i_transport_transfer_rx_fragment(
              &owner, rx, &prefix, &replay_facts, fragment_bytes,
              (uint16_t)copied_bytes, aad, 602U, &exact) == UCN_OK);
    CHECK(exact);
    fragment_bytes[0] ^= 1U;
    CHECK(ucn_i_transport_transfer_rx_fragment(
              &owner, rx, &prefix, &replay_facts, fragment_bytes,
              (uint16_t)copied_bytes, aad, 603U, &exact) == UCN_ERR_REPLAY);

    CHECK(ucn_i_transport_transfer_tx_fragment_copy(
              &owner, tx, 1U, 604U, &prefix, fragment_bytes,
              sizeof(fragment_bytes), &copied_bytes) == UCN_OK);
    CHECK(ucn_i_transport_transfer_rx_fragment(
              &owner, rx, &prefix, &facts, fragment_bytes,
              (uint16_t)copied_bytes, aad, 605U, &exact) == UCN_OK);
    CHECK(!exact);
    CHECK(ucn_i_transport_transfer_view(&owner, rx, &view) == UCN_OK);
    CHECK(view.phase == UCN_I_TRANSPORT_TRANSFER_COMPLETE);
    CHECK(view.completed_bytes == sizeof(message));
    CHECK(ucn_i_transport_transfer_rx_fragment(
              &owner, rx, &prefix, &replay_facts, fragment_bytes,
              (uint16_t)copied_bytes, aad, 606U, &exact) == UCN_OK);
    CHECK(exact);

    CHECK(ucn_i_transport_transfer_rx_sack(
              &owner, rx, 0U, 2U, &sack) == UCN_OK);
    CHECK(sack.bitmap == 3U);
    CHECK(ucn_i_transport_transfer_tx_accept_sack(
              &owner, tx, &sack, &response_facts, 607U) == UCN_OK);
    CHECK(ucn_i_transport_transfer_view(&owner, tx, &view) == UCN_OK);
    CHECK(view.phase == UCN_I_TRANSPORT_TRANSFER_COMPLETE);
    CHECK(ucn_i_transport_transfer_tx_accept_setup(
              &owner, tx, &setup, &response_facts, 1100U) == UCN_OK);
    CHECK(ucn_i_transport_transfer_tx_fragment_copy(
              &owner, tx, 0U, 1100U, &prefix, fragment_bytes,
              sizeof(fragment_bytes), &copied_bytes) == UCN_ERR_STATE);
    CHECK(ucn_i_transport_transfer_tx_accept_sack(
              &owner, tx, &sack, &response_facts, 1100U) == UCN_OK);
    CHECK(ucn_i_transport_transfer_view(&owner, tx, &view) == UCN_OK);
    CHECK(view.phase == UCN_I_TRANSPORT_TRANSFER_COMPLETE);
    CHECK(ucn_i_transport_maintain(
              &owner, 1100U, UINT16_MAX, &inspected, &changed) == UCN_OK);
    CHECK(ucn_i_transport_transfer_view(&owner, rx, &view) == UCN_OK);
    CHECK(view.phase == UCN_I_TRANSPORT_TRANSFER_COMPLETE);

    CHECK(ucn_i_transport_transfer_rx_mark_delivered(
              &owner, rx, 1100U) == UCN_ERR_STATE);
    CHECK(ucn_i_transport_transfer_rx_copy_complete(
              &owner, rx, output, sizeof(output), &output_bytes) == UCN_OK);
    CHECK(output_bytes == sizeof(message));
    CHECK(memcmp(output, message, sizeof(message)) == 0);
    CHECK(ucn_i_transport_transfer_rx_mark_delivered(
              &owner, rx, 1101U) == UCN_OK);
    CHECK(ucn_i_transport_transfer_rx_setup(
              &owner, &setup, &replay_facts, 1102U, &duplicate,
              &exact) == UCN_OK);
    CHECK(exact);
    CHECK(ucn_i_transport_terminal_receipt_view(
              &owner, duplicate, 1102U, &receipt_view) == UCN_OK);
    CHECK(ucn_i_transport_transfer_rx_fragment(
              &owner, rx, &prefix, &replay_facts, fragment_bytes,
              (uint16_t)copied_bytes, aad, 1102U, &exact) == UCN_OK);
    CHECK(exact);
    CHECK(ucn_i_transport_transfer_retire(&owner, rx) == UCN_OK);
    CHECK(ucn_i_transport_transfer_rx_setup(
              &owner, &setup, &replay_facts, 1103U, &duplicate,
              &exact) == UCN_OK);
    CHECK(exact);
    CHECK(ucn_i_transport_terminal_receipt_view(
              &owner, duplicate, 1103U, &receipt_view) == UCN_OK);
    CHECK(receipt_view.expires_at_us == 1601U);
    CHECK(ucn_i_transport_terminal_receipt_view(
              &owner, duplicate, 1601U, &receipt_view) == UCN_ERR_TIMEOUT);
    CHECK(ucn_i_transport_transfer_retire(&owner, tx) == UCN_OK);
    return 0;
}

static int test_receipt_expiry_retires_linked_copy(void)
{
    static const uint8_t message[] = {0x21U, 0x22U, 0x23U, 0x24U};
    uint8_t aad[UCN_I_TRANSPORT_DIGEST_BYTES];
    uint8_t fragment_bytes[sizeof(message)];
    uint8_t output[sizeof(message)];
    size_t copied_bytes;
    size_t output_bytes;
    ucn_i_transport_transfer_setup_t setup = setup_for(
        message, sizeof(message), UCN_I_TRANSPORT_PARENT_KIND_FLOW);
    ucn_i_transport_transfer_facts_t facts = flow_facts(false);
    ucn_i_transport_transfer_facts_t response_facts = flow_facts(true);
    ucn_i_transport_transfer_facts_t replay_facts = facts;
    ucn_i_transport_fragment_prefix_t prefix;
    ucn_i_transport_sack_t sack;
    ucn_i_transport_transfer_view_t view;
    ucn_i_transport_terminal_receipt_view_t receipt_view;
    ucn_handle_t tx;
    ucn_handle_t rx;
    ucn_handle_t receipt;
    ucn_handle_t next_rx;
    uint16_t inspected;
    uint16_t changed;
    bool exact;

    memset(aad, 0xB2, sizeof(aad));
    replay_facts.security.authenticated_replay_candidate = 1U;
    setup.transfer_id = 30U;
    setup.lifetime_ms = 1U;
    CHECK(ucn_i_transport_transfer_tx_begin(
              &owner, &setup, &facts, message, sizeof(message), 3000U,
              &tx) == UCN_OK);
    CHECK(ucn_i_transport_transfer_tx_accept_setup(
              &owner, tx, &setup, &response_facts, 3001U) == UCN_OK);
    CHECK(ucn_i_transport_transfer_rx_setup(
              &owner, &setup, &facts, 3000U, &rx, &exact) == UCN_OK);
    CHECK(!exact);
    CHECK(ucn_i_transport_transfer_tx_fragment_copy(
              &owner, tx, 0U, 3001U, &prefix, fragment_bytes,
              sizeof(fragment_bytes), &copied_bytes) == UCN_OK);
    CHECK(ucn_i_transport_transfer_rx_fragment(
              &owner, rx, &prefix, &facts, fragment_bytes,
              (uint16_t)copied_bytes, aad, 3001U, &exact) == UCN_OK);
    CHECK(ucn_i_transport_transfer_rx_sack(
              &owner, rx, 0U, 1U, &sack) == UCN_OK);
    CHECK(ucn_i_transport_transfer_tx_accept_sack(
              &owner, tx, &sack, &response_facts, 3001U) == UCN_OK);
    CHECK(ucn_i_transport_transfer_rx_copy_complete(
              &owner, rx, output, sizeof(output), &output_bytes) == UCN_OK);
    CHECK(ucn_i_transport_transfer_rx_mark_delivered(
              &owner, rx, 3002U) == UCN_OK);
    CHECK(ucn_i_transport_transfer_rx_setup(
              &owner, &setup, &replay_facts, 3003U, &receipt,
              &exact) == UCN_OK);
    CHECK(exact);
    CHECK(ucn_i_transport_transfer_retire(&owner, tx) == UCN_OK);

    CHECK(ucn_i_transport_transfer_rx_setup(
              &owner, &setup, &replay_facts, 3502U, &next_rx,
              &exact) == UCN_ERR_TIMEOUT);
    CHECK(ucn_i_transport_transfer_view(&owner, rx, &view) == UCN_OK);
    CHECK(view.phase == UCN_I_TRANSPORT_TRANSFER_DELIVERED);
    CHECK(ucn_i_transport_maintain(
              &owner, 3502U, UINT16_MAX, &inspected, &changed) == UCN_OK);
    CHECK(changed != 0U);
    CHECK(ucn_i_transport_transfer_view(&owner, rx, &view) ==
          UCN_ERR_NOT_FOUND);
    CHECK(ucn_i_transport_terminal_receipt_view(
              &owner, receipt, 3502U, &receipt_view) == UCN_ERR_NOT_FOUND);

    CHECK(ucn_i_transport_transfer_rx_setup(
              &owner, &setup, &replay_facts, 3503U, &next_rx,
              &exact) == UCN_ERR_REPLAY);

    setup.transfer_id = 31U;
    CHECK(ucn_i_transport_transfer_rx_setup(
              &owner, &setup, &facts, 3503U, &next_rx, &exact) == UCN_OK);
    CHECK(!exact);
    CHECK(ucn_i_transport_transfer_abort(&owner, next_rx) == UCN_OK);
    CHECK(ucn_i_transport_transfer_rx_setup(
              &owner, &setup, &replay_facts, 3504U, &receipt,
              &exact) == UCN_ERR_STATE);
    CHECK(ucn_i_transport_transfer_retire(&owner, next_rx) == UCN_OK);
    return 0;
}

static int test_setup_before_fragment_and_expiry(void)
{
    static const uint8_t message[] = {1U, 2U, 3U, 4U};
    ucn_i_transport_transfer_setup_t setup = setup_for(
        message, sizeof(message), UCN_I_TRANSPORT_PARENT_KIND_FLOW);
    ucn_i_transport_transfer_facts_t facts = flow_facts(false);
    ucn_i_transport_transfer_view_t view;
    ucn_handle_t tx;
    uint16_t inspected;
    uint16_t changed;

    setup.transfer_id = 20U;
    setup.lifetime_ms = 1U;
    CHECK(ucn_i_transport_transfer_tx_begin(
              &owner, &setup, &facts, message, sizeof(message), 1000U,
              &tx) == UCN_OK);
    CHECK(ucn_i_transport_maintain(
              &owner, 2000U, UINT16_MAX, &inspected, &changed) == UCN_OK);
    CHECK(changed != 0U);
    CHECK(ucn_i_transport_transfer_view(&owner, tx, &view) == UCN_OK);
    CHECK(view.phase == UCN_I_TRANSPORT_TRANSFER_FAILED);
    CHECK(ucn_i_transport_transfer_retire(&owner, tx) == UCN_OK);
    return 0;
}

static int test_o0_requires_exact_link_policy(void)
{
    static const uint8_t message[] = {5U, 6U, 7U, 8U};
    ucn_i_transport_transfer_setup_t setup = setup_for(
        message, sizeof(message), UCN_I_TRANSPORT_PARENT_KIND_FLOW);
    ucn_i_transport_transfer_facts_t facts = flow_o0_facts();
    ucn_i_transport_transfer_facts_t invalid = facts;
    ucn_i_transport_transfer_facts_t changed = facts;
    ucn_handle_t handle;
    ucn_handle_t sentinel;
    bool exact = true;

    setup.transfer_id = 40U;
    invalid.security.link_generation = 0U;
    memset(&handle, 0xA5, sizeof(handle));
    sentinel = handle;
    CHECK(ucn_i_transport_transfer_rx_setup(
              &owner, &setup, &invalid, 4000U, &handle,
              &exact) == UCN_ERR_ARGUMENT);
    CHECK(memcmp(&handle, &sentinel, sizeof(handle)) == 0);
    CHECK(exact);

    CHECK(ucn_i_transport_transfer_rx_setup(
              &owner, &setup, &facts, 4000U, &handle, &exact) == UCN_OK);
    CHECK(!exact);
    ++changed.security.policy_generation;
    CHECK(ucn_i_transport_transfer_rx_setup(
              &owner, &setup, &changed, 4001U, &sentinel,
              &exact) == UCN_ERR_REPLAY);
    CHECK(ucn_i_transport_transfer_abort(&owner, handle) == UCN_OK);
    CHECK(ucn_i_transport_transfer_retire(&owner, handle) == UCN_OK);
    return 0;
}

int main(void)
{
    CHECK(start_owner() == 0);
    CHECK(test_flow_prefixes() == 0);
    CHECK(test_transfer_roundtrip() == 0);
    CHECK(test_setup_before_fragment_and_expiry() == 0);
    CHECK(test_receipt_expiry_retires_linked_copy() == 0);
    CHECK(test_o0_requires_exact_link_policy() == 0);
    CHECK(ucn_i_transport_owner_destroy(&owner) == UCN_OK);
    puts("transfer tests passed");
    return 0;
}
