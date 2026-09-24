#include "internal/ucn_admission.h"

#include "rust/tests/conformance/v6s_admission_capability_v1.h"

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

typedef struct test_provider {
    ucn_i_admission_owner_t *owner;
    ucn_i_admission_handle_t handle;
    ucn_i_admission_hello_t *external_hello;
    ucn_i_admission_transcript_t *external_transcript;
    ucn_i_admission_binding_view_t *external_binding;
    uint32_t cookie_calls;
    uint32_t verify_calls;
    uint32_t authorize_calls;
    uint32_t reentry_rejected;
} test_provider_t;

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

static ucn_i_lock_ops_t lock_ops(test_lock_t *lock)
{
    ucn_i_lock_ops_t ops;
    memset(&ops, 0, sizeof(ops));
    ops.struct_size = sizeof(ops);
    ops.api_version = UCN_I_LOCK_OPS_VERSION;
    ops.context = lock;
    ops.enter = lock_enter;
    ops.leave = lock_leave;
    return ops;
}

static void fill(uint8_t *bytes, size_t length, uint8_t value)
{
    memset(bytes, value, length);
}

static ucn_result_t issue_cookie(
    void *context,
    const ucn_i_admission_hello_t *hello,
    ucn_i_admission_link_t link,
    uint32_t bucket,
    uint8_t output[UCN_I_ADMISSION_COOKIE_BYTES],
    uint8_t *output_bytes)
{
    test_provider_t *provider = context;
    ucn_i_admission_pending_view_t view;

    provider->cookie_calls++;
    if (provider->external_hello != NULL) {
        provider->external_hello->transaction_id = UINT64_C(0xDEADBEEF);
    }
    if (provider->owner != NULL &&
        ucn_i_admission_pending_get(provider->owner, provider->handle,
                                    &view) == UCN_ERR_STATE) {
        provider->reentry_rejected++;
    }
    if (hello == NULL || link.id == 0U || bucket == 0U || output == NULL ||
        output_bytes == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    memset(output, 0, UCN_I_ADMISSION_COOKIE_BYTES);
    output[0] = 0x01U;
    output[1] = 0x02U;
    output[2] = 0x03U;
    output[3] = 0x04U;
    *output_bytes = 4U;
    return UCN_OK;
}

static ucn_result_t verify_cookie(
    void *context,
    const ucn_i_admission_hello_cookie_t *hello_cookie)
{
    test_provider_t *provider = context;
    ucn_i_admission_pending_view_t view;

    provider->verify_calls++;
    if (provider->external_transcript != NULL) {
        provider->external_transcript->protocol_version = 0U;
    }
    if (provider->owner != NULL &&
        ucn_i_admission_pending_get(provider->owner, provider->handle,
                                    &view) == UCN_ERR_STATE) {
        provider->reentry_rejected++;
    }
    return hello_cookie != NULL && hello_cookie->cookie_bytes == 4U &&
                   hello_cookie->cookie[0] == 0xC1U ?
               UCN_OK : UCN_ERR_SECURITY;
}

static ucn_result_t authorize_event(
    void *context,
    ucn_i_admission_event_t event,
    const ucn_i_admission_key_t *key,
    const ucn_i_admission_transcript_t *transcript,
    uint64_t now_us,
    const ucn_i_admission_evidence_t *evidence)
{
    test_provider_t *provider = context;
    ucn_i_admission_pending_view_t view;

    provider->authorize_calls++;
    if (event == UCN_I_ADMISSION_EVENT_FINAL_DURABLE &&
        provider->external_binding != NULL) {
        provider->external_binding->local_deadline_us = now_us + 1U;
    }
    if (provider->owner != NULL &&
        ucn_i_admission_pending_get(provider->owner, provider->handle,
                                    &view) == UCN_ERR_STATE) {
        provider->reentry_rejected++;
    }
    return event != 0U && key != NULL && transcript != NULL && now_us != 0U &&
                   evidence != NULL && evidence->length == 1U &&
                   evidence->bytes[0] == event ?
               UCN_OK : UCN_ERR_SECURITY;
}

static ucn_i_admission_hello_t make_hello(void)
{
    ucn_i_admission_hello_t hello;
    memset(&hello, 0, sizeof(hello));
    hello.device_nonce = UINT64_C(0x0102030405060708);
    hello.transaction_id = UINT64_C(0x1112131415161718);
    fill(hello.identity_digest, sizeof(hello.identity_digest), 0x11U);
    return hello;
}

static ucn_i_admission_hello_cookie_t make_hello_cookie(void)
{
    ucn_i_admission_hello_cookie_t value;
    memset(&value, 0, sizeof(value));
    value.key.link.id = 7U;
    value.key.link.generation = 9U;
    value.key.local_peer_discriminator = 77U;
    value.key.transaction_id = UINT64_C(0x1112131415161718);
    fill(value.key.identity_digest, sizeof(value.key.identity_digest), 0x11U);
    value.device_nonce = UINT64_C(0x0102030405060708);
    value.lease_freshness_challenge_nonce =
        UINT64_C(0x2122232425262728);
    fill(value.prior_messages_hash, sizeof(value.prior_messages_hash), 0x31U);
    value.cookie_bytes = 4U;
    value.cookie[0] = 0xC1U;
    value.cookie[1] = 0xC2U;
    value.cookie[2] = 0xC3U;
    value.cookie[3] = 0xC4U;
    return value;
}

static ucn_i_admission_transcript_t make_cookie_transcript(void)
{
    ucn_i_admission_transcript_t value;
    memset(&value, 0, sizeof(value));
    value.protocol_version = 6U;
    value.bootstrap_header_contract = 1U;
    fill(value.joining_device_identity_digest,
         sizeof(value.joining_device_identity_digest), 0x11U);
    value.device_nonce = UINT64_C(0x0102030405060708);
    value.transaction_id = UINT64_C(0x1112131415161718);
    value.lease_freshness_challenge_nonce =
        UINT64_C(0x2122232425262728);
    value.selected_link_id = 7U;
    value.selected_link_generation = 9U;
    fill(value.prior_messages_hash, sizeof(value.prior_messages_hash), 0x31U);
    return value;
}

static void add_authority(ucn_i_admission_transcript_t *value)
{
    fill(value->authority_principal, sizeof(value->authority_principal), 0x22U);
    value->authority_generation = 3U;
    value->authority_nonce = UINT64_C(0x3132333435363738);
    value->realm_id = 42U;
    value->authority_address = 2U;
    value->authority_binding_generation = 5U;
    value->authority_lease_sequence = 6U;
    value->authority_lease_duration_us = 10000U;
    value->freshness_max_remaining_lease_us = 9000U;
    fill(value->durable_fence_token, sizeof(value->durable_fence_token), 1U);
    fill(value->allocation_high_water_digest,
         sizeof(value->allocation_high_water_digest), 2U);
    fill(value->quorum_config_digest, sizeof(value->quorum_config_digest), 3U);
    fill(value->signer_set_digest, sizeof(value->signer_set_digest), 4U);
    fill(value->threshold_proof_digest,
         sizeof(value->threshold_proof_digest), 5U);
    fill(value->freshness_proof_transcript_hash,
         sizeof(value->freshness_proof_transcript_hash), 6U);
    value->authority_signer_count = 3U;
    value->authority_quorum_threshold = 2U;
}

static void add_device(ucn_i_admission_transcript_t *value)
{
    fill(value->joining_device_principal,
         sizeof(value->joining_device_principal), 0x44U);
    value->selected_hop_suite = 16U;
    value->selected_hop_key_id = 1U;
    value->selected_hop_key_generation = 2U;
    value->selected_e2e_mode = 1U;
    value->selected_e2e_suite = 1U;
    value->selected_e2e_key_id = 3U;
    value->selected_e2e_key_generation = 4U;
    value->selected_session_generation = 5U;
}

static void add_address(ucn_i_admission_transcript_t *value)
{
    value->proposed_address = 9U;
    value->address_binding_generation = 11U;
    fill(value->binding_lease_id, sizeof(value->binding_lease_id), 0x55U);
    value->binding_lease_duration_us = 2000U;
    value->binding_mode = 1U;
}

static ucn_i_admission_evidence_t evidence_for(uint8_t event)
{
    ucn_i_admission_evidence_t evidence;
    memset(&evidence, 0, sizeof(evidence));
    evidence.length = 1U;
    evidence.bytes[0] = event;
    return evidence;
}

int main(void)
{
    static const uint8_t hello_golden[40] = {
        UCN_V6S_ADMISSION_HELLO_JOIN_BYTES};
    static const uint8_t challenge_golden[40] = {
        UCN_V6S_ADMISSION_COOKIE_CHALLENGE_BYTES};
    static const uint8_t hello_cookie_golden[86] = {
        UCN_V6S_ADMISSION_HELLO_COOKIE_BYTES};
    ucn_i_admission_owner_t owner;
    ucn_i_callback_gate_t gate;
    ucn_i_admission_config_t config;
    ucn_i_admission_hello_t hello = make_hello();
    ucn_i_admission_hello_t decoded_hello;
    ucn_i_admission_cookie_challenge_t challenge;
    ucn_i_admission_cookie_challenge_t decoded_challenge;
    ucn_i_admission_hello_cookie_t hello_cookie = make_hello_cookie();
    ucn_i_admission_hello_cookie_t decoded_cookie;
    ucn_i_admission_transcript_t transcript = make_cookie_transcript();
    ucn_i_admission_handle_t handle;
    ucn_i_admission_pending_view_t pending;
    ucn_i_admission_binding_view_t binding;
    ucn_i_admitted_view_t admitted;
    ucn_i_admission_session_requirement_t session_requirement;
    ucn_i_admission_evidence_t evidence;
    test_lock_t state_lock = {0};
    test_lock_t gate_lock = {0};
    test_provider_t provider;
    ucn_i_lock_ops_t state_ops = lock_ops(&state_lock);
    ucn_i_lock_ops_t gate_ops = lock_ops(&gate_lock);
    uint8_t bytes[UCN_I_ADMISSION_HELLO_COOKIE_MAX_BYTES];
    uint8_t before[sizeof(bytes)];
    size_t encoded_bytes = 0U;
    uint16_t inspected;
    uint16_t expired;

    memset(&owner, 0, sizeof(owner));
    memset(&gate, 0, sizeof(gate));
    memset(&provider, 0, sizeof(provider));
    memset(&config, 0, sizeof(config));
    memset(&decoded_hello, 0, sizeof(decoded_hello));
    memset(&decoded_challenge, 0, sizeof(decoded_challenge));
    memset(&decoded_cookie, 0, sizeof(decoded_cookie));
    memset(&handle, 0, sizeof(handle));
    memset(&binding, 0, sizeof(binding));
    memset(&admitted, 0, sizeof(admitted));
    memset(&session_requirement, 0, sizeof(session_requirement));

    CHECK(ucn_i_admission_hello_encode(&hello, bytes) == UCN_OK);
    CHECK(memcmp(bytes, hello_golden, sizeof(hello_golden)) == 0);
    CHECK(ucn_i_admission_hello_decode(hello_golden, &decoded_hello) == UCN_OK);
    CHECK(memcmp(&hello, &decoded_hello, sizeof(hello)) == 0);
    memset(bytes, 0xA5, sizeof(bytes));
    memcpy(before, bytes, sizeof(bytes));
    hello.device_nonce = 0U;
    CHECK(ucn_i_admission_hello_encode(&hello, bytes) == UCN_ERR_ARGUMENT);
    CHECK(memcmp(bytes, before, sizeof(bytes)) == 0);
    hello = make_hello();

    CHECK(ucn_i_admission_hello_cookie_encode(
              &hello_cookie, bytes, sizeof(bytes), &encoded_bytes) == UCN_OK);
    CHECK(encoded_bytes == sizeof(hello_cookie_golden));
    CHECK(memcmp(bytes, hello_cookie_golden,
                 sizeof(hello_cookie_golden)) == 0);
    CHECK(ucn_i_admission_hello_cookie_decode(
              hello_cookie_golden, sizeof(hello_cookie_golden), 77U,
              &decoded_cookie) == UCN_OK);
    CHECK(memcmp(&decoded_cookie, &hello_cookie,
                 sizeof(hello_cookie)) == 0);

    CHECK(ucn_i_callback_gate_init(&gate, 55U, &gate_ops) == UCN_OK);
    provider.owner = &owner;
    provider.external_hello = &hello;
    config.struct_size = sizeof(config);
    config.api_version = UCN_API_VERSION;
    config.runtime_instance = 10U;
    config.realm_id = 42U;
    config.owner_instance = 20U;
    config.identity_owner_instance = 21U;
    config.persistence_owner_instance = 22U;
    config.max_pending_per_link = UCN_I_ADMISSION_MAX_PENDING_PER_LINK;
    config.token_burst = 4U;
    config.tokens_per_second = 2U;
    config.pending_timeout_us = 5000U;
    config.provider.struct_size = sizeof(config.provider);
    config.provider.api_version = 1U;
    config.provider.context = &provider;
    config.provider.issue_cookie = issue_cookie;
    config.provider.verify_cookie = verify_cookie;
    config.provider.authorize_event = authorize_event;
    config.state_lock = state_ops;
    config.provider_gate = &gate;
    CHECK(ucn_i_admission_owner_init(&owner, &config) == UCN_OK);

    CHECK(ucn_i_admission_issue_cookie(
              &owner, &hello, hello_cookie.key.link, 100U, 40U, 40U,
              UINT32_C(0x01020304), &challenge) == UCN_OK);
    CHECK(provider.cookie_calls == 1U);
    CHECK(challenge.transaction_id == UINT64_C(0x1112131415161718));
    hello = make_hello();
    provider.external_hello = NULL;
    CHECK(ucn_i_admission_cookie_challenge_encode(
              &challenge, bytes) == UCN_OK);
    CHECK(memcmp(bytes, challenge_golden, sizeof(challenge_golden)) == 0);
    CHECK(ucn_i_admission_cookie_challenge_decode(
              challenge_golden, &decoded_challenge) == UCN_OK);
    CHECK(memcmp(&challenge, &decoded_challenge, sizeof(challenge)) == 0);

    provider.external_transcript = &transcript;
    CHECK(ucn_i_admission_open_after_cookie(
              &owner, &hello_cookie, &transcript, 100U, &handle) == UCN_OK);
    transcript = make_cookie_transcript();
    provider.external_transcript = NULL;
    provider.handle = handle;
    CHECK(ucn_i_admission_pending_get(&owner, handle, &pending) == UCN_OK);
    CHECK(pending.phase == UCN_I_ADMISSION_COOKIE_VERIFIED);
    CHECK(pending.deadline_us == 5100U);
    CHECK(ucn_i_admission_open_after_cookie(
              &owner, &hello_cookie, &transcript, 101U, &handle) == UCN_OK);
    CHECK(pending.deadline_us == 5100U);

    add_authority(&transcript);
    evidence = evidence_for(UCN_I_ADMISSION_EVENT_AUTHORITY_PROOF);
    CHECK(ucn_i_admission_advance(
              &owner, handle, UCN_I_ADMISSION_EVENT_AUTHORITY_PROOF,
              &transcript, &evidence, 110U) == UCN_OK);
    add_device(&transcript);
    evidence = evidence_for(UCN_I_ADMISSION_EVENT_DEVICE_PROOF);
    CHECK(ucn_i_admission_advance(
              &owner, handle, UCN_I_ADMISSION_EVENT_DEVICE_PROOF,
              &transcript, &evidence, 120U) == UCN_OK);
    add_address(&transcript);
    evidence = evidence_for(UCN_I_ADMISSION_EVENT_ADDRESS_OFFER);
    CHECK(ucn_i_admission_advance(
              &owner, handle, UCN_I_ADMISSION_EVENT_ADDRESS_OFFER,
              &transcript, &evidence, 130U) == UCN_OK);
    evidence = evidence_for(UCN_I_ADMISSION_EVENT_DEVICE_COMMIT);
    CHECK(ucn_i_admission_advance(
              &owner, handle, UCN_I_ADMISSION_EVENT_DEVICE_COMMIT,
              &transcript, &evidence, 140U) == UCN_OK);

    binding.runtime_instance = 10U;
    binding.realm_id = 42U;
    binding.address = 9U;
    binding.binding_generation = 11U;
    binding.authority_generation = 3U;
    binding.link_generation = 9U;
    binding.local_deadline_us = 2100U;
    binding.record_generation = 1U;
    binding.foundation_transaction_id = 1U;
    binding.witness_generation = 1U;
    binding.identity_owner_instance = 21U;
    binding.persistence_owner_instance = 22U;
    binding.schema_id = UCN_I_IDENTITY_BINDING_SCHEMA_ID;
    binding.schema_version = 1U;
    fill(binding.principal, sizeof(binding.principal), 0x44U);
    fill(binding.body_digest, sizeof(binding.body_digest), 0x66U);
    evidence = evidence_for(UCN_I_ADMISSION_EVENT_FINAL_DURABLE);
    {
        ucn_i_admission_binding_view_t wrong = binding;
        ucn_i_admitted_view_t sentinel;
        ucn_i_admitted_view_t sentinel_before;
        wrong.local_deadline_us = 2150U;
        memset(&sentinel, 0xA5, sizeof(sentinel));
        sentinel_before = sentinel;
        CHECK(ucn_i_admission_finalize(
                  &owner, handle, &transcript, &evidence, &wrong, 150U,
                  &sentinel) == UCN_ERR_SECURITY);
        CHECK(memcmp(&sentinel, &sentinel_before, sizeof(sentinel)) == 0);
    }
    provider.external_binding = &binding;
    CHECK(ucn_i_admission_finalize(
              &owner, handle, &transcript, &evidence, &binding, 150U,
              &admitted) == UCN_OK);
    provider.external_binding = NULL;
    CHECK(admitted.binding.local_deadline_us == 2100U);
    CHECK(admitted.session_generation == 5U);
    CHECK(ucn_i_admission_session_requirement_get(
              &owner, handle, 2099U, &session_requirement) == UCN_OK);
    CHECK(session_requirement.transaction_id == transcript.transaction_id);
    CHECK(session_requirement.absolute_deadline_us == 2100U);
    CHECK(session_requirement.runtime_instance == 10U);
    CHECK(session_requirement.realm_id == 42U);
    CHECK(session_requirement.authority_address == 2U);
    CHECK(session_requirement.authority_binding_generation == 5U);
    CHECK(session_requirement.hop_key_id == 1U);
    CHECK(session_requirement.hop_key_generation == 2U);
    CHECK(session_requirement.e2e_key_id == 3U);
    CHECK(session_requirement.e2e_key_generation == 4U);
    CHECK(memcmp(session_requirement.authority_freshness_transcript_hash,
                 transcript.freshness_proof_transcript_hash, 32U) == 0);
    CHECK(ucn_i_admission_admitted_get(
              &owner, handle, 2099U, &admitted) == UCN_OK);
    CHECK(ucn_i_admission_admitted_get(
              &owner, handle, 2100U, &admitted) == UCN_ERR_ACCESS);
    {
        ucn_i_admission_session_requirement_t sentinel;
        ucn_i_admission_session_requirement_t sentinel_before;
        memset(&sentinel, 0xA5, sizeof(sentinel));
        sentinel_before = sentinel;
        CHECK(ucn_i_admission_session_requirement_get(
                  &owner, handle, 2100U, &sentinel) == UCN_ERR_STATE);
        CHECK(memcmp(&sentinel, &sentinel_before, sizeof(sentinel)) == 0);
    }
    CHECK(provider.authorize_calls == 5U);
    CHECK(provider.reentry_rejected >= 6U);
    CHECK(ucn_i_admission_expire(
              &owner, 6000U, UCN_I_ADMISSION_PENDING_COUNT,
              &inspected, &expired) == UCN_OK);
    CHECK(expired == 0U);
    CHECK(ucn_i_admission_retire(&owner, handle) == UCN_OK);
    CHECK(ucn_i_admission_owner_destroy(&owner) == UCN_OK);

    /* Explicit maintenance may terminate expired pending, but expiry alone
     * never evicts a fixed slot for a new requester. */
    CHECK(ucn_i_admission_owner_init(&owner, &config) == UCN_OK);
    {
        ucn_i_admission_owner_t owner_before = owner;
        uint16_t expired_sentinel = UINT16_C(0xA5A5);

        CHECK(ucn_i_admission_expire(
                  &owner, 100U, 1U, &owner.maintenance_cursor,
                  &expired_sentinel) == UCN_ERR_ARGUMENT);
        CHECK(memcmp(&owner, &owner_before, sizeof(owner)) == 0);
        CHECK(expired_sentinel == UINT16_C(0xA5A5));
    }
    {
        ucn_i_admission_handle_t handles[UCN_I_ADMISSION_PENDING_COUNT];
        uint16_t index;

        for (index = 0U; index < UCN_I_ADMISSION_PENDING_COUNT; ++index) {
            ucn_i_admission_hello_cookie_t item = make_hello_cookie();
            ucn_i_admission_transcript_t item_transcript =
                make_cookie_transcript();
            item.key.link.id = (uint16_t)(index + 1U);
            item.key.transaction_id += index;
            item.key.local_peer_discriminator += index;
            item_transcript.selected_link_id = item.key.link.id;
            item_transcript.transaction_id = item.key.transaction_id;
            CHECK(ucn_i_admission_open_after_cookie(
                      &owner, &item, &item_transcript, 100U,
                      &handles[index]) == UCN_OK);
        }
        CHECK(ucn_i_admission_expire(
                  &owner, 5100U, UCN_I_ADMISSION_PENDING_COUNT,
                  &inspected, &expired) == UCN_OK);
        CHECK(expired == UCN_I_ADMISSION_PENDING_COUNT);
        {
            ucn_i_admission_hello_cookie_t extra = make_hello_cookie();
            ucn_i_admission_transcript_t extra_transcript =
                make_cookie_transcript();
            ucn_i_admission_handle_t sentinel;
            ucn_i_admission_handle_t sentinel_before;
            extra.key.transaction_id += UCN_I_ADMISSION_PENDING_COUNT + 1U;
            extra.key.local_peer_discriminator +=
                UCN_I_ADMISSION_PENDING_COUNT + 1U;
            extra_transcript.transaction_id = extra.key.transaction_id;
            memset(&sentinel, 0xA5, sizeof(sentinel));
            sentinel_before = sentinel;
            CHECK(ucn_i_admission_open_after_cookie(
                      &owner, &extra, &extra_transcript, 5200U,
                      &sentinel) == UCN_ERR_NO_SPACE);
            CHECK(memcmp(&sentinel, &sentinel_before,
                         sizeof(sentinel)) == 0);
        }
        for (index = 0U; index < UCN_I_ADMISSION_PENDING_COUNT; ++index) {
            CHECK(ucn_i_admission_retire(&owner, handles[index]) == UCN_OK);
        }
    }
    CHECK(ucn_i_admission_owner_destroy(&owner) == UCN_OK);
    CHECK(ucn_i_callback_gate_destroy(&gate) == UCN_OK);

    puts("admission tests passed");
    return 0;
}
