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
    uint32_t protect_origin_calls;
    uint32_t open_origin_calls;
    uint32_t protect_hop_calls;
    uint32_t verify_hop_calls;
    uint8_t expect_proof;
    uint8_t reentry_checked;
    uint8_t reinit_checked;
    uint8_t corrupt_o1_payload;
    const ucn_i_peer_reauth_record_t *reauth_record;
    const ucn_i_security_candidate_t *reauth_candidate;
    uint32_t reauth_verify_calls;
} test_provider_t;

static ucn_result_t reauth_identity_verify(void *, const uint8_t[16], const uint8_t[227], const uint8_t *, uint8_t);

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
        ucn_i_security_acl_rule_t rule;
        memset(&sentinel, 0xA5, sizeof(sentinel));
        before = sentinel;
        if (ucn_i_security_session_get(provider->owner,
                                       provider->invalid_handle,
                                       &sentinel) != UCN_ERR_STATE ||
            memcmp(&sentinel, &before, sizeof(sentinel)) != 0) {
            return UCN_ERR_STATE;
        }
        provider->reentry_checked = 1U;
        if (candidate != NULL) {
            memset(&rule, 0, sizeof(rule));
            memcpy(rule.peer_principal, candidate->peer.principal, 16U);
            memcpy(rule.context_fingerprint, candidate->origin_fingerprint, 16U);
            rule.peer_binding_generation = candidate->peer.binding_generation;
            rule.service_id = 0x1201U; rule.direction = UCN_I_SECURITY_ACCESS_OUTBOUND;
            if (ucn_i_security_bind_session_acl(provider->owner, provider->invalid_handle, &rule, 1U) != UCN_ERR_STATE) { return UCN_ERR_STATE; }
        }
        if (provider->reinit_config != NULL) {
            if (ucn_i_security_owner_init(provider->owner,
                                          provider->reinit_config) !=
                UCN_ERR_STATE) {
                return UCN_ERR_STATE;
            }
            provider->reinit_checked = 1U;
        }
    }
    if (provider->reauth_record != NULL) {
        uint8_t message[227];
        if (candidate == NULL || provider->reauth_candidate == NULL || proof == NULL || proof_bytes != 32U ||
            memcmp(candidate, provider->reauth_candidate, sizeof(*candidate)) != 0) { return UCN_ERR_SECURITY; }
        memcpy(message, "UCN6-PEER-REAUTH-V2", 19U);
        memcpy(message + 19U, provider->reauth_record->canonical, 208U);
        message[20] = 1U;
        if (reauth_identity_verify(NULL, message + 59U, message, proof, 16U) != UCN_OK) { return UCN_ERR_SECURITY; }
        message[20] = 2U;
        return reauth_identity_verify(NULL, message + 35U, message, proof + 16U, 16U);
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

static ucn_result_t reauth_identity_verify(void *context, const uint8_t principal[16],
    const uint8_t message[227], const uint8_t *proof, uint8_t bytes)
{
    uint8_t expected[16];
    ucn_i_security_key_selector_t identity_key;
    test_provider_t *provider = context;
    memset(&identity_key, 0, sizeof(identity_key)); identity_key.suite_id = 1U;
    identity_key.key_id = (uint16_t)(principal[0] + 1U); identity_key.key_generation = 1U;
    if (provider != NULL) {
        bool active = false, fenced = false;
        ucn_i_callback_claim_t claim;
        ++provider->reauth_verify_calls;
        if (ucn_i_callback_gate_view(provider->owner->provider_gate, &active, &claim) != UCN_OK || !active ||
            ucn_i_security_peer_is_fenced(provider->owner, &provider->reauth_candidate->local,
                &provider->reauth_candidate->peer, &fenced) != UCN_ERR_STATE || fenced) { return UCN_ERR_STATE; }
    }
    test_tag(&identity_key, message, 227U, principal, 16U, expected);
    return bytes == 16U && memcmp(expected, proof, 16U) == 0 ? UCN_OK : UCN_ERR_SECURITY;
}
static void reauth_identity_sign(const uint8_t principal[16], const uint8_t message[227], uint8_t proof[16])
{
    ucn_i_security_key_selector_t identity_key;
    memset(&identity_key, 0, sizeof(identity_key)); identity_key.suite_id = 1U;
    identity_key.key_id = (uint16_t)(principal[0] + 1U); identity_key.key_generation = 1U;
    test_tag(&identity_key, message, 227U, principal, 16U, proof);
}

static void test_write_be(uint8_t *output, uint64_t value, uint8_t bytes)
{
    uint8_t index;

    for (index = 0U; index < bytes; ++index) {
        const uint8_t shift = (uint8_t)((bytes - index - 1U) * 8U);

        output[index] = (uint8_t)(value >> shift);
    }
}

static void test_c0_nonce_input(
    const ucn_i_security_candidate_t *candidate,
    const ucn_i_c0_frame_t *frame,
    uint8_t direction,
    uint8_t output[UCN_I_SECURITY_SEQUENCE_NONCE_INPUT_BYTES])
{
    static const uint8_t domain[] = "UCN6-NONCE-C0-V1";
    size_t offset = 0U;

    memcpy(output + offset, domain, sizeof(domain) - 1U);
    offset += sizeof(domain) - 1U;
    memcpy(output + offset, candidate->origin_fingerprint,
           UCN_I_SECURITY_FINGERPRINT_BYTES);
    offset += UCN_I_SECURITY_FINGERPRINT_BYTES;
    test_write_be(output + offset, frame->transaction_id, 8U);
    offset += 8U;
    test_write_be(output + offset, frame->opcode, 2U);
    offset += 2U;
    output[offset++] = (uint8_t)((direction == UCN_I_SECURITY_ACCESS_OUTBOUND ?
        memcmp(candidate->local.principal, candidate->peer.principal, 16U) :
        memcmp(candidate->peer.principal, candidate->local.principal, 16U)) < 0 ? 0U : 1U);
    output[offset] = frame->interaction_role;
}

static size_t test_hop_aad(
    const uint8_t *packet_before_hop,
    size_t packet_before_hop_bytes,
    uint32_t hop_sequence,
    const uint8_t fingerprint[UCN_I_SECURITY_FINGERPRINT_BYTES],
    uint8_t output[UCN_I_SECURITY_HOP_AAD_BYTES])
{
    static const uint8_t domain[] = "UCN6-HOP-V1";
    size_t offset = 0U;

    memcpy(output + offset, domain, sizeof(domain) - 1U);
    offset += sizeof(domain) - 1U;
    test_write_be(output + offset,
                  packet_before_hop_bytes + 4U +
                      UCN_I_SECURITY_FINGERPRINT_BYTES,
                  4U);
    offset += 4U;
    memcpy(output + offset, packet_before_hop, packet_before_hop_bytes);
    offset += packet_before_hop_bytes;
    test_write_be(output + offset, hop_sequence, 4U);
    offset += 4U;
    memcpy(output + offset, fingerprint,
           UCN_I_SECURITY_FINGERPRINT_BYTES);
    offset += UCN_I_SECURITY_FINGERPRINT_BYTES;
    return offset;
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
    provider->protect_origin_calls++;
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
        (request->aad_bytes != UCN_I_SECURITY_C1_ORIGIN_AAD_BYTES &&
         (request->aad_bytes < 50U ||
          request->aad_bytes > UCN_I_C0_ORIGIN_AAD_MAX_BYTES))) {
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
    provider->open_origin_calls++;
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
        (request->aad_bytes != UCN_I_SECURITY_C1_ORIGIN_AAD_BYTES &&
         (request->aad_bytes < 50U ||
          request->aad_bytes > UCN_I_C0_ORIGIN_AAD_MAX_BYTES))) {
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
    provider->protect_hop_calls++;
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
    provider->verify_hop_calls++;
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

static int pending_policy_rejects_invalid_without_writes(session_fixture_t *f,
    const ucn_i_security_acl_rule_t *grant)
{
    static ucn_i_security_owner_t before;
    ucn_i_security_acl_rule_t bad[2];
    ucn_i_security_handle_t foreign = f->handle;
    ucn_i_security_candidate_t output, output_before;
    unsigned i;
    before = f->owner; foreign.owner_instance++;
    {
        const ucn_i_callback_claim_t claim = {99U, 100U, 1U, 1U};
        CHECK(ucn_i_callback_gate_enter(&f->gate, &claim) == UCN_OK);
        CHECK(ucn_i_security_bind_session_acl(&f->owner, f->handle, grant, 1U) == UCN_ERR_STATE);
        CHECK(ucn_i_callback_gate_leave(&f->gate, &claim) == UCN_OK);
        CHECK(memcmp(&f->owner, &before, sizeof(before)) == 0);
    }
    CHECK(ucn_i_security_bind_session_acl(&f->owner, foreign, grant, 1U) == UCN_ERR_STATE);
    CHECK(ucn_i_security_bind_session_acl(&f->owner, f->handle, NULL, 1U) == UCN_ERR_ARGUMENT);
    CHECK(ucn_i_security_bind_session_acl(&f->owner, f->handle, grant, 0U) == UCN_ERR_ARGUMENT);
    CHECK(ucn_i_security_bind_session_acl(&f->owner, f->handle, grant, UCN_I_SECURITY_SESSION_ACL_MAX + 1U) == UCN_ERR_ARGUMENT);
    CHECK(ucn_i_security_bind_session_acl(&f->owner, f->handle, (const ucn_i_security_acl_rule_t *)&f->owner, 1U) == UCN_ERR_ARGUMENT);
    for (i = 0U; i < 8U; ++i) {
        bad[0] = *grant;
        switch (i) {
        case 0U: bad[0].peer_principal[0] ^= 1U; break;
        case 1U: ++bad[0].peer_binding_generation; break;
        case 2U: bad[0].context_fingerprint[0] ^= 1U; break;
        case 3U: bad[0].protocol_opcode = UINT16_MAX; break;
        case 4U: bad[0].direction = 3U; break;
        case 5U: bad[0].reserved_zero[0] = 1U; break;
        case 6U: bad[0].service_id = UINT16_MAX; break;
        default: bad[0].service_id = 0U; bad[0].protocol_opcode = 0U; break;
        }
        CHECK(ucn_i_security_bind_session_acl(&f->owner, f->handle, bad, 1U) == UCN_ERR_ARGUMENT);
        CHECK(memcmp(&f->owner, &before, sizeof(before)) == 0);
    }
    bad[0] = bad[1] = *grant;
    CHECK(ucn_i_security_bind_session_acl(&f->owner, f->handle, bad, 2U) == UCN_ERR_ARGUMENT);
    memset(&output, 0xA5, sizeof(output)); output_before = output;
    CHECK(ucn_i_security_session_candidate_get(&f->owner, f->handle, &output) == UCN_ERR_STATE);
    CHECK(memcmp(&output, &output_before, sizeof(output)) == 0);
    CHECK(memcmp(&f->owner, &before, sizeof(before)) == 0);
    return 0;
}

static int fixture_activate_profile_policy(session_fixture_t *fixture,
                            const ucn_i_security_candidate_t *candidate,
                            uint8_t direction,
                            uint32_t runtime_instance,
                            uint16_t gate_owner,
                            uint16_t service_id,
                            uint16_t protocol_opcode,
                            uint32_t realm_id, uint8_t address_width,
                            const ucn_i_security_acl_rule_t *session_acl,
                            uint16_t session_acl_count,
                            const ucn_i_security_acl_rule_t *initial_acl)
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
    fixture->acl.service_id = service_id;
    fixture->acl.protocol_opcode = protocol_opcode;
    if (initial_acl != NULL) { fixture->acl = *initial_acl; }
    /* Exercise the supported shared non-recursive lock arrangement for hot policy. */
    gate_ops = lock_ops(session_acl != NULL ? &fixture->state_lock : &fixture->gate_lock);
    CHECK(ucn_i_callback_gate_init(&fixture->gate, gate_owner,
                                   &gate_ops) == UCN_OK);
    fixture->config = config_make(&fixture->state_lock, &fixture->gate,
                                  &fixture->provider, &fixture->rule, 1U,
                                  &fixture->acl, 1U);
    fixture->config.runtime_instance = runtime_instance;
    fixture->config.realm_id = realm_id;
    fixture->config.address_width = address_width;
    CHECK(ucn_i_security_owner_init(&fixture->owner,
                                    &fixture->config) == UCN_OK);
    fixture->provider.owner = &fixture->owner;
    CHECK(ucn_i_security_prepare_static_session(
              &fixture->owner, &fixture->candidate, &session_proof, 1U,
              &durability, 1000U, &fixture->handle) == UCN_OK);
    if (session_acl != NULL) {
        static ucn_i_security_owner_t before;
        CHECK(pending_policy_rejects_invalid_without_writes(fixture, session_acl) == 0);
        CHECK(ucn_i_security_bind_session_acl(&fixture->owner, fixture->handle,
            session_acl, session_acl_count) == UCN_OK);
        before = fixture->owner;
        CHECK(ucn_i_security_bind_session_acl(&fixture->owner, fixture->handle,
            session_acl, session_acl_count) == UCN_ERR_STATE);
        CHECK(memcmp(&fixture->owner, &before, sizeof(before)) == 0);
    }
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

static int fixture_activate_profile(session_fixture_t *fixture,
    const ucn_i_security_candidate_t *candidate, uint8_t direction,
    uint32_t runtime_instance, uint16_t gate_owner, uint16_t service_id,
    uint16_t protocol_opcode, uint32_t realm_id, uint8_t address_width)
{
    return fixture_activate_profile_policy(fixture, candidate, direction,
        runtime_instance, gate_owner, service_id, protocol_opcode,
        realm_id, address_width, NULL, 0U, NULL);
}

static int fixture_activate(session_fixture_t *fixture,
    const ucn_i_security_candidate_t *candidate, uint8_t direction,
    uint32_t runtime_instance, uint16_t gate_owner, uint16_t service_id,
    uint16_t protocol_opcode)
{
    return fixture_activate_profile(fixture, candidate, direction, runtime_instance,
        gate_owner, service_id, protocol_opcode, UINT32_C(0x12345678), 1U);
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

typedef struct dual_session_fixture {
    ucn_i_security_owner_t owner;
    ucn_i_callback_gate_t gate;
    test_lock_t state_lock;
    test_lock_t gate_lock;
    test_provider_t provider;
    ucn_i_security_domain_rule_t rules[2];
    ucn_i_security_acl_rule_t acl[2];
    ucn_i_security_config_t config;
    ucn_i_security_candidate_t candidates[2];
    ucn_i_security_handle_t handles[2];
    ucn_i_security_current_facts_t facts[2];
} dual_session_fixture_t;

static int dual_session_activate_one(dual_session_fixture_t *fixture,
                                     uint16_t index,
                                     uint64_t domain_id)
{
    ucn_i_security_durability_base_t durability = durability_make(
        domain_id, fixture->candidates[index].session_generation);
    ucn_i_security_requirement_view_t requirement;
    ucn_i_security_durability_proof_t proof;
    ucn_handle_t persistence_handle;
    uint8_t published_digest[16];
    uint8_t session_proof = 0xC3U;

    CHECK(ucn_i_security_prepare_static_session(
              &fixture->owner, &fixture->candidates[index],
              &session_proof, 1U, &durability, 1000U,
              &fixture->handles[index]) == UCN_OK);
    CHECK(ucn_i_security_requirement_get(
              &fixture->owner, fixture->handles[index],
              &requirement) == UCN_OK);
    memset(&persistence_handle, 0, sizeof(persistence_handle));
    persistence_handle.runtime_instance = fixture->config.runtime_instance;
    persistence_handle.owner_instance = 8U;
    persistence_handle.slot = (uint16_t)(index + 1U);
    persistence_handle.generation = (uint32_t)(index + 2U);
    persistence_handle.object_kind = UCN_OBJECT_KIND_PERSISTENCE;
    fill_bytes(published_digest, sizeof(published_digest),
               (uint8_t)(0xD1U + index));
    CHECK(ucn_i_security_bind_persistence(
              &fixture->owner, fixture->handles[index], persistence_handle,
              published_digest) == UCN_OK);
    memset(&proof, 0, sizeof(proof));
    proof.persistence_handle = persistence_handle;
    proof.domain_id = requirement.domain_id;
    proof.record_generation = requirement.expected_record_generation + 1U;
    proof.foundation_transaction_id = requirement.foundation_transaction_id;
    proof.witness_generation = proof.record_generation;
    proof.transition_fingerprint = requirement.transition_fingerprint;
    proof.runtime_instance = fixture->config.runtime_instance;
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
    fixture->facts[index].local = fixture->candidates[index].local;
    fixture->facts[index].peer = fixture->candidates[index].peer;
    fixture->facts[index].now_us = 1500U;
    fixture->facts[index].link_generation =
        fixture->candidates[index].link_generation;
    fixture->facts[index].policy_generation =
        fixture->candidates[index].policy_generation;
    CHECK(ucn_i_security_activate(
              &fixture->owner, fixture->handles[index], &proof,
              &fixture->facts[index]) == UCN_OK);
    return 0;
}

static int dual_session_fixture_activate(
    dual_session_fixture_t *fixture,
    const ucn_i_security_candidate_t candidates[2],
    const uint8_t directions[2],
    uint32_t runtime_instance,
    uint16_t gate_owner,
    uint16_t protocol_opcode)
{
    ucn_i_lock_ops_t gate_ops;
    uint16_t index;

    memset(fixture, 0, sizeof(*fixture));
    fixture->provider.expect_proof = 0xC3U;
    for (index = 0U; index < 2U; ++index) {
        fixture->candidates[index] = candidates[index];
        fixture->rules[index].domain_id = UINT64_C(0x8100) + index;
        memcpy(fixture->rules[index].peer_principal,
               candidates[index].peer.principal,
               sizeof(fixture->rules[index].peer_principal));
        fixture->acl[index] = acl_make(&candidates[index],
                                       directions[index]);
        fixture->acl[index].service_id = 0U;
        fixture->acl[index].protocol_opcode = protocol_opcode;
        if (candidates[index].hop_profile == UCN_I_HOP_PROFILE_H1) {
            memcpy(fixture->acl[index].context_fingerprint,
                   candidates[index].hop_fingerprint,
                   sizeof(fixture->acl[index].context_fingerprint));
        }
    }
    gate_ops = lock_ops(&fixture->gate_lock);
    CHECK(ucn_i_callback_gate_init(&fixture->gate, gate_owner,
                                   &gate_ops) == UCN_OK);
    fixture->config = config_make(
        &fixture->state_lock, &fixture->gate, &fixture->provider,
        fixture->rules, 2U, fixture->acl, 2U);
    fixture->config.runtime_instance = runtime_instance;
    CHECK(ucn_i_security_owner_init(&fixture->owner,
                                    &fixture->config) == UCN_OK);
    fixture->provider.owner = &fixture->owner;
    CHECK(dual_session_activate_one(fixture, 0U,
                                    fixture->rules[0].domain_id) == 0);
    CHECK(dual_session_activate_one(fixture, 1U,
                                    fixture->rules[1].domain_id) == 0);
    return 0;
}

static int dual_session_fixture_destroy(dual_session_fixture_t *fixture)
{
    uint16_t index;

    for (index = 0U; index < 2U; ++index) {
        CHECK(ucn_i_security_fence(&fixture->owner,
                                   fixture->handles[index]) == UCN_OK);
        CHECK(ucn_i_security_retire_fenced(
                  &fixture->owner, fixture->handles[index]) == UCN_OK);
    }
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
    {
        static ucn_i_security_owner_t owner_before;
        ucn_i_security_current_facts_t borrowed_facts;
        const ucn_i_security_replay_handle_t *alias=(const ucn_i_security_replay_handle_t *)&owner;
        const ucn_i_security_hop_replay_handle_t *hop_alias=(const ucn_i_security_hop_replay_handle_t *)&owner;
        test_lock_t lock_before=state_lock;
        owner_before=owner; memset(&borrowed_facts,0,sizeof(borrowed_facts));
        CHECK(ucn_i_security_replay_commit(&owner,NULL,&borrowed_facts)==UCN_ERR_ARGUMENT);
        CHECK(ucn_i_security_control_replay_commit(&owner,NULL,&borrowed_facts)==UCN_ERR_ARGUMENT);
        CHECK(ucn_i_security_control_replay_complete(&owner,NULL,&borrowed_facts)==UCN_ERR_ARGUMENT);
        CHECK(ucn_i_security_replay_abort(&owner,NULL)==UCN_ERR_ARGUMENT);
        CHECK(ucn_i_security_hop_replay_commit(&owner,NULL,&borrowed_facts)==UCN_ERR_ARGUMENT);
        CHECK(ucn_i_security_hop_replay_abort(&owner,NULL)==UCN_ERR_ARGUMENT);
        CHECK(ucn_i_security_replay_commit(&owner,alias,&borrowed_facts)==UCN_ERR_ARGUMENT);
        CHECK(ucn_i_security_control_replay_commit(&owner,alias,&borrowed_facts)==UCN_ERR_ARGUMENT);
        CHECK(ucn_i_security_control_replay_complete(&owner,alias,&borrowed_facts)==UCN_ERR_ARGUMENT);
        CHECK(ucn_i_security_replay_abort(&owner,alias)==UCN_ERR_ARGUMENT);
        CHECK(ucn_i_security_hop_replay_commit(&owner,hop_alias,&borrowed_facts)==UCN_ERR_ARGUMENT);
        CHECK(ucn_i_security_hop_replay_abort(&owner,hop_alias)==UCN_ERR_ARGUMENT);
        CHECK(memcmp(&owner,&owner_before,sizeof(owner))==0);
        CHECK(memcmp(&state_lock,&lock_before,sizeof(state_lock))==0);
    }
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
    CHECK(ucn_i_security_replay_commit(&owner, &(replay2), &facts) ==
          UCN_ERR_STATE);
    facts.now_us = 1550U;
    CHECK(ucn_i_security_replay_commit(&owner, &(replay), &facts) == UCN_OK);
    CHECK(ucn_i_security_replay_reserve(&owner, handle, &facts, 10U,
                                        aad_digest, payload_digest,
                                        &replay2) == UCN_ERR_REPLAY);
    CHECK(ucn_i_security_replay_reserve(&owner, handle, &facts, 12U,
                                        aad_digest, payload_digest,
                                        &replay2) == UCN_OK);
    CHECK(ucn_i_security_replay_abort(&owner, &(replay2)) == UCN_OK);
    CHECK(ucn_i_security_replay_reserve(&owner, handle, &facts, 13U,
                                        aad_digest, payload_digest,
                                        &replay2) == UCN_OK);
    CHECK(ucn_i_security_replay_maintain(&owner, 1650U, 1U,
                                         &inspected, &expired) == UCN_OK);
    while (expired == 0U) {
        CHECK(ucn_i_security_replay_maintain(&owner, 1650U, 1U,
                                             &inspected, &expired) == UCN_OK);
    }
    CHECK(ucn_i_security_replay_abort(&owner, &(replay2)) == UCN_ERR_STATE);

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
                           UCN_I_SECURITY_ACCESS_OUTBOUND, 51U, 81U,
                           0x1201U, 0U) == 0);
    CHECK(fixture_activate(&receiver, &receiver_candidate,
                           UCN_I_SECURITY_ACCESS_INBOUND, 52U, 82U,
                           0x1201U, 0U) == 0);

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
    CHECK(ucn_i_security_replay_commit(&receiver.owner, &(replay),
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

/* A single real H1 Session can project Origin without changing its Hop profile.
 * 中文：真实H1 Session独立处理Origin，既不伪造H0 Session，也不消耗Hop序号。 */
static int run_c1_origin_projection(uint8_t level, uint8_t suite)
{
    session_fixture_t sender;
    session_fixture_t receiver;
    ucn_i_security_candidate_t sc = candidate_make(0x20U, 1U);
    ucn_i_security_candidate_t rc;
    ucn_i_security_binding_t binding;
    ucn_i_security_key_selector_t key;
    ucn_i_security_access_request_t send_access;
    ucn_i_security_access_request_t receive_access;
    ucn_i_security_tx_handle_t tx;
    ucn_i_security_c1_origin_material_t material;
    ucn_i_security_packet_workspace_t sw;
    ucn_i_security_packet_workspace_t rw;
    ucn_i_security_replay_handle_t replay;
    ucn_i_security_replay_handle_t sentinel;
    ucn_i_security_session_view_t view;
    const uint8_t payload[4] = {1U, 2U, 3U, 4U};
    uint8_t packet[UCN_ADAPTER_FRAME_BYTES];
    uint8_t output[4];
    size_t bytes = 0U;
    size_t opened = SIZE_MAX;

    sc.origin_level = level;
    sc.origin_tx.suite_id = suite;
    sc.origin_rx.suite_id = suite;
    sc.hop_profile = UCN_I_HOP_PROFILE_H1;
    sc.hop_tx.suite_id = 16U;
    sc.hop_tx.key_id = 5U;
    sc.hop_tx.key_generation = 23U;
    sc.hop_rx.suite_id = 16U;
    sc.hop_rx.key_id = 6U;
    sc.hop_rx.key_generation = 24U;
    rc = sc;
    binding = rc.local; rc.local = rc.peer; rc.peer = binding;
    key = rc.origin_tx; rc.origin_tx = rc.origin_rx; rc.origin_rx = key;
    key = rc.hop_tx; rc.hop_tx = rc.hop_rx; rc.hop_rx = key;
    CHECK(fixture_activate(&sender, &sc, UCN_I_SECURITY_ACCESS_OUTBOUND,
                           51U, 81U, 0x1201U, 0U) == 0);
    CHECK(fixture_activate(&receiver, &rc, UCN_I_SECURITY_ACCESS_INBOUND,
                           52U, 82U, 0x1201U, 0U) == 0);
    memset(&send_access, 0, sizeof(send_access));
    send_access.session = sender.handle;
    memcpy(send_access.context_fingerprint, sc.origin_fingerprint, 16U);
    send_access.service_id = 0x1201U;
    send_access.direction = UCN_I_SECURITY_ACCESS_OUTBOUND;
    CHECK(ucn_i_security_tx_reserve(&sender.owner, &send_access,
                                    &sender.facts, &tx) == UCN_OK);
    memset(&material, 0, sizeof(material));
    material.session = sender.handle;
    material.facts = sender.facts;
    material.payload_bytes = sizeof(payload);
    material.source_address = sc.local.address;
    material.destination_address = sc.peer.address;
    material.origin_sequence = tx.origin_sequence;
    material.service_id = send_access.service_id;
    material.common_header[0] = 0x61U;
    material.common_header[2] = (uint8_t)((level << 6U) | 5U);
    material.direction = UCN_I_SECURITY_ACCESS_OUTBOUND;
    memset(packet, 0xA5, sizeof(packet));
    bytes = SIZE_MAX;
    CHECK(ucn_i_security_c1_origin_protect(&sender.owner, tx, &material,
              payload, &sw, packet, 11U + sizeof(payload) + 15U,
              &bytes) == UCN_ERR_NO_SPACE);
    CHECK(bytes == SIZE_MAX);
    {
        size_t i;
        for (i = 0U; i < sizeof(packet); ++i) { CHECK(packet[i] == 0xA5U); }
    }
    CHECK(ucn_i_security_c1_origin_protect(&sender.owner, tx, &material,
              payload, &sw, packet, sizeof(packet), &bytes) == UCN_OK);
    CHECK(bytes == 11U + sizeof(payload) + 16U);
    CHECK(ucn_i_security_session_get(&sender.owner, sender.handle, &view) == UCN_OK);
    CHECK(view.hop_profile == UCN_I_HOP_PROFILE_H1);
    CHECK(sender.owner.sessions[sender.handle.slot].hop_tx_highest_reserved == 0U);

    receive_access = send_access;
    receive_access.session = receiver.handle;
    receive_access.direction = UCN_I_SECURITY_ACCESS_INBOUND;
    memset(output, 0xA5, sizeof(output));
    memset(&replay, 0xA5, sizeof(replay));
    sentinel = replay;
    /* Complete H1 open must not silently accept a stripped Origin projection. */
    CHECK(ucn_i_security_c1_open(&receiver.owner, receiver.handle,
              &receiver.facts, &receive_access, packet, bytes, &rw,
              output, sizeof(output), &opened, &replay) == UCN_ERR_MALFORMED);
    CHECK(opened == SIZE_MAX && output[0] == 0xA5U && output[3] == 0xA5U);
    CHECK(memcmp(&replay, &sentinel, sizeof(replay)) == 0);
    receive_access.context_fingerprint[0] ^= 1U;
    CHECK(ucn_i_security_c1_origin_open(&receiver.owner, receiver.handle,
              &receiver.facts, &receive_access, packet, bytes, &rw,
              output, sizeof(output), &opened, &replay) == UCN_ERR_STATE);
    CHECK(opened == SIZE_MAX && memcmp(&replay, &sentinel, sizeof(replay)) == 0);
    receive_access.context_fingerprint[0] ^= 1U;
    {
        ucn_i_security_c1_open_call_t call, before;
        static ucn_i_security_owner_t owner_before;
        ucn_i_security_packet_workspace_t work_before;
        memset(&call, 0, sizeof(call));
        call.session = receiver.handle; call.facts = &receiver.facts;
        call.access = &receive_access; call.packet = packet; call.packet_bytes = bytes;
        call.plaintext_output = output; call.plaintext_capacity = sizeof(output);
        call.plaintext_bytes = &opened; call.replay_out = &replay; call.origin_only = 2U;
        owner_before = receiver.owner; work_before = rw;
        CHECK(ucn_i_security_c1_open_borrowed(&receiver.owner, &call, &rw) == UCN_ERR_ARGUMENT);
        CHECK(ucn_i_security_c1_open_borrowed(&receiver.owner, NULL, &rw) == UCN_ERR_ARGUMENT);
        call.origin_only = 1U;
        call.plaintext_output = (uint8_t *)&call + 1U;
        before = call;
        CHECK(ucn_i_security_c1_open_borrowed(&receiver.owner, &call, &rw) == UCN_ERR_ARGUMENT);
        CHECK(memcmp(&call, &before, sizeof(call)) == 0);
        call.plaintext_output = output;
        call.plaintext_bytes = (size_t *)&call;
        before = call;
        CHECK(ucn_i_security_c1_open_borrowed(&receiver.owner, &call, &rw) == UCN_ERR_ARGUMENT);
        CHECK(memcmp(&call, &before, sizeof(call)) == 0);
        CHECK(memcmp(&receiver.owner, &owner_before, sizeof(owner_before)) == 0);
        CHECK(memcmp(&rw, &work_before, sizeof(rw)) == 0);
        CHECK(opened == SIZE_MAX && memcmp(&replay, &sentinel, sizeof(replay)) == 0);
        CHECK(output[0] == 0xA5U && output[sizeof(output)-1U] == 0xA5U);
    }
    packet[bytes - 1U] ^= 1U;
    CHECK(ucn_i_security_c1_origin_open(&receiver.owner, receiver.handle,
              &receiver.facts, &receive_access, packet, bytes, &rw,
              output, sizeof(output), &opened, &replay) == UCN_ERR_SECURITY);
    CHECK(opened == SIZE_MAX && memcmp(&replay, &sentinel, sizeof(replay)) == 0);
    {
        const uint8_t expected[4] = {0xA5U, 0xA5U, 0xA5U, 0xA5U};
        CHECK(memcmp(output, expected, sizeof(output)) == 0);
    }
    packet[bytes - 1U] ^= 1U;
    CHECK(ucn_i_security_c1_origin_open(&receiver.owner, receiver.handle,
              &receiver.facts, &receive_access, packet, bytes, &rw,
              output, sizeof(output), &opened, &replay) == UCN_OK);
    CHECK(opened == sizeof(payload) && memcmp(output, payload, sizeof(payload)) == 0);
    CHECK(ucn_i_security_replay_commit(&receiver.owner, &(replay),
                                       &receiver.facts) == UCN_OK);
    CHECK(ucn_i_security_c1_origin_open(&receiver.owner, receiver.handle,
              &receiver.facts, &receive_access, packet, bytes, &rw,
              output, sizeof(output), &opened, &replay) == UCN_ERR_REPLAY);
    CHECK(ucn_i_security_session_get(&receiver.owner, receiver.handle, &view) == UCN_OK);
    CHECK(view.hop_profile == UCN_I_HOP_PROFILE_H1);
    /* Interleave the complete path: Origin continues at 2, Hop starts at 1. */
    CHECK(ucn_i_security_tx_reserve(&sender.owner, &send_access,
                                    &sender.facts, &tx) == UCN_OK);
    CHECK(tx.origin_sequence == 2U);
    material.origin_sequence = tx.origin_sequence;
    CHECK(ucn_i_security_c1_protect(&sender.owner, tx, &material, payload,
              &sw, packet, sizeof(packet), &bytes) == UCN_OK);
    CHECK(bytes == 11U + sizeof(payload) + 32U);
    CHECK(packet[bytes - 13U] == 1U);
    CHECK(ucn_i_security_c1_open(&receiver.owner, receiver.handle,
              &receiver.facts, &receive_access, packet, bytes, &rw,
              output, sizeof(output), &opened, &replay) == UCN_OK);
    CHECK(opened == sizeof(payload) && memcmp(output, payload, sizeof(payload)) == 0);
    CHECK(ucn_i_security_replay_commit(&receiver.owner, &(replay),
                                       &receiver.facts) == UCN_OK);
    CHECK(fixture_destroy(&receiver) == 0);
    CHECK(fixture_destroy(&sender) == 0);
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

static int test_c0_authenticated_control_fact(void)
{
    session_fixture_t receiver;
    ucn_i_security_candidate_t candidate = candidate_make(0x20U, 1U);
    ucn_i_security_control_access_request_t access;
    ucn_i_security_packet_workspace_t workspace;
    ucn_i_security_authenticated_control_t fact;
    ucn_i_security_authenticated_control_t fact_before;
    ucn_i_security_control_validate_workspace_t validate_workspace;
    ucn_i_c0_frame_t frame;
    uint8_t plaintext[] = {0x11U, 0x22U, 0x33U};
    uint8_t opened[sizeof(plaintext)];
    uint8_t opened_before[sizeof(opened)];
    uint8_t origin_tag[UCN_I_C0_ORIGIN_TAG_BYTES];
    uint8_t aad[UCN_I_C0_ORIGIN_AAD_MAX_BYTES];
    uint8_t packet[UCN_ADAPTER_FRAME_BYTES];
    size_t aad_bytes = 0U;
    size_t packet_bytes = 0U;
    size_t opened_bytes = SIZE_MAX;
    ucn_i_security_replay_handle_t origin_replay;
    uint8_t origin_digest[16];

    CHECK(fixture_activate(&receiver, &candidate,
                           UCN_I_SECURITY_ACCESS_INBOUND, 61U, 91U,
                           0U, UCN_I_OPCODE_DIAGNOSTIC_PING) == 0);
    /* Same integer belongs to independent C1 Origin and C0 message domains. */
    memset(origin_digest, 0x77, sizeof(origin_digest));
    CHECK(ucn_i_security_replay_reserve(&receiver.owner, receiver.handle,
        &receiver.facts, 1U, origin_digest, origin_digest,
        &origin_replay) == UCN_OK);
    CHECK(ucn_i_security_replay_commit(&receiver.owner, &(origin_replay),
        &receiver.facts) == UCN_OK);
    memset(&frame, 0, sizeof(frame));
    frame.payload = plaintext;
    frame.payload_bytes = sizeof(plaintext);
    frame.origin_tag = origin_tag;
    frame.transaction_id = 1U;
    frame.realm_id = receiver.config.realm_id;
    frame.source_address = candidate.peer.address;
    frame.destination_address = candidate.local.address;
    frame.source_binding_generation = candidate.peer.binding_generation;
    frame.destination_binding_generation = candidate.local.binding_generation;
    frame.opcode = UCN_I_OPCODE_DIAGNOSTIC_PING;
    frame.traffic_class = UCN_TRAFFIC_Q0;
    frame.delivery_guarantee = UCN_DELIVERY_BEST_EFFORT;
    frame.interaction_role = UCN_INTERACTION_REQUEST;
    frame.payload_kind = UCN_I_PAYLOAD_DIAGNOSTIC;
    frame.origin_security = UCN_I_ORIGIN_O1;
    frame.hop_limit = 1U;
    CHECK(ucn_i_c0_origin_aad_build(&frame, candidate.address_width,
                                     aad, &aad_bytes) == UCN_OK);
    test_tag(&candidate.origin_rx, aad, aad_bytes, plaintext,
             sizeof(plaintext), origin_tag);
    CHECK(ucn_i_c0_encode(&frame, candidate.address_width,
                           UCN_I_CONTROL_H0, packet, sizeof(packet),
                           &packet_bytes) == UCN_OK);
    memset(&access, 0, sizeof(access));
    access.session = receiver.handle;
    memcpy(access.context_fingerprint, candidate.origin_fingerprint,
           sizeof(access.context_fingerprint));
    access.protocol_opcode = UCN_I_OPCODE_DIAGNOSTIC_PING;
    access.direction = UCN_I_SECURITY_ACCESS_INBOUND;
    memset(opened, 0xA5, sizeof(opened));
    memset(&fact, 0xA5, sizeof(fact));
    CHECK(ucn_i_security_c0_open(
              &receiver.owner, &access, &receiver.facts, packet,
              packet_bytes, &workspace, opened, sizeof(opened),
              &opened_bytes, &fact) == UCN_OK);
    CHECK(opened_bytes == sizeof(plaintext));
    CHECK(memcmp(opened, plaintext, sizeof(opened)) == 0);
    CHECK(fact.body == opened && fact.body_bytes == sizeof(opened));
    CHECK(fact.protocol_opcode == UCN_I_OPCODE_DIAGNOSTIC_PING);
    CHECK(fact.transaction_id == frame.transaction_id);
    CHECK(fact.replay.replay_domain == 1U);
    CHECK(ucn_i_security_control_fact_validate(
              &receiver.owner, &fact, &receiver.facts,
              &validate_workspace) == UCN_OK);
    opened[0] ^= 1U;
    CHECK(ucn_i_security_control_fact_validate(
              &receiver.owner, &fact, &receiver.facts,
              &validate_workspace) == UCN_ERR_SECURITY);
    opened[0] ^= 1U;
    CHECK(ucn_i_security_replay_commit(&receiver.owner, &(fact.replay),
                                       &receiver.facts) == UCN_OK);
    CHECK(ucn_i_security_control_fact_validate(
              &receiver.owner, &fact, &receiver.facts,
              &validate_workspace) == UCN_ERR_STATE);

    frame.origin_security = UCN_I_ORIGIN_O0;
    frame.origin_tag = NULL;
    CHECK(ucn_i_c0_encode(&frame, candidate.address_width,
                           UCN_I_CONTROL_H0, packet, sizeof(packet),
                           &packet_bytes) == UCN_OK);
    memset(opened, 0x5A, sizeof(opened));
    memcpy(opened_before, opened, sizeof(opened));
    memset(&fact, 0x3C, sizeof(fact));
    fact_before = fact;
    opened_bytes = SIZE_MAX;
    CHECK(ucn_i_security_c0_open(
              &receiver.owner, &access, &receiver.facts, packet,
              packet_bytes, &workspace, opened, sizeof(opened),
              &opened_bytes, &fact) == UCN_ERR_STATE);
    CHECK(memcmp(opened, opened_before, sizeof(opened)) == 0);
    CHECK(opened_bytes == SIZE_MAX);
    CHECK(memcmp(&fact, &fact_before, sizeof(fact)) == 0);
    CHECK(fixture_destroy(&receiver) == 0);
    return 0;
}

static int test_c0_confidential_hop_protected_fact(void)
{
    session_fixture_t receiver;
    ucn_i_security_candidate_t candidate = candidate_make(0x28U, 2U);
    ucn_i_security_control_access_request_t access;
    ucn_i_security_packet_workspace_t workspace;
    ucn_i_security_authenticated_control_t fact;
    ucn_i_c0_frame_t frame;
    uint8_t plaintext[] = {0x91U, 0x82U, 0x73U, 0x64U};
    uint8_t ciphertext[sizeof(plaintext)];
    uint8_t opened[sizeof(plaintext)];
    uint8_t origin_tag[UCN_I_C0_ORIGIN_TAG_BYTES];
    uint8_t hop_tag[UCN_I_SECURITY_HOP_TAG_BYTES];
    uint8_t hop_tag_full[UCN_I_SECURITY_ORIGIN_TAG_BYTES];
    uint8_t nonce[UCN_I_SECURITY_SEQUENCE_NONCE_INPUT_BYTES];
    uint8_t origin_aad[UCN_I_C0_ORIGIN_AAD_MAX_BYTES];
    uint8_t hop_aad[UCN_I_SECURITY_HOP_AAD_BYTES];
    uint8_t packet[UCN_ADAPTER_FRAME_BYTES];
    size_t origin_aad_bytes = 0U;
    size_t header_bytes = 0U;
    size_t packet_bytes = 0U;
    size_t packet_before_hop_bytes;
    size_t hop_aad_bytes;
    size_t opened_bytes = 0U;
    size_t index;

    candidate.origin_level = UCN_I_SECURITY_CONFIDENTIAL;
    candidate.origin_tx.suite_id = 2U;
    candidate.origin_rx.suite_id = 2U;
    candidate.hop_profile = UCN_I_HOP_PROFILE_H1;
    candidate.hop_tx.key_generation = 30U;
    candidate.hop_tx.key_id = 7U;
    candidate.hop_tx.suite_id = 16U;
    candidate.hop_rx.key_generation = 31U;
    candidate.hop_rx.key_id = 8U;
    candidate.hop_rx.suite_id = 16U;
    CHECK(fixture_activate(&receiver, &candidate,
                           UCN_I_SECURITY_ACCESS_INBOUND, 62U, 92U,
                           0U, UCN_I_OPCODE_DIAGNOSTIC_PING) == 0);
    memset(&frame, 0, sizeof(frame));
    frame.payload = ciphertext;
    frame.payload_bytes = sizeof(ciphertext);
    frame.origin_tag = origin_tag;
    frame.hop_tag = hop_tag;
    frame.transaction_id = UINT64_C(0x1112131415161718);
    frame.realm_id = receiver.config.realm_id;
    frame.source_address = candidate.peer.address;
    frame.destination_address = candidate.local.address;
    frame.source_binding_generation = candidate.peer.binding_generation;
    frame.destination_binding_generation = candidate.local.binding_generation;
    frame.hop_sequence = 7U;
    frame.opcode = UCN_I_OPCODE_DIAGNOSTIC_PING;
    frame.traffic_class = UCN_TRAFFIC_Q0;
    frame.delivery_guarantee = UCN_DELIVERY_RELIABLE;
    frame.interaction_role = UCN_INTERACTION_REQUEST;
    frame.payload_kind = UCN_I_PAYLOAD_DIAGNOSTIC;
    frame.origin_security = UCN_I_ORIGIN_O2;
    frame.hop_limit = 2U;
    test_c0_nonce_input(&candidate, &frame,
                        UCN_I_SECURITY_ACCESS_INBOUND, nonce);
    for (index = 0U; index < sizeof(ciphertext); ++index) {
        ciphertext[index] = plaintext[index] ^
            nonce[index % sizeof(nonce)] ^
            (uint8_t)candidate.origin_rx.key_id;
    }
    CHECK(ucn_i_c0_origin_aad_build(&frame, candidate.address_width,
                                     origin_aad,
                                     &origin_aad_bytes) == UCN_OK);
    test_tag(&candidate.origin_rx, origin_aad, origin_aad_bytes,
             ciphertext, sizeof(ciphertext), origin_tag);

    /* Encode once with a placeholder hop tag so the exact packet prefix can
     * be used to derive the hop-authenticated AAD. */
    memset(hop_tag, 0, sizeof(hop_tag));
    CHECK(ucn_i_c0_encode(&frame, candidate.address_width,
                           UCN_I_CONTROL_H1, packet, sizeof(packet),
                           &packet_bytes) == UCN_OK);
    CHECK(ucn_i_c0_header_bytes(candidate.address_width,
                                 &header_bytes) == UCN_OK);
    packet_before_hop_bytes = header_bytes + sizeof(ciphertext) +
                              UCN_I_C0_ORIGIN_TAG_BYTES;
    hop_aad_bytes = test_hop_aad(packet, packet_before_hop_bytes,
                                 frame.hop_sequence,
                                 candidate.hop_fingerprint, hop_aad);
    test_tag(&candidate.hop_rx, hop_aad, hop_aad_bytes, NULL, 0U,
             hop_tag_full);
    memcpy(hop_tag, hop_tag_full, sizeof(hop_tag));
    CHECK(ucn_i_c0_encode(&frame, candidate.address_width,
                           UCN_I_CONTROL_H1, packet, sizeof(packet),
                           &packet_bytes) == UCN_OK);

    memset(&access, 0, sizeof(access));
    access.session = receiver.handle;
    memcpy(access.context_fingerprint, candidate.origin_fingerprint,
           sizeof(access.context_fingerprint));
    access.protocol_opcode = UCN_I_OPCODE_DIAGNOSTIC_PING;
    access.direction = UCN_I_SECURITY_ACCESS_INBOUND;
    CHECK(ucn_i_security_c0_open(
              &receiver.owner, &access, &receiver.facts, packet,
              packet_bytes, &workspace, opened, sizeof(opened),
              &opened_bytes, &fact) == UCN_OK);
    CHECK(opened_bytes == sizeof(plaintext));
    CHECK(memcmp(opened, plaintext, sizeof(opened)) == 0);
    CHECK(fact.replay.has_hop == 1U);
    CHECK(ucn_i_security_replay_abort(&receiver.owner,
                                       &(fact.replay)) == UCN_OK);
    CHECK(fixture_destroy(&receiver) == 0);
    return 0;
}

static int run_c0_protect_roundtrip(uint8_t origin_level,
                                    uint8_t origin_suite,
                                    uint8_t hop_profile)
{
    session_fixture_t sender;
    session_fixture_t receiver;
    ucn_i_security_candidate_t sender_candidate = candidate_make(0x38U, 3U);
    ucn_i_security_candidate_t receiver_candidate;
    ucn_i_security_binding_t binding_swap;
    ucn_i_security_key_selector_t selector_swap;
    ucn_i_security_c0_protect_request_t request;
    ucn_i_security_control_access_request_t access;
    ucn_i_security_control_access_request_t access_before;
    ucn_i_security_current_facts_t resolved_facts;
    ucn_i_security_current_facts_t facts_before;
    ucn_i_security_packet_workspace_t sender_workspace;
    ucn_i_security_packet_workspace_t receiver_workspace;
    ucn_i_security_authenticated_control_t fact;
    uint8_t plaintext[] = {0xA1U, 0xB2U, 0xC3U};
    uint8_t sender_nonce_snapshot[UCN_I_SECURITY_SEQUENCE_NONCE_INPUT_BYTES];
    uint8_t packet[UCN_ADAPTER_FRAME_BYTES];
    uint8_t packet_before[UCN_ADAPTER_FRAME_BYTES];
    uint8_t opened[sizeof(plaintext)];
    uint32_t provider_calls;
    size_t packet_bytes = SIZE_MAX;
    size_t opened_bytes = SIZE_MAX;

    sender_candidate.origin_level = origin_level;
    sender_candidate.origin_tx.suite_id = origin_suite;
    sender_candidate.origin_rx.suite_id = origin_suite;
    if (hop_profile == UCN_I_HOP_PROFILE_H1) {
        sender_candidate.hop_profile = UCN_I_HOP_PROFILE_H1;
        sender_candidate.hop_tx.suite_id = 16U;
        sender_candidate.hop_tx.key_id = 11U;
        sender_candidate.hop_tx.key_generation = 41U;
        sender_candidate.hop_rx.suite_id = 16U;
        sender_candidate.hop_rx.key_id = 12U;
        sender_candidate.hop_rx.key_generation = 42U;
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
                           UCN_I_SECURITY_ACCESS_OUTBOUND, 63U, 93U,
                           0U, UCN_I_OPCODE_DIAGNOSTIC_PING) == 0);
    CHECK(fixture_activate(&receiver, &receiver_candidate,
                           UCN_I_SECURITY_ACCESS_INBOUND, 64U, 94U,
                           0U, UCN_I_OPCODE_DIAGNOSTIC_PING) == 0);

    memset(&request, 0, sizeof(request));
    request.session = sender.handle;
    request.facts = sender.facts;
    request.transaction_id = UINT64_C(0x2122232425262728);
    request.protocol_opcode = UCN_I_OPCODE_DIAGNOSTIC_PING;
    memcpy(request.context_fingerprint,
           sender_candidate.origin_fingerprint,
           sizeof(request.context_fingerprint));
    request.traffic_class = UCN_TRAFFIC_Q0;
    request.delivery_guarantee = UCN_DELIVERY_RELIABLE;
    request.interaction_role = UCN_INTERACTION_REQUEST;
    request.payload_kind = UCN_I_PAYLOAD_DIAGNOSTIC;
    request.hop_limit = 2U;
    request.direction = UCN_I_SECURITY_ACCESS_OUTBOUND;
    memset(packet, 0xA5, sizeof(packet));
    if (hop_profile == UCN_I_HOP_PROFILE_H1) {
        static ucn_i_security_c0_path_workspace_t path, prior_path;
        static ucn_i_security_owner_t prior_owner;
        memset(&path, 0xA5, sizeof(path)); prior_path = path; prior_owner = sender.owner;
        provider_calls = sender.provider.calls;
        CHECK(ucn_i_security_c0_path_protect(&sender.owner, &request, sender.handle, &sender.facts,
            plaintext, sizeof(plaintext), &path, packet, sizeof(packet), &packet_bytes) == UCN_ERR_STATE);
        CHECK(memcmp(&sender.owner, &prior_owner, sizeof(prior_owner)) == 0);
        CHECK(memcmp(&path, &prior_path, sizeof(path)) == 0 && packet[0] == 0xA5U && packet_bytes == SIZE_MAX);
        CHECK(sender.provider.calls == provider_calls);
    }
    CHECK(ucn_i_security_c0_protect(
              &sender.owner, &request, plaintext, sizeof(plaintext),
              &sender_workspace, packet, 1U, &packet_bytes) ==
          UCN_ERR_NO_SPACE);
    CHECK(packet[0] == 0xA5U && packet_bytes == SIZE_MAX);
    CHECK(ucn_i_security_c0_protect(
              &sender.owner, &request, plaintext, sizeof(plaintext),
              &sender_workspace, packet, sizeof(packet),
              &packet_bytes) == UCN_OK);
    CHECK(packet_bytes > sizeof(plaintext));
    memcpy(sender_nonce_snapshot, sender_workspace.nonce_input, sizeof(sender_nonce_snapshot));
    memcpy(packet_before, packet, packet_bytes);
    provider_calls = sender.provider.calls;
    CHECK(ucn_i_security_c0_protect(
              &sender.owner, &request, plaintext, sizeof(plaintext),
              &sender_workspace, packet, sizeof(packet),
              &packet_bytes) == UCN_ERR_REPLAY);
    CHECK(sender.provider.calls == provider_calls);
    CHECK(memcmp(packet, packet_before, packet_bytes) == 0);
    --request.transaction_id;
    CHECK(ucn_i_security_c0_protect(
              &sender.owner, &request, plaintext, sizeof(plaintext),
              &sender_workspace, packet, sizeof(packet),
              &packet_bytes) == UCN_ERR_REPLAY);
    CHECK(sender.provider.calls == provider_calls);
    ++request.transaction_id;

    memset(&access, 0xA5, sizeof(access));
    memset(&resolved_facts, 0x5A, sizeof(resolved_facts));
    access_before = access;
    facts_before = resolved_facts;
    CHECK(ucn_i_security_c0_receive_context_resolve(
              &receiver.owner, packet, packet_bytes, receiver.facts.now_us,
              receiver.facts.link_generation + 1U, &access,
              &resolved_facts) == UCN_ERR_NOT_FOUND);
    CHECK(memcmp(&access, &access_before, sizeof(access)) == 0);
    CHECK(memcmp(&resolved_facts, &facts_before,
                 sizeof(resolved_facts)) == 0);
    CHECK(ucn_i_security_c0_receive_context_resolve(
              &receiver.owner, packet, packet_bytes, receiver.facts.now_us,
              receiver.facts.link_generation, &access,
              &resolved_facts) == UCN_OK);
    CHECK(memcmp(&access.session, &receiver.handle,
                 sizeof(access.session)) == 0);
    CHECK(access.protocol_opcode == UCN_I_OPCODE_DIAGNOSTIC_PING);
    CHECK(access.direction == UCN_I_SECURITY_ACCESS_INBOUND);
    CHECK(memcmp(&resolved_facts, &receiver.facts,
                 sizeof(resolved_facts)) == 0);
    if (hop_profile == UCN_I_HOP_PROFILE_H1) {
        static ucn_i_security_c0_path_workspace_t path, prior_path;
        static ucn_i_security_owner_t prior_owner;
        ucn_i_security_hop_replay_handle_t hop, prior_hop;
        ucn_i_security_authenticated_control_t prior_fact;
        memset(&path, 0xA5, sizeof(path)); prior_path = path; prior_owner = receiver.owner;
        memset(&hop, 0x5A, sizeof(hop)); prior_hop = hop;
        memset(&fact, 0xA5, sizeof(fact)); prior_fact = fact; memset(opened, 0xA5, sizeof(opened));
        provider_calls = receiver.provider.calls;
        CHECK(ucn_i_security_c0_path_open(&receiver.owner, &access, &resolved_facts,
            receiver.handle, &receiver.facts, packet, packet_bytes, &path, opened, sizeof(opened),
            &opened_bytes, &fact, &hop) == UCN_ERR_STATE);
        CHECK(memcmp(&receiver.owner, &prior_owner, sizeof(prior_owner)) == 0);
        CHECK(memcmp(&path, &prior_path, sizeof(path)) == 0 && opened[0] == 0xA5U && opened_bytes == SIZE_MAX);
        CHECK(memcmp(&hop, &prior_hop, sizeof(hop)) == 0 && memcmp(&fact, &prior_fact, sizeof(fact)) == 0);
        CHECK(receiver.provider.calls == provider_calls);
    }
    {
        ucn_i_security_c0_open_call_t call, call_before;
        static ucn_i_security_owner_t owner_before;
        ucn_i_security_packet_workspace_t workspace_before;
        ucn_i_security_authenticated_control_t fact_before;
        size_t length_before = opened_bytes;
        uint8_t output_before[sizeof(opened)];
        memset(&fact, 0xA5, sizeof(fact)); fact_before = fact;
        memset(&receiver_workspace, 0, sizeof(receiver_workspace));
        memset(opened, 0x5A, sizeof(opened));
        memcpy(output_before, opened, sizeof(opened));
        memset(&call, 0, sizeof(call));
        call.access = &access; call.facts = &resolved_facts;
        call.packet = packet; call.packet_bytes = packet_bytes;
        call.plaintext_output = (uint8_t *)&call + 1U;
        call.plaintext_capacity = sizeof(opened); call.plaintext_bytes = &opened_bytes;
        call.fact_out = &fact;
        owner_before = receiver.owner; workspace_before = receiver_workspace;
        call_before = call;
        CHECK(ucn_i_security_c0_open_borrowed(&receiver.owner, NULL, &receiver_workspace) == UCN_ERR_ARGUMENT);
        CHECK(ucn_i_security_c0_open_borrowed(&receiver.owner, &call, &receiver_workspace) == UCN_ERR_ARGUMENT);
        CHECK(memcmp(&call, &call_before, sizeof(call)) == 0);
        call.plaintext_output = opened; call.fact_out = (ucn_i_security_authenticated_control_t *)&call;
        call_before = call;
        CHECK(ucn_i_security_c0_open_borrowed(&receiver.owner, &call, &receiver_workspace) == UCN_ERR_ARGUMENT);
        CHECK(memcmp(&call, &call_before, sizeof(call)) == 0);
        CHECK(memcmp(&receiver.owner, &owner_before, sizeof(owner_before)) == 0);
        CHECK(memcmp(&receiver_workspace, &workspace_before, sizeof(workspace_before)) == 0);
        CHECK(memcmp(&fact, &fact_before, sizeof(fact)) == 0 && opened_bytes == length_before);
        CHECK(memcmp(opened, output_before, sizeof(opened)) == 0);
    }
    CHECK(ucn_i_security_c0_open(
              &receiver.owner, &access, &resolved_facts, packet,
              packet_bytes, &receiver_workspace, opened, sizeof(opened),
              &opened_bytes, &fact) == UCN_OK);
    CHECK(opened_bytes == sizeof(plaintext));
    CHECK(memcmp(opened, plaintext, sizeof(opened)) == 0);
    CHECK(fact.origin_security == origin_level);
    if (origin_level == UCN_I_ORIGIN_O2) {
        CHECK(memcmp(sender_nonce_snapshot, receiver_workspace.nonce_input,
                     UCN_I_SECURITY_SEQUENCE_NONCE_INPUT_BYTES) == 0);
    }
    CHECK(fact.replay.has_hop ==
          (hop_profile == UCN_I_HOP_PROFILE_H1 ? 1U : 0U));
    CHECK(ucn_i_security_replay_commit(&receiver.owner, &(fact.replay),
                                        &receiver.facts) == UCN_OK);
    CHECK(fixture_destroy(&receiver) == 0);
    CHECK(fixture_destroy(&sender) == 0);
    return 0;
}

static int test_c0_protect_roundtrips(void)
{
    CHECK(run_c0_protect_roundtrip(UCN_I_SECURITY_AUTHENTICATED, 1U,
                                   UCN_I_HOP_PROFILE_H0) == 0);
    CHECK(run_c0_protect_roundtrip(UCN_I_SECURITY_CONFIDENTIAL, 2U,
                                   UCN_I_HOP_PROFILE_H1) == 0);
    return 0;
}

static void binding_set(ucn_i_security_binding_t *binding,
                        uint32_t address,
                        uint32_t generation,
                        uint8_t principal_seed)
{
    memset(binding, 0, sizeof(*binding));
    binding->address = address;
    binding->binding_generation = generation;
    fill_bytes(binding->principal, sizeof(binding->principal),
               principal_seed);
}

static void make_h1_candidate(ucn_i_security_candidate_t *candidate,
                              const ucn_i_security_binding_t *local,
                              const ucn_i_security_binding_t *peer,
                              uint32_t session_generation,
                              uint32_t link_generation,
                              uint16_t tx_key,
                              uint16_t rx_key,
                              uint8_t fingerprint_seed)
{
    *candidate = candidate_make(peer->principal[0], session_generation);
    candidate->local = *local;
    candidate->peer = *peer;
    candidate->link_generation = link_generation;
    candidate->origin_level = UCN_I_SECURITY_AUTHENTICATED;
    candidate->hop_profile = UCN_I_HOP_PROFILE_H1;
    candidate->hop_tx.suite_id = 16U;
    candidate->hop_tx.key_id = tx_key;
    candidate->hop_tx.key_generation = 51U;
    candidate->hop_rx.suite_id = 16U;
    candidate->hop_rx.key_id = rx_key;
    candidate->hop_rx.key_generation = 51U;
    fill_bytes(candidate->hop_fingerprint,
               sizeof(candidate->hop_fingerprint), fingerprint_seed);
}

static int test_c0_multihop_split_origin_and_hop_contexts(void)
{
    dual_session_fixture_t source;
    dual_session_fixture_t relay;
    dual_session_fixture_t destination;
    ucn_i_security_binding_t binding_a;
    ucn_i_security_binding_t binding_b;
    ucn_i_security_binding_t binding_d;
    ucn_i_security_candidate_t source_candidates[2];
    ucn_i_security_candidate_t relay_candidates[2];
    ucn_i_security_candidate_t destination_candidates[2];
    const uint8_t outbound[2] = {
        UCN_I_SECURITY_ACCESS_OUTBOUND,
        UCN_I_SECURITY_ACCESS_OUTBOUND
    };
    const uint8_t relay_directions[2] = {
        UCN_I_SECURITY_ACCESS_INBOUND,
        UCN_I_SECURITY_ACCESS_OUTBOUND
    };
    const uint8_t inbound[2] = {
        UCN_I_SECURITY_ACCESS_INBOUND,
        UCN_I_SECURITY_ACCESS_INBOUND
    };
    ucn_i_security_c0_protect_request_t request;
    ucn_i_security_c0_path_workspace_t source_workspace;
    ucn_i_security_c0_path_workspace_t destination_workspace;
    ucn_i_security_c0_path_workspace_t relay_open_workspace;
    ucn_i_security_c0_origin_resolve_workspace_t origin_resolve_workspace;
    ucn_i_security_packet_workspace_t relay_seal_workspace;
    ucn_i_security_c0_relay_fact_t relay_fact;
    ucn_i_security_handle_t resolved_hop;
    ucn_i_security_current_facts_t resolved_hop_facts;
    ucn_i_security_control_access_request_t resolved_origin;
    ucn_i_security_current_facts_t resolved_origin_facts;
    ucn_i_security_authenticated_control_t authenticated;
    ucn_i_security_hop_replay_handle_t destination_hop_replay;
    ucn_i_c0_frame_t source_frame;
    ucn_i_c0_frame_t relay_frame;
    uint8_t plaintext[] = {0x91U, 0x82U, 0x73U, 0x64U};
    uint8_t source_packet[UCN_ADAPTER_FRAME_BYTES];
    uint8_t relay_packet[UCN_ADAPTER_FRAME_BYTES];
    uint8_t opened[sizeof(plaintext)];
    size_t source_packet_bytes = 0U;
    size_t relay_packet_bytes = 0U;
    size_t opened_bytes = 0U;
    uint32_t provider_calls;
    uint32_t origin_open_calls;
    ucn_result_t path_result;

    binding_set(&binding_a, 1U, 101U, 0x10U);
    binding_set(&binding_b, 2U, 102U, 0x30U);
    binding_set(&binding_d, 4U, 104U, 0x50U);

    source_candidates[0] = candidate_make(0x50U, 1U);
    source_candidates[0].local = binding_a;
    source_candidates[0].peer = binding_d;
    source_candidates[0].origin_level = UCN_I_SECURITY_CONFIDENTIAL;
    source_candidates[0].origin_tx.suite_id = 2U;
    source_candidates[0].origin_tx.key_id = 21U;
    source_candidates[0].origin_tx.key_generation = 61U;
    source_candidates[0].origin_rx.suite_id = 2U;
    source_candidates[0].origin_rx.key_id = 22U;
    source_candidates[0].origin_rx.key_generation = 62U;
    source_candidates[0].hop_profile = UCN_I_HOP_PROFILE_H0;
    fill_bytes(source_candidates[0].origin_fingerprint,
               sizeof(source_candidates[0].origin_fingerprint), 0x81U);
    make_h1_candidate(&source_candidates[1], &binding_a, &binding_b,
                      2U, 111U, 31U, 32U, 0x91U);

    make_h1_candidate(&relay_candidates[0], &binding_b, &binding_a,
                      3U, 111U, 32U, 31U, 0x91U);
    make_h1_candidate(&relay_candidates[1], &binding_b, &binding_d,
                      4U, 222U, 41U, 42U, 0xA1U);

    destination_candidates[0] = source_candidates[0];
    destination_candidates[0].local = binding_d;
    destination_candidates[0].peer = binding_a;
    destination_candidates[0].session_generation = 5U;
    destination_candidates[0].origin_tx = source_candidates[0].origin_rx;
    destination_candidates[0].origin_rx = source_candidates[0].origin_tx;
    make_h1_candidate(&destination_candidates[1], &binding_d, &binding_b,
                      6U, 222U, 42U, 41U, 0xA1U);

    CHECK(dual_session_fixture_activate(
              &source, source_candidates, outbound, 81U, 181U,
              UCN_I_OPCODE_DIAGNOSTIC_PING) == 0);
    CHECK(dual_session_fixture_activate(
              &relay, relay_candidates, relay_directions, 82U, 182U,
              UCN_I_OPCODE_DIAGNOSTIC_PING) == 0);
    CHECK(dual_session_fixture_activate(
              &destination, destination_candidates, inbound, 83U, 183U,
              UCN_I_OPCODE_DIAGNOSTIC_PING) == 0);

    memset(&request, 0, sizeof(request));
    request.session = source.handles[0];
    request.facts = source.facts[0];
    request.transaction_id = UINT64_C(0x3132333435363738);
    request.protocol_opcode = UCN_I_OPCODE_DIAGNOSTIC_PING;
    memcpy(request.context_fingerprint,
           source_candidates[0].origin_fingerprint,
           sizeof(request.context_fingerprint));
    request.traffic_class = UCN_TRAFFIC_Q0;
    request.delivery_guarantee = UCN_DELIVERY_RELIABLE;
    request.interaction_role = UCN_INTERACTION_REQUEST;
    request.payload_kind = UCN_I_PAYLOAD_DIAGNOSTIC;
    request.hop_limit = 3U;
    request.direction = UCN_I_SECURITY_ACCESS_OUTBOUND;
    CHECK(ucn_i_security_c0_path_protect(
              &source.owner, &request, source.handles[1], &source.facts[1],
              plaintext, sizeof(plaintext), &source_workspace,
              source_packet, sizeof(source_packet),
              &source_packet_bytes) == UCN_OK);
    CHECK(ucn_i_c0_decode(source_packet, source_packet_bytes, 1U,
                          UCN_I_CONTROL_H1, &source_frame) == UCN_OK);
    CHECK(source_frame.hop_limit == 3U);

    CHECK(ucn_i_security_c0_hop_receive_context_resolve(
              &relay.owner, source_packet, source_packet_bytes, 1500U,
              relay.facts[0].link_generation, &resolved_hop,
              &resolved_hop_facts) == UCN_OK);
    CHECK(memcmp(&resolved_hop, &relay.handles[0],
                 sizeof(resolved_hop)) == 0);
    provider_calls = relay.provider.calls;
    origin_open_calls = relay.provider.open_origin_calls;
    CHECK(ucn_i_security_c0_hop_open(
              &relay.owner, resolved_hop, &resolved_hop_facts,
              source_packet, source_packet_bytes,
              &relay_open_workspace) == UCN_OK);
    relay_fact = relay_open_workspace.relay;
    CHECK(relay.provider.calls == provider_calls + 1U);
    CHECK(relay.provider.open_origin_calls == origin_open_calls);
    source_packet[0] ^= 1U;
    memset(relay_packet, 0xA5, sizeof(relay_packet));
    relay_packet_bytes = SIZE_MAX;
    CHECK(ucn_i_security_c0_hop_seal(
              &relay.owner, relay.handles[1], &relay.facts[1], &relay_fact,
              &relay_seal_workspace, relay_packet, sizeof(relay_packet),
              &relay_packet_bytes) != UCN_OK);
    CHECK(relay_packet[0] == 0xA5U && relay_packet_bytes == SIZE_MAX);
    source_packet[0] ^= 1U;
    CHECK(ucn_i_security_c0_hop_seal(
              &relay.owner, relay.handles[1], &relay.facts[1], &relay_fact,
              &relay_seal_workspace, relay_packet, sizeof(relay_packet),
              &relay_packet_bytes) == UCN_OK);
    CHECK(ucn_i_c0_decode(relay_packet, relay_packet_bytes, 1U,
                          UCN_I_CONTROL_H1, &relay_frame) == UCN_OK);
    CHECK(relay_frame.hop_limit == 2U);
    CHECK(relay_frame.payload_bytes == source_frame.payload_bytes);
    CHECK(memcmp(relay_frame.payload, source_frame.payload,
                 source_frame.payload_bytes) == 0);
    CHECK(memcmp(relay_frame.origin_tag, source_frame.origin_tag,
                 UCN_I_SECURITY_ORIGIN_TAG_BYTES) == 0);
    CHECK(ucn_i_security_hop_replay_commit(
              &relay.owner, &(relay_fact.replay),
              &relay.facts[0]) == UCN_OK);
    CHECK(ucn_i_security_c0_hop_open(
              &relay.owner, relay.handles[0], &relay.facts[0],
              source_packet, source_packet_bytes,
              &relay_open_workspace) == UCN_ERR_REPLAY);

    CHECK(ucn_i_security_c0_hop_receive_context_resolve(
              &destination.owner, relay_packet, relay_packet_bytes, 1500U,
              destination.facts[1].link_generation, &resolved_hop,
              &resolved_hop_facts) == UCN_OK);
    CHECK(ucn_i_security_c0_origin_receive_context_resolve(
              &destination.owner, relay_packet, relay_packet_bytes, 1500U,
              &origin_resolve_workspace,
              &resolved_origin, &resolved_origin_facts) == UCN_OK);
    CHECK(memcmp(&resolved_hop, &destination.handles[1],
                 sizeof(resolved_hop)) == 0);
    CHECK(memcmp(&resolved_origin.session, &destination.handles[0],
                 sizeof(resolved_origin.session)) == 0);
    path_result = ucn_i_security_c0_path_open(
        &destination.owner, &resolved_origin, &resolved_origin_facts,
        resolved_hop, &resolved_hop_facts, relay_packet,
        relay_packet_bytes, &destination_workspace, opened,
        sizeof(opened), &opened_bytes, &authenticated,
        &destination_hop_replay);
    CHECK(path_result == UCN_OK);
    CHECK(opened_bytes == sizeof(plaintext));
    CHECK(memcmp(opened, plaintext, sizeof(plaintext)) == 0);
    CHECK(authenticated.hop_limit == 2U);
    CHECK(ucn_i_security_replay_commit(
              &destination.owner, &(authenticated.replay),
              &resolved_origin_facts) == UCN_OK);
    CHECK(ucn_i_security_hop_replay_commit(
              &destination.owner, &(destination_hop_replay),
              &resolved_hop_facts) == UCN_OK);
    CHECK(ucn_i_security_c0_path_open(
              &destination.owner, &resolved_origin, &resolved_origin_facts,
              resolved_hop, &resolved_hop_facts, relay_packet,
              relay_packet_bytes, &destination_workspace, opened,
              sizeof(opened), &opened_bytes, &authenticated,
              &destination_hop_replay) == UCN_ERR_REPLAY);

    CHECK(dual_session_fixture_destroy(&destination) == 0);
    CHECK(dual_session_fixture_destroy(&relay) == 0);
    CHECK(dual_session_fixture_destroy(&source) == 0);
    return 0;
}

static int test_peer_reauth_codec(void)
{
    uint8_t raw[208]={0},out[208],old[208];size_t offsets[]={0U,1U,2U,3U,65U,70U,71U};size_t i;
    ucn_i_peer_reauth_record_t value,old_value;
    raw[0]=2U;raw[2]=2U;raw[7]=1U;raw[11]=1U;raw[15]=1U;memset(raw+16U,0x11,16U);raw[159]=1U;raw[163]=1U;
    raw[35]=2U;raw[39]=1U;memset(raw+40U,0x22,16U);raw[59]=1U;raw[63]=2U;
    raw[64]=1U;raw[65]=16U;raw[67]=1U;raw[69]=3U;raw[75]=2U;raw[79]=2U;
    raw[81]=7U;raw[87]=9U;raw[99]=3U;raw[107]=4U;raw[123]=1U;memset(raw+124U,0x77,32U);
    CHECK(ucn_i_peer_reauth_decode(raw,sizeof(raw),&value)==UCN_OK);
    CHECK(ucn_i_peer_reauth_encode(&value,out,sizeof(out))==UCN_OK);CHECK(memcmp(raw,out,sizeof(raw))==0);
    memset(&value,0xcc,sizeof(value));old_value=value;
    CHECK(ucn_i_peer_reauth_decode(raw,sizeof(raw)-1U,&value)==UCN_ERR_MALFORMED);
    CHECK(memcmp(&value,&old_value,sizeof(value))==0);
    for(i=0U;i<sizeof(offsets)/sizeof(offsets[0]);++i){
        uint8_t saved=raw[offsets[i]];raw[offsets[i]]=255U;
        CHECK(ucn_i_peer_reauth_decode(raw,sizeof(raw),&value)==UCN_ERR_MALFORMED);
        CHECK(memcmp(&value,&old_value,sizeof(value))==0);raw[offsets[i]]=saved;
    }
    CHECK(ucn_i_peer_reauth_decode(raw,sizeof(raw),&value)==UCN_OK);memset(out,0xcc,sizeof(out));memcpy(old,out,sizeof(old));
    CHECK(ucn_i_peer_reauth_encode(&value,out,sizeof(out)-1U)==UCN_ERR_NO_SPACE);CHECK(memcmp(old,out,sizeof(out))==0);
    CHECK(ucn_i_peer_reauth_encode(&value,value.canonical,sizeof(value.canonical))==UCN_ERR_ARGUMENT);
    raw[1]=2U;raw[83]=8U;raw[91]=10U;raw[115]=5U;
    raw[63]=3U;raw[75]=3U;raw[79]=3U;raw[167]=2U;raw[171]=2U;raw[175]=2U;memset(raw+176U,0x88,32U);
    CHECK(ucn_i_peer_reauth_decode(raw,sizeof(raw),&value)==UCN_OK);
    old_value=value;raw[171]=1U;
    CHECK(ucn_i_peer_reauth_decode(raw,sizeof(raw),&value)==UCN_ERR_MALFORMED);CHECK(memcmp(&value,&old_value,sizeof(value))==0);
    raw[171]=2U;raw[167]=1U;
    CHECK(ucn_i_peer_reauth_decode(raw,sizeof(raw),&value)==UCN_ERR_MALFORMED);CHECK(memcmp(&value,&old_value,sizeof(value))==0);
    {
        ucn_i_peer_reauth_fragment_t v={0},decoded;
        uint8_t before[sizeof(decoded)];
        uint8_t fragment[98],sentinel[98];size_t n=999U;
        v.stage=2U;v.index=3U;v.total_bytes=241U;v.transaction=3U;v.data_bytes=1U;v.data[0]=0x44;
        CHECK(ucn_i_peer_reauth_fragment_encode(&v,fragment,sizeof(fragment),&n)==UCN_OK);CHECK(n==19U);
        CHECK(ucn_i_peer_reauth_fragment_decode(fragment,n,&decoded)==UCN_OK);CHECK(decoded.transaction==3U&&decoded.index==3U);
        memcpy(before,&decoded,sizeof(decoded));fragment[8]=79U;
        CHECK(ucn_i_peer_reauth_fragment_decode(fragment,n,&decoded)==UCN_ERR_MALFORMED);CHECK(memcmp(&decoded,before,sizeof(decoded))==0);
        memset(fragment,0xcc,sizeof(fragment));memcpy(sentinel,fragment,sizeof(fragment));n=999U;
        CHECK(ucn_i_peer_reauth_fragment_encode(&v,fragment,18U,&n)==UCN_ERR_NO_SPACE);CHECK(n==999U);CHECK(memcmp(fragment,sentinel,sizeof(fragment))==0);
        v.index=4U;CHECK(ucn_i_peer_reauth_fragment_encode(&v,fragment,sizeof(fragment),&n)==UCN_ERR_ARGUMENT);CHECK(n==999U);CHECK(memcmp(fragment,sentinel,sizeof(fragment))==0);
    }
    return 0;
}

/* EN: A fenced predecessor is not a reason to resurrect it or reset history.
 * ZH: 围栏后允许准备精确后继，但不复活前驱或绕过耐久激活门。 */
static int test_fenced_successor_requires_exact_prior(void)
{
    static session_fixture_t f;
    static ucn_i_security_owner_t before;
    ucn_i_security_candidate_t next;
    ucn_i_security_durability_base_t base;
    ucn_i_security_handle_t successor, sentinel;
    ucn_i_security_session_view_t view;
    const uint8_t proof = 0xC3U;
    next = candidate_make(0x20U, 1U);
    CHECK(fixture_activate(&f, &next, UCN_I_SECURITY_ACCESS_OUTBOUND,
        88U, 89U, 0x1201U, 0U) == 0);
    CHECK(ucn_i_security_fence(&f.owner, f.handle) == UCN_OK);
    next = f.candidate;
    next.session_generation = 2U;
    next.origin_tx.key_generation = next.origin_rx.key_generation = 2U;
    next.origin_fingerprint[0] ^= 0x40U;
    next.transcript_digest[0] ^= 0x40U;
    base = durability_make(f.rule.domain_id, 2U);
    memset(&successor, 0xA5, sizeof(successor)); sentinel = successor;
    before = f.owner;
    /* A fabricated later history is not a successor of the fenced slot. */
    base.prior_session_generation = 2U; next.session_generation = 3U;
    CHECK(ucn_i_security_prepare_static_session(&f.owner, &next, &proof, 1U,
        &base, 1500U, &successor) == UCN_ERR_STATE);
    CHECK(memcmp(&f.owner, &before, sizeof(before)) == 0);
    CHECK(memcmp(&successor, &sentinel, sizeof(sentinel)) == 0);
    base.prior_session_generation = 1U; next.session_generation = 2U;
    CHECK(ucn_i_security_prepare_static_session(&f.owner, &next, &proof, 1U,
        &base, 1500U, &successor) == UCN_OK);
    CHECK(ucn_i_security_session_get(&f.owner, successor, &view) == UCN_OK);
    CHECK(view.phase == UCN_I_SECURITY_SESSION_AWAITING_DURABILITY);
    CHECK(ucn_i_security_session_get(&f.owner, f.handle, &view) == UCN_OK);
    CHECK(view.phase == UCN_I_SECURITY_SESSION_FENCED);
    CHECK(ucn_i_security_fence(&f.owner, successor) == UCN_OK);
    CHECK(ucn_i_security_retire_fenced(&f.owner, successor) == UCN_OK);
    CHECK(fixture_destroy(&f) == 0);
    return 0;
}

static int test_explicit_pending_session_policy_replaces_defaults(void)
{
    static session_fixture_t f;
    static ucn_i_security_owner_t owner_before;
    static union {
        ucn_i_security_candidate_t candidate_sized;
        ucn_i_security_acl_rule_t rule;
    } borrowed_policy;
    ucn_i_security_candidate_t candidate = candidate_make(0x20U, 1U);
    ucn_i_security_acl_rule_t grant = acl_make(&candidate, UCN_I_SECURITY_ACCESS_OUTBOUND);
    ucn_i_security_acl_rule_t old_default = grant;
    ucn_i_security_access_request_t access;
    grant.service_id = 0x1202U;
    memset(&borrowed_policy, 0, sizeof(borrowed_policy)); borrowed_policy.rule = grant;
    old_default.context_fingerprint[0] ^= 0x80U;
    CHECK(fixture_activate_profile_policy(&f, &candidate, UCN_I_SECURITY_ACCESS_OUTBOUND,
        88U, 89U, 0x1201U, 0U, UINT32_C(0x12345678), 1U, &borrowed_policy.rule, 1U, &old_default) == 0);
    {
        ucn_i_security_candidate_t snapshot;
        ucn_i_security_candidate_t sentinel;
        ucn_i_security_handle_t foreign = f.handle;
        memset(&snapshot, 0xA5, sizeof(snapshot));
        CHECK(ucn_i_security_session_candidate_get(&f.owner, f.handle, &snapshot) == UCN_OK);
        CHECK(memcmp(&snapshot, &candidate, sizeof(candidate)) == 0);
        owner_before = f.owner;
        CHECK(ucn_i_security_session_candidate_get(&f.owner, f.handle,
            &borrowed_policy.candidate_sized) == UCN_ERR_ARGUMENT);
        CHECK(memcmp(&borrowed_policy.rule, &grant, sizeof(grant)) == 0);
        CHECK(ucn_i_security_peer_is_fenced(&f.owner, &candidate.local, &candidate.peer,
            (bool *)(void *)&borrowed_policy.rule) == UCN_ERR_ARGUMENT);
        CHECK(memcmp(&borrowed_policy.rule, &grant, sizeof(grant)) == 0);
        CHECK(ucn_i_security_peer_is_fenced(&f.owner, &candidate.local, &candidate.peer,
            (bool *)(void *)&f.acl) == UCN_ERR_ARGUMENT);
        CHECK(ucn_i_security_peer_is_fenced(&f.owner, &candidate.local, &candidate.peer,
            (bool *)(void *)&f.rule) == UCN_ERR_ARGUMENT);
        CHECK(ucn_i_security_session_candidate_get(&f.owner, f.handle,
            &f.owner.sessions[f.handle.slot].candidate) == UCN_ERR_ARGUMENT);
        CHECK(ucn_i_security_bind_session_acl(&f.owner, f.handle, &grant, 1U) == UCN_ERR_STATE);
        CHECK(memcmp(&f.owner, &owner_before, sizeof(owner_before)) == 0);
        memset(&snapshot, 0xA5, sizeof(snapshot)); sentinel = snapshot; foreign.slot_generation++;
        CHECK(ucn_i_security_session_candidate_get(&f.owner, foreign, &snapshot) == UCN_ERR_STATE);
        CHECK(memcmp(&snapshot, &sentinel, sizeof(snapshot)) == 0);
    }
    memset(&access, 0, sizeof(access)); access.session = f.handle;
    memcpy(access.context_fingerprint, candidate.origin_fingerprint, 16U);
    access.service_id = 0x1202U; access.direction = UCN_I_SECURITY_ACCESS_OUTBOUND;
    CHECK(ucn_i_security_authorize(&f.owner, &access, &f.facts) == UCN_OK);
    access.service_id = 0x1201U;
    memcpy(access.context_fingerprint, old_default.context_fingerprint, 16U);
    CHECK(ucn_i_security_authorize(&f.owner, &access, &f.facts) == UCN_ERR_ACCESS);
    access.service_id = 0x1202U; memcpy(access.context_fingerprint, candidate.origin_fingerprint, 16U); access.context_fingerprint[0] ^= 1U;
    CHECK(ucn_i_security_authorize(&f.owner, &access, &f.facts) == UCN_ERR_ACCESS);
    CHECK(ucn_i_security_fence(&f.owner, f.handle) == UCN_OK);
    {
        ucn_i_security_candidate_t snapshot, sentinel;
        memset(&snapshot, 0xA5, sizeof(snapshot)); sentinel = snapshot;
        CHECK(ucn_i_security_session_candidate_get(&f.owner, f.handle, &snapshot) == UCN_ERR_STATE);
        CHECK(memcmp(&snapshot, &sentinel, sizeof(snapshot)) == 0);
    }
    owner_before = f.owner;
    CHECK(ucn_i_security_bind_session_acl(&f.owner, f.handle, &grant, 1U) == UCN_ERR_STATE);
    CHECK(memcmp(&f.owner, &owner_before, sizeof(owner_before)) == 0);
    {
        ucn_i_security_candidate_t next = candidate;
        ucn_i_security_durability_base_t base = durability_make(f.rule.domain_id, 2U);
        ucn_i_security_handle_t successor;
        ucn_i_security_acl_rule_t new_grant = grant;
        ucn_i_security_acl_rule_t maximum[UCN_I_SECURITY_SESSION_ACL_MAX];
        uint16_t i;
        const uint8_t proof = 0xC3U;
        next.session_generation = 2U; ++next.origin_tx.key_generation; ++next.origin_rx.key_generation;
        next.origin_fingerprint[0] ^= 0x40U; next.transcript_digest[0] ^= 0x40U;
        CHECK(ucn_i_security_prepare_static_session(&f.owner, &next, &proof, 1U, &base, 1500U, &successor) == UCN_OK);
        CHECK(f.owner.sessions[successor.slot].session_acl == NULL);
        CHECK(f.owner.sessions[successor.slot].session_acl_count == 0U);
        owner_before = f.owner;
        CHECK(ucn_i_security_bind_session_acl(&f.owner, successor, &grant, 1U) == UCN_ERR_ARGUMENT);
        CHECK(memcmp(&f.owner, &owner_before, sizeof(owner_before)) == 0);
        memcpy(new_grant.context_fingerprint, next.origin_fingerprint, 16U);
        for (i = 0U; i < UCN_I_SECURITY_SESSION_ACL_MAX; ++i) { maximum[i] = new_grant; maximum[i].service_id = (uint16_t)(i + 1U); }
        CHECK(ucn_i_security_bind_session_acl(&f.owner, successor, maximum, UCN_I_SECURITY_SESSION_ACL_MAX) == UCN_OK);
        owner_before = f.owner;
        CHECK(ucn_i_security_borrowed_output_check(&f.owner, maximum, sizeof(maximum)) == UCN_ERR_ARGUMENT);
        CHECK(ucn_i_security_borrowed_output_check(&f.owner, &maximum[63].direction, 1U) == UCN_ERR_ARGUMENT);
        CHECK(ucn_i_security_borrowed_output_check(&f.owner, &f.acl, sizeof(f.acl)) == UCN_ERR_ARGUMENT);
        CHECK(ucn_i_security_borrowed_output_check(&f.owner, &f.rule, sizeof(f.rule)) == UCN_ERR_ARGUMENT);
        CHECK(ucn_i_security_borrowed_output_check(&f.owner, &f.owner, 1U) == UCN_ERR_ARGUMENT);
        CHECK(ucn_i_security_borrowed_output_check(&f.owner, NULL, 1U) == UCN_ERR_ARGUMENT);
        CHECK(ucn_i_security_borrowed_output_check(&f.owner, &i, 0U) == UCN_ERR_ARGUMENT);
        CHECK(ucn_i_security_borrowed_output_check(&f.owner, &i, sizeof(i)) == UCN_OK);
        CHECK(memcmp(&f.owner, &owner_before, sizeof(owner_before)) == 0);
        CHECK(ucn_i_security_fence(&f.owner, successor) == UCN_OK);
        CHECK(ucn_i_security_borrowed_output_check(&f.owner, maximum, sizeof(maximum)) == UCN_ERR_ARGUMENT);
        CHECK(ucn_i_security_retire_fenced(&f.owner, successor) == UCN_OK);
        CHECK(ucn_i_security_borrowed_output_check(&f.owner, maximum, sizeof(maximum)) == UCN_OK);
    }
    CHECK(fixture_destroy(&f) == 0);
    CHECK(fixture_activate_profile_policy(&f, &candidate, UCN_I_SECURITY_ACCESS_OUTBOUND,
        90U, 91U, 0x1201U, 0U, UINT32_C(0x12345678), 1U, NULL, 0U, &old_default) == 0);
    access.session = f.handle; access.service_id = 0x1201U;
    memcpy(access.context_fingerprint, candidate.origin_fingerprint, 16U);
    CHECK(ucn_i_security_authorize(&f.owner, &access, &f.facts) == UCN_ERR_ACCESS);
    CHECK(fixture_destroy(&f) == 0);
    return 0;
}

static int test_local_c0_retry_refresh_preserves_origin(void)
{
    static session_fixture_t f;
    static ucn_i_security_packet_workspace_t workspace;
    static ucn_i_security_owner_t before;
    ucn_i_security_candidate_t candidate = candidate_make(0x20U, 1U);
    ucn_i_security_acl_rule_t grants[2];
    ucn_i_security_c0_protect_request_t request;
    ucn_i_c0_frame_t frame;
    uint8_t packet[UCN_ADAPTER_FRAME_BYTES], output[UCN_ADAPTER_FRAME_BYTES];
    const uint8_t plaintext[3] = {1U, 2U, 3U};
    size_t bytes = 0U, written = SIZE_MAX;
    unsigned origin_calls;
    candidate.hop_profile = UCN_I_HOP_PROFILE_H1;
    candidate.hop_tx.suite_id = candidate.hop_rx.suite_id = 16U;
    candidate.hop_tx.key_id = 11U; candidate.hop_rx.key_id = 12U;
    candidate.hop_tx.key_generation = 41U; candidate.hop_rx.key_generation = 42U;
    grants[0] = acl_make(&candidate, UCN_I_SECURITY_ACCESS_OUTBOUND);
    grants[0].service_id = 0U; grants[0].protocol_opcode = UCN_I_OPCODE_DIAGNOSTIC_PING;
    grants[1] = grants[0]; memcpy(grants[1].context_fingerprint, candidate.hop_fingerprint, 16U);
    CHECK(fixture_activate_profile_policy(&f, &candidate, UCN_I_SECURITY_ACCESS_OUTBOUND,
        88U, 89U, 0U, UCN_I_OPCODE_DIAGNOSTIC_PING, UINT32_C(0x12345678), 1U, grants, 2U, NULL) == 0);
    memset(&request, 0, sizeof(request)); request.session = f.handle; request.facts = f.facts;
    memcpy(request.context_fingerprint, candidate.origin_fingerprint, 16U);
    request.transaction_id = 100U; request.protocol_opcode = UCN_I_OPCODE_DIAGNOSTIC_PING;
    request.traffic_class = UCN_TRAFFIC_Q0; request.delivery_guarantee = UCN_DELIVERY_RELIABLE;
    request.interaction_role = UCN_INTERACTION_REQUEST; request.payload_kind = UCN_I_PAYLOAD_DIAGNOSTIC;
    request.hop_limit = 2U; request.direction = UCN_I_SECURITY_ACCESS_OUTBOUND;
    CHECK(ucn_i_security_c0_protect(&f.owner, &request, plaintext, sizeof(plaintext),
        &workspace, packet, sizeof(packet), &bytes) == UCN_OK);
    {
        ucn_i_security_acl_rule_t prior_grants[2]; unsigned calls = f.provider.calls;
        memcpy(prior_grants, grants, sizeof(grants)); before = f.owner;
        CHECK(ucn_i_security_c0_refresh_hop(&f.owner, f.handle, &f.facts, packet, bytes,
            &workspace, (uint8_t *)grants, sizeof(grants), &written) == UCN_ERR_ARGUMENT);
        CHECK(memcmp(grants, prior_grants, sizeof(grants)) == 0 && written == SIZE_MAX);
        CHECK(memcmp(&f.owner, &before, sizeof(before)) == 0 && f.provider.calls == calls);
        CHECK(ucn_i_security_c0_refresh_hop(&f.owner, f.handle, &f.facts, packet, bytes,
            &workspace, output, sizeof(output), (size_t *)(void *)grants[0].context_fingerprint) == UCN_ERR_ARGUMENT);
        CHECK(memcmp(grants, prior_grants, sizeof(grants)) == 0);
        CHECK(memcmp(&f.owner, &before, sizeof(before)) == 0 && f.provider.calls == calls);
    }
    origin_calls = f.provider.protect_origin_calls;
    CHECK(ucn_i_security_c0_refresh_hop(&f.owner, f.handle, &f.facts, packet, bytes,
        &workspace, output, sizeof(output), &written) == UCN_OK);
    CHECK(written == bytes && memcmp(packet, output, bytes - UCN_I_SECURITY_HOP_TRAILER_BYTES) == 0);
    CHECK(f.provider.protect_origin_calls == origin_calls);
    CHECK(ucn_i_c0_decode(output, written, 1U, UCN_I_CONTROL_H1, &frame) == UCN_OK);
    CHECK(frame.hop_sequence == 2U);
    {
        uint8_t tampered[UCN_ADAPTER_FRAME_BYTES];
        ucn_i_c0_frame_t original, bad;
        ucn_i_security_current_facts_t wrong_facts = f.facts;
        ucn_i_security_handle_t foreign = f.handle;
        size_t tampered_bytes; unsigned i;
        CHECK(ucn_i_c0_decode(packet, bytes, 1U, UCN_I_CONTROL_H1, &original) == UCN_OK);
        for (i = 0U; i < 9U; ++i) {
            bad = original;
            switch (i) {
            case 0U: ++bad.realm_id; break;
            case 1U: ++bad.source_address; break;
            case 2U: ++bad.destination_address; break;
            case 3U: ++bad.source_binding_generation; break;
            case 4U: ++bad.destination_binding_generation; break;
            case 5U: bad.opcode = UCN_I_OPCODE_DIAGNOSTIC_PONG; break;
            case 6U: ++bad.transaction_id; break;
            case 7U: bad.hop_sequence = 99U; break;
            default: bad.origin_security = UCN_I_ORIGIN_O2; break;
            }
            CHECK(ucn_i_c0_encode(&bad, 1U, UCN_I_CONTROL_H1, tampered, sizeof(tampered), &tampered_bytes) == UCN_OK);
            memset(output, 0xA5, sizeof(output)); written = SIZE_MAX; before = f.owner;
            CHECK(ucn_i_security_c0_refresh_hop(&f.owner, f.handle, &f.facts, tampered, tampered_bytes,
                &workspace, output, sizeof(output), &written) != UCN_OK);
            CHECK(memcmp(&f.owner, &before, sizeof(before)) == 0 && written == SIZE_MAX);
            CHECK(output[0] == 0xA5U && output[sizeof(output) - 1U] == 0xA5U);
        }
        ++wrong_facts.link_generation; ++foreign.slot_generation;
        CHECK(ucn_i_security_c0_refresh_hop(&f.owner, f.handle, &wrong_facts, packet, bytes,
            &workspace, output, sizeof(output), &written) == UCN_ERR_STATE);
        CHECK(ucn_i_security_c0_refresh_hop(&f.owner, foreign, &f.facts, packet, bytes,
            &workspace, output, sizeof(output), &written) == UCN_ERR_STATE);
        CHECK(ucn_i_security_c0_refresh_hop(&f.owner, f.handle, &f.facts, packet, bytes,
            &workspace, output, 1U, &written) == UCN_ERR_NO_SPACE);
        CHECK(memcmp(&f.owner, &before, sizeof(before)) == 0 && written == SIZE_MAX);
        CHECK(ucn_i_security_c0_refresh_hop(&f.owner, f.handle, &f.facts, packet, bytes,
            &workspace, output, sizeof(output), &written) == UCN_OK);
        CHECK(memcmp(packet, output, bytes - UCN_I_SECURITY_HOP_TRAILER_BYTES) == 0);
        CHECK(ucn_i_c0_decode(output, written, 1U, UCN_I_CONTROL_H1, &frame) == UCN_OK && frame.hop_sequence == 3U);
    }
    before = f.owner;
    CHECK(ucn_i_security_c0_refresh_hop(&f.owner, f.handle, &f.facts, packet, bytes,
        &workspace, packet, sizeof(packet), &written) == UCN_ERR_ARGUMENT);
    CHECK(memcmp(&f.owner, &before, sizeof(before)) == 0);
    CHECK(fixture_destroy(&f) == 0);
    return 0;
}

static int test_signed_reauth_uses_both_histories_without_faking_local_prior(void)
{
    static session_fixture_t f;
    static ucn_i_security_reauth_workspace_t work;
    static ucn_i_security_owner_t before;
    static union {
        ucn_i_security_reauth_workspace_t workspace_sized;
        ucn_i_security_acl_rule_t rule;
    } borrowed_policy, policy_before;
    ucn_i_security_candidate_t previous = candidate_make(0x20U, 1U), next;
    ucn_i_peer_reauth_record_t record;
    ucn_i_security_reauth_call_t call;
    ucn_i_security_durability_base_t base = durability_make(0x8001U, 2U);
    ucn_i_security_handle_t successor, sentinel;
    uint8_t local_record[192], message[227], challenge[16], response[16], digest[32];
    ucn_i_sha256_workspace_t hash;
    uint8_t *b = record.canonical;
    bool fenced = true;
    const uint8_t old_proof = 0xC3U;
    previous.origin_tx.key_id = 1U; previous.origin_rx.key_id = 2U;
    previous.origin_tx.key_generation = previous.origin_rx.key_generation = 1U;
    previous.hop_profile = UCN_I_HOP_PROFILE_H1;
    previous.hop_tx.suite_id = previous.hop_rx.suite_id = 16U;
    previous.hop_tx.key_id = 3U; previous.hop_rx.key_id = 4U;
    previous.hop_tx.key_generation = previous.hop_rx.key_generation = 1U;
    memset(&borrowed_policy, 0, sizeof(borrowed_policy));
    borrowed_policy.rule = acl_make(&previous, UCN_I_SECURITY_ACCESS_OUTBOUND);
    CHECK(fixture_activate_profile_policy(&f, &previous, UCN_I_SECURITY_ACCESS_OUTBOUND,
        88U, 89U, 0x1201U, 0U, UINT32_C(0x12345678), 1U, &borrowed_policy.rule, 1U, NULL) == 0);
    CHECK(ucn_i_security_peer_is_fenced(&f.owner, &previous.local, &previous.peer, &fenced) == UCN_OK && !fenced);
    memcpy(local_record, f.owner.sessions[f.handle.slot].body, sizeof(local_record));
    CHECK(ucn_i_security_fence(&f.owner, f.handle) == UCN_OK);
    CHECK(ucn_i_security_peer_is_fenced(&f.owner, &previous.local, &previous.peer, &fenced) == UCN_OK && fenced);
    memset(&record, 0, sizeof(record)); b[0] = 2U; b[1] = 2U; b[2] = 1U;
    test_write_be(b + 4U, f.owner.realm_id, 4U);
    test_write_be(b + 8U, previous.local.address, 4U); test_write_be(b + 12U, previous.local.binding_generation, 4U); memcpy(b + 16U, previous.local.principal, 16U);
    test_write_be(b + 32U, previous.peer.address, 4U); test_write_be(b + 36U, previous.peer.binding_generation, 4U); memcpy(b + 40U, previous.peer.principal, 16U);
    test_write_be(b + 56U, 1U, 4U); test_write_be(b + 60U, 3U, 4U);
    b[64] = 1U; b[65] = 16U; test_write_be(b + 66U, 1U, 2U); test_write_be(b + 68U, 3U, 2U);
    test_write_be(b + 72U, 3U, 4U); test_write_be(b + 76U, 3U, 4U);
    test_write_be(b + 80U, 1U, 2U); test_write_be(b + 82U, 2U, 2U);
    test_write_be(b + 84U, previous.link_generation, 4U); test_write_be(b + 88U, 12U, 4U);
    test_write_be(b + 92U, 100U, 8U); test_write_be(b + 100U, 101U, 8U); test_write_be(b + 108U, 102U, 8U); test_write_be(b + 116U, 2000U, 8U);
    memcpy(b + 124U, previous.transcript_digest, 32U);
    test_write_be(b + 156U, 1U, 4U); test_write_be(b + 160U, 1U, 4U);
    test_write_be(b + 164U, 2U, 4U); test_write_be(b + 168U, 2U, 4U); test_write_be(b + 172U, 2U, 4U); memset(b + 176U, 0x91, 32U);
    memcpy(message, "UCN6-PEER-REAUTH-V2", 19U); memcpy(message + 19U, b, 208U);
    CHECK(ucn_i_sha256_256(message, sizeof(message), digest, &hash) == UCN_OK);
    next = previous; next.session_generation = 3U; next.expires_at_us = 3000U;
    next.origin_tx.key_generation = next.origin_rx.key_generation = 3U;
    next.hop_tx.key_generation = next.hop_rx.key_generation = 3U;
    memcpy(next.origin_fingerprint, digest, 16U); memcpy(next.hop_fingerprint, digest + 16U, 16U); memcpy(next.transcript_digest, digest, 32U);
    message[20] = 1U; reauth_identity_sign(previous.peer.principal, message, challenge);
    message[20] = 2U; reauth_identity_sign(previous.local.principal, message, response);
    memset(&call, 0, sizeof(call)); call.candidate = &next; call.previous = &previous;
    call.local_record = local_record; call.record = &record; call.durability = &base;
    call.challenge_proof = challenge; call.response_proof = response;
    call.challenge_proof_bytes = call.response_proof_bytes = 16U;
    call.verify_identity = reauth_identity_verify; call.verify_context = &f.provider;
    call.started_us = 1000U; call.parent_deadline_us = 4500U; call.now_us = 1500U; call.initiator = 1U;
    f.provider.reauth_record = &record; f.provider.reauth_candidate = &next;
    memset(&successor, 0xA5, sizeof(successor)); sentinel = successor; before = f.owner;
    CHECK(ucn_i_security_prepare_static_session(&f.owner, &next, &old_proof, 1U, &base, 1500U, &successor) == UCN_ERR_STATE);
    CHECK(memcmp(&f.owner, &before, sizeof(before)) == 0);
    policy_before = borrowed_policy;
    CHECK(ucn_i_security_prepare_reauth_session(&f.owner, &call,
        &borrowed_policy.workspace_sized, &successor) == UCN_ERR_ARGUMENT);
    CHECK(memcmp(&borrowed_policy, &policy_before, sizeof(borrowed_policy)) == 0);
    CHECK(ucn_i_security_prepare_reauth_session(&f.owner, &call, &work,
        (ucn_i_security_handle_t *)(void *)&borrowed_policy) == UCN_ERR_ARGUMENT);
    CHECK(ucn_i_security_prepare_reauth_session(&f.owner, &call, &work,
        (ucn_i_security_handle_t *)(void *)&f.acl) == UCN_ERR_ARGUMENT);
    CHECK(ucn_i_security_prepare_reauth_session(&f.owner, &call, &work,
        (ucn_i_security_handle_t *)(void *)&f.rule) == UCN_ERR_ARGUMENT);
    CHECK(memcmp(&borrowed_policy, &policy_before, sizeof(borrowed_policy)) == 0);
    CHECK(memcmp(&f.owner, &before, sizeof(before)) == 0 && f.provider.reauth_verify_calls == 0U);
    {
        static const uint16_t changed[] = {0, 1, 2, 3, 4, 8, 12, 16, 32, 36, 40, 56, 60,
            64, 65, 66, 68, 70, 72, 76, 80, 82, 84, 88, 92, 100, 108, 116, 124, 156, 160, 164, 168, 172, 176};
        size_t i;
        for (i = 0U; i < sizeof(changed) / sizeof(changed[0]); ++i) {
            b[changed[i]] ^= 1U;
            CHECK(ucn_i_security_prepare_reauth_session(&f.owner, &call, &work, &successor) != UCN_OK);
            CHECK(memcmp(&f.owner, &before, sizeof(before)) == 0 && memcmp(&successor, &sentinel, sizeof(sentinel)) == 0);
            b[changed[i]] ^= 1U;
        }
        call.challenge_proof_bytes = 15U;
        CHECK(ucn_i_security_prepare_reauth_session(&f.owner, &call, &work, &successor) == UCN_ERR_ARGUMENT);
        call.challenge_proof_bytes = 65U;
        CHECK(ucn_i_security_prepare_reauth_session(&f.owner, &call, &work, &successor) == UCN_ERR_ARGUMENT);
        call.challenge_proof_bytes = 16U;
        call.local_record = work.message;
        CHECK(ucn_i_security_prepare_reauth_session(&f.owner, &call, &work, &successor) == UCN_ERR_ARGUMENT);
        call.local_record = local_record;
        CHECK(ucn_i_security_prepare_reauth_session(&f.owner, &call, &work, (ucn_i_security_handle_t *)&next) == UCN_ERR_ARGUMENT);
        CHECK(ucn_i_security_prepare_reauth_session(&f.owner, &call, (ucn_i_security_reauth_workspace_t *)&f.owner, &successor) == UCN_ERR_ARGUMENT);
        call.now_us = 3000U;
        CHECK(ucn_i_security_prepare_reauth_session(&f.owner, &call, &work, &successor) != UCN_OK);
        call.now_us = 1500U;
        base.prior_session_generation = 2U;
        CHECK(ucn_i_security_prepare_reauth_session(&f.owner, &call, &work, &successor) != UCN_OK);
        base.prior_session_generation = 1U;
        CHECK(memcmp(&f.owner, &before, sizeof(before)) == 0 && f.provider.reauth_verify_calls == 0U);
    }
    CHECK(ucn_i_security_prepare_reauth_session(&f.owner, &call, &work, &successor) == UCN_OK);
    CHECK(base.prior_session_generation == 1U && f.provider.reauth_verify_calls == 2U);
    CHECK(f.owner.sessions[successor.slot].candidate.session_generation == 3U);
    CHECK(ucn_i_security_fence(&f.owner, successor) == UCN_OK);
    CHECK(ucn_i_security_retire_fenced(&f.owner, successor) == UCN_OK);
    successor = sentinel; challenge[0] ^= 1U;
    CHECK(ucn_i_security_prepare_reauth_session(&f.owner, &call, &work, &successor) == UCN_ERR_SECURITY);
    CHECK(memcmp(&successor, &sentinel, sizeof(sentinel)) == 0); challenge[0] ^= 1U;
    response[0] ^= 1U;
    CHECK(ucn_i_security_prepare_reauth_session(&f.owner, &call, &work, &successor) == UCN_ERR_SECURITY);
    CHECK(memcmp(&successor, &sentinel, sizeof(sentinel)) == 0); response[0] ^= 1U;
    before = f.owner; local_record[148] ^= 1U;
    CHECK(ucn_i_security_prepare_reauth_session(&f.owner, &call, &work, &successor) != UCN_OK);
    CHECK(memcmp(&f.owner, &before, sizeof(before)) == 0); local_record[148] ^= 1U;
#if UCN_I_SECURITY_SESSION_COUNT >= 3
    {
        ucn_i_security_handle_t tombstone;
        CHECK(ucn_i_security_prepare_reauth_session(&f.owner, &call, &work, &tombstone) == UCN_OK);
        CHECK(ucn_i_security_fence(&f.owner, tombstone) == UCN_OK);
        /* A newer fenced slot than the supplied local Foundation baseline
         * still rejects, even when both signatures are otherwise valid. */
        before = f.owner;
        CHECK(ucn_i_security_prepare_reauth_session(&f.owner, &call, &work, &successor) == UCN_ERR_STATE);
        CHECK(memcmp(&f.owner, &before, sizeof(before)) == 0);
        previous = next;
        memcpy(local_record, f.owner.sessions[tombstone.slot].body, sizeof(local_record));
        base = durability_make(0x8001U, 4U);
        test_write_be(b + 56U, 3U, 4U); test_write_be(b + 60U, 4U, 4U);
        test_write_be(b + 156U, 3U, 4U); test_write_be(b + 160U, 3U, 4U);
        test_write_be(b + 72U, 4U, 4U); test_write_be(b + 76U, 4U, 4U);
        memcpy(b + 124U, previous.transcript_digest, 32U);
        memcpy(message + 19U, b, 208U);
        CHECK(ucn_i_sha256_256(message, sizeof(message), digest, &hash) == UCN_OK);
        next.session_generation = 4U;
        next.origin_tx.key_generation = next.origin_rx.key_generation = 4U;
        next.hop_tx.key_generation = next.hop_rx.key_generation = 4U;
        memcpy(next.origin_fingerprint, digest, 16U); memcpy(next.hop_fingerprint, digest + 16U, 16U); memcpy(next.transcript_digest, digest, 32U);
        message[20] = 1U; reauth_identity_sign(previous.peer.principal, message, challenge);
        message[20] = 2U; reauth_identity_sign(previous.local.principal, message, response);
        CHECK(ucn_i_security_prepare_static_session(&f.owner, &next, &old_proof, 1U, &base, 1500U, &successor) == UCN_ERR_STATE);
        CHECK(ucn_i_security_prepare_reauth_session(&f.owner, &call, &work, &successor) == UCN_OK);
        CHECK(ucn_i_security_fence(&f.owner, successor) == UCN_OK);
        CHECK(ucn_i_security_retire_fenced(&f.owner, successor) == UCN_OK);
        CHECK(ucn_i_security_retire_fenced(&f.owner, tombstone) == UCN_OK);
    }
#endif
    CHECK(sizeof(work) <= 512U);
    printf("reauth workspace=%zu descriptor=%zu bytes\n", sizeof(work), sizeof(call));
    CHECK(fixture_destroy(&f) == 0);
    return 0;
}

int main(void)
{
    CHECK(test_signed_reauth_uses_both_histories_without_faking_local_prior() == 0);
    CHECK(test_local_c0_retry_refresh_preserves_origin() == 0);
    CHECK(test_explicit_pending_session_policy_replaces_defaults() == 0);
    CHECK(test_fenced_successor_requires_exact_prior() == 0);
    CHECK(test_peer_reauth_codec() == 0);
    CHECK(test_record_codec() == 0);
    CHECK(test_session_prepare_and_lifecycle() == 0);
    CHECK(test_preflight_fail_closed() == 0);
    CHECK(test_packet_roundtrip() == 0);
    CHECK(run_c1_origin_projection(UCN_I_SECURITY_AUTHENTICATED, 1U) == 0);
    CHECK(run_c1_origin_projection(UCN_I_SECURITY_CONFIDENTIAL, 2U) == 0);
    CHECK(run_c1_origin_projection(UCN_I_SECURITY_CONFIDENTIAL, 3U) == 0);
    CHECK(test_c0_authenticated_control_fact() == 0);
    CHECK(test_c0_confidential_hop_protected_fact() == 0);
    CHECK(test_c0_protect_roundtrips() == 0);
    CHECK(test_c0_multihop_split_origin_and_hop_contexts() == 0);
    CHECK(test_session_capacity() == 0);
    puts("ucn simplified security session tests: PASS");
    return 0;
}
