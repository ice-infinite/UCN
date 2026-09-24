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
    test_lock_t *lock = context;
    lock->held = 0U;
}

static ucn_i_lock_ops_t lock_ops(void)
{
    ucn_i_lock_ops_t ops;
    memset(&ops, 0, sizeof(ops));
    ops.struct_size = sizeof(ops);
    ops.api_version = UCN_I_LOCK_OPS_VERSION;
    ops.context = &test_lock;
    ops.enter = lock_enter;
    ops.leave = lock_leave;
    return ops;
}

static void fill_principal(uint8_t principal[16], uint8_t seed)
{
    size_t index;
    for (index = 0U; index < 16U; ++index) {
        principal[index] = (uint8_t)(seed + index);
    }
}

static ucn_i_transport_reliable_key_t key(uint32_t sequence)
{
    ucn_i_transport_reliable_key_t value;
    memset(&value, 0, sizeof(value));
    value.source.address = 1U;
    value.source.generation = 2U;
    fill_principal(value.source.principal, 0x10U);
    value.destination.address = 3U;
    value.destination.generation = 4U;
    fill_principal(value.destination.principal, 0x30U);
    value.realm = 5U;
    value.origin_sequence = sequence;
    value.service_id = UINT16_C(0x1234);
    value.delivery = UCN_DELIVERY_RELIABLE;
    return value;
}

static ucn_i_transport_security_facts_t o0(void)
{
    ucn_i_transport_security_facts_t value;
    memset(&value, 0, sizeof(value));
    value.link_generation = 6U;
    value.policy_generation = 7U;
    value.link_id = 8U;
    value.endpoint_public_unauthenticated = 1U;
    value.trusted_link_policy = 1U;
    return value;
}

static ucn_i_transport_security_facts_t o1(void)
{
    ucn_i_transport_security_facts_t value;
    memset(&value, 0, sizeof(value));
    value.origin_security = 1U;
    value.session_generation = 6U;
    value.key_generation = 7U;
    return value;
}

static ucn_i_transport_ack_facts_t ack_facts_for(
    const ucn_i_transport_reliable_key_t *tx_key,
    const ucn_i_transport_security_facts_t *security)
{
    ucn_i_transport_ack_facts_t facts;

    memset(&facts, 0, sizeof(facts));
    facts.source = tx_key->destination;
    facts.destination = tx_key->source;
    facts.security = *security;
    facts.security.authenticated_replay_candidate = 0U;
    facts.realm = tx_key->realm;
    return facts;
}

static int start_owner(void)
{
    ucn_i_transport_config_t config;
    memset(&owner, 0, sizeof(owner));
    memset(&test_lock, 0, sizeof(test_lock));
    memset(&config, 0, sizeof(config));
    config.reliable_lifetime_us = 1000U;
    config.reliable_retry_us = 100U;
    config.receipt_lifetime_us = 500U;
    config.runtime_instance = 11U;
    config.owner_instance = 12U;
    config.reliable_max_attempts = 2U;
    config.state_lock = lock_ops();
    return ucn_i_transport_owner_init(&owner, &config) == UCN_OK ? 0 : 1;
}

static int test_ack_codec(void)
{
    static const uint8_t golden[UCN_I_TRANSPORT_DELIVERY_ACK_BYTES] = {
        0x01U, 0x12U, 0x34U, 0x01U, 0x02U, 0x03U, 0x04U, 0x00U, 0x07U
    };
    ucn_i_transport_delivery_ack_t ack;
    uint8_t bytes[sizeof(golden)];
    uint8_t sentinel[sizeof(golden)];

    memset(&ack, 0, sizeof(ack));
    ack.service_id = UINT16_C(0x1234);
    ack.origin_sequence = UINT32_C(0x01020304);
    ack.receive_credit = 7U;
    CHECK(ucn_i_transport_delivery_ack_encode(&ack, bytes) == UCN_OK);
    CHECK(memcmp(bytes, golden, sizeof(golden)) == 0);
    memset(&ack, 0, sizeof(ack));
    CHECK(ucn_i_transport_delivery_ack_decode(golden, &ack) == UCN_OK);
    CHECK(ack.service_id == UINT16_C(0x1234));
    CHECK(ack.origin_sequence == UINT32_C(0x01020304));
    CHECK(ack.receive_credit == 7U);
    memset(sentinel, 0xA5, sizeof(sentinel));
    memcpy(bytes, sentinel, sizeof(bytes));
    ack.service_id = 0U;
    CHECK(ucn_i_transport_delivery_ack_encode(&ack, bytes) ==
          UCN_ERR_ARGUMENT);
    CHECK(memcmp(bytes, sentinel, sizeof(bytes)) == 0);
    return 0;
}

static int test_setup_and_fragment_codecs(void)
{
    static const uint8_t setup_golden[
        UCN_I_TRANSPORT_TRANSFER_SETUP_ONE_WAY_BYTES] = {
        0x01U, 0x00U, 0x00U, 0x01U, 0x02U, 0x03U, 0x04U,
        0x05U, 0x06U, 0x07U, 0x08U, 0x12U, 0x34U,
        0x00U, 0x00U, 0x00U, 0x08U,
        0x00U, 0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U,
        0x08U, 0x09U, 0x0AU, 0x0BU, 0x0CU, 0x0DU, 0x0EU, 0x0FU,
        0x02U, 0x00U, 0x00U, 0x02U, 0x00U, 0x04U,
        0x00U, 0x00U, 0x03U, 0xE8U
    };
    static const uint8_t fragment_golden[
        UCN_I_TRANSPORT_C1_FRAGMENT_PREFIX_BYTES] = {
        0x05U, 0x06U, 0x07U, 0x08U, 0x01U, 0x02U, 0x03U, 0x04U,
        0x00U, 0x01U, 0x00U, 0x02U
    };
    static const uint8_t sack_golden[UCN_I_TRANSPORT_C1_SACK_BYTES] = {
        0x05U, 0x06U, 0x07U, 0x08U, 0x01U, 0x02U, 0x03U, 0x04U,
        0x80U, 0x00U, 0x12U, 0x34U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x03U, 0x00U, 0x04U
    };
    ucn_i_transport_transfer_setup_t setup;
    ucn_i_transport_fragment_prefix_t fragment;
    ucn_i_transport_sack_t sack;
    uint8_t bytes[UCN_I_TRANSPORT_TRANSFER_SETUP_RESULT_BYTES];
    size_t output_bytes = 0U;
    size_t index;

    memset(&setup, 0, sizeof(setup));
    setup.parent_kind = UCN_I_TRANSPORT_PARENT_KIND_C1;
    setup.parent_generation = UINT32_C(0x01020304);
    setup.transfer_id = UINT32_C(0x05060708);
    setup.service_id = UINT16_C(0x1234);
    setup.total_length = 8U;
    for (index = 0U; index < 16U; ++index) {
        setup.message_digest[index] = (uint8_t)index;
    }
    setup.delivery = UCN_DELIVERY_RELIABLE;
    setup.interaction = UCN_INTERACTION_ONE_WAY;
    setup.fragment_count = 2U;
    setup.fragment_budget = 4U;
    setup.lifetime_ms = 1000U;
    CHECK(ucn_i_transport_transfer_setup_encode(
              &setup, bytes, sizeof(bytes), &output_bytes) == UCN_OK);
    CHECK(output_bytes == sizeof(setup_golden));
    CHECK(memcmp(bytes, setup_golden, sizeof(setup_golden)) == 0);
    memset(&setup, 0, sizeof(setup));
    CHECK(ucn_i_transport_transfer_setup_decode(
              setup_golden, sizeof(setup_golden), &setup) == UCN_OK);
    CHECK(setup.transfer_id == UINT32_C(0x05060708));

    memset(&fragment, 0, sizeof(fragment));
    fragment.parent_kind = UCN_I_TRANSPORT_PARENT_KIND_C1;
    fragment.parent_generation = UINT32_C(0x01020304);
    fragment.transfer_id = UINT32_C(0x05060708);
    fragment.fragment_index = 1U;
    fragment.fragment_count = 2U;
    CHECK(ucn_i_transport_fragment_prefix_encode(
              &fragment, bytes, sizeof(bytes), &output_bytes) == UCN_OK);
    CHECK(output_bytes == sizeof(fragment_golden));
    CHECK(memcmp(bytes, fragment_golden, sizeof(fragment_golden)) == 0);
    memset(&fragment, 0, sizeof(fragment));
    CHECK(ucn_i_transport_fragment_prefix_decode(
              fragment_golden, sizeof(fragment_golden),
              UCN_I_TRANSPORT_PARENT_KIND_C1, &fragment) == UCN_OK);
    CHECK(fragment.fragment_index == 1U);

    memset(&sack, 0, sizeof(sack));
    sack.parent_kind = UCN_I_TRANSPORT_PARENT_KIND_C1;
    sack.parent_generation = UINT32_C(0x01020304);
    sack.transfer_id = UINT32_C(0x05060708);
    sack.original_service_id = UINT16_C(0x1234);
    sack.bitmap = 3U;
    sack.receive_credit = 4U;
    CHECK(ucn_i_transport_sack_encode(
              &sack, bytes, sizeof(bytes), &output_bytes) == UCN_OK);
    CHECK(output_bytes == sizeof(sack_golden));
    CHECK(memcmp(bytes, sack_golden, sizeof(sack_golden)) == 0);
    memset(&sack, 0, sizeof(sack));
    CHECK(ucn_i_transport_sack_decode(
              sack_golden, sizeof(sack_golden),
              UCN_I_TRANSPORT_PARENT_KIND_C1, &sack) == UCN_OK);
    CHECK(sack.bitmap == 3U);

    return 0;
}

static int test_operation_setup_lengths_and_failure_atomicity(void)
{
    ucn_i_transport_transfer_setup_t setup;
    ucn_i_transport_transfer_setup_t decoded;
    uint8_t bytes[UCN_I_TRANSPORT_TRANSFER_SETUP_RESULT_BYTES];
    uint8_t sentinel[sizeof(bytes)];
    size_t output_bytes;

    memset(&setup, 0, sizeof(setup));
    setup.parent_kind = UCN_I_TRANSPORT_PARENT_KIND_C1;
    setup.parent_generation = 1U;
    setup.transfer_id = 2U;
    setup.service_id = 3U;
    setup.total_length = 4U;
    memset(setup.message_digest, 0x5AU, sizeof(setup.message_digest));
    setup.delivery = UCN_DELIVERY_RELIABLE;
    setup.interaction = UCN_INTERACTION_REQUEST;
    setup.fragment_count = 1U;
    setup.fragment_budget = 4U;
    setup.lifetime_ms = 100U;
    setup.operation_id = UINT64_C(0x0102030405060708);
    setup.operation_flags = 1U;
    output_bytes = UINT32_MAX;
    CHECK(ucn_i_transport_transfer_setup_encode(
              &setup, bytes, sizeof(bytes), &output_bytes) == UCN_OK);
    CHECK(output_bytes == UCN_I_TRANSPORT_TRANSFER_SETUP_OPERATION_BYTES);
    memset(&decoded, 0, sizeof(decoded));
    CHECK(ucn_i_transport_transfer_setup_decode(
              bytes, output_bytes, &decoded) == UCN_OK);
    CHECK(decoded.interaction == UCN_INTERACTION_REQUEST &&
          decoded.operation_id == setup.operation_id &&
          decoded.operation_flags == setup.operation_flags);

    setup.interaction = UCN_INTERACTION_RESULT;
    setup.result_code = UINT16_C(0x1234);
    CHECK(ucn_i_transport_transfer_setup_encode(
              &setup, bytes, sizeof(bytes), &output_bytes) == UCN_OK);
    CHECK(output_bytes == UCN_I_TRANSPORT_TRANSFER_SETUP_RESULT_BYTES);
    memset(&decoded, 0, sizeof(decoded));
    CHECK(ucn_i_transport_transfer_setup_decode(
              bytes, output_bytes, &decoded) == UCN_OK);
    CHECK(decoded.interaction == UCN_INTERACTION_RESULT &&
          decoded.result_code == UINT16_C(0x1234));

    memset(bytes, 0xA5, sizeof(bytes));
    memcpy(sentinel, bytes, sizeof(bytes));
    output_bytes = SIZE_MAX;
    setup.fragment_count = 2U;
    CHECK(ucn_i_transport_transfer_setup_encode(
              &setup, bytes, sizeof(bytes), &output_bytes) ==
          UCN_ERR_ARGUMENT);
    CHECK(memcmp(bytes, sentinel, sizeof(bytes)) == 0);
    CHECK(output_bytes == SIZE_MAX);
    memset(&decoded, 0xA5, sizeof(decoded));
    CHECK(ucn_i_transport_transfer_setup_decode(
              sentinel,
              UCN_I_TRANSPORT_TRANSFER_SETUP_RESULT_BYTES - 1U,
              &decoded) == UCN_ERR_MALFORMED);
    {
        ucn_i_transport_transfer_setup_t unchanged;
        memset(&unchanged, 0xA5, sizeof(unchanged));
        CHECK(memcmp(&decoded, &unchanged, sizeof(decoded)) == 0);
    }
    return 0;
}

static int test_reliable_retry_and_ack(void)
{
    static const uint8_t sealed[] = {0x61U, 0x40U, 0x01U, 0x02U, 0x03U};
    uint8_t aad[16];
    uint8_t payload[16];
    uint8_t copied[sizeof(sealed)];
    size_t copied_bytes = 0U;
    ucn_i_transport_reliable_key_t tx_key = key(1U);
    ucn_i_transport_security_facts_t security = o0();
    ucn_i_transport_delivery_ack_t ack;
    ucn_i_transport_ack_facts_t facts;
    ucn_i_transport_reliable_view_t view;
    ucn_handle_t handle;
    uint16_t inspected;
    uint16_t changed;

    memset(aad, 0x11, sizeof(aad));
    memset(payload, 0x22, sizeof(payload));
    CHECK(ucn_i_transport_reliable_begin(
              &owner, &tx_key, &security, aad, payload, sealed,
              sizeof(sealed), 10U, &handle) == UCN_OK);
    CHECK(ucn_i_transport_reliable_copy_attempt(
              &owner, handle, 10U, copied, sizeof(copied),
              &copied_bytes) == UCN_OK);
    CHECK(copied_bytes == sizeof(sealed));
    CHECK(memcmp(copied, sealed, sizeof(sealed)) == 0);

    CHECK(ucn_i_transport_reliable_note_submit(
              &owner, handle, false, 10U) == UCN_OK);
    CHECK(ucn_i_transport_reliable_view(&owner, handle, &view) == UCN_OK);
    CHECK(view.attempts == 0U);
    CHECK(view.phase == UCN_I_TRANSPORT_RELIABLE_READY);
    memset(&ack, 0, sizeof(ack));
    ack.service_id = tx_key.service_id;
    ack.origin_sequence = tx_key.origin_sequence;
    facts = ack_facts_for(&tx_key, &security);
    CHECK(ucn_i_transport_reliable_accept_ack(
              &owner, handle, &ack, &facts, 11U) == UCN_ERR_STATE);
    CHECK(ucn_i_transport_reliable_copy_attempt(
              &owner, handle, 109U, copied, sizeof(copied),
              &copied_bytes) == UCN_ERR_STATE);
    CHECK(ucn_i_transport_reliable_copy_attempt(
              &owner, handle, 110U, copied, sizeof(copied),
              &copied_bytes) == UCN_OK);
    CHECK(memcmp(copied, sealed, sizeof(sealed)) == 0);
    CHECK(ucn_i_transport_reliable_note_submit(
              &owner, handle, true, 110U) == UCN_OK);
    CHECK(ucn_i_transport_maintain(
              &owner, 210U, 1U, &inspected, &changed) == UCN_OK);
    CHECK(inspected == 1U && changed == 1U);
    CHECK(ucn_i_transport_reliable_view(&owner, handle, &view) == UCN_OK);
    CHECK(view.phase == UCN_I_TRANSPORT_RELIABLE_READY);
    CHECK(view.attempts == 1U);

    memset(&ack, 0, sizeof(ack));
    ack.service_id = tx_key.service_id;
    ack.origin_sequence = tx_key.origin_sequence;
    ack.receive_credit = 2U;
    facts = ack_facts_for(&tx_key, &security);
    facts.source.address++;
    CHECK(ucn_i_transport_reliable_accept_ack(
              &owner, handle, &ack, &facts, 211U) == UCN_ERR_STATE);
    facts = ack_facts_for(&tx_key, &security);
    CHECK(ucn_i_transport_reliable_accept_ack(
              &owner, handle, &ack, &facts, 211U) == UCN_OK);
    CHECK(ucn_i_transport_reliable_view(&owner, handle, &view) == UCN_OK);
    CHECK(view.phase == UCN_I_TRANSPORT_RELIABLE_DONE);
    CHECK(view.terminal_result == UCN_OK);
    CHECK(ucn_i_transport_reliable_retire(&owner, handle) == UCN_OK);
    return 0;
}

static int test_reliable_receive_replay(void)
{
    uint8_t aad[16];
    uint8_t payload[16];
    uint8_t conflicting[16];
    ucn_i_transport_reliable_key_t rx_key = key(20U);
    ucn_i_transport_security_facts_t security = o1();
    ucn_i_transport_receive_action_t action;

    memset(aad, 0x31, sizeof(aad));
    memset(payload, 0x32, sizeof(payload));
    memset(conflicting, 0x33, sizeof(conflicting));
    CHECK(ucn_i_transport_reliable_receive(
              &owner, &rx_key, &security, aad, payload, 0U, 3U, 100U,
              &action) == UCN_OK);
    CHECK(action.deliver_to_application == 1U);
    CHECK(action.exact_duplicate == 0U);

    security.authenticated_replay_candidate = 1U;
    CHECK(ucn_i_transport_reliable_receive(
              &owner, &rx_key, &security, aad, payload, 0U, 3U, 101U,
              &action) == UCN_OK);
    CHECK(action.deliver_to_application == 0U);
    CHECK(action.exact_duplicate == 1U);
    CHECK(action.ack.receive_credit == 3U);
    CHECK(ucn_i_transport_reliable_receive(
              &owner, &rx_key, &security, aad, conflicting, 0U, 3U, 102U,
              &action) == UCN_ERR_REPLAY);
    security.authenticated_replay_candidate = 0U;
    CHECK(ucn_i_transport_reliable_receive(
              &owner, &rx_key, &security, aad, payload, 0U, 3U, 103U,
              &action) == UCN_ERR_REPLAY);
    return 0;
}

static int test_o0_requires_exact_current_link_policy(void)
{
    static const uint8_t sealed[] = {0x31U, 0x32U};
    ucn_i_transport_reliable_key_t tx_key = key(25U);
    ucn_i_transport_security_facts_t security = o0();
    uint8_t aad[16];
    uint8_t payload[16];
    ucn_handle_t handle;

    memset(aad, 0x41, sizeof(aad));
    memset(payload, 0x42, sizeof(payload));
    security.link_generation = 0U;
    CHECK(ucn_i_transport_reliable_begin(
              &owner, &tx_key, &security, aad, payload, sealed,
              sizeof(sealed), 500U, &handle) == UCN_ERR_ARGUMENT);
    security = o0();
    security.policy_generation = 0U;
    CHECK(ucn_i_transport_reliable_begin(
              &owner, &tx_key, &security, aad, payload, sealed,
              sizeof(sealed), 500U, &handle) == UCN_ERR_ARGUMENT);
    security = o0();
    security.authenticated_replay_candidate = 1U;
    CHECK(ucn_i_transport_reliable_begin(
              &owner, &tx_key, &security, aad, payload, sealed,
              sizeof(sealed), 500U, &handle) == UCN_ERR_ARGUMENT);

    security = o0();
    CHECK(ucn_i_transport_reliable_begin(
              &owner, &tx_key, &security, aad, payload, sealed,
              sizeof(sealed), 500U, &handle) == UCN_OK);
    CHECK(ucn_i_transport_reliable_cancel(&owner, handle) == UCN_OK);
    CHECK(ucn_i_transport_reliable_retire(&owner, handle) == UCN_OK);
    return 0;
}

static int test_retry_exhaustion(void)
{
    static const uint8_t sealed[] = {1U, 2U, 3U};
    uint8_t aad[16];
    uint8_t payload[16];
    uint8_t copied[sizeof(sealed)];
    size_t copied_bytes;
    ucn_i_transport_reliable_key_t tx_key = key(30U);
    ucn_i_transport_security_facts_t security = o0();
    ucn_i_transport_reliable_view_t view;
    ucn_handle_t handle;
    uint16_t inspected;
    uint16_t changed;
    unsigned pass;

    memset(aad, 1, sizeof(aad));
    memset(payload, 2, sizeof(payload));
    CHECK(ucn_i_transport_reliable_begin(
              &owner, &tx_key, &security, aad, payload, sealed,
              sizeof(sealed), 1000U, &handle) == UCN_OK);
    for (pass = 0U; pass < 2U; ++pass) {
        uint64_t now = UINT64_C(1000) + (uint64_t)pass * 100U;
        CHECK(ucn_i_transport_reliable_copy_attempt(
                  &owner, handle, now, copied, sizeof(copied),
                  &copied_bytes) == UCN_OK);
        CHECK(ucn_i_transport_reliable_note_submit(
                  &owner, handle, true, now) == UCN_OK);
        CHECK(ucn_i_transport_maintain(
                  &owner, now + 100U, UINT16_MAX,
                  &inspected, &changed) == UCN_OK);
    }
    CHECK(ucn_i_transport_reliable_view(&owner, handle, &view) == UCN_OK);
    CHECK(view.phase == UCN_I_TRANSPORT_RELIABLE_FAILED);
    CHECK(view.terminal_result == UCN_ERR_EXHAUSTED);
    CHECK(ucn_i_transport_reliable_retire(&owner, handle) == UCN_OK);
    return 0;
}

int main(void)
{
    uint16_t inspected;
    uint16_t changed;

    CHECK(test_ack_codec() == 0);
    CHECK(test_setup_and_fragment_codecs() == 0);
    CHECK(test_operation_setup_lengths_and_failure_atomicity() == 0);
    CHECK(start_owner() == 0);
    CHECK(test_reliable_retry_and_ack() == 0);
    CHECK(test_reliable_receive_replay() == 0);
    CHECK(test_o0_requires_exact_current_link_policy() == 0);
    CHECK(test_retry_exhaustion() == 0);
    CHECK(ucn_i_transport_maintain(
              &owner, 10000U, UINT16_MAX, &inspected, &changed) == UCN_OK);
    CHECK(ucn_i_transport_owner_destroy(&owner) == UCN_OK);
    puts("transport reliable tests passed");
    return 0;
}
