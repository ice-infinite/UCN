#include "internal/ucn_security.h"

#include <stdio.h>
#include <string.h>

#define CHECK(expression)                                                   \
    do {                                                                    \
        if (!(expression)) {                                                \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,       \
                    __LINE__, #expression);                                 \
            return 1;                                                       \
        }                                                                   \
    } while (0)

typedef struct test_lock {
    uint8_t held;
} test_lock_t;

typedef struct test_provider {
    ucn_i_security_owner_t *owner;
    const ucn_i_security_config_t *reinit_config;
    ucn_i_security_handle_t invalid_handle;
    uint32_t calls;
    uint8_t expect_proof;
    uint8_t reentry_checked;
    uint8_t reinit_checked;
    uint8_t corrupt_o1_payload;
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

static ucn_result_t verify_session(
    void *context,
    const ucn_i_security_candidate_t *candidate,
    const uint8_t *proof,
    size_t proof_bytes)
{
    test_provider_t *provider = context;
    ucn_i_security_session_view_t sentinel;
    ucn_i_security_session_view_t before;

    provider->calls++;
    if (provider->owner != NULL) {
        memset(&sentinel, 0xA5, sizeof(sentinel));
        before = sentinel;
        if (ucn_i_security_session_get(provider->owner,
                                       provider->invalid_handle,
                                       &sentinel) != UCN_ERR_STATE ||
            memcmp(&sentinel, &before, sizeof(sentinel)) != 0) {
            return UCN_ERR_STATE;
        }
        provider->reentry_checked = 1U;
        if (provider->reinit_config != NULL) {
            if (ucn_i_security_owner_init(provider->owner,
                                          provider->reinit_config) !=
                UCN_ERR_STATE) {
                return UCN_ERR_STATE;
            }
            provider->reinit_checked = 1U;
        }
    }
    if (candidate == NULL || proof == NULL || proof_bytes != 1U ||
        proof[0] != provider->expect_proof) {
        return UCN_ERR_SECURITY;
    }
    return UCN_OK;
}

static void test_tag(const ucn_i_security_key_selector_t *selector,
                     const uint8_t *aad,
                     size_t aad_bytes,
                     const uint8_t *payload,
                     size_t payload_bytes,
                     uint8_t tag[UCN_I_SECURITY_ORIGIN_TAG_BYTES])
{
    uint32_t value = UINT32_C(2166136261) ^ selector->key_generation ^
                     selector->key_id ^ selector->suite_id;
    size_t index;

    for (index = 0U; index < aad_bytes; ++index) {
        value = (value ^ aad[index]) * UINT32_C(16777619);
    }
    for (index = 0U; index < payload_bytes; ++index) {
        value = (value ^ payload[index]) * UINT32_C(16777619);
    }
    for (index = 0U; index < UCN_I_SECURITY_ORIGIN_TAG_BYTES; ++index) {
        value = value * UINT32_C(1664525) + UINT32_C(1013904223);
        tag[index] = (uint8_t)(value >> 24U);
    }
}

static ucn_result_t protect_origin(
    void *context,
    const ucn_i_security_origin_protect_request_t *request)
{
    test_provider_t *provider = context;
    ucn_i_security_session_view_t sentinel;
    ucn_i_security_session_view_t before;
    size_t index;

    provider->calls++;
    if (provider->owner != NULL) {
        memset(&sentinel, 0xA5, sizeof(sentinel));
        before = sentinel;
        if (ucn_i_security_session_get(provider->owner,
                                       provider->invalid_handle,
                                       &sentinel) != UCN_ERR_STATE ||
            memcmp(&sentinel, &before, sizeof(sentinel)) != 0) {
            return UCN_ERR_STATE;
        }
        provider->reentry_checked++;
    }
    if (request == NULL || request->aad == NULL ||
        request->plaintext == NULL || request->protected_payload == NULL ||
        request->origin_tag == NULL ||
        request->aad_bytes != UCN_I_SECURITY_C1_ORIGIN_AAD_BYTES) {
        return UCN_ERR_ARGUMENT;
    }
    if (request->selector.suite_id == 1U) {
        if (request->nonce_input != NULL || request->nonce_input_bytes != 0U) {
            return UCN_ERR_ARGUMENT;
        }
        memcpy(request->protected_payload, request->plaintext,
               request->payload_bytes);
    } else if (request->selector.suite_id == 2U ||
               request->selector.suite_id == 3U) {
        if (request->nonce_input == NULL ||
            request->nonce_input_bytes !=
                UCN_I_SECURITY_SEQUENCE_NONCE_INPUT_BYTES) {
            return UCN_ERR_ARGUMENT;
        }
        for (index = 0U; index < request->payload_bytes; ++index) {
            request->protected_payload[index] =
                request->plaintext[index] ^ request->nonce_input[index %
                    request->nonce_input_bytes] ^
                (uint8_t)request->selector.key_id;
        }
    } else {
        return UCN_ERR_UNSUPPORTED;
    }
    if (request->selector.suite_id == 1U &&
        provider->corrupt_o1_payload != 0U &&
        request->payload_bytes != 0U) {
        request->protected_payload[0] ^= 1U;
    }
    test_tag(&request->selector, request->aad, request->aad_bytes,
             request->protected_payload, request->payload_bytes,
             request->origin_tag);
    return UCN_OK;
}

static ucn_result_t open_origin(
    void *context,
    const ucn_i_security_origin_open_request_t *request)
{
    test_provider_t *provider = context;
    ucn_i_security_session_view_t sentinel;
    ucn_i_security_session_view_t before;
    uint8_t expected[UCN_I_SECURITY_ORIGIN_TAG_BYTES];
    size_t index;

    provider->calls++;
    if (provider->owner != NULL) {
        memset(&sentinel, 0xA5, sizeof(sentinel));
        before = sentinel;
        if (ucn_i_security_session_get(provider->owner,
                                       provider->invalid_handle,
                                       &sentinel) != UCN_ERR_STATE ||
            memcmp(&sentinel, &before, sizeof(sentinel)) != 0) {
            return UCN_ERR_STATE;
        }
        provider->reentry_checked++;
    }
    if (request == NULL || request->aad == NULL ||
        request->protected_payload == NULL || request->origin_tag == NULL ||
        request->plaintext == NULL ||
        request->aad_bytes != UCN_I_SECURITY_C1_ORIGIN_AAD_BYTES) {
        return UCN_ERR_ARGUMENT;
    }
    test_tag(&request->selector, request->aad, request->aad_bytes,
             request->protected_payload, request->payload_bytes, expected);
    if (memcmp(expected, request->origin_tag, sizeof(expected)) != 0) {
        return UCN_ERR_SECURITY;
    }
    if (request->selector.suite_id == 1U) {
        if (request->nonce_input != NULL || request->nonce_input_bytes != 0U) {
            return UCN_ERR_ARGUMENT;
        }
        memcpy(request->plaintext, request->protected_payload,
               request->payload_bytes);
    } else if (request->selector.suite_id == 2U ||
               request->selector.suite_id == 3U) {
        if (request->nonce_input == NULL ||
            request->nonce_input_bytes !=
                UCN_I_SECURITY_SEQUENCE_NONCE_INPUT_BYTES) {
            return UCN_ERR_ARGUMENT;
        }
        for (index = 0U; index < request->payload_bytes; ++index) {
            request->plaintext[index] =
                request->protected_payload[index] ^
                request->nonce_input[index % request->nonce_input_bytes] ^
                (uint8_t)request->selector.key_id;
        }
    } else {
        return UCN_ERR_UNSUPPORTED;
    }
    return UCN_OK;
}

static ucn_result_t protect_hop(
    void *context,
    const ucn_i_security_hop_request_t *request)
{
    test_provider_t *provider = context;
    uint8_t tag[UCN_I_SECURITY_ORIGIN_TAG_BYTES];
    ucn_i_security_session_view_t sentinel;
    ucn_i_security_session_view_t before;

    provider->calls++;
    if (provider->owner != NULL) {
        memset(&sentinel, 0xA5, sizeof(sentinel));
        before = sentinel;
        if (ucn_i_security_session_get(provider->owner,
                                       provider->invalid_handle,
                                       &sentinel) != UCN_ERR_STATE ||
            memcmp(&sentinel, &before, sizeof(sentinel)) != 0) {
            return UCN_ERR_STATE;
        }
        provider->reentry_checked++;
    }
    if (request == NULL || request->aad == NULL ||
        request->hop_tag == NULL || request->selector.suite_id != 16U) {
        return UCN_ERR_ARGUMENT;
    }
    test_tag(&request->selector, request->aad, request->aad_bytes,
             NULL, 0U, tag);
    memcpy(request->hop_tag, tag, UCN_I_SECURITY_HOP_TAG_BYTES);
    return UCN_OK;
}

static ucn_result_t verify_hop(
    void *context,
    const ucn_i_security_hop_verify_request_t *request)
{
    test_provider_t *provider = context;
    uint8_t tag[UCN_I_SECURITY_ORIGIN_TAG_BYTES];
    ucn_i_security_session_view_t sentinel;
    ucn_i_security_session_view_t before;

    provider->calls++;
    if (provider->owner != NULL) {
        memset(&sentinel, 0xA5, sizeof(sentinel));
        before = sentinel;
        if (ucn_i_security_session_get(provider->owner,
                                       provider->invalid_handle,
                                       &sentinel) != UCN_ERR_STATE ||
            memcmp(&sentinel, &before, sizeof(sentinel)) != 0) {
            return UCN_ERR_STATE;
        }
        provider->reentry_checked++;
    }
    if (request == NULL || request->aad == NULL ||
        request->hop_tag == NULL || request->selector.suite_id != 16U) {
        return UCN_ERR_ARGUMENT;
    }
    test_tag(&request->selector, request->aad, request->aad_bytes,
             NULL, 0U, tag);
    return memcmp(request->hop_tag, tag,
                  UCN_I_SECURITY_HOP_TAG_BYTES) == 0 ?
               UCN_OK : UCN_ERR_SECURITY;
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

static void fill_bytes(uint8_t *bytes, size_t length, uint8_t seed)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = (uint8_t)(seed + index);
    }
}

static ucn_i_security_candidate_t candidate_make(uint8_t peer_seed,
                                                 uint32_t generation)
{
    ucn_i_security_candidate_t candidate;

    memset(&candidate, 0, sizeof(candidate));
    candidate.local.address = 1U;
    candidate.local.binding_generation = 7U;
    fill_bytes(candidate.local.principal,
               sizeof(candidate.local.principal), 0x10U);
    candidate.peer.address = (uint32_t)(peer_seed + 2U);
    candidate.peer.binding_generation = 9U;
    fill_bytes(candidate.peer.principal,
               sizeof(candidate.peer.principal), peer_seed);
    candidate.expires_at_us = 5000U;
    candidate.link_generation = 11U;
    candidate.session_generation = generation;
    candidate.policy_generation = 13U;
    candidate.origin_tx.key_generation = 21U;
    candidate.origin_tx.key_id = 3U;
    candidate.origin_tx.suite_id = 1U;
    candidate.origin_rx.key_generation = 22U;
    candidate.origin_rx.key_id = 4U;
    candidate.origin_rx.suite_id = 1U;
    fill_bytes(candidate.origin_fingerprint,
               sizeof(candidate.origin_fingerprint), 0x31U);
    fill_bytes(candidate.hop_fingerprint,
               sizeof(candidate.hop_fingerprint), 0x51U);
    fill_bytes(candidate.transcript_digest,
               sizeof(candidate.transcript_digest), 0x71U);
    candidate.origin_level = UCN_I_SECURITY_AUTHENTICATED;
    candidate.hop_profile = UCN_I_HOP_PROFILE_H0;
    candidate.address_width = 1U;
    return candidate;
}

static ucn_i_security_durability_base_t durability_make(uint64_t domain_id,
                                                        uint32_t generation)
{
    ucn_i_security_durability_base_t base;

    memset(&base, 0, sizeof(base));
    base.domain_id = domain_id;
    base.next_transaction_id = 10U;
    base.expected_record_generation = 3U;
    base.absolute_deadline_us = 4000U;
    base.prior_session_generation = generation - 1U;
    base.domain_generation = 5U;
    base.volatile_continuation = 17U;
    fill_bytes(base.expected_body_digest,
               sizeof(base.expected_body_digest), 0x91U);
    return base;
}

static ucn_i_security_config_t config_make(
    test_lock_t *state_lock,
    ucn_i_callback_gate_t *gate,
    test_provider_t *provider,
    const ucn_i_security_domain_rule_t *rules,
    uint16_t rule_count,
    const ucn_i_security_acl_rule_t *acl,
    uint16_t acl_count)
{
    ucn_i_security_config_t config;

    memset(&config, 0, sizeof(config));
    config.struct_size = sizeof(config);
    config.api_version = UCN_API_VERSION;
    config.runtime_instance = 41U;
    config.realm_id = 0x12345678U;
    config.owner_instance = 6U;
    config.persistence_business_owner_instance = 7U;
    config.address_width = 1U;
    config.domain_rules = rules;
    config.domain_rule_count = rule_count;
    config.acl_rules = acl;
    config.acl_rule_count = acl_count;
    config.replay_reservation_lifetime_us = 100U;
    config.provider.struct_size = sizeof(config.provider);
    config.provider.api_version = 1U;
    config.provider.context = provider;
    config.provider.verify_session = verify_session;
    config.provider.protect_origin = protect_origin;
    config.provider.open_origin = open_origin;
    config.provider.protect_hop = protect_hop;
    config.provider.verify_hop = verify_hop;
    config.state_lock = lock_ops(state_lock);
    config.provider_gate = gate;
    return config;
}

static ucn_i_security_acl_rule_t acl_make(
    const ucn_i_security_candidate_t *candidate,
    uint8_t direction)
{
    ucn_i_security_acl_rule_t rule;

    memset(&rule, 0, sizeof(rule));
    memcpy(rule.peer_principal, candidate->peer.principal,
           sizeof(rule.peer_principal));
    memcpy(rule.context_fingerprint, candidate->origin_fingerprint,
           sizeof(rule.context_fingerprint));
    rule.peer_binding_generation = candidate->peer.binding_generation;
    rule.service_id = 0x1201U;
    rule.protocol_opcode = 0U;
    rule.direction = direction;
    return rule;
}

typedef struct session_fixture {
    ucn_i_security_owner_t owner;
    ucn_i_callback_gate_t gate;
    test_lock_t state_lock;
    test_lock_t gate_lock;
    test_provider_t provider;
    ucn_i_security_domain_rule_t rule;
    ucn_i_security_acl_rule_t acl;
    ucn_i_security_config_t config;
    ucn_i_security_candidate_t candidate;
    ucn_i_security_handle_t handle;
    ucn_i_security_current_facts_t facts;
} session_fixture_t;

static int fixture_activate(session_fixture_t *fixture,
                            const ucn_i_security_candidate_t *candidate,
                            uint8_t direction,
                            uint32_t runtime_instance,
                            uint16_t gate_owner)
{
    ucn_i_security_durability_base_t durability =
        durability_make(0x8001U, candidate->session_generation);
    ucn_i_security_requirement_view_t requirement;
    ucn_i_security_durability_proof_t proof;
    ucn_handle_t persistence_handle;
    ucn_i_lock_ops_t gate_ops;
    uint8_t published_digest[16];
    uint8_t session_proof = 0xC3U;

    memset(fixture, 0, sizeof(*fixture));
    fixture->candidate = *candidate;
    fixture->provider.expect_proof = session_proof;
    fixture->rule.domain_id = durability.domain_id;
    memcpy(fixture->rule.peer_principal, candidate->peer.principal,
           sizeof(fixture->rule.peer_principal));
    fixture->acl = acl_make(candidate, direction);
    gate_ops = lock_ops(&fixture->gate_lock);
    CHECK(ucn_i_callback_gate_init(&fixture->gate, gate_owner,
                                   &gate_ops) == UCN_OK);
    fixture->config = config_make(&fixture->state_lock, &fixture->gate,
                                  &fixture->provider, &fixture->rule, 1U,
                                  &fixture->acl, 1U);
    fixture->config.runtime_instance = runtime_instance;
    CHECK(ucn_i_security_owner_init(&fixture->owner,
                                    &fixture->config) == UCN_OK);
    fixture->provider.owner = &fixture->owner;
    CHECK(ucn_i_security_prepare_static_session(
              &fixture->owner, &fixture->candidate, &session_proof, 1U,
              &durability, 1000U, &fixture->handle) == UCN_OK);
    CHECK(ucn_i_security_requirement_get(&fixture->owner, fixture->handle,
                                         &requirement) == UCN_OK);
    memset(&persistence_handle, 0, sizeof(persistence_handle));
    persistence_handle.runtime_instance = runtime_instance;
    persistence_handle.owner_instance = 8U;
    persistence_handle.slot = 1U;
    persistence_handle.generation = 2U;
    persistence_handle.object_kind = UCN_OBJECT_KIND_PERSISTENCE;
    fill_bytes(published_digest, sizeof(published_digest), 0xD1U);
    CHECK(ucn_i_security_bind_persistence(
              &fixture->owner, fixture->handle, persistence_handle,
              published_digest) == UCN_OK);
    memset(&proof, 0, sizeof(proof));
    proof.persistence_handle = persistence_handle;
    proof.domain_id = requirement.domain_id;
    proof.record_generation = requirement.expected_record_generation + 1U;
    proof.foundation_transaction_id = requirement.foundation_transaction_id;
    proof.witness_generation = proof.record_generation;
    proof.transition_fingerprint = requirement.transition_fingerprint;
    proof.runtime_instance = runtime_instance;
    proof.body_bytes = UCN_I_SECURITY_RECORD_BYTES;
    proof.volatile_continuation = requirement.volatile_continuation;
    proof.persistence_owner_instance = persistence_handle.owner_instance;
    proof.caller_owner_instance =
        fixture->config.persistence_business_owner_instance;
    proof.domain_generation = requirement.domain_generation;
    proof.schema_id = requirement.schema_id;
    proof.schema_version = requirement.schema_version;
    proof.operation_kind = requirement.operation_kind;
    memcpy(proof.body_digest, published_digest, sizeof(published_digest));
    fixture->facts.local = candidate->local;
    fixture->facts.peer = candidate->peer;
    fixture->facts.now_us = 1500U;
    fixture->facts.link_generation = candidate->link_generation;
    fixture->facts.policy_generation = candidate->policy_generation;
    CHECK(ucn_i_security_activate(&fixture->owner, fixture->handle,
                                  &proof, &fixture->facts) == UCN_OK);
    return 0;
}

static int fixture_destroy(session_fixture_t *fixture)
{
    CHECK(ucn_i_security_fence(&fixture->owner, fixture->handle) == UCN_OK);
    CHECK(ucn_i_security_retire_fenced(&fixture->owner,
                                       fixture->handle) == UCN_OK);
    CHECK(ucn_i_security_owner_destroy(&fixture->owner) == UCN_OK);
    CHECK(ucn_i_callback_gate_destroy(&fixture->gate) == UCN_OK);
    return 0;
}

static int test_record_codec(void)
{
    ucn_i_security_candidate_t candidate = candidate_make(0x20U, 1U);
    ucn_i_security_candidate_t decoded;
    ucn_i_security_codec_workspace_t workspace;
    uint8_t record[UCN_I_SECURITY_RECORD_BYTES];
    uint8_t malformed[UCN_I_SECURITY_RECORD_BYTES];
    uint32_t realm = 0U;

    CHECK(ucn_i_security_record_encode(0x12345678U, &candidate, record) ==
          UCN_OK);
    CHECK(memcmp(record, "UC6S", 4U) == 0);
    CHECK(record[4] == 0U && record[5] == 1U);
    CHECK(record[12] == 0x12U && record[13] == 0x34U &&
          record[14] == 0x56U && record[15] == 0x78U);
    memset(&decoded, 0xA5, sizeof(decoded));
    memset(&workspace, 0, sizeof(workspace));
    CHECK(ucn_i_security_record_decode(record, &workspace, &realm,
                                       &decoded) == UCN_OK);
    CHECK(realm == 0x12345678U);
    CHECK(memcmp(&decoded, &candidate, sizeof(decoded)) == 0);

    memcpy(malformed, record, sizeof(malformed));
    malformed[9] = 1U;
    memset(&decoded, 0x5A, sizeof(decoded));
    candidate = decoded;
    realm = 0xA5A5A5A5U;
    CHECK(ucn_i_security_record_decode(malformed, &workspace, &realm,
                                       &decoded) == UCN_ERR_MALFORMED);
    CHECK(realm == 0xA5A5A5A5U);
    CHECK(memcmp(&decoded, &candidate, sizeof(decoded)) == 0);
    return 0;
}

static int test_session_prepare_and_lifecycle(void)
{
    ucn_i_security_owner_t owner;
    ucn_i_callback_gate_t gate;
    test_lock_t state_lock = {0};
    test_lock_t gate_lock = {0};
    test_provider_t provider;
    ucn_i_security_domain_rule_t rule;
    ucn_i_security_acl_rule_t acl;
    ucn_i_security_config_t config;
    ucn_i_lock_ops_t gate_ops;
    ucn_i_security_candidate_t candidate = candidate_make(0x20U, 1U);
    ucn_i_security_durability_base_t durability =
        durability_make(0x8001U, 1U);
    ucn_i_security_handle_t handle;
    ucn_i_security_handle_t before;
    ucn_i_security_requirement_view_t requirement;
    ucn_i_security_session_view_t view;
    ucn_i_security_current_facts_t facts;
    ucn_i_security_durability_proof_t durability_proof;
    ucn_handle_t persistence_handle;
    uint8_t published_digest[16];
    ucn_i_security_access_request_t access;
    ucn_i_security_replay_handle_t replay;
    ucn_i_security_replay_handle_t replay2;
    ucn_i_security_tx_handle_t tx;
    ucn_i_security_tx_handle_t tx2;
    ucn_i_security_tx_handle_t tx_before;
    ucn_i_security_c1_origin_material_t material;
    ucn_i_security_candidate_t next_candidate;
    ucn_i_security_durability_base_t next_durability;
    ucn_i_security_handle_t next_handle;
    ucn_i_security_handle_t next_before;
    uint8_t aad[UCN_I_SECURITY_C1_ORIGIN_AAD_BYTES];
    uint8_t aad_before[UCN_I_SECURITY_C1_ORIGIN_AAD_BYTES];
    uint8_t nonce_input[UCN_I_SECURITY_SEQUENCE_NONCE_INPUT_BYTES];
    uint8_t aad_digest[16];
    uint8_t payload_digest[16];
    uint16_t inspected = 0U;
    uint16_t expired = 0U;
    uint8_t proof = 0xC3U;

    memset(&provider, 0, sizeof(provider));
    memset(&owner, 0, sizeof(owner));
    memset(&gate, 0, sizeof(gate));
    provider.expect_proof = proof;
    memset(&rule, 0, sizeof(rule));
    rule.domain_id = durability.domain_id;
    memcpy(rule.peer_principal, candidate.peer.principal,
           sizeof(rule.peer_principal));
    acl = acl_make(&candidate, UCN_I_SECURITY_ACCESS_OUTBOUND);
    gate_ops = lock_ops(&gate_lock);
    CHECK(ucn_i_callback_gate_init(&gate, 71U, &gate_ops) == UCN_OK);
    config = config_make(&state_lock, &gate, &provider, &rule, 1U,
                         &acl, 1U);
    CHECK(ucn_i_security_owner_init(&owner, &config) == UCN_OK);
    provider.owner = &owner;
    provider.reinit_config = &config;
    memset(&provider.invalid_handle, 0, sizeof(provider.invalid_handle));
    memset(&handle, 0xA5, sizeof(handle));
    CHECK(ucn_i_security_prepare_static_session(
              &owner, &candidate, &proof, 1U, &durability, 1000U,
              &handle) == UCN_OK);
    CHECK(provider.calls == 1U && provider.reentry_checked == 1U &&
          provider.reinit_checked == 1U);
    CHECK(ucn_i_security_session_get(&owner, handle, &view) == UCN_OK);
    CHECK(ucn_i_security_owner_init(&owner, &config) == UCN_ERR_STATE);
    CHECK(ucn_i_security_session_get(&owner, handle, &view) == UCN_OK);
    CHECK(view.phase == UCN_I_SECURITY_SESSION_AWAITING_DURABILITY);
    CHECK(view.session_generation == 1U);

    next_candidate = candidate_make(0x20U, 2U);
    next_durability = durability_make(durability.domain_id, 2U);
    memset(&next_handle, 0x5A, sizeof(next_handle));
    next_before = next_handle;
    CHECK(ucn_i_security_prepare_static_session(
              &owner, &next_candidate, &proof, 1U, &next_durability,
              1000U, &next_handle) == UCN_ERR_STATE);
    CHECK(provider.calls == 1U);
    CHECK(memcmp(&next_handle, &next_before, sizeof(next_handle)) == 0);
    CHECK(ucn_i_security_requirement_get(&owner, handle, &requirement) ==
          UCN_OK);
    CHECK(requirement.body_bytes == UCN_I_SECURITY_RECORD_BYTES);
    CHECK(requirement.domain_id == durability.domain_id);
    CHECK(requirement.foundation_transaction_id ==
          durability.next_transaction_id);
    CHECK(requirement.schema_id == UCN_I_SECURITY_RECORD_SCHEMA_ID);
    CHECK(requirement.transition_fingerprint != 0U);

    memset(&persistence_handle, 0, sizeof(persistence_handle));
    persistence_handle.runtime_instance = config.runtime_instance;
    persistence_handle.owner_instance = 8U;
    persistence_handle.slot = 1U;
    persistence_handle.generation = 2U;
    persistence_handle.object_kind = UCN_OBJECT_KIND_PERSISTENCE;
    fill_bytes(published_digest, sizeof(published_digest), 0xD1U);
    CHECK(ucn_i_security_bind_persistence(&owner, handle,
                                          persistence_handle,
                                          published_digest) == UCN_OK);
    memset(&durability_proof, 0, sizeof(durability_proof));
    durability_proof.persistence_handle = persistence_handle;
    durability_proof.domain_id = requirement.domain_id;
    durability_proof.record_generation =
        requirement.expected_record_generation + 1U;
    durability_proof.foundation_transaction_id =
        requirement.foundation_transaction_id;
    durability_proof.witness_generation = durability_proof.record_generation;
    durability_proof.transition_fingerprint =
        requirement.transition_fingerprint;
    durability_proof.runtime_instance = config.runtime_instance;
    durability_proof.body_bytes = UCN_I_SECURITY_RECORD_BYTES;
    durability_proof.volatile_continuation =
        requirement.volatile_continuation;
    durability_proof.persistence_owner_instance =
        persistence_handle.owner_instance;
    durability_proof.caller_owner_instance =
        config.persistence_business_owner_instance;
    durability_proof.domain_generation = requirement.domain_generation;
    durability_proof.schema_id = requirement.schema_id;
    durability_proof.schema_version = requirement.schema_version;
    durability_proof.operation_kind = UCN_I_SECURITY_OPERATION_KIND;
    memcpy(durability_proof.body_digest, published_digest,
           sizeof(published_digest));
    memset(&facts, 0, sizeof(facts));
    facts.local = candidate.local;
    facts.peer = candidate.peer;
    facts.now_us = 1500U;
    facts.link_generation = candidate.link_generation;
    facts.policy_generation = candidate.policy_generation;

    durability_proof.schema_id ^= 1U;
    CHECK(ucn_i_security_activate(&owner, handle, &durability_proof,
                                  &facts) == UCN_ERR_STATE);
    durability_proof.schema_id ^= 1U;
    durability_proof.body_digest[0] ^= 1U;
    CHECK(ucn_i_security_activate(&owner, handle, &durability_proof,
                                  &facts) == UCN_ERR_STATE);
    CHECK(ucn_i_security_session_get(&owner, handle, &view) == UCN_OK);
    CHECK(view.phase == UCN_I_SECURITY_SESSION_AWAITING_DURABILITY);
    durability_proof.body_digest[0] ^= 1U;
    CHECK(ucn_i_security_activate(&owner, handle, &durability_proof,
                                  &facts) == UCN_OK);
    CHECK(ucn_i_security_session_get(&owner, handle, &view) == UCN_OK);
    CHECK(view.phase == UCN_I_SECURITY_SESSION_ACTIVE);

    memset(&access, 0, sizeof(access));
    access.session = handle;
    memcpy(access.context_fingerprint, candidate.origin_fingerprint,
           sizeof(access.context_fingerprint));
    access.service_id = acl.service_id;
    access.protocol_opcode = acl.protocol_opcode;
    access.direction = acl.direction;
    CHECK(ucn_i_security_authorize(&owner, &access, &facts) == UCN_OK);

    memset(&tx, 0xA5, sizeof(tx));
    CHECK(ucn_i_security_tx_reserve(&owner, &access, &facts, &tx) ==
          UCN_OK);
    CHECK(tx.origin_sequence == 1U && tx.deadline_us == 1600U);
    memset(&tx2, 0x5A, sizeof(tx2));
    tx_before = tx2;
    CHECK(ucn_i_security_tx_reserve(&owner, &access, &facts, &tx2) ==
          UCN_ERR_NO_SPACE);
    CHECK(memcmp(&tx2, &tx_before, sizeof(tx2)) == 0);
    CHECK(ucn_i_security_tx_commit(&owner, tx, &facts) == UCN_OK);
    CHECK(ucn_i_security_tx_commit(&owner, tx, &facts) == UCN_ERR_STATE);
    CHECK(ucn_i_security_tx_reserve(&owner, &access, &facts, &tx2) ==
          UCN_OK);
    CHECK(tx2.origin_sequence == 2U);
    CHECK(ucn_i_security_tx_abort(&owner, tx2) == UCN_OK);
    CHECK(ucn_i_security_tx_abort(&owner, tx2) == UCN_ERR_STATE);

    memset(&material, 0, sizeof(material));
    material.session = handle;
    material.facts = facts;
    material.payload_bytes = 4U;
    material.source_address = candidate.local.address;
    material.destination_address = candidate.peer.address;
    material.origin_sequence = UINT32_C(0x01020304);
    material.service_id = acl.service_id;
    material.protocol_opcode = acl.protocol_opcode;
    material.common_header[0] = UINT8_C(0x61);
    material.common_header[1] = UINT8_C(0x40);
    material.common_header[2] = UINT8_C(0x45);
    material.direction = UCN_I_SECURITY_ACCESS_OUTBOUND;
    CHECK(ucn_i_security_c1_origin_aad_build(&owner, &material, aad) ==
          UCN_OK);
    CHECK(memcmp(aad, "UCN6-ORIGIN-V1", 14U) == 0);
    CHECK(aad[14] == 0U && aad[15] == 0U && aad[16] == 0U &&
          aad[17] == 54U);
    CHECK(aad[18] == 0x61U && aad[19] == 0x40U && aad[20] == 0x40U &&
          aad[21] == 1U);
    CHECK(memcmp(aad + 22U, candidate.local.principal, 16U) == 0);
    CHECK(aad[38] == 0U && aad[39] == 0U && aad[40] == 0U &&
          aad[41] == 7U);
    CHECK(memcmp(aad + 42U, candidate.peer.principal, 16U) == 0);
    CHECK(aad[58] == 0U && aad[59] == 0U && aad[60] == 0U &&
          aad[61] == 9U);
    CHECK(aad[62] == 0x12U && aad[63] == 0x01U);
    CHECK(aad[64] == 1U && aad[65] == 2U && aad[66] == 3U &&
          aad[67] == 4U);
    CHECK(aad[68] == 0U && aad[69] == 0U && aad[70] == 0U &&
          aad[71] == 4U);
    CHECK(ucn_i_security_sequence_nonce_input_build(
              &owner, &material, nonce_input) == UCN_OK);
    CHECK(memcmp(nonce_input, "UCN6-NONCE-SEQ-V1", 17U) == 0);
    CHECK(memcmp(nonce_input + 17U, candidate.origin_fingerprint, 16U) == 0);
    CHECK(nonce_input[33] == 1U && nonce_input[34] == 2U &&
          nonce_input[35] == 3U && nonce_input[36] == 4U);
    memcpy(aad_before, aad, sizeof(aad));
    material.common_header[2] = UINT8_C(0x46);
    material.destination_address++;
    CHECK(ucn_i_security_c1_origin_aad_build(&owner, &material, aad) ==
          UCN_ERR_STATE);
    CHECK(memcmp(aad, aad_before, sizeof(aad)) == 0);
    material.destination_address--;
    material.common_header[2] = UINT8_C(0x85);
    CHECK(ucn_i_security_c1_origin_aad_build(&owner, &material, aad) ==
          UCN_ERR_STATE);
    CHECK(memcmp(aad, aad_before, sizeof(aad)) == 0);

    access.protocol_opcode = 1U;
    CHECK(ucn_i_security_authorize(&owner, &access, &facts) ==
          UCN_ERR_ACCESS);

    fill_bytes(aad_digest, sizeof(aad_digest), 0x11U);
    fill_bytes(payload_digest, sizeof(payload_digest), 0x21U);
    CHECK(ucn_i_security_replay_reserve(&owner, handle, &facts, 10U,
                                        aad_digest, payload_digest,
                                        &replay) == UCN_OK);
    CHECK(ucn_i_security_replay_reserve(&owner, handle, &facts, 10U,
                                        aad_digest, payload_digest,
                                        &replay2) == UCN_ERR_STATE);
    replay2 = replay;
    replay2.hop_sequence = 1U;
    CHECK(ucn_i_security_replay_commit(&owner, replay2, &facts) ==
          UCN_ERR_STATE);
    facts.now_us = 1550U;
    CHECK(ucn_i_security_replay_commit(&owner, replay, &facts) == UCN_OK);
    CHECK(ucn_i_security_replay_reserve(&owner, handle, &facts, 10U,
                                        aad_digest, payload_digest,
                                        &replay2) == UCN_ERR_REPLAY);
    CHECK(ucn_i_security_replay_reserve(&owner, handle, &facts, 12U,
                                        aad_digest, payload_digest,
                                        &replay2) == UCN_OK);
    CHECK(ucn_i_security_replay_abort(&owner, replay2) == UCN_OK);
    CHECK(ucn_i_security_replay_reserve(&owner, handle, &facts, 13U,
                                        aad_digest, payload_digest,
                                        &replay2) == UCN_OK);
    CHECK(ucn_i_security_replay_maintain(&owner, 1650U, 1U,
                                         &inspected, &expired) == UCN_OK);
    while (expired == 0U) {
        CHECK(ucn_i_security_replay_maintain(&owner, 1650U, 1U,
                                             &inspected, &expired) == UCN_OK);
    }
    CHECK(ucn_i_security_replay_abort(&owner, replay2) == UCN_ERR_STATE);

    CHECK(ucn_i_security_fence(&owner, handle) == UCN_OK);
    CHECK(ucn_i_security_owner_init(&owner, &config) == UCN_ERR_STATE);
    CHECK(ucn_i_security_session_get(&owner, handle, &view) == UCN_OK);
    CHECK(view.phase == UCN_I_SECURITY_SESSION_FENCED);
    CHECK(ucn_i_security_requirement_get(&owner, handle, &requirement) ==
          UCN_ERR_STATE);
    CHECK(ucn_i_security_retire_fenced(&owner, handle) == UCN_OK);
    memset(&view, 0xA5, sizeof(view));
    CHECK(ucn_i_security_session_get(&owner, handle, &view) ==
          UCN_ERR_STATE);

    before = handle;
    proof = 0x00U;
    CHECK(ucn_i_security_prepare_static_session(
              &owner, &candidate, &proof, 1U, &durability, 1000U,
              &handle) == UCN_ERR_SECURITY);
    CHECK(memcmp(&handle, &before, sizeof(handle)) == 0);
    CHECK(ucn_i_security_owner_destroy(&owner) == UCN_OK);
    CHECK(ucn_i_security_owner_init(&owner, &config) == UCN_OK);
    CHECK(ucn_i_security_owner_destroy(&owner) == UCN_OK);
    CHECK(ucn_i_callback_gate_destroy(&gate) == UCN_OK);
    return 0;
}

static int test_preflight_fail_closed(void)
{
    ucn_i_security_owner_t owner;
    ucn_i_callback_gate_t gate;
    test_lock_t state_lock = {0};
    test_lock_t gate_lock = {0};
    test_provider_t provider;
    ucn_i_security_domain_rule_t rule;
    ucn_i_security_acl_rule_t acl;
    ucn_i_security_config_t config;
    ucn_i_lock_ops_t gate_ops;
    ucn_i_security_candidate_t candidate = candidate_make(0x20U, 2U);
    ucn_i_security_durability_base_t durability =
        durability_make(0x8001U, 2U);
    ucn_i_security_handle_t output;
    ucn_i_security_handle_t before;
    uint8_t proof = 0xC3U;

    memset(&provider, 0, sizeof(provider));
    memset(&owner, 0, sizeof(owner));
    memset(&gate, 0, sizeof(gate));
    provider.expect_proof = proof;
    memset(&rule, 0, sizeof(rule));
    rule.domain_id = durability.domain_id;
    memcpy(rule.peer_principal, candidate.peer.principal,
           sizeof(rule.peer_principal));
    acl = acl_make(&candidate, UCN_I_SECURITY_ACCESS_INBOUND);
    gate_ops = lock_ops(&gate_lock);
    CHECK(ucn_i_callback_gate_init(&gate, 72U, &gate_ops) == UCN_OK);
    config = config_make(&state_lock, &gate, &provider, &rule, 1U,
                         &acl, 1U);
    CHECK(ucn_i_security_owner_init(&owner, &config) == UCN_OK);
    memset(&output, 0xA5, sizeof(output));
    before = output;
    durability.prior_session_generation = 0U;
    CHECK(ucn_i_security_prepare_static_session(
              &owner, &candidate, &proof, 1U, &durability, 1000U,
              &output) == UCN_ERR_STATE);
    CHECK(provider.calls == 0U);
    CHECK(memcmp(&output, &before, sizeof(output)) == 0);

    durability = durability_make(0x9001U, 2U);
    CHECK(ucn_i_security_prepare_static_session(
              &owner, &candidate, &proof, 1U, &durability, 1000U,
              &output) == UCN_ERR_ACCESS);
    CHECK(provider.calls == 0U);
    CHECK(memcmp(&output, &before, sizeof(output)) == 0);
    CHECK(ucn_i_security_owner_destroy(&owner) == UCN_OK);
    CHECK(ucn_i_callback_gate_destroy(&gate) == UCN_OK);
    return 0;
}

static int run_packet_roundtrip(uint8_t origin_level,
                                uint8_t origin_suite,
                                uint8_t hop_profile)
{
    session_fixture_t sender;
    session_fixture_t receiver;
    ucn_i_security_candidate_t sender_candidate = candidate_make(0x20U, 1U);
    ucn_i_security_candidate_t receiver_candidate;
    ucn_i_security_binding_t binding_swap;
    ucn_i_security_key_selector_t selector_swap;
    ucn_i_security_access_request_t send_access;
    ucn_i_security_access_request_t receive_access;
    ucn_i_security_tx_handle_t tx;
    ucn_i_security_c1_origin_material_t material;
    ucn_i_security_packet_workspace_t sender_workspace;
    ucn_i_security_packet_workspace_t receiver_workspace;
    ucn_i_security_replay_handle_t replay;
    ucn_i_security_replay_handle_t replay_before;
    uint8_t plaintext[] = {0x11U, 0x22U, 0x33U, 0x44U};
    uint8_t packet[UCN_ADAPTER_FRAME_BYTES];
    uint8_t opened[sizeof(plaintext)];
    uint8_t opened_before[sizeof(opened)];
    size_t packet_bytes = 0U;
    size_t opened_bytes = 0U;
    size_t opened_bytes_before;

    if (origin_level == UCN_I_SECURITY_CONFIDENTIAL) {
        sender_candidate.origin_level = origin_level;
        sender_candidate.origin_tx.suite_id = origin_suite;
        sender_candidate.origin_rx.suite_id = origin_suite;
    }
    if (hop_profile == UCN_I_HOP_PROFILE_H1) {
        sender_candidate.hop_profile = UCN_I_HOP_PROFILE_H1;
        sender_candidate.hop_tx.suite_id = 16U;
        sender_candidate.hop_tx.key_id = 5U;
        sender_candidate.hop_tx.key_generation = 23U;
        sender_candidate.hop_rx.suite_id = 16U;
        sender_candidate.hop_rx.key_id = 6U;
        sender_candidate.hop_rx.key_generation = 24U;
    }
    receiver_candidate = sender_candidate;
    binding_swap = receiver_candidate.local;
    receiver_candidate.local = receiver_candidate.peer;
    receiver_candidate.peer = binding_swap;
    selector_swap = receiver_candidate.origin_tx;
    receiver_candidate.origin_tx = receiver_candidate.origin_rx;
    receiver_candidate.origin_rx = selector_swap;
    selector_swap = receiver_candidate.hop_tx;
    receiver_candidate.hop_tx = receiver_candidate.hop_rx;
    receiver_candidate.hop_rx = selector_swap;
    CHECK(fixture_activate(&sender, &sender_candidate,
                           UCN_I_SECURITY_ACCESS_OUTBOUND, 51U, 81U) == 0);
    CHECK(fixture_activate(&receiver, &receiver_candidate,
                           UCN_I_SECURITY_ACCESS_INBOUND, 52U, 82U) == 0);

    memset(&send_access, 0, sizeof(send_access));
    send_access.session = sender.handle;
    memcpy(send_access.context_fingerprint,
           sender_candidate.origin_fingerprint,
           sizeof(send_access.context_fingerprint));
    send_access.service_id = sender.acl.service_id;
    send_access.protocol_opcode = sender.acl.protocol_opcode;
    send_access.direction = UCN_I_SECURITY_ACCESS_OUTBOUND;
    CHECK(ucn_i_security_tx_reserve(&sender.owner, &send_access,
                                    &sender.facts, &tx) == UCN_OK);
    memset(&material, 0, sizeof(material));
    material.session = sender.handle;
    material.facts = sender.facts;
    material.payload_bytes = sizeof(plaintext);
    material.source_address = sender_candidate.local.address;
    material.destination_address = sender_candidate.peer.address;
    material.origin_sequence = tx.origin_sequence;
    material.service_id = send_access.service_id;
    material.protocol_opcode = send_access.protocol_opcode;
    material.common_header[0] = 0x61U;
    material.common_header[1] = 0x00U;
    material.common_header[2] =
        (uint8_t)((origin_level << 6U) | 5U);
    material.direction = UCN_I_SECURITY_ACCESS_OUTBOUND;
    memset(packet, 0xA5, sizeof(packet));
    {
        uint8_t packet_before[UCN_ADAPTER_FRAME_BYTES];
        size_t bytes_before = SIZE_MAX;
        size_t required = 11U + sizeof(plaintext) +
                          UCN_I_SECURITY_ORIGIN_TAG_BYTES +
                          (hop_profile == UCN_I_HOP_PROFILE_H1 ?
                               UCN_I_SECURITY_HOP_TRAILER_BYTES : 0U);

        memcpy(packet_before, packet, sizeof(packet));
        CHECK(ucn_i_security_c1_protect(
                  &sender.owner, tx, &material, plaintext,
                  &sender_workspace, packet, required - 1U,
                  &bytes_before) == UCN_ERR_NO_SPACE);
        CHECK(memcmp(packet, packet_before, sizeof(packet)) == 0);
        CHECK(bytes_before == SIZE_MAX);
        CHECK(ucn_i_security_c1_protect(
                  &sender.owner, tx, &material, plaintext,
                  &sender_workspace,
                  ((uint8_t *)&sender_workspace) + 1U, required,
                  &bytes_before) == UCN_ERR_ARGUMENT);
        CHECK(bytes_before == SIZE_MAX);
    }
    CHECK(ucn_i_security_c1_protect(
              &sender.owner, tx, &material, plaintext, &sender_workspace,
              packet, sizeof(packet), &packet_bytes) == UCN_OK);
    CHECK(packet_bytes == 11U + sizeof(plaintext) +
                          UCN_I_SECURITY_ORIGIN_TAG_BYTES +
                          (hop_profile == UCN_I_HOP_PROFILE_H1 ?
                               UCN_I_SECURITY_HOP_TRAILER_BYTES : 0U));
    CHECK(packet[0] == 0x61U && packet[1] == 0x00U &&
          packet[2] == material.common_header[2]);
    CHECK(packet[3] == sender_candidate.local.address &&
          packet[4] == sender_candidate.peer.address);
    CHECK(packet[5] == 0x12U && packet[6] == 0x01U);
    CHECK(packet[7] == 0U && packet[8] == 0U && packet[9] == 0U &&
          packet[10] == 1U);
    if (hop_profile == UCN_I_HOP_PROFILE_H1) {
        size_t hop_offset = packet_bytes - UCN_I_SECURITY_HOP_TRAILER_BYTES;

        CHECK(packet[hop_offset] == 0U && packet[hop_offset + 1U] == 0U &&
              packet[hop_offset + 2U] == 0U &&
              packet[hop_offset + 3U] == 1U);
    }
    if (origin_level == UCN_I_SECURITY_AUTHENTICATED) {
        CHECK(memcmp(packet + 11U, plaintext, sizeof(plaintext)) == 0);
    } else {
        CHECK(memcmp(packet + 11U, plaintext, sizeof(plaintext)) != 0);
    }
    CHECK(ucn_i_security_tx_commit(&sender.owner, tx, &sender.facts) ==
          UCN_ERR_STATE);

    memset(&receive_access, 0, sizeof(receive_access));
    receive_access.session = receiver.handle;
    memcpy(receive_access.context_fingerprint,
           receiver_candidate.origin_fingerprint,
           sizeof(receive_access.context_fingerprint));
    receive_access.service_id = receiver.acl.service_id;
    receive_access.protocol_opcode = receiver.acl.protocol_opcode;
    receive_access.direction = UCN_I_SECURITY_ACCESS_INBOUND;
    memset(opened, 0xA5, sizeof(opened));
    memcpy(opened_before, opened, sizeof(opened));
    memset(&replay, 0xA5, sizeof(replay));
    opened_bytes = SIZE_MAX;
    replay_before = replay;
    opened_bytes_before = opened_bytes;
    receive_access.service_id++;
    CHECK(ucn_i_security_c1_open(
              &receiver.owner, receiver.handle, &receiver.facts,
              &receive_access, packet, packet_bytes, &receiver_workspace,
              opened, sizeof(opened), &opened_bytes, &replay) ==
          UCN_ERR_STATE);
    CHECK(memcmp(opened, opened_before, sizeof(opened)) == 0);
    CHECK(opened_bytes == opened_bytes_before);
    CHECK(memcmp(&replay, &replay_before, sizeof(replay)) == 0);
    receive_access.service_id--;
    CHECK(ucn_i_security_c1_open(
              &receiver.owner, receiver.handle, &receiver.facts,
              &receive_access, packet, packet_bytes, &receiver_workspace,
              receiver_workspace.plaintext + 1U, sizeof(opened),
              &opened_bytes, &replay) == UCN_ERR_ARGUMENT);
    CHECK(opened_bytes == opened_bytes_before);
    CHECK(memcmp(&replay, &replay_before, sizeof(replay)) == 0);
    CHECK(ucn_i_security_c1_open(
              &receiver.owner, receiver.handle, &receiver.facts,
              &receive_access, packet, packet_bytes, &receiver_workspace,
              opened, sizeof(opened), &opened_bytes, &replay) == UCN_OK);
    CHECK(opened_bytes == sizeof(plaintext));
    CHECK(memcmp(opened, plaintext, sizeof(plaintext)) == 0);
    CHECK(ucn_i_security_replay_commit(&receiver.owner, replay,
                                       &receiver.facts) == UCN_OK);

    memset(opened, 0x5A, sizeof(opened));
    memcpy(opened_before, opened, sizeof(opened));
    opened_bytes = SIZE_MAX - 1U;
    opened_bytes_before = opened_bytes;
    memset(&replay, 0x3C, sizeof(replay));
    replay_before = replay;
    CHECK(ucn_i_security_c1_open(
              &receiver.owner, receiver.handle, &receiver.facts,
              &receive_access, packet, packet_bytes, &receiver_workspace,
              opened, sizeof(opened), &opened_bytes, &replay) ==
          UCN_ERR_REPLAY);
    CHECK(memcmp(opened, opened_before, sizeof(opened)) == 0);
    CHECK(opened_bytes == opened_bytes_before);
    CHECK(memcmp(&replay, &replay_before, sizeof(replay)) == 0);

    packet[packet_bytes - 1U] ^= 1U;
    CHECK(ucn_i_security_c1_open(
              &receiver.owner, receiver.handle, &receiver.facts,
              &receive_access, packet, packet_bytes, &receiver_workspace,
              opened, sizeof(opened), &opened_bytes, &replay) ==
          UCN_ERR_SECURITY);
    CHECK(memcmp(opened, opened_before, sizeof(opened)) == 0);
    CHECK(opened_bytes == opened_bytes_before);
    CHECK(memcmp(&replay, &replay_before, sizeof(replay)) == 0);

    if (origin_level == UCN_I_SECURITY_AUTHENTICATED) {
        ucn_i_security_tx_handle_t failed_tx;
        ucn_i_security_tx_handle_t next_tx;
        size_t output_sentinel = SIZE_MAX - 2U;
        uint8_t packet_sentinel[UCN_ADAPTER_FRAME_BYTES];

        CHECK(ucn_i_security_tx_reserve(&sender.owner, &send_access,
                                        &sender.facts, &failed_tx) == UCN_OK);
        CHECK(failed_tx.origin_sequence == 2U);
        material.origin_sequence = failed_tx.origin_sequence;
        memset(packet, 0x6BU, sizeof(packet));
        memcpy(packet_sentinel, packet, sizeof(packet));
        sender.provider.corrupt_o1_payload = 1U;
        CHECK(ucn_i_security_c1_protect(
                  &sender.owner, failed_tx, &material, plaintext,
                  &sender_workspace, packet, sizeof(packet),
                  &output_sentinel) == UCN_ERR_SECURITY);
        sender.provider.corrupt_o1_payload = 0U;
        CHECK(memcmp(packet, packet_sentinel, sizeof(packet)) == 0);
        CHECK(output_sentinel == SIZE_MAX - 2U);
        CHECK(ucn_i_security_tx_commit(&sender.owner, failed_tx,
                                       &sender.facts) == UCN_ERR_STATE);
        CHECK(ucn_i_security_tx_reserve(&sender.owner, &send_access,
                                        &sender.facts, &next_tx) == UCN_OK);
        CHECK(next_tx.origin_sequence == 3U);
        CHECK(ucn_i_security_tx_abort(&sender.owner, next_tx) == UCN_OK);
    }

    CHECK(sender.provider.reentry_checked >=
          (hop_profile == UCN_I_HOP_PROFILE_H1 ? 3U : 2U));
    CHECK(receiver.provider.reentry_checked >=
          (hop_profile == UCN_I_HOP_PROFILE_H1 ? 4U : 3U));

    CHECK(fixture_destroy(&receiver) == 0);
    CHECK(fixture_destroy(&sender) == 0);
    return 0;
}

static int test_packet_roundtrip(void)
{
    CHECK(run_packet_roundtrip(UCN_I_SECURITY_AUTHENTICATED,
                               1U,
                               UCN_I_HOP_PROFILE_H0) == 0);
    CHECK(run_packet_roundtrip(UCN_I_SECURITY_CONFIDENTIAL,
                               2U,
                               UCN_I_HOP_PROFILE_H0) == 0);
    CHECK(run_packet_roundtrip(UCN_I_SECURITY_CONFIDENTIAL,
                               3U,
                               UCN_I_HOP_PROFILE_H0) == 0);
    CHECK(run_packet_roundtrip(UCN_I_SECURITY_AUTHENTICATED,
                               1U,
                               UCN_I_HOP_PROFILE_H1) == 0);
    CHECK(run_packet_roundtrip(UCN_I_SECURITY_CONFIDENTIAL,
                               2U,
                               UCN_I_HOP_PROFILE_H1) == 0);
    CHECK(run_packet_roundtrip(UCN_I_SECURITY_CONFIDENTIAL,
                               3U,
                               UCN_I_HOP_PROFILE_H1) == 0);
    return 0;
}

static int test_session_capacity(void)
{
    ucn_i_security_owner_t owner;
    ucn_i_callback_gate_t gate;
    test_lock_t state_lock = {0};
    test_lock_t gate_lock = {0};
    test_provider_t provider;
    ucn_i_security_domain_rule_t rules[UCN_I_SECURITY_SESSION_COUNT];
    ucn_i_security_acl_rule_t acl;
    ucn_i_security_config_t config;
    ucn_i_lock_ops_t gate_ops;
    ucn_i_security_candidate_t candidates[UCN_I_SECURITY_SESSION_COUNT];
    ucn_i_security_handle_t handles[UCN_I_SECURITY_SESSION_COUNT];
    ucn_i_security_candidate_t overflow_candidate;
    ucn_i_security_durability_base_t durability;
    ucn_i_security_handle_t overflow_handle;
    ucn_i_security_handle_t overflow_before;
    uint16_t index;
    uint8_t proof = 0xC3U;

    memset(&provider, 0, sizeof(provider));
    memset(&owner, 0, sizeof(owner));
    memset(&gate, 0, sizeof(gate));
    provider.expect_proof = proof;
    for (index = 0U; index < UCN_I_SECURITY_SESSION_COUNT; ++index) {
        candidates[index] = candidate_make((uint8_t)(0x20U + index), 1U);
        memset(&rules[index], 0, sizeof(rules[index]));
        rules[index].domain_id = UINT64_C(0x9000) + index;
        memcpy(rules[index].peer_principal,
               candidates[index].peer.principal,
               sizeof(rules[index].peer_principal));
    }
    acl = acl_make(&candidates[0], UCN_I_SECURITY_ACCESS_OUTBOUND);
    gate_ops = lock_ops(&gate_lock);
    CHECK(ucn_i_callback_gate_init(&gate, 91U, &gate_ops) == UCN_OK);
    config = config_make(&state_lock, &gate, &provider, rules,
                         UCN_I_SECURITY_SESSION_COUNT, &acl, 1U);
    CHECK(ucn_i_security_owner_init(&owner, &config) == UCN_OK);
    provider.owner = &owner;
    for (index = 0U; index < UCN_I_SECURITY_SESSION_COUNT; ++index) {
        durability = durability_make(rules[index].domain_id, 1U);
        CHECK(ucn_i_security_prepare_static_session(
                  &owner, &candidates[index], &proof, 1U, &durability,
                  1000U, &handles[index]) == UCN_OK);
    }
    overflow_candidate = candidates[0];
    overflow_candidate.session_generation = 2U;
    durability = durability_make(rules[0].domain_id, 2U);
    memset(&overflow_handle, 0xA5, sizeof(overflow_handle));
    overflow_before = overflow_handle;
    CHECK(ucn_i_security_prepare_static_session(
              &owner, &overflow_candidate, &proof, 1U, &durability,
              1000U, &overflow_handle) == UCN_ERR_NO_SPACE);
    CHECK(memcmp(&overflow_handle, &overflow_before,
                 sizeof(overflow_handle)) == 0);
    for (index = 0U; index < UCN_I_SECURITY_SESSION_COUNT; ++index) {
        CHECK(ucn_i_security_fence(&owner, handles[index]) == UCN_OK);
        CHECK(ucn_i_security_retire_fenced(&owner, handles[index]) ==
              UCN_OK);
    }
    CHECK(ucn_i_security_owner_destroy(&owner) == UCN_OK);
    CHECK(ucn_i_callback_gate_destroy(&gate) == UCN_OK);
    return 0;
}

int main(void)
{
    CHECK(test_record_codec() == 0);
    CHECK(test_session_prepare_and_lifecycle() == 0);
    CHECK(test_preflight_fail_closed() == 0);
    CHECK(test_packet_roundtrip() == 0);
    CHECK(test_session_capacity() == 0);
    puts("ucn simplified security session tests: PASS");
    return 0;
}
