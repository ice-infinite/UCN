#include "native_join_fixture.h"
#include <string.h>

static ucn_result_t fixture_lock_enter(void *context)
{
    native_join_fixture_lock_t *lock = context;
    if (lock == NULL || lock->held != 0U) return UCN_ERR_STATE;
    lock->held = 1U;
    return UCN_OK;
}

static void fixture_lock_leave(void *context)
{
    native_join_fixture_lock_t *lock = context;
    lock->held = 0U;
}

static ucn_lock_ops_t fixture_lock(native_join_fixture_lock_t *state)
{
    ucn_lock_ops_t ops;
    memset(&ops, 0, sizeof(ops));
    ops.struct_size = sizeof(ops);
    ops.api_version = UCN_API_VERSION;
    ops.context = state;
    ops.enter = fixture_lock_enter;
    ops.leave = fixture_lock_leave;
    return ops;
}

static ucn_i_lock_ops_t fixture_internal_lock(native_join_fixture_lock_t *state)
{
    ucn_i_lock_ops_t ops;
    memset(&ops, 0, sizeof(ops)); ops.struct_size = sizeof(ops);
    ops.api_version = UCN_I_LOCK_OPS_VERSION; ops.context = state;
    ops.enter = fixture_lock_enter; ops.leave = fixture_lock_leave;
    return ops;
}

static ucn_driver_submit_result_t fixture_submit(void *context, ucn_link_handle_t link,
    const uint8_t *bytes, size_t count, ucn_driver_token_t token)
{
    native_join_fixture_t *f = context;
    (void)link; (void)token;
    ++f->submit_calls;
    if (f->driver_mode == 1U || f->captured_bytes != 0U || count > sizeof(f->captured)) return UCN_DRIVER_NOT_SUBMITTED;
    memcpy(f->captured, bytes, count); f->captured_bytes = count;
    return f->driver_mode == 2U ? UCN_DRIVER_SUBMIT_UNKNOWN : UCN_DRIVER_COMPLETE;
}
static ucn_result_t fixture_cancel(void *context, ucn_driver_token_t token)
{ native_join_fixture_t *f = context; (void)token; return f->cancel_result; }
static ucn_result_t fixture_authority(void *context, uint64_t now, ucn_i_identity_authority_view_t *out)
{
    native_join_fixture_t *f = context;
    if (f == NULL || out == NULL || now == 0U || now >= f->authority.local_deadline_us) return UCN_ERR_TIMEOUT;
    *out = f->authority; return UCN_OK;
}
static ucn_result_t fixture_cookie_issue(void *context, const ucn_i_admission_hello_t *hello,
    ucn_i_admission_link_t link, uint32_t bucket, uint8_t output[16], uint8_t *output_bytes)
{
    native_join_fixture_t *f = context;
    ucn_result_t r;
    if (!f || !hello || !output || !output_bytes || !bucket) return UCN_ERR_ARGUMENT;
    r = ucn_i_admission_hello_encode(hello, f->logical);
    if (r != UCN_OK) return r;
    memset(f->hash_input, 0x36 ^ 0x91, 32U); memset(f->hash_input + 32, 0x36, 32U);
    memcpy(f->hash_input + 64, f->logical, 40U);
    f->hash_input[104] = (uint8_t)(link.id >> 8); f->hash_input[105] = (uint8_t)link.id;
    f->hash_input[106] = (uint8_t)(link.generation >> 24); f->hash_input[107] = (uint8_t)(link.generation >> 16);
    f->hash_input[108] = (uint8_t)(link.generation >> 8); f->hash_input[109] = (uint8_t)link.generation;
    f->hash_input[110] = (uint8_t)(bucket >> 24); f->hash_input[111] = (uint8_t)(bucket >> 16);
    f->hash_input[112] = (uint8_t)(bucket >> 8); f->hash_input[113] = (uint8_t)bucket;
    f->hash_input[114] = (uint8_t)(f->config.realm_id >> 24); f->hash_input[115] = (uint8_t)(f->config.realm_id >> 16);
    f->hash_input[116] = (uint8_t)(f->config.realm_id >> 8); f->hash_input[117] = (uint8_t)f->config.realm_id;
    r = ucn_i_sha256_256(f->hash_input, 118U, f->hash_inner, &f->hash_workspace);
    if (r != UCN_OK) return r;
    memset(f->hash_input, 0x5c ^ 0x91, 32U); memset(f->hash_input + 32, 0x5c, 32U);
    memcpy(f->hash_input + 64, f->hash_inner, 32U);
    r = ucn_i_sha256_256(f->hash_input, 96U, f->logical, &f->hash_workspace);
    if (r != UCN_OK) return r;
    output[0] = (uint8_t)(bucket >> 24); output[1] = (uint8_t)(bucket >> 16);
    output[2] = (uint8_t)(bucket >> 8); output[3] = (uint8_t)bucket;
    memcpy(output + 4U, f->logical, 12U); *output_bytes = 16U;
    return UCN_OK;
}
static ucn_result_t fixture_cookie_verify(void *context, const ucn_i_admission_hello_cookie_t *e, uint64_t now)
{
    static const uint8_t domain[] = "UCN6-BOOTSTRAP-COOKIE-V1";
    native_join_fixture_t *f = context;
    ucn_i_admission_hello_t hello;
    ucn_i_admission_cookie_challenge_t challenge;
    ucn_i_c0_frame_t frame;
    uint8_t expected[16], expected_bytes, diff = 0U, payload[40], input[196], digest[32];
    uint32_t bucket; uint64_t current; size_t i, a, b, offset = sizeof(domain) - 1U + 4U;
    if (!f || !e || !now || e->cookie_bytes != 16U || f->config.cookie_bucket_us == 0U) return UCN_ERR_SECURITY;
    bucket = ((uint32_t)e->cookie[0] << 24) | ((uint32_t)e->cookie[1] << 16) | ((uint32_t)e->cookie[2] << 8) | e->cookie[3];
    current = now / f->config.cookie_bucket_us + 1U;
    if (bucket == 0U || current > UINT32_MAX || bucket > current || current - bucket > 1U) return UCN_ERR_SECURITY;
    memset(&hello, 0, sizeof(hello)); hello.transaction_id = e->key.transaction_id;
    hello.device_nonce = e->device_nonce; memcpy(hello.identity_digest, e->key.identity_digest, 16U);
    if (fixture_cookie_issue(f, &hello, e->key.link, bucket, expected, &expected_bytes) != UCN_OK || expected_bytes != 16U)
        return UCN_ERR_SECURITY;
    for (i = 0; i < 16U; ++i) diff |= e->cookie[i] ^ expected[i];
    if (diff != 0U) return UCN_ERR_SECURITY;
    memset(&challenge, 0, sizeof(challenge)); challenge.transaction_id = hello.transaction_id;
    challenge.cookie_time_bucket = bucket; challenge.issuer_link = e->key.link;
    challenge.cookie_bytes = 16U; memcpy(challenge.cookie, e->cookie, 16U);
    memset(&frame, 0, sizeof(frame)); frame.transaction_id = hello.transaction_id;
    frame.realm_id = f->config.realm_id; frame.destination_address = UINT32_C(65535);
    frame.opcode = 1U; frame.interaction_role = 1U; frame.payload_kind = UCN_I_PAYLOAD_CONTROL; frame.hop_limit = 1U;
    frame.payload = payload; frame.payload_bytes = 40U;
    memcpy(input, domain, sizeof(domain) - 1U);
    if (ucn_i_admission_hello_encode(&hello, payload) != UCN_OK ||
        ucn_i_c0_encode(&frame, 2U, 0U, input + offset, sizeof(input) - offset, &a) != UCN_OK ||
        ucn_i_admission_cookie_challenge_encode(&challenge, payload) != UCN_OK) return UCN_ERR_SECURITY;
    frame.opcode = 2U; frame.interaction_role = 2U;
    if (ucn_i_c0_encode(&frame, 2U, 0U, input + offset + a, sizeof(input) - offset - a, &b) != UCN_OK) return UCN_ERR_SECURITY;
    input[sizeof(domain)-1U] = 0U; input[sizeof(domain)] = 0U; input[sizeof(domain)+1U] = 0U;
    input[sizeof(domain)+2U] = (uint8_t)(a + b);
    if (ucn_i_sha256_256(input, offset + a + b, digest, &f->hash_workspace) != UCN_OK) return UCN_ERR_SECURITY;
    for (i = 0U; i < 32U; ++i) diff |= digest[i] ^ e->prior_messages_hash[i];
    return diff == 0U ? UCN_OK : UCN_ERR_SECURITY;
}
/* Host-only authenticated plaintext. This is not encryption and rejects suites
 * other than the fixture's auth-only suite. Tags cover selector and content. */
static ucn_result_t fixture_packet_mac(native_join_fixture_t *f, uint8_t domain,
    ucn_i_security_key_selector_t selector, const uint8_t *a, size_t an,
    const uint8_t *b, size_t bn, const uint8_t *c, size_t cn, uint8_t out[32])
{
    uint8_t *p = f->packet_hash_input; size_t at = 64U; ucn_result_t r;
    if ((an && !a) || (bn && !b) || (cn && !c) || an > sizeof(f->packet_hash_input) - 84U ||
        bn > sizeof(f->packet_hash_input) - 84U - an || cn > sizeof(f->packet_hash_input) - 84U - an - bn)
        return UCN_ERR_ARGUMENT;
    memset(p, 0x36 ^ 0x42, 32U); memset(p + 32U, 0x36, 32U);
    p[at++] = domain; p[at++] = selector.suite_id; p[at++] = (uint8_t)(selector.key_id >> 8);
    p[at++] = (uint8_t)selector.key_id; p[at++] = (uint8_t)(selector.key_generation >> 24);
    p[at++] = (uint8_t)(selector.key_generation >> 16); p[at++] = (uint8_t)(selector.key_generation >> 8);
    p[at++] = (uint8_t)selector.key_generation;
    p[at++] = (uint8_t)(an >> 8); p[at++] = (uint8_t)an;
    p[at++] = (uint8_t)(bn >> 8); p[at++] = (uint8_t)bn;
    p[at++] = (uint8_t)(cn >> 8); p[at++] = (uint8_t)cn;
    if (an) { memcpy(p + at, a, an); at += an; }
    if (bn) { memcpy(p + at, b, bn); at += bn; }
    if (cn) { memcpy(p + at, c, cn); at += cn; }
    r = ucn_i_sha256_256(p, at, f->hash_inner, &f->hash_workspace); if (r != UCN_OK) return r;
    memset(p, 0x5c ^ 0x42, 32U); memset(p + 32U, 0x5c, 32U);
    memcpy(p + 64U, f->hash_inner, 32U);
    return ucn_i_sha256_256(p, 96U, out, &f->hash_workspace);
}
static ucn_result_t fixture_protect_origin(void *c, const ucn_i_security_origin_protect_request_t *r)
{
    native_join_fixture_t *f = c; uint8_t mac[32]; ucn_result_t result;
    if (!f || !r || r->selector.suite_id != 1U || !r->origin_tag || (r->payload_bytes && (!r->plaintext || !r->protected_payload))) return UCN_ERR_UNSUPPORTED;
    result = fixture_packet_mac(f, 1U, r->selector, r->nonce_input, r->nonce_input_bytes, r->aad, r->aad_bytes, r->plaintext, r->payload_bytes, mac);
    if (result != UCN_OK) return result;
    if (r->payload_bytes) memmove(r->protected_payload, r->plaintext, r->payload_bytes);
    memcpy(r->origin_tag, mac, 16U); return UCN_OK;
}
static ucn_result_t fixture_open_origin(void *c, const ucn_i_security_origin_open_request_t *r)
{
    native_join_fixture_t *f = c; uint8_t mac[32], diff = 0; size_t i; ucn_result_t result;
    if (!f || !r || r->selector.suite_id != 1U || !r->origin_tag || (r->payload_bytes && (!r->plaintext || !r->protected_payload))) return UCN_ERR_UNSUPPORTED;
    result = fixture_packet_mac(f, 1U, r->selector, r->nonce_input, r->nonce_input_bytes, r->aad, r->aad_bytes, r->protected_payload, r->payload_bytes, mac);
    if (result != UCN_OK) return result;
    for (i = 0; i < 16U; ++i) diff |= mac[i] ^ r->origin_tag[i];
    if (diff) return UCN_ERR_SECURITY;
    if (r->payload_bytes) memmove(r->plaintext, r->protected_payload, r->payload_bytes);
    return UCN_OK;
}
static ucn_result_t fixture_protect_hop(void *c, const ucn_i_security_hop_request_t *r)
{
    native_join_fixture_t *f = c; uint8_t mac[32]; ucn_result_t result;
    if (!f || !r || !r->hop_tag || r->selector.suite_id != 16U) return UCN_ERR_UNSUPPORTED;
    result = fixture_packet_mac(f, 2U, r->selector, r->aad, r->aad_bytes, NULL, 0U, NULL, 0U, mac);
    if (result == UCN_OK) memcpy(r->hop_tag, mac, 12U);
    return result;
}
static ucn_result_t fixture_verify_hop(void *c, const ucn_i_security_hop_verify_request_t *r)
{
    native_join_fixture_t *f = c; uint8_t mac[32], diff = 0; size_t i; ucn_result_t result;
    if (!f || !r || !r->hop_tag || r->selector.suite_id != 16U) return UCN_ERR_UNSUPPORTED;
    result = fixture_packet_mac(f, 2U, r->selector, r->aad, r->aad_bytes, NULL, 0U, NULL, 0U, mac);
    if (result != UCN_OK) return result;
    for (i = 0; i < 12U; ++i) diff |= mac[i] ^ r->hop_tag[i];
    return diff ? UCN_ERR_SECURITY : UCN_OK;
}

ucn_result_t native_join_fixture_foundation_init(native_join_fixture_t *f)
{
    uint8_t digest[16];
    uint16_t i;
    ucn_result_t result;
    ucn_lock_ops_t gate_lock;
    if (f == NULL) return UCN_ERR_ARGUMENT;
    if (UCN_PERSIST_DOMAIN_COUNT < 3U) return UCN_ERR_POLICY;
    memset(f, 0, sizeof(*f));
    for (i = 0U; i < 3U; ++i) {
        ucn_persist_manifest_entry_t *entry = &f->entries[i];
        ucn_persist_domain_binding_t *binding = &f->bindings[i];
        entry->struct_size = sizeof(*entry);
        entry->api_version = UCN_PERSIST_API_VERSION;
        entry->domain.domain_kind = i < 2U ? UCN_PERSIST_DOMAIN_IDENTITY_BINDING : UCN_PERSIST_DOMAIN_SECURITY_HIGH_WATER;
        entry->domain.domain_id = UINT64_C(0x9001) + i;
        entry->body_capacity_bytes = UCN_PERSIST_BODY_BYTES;
        entry->slot_capacity_bytes = UCN_PERSIST_SLOT_BYTES;
        entry->schema_id = i < 2U ? UCN_I_IDENTITY_BINDING_SCHEMA_ID : UCN_I_SECURITY_RECORD_SCHEMA_ID;
        entry->schema_version = i < 2U ? UCN_I_IDENTITY_BINDING_RECORD_SCHEMA : UCN_I_SECURITY_RECORD_SCHEMA;
        entry->digest_suite = UCN_PERSIST_DIGEST_BLAKE2S_128;
        entry->witness_policy = UCN_PERSIST_WITNESS_INDEPENDENT_MONOTONIC;
        entry->provider_atomicity_class = UCN_PERSIST_ATOMIC_COMMIT_MARKER_16;
        binding->struct_size = sizeof(*binding);
        binding->api_version = UCN_PERSIST_API_VERSION;
        binding->domain = entry->domain;
        binding->business_owner_instance = i < 2U ? 21U : 23U;
        binding->domain_generation = 1U;
    }
    f->manifest.struct_size = sizeof(f->manifest);
    f->manifest.api_version = UCN_PERSIST_API_VERSION;
    f->manifest.protocol_manifest_version = UCN_PERSIST_PROTOCOL_MANIFEST_VERSION;
    f->manifest.storage_layout_version = UCN_PERSIST_STORAGE_LAYOUT;
    f->manifest.composition_feature_bits = UCN_COMPILED_FEATURE_MASK;
    f->manifest.entries = f->entries;
    f->manifest.entry_count = 3U;
    f->manifest.profile_id = UCN_PROFILE;
    result = ucn_persistence_manifest_digest(&f->manifest, &f->digest_workspace, digest);
    if (result != UCN_OK) return result;
    memcpy(f->manifest.expected_digest, digest, sizeof(digest));
    fake_persist_provider_init(&f->fake, &f->manifest, 0xFFU);
    fake_persist_provider_make_public(&f->fake, 0xFFU, &f->provider);
    gate_lock = fixture_lock(&f->gate_lock);
    result = ucn_persist_callback_gate_init_in_place(&f->gate_storage, sizeof(f->gate_storage), &gate_lock, &f->gate);
    if (result != UCN_OK) return result;
    f->persistence_config.struct_size = sizeof(f->persistence_config);
    f->persistence_config.api_version = UCN_PERSIST_API_VERSION;
    f->persistence_config.runtime_instance = 10U;
    f->persistence_config.owner_instance = 22U;
    f->persistence_config.required_domain_mask = 7U;
    f->persistence_config.manifest = &f->manifest;
    f->persistence_config.domain_bindings = f->bindings;
    f->persistence_config.domain_binding_count = 3U;
    f->persistence_config.provider = &f->provider;
    f->persistence_config.state_lock = fixture_lock(&f->owner_lock);
    f->persistence_config.shared_callback_gate = f->gate;
    f->persistence_config.digest_workspace = &f->digest_workspace;
    result = ucn_persistence_init_in_place(&f->storage, sizeof(f->storage), &f->persistence_config, &f->persistence);
    if (result != UCN_OK) return result;
    result = ucn_i_persistence_start_recovery(f->persistence);
    if (result != UCN_OK) return result;
    for (i = 0U; i < 128U; ++i) {
        ucn_persistence_step_result_t step;
        result = ucn_i_persistence_step(f->persistence, i + 1U, 8U, &step);
        if (result != UCN_OK) return result;
        if (step.owner_ready != 0U && step.made_progress == 0U) return UCN_OK;
    }
    return UCN_ERR_STATE;
}

ucn_result_t native_join_fixture_provision_authority_binding(native_join_fixture_t *f)
{
    ucn_i_identity_config_t cfg;
    ucn_i_lock_ops_t lock;
    ucn_result_t result;
    uint16_t i;
    if (f == NULL || f->persistence == NULL) return UCN_ERR_ARGUMENT;
    memset(&cfg, 0, sizeof(cfg));
    memset(&lock, 0, sizeof(lock));
    lock.struct_size = sizeof(lock); lock.api_version = UCN_I_LOCK_OPS_VERSION;
    lock.context = &f->identity_lock; lock.enter = fixture_lock_enter; lock.leave = fixture_lock_leave;
    f->identity_rules[0].domain_id = f->entries[0].domain.domain_id;
    f->identity_rules[1].domain_id = f->entries[1].domain.domain_id;
    cfg.struct_size = sizeof(cfg); cfg.api_version = UCN_API_VERSION;
    cfg.runtime_instance = 10U; cfg.realm_id = 42U; cfg.owner_instance = 21U;
    cfg.authority_owner_instance = 19U; cfg.persistence_business_owner_instance = 21U;
    cfg.persistence_owner_instance = 22U; cfg.address_width = 2U;
    cfg.domain_rules = f->identity_rules; cfg.domain_rule_count = 2U; cfg.state_lock = lock;
    result = ucn_i_identity_owner_init(&f->identity, &cfg);
    if (result != UCN_OK) return result;
    /* Separate provisioning issuer; the JOIN Authority is provisioned below. */
    f->authority.lease_sequence = 6U; f->authority.local_deadline_us = 10000000U;
    f->authority.record_generation = 1U; f->authority.foundation_transaction_id = 1U;
    f->authority.witness_generation = 1U; f->authority.runtime_instance = 10U;
    f->authority.realm_id = 42U; f->authority.authority_generation = 3U;
    f->authority.authority_owner_instance = 19U; f->authority.persistence_owner_instance = 22U;
    f->authority.schema_id = UCN_I_IDENTITY_AUTHORITY_SCHEMA_ID; f->authority.schema_version = 1U;
    memset(f->authority.principal, 0x11, sizeof(f->authority.principal));
    memset(f->authority.body_digest, 0x33, sizeof(f->authority.body_digest));
    f->issue.transaction_id = 100U; f->issue.challenge_started_local_us = 100U;
    f->issue.challenge_deadline_us = 9000000U; f->issue.lease_duration_us = 8000000U;
    f->issue.authority_lease_sequence = 6U; f->issue.runtime_instance = 10U;
    f->issue.realm_id = 42U; f->issue.address = 9U; f->issue.binding_generation = 1U;
    f->issue.authority_generation = 3U; f->issue.link_generation = f->device ? 3U : 9U;
    f->issue.admission_generation = 1U; f->issue.admission_owner_instance = 20U;
    f->issue.address_width = 2U; f->issue.mode = UCN_I_IDENTITY_ADDRESS_LEASED;
    memset(f->issue.principal, 0x22, sizeof(f->issue.principal));
    memset(f->issue.authority_principal, 0x11, sizeof(f->issue.authority_principal));
    memset(f->issue.lease_id, 0x55, sizeof(f->issue.lease_id));
    memset(f->issue.transcript_digest, 0x77, sizeof(f->issue.transcript_digest));
    f->identity_base.domain_id = f->entries[0].domain.domain_id;
    f->identity_base.next_transaction_id = 1U; f->identity_base.absolute_deadline_us = 4000000U;
    f->identity_base.domain_generation = 1U;
    f->identity_base.volatile_continuation.runtime_instance = 10U;
    f->identity_base.volatile_continuation.owner_instance = 21U;
    f->identity_base.volatile_continuation.slot = 1U;
    f->identity_base.volatile_continuation.generation = 1U;
    f->identity_base.volatile_continuation.object_kind = UCN_OBJECT_KIND_SEND;
    result = ucn_i_identity_prepare_binding(&f->identity, &f->issue, &f->authority, &f->identity_base, 150U, &f->authority_binding);
    if (result != UCN_OK) return result;
    result = ucn_i_identity_requirement_get(&f->identity, f->authority_binding, &f->requirement);
    if (result != UCN_OK) return result;
    f->request.struct_size = sizeof(f->request); f->request.api_version = UCN_PERSIST_API_VERSION;
    f->request.runtime_instance = f->requirement.runtime_instance;
    f->request.caller_owner_instance = f->requirement.caller_owner_instance;
    f->request.domain_generation = f->requirement.domain_generation;
    f->request.domain = f->entries[0].domain;
    f->request.foundation_transaction_id = f->requirement.foundation_transaction_id;
    f->request.expected_record_generation = f->requirement.expected_record_generation;
    f->request.absolute_deadline_us = f->requirement.absolute_deadline_us;
    f->request.business_transition_digest = f->requirement.transition_fingerprint;
    f->request.canonical_body = f->requirement.canonical_body; f->request.body_bytes = f->requirement.body_bytes;
    f->request.schema_id = f->requirement.schema_id; f->request.schema_version = f->requirement.schema_version;
    f->request.operation_kind = f->requirement.operation_kind;
    memcpy(f->request.expected_body_digest, f->requirement.expected_body_digest, 16U);
    f->request.volatile_continuation = f->requirement.volatile_continuation;
    f->meta.domain = f->request.domain; f->meta.record_generation = 1U;
    f->meta.transaction_id = f->request.foundation_transaction_id; f->meta.body_bytes = f->request.body_bytes;
    f->meta.schema_id = f->request.schema_id; f->meta.schema_version = f->request.schema_version;
    f->meta.operation_kind = f->request.operation_kind;
    result = ucn_i_persist_body_digest(&f->meta, f->request.canonical_body, f->published_digest, &f->codec);
    if (result != UCN_OK) return result;
    result = ucn_i_persistence_submit(f->persistence, &f->request, 160U, &f->persistence_handle);
    if (result != UCN_OK) return result;
    result = ucn_i_identity_bind_persistence(&f->identity, f->authority_binding, f->persistence_handle, f->published_digest);
    if (result != UCN_OK) return result;
    for (i = 0U; i < 128U; ++i) {
        ucn_persistence_step_result_t step;
        result = ucn_i_persistence_step(f->persistence, 170U + i, 8U, &step);
        if (result != UCN_OK) return result;
        if (step.proofs_ready != 0U) break;
    }
    if (i == 128U) return UCN_ERR_STATE;
    result = ucn_i_persistence_proof_get(f->persistence, f->persistence_handle, &f->foundation_proof);
    if (result != UCN_OK) return result;
    f->identity_proof.persistence_handle = f->persistence_handle;
    f->identity_proof.domain_id = f->foundation_proof.domain.domain_id;
    f->identity_proof.record_generation = f->foundation_proof.record_generation;
    f->identity_proof.foundation_transaction_id = f->foundation_proof.foundation_transaction_id;
    f->identity_proof.witness_generation = f->foundation_proof.witness_generation;
    f->identity_proof.transition_fingerprint = f->requirement.transition_fingerprint;
    f->identity_proof.runtime_instance = f->foundation_proof.runtime_instance;
    f->identity_proof.body_bytes = f->foundation_proof.body_bytes;
    f->identity_proof.volatile_continuation = f->requirement.volatile_continuation;
    f->identity_proof.persistence_owner_instance = f->foundation_proof.persistence_owner_instance;
    f->identity_proof.caller_owner_instance = f->foundation_proof.caller_owner_instance;
    f->identity_proof.domain_generation = f->foundation_proof.domain_generation;
    f->identity_proof.schema_id = f->requirement.schema_id;
    f->identity_proof.schema_version = f->requirement.schema_version;
    f->identity_proof.operation_kind = f->foundation_proof.operation_kind;
    memcpy(f->identity_proof.body_digest, f->foundation_proof.body_digest, 16U);
    result = ucn_i_identity_activate_binding(&f->identity, f->authority_binding, &f->identity_proof, &f->authority, 250U, &f->authority_binding_view);
    if (result != UCN_OK) return result;
    /* Explicitly trusted preprovisioned JOIN Authority, not a fresh proof. */
    memset(f->authority.principal, 0x22, sizeof(f->authority.principal));
    return ucn_i_persistence_proof_retire(f->persistence, f->persistence_handle);
}

static ucn_result_t fixture_session_policy(void *context,
    const ucn_i_security_candidate_t *candidate,
    const ucn_i_security_acl_rule_t **rules, uint16_t *count)
{
    native_join_fixture_t *f = context;
    unsigned i;
    if (!f || !candidate || !rules || !count) return UCN_ERR_ARGUMENT;
    memset(f->session_acl_rules, 0, sizeof(f->session_acl_rules));
    for (i = 0U; i < 2U; ++i) {
        memcpy(f->session_acl_rules[i].peer_principal, candidate->peer.principal, 16U);
        memcpy(f->session_acl_rules[i].context_fingerprint, candidate->origin_fingerprint, 16U);
        f->session_acl_rules[i].peer_binding_generation = candidate->peer.binding_generation;
        f->session_acl_rules[i].service_id = 1U;
        f->session_acl_rules[i].direction = i == 0U ? UCN_I_SECURITY_ACCESS_INBOUND : UCN_I_SECURITY_ACCESS_OUTBOUND;
    }
    *rules = f->session_acl_rules; *count = 2U; return UCN_OK;
}

ucn_result_t native_join_fixture_init_with_mtu(native_join_fixture_t *f, bool device, uint16_t mtu)
{
    ucn_ports_t ports;
    ucn_link_port_t link;
    ucn_i_lock_ops_t gate_lock;
    ucn_i_admission_config_t admission;
    ucn_i_security_config_t security;
    ucn_result_t r;
    if (mtu < 127U || mtu > UCN_ADAPTER_FRAME_BYTES) return UCN_ERR_CONFIG;
    r = native_join_fixture_foundation_init(f); if (r != UCN_OK) return r;
    f->device = device ? 1U : 0U;
    r = native_join_fixture_provision_authority_binding(f); if (r != UCN_OK) return r;
    memset(&ports, 0, sizeof(ports)); memset(&link, 0, sizeof(link));
    ports.struct_size = sizeof(ports); ports.api_version = UCN_API_VERSION;
    ports.links = &link; ports.link_count = 1U;
    ports.state_lock = fixture_lock(&f->adapter_lock); ports.driver_callback_gate = fixture_lock(&f->driver_lock);
    link.struct_size = sizeof(link); link.api_version = UCN_API_VERSION;
    link.link_instance = f->device ? 3U : 9U; link.frame_mtu = mtu; link.context = f;
    link.tx.struct_size = sizeof(link.tx); link.tx.api_version = UCN_API_VERSION;
    link.tx.submit = fixture_submit; link.tx.cancel = fixture_cancel;
    r = ucn_i_adapter_init(&f->adapter, 10U, 24U, &ports); if (r != UCN_OK) return r;
    r = ucn_i_adapter_set_rx_enabled(&f->adapter, true); if (r != UCN_OK) return r;
    gate_lock = fixture_internal_lock(&f->callback_lock);
    r = ucn_i_callback_gate_init(&f->callback_gate, 25U, &gate_lock); if (r != UCN_OK) return r;
    memset(&admission, 0, sizeof(admission));
    admission.struct_size = sizeof(admission); admission.api_version = UCN_API_VERSION;
    admission.runtime_instance = 10U; admission.realm_id = 42U; admission.owner_instance = 20U;
    admission.identity_owner_instance = 21U; admission.persistence_owner_instance = 22U;
    admission.max_pending_per_link = 1U; admission.token_burst = 4U; admission.tokens_per_second = 2U;
    admission.pending_timeout_us = 3000000U;
    admission.provider.struct_size = sizeof(admission.provider); admission.provider.api_version = 1U;
    admission.provider.context = f; admission.provider.issue_cookie = fixture_cookie_issue;
    admission.provider.verify_cookie = fixture_cookie_verify; admission.provider.authorize_event = native_join_fixture_authorize_event;
    admission.state_lock = fixture_internal_lock(&f->admission_lock); admission.provider_gate = &f->callback_gate;
    if (!device) { r = ucn_i_admission_owner_init(&f->admission, &admission); if (r != UCN_OK) return r; }
    memset(&security, 0, sizeof(security));
    f->security_rule.domain_id = f->entries[2].domain.domain_id;
    memset(f->security_rule.peer_principal, device ? 0x22 : 0x44, 16U);
    memcpy(f->security_acl.peer_principal, f->security_rule.peer_principal, 16U);
    memset(f->security_acl.context_fingerprint, 0x55, 16U);
    f->security_acl.peer_binding_generation = 1U; f->security_acl.service_id = 1U;
    f->security_acl.direction = UCN_I_SECURITY_ACCESS_INBOUND;
    security.struct_size = sizeof(security); security.api_version = UCN_API_VERSION;
    security.runtime_instance = 10U; security.realm_id = 42U; security.owner_instance = 23U;
    security.persistence_business_owner_instance = 23U; security.address_width = 2U;
    security.domain_rules = &f->security_rule; security.domain_rule_count = 1U;
    security.acl_rules = &f->security_acl; security.acl_rule_count = 1U;
    security.replay_reservation_lifetime_us = 100000U;
    security.provider.struct_size = sizeof(security.provider); security.provider.api_version = 1U;
    security.provider.context = f; security.provider.verify_session = native_join_fixture_verify_session;
    security.provider.protect_origin = fixture_protect_origin; security.provider.open_origin = fixture_open_origin;
    security.provider.protect_hop = fixture_protect_hop; security.provider.verify_hop = fixture_verify_hop;
    security.state_lock = fixture_internal_lock(&f->security_lock); security.provider_gate = &f->callback_gate;
    r = ucn_i_security_owner_init(&f->security, &security); if (r != UCN_OK) return r;
    f->config.adapter = &f->adapter; f->config.admission = device ? NULL : &f->admission;
    f->config.identity = &f->identity; f->config.security = &f->security; f->config.persistence = f->persistence;
    f->config.callback_gate = &f->callback_gate; f->config.authentication = admission.provider;
    f->config.product_context = f; f->config.current_authority = fixture_authority;
    f->config.sign_event = native_join_fixture_sign_event; f->config.authority_binding = f->authority_binding;
    f->config.configure_session_policy = fixture_session_policy;
    f->config.binding_domain_id = f->entries[1].domain.domain_id; f->config.session_domain_id = f->entries[2].domain.domain_id;
    f->config.transaction = 0x1234U; f->config.device_nonce = 0x5678U; f->config.freshness_nonce = 0x9abcU;
    f->config.timeout_us = 3000000U; f->config.session_lifetime_us = 2000000U;
    f->config.retry_interval_us = 100000U; f->config.cookie_bucket_us = 1000000U;
    f->config.realm_id = 42U; f->config.policy_generation = 1U; f->config.physical_peer = 77U;
    f->config.device_address = 10U; f->config.device_binding_generation = 1U;
    f->config.link_generation = f->device ? 3U : 9U; f->config.link_index = 0U; f->config.owner_instance = 26U;
    f->config.max_attempts = 1000U; f->config.address_width = 2U; f->config.device = f->device;
    memset(f->config.device_principal, 0x44, 16U); memset(f->config.authority_principal, 0x22, 16U);
    memset(f->config.identity_digest, 0x33, 16U);
    return UCN_OK;
}

ucn_result_t native_join_fixture_init(native_join_fixture_t *f, bool device)
{ return native_join_fixture_init_with_mtu(f, device, UCN_ADAPTER_FRAME_BYTES); }

/* Host test keys are deliberately fixed and role separated. They are not
 * suitable for products. HMAC input is opcode || exact canonical Transcript. */
static ucn_result_t fixture_event_mac(native_join_fixture_t *f, uint8_t event,
    const ucn_i_admission_transcript_t *t, uint8_t output[32])
{
    size_t n, i;
    uint8_t key = (event == 2U || event == 4U) ? 0x64U : 0xa7U;
    ucn_result_t r;
    memset(&f->encoding_evidence, 0, sizeof(f->encoding_evidence));
    f->encoding_evidence.length = 1U;
    f->encoding_evidence.bytes[0] = 1U;
    r = ucn_i_admission_logical_encode((uint16_t)(event + 3U), (uint8_t)(event + 1U),
        t, &f->encoding_evidence, f->logical, sizeof(f->logical), &n);
    if (r != UCN_OK || n != UCN_I_ADMISSION_TRANSCRIPT_BYTES + 2U) return UCN_ERR_ARGUMENT;
    memset(f->hash_input, 0x36, 64U);
    for (i = 0U; i < 32U; ++i) f->hash_input[i] ^= key;
    f->hash_input[64] = (uint8_t)(event + 3U);
    memcpy(&f->hash_input[65], f->logical, UCN_I_ADMISSION_TRANSCRIPT_BYTES);
    r = ucn_i_sha256_256(f->hash_input, sizeof(f->hash_input), f->hash_inner, &f->hash_workspace);
    if (r != UCN_OK) return r;
    memset(f->hash_input, 0x5c, 64U);
    for (i = 0U; i < 32U; ++i) f->hash_input[i] ^= key;
    memcpy(&f->hash_input[64], f->hash_inner, 32U);
    return ucn_i_sha256_256(f->hash_input, 96U, output, &f->hash_workspace);
}

ucn_result_t native_join_fixture_sign_event(void *context, ucn_i_admission_event_t event,
    const ucn_i_admission_transcript_t *previous, ucn_i_admission_transcript_t *next,
    ucn_i_admission_evidence_t *evidence)
{
    native_join_fixture_t *f = context;
    ucn_result_t r;
    if (f == NULL || previous == NULL || next == NULL || evidence == NULL || event < 1U || event > 5U)
        return UCN_ERR_ARGUMENT;
    *next = *previous;
    if (event == 1U) {
        memset(next->authority_principal, 0x22, 16U);
        next->authority_generation = 3U; next->authority_nonce = UINT64_C(0x3132333435363738);
        next->realm_id = 42U; next->authority_address = 9U; next->authority_binding_generation = 1U;
        next->authority_lease_sequence = 6U; next->authority_lease_duration_us = 8000000U;
        next->freshness_max_remaining_lease_us = 8000000U;
        memset(next->durable_fence_token, 1, 16U); memset(next->allocation_high_water_digest, 2, 16U);
        memset(next->quorum_config_digest, 3, 32U); memset(next->signer_set_digest, 4, 32U);
        memset(next->threshold_proof_digest, 5, 32U); memset(next->freshness_proof_transcript_hash, 6, 32U);
        next->authority_signer_count = 3U; next->authority_quorum_threshold = 2U;
    } else if (event == 2U) {
        memset(next->joining_device_principal, 0x44, 16U);
        next->selected_hop_suite = 16U; next->selected_hop_key_id = 1U;
        next->selected_hop_key_generation = 2U; next->selected_e2e_mode = 1U;
        next->selected_e2e_suite = 1U; next->selected_e2e_key_id = 3U;
        next->selected_e2e_key_generation = 4U; next->selected_session_generation = 1U;
    } else if (event == 3U) {
        next->proposed_address = 10U; next->address_binding_generation = 1U;
        memset(next->binding_lease_id, 0x55, 16U);
        next->binding_lease_duration_us = 2000000U; next->binding_mode = 1U;
    }
    memset(evidence, 0, sizeof(*evidence)); evidence->length = 32U;
    r = fixture_event_mac(f, event, next, evidence->bytes);
    if (r == UCN_OK && f->sign_corrupt_event == event) evidence->bytes[0] ^= 1U;
    if (r == UCN_OK && event == 5U) {
        f->final_transcript = *next; f->final_evidence = *evidence; f->final_valid = 1U;
    }
    return r;
}

ucn_result_t native_join_fixture_authorize_event(void *context, ucn_i_admission_event_t event,
    const ucn_i_admission_key_t *key, const ucn_i_admission_transcript_t *transcript,
    uint64_t now_us, const ucn_i_admission_evidence_t *evidence)
{
    native_join_fixture_t *f = context;
    uint8_t mac[32], diff = 0U;
    size_t i;
    ucn_result_t r;
    if (f == NULL || key == NULL || transcript == NULL || evidence == NULL ||
        event < 1U || event > 5U || now_us == 0U || evidence->length != 32U ||
        key->transaction_id != transcript->transaction_id ||
        memcmp(key->identity_digest, transcript->joining_device_identity_digest, 16U) != 0)
        return UCN_ERR_SECURITY;
    r = fixture_event_mac(f, event, transcript, mac);
    if (r != UCN_OK) return r;
    for (i = 0U; i < 32U; ++i) diff |= mac[i] ^ evidence->bytes[i];
    if (diff != 0U) return UCN_ERR_SECURITY;
    if (event == 5U) { f->final_transcript = *transcript; f->final_evidence = *evidence; f->final_valid = 1U; }
    return UCN_OK;
}

ucn_result_t native_join_fixture_verify_session(void *context,
    const ucn_i_security_candidate_t *c, const uint8_t *proof, size_t proof_bytes)
{
    native_join_fixture_t *f = context;
    const ucn_i_admission_transcript_t *t;
    const ucn_i_security_binding_t *authority, *device;
    uint8_t mac[32], diff = 0U;
    size_t i;
    if (f != NULL && proof_bytes != 32U && f->additional_verify_session != NULL)
        return f->additional_verify_session(f->additional_verify_context, c, proof, proof_bytes);
    if (f == NULL || c == NULL || proof == NULL || proof_bytes != 32U || !f->final_valid)
        return UCN_ERR_SECURITY;
    t = &f->final_transcript;
    if (fixture_event_mac(f, 5U, t, mac) != UCN_OK) return UCN_ERR_SECURITY;
    for (i = 0U; i < 32U; ++i) diff |= (uint8_t)(mac[i] ^ proof[i]) | (uint8_t)(mac[i] ^ f->final_evidence.bytes[i]);
    if (diff != 0U || f->final_evidence.length != 32U ||
        ucn_i_admission_session_seed(t, 1U, 1U, 2U, &f->seed, &f->seed_workspace) != UCN_OK)
        return UCN_ERR_SECURITY;
    authority = f->device ? &c->peer : &c->local;
    device = f->device ? &c->local : &c->peer;
    if (ucn_i_identity_binding_get(&f->identity, f->join.device_binding, f->join.now_us,
            &f->checked_device_binding) != UCN_OK ||
        f->checked_device_binding.address != device->address ||
        f->checked_device_binding.binding_generation != device->binding_generation ||
        memcmp(f->checked_device_binding.principal, device->principal, 16U) != 0)
        return UCN_ERR_SECURITY;
    if (memcmp(c->transcript_digest, f->seed.transcript_digest, 32U) != 0 ||
        memcmp(c->origin_fingerprint, f->seed.transcript_digest, 16U) != 0 ||
        memcmp(c->hop_fingerprint, f->seed.transcript_digest + 16U, 16U) != 0 ||
        memcmp(authority->principal, t->authority_principal, 16U) != 0 ||
        authority->address != t->authority_address || authority->binding_generation != t->authority_binding_generation ||
        memcmp(device->principal, t->joining_device_principal, 16U) != 0 ||
        device->address != t->proposed_address || device->binding_generation != t->address_binding_generation ||
        c->session_generation != t->selected_session_generation || c->link_generation != f->config.link_generation ||
        c->policy_generation != 1U || c->address_width != 2U || c->origin_level != t->selected_e2e_mode ||
        c->hop_profile != UCN_I_HOP_PROFILE_H1 || c->expires_at_us == 0U ||
        c->expires_at_us > f->authority.local_deadline_us ||
        c->expires_at_us > f->authority_binding_view.local_deadline_us ||
        c->expires_at_us > f->checked_device_binding.local_deadline_us ||
        f->join.started_us == 0U || f->config.session_lifetime_us > UINT64_MAX - f->join.started_us ||
        c->expires_at_us > f->join.started_us + f->config.session_lifetime_us ||
        c->expires_at_us > (f->device ? f->join.current.local.local_deadline_us : f->join.current.peer.local_deadline_us) ||
        c->origin_tx.suite_id != t->selected_e2e_suite || c->origin_rx.suite_id != t->selected_e2e_suite ||
        c->origin_tx.key_generation != t->selected_e2e_key_generation || c->origin_rx.key_generation != t->selected_e2e_key_generation ||
        c->hop_tx.suite_id != t->selected_hop_suite || c->hop_rx.suite_id != t->selected_hop_suite ||
        c->hop_tx.key_generation != t->selected_hop_key_generation || c->hop_rx.key_generation != t->selected_hop_key_generation ||
        c->origin_tx.key_id != (f->device ? f->seed.origin_forward_key_id : f->seed.origin_reverse_key_id) ||
        c->origin_rx.key_id != (f->device ? f->seed.origin_reverse_key_id : f->seed.origin_forward_key_id) ||
        c->hop_tx.key_id != (f->device ? f->seed.hop_forward_key_id : f->seed.hop_reverse_key_id) ||
        c->hop_rx.key_id != (f->device ? f->seed.hop_reverse_key_id : f->seed.hop_forward_key_id))
        return UCN_ERR_SECURITY;
    return UCN_OK;
}
