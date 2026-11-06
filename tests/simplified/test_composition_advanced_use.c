#include "internal/ucn_composition.h"
#include "internal/ucn_digest.h"
#include "internal/ucn_runtime.h"

#include <stdio.h>
#include <string.h>

#define TEST_COMPOSITION_STORAGE_BYTES 400000U
#define TEST_FLOW_ACTIVE_KIND UINT8_C(0x45)

typedef union test_composition_storage {
    uint64_t alignment;
    uint8_t bytes[TEST_COMPOSITION_STORAGE_BYTES];
} test_composition_storage_t;

typedef struct test_lock {
    uint8_t held;
} test_lock_t;

typedef struct test_fixture {
    test_composition_storage_t composition_storage;
    ucn_storage_t foundation_storage;
    ucn_static_binding_t foundation_binding;
    ucn_link_port_t foundation_links[2];
    ucn_ports_t foundation_ports;
    ucn_config_t foundation_config;
    test_lock_t foundation_lock;
    test_lock_t foundation_driver_gate;
    test_lock_t composition_lock;
    test_lock_t identity_lock;
    test_lock_t admission_lock;
    test_lock_t admission_gate_lock;
    test_lock_t security_lock;
    test_lock_t security_gate_lock;
    test_lock_t capability_lock;
    test_lock_t route_lock;
    test_lock_t flow_lock;
    test_lock_t transport_lock;
    test_lock_t service_lock;
    ucn_i_callback_gate_t admission_gate;
    ucn_i_callback_gate_t security_gate;
    ucn_i_identity_domain_rule_t identity_rule;
    ucn_i_security_domain_rule_t security_rule;
    ucn_i_security_acl_rule_t security_acl[4];
    uint8_t admission_provider_context;
    ucn_driver_submit_result_t driver_submit_result;
    uint32_t driver_submit_calls;
    ucn_link_handle_t driver_last_link;
    ucn_driver_token_t driver_last_token;
    size_t driver_last_bytes;
    uint8_t driver_last_frame[UCN_ADAPTER_FRAME_BYTES];
    uint32_t security_protect_calls;
    ucn_i_composition_runtime_t *runtime;
    ucn_i_identity_owner_t *identity_owner;
    ucn_i_admission_owner_t *admission_owner;
    ucn_i_security_owner_t *security_owner;
    ucn_i_capability_owner_t *capability_owner;
    ucn_i_route_owner_t *route_owner;
    ucn_i_flow_owner_t *flow_owner;
    ucn_i_transport_owner_t *transport_owner;
    ucn_i_service_owner_t *service_owner;
    ucn_i_composition_advanced_use_request_t request;
} test_fixture_t;

static test_fixture_t fixture;
static ucn_i_composition_control_workspace_t control_workspace;
static ucn_i_composition_ingress_workspace_t ingress_workspace;
static ucn_i_composition_control_tx_workspace_t source_tx_workspace;
static ucn_i_sha256_workspace_t source_tx_finish_workspace;
static int failures;

#define CHECK(condition_)                                                   \
    do {                                                                    \
        if (!(condition_)) {                                                \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,       \
                    __LINE__, #condition_);                                 \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

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

    if (lock != NULL) {
        lock->held = 0U;
    }
}

static ucn_i_lock_ops_t internal_lock(test_lock_t *lock)
{
    ucn_i_lock_ops_t value;

    memset(&value, 0, sizeof(value));
    value.struct_size = (uint16_t)sizeof(value);
    value.api_version = UCN_I_LOCK_OPS_VERSION;
    value.context = lock;
    value.enter = lock_enter;
    value.leave = lock_leave;
    return value;
}

static ucn_lock_ops_t public_lock(test_lock_t *lock)
{
    ucn_lock_ops_t value;

    memset(&value, 0, sizeof(value));
    value.struct_size = (uint16_t)sizeof(value);
    value.api_version = UCN_API_VERSION;
    value.context = lock;
    value.enter = lock_enter;
    value.leave = lock_leave;
    return value;
}

static ucn_driver_submit_result_t driver_submit(
    void *context,
    ucn_link_handle_t link,
    const uint8_t *bytes,
    size_t length,
    ucn_driver_token_t token)
{
    test_fixture_t *value = context;

    if (value == NULL) {
        return UCN_DRIVER_SUBMIT_UNKNOWN;
    }
    if (bytes == NULL || length == 0U ||
        length > sizeof(value->driver_last_frame)) {
        return UCN_DRIVER_SUBMIT_UNKNOWN;
    }
    ++value->driver_submit_calls;
    value->driver_last_link = link;
    value->driver_last_token = token;
    value->driver_last_bytes = length;
    memcpy(value->driver_last_frame, bytes, length);
    return value->driver_submit_result;
}

static ucn_result_t driver_cancel(void *context, ucn_driver_token_t token)
{
    (void)context;
    (void)token;
    return UCN_OK;
}

static ucn_result_t admission_issue_cookie(
    void *context,
    const ucn_i_admission_hello_t *hello,
    ucn_i_admission_link_t link,
    uint32_t cookie_time_bucket,
    uint8_t output[UCN_I_ADMISSION_COOKIE_BYTES],
    uint8_t *output_bytes)
{
    (void)context;
    (void)hello;
    (void)link;
    (void)cookie_time_bucket;
    (void)output;
    (void)output_bytes;
    return UCN_ERR_STATE;
}

static ucn_result_t admission_verify_cookie(
    void *context,
    const ucn_i_admission_hello_cookie_t *hello_cookie, uint64_t now_us)
{
    (void)now_us;
    (void)context;
    (void)hello_cookie;
    return UCN_ERR_STATE;
}

static ucn_result_t admission_authorize_event(
    void *context,
    ucn_i_admission_event_t event,
    const ucn_i_admission_key_t *key,
    const ucn_i_admission_transcript_t *transcript,
    uint64_t now_us,
    const ucn_i_admission_evidence_t *evidence)
{
    (void)context;
    (void)event;
    (void)key;
    (void)transcript;
    (void)now_us;
    (void)evidence;
    return UCN_ERR_STATE;
}

static ucn_result_t security_verify_session(
    void *context,
    const ucn_i_security_candidate_t *candidate,
    const uint8_t *proof,
    size_t proof_bytes)
{
    (void)context;
    (void)candidate;
    (void)proof;
    (void)proof_bytes;
    return UCN_ERR_STATE;
}

static ucn_result_t security_protect_origin(
    void *context,
    const ucn_i_security_origin_protect_request_t *request)
{
    uint32_t value;
    size_t index;

    test_fixture_t *value_fixture = context;

    if (value_fixture != NULL) {
        ++value_fixture->security_protect_calls;
    }
    if (request == NULL || request->aad == NULL ||
        request->plaintext == NULL || request->protected_payload == NULL ||
        request->origin_tag == NULL || request->selector.suite_id != 1U ||
        request->nonce_input != NULL || request->nonce_input_bytes != 0U) {
        return UCN_ERR_ARGUMENT;
    }
    value = UINT32_C(2166136261) ^ request->selector.key_generation ^
            request->selector.key_id ^ request->selector.suite_id;
    for (index = 0U; index < request->aad_bytes; ++index) {
        value = (value ^ request->aad[index]) * UINT32_C(16777619);
    }
    for (index = 0U; index < request->payload_bytes; ++index) {
        value = (value ^ request->plaintext[index]) * UINT32_C(16777619);
    }
    if (request->payload_bytes != 0U) {
        memcpy(request->protected_payload, request->plaintext,
               request->payload_bytes);
    }
    for (index = 0U; index < UCN_I_SECURITY_ORIGIN_TAG_BYTES; ++index) {
        value = value * UINT32_C(1664525) + UINT32_C(1013904223);
        request->origin_tag[index] = (uint8_t)(value >> 24U);
    }
    return UCN_OK;
}

static ucn_result_t security_open_origin(
    void *context,
    const ucn_i_security_origin_open_request_t *request)
{
    uint32_t value;
    uint8_t expected[UCN_I_SECURITY_ORIGIN_TAG_BYTES];
    size_t index;

    (void)context;
    if (request == NULL || request->aad == NULL ||
        request->protected_payload == NULL || request->origin_tag == NULL ||
        request->plaintext == NULL || request->selector.suite_id != 1U ||
        request->nonce_input != NULL || request->nonce_input_bytes != 0U) {
        return UCN_ERR_ARGUMENT;
    }
    value = UINT32_C(2166136261) ^ request->selector.key_generation ^
            request->selector.key_id ^ request->selector.suite_id;
    for (index = 0U; index < request->aad_bytes; ++index) {
        value = (value ^ request->aad[index]) * UINT32_C(16777619);
    }
    for (index = 0U; index < request->payload_bytes; ++index) {
        value = (value ^ request->protected_payload[index]) *
                UINT32_C(16777619);
    }
    for (index = 0U; index < sizeof(expected); ++index) {
        value = value * UINT32_C(1664525) + UINT32_C(1013904223);
        expected[index] = (uint8_t)(value >> 24U);
    }
    if (memcmp(expected, request->origin_tag, sizeof(expected)) != 0) {
        return UCN_ERR_SECURITY;
    }
    memcpy(request->plaintext, request->protected_payload,
           request->payload_bytes);
    return UCN_OK;
}

static void security_test_tag(
    const ucn_i_security_key_selector_t *selector,
    const uint8_t *aad, size_t aad_bytes,
    const uint8_t *payload, size_t payload_bytes,
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

static ucn_result_t security_protect_hop(
    void *context,
    const ucn_i_security_hop_request_t *request)
{
    uint8_t tag[UCN_I_SECURITY_ORIGIN_TAG_BYTES];

    (void)context;
    if (request == NULL || request->aad == NULL ||
        request->hop_tag == NULL || request->selector.suite_id != 16U) {
        return UCN_ERR_ARGUMENT;
    }
    security_test_tag(&request->selector, request->aad,
                      request->aad_bytes, NULL, 0U, tag);
    memcpy(request->hop_tag, tag, UCN_I_SECURITY_HOP_TAG_BYTES);
    return UCN_OK;
}

static ucn_result_t security_verify_hop(
    void *context,
    const ucn_i_security_hop_verify_request_t *request)
{
    uint8_t tag[UCN_I_SECURITY_ORIGIN_TAG_BYTES];

    (void)context;
    if (request == NULL || request->aad == NULL ||
        request->hop_tag == NULL || request->selector.suite_id != 16U) {
        return UCN_ERR_ARGUMENT;
    }
    security_test_tag(&request->selector, request->aad,
                      request->aad_bytes, NULL, 0U, tag);
    return memcmp(request->hop_tag, tag,
                  UCN_I_SECURITY_HOP_TAG_BYTES) == 0 ?
               UCN_OK : UCN_ERR_SECURITY;
}

static ucn_i_composition_owner_record_t *record_for(
    ucn_i_composition_runtime_t *runtime,
    uint8_t module_id)
{
    uint8_t index;

    for (index = 0U; index < runtime->registry_count; ++index) {
        if (runtime->registry[index].ref.module_id == module_id) {
            return &runtime->registry[index];
        }
    }
    return NULL;
}

static void *owner_for(ucn_i_composition_runtime_t *runtime,
                       uint8_t module_id)
{
    ucn_i_composition_owner_record_t *record = record_for(runtime, module_id);

    if (record == NULL || record->storage_offset == 0U) {
        return NULL;
    }
    return (uint8_t *)runtime + record->storage_offset;
}

static ucn_i_route_binding_t route_binding(uint32_t address,
                                           uint32_t generation,
                                           uint8_t principal_byte)
{
    ucn_i_route_binding_t value;

    memset(&value, 0, sizeof(value));
    value.address = address;
    value.generation = generation;
    memset(value.principal, principal_byte, sizeof(value.principal));
    return value;
}

static ucn_i_security_binding_t security_binding(
    ucn_i_route_binding_t route)
{
    ucn_i_security_binding_t value;

    memset(&value, 0, sizeof(value));
    value.address = route.address;
    value.binding_generation = route.generation;
    memcpy(value.principal, route.principal, sizeof(value.principal));
    return value;
}

static ucn_i_security_handle_t security_active_slot_install(
    test_fixture_t *value,
    uint16_t slot_index,
    const ucn_i_security_candidate_t *candidate)
{
    ucn_i_security_handle_t handle;
    ucn_i_security_slot_t *slot =
        &value->security_owner->sessions[slot_index];

    memset(slot, 0, sizeof(*slot));
    slot->candidate = *candidate;
    slot->durability.absolute_deadline_us = candidate->expires_at_us;
    slot->slot_generation = (uint32_t)(slot_index + 1U);
    slot->occupied = 1U;
    slot->phase = UCN_I_SECURITY_SESSION_ACTIVE;
    memset(&handle, 0, sizeof(handle));
    handle.runtime_instance = value->security_owner->runtime_instance;
    handle.owner_instance = value->security_owner->owner_instance;
    handle.slot = slot_index;
    handle.slot_generation = slot->slot_generation;
    handle.session_generation = candidate->session_generation;
    return handle;
}

static ucn_i_security_current_facts_t security_candidate_facts(
    const ucn_i_security_candidate_t *candidate,
    uint64_t now_us)
{
    ucn_i_security_current_facts_t facts;

    memset(&facts, 0, sizeof(facts));
    facts.local = candidate->local;
    facts.peer = candidate->peer;
    facts.now_us = now_us;
    facts.link_generation = candidate->link_generation;
    facts.policy_generation = candidate->policy_generation;
    return facts;
}

static ucn_i_security_candidate_t security_path_candidate(
    ucn_i_route_binding_t local,
    ucn_i_route_binding_t peer,
    uint32_t session_generation,
    uint32_t link_generation,
    uint8_t hop_profile,
    uint16_t hop_tx_key,
    uint16_t hop_rx_key,
    uint8_t origin_fingerprint,
    uint8_t hop_fingerprint)
{
    ucn_i_security_candidate_t candidate;

    memset(&candidate, 0, sizeof(candidate));
    candidate.local = security_binding(local);
    candidate.peer = security_binding(peer);
    candidate.expires_at_us = 9000U;
    candidate.link_generation = link_generation;
    candidate.session_generation = session_generation;
    candidate.policy_generation = 60U;
    candidate.origin_tx.suite_id = 1U;
    candidate.origin_tx.key_id = 21U;
    candidate.origin_tx.key_generation = 61U;
    candidate.origin_rx = candidate.origin_tx;
    candidate.hop_tx.suite_id = 16U;
    candidate.hop_tx.key_id = hop_tx_key;
    candidate.hop_tx.key_generation = 51U;
    candidate.hop_rx.suite_id = 16U;
    candidate.hop_rx.key_id = hop_rx_key;
    candidate.hop_rx.key_generation = 51U;
    memset(candidate.origin_fingerprint, origin_fingerprint,
           sizeof(candidate.origin_fingerprint));
    memset(candidate.hop_fingerprint, hop_fingerprint,
           sizeof(candidate.hop_fingerprint));
    memset(candidate.transcript_digest, 0xC1,
           sizeof(candidate.transcript_digest));
    candidate.origin_level = UCN_I_SECURITY_AUTHENTICATED;
    candidate.hop_profile = hop_profile;
    candidate.address_width = 1U;
    return candidate;
}

static void foundation_init(test_fixture_t *value,
                            uint32_t runtime_instance,
                            uint16_t owner_instance)
{
    ucn_node_t *node = NULL;

    memset(&value->foundation_storage, 0, sizeof(value->foundation_storage));
    memset(&value->foundation_binding, 0,
           sizeof(value->foundation_binding));
    value->foundation_binding.struct_size =
        (uint16_t)sizeof(value->foundation_binding);
    value->foundation_binding.api_version = UCN_API_VERSION;
    value->foundation_binding.address = 1U;
    value->foundation_binding.binding_generation = 10U;
    value->foundation_binding.principal_digest =
        UINT64_C(0x1111111111111111);

    memset(value->foundation_links, 0, sizeof(value->foundation_links));
    value->driver_submit_result = UCN_DRIVER_COMPLETE;
    for (uint16_t index = 0U; index < 2U; ++index) {
        value->foundation_links[index].struct_size =
            (uint16_t)sizeof(value->foundation_links[index]);
        value->foundation_links[index].api_version = UCN_API_VERSION;
        value->foundation_links[index].link_instance =
            (uint32_t)(index + 1U);
        value->foundation_links[index].frame_mtu = 256U;
        value->foundation_links[index].context = value;
        value->foundation_links[index].tx.struct_size =
            (uint16_t)sizeof(value->foundation_links[index].tx);
        value->foundation_links[index].tx.api_version = UCN_API_VERSION;
        value->foundation_links[index].tx.submit = driver_submit;
        value->foundation_links[index].tx.cancel = driver_cancel;
    }

    memset(&value->foundation_ports, 0, sizeof(value->foundation_ports));
    value->foundation_ports.struct_size =
        (uint16_t)sizeof(value->foundation_ports);
    value->foundation_ports.api_version = UCN_API_VERSION;
    value->foundation_ports.links = value->foundation_links;
    value->foundation_ports.link_count = 2U;
    value->foundation_ports.state_lock =
        public_lock(&value->foundation_lock);
    value->foundation_ports.driver_callback_gate =
        public_lock(&value->foundation_driver_gate);

    memset(&value->foundation_config, 0,
           sizeof(value->foundation_config));
    value->foundation_config.struct_size =
        (uint16_t)sizeof(value->foundation_config);
    value->foundation_config.api_version = UCN_API_VERSION;
    value->foundation_config.storage_layout = UCN_STORAGE_LAYOUT;
    value->foundation_config.compiled_manifest_hash =
        UCN_COMPILED_MANIFEST_HASH;
    value->foundation_config.runtime_instance = runtime_instance;
    value->foundation_config.realm_id = 7U;
    value->foundation_config.local_address = 1U;
    value->foundation_config.local_binding_generation = 10U;
    value->foundation_config.local_principal_digest =
        value->foundation_binding.principal_digest;
    value->foundation_config.bindings = &value->foundation_binding;
    value->foundation_config.binding_count = 1U;
    value->foundation_config.address_width = 1U;
    value->foundation_config.trusted_o0_network = 1U;

    CHECK(ucn_i_node_init_with_owner_instance(
              &value->foundation_storage, sizeof(value->foundation_storage),
              &value->foundation_config, &value->foundation_ports,
              owner_instance, &node) == UCN_OK);
    CHECK(node != NULL);
    CHECK(ucn_start(node, 1U) == UCN_OK);
    value->runtime->foundation_node = node;
    value->runtime->foundation_storage_bytes =
        (uint32_t)sizeof(value->foundation_storage);
}

static void owners_init(test_fixture_t *value)
{
    const uint32_t runtime_instance = value->runtime->runtime_instance;
    ucn_i_composition_owner_record_t *identity_record = record_for(
        value->runtime, UCN_I_COMPOSITION_MODULE_IDENTITY);
    ucn_i_composition_owner_record_t *admission_record = record_for(
        value->runtime, UCN_I_COMPOSITION_MODULE_ADMISSION);
    ucn_i_composition_owner_record_t *security_record = record_for(
        value->runtime, UCN_I_COMPOSITION_MODULE_SECURITY);
    ucn_i_composition_owner_record_t *capability_record = record_for(
        value->runtime, UCN_I_COMPOSITION_MODULE_CAPABILITY);
    ucn_i_composition_owner_record_t *route_record = record_for(
        value->runtime, UCN_I_COMPOSITION_MODULE_ROUTE);
    ucn_i_composition_owner_record_t *flow_record = record_for(
        value->runtime, UCN_I_COMPOSITION_MODULE_FLOW);
    ucn_i_composition_owner_record_t *transport_record = record_for(
        value->runtime, UCN_I_COMPOSITION_MODULE_TRANSPORT);
    ucn_i_composition_owner_record_t *service_record = record_for(
        value->runtime, UCN_I_COMPOSITION_MODULE_SERVICE);
    ucn_i_composition_owner_record_t *persistence_record = record_for(
        value->runtime, UCN_I_COMPOSITION_MODULE_PERSISTENCE);
    ucn_i_identity_config_t identity_config;
    ucn_i_admission_config_t admission_config;
    ucn_i_security_config_t security_config;
    ucn_i_route_config_t route_config;
    ucn_i_flow_config_t flow_config;
    ucn_i_transport_config_t transport_config;
    ucn_i_service_config_t service_config;
    ucn_i_lock_ops_t lock;
    ucn_i_route_binding_t local = route_binding(1U, 10U, 0x11U);

    CHECK(identity_record != NULL);
    CHECK(admission_record != NULL);
    CHECK(security_record != NULL);
    CHECK(capability_record != NULL);
    CHECK(route_record != NULL);
    CHECK(flow_record != NULL);
    CHECK(transport_record != NULL);
    CHECK(service_record != NULL);
    CHECK(persistence_record != NULL);

    value->identity_owner = owner_for(
        value->runtime, UCN_I_COMPOSITION_MODULE_IDENTITY);
    value->admission_owner = owner_for(
        value->runtime, UCN_I_COMPOSITION_MODULE_ADMISSION);
    value->security_owner = owner_for(
        value->runtime, UCN_I_COMPOSITION_MODULE_SECURITY);
    value->capability_owner = owner_for(
        value->runtime, UCN_I_COMPOSITION_MODULE_CAPABILITY);
    value->route_owner = owner_for(
        value->runtime, UCN_I_COMPOSITION_MODULE_ROUTE);
    value->flow_owner = owner_for(
        value->runtime, UCN_I_COMPOSITION_MODULE_FLOW);
    value->transport_owner = owner_for(
        value->runtime, UCN_I_COMPOSITION_MODULE_TRANSPORT);
    value->service_owner = owner_for(
        value->runtime, UCN_I_COMPOSITION_MODULE_SERVICE);

    memset(&identity_config, 0, sizeof(identity_config));
    value->identity_rule.domain_id = 1U;
    identity_config.struct_size = (uint16_t)sizeof(identity_config);
    identity_config.api_version = UCN_API_VERSION;
    identity_config.runtime_instance = runtime_instance;
    identity_config.realm_id = 7U;
    identity_config.owner_instance = identity_record->ref.owner_instance;
    identity_config.authority_owner_instance = 300U;
    identity_config.persistence_business_owner_instance =
        identity_record->ref.owner_instance;
    identity_config.persistence_owner_instance =
        persistence_record->ref.owner_instance;
    identity_config.address_width = 1U;
    identity_config.domain_rules = &value->identity_rule;
    identity_config.domain_rule_count = 1U;
    identity_config.state_lock = internal_lock(&value->identity_lock);
    CHECK(ucn_i_identity_owner_init(value->identity_owner,
                                    &identity_config) == UCN_OK);

    lock = internal_lock(&value->admission_gate_lock);
    CHECK(ucn_i_callback_gate_init(&value->admission_gate,
                                   admission_record->ref.owner_instance,
                                   &lock) == UCN_OK);
    memset(&admission_config, 0, sizeof(admission_config));
    admission_config.struct_size = (uint16_t)sizeof(admission_config);
    admission_config.api_version = UCN_API_VERSION;
    admission_config.runtime_instance = runtime_instance;
    admission_config.realm_id = 7U;
    admission_config.owner_instance = admission_record->ref.owner_instance;
    admission_config.identity_owner_instance =
        identity_record->ref.owner_instance;
    admission_config.persistence_owner_instance =
        persistence_record->ref.owner_instance;
    admission_config.max_pending_per_link = 1U;
    admission_config.token_burst = 1U;
    admission_config.tokens_per_second = 1U;
    admission_config.pending_timeout_us = 1000U;
    admission_config.provider.struct_size =
        (uint16_t)sizeof(admission_config.provider);
    admission_config.provider.api_version = 1U;
    admission_config.provider.context = &value->admission_provider_context;
    admission_config.provider.issue_cookie = admission_issue_cookie;
    admission_config.provider.verify_cookie = admission_verify_cookie;
    admission_config.provider.authorize_event = admission_authorize_event;
    admission_config.state_lock = internal_lock(&value->admission_lock);
    admission_config.provider_gate = &value->admission_gate;
    CHECK(ucn_i_admission_owner_init(value->admission_owner,
                                     &admission_config) == UCN_OK);

    lock = internal_lock(&value->security_gate_lock);
    CHECK(ucn_i_callback_gate_init(&value->security_gate,
                                   security_record->ref.owner_instance,
                                   &lock) == UCN_OK);
    memset(&security_config, 0, sizeof(security_config));
    memset(&value->security_rule, 0, sizeof(value->security_rule));
    memset(value->security_acl, 0, sizeof(value->security_acl));
    value->security_rule.domain_id = 2U;
    memset(value->security_rule.peer_principal, 0x22,
           sizeof(value->security_rule.peer_principal));
    memset(value->security_acl[0].peer_principal, 0x22,
           sizeof(value->security_acl[0].peer_principal));
    memset(value->security_acl[0].context_fingerprint, 0x55,
           sizeof(value->security_acl[0].context_fingerprint));
    value->security_acl[0].peer_binding_generation = 20U;
    value->security_acl[0].service_id = 11U;
    value->security_acl[0].protocol_opcode = UCN_I_OPCODE_OPERATION_QUERY;
    value->security_acl[0].direction = UCN_I_SECURITY_ACCESS_OUTBOUND;
    value->security_acl[1] = value->security_acl[0];
    value->security_acl[1].service_id = 0U;
    value->security_acl[1].protocol_opcode = UCN_I_OPCODE_DIAGNOSTIC_PING;
    value->security_acl[1].direction = UCN_I_SECURITY_ACCESS_INBOUND;
    value->security_acl[2] = value->security_acl[0];
    memset(value->security_acl[2].peer_principal, 0x33,
           sizeof(value->security_acl[2].peer_principal));
    memset(value->security_acl[2].context_fingerprint, 0x66,
           sizeof(value->security_acl[2].context_fingerprint));
    value->security_acl[2].peer_binding_generation = 30U;
    value->security_acl[2].service_id = 12U;
    value->security_acl[2].protocol_opcode = 0U;
    value->security_acl[3] = value->security_acl[2];
    memset(value->security_acl[3].peer_principal, 0x44,
           sizeof(value->security_acl[3].peer_principal));
    memset(value->security_acl[3].context_fingerprint, 0x77,
           sizeof(value->security_acl[3].context_fingerprint));
    value->security_acl[3].peer_binding_generation = 40U;
    value->security_acl[3].service_id = 13U;
    security_config.struct_size = (uint16_t)sizeof(security_config);
    security_config.api_version = UCN_API_VERSION;
    security_config.runtime_instance = runtime_instance;
    security_config.realm_id = 7U;
    security_config.owner_instance = security_record->ref.owner_instance;
    security_config.persistence_business_owner_instance =
        security_record->ref.owner_instance;
    security_config.address_width = 1U;
    security_config.domain_rules = &value->security_rule;
    security_config.domain_rule_count = 1U;
    security_config.acl_rules = value->security_acl;
    security_config.acl_rule_count = 4U;
    security_config.replay_reservation_lifetime_us = 1000U;
    security_config.provider.struct_size =
        (uint16_t)sizeof(security_config.provider);
    security_config.provider.api_version = 1U;
    security_config.provider.context = value;
    security_config.provider.verify_session = security_verify_session;
    security_config.provider.protect_origin = security_protect_origin;
    security_config.provider.open_origin = security_open_origin;
    security_config.provider.protect_hop = security_protect_hop;
    security_config.provider.verify_hop = security_verify_hop;
    security_config.state_lock = internal_lock(&value->security_lock);
    security_config.provider_gate = &value->security_gate;
    CHECK(ucn_i_security_owner_init(value->security_owner,
                                    &security_config) == UCN_OK);

    lock = internal_lock(&value->capability_lock);
    CHECK(ucn_i_capability_owner_init(
              value->capability_owner, runtime_instance, 7U,
              capability_record->ref.owner_instance,
              security_record->ref.owner_instance, 5000U, 5000U,
              &lock) == UCN_OK);

    memset(&route_config, 0, sizeof(route_config));
    route_config.local = local;
    route_config.discovery_lifetime_us = 1000U;
    route_config.discovery_retry_us = 100U;
    route_config.reverse_lifetime_us = 1200U;
    route_config.route_lifetime_us = 5000U;
    route_config.first_transaction_id = 1U;
    route_config.runtime_instance = runtime_instance;
    route_config.realm = 7U;
    route_config.local_session_generation = 80U;
    route_config.owner_instance = route_record->ref.owner_instance;
    route_config.address_width = 1U;
    route_config.discovery_max_attempts = 3U;
    route_config.maximum_hops = 8U;
    lock = internal_lock(&value->route_lock);
    CHECK(ucn_i_route_owner_init(value->route_owner, &route_config,
                                 &lock) == UCN_OK);

    memset(&flow_config, 0, sizeof(flow_config));
    flow_config.local = local;
    flow_config.probe_lifetime_us = 1000U;
    flow_config.stage_lifetime_us = 1000U;
    flow_config.commit_lifetime_us = 1000U;
    flow_config.flow_lifetime_us = 5000U;
    flow_config.receipt_lifetime_us = 1000U;
    flow_config.first_transaction_id = 1U;
    flow_config.runtime_instance = runtime_instance;
    flow_config.realm = 7U;
    flow_config.policy_generation = 60U;
    flow_config.first_candidate_id = 1U;
    flow_config.first_route_generation = 1U;
    flow_config.first_flow_generation = 1U;
    flow_config.owner_instance = flow_record->ref.owner_instance;
    flow_config.first_context_id = 1U;
    flow_config.first_label = 1U;
    lock = internal_lock(&value->flow_lock);
    CHECK(ucn_i_flow_owner_init(value->flow_owner, &flow_config,
                                &lock) == UCN_OK);

    memset(&transport_config, 0, sizeof(transport_config));
    transport_config.reliable_lifetime_us = 1000U;
    transport_config.reliable_retry_us = 100U;
    transport_config.receipt_lifetime_us = 1000U;
    transport_config.runtime_instance = runtime_instance;
    transport_config.owner_instance = transport_record->ref.owner_instance;
    transport_config.reliable_max_attempts = 3U;
    transport_config.state_lock = internal_lock(&value->transport_lock);
    CHECK(ucn_i_transport_owner_init(value->transport_owner,
                                     &transport_config) == UCN_OK);

    memset(&service_config, 0, sizeof(service_config));
    service_config.receipt_lifetime_us = 1000U;
    service_config.runtime_instance = runtime_instance;
    service_config.owner_instance = service_record->ref.owner_instance;
    service_config.state_lock = internal_lock(&value->service_lock);
    CHECK(ucn_i_service_owner_init(value->service_owner,
                                   &service_config) == UCN_OK);

    memset((uint8_t *)value->runtime + persistence_record->storage_offset,
           0xA5, persistence_record->storage_bytes);
}

static void active_state_install(test_fixture_t *value)
{
    const uint32_t runtime_instance = value->runtime->runtime_instance;
    const ucn_i_route_binding_t local = route_binding(1U, 10U, 0x11U);
    const ucn_i_route_binding_t peer = route_binding(2U, 20U, 0x22U);
    ucn_i_identity_slot_t *identity = &value->identity_owner->bindings[0];
    ucn_i_admission_slot_t *admission = &value->admission_owner->pending[0];
    ucn_i_security_slot_t *security = &value->security_owner->sessions[0];
    ucn_i_cached_capability_t *capability =
        &value->capability_owner->peers[0].value;
    ucn_i_route_soft_view_t *route = &value->route_owner->routes[0].value;
    ucn_i_flow_active_record_t *flow = &value->flow_owner->flows[0];
    uint8_t index;

    memset(identity, 0, sizeof(*identity));
    identity->issue.binding_generation = 20U;
    identity->active_view.runtime_instance = runtime_instance;
    identity->active_view.realm_id = 7U;
    identity->active_view.address = peer.address;
    identity->active_view.binding_generation = peer.generation;
    identity->active_view.authority_generation = 3U;
    identity->active_view.link_generation = 30U;
    identity->active_view.local_deadline_us = 10000U;
    identity->active_view.record_generation = 1U;
    identity->active_view.foundation_transaction_id = 1U;
    identity->active_view.witness_generation = 1U;
    identity->active_view.slot_generation = 1U;
    identity->active_view.identity_owner_instance =
        value->identity_owner->owner_instance;
    identity->active_view.persistence_owner_instance =
        record_for(value->runtime,
                   UCN_I_COMPOSITION_MODULE_PERSISTENCE)->ref.owner_instance;
    identity->active_view.schema_id = 1U;
    identity->active_view.schema_version = 1U;
    memcpy(identity->active_view.principal, peer.principal,
           sizeof(identity->active_view.principal));
    memset(identity->active_view.body_digest, 0x31,
           sizeof(identity->active_view.body_digest));
    identity->slot_generation = 1U;
    identity->generation_high_water = 1U;
    identity->occupied = 1U;
    identity->phase = UCN_I_IDENTITY_BINDING_ACTIVE;

    memset(admission, 0, sizeof(*admission));
    admission->key.transaction_id = 500U;
    admission->admitted.binding = identity->active_view;
    admission->admitted.link.id = 2U;
    admission->admitted.link.generation = 30U;
    admission->admitted.session_generation = 40U;
    admission->admitted.admission_generation = 50U;
    admission->slot_generation = 1U;
    admission->occupied = 1U;
    admission->phase = UCN_I_ADMISSION_FINAL_DURABLE;

    memset(security, 0, sizeof(*security));
    security->candidate.local = security_binding(local);
    security->candidate.peer = security_binding(peer);
    security->candidate.expires_at_us = 9000U;
    security->candidate.link_generation = 30U;
    security->candidate.session_generation = 40U;
    security->candidate.policy_generation = 60U;
    security->candidate.origin_tx.key_id = 1U;
    security->candidate.origin_tx.key_generation = 61U;
    security->candidate.origin_tx.suite_id = 1U;
    security->candidate.origin_rx = security->candidate.origin_tx;
    memset(security->candidate.origin_fingerprint, 0x55,
           sizeof(security->candidate.origin_fingerprint));
    security->candidate.origin_level = 1U;
    security->candidate.hop_profile = 1U;
    security->durability.absolute_deadline_us = 9000U;
    security->slot_generation = 1U;
    security->occupied = 1U;
    security->phase = UCN_I_SECURITY_SESSION_ACTIVE;

    memset(capability, 0, sizeof(*capability));
    capability->reference.runtime_instance = runtime_instance;
    capability->reference.realm_id = 7U;
    capability->reference.address = peer.address;
    capability->reference.binding_generation = peer.generation;
    capability->reference.session_generation = 40U;
    capability->reference.link_id = 2U;
    capability->reference.link_generation = 30U;
    capability->reference.security_owner_instance =
        value->security_owner->owner_instance;
    memcpy(capability->reference.principal, peer.principal,
           sizeof(capability->reference.principal));
    capability->record.capability_generation = 70U;
    capability->record.link.link_instance_generation = 30U;
    capability->record.link.carrier_mtu = 512U;
    capability->record.link.link_frame_mtu = 512U;
    capability->record.link.processing_frame_mtu = 512U;
    capability->record.peer.feature_bits =
        UCN_I_CAPABILITY_REQUIRED_BASE_FEATURES;
    capability->discovery_deadline_us = 8000U;
    capability->capability_deadline_us = 8000U;
    memset(capability->digest, 0x71, sizeof(capability->digest));
    value->capability_owner->peers[0].occupied = 1U;

    memset(route, 0, sizeof(*route));
    route->domain.origin = local;
    route->domain.destination = peer;
    route->domain.realm = 7U;
    route->domain.origin_session_generation = 80U;
    route->next_hop.peer = peer;
    route->next_hop.link_generation = 31U;
    route->next_hop.cost = 3U;
    route->next_hop.link_id = 2U;
    route->next_hop.frame_mtu = 256U;
    route->next_hop.capability_bits = 3U;
    route->route_causal_id = 81U;
    route->expires_at_us = 7000U;
    route->runtime_instance = runtime_instance;
    route->route_generation = 90U;
    route->cost = 3U;
    route->owner_instance = value->route_owner->owner_instance;
    route->path_frame_mtu = 256U;
    route->capability_bits = 3U;
    route->hop_count = 1U;
    value->route_owner->routes[0].occupied = 1U;

    memset(flow, 0, sizeof(*flow));
    flow->proposal.key.route_domain = route->domain;
    flow->proposal.key.route_generation = route->route_generation;
    flow->proposal.key.forward_label = 7U;
    flow->proposal.source_route = *route;
    memcpy(flow->proposal.capability.digest, capability->digest,
           sizeof(flow->proposal.capability.digest));
    flow->proposal.capability.expires_at_us =
        capability->capability_deadline_us;
    flow->proposal.capability.runtime_instance = runtime_instance;
    flow->proposal.capability.session_generation = 40U;
    flow->proposal.capability.capability_generation = 70U;
    flow->proposal.capability.feature_bits =
        capability->record.peer.feature_bits;
    flow->proposal.capability.security_owner_instance =
        value->security_owner->owner_instance;
    flow->proposal.requirements.expires_at_us = 6500U;
    flow->proposal.requirements.required_feature_bits = 1U;
    flow->proposal.requirements.policy_generation = 60U;
    flow->proposal.requirements.service_id = 11U;
    flow->proposal.requirements.protocol_opcode =
        UCN_I_OPCODE_OPERATION_QUERY;
    flow->proposal.requirements.minimum_payload_bytes = 32U;
    flow->proposal.requirements.path_profile_id = 1U;
    flow->proposal.requirements.contract = 4U;
    flow->proposal.requirements.traffic_ceiling = 2U;
    flow->proposal.requirements.delivery = 0U;
    flow->proposal.requirements.interaction = 0U;
    flow->proposal.requirements.payload_kind = 0U;
    flow->proposal.requirements.origin_security = 1U;
    flow->proposal.requirements.hop_profile = 1U;
    flow->proposal.flow_expires_at_us = 6500U;
    flow->proposal.context_id = 9U;
    flow->proposal.payload_budget = 128U;
    memset(flow->fingerprint, 0x91, sizeof(flow->fingerprint));
    flow->flow_generation = 1U;
    flow->next_sequence = 1U;
    flow->generation = 1U;
    flow->phase = UCN_I_FLOW_ACTIVE;
    flow->occupied = 1U;

    memset(&value->request, 0, sizeof(value->request));
    value->request.struct_size = (uint16_t)sizeof(value->request);
    value->request.schema = UCN_I_COMPOSITION_ADVANCED_USE_SCHEMA;
    value->request.now_us = 100U;
    value->request.identity.runtime_instance = runtime_instance;
    value->request.identity.slot_generation = 1U;
    value->request.identity.binding_generation = 20U;
    value->request.identity.owner_instance =
        value->identity_owner->owner_instance;
    value->request.identity.slot = 0U;
    value->request.admission.runtime_instance = runtime_instance;
    value->request.admission.slot_generation = 1U;
    value->request.admission.transaction_id = 500U;
    value->request.admission.owner_instance =
        value->admission_owner->owner_instance;
    value->request.admission.slot = 1U;
    value->request.security.session.runtime_instance = runtime_instance;
    value->request.security.session.owner_instance =
        value->security_owner->owner_instance;
    value->request.security.session.slot = 0U;
    value->request.security.session.slot_generation = 1U;
    value->request.security.session.session_generation = 40U;
    memset(value->request.security.context_fingerprint, 0x55,
           sizeof(value->request.security.context_fingerprint));
    value->request.security.service_id = 11U;
    value->request.security.protocol_opcode = UCN_I_OPCODE_OPERATION_QUERY;
    value->request.security.direction = UCN_I_SECURITY_ACCESS_OUTBOUND;
    value->request.security_facts.local = security_binding(local);
    value->request.security_facts.peer = security_binding(peer);
    value->request.security_facts.now_us = value->request.now_us;
    value->request.security_facts.link_generation = 30U;
    value->request.security_facts.policy_generation = 60U;
    value->request.capability = capability->reference;
    value->request.route_domain = route->domain;
    value->request.flow.runtime_instance = runtime_instance;
    value->request.flow.owner_instance = value->flow_owner->owner_instance;
    value->request.flow.slot = 1U;
    value->request.flow.generation = 1U;
    value->request.flow.object_kind = TEST_FLOW_ACTIVE_KIND;
    value->request.flow_request.service_id = 11U;
    value->request.flow_request.protocol_opcode =
        UCN_I_OPCODE_OPERATION_QUERY;
    value->request.flow_request.payload_bytes = 64U;
    value->request.flow_request.contract = 4U;
    value->request.flow_request.traffic_class = 1U;
    value->request.flow_request.origin_security = 1U;
    value->request.required_feature_bits = 1U;
    value->request.route_link_generation = 31U;
    value->request.required_path_capability_bits = 1U;
    value->request.minimum_payload_bytes = 32U;

    for (index = 0U; index < value->runtime->plan.init_count; ++index) {
        ucn_i_composition_owner_record_t *record = record_for(
            value->runtime, value->runtime->plan.init_order[index]);
        CHECK(record != NULL);
        if (record != NULL) {
            record->state = UCN_I_COMPOSITION_OWNER_ACTIVE;
        }
    }
    value->runtime->initialized_count = value->runtime->plan.init_count;
    value->runtime->persistence_ready = 1U;
    value->runtime->registry[0].state = UCN_I_COMPOSITION_OWNER_ACTIVE;
    value->runtime->phase = UCN_I_COMPOSITION_RUNNING;
}

static void fixture_init(test_fixture_t *value)
{
    ucn_i_composition_request_t request;
    ucn_i_composition_plan_t plan;
    ucn_i_composition_prepare_config_t prepare;
    size_t required = 0U;

    memset(value, 0, sizeof(*value));
    memset(&request, 0, sizeof(request));
    request.struct_size = (uint16_t)sizeof(request);
    request.schema = UCN_I_COMPOSITION_SCHEMA;
    request.profile = UCN_PROFILE;
    request.available_module_mask =
        UCN_I_COMPOSE_FOUNDATION | UCN_I_COMPOSE_PERSISTENCE |
        UCN_I_COMPOSE_IDENTITY | UCN_I_COMPOSE_SECURITY |
        UCN_I_COMPOSE_ADMISSION | UCN_I_COMPOSE_CAPABILITY |
        UCN_I_COMPOSE_ROUTE | UCN_I_COMPOSE_FLOW |
        UCN_I_COMPOSE_TRANSPORT | UCN_I_COMPOSE_SERVICE;
    request.capability_mask =
        UCN_I_CAP_STATIC_C1 | UCN_I_CAP_DYNAMIC_ADMISSION |
        UCN_I_CAP_AUTO_ROUTE | UCN_I_CAP_ADVANCED_FLOW |
        UCN_I_CAP_RELIABLE |
        UCN_I_CAP_TRANSFER | UCN_I_CAP_SERVICE |
        UCN_I_CAP_DURABLE_OPERATION;
    CHECK(ucn_i_composition_plan_build(&request, &plan) == UCN_OK);
    CHECK(ucn_i_composition_storage_required(&plan, &required) == UCN_OK);
    CHECK(required <= sizeof(value->composition_storage));
    memset(&prepare, 0, sizeof(prepare));
    prepare.struct_size = (uint16_t)sizeof(prepare);
    prepare.schema = UCN_I_COMPOSITION_RUNTIME_SCHEMA;
    prepare.runtime_instance = 101U;
    prepare.owner_instance_base = 100U;
    prepare.coordinator_lock = internal_lock(&value->composition_lock);
    CHECK(ucn_i_composition_prepare(
              value->composition_storage.bytes, required, &plan, &prepare,
              &value->runtime) == UCN_OK);
    CHECK(value->runtime != NULL);
    foundation_init(value, value->runtime->runtime_instance,
                    value->runtime->registry[0].ref.owner_instance);
    owners_init(value);
    active_state_install(value);
    CHECK(ucn_i_composition_runtime_valid(value->runtime));
}

static void plan_sentinel(ucn_i_composition_advanced_use_plan_t *plan)
{
    memset(plan, 0xA5, sizeof(*plan));
}

static void expect_preflight_failure(
    test_fixture_t *value,
    ucn_result_t expected)
{
    ucn_i_composition_advanced_use_workspace_t workspace;
    ucn_i_composition_advanced_use_plan_t plan;
    ucn_i_composition_advanced_use_plan_t before;

    memset(&workspace, 0xC3, sizeof(workspace));
    plan_sentinel(&plan);
    before = plan;
    CHECK(ucn_i_composition_advanced_use_preflight(
              value->runtime, &value->request, &workspace,
              &plan) == expected);
    CHECK(memcmp(&plan, &before, sizeof(plan)) == 0);
    CHECK(value->flow_owner->flows[0].sequence_reserved == 0U);
}

static void test_success_abort_commit_and_acl_binding(void)
{
    ucn_i_composition_advanced_use_workspace_t workspace;
    ucn_i_composition_advanced_use_plan_t plan;
    ucn_i_composition_advanced_use_plan_t old_plan;

    fixture_init(&fixture);
    memset(&workspace, 0, sizeof(workspace));
    memset(&plan, 0, sizeof(plan));
    CHECK(ucn_i_composition_advanced_use_preflight(
              fixture.runtime, &fixture.request, &workspace,
              &plan) == UCN_OK);
    CHECK(plan.struct_size == sizeof(plan));
    CHECK(plan.schema == UCN_I_COMPOSITION_ADVANCED_USE_SCHEMA);
    CHECK(plan.runtime_instance == fixture.runtime->runtime_instance);
    CHECK(plan.identity_binding_generation == 20U);
    CHECK(plan.admission_generation == 50U);
    CHECK(plan.security_session_generation == 40U);
    CHECK(plan.capability_generation == 70U);
    CHECK(plan.route_generation == 90U);
    CHECK(plan.route_kind == UCN_I_ROUTE_RESOLVED_DYNAMIC);
    CHECK(plan.effective_deadline_us == 6500U);
    CHECK(plan.flow_plan.sequence == 1U);
    CHECK(plan.flow_plan.flow_generation == 1U);
    CHECK(plan.flow_plan.reservation_generation == 1U);
    old_plan = plan;
    CHECK(fixture.flow_owner->flows[0].sequence_reserved == 1U);
    CHECK(ucn_i_composition_advanced_use_finish(
              fixture.runtime, &plan, 0U) == UCN_OK);
    CHECK(fixture.flow_owner->flows[0].sequence_reserved == 0U);
    CHECK(fixture.flow_owner->flows[0].next_sequence == 1U);

    CHECK(ucn_i_composition_advanced_use_preflight(
              fixture.runtime, &fixture.request, &workspace,
              &plan) == UCN_OK);
    CHECK(plan.flow_plan.reservation_generation == 2U);
    CHECK(ucn_i_composition_advanced_use_finish(
              fixture.runtime, &old_plan, 0U) == UCN_ERR_STATE);
    CHECK(fixture.flow_owner->flows[0].sequence_reserved == 1U);
    CHECK(ucn_i_composition_advanced_use_finish(
              fixture.runtime, &plan, 1U) == UCN_OK);
    CHECK(fixture.flow_owner->flows[0].next_sequence == 2U);

    fixture.request.flow_request.service_id = 12U;
    fixture.request.security.service_id = 12U;
    expect_preflight_failure(&fixture, UCN_ERR_ACCESS);
}

static void test_each_stale_layer_and_order(void)
{
    ucn_i_identity_slot_t identity_before;
    ucn_i_admission_slot_t admission_before;
    ucn_i_security_slot_t security_before;
    ucn_i_cached_capability_t capability_before;
    ucn_i_route_soft_view_t route_before;
    ucn_i_flow_active_record_t flow_before;

    fixture_init(&fixture);
    identity_before = fixture.identity_owner->bindings[0];
    admission_before = fixture.admission_owner->pending[0];
    security_before = fixture.security_owner->sessions[0];
    capability_before = fixture.capability_owner->peers[0].value;
    route_before = fixture.route_owner->routes[0].value;
    flow_before = fixture.flow_owner->flows[0];

    fixture.identity_owner->bindings[0].phase =
        UCN_I_IDENTITY_BINDING_FENCED;
    expect_preflight_failure(&fixture, UCN_ERR_ACCESS);
    fixture.identity_owner->bindings[0] = identity_before;

    fixture.admission_owner->pending[0].phase = UCN_I_ADMISSION_ABORTED;
    expect_preflight_failure(&fixture, UCN_ERR_STATE);
    fixture.admission_owner->pending[0] = admission_before;

    fixture.security_owner->sessions[0].phase =
        UCN_I_SECURITY_SESSION_FENCED;
    expect_preflight_failure(&fixture, UCN_ERR_STATE);
    fixture.security_owner->sessions[0] = security_before;

    fixture.capability_owner->peers[0].occupied = 0U;
    expect_preflight_failure(&fixture, UCN_ERR_NOT_FOUND);
    fixture.capability_owner->peers[0].value = capability_before;
    fixture.capability_owner->peers[0].occupied = 1U;

    fixture.route_owner->routes[0].occupied = 0U;
    expect_preflight_failure(&fixture, UCN_ERR_NOT_FOUND);
    fixture.route_owner->routes[0].value = route_before;
    fixture.route_owner->routes[0].occupied = 1U;

    fixture.request.route_link_generation += 1U;
    expect_preflight_failure(&fixture, UCN_ERR_NOT_FOUND);
    fixture.request.route_link_generation -= 1U;

    fixture.flow_owner->flows[0].phase = UCN_I_FLOW_FENCED;
    expect_preflight_failure(&fixture, UCN_ERR_ACCESS);
    fixture.flow_owner->flows[0] = flow_before;

    /* All earlier owners remain current at this instant.  The half-open Flow
     * lifetime alone has expired, so the request must reach Flow and stop
     * without reserving a sequence. */
    fixture.request.now_us = 6500U;
    fixture.request.security_facts.now_us = 6500U;
    expect_preflight_failure(&fixture, UCN_ERR_ACCESS);
    fixture.request.now_us = 100U;
    fixture.request.security_facts.now_us = 100U;

    /* Security is earlier than Capability: simultaneous invalidity must stop
     * at Security and never reserve a Flow sequence. */
    fixture.security_owner->sessions[0].phase =
        UCN_I_SECURITY_SESSION_FENCED;
    fixture.capability_owner->peers[0].occupied = 0U;
    expect_preflight_failure(&fixture, UCN_ERR_STATE);
}

static void test_request_zero_write_and_lifecycle_gate(void)
{
    ucn_i_composition_advanced_use_workspace_t workspace;
    ucn_i_composition_advanced_use_plan_t plan;
    ucn_i_composition_advanced_use_plan_t before;
    ucn_i_callback_claim_t claim;

    fixture_init(&fixture);
    plan_sentinel(&plan);
    before = plan;
    fixture.request.minimum_payload_bytes = 0U;
    CHECK(ucn_i_composition_advanced_use_preflight(
              fixture.runtime, &fixture.request, &workspace,
              &plan) == UCN_ERR_ARGUMENT);
    CHECK(memcmp(&plan, &before, sizeof(plan)) == 0);
    fixture.request.minimum_payload_bytes = 32U;

    memset(&claim, 0, sizeof(claim));
    claim.owner_instance = 999U;
    claim.operation_id = 1U;
    claim.operation_generation = 1U;
    claim.operation_kind = 1U;
    CHECK(ucn_i_callback_gate_enter(&fixture.runtime->lifecycle_gate,
                                    &claim) == UCN_OK);
    CHECK(ucn_i_composition_advanced_use_preflight(
              fixture.runtime, &fixture.request, &workspace,
              &plan) == UCN_ERR_STATE);
    CHECK(memcmp(&plan, &before, sizeof(plan)) == 0);
    CHECK(ucn_i_callback_gate_leave(&fixture.runtime->lifecycle_gate,
                                    &claim) == UCN_OK);
}

static bool handle_zero(ucn_handle_t value)
{
    const uint8_t *bytes = (const uint8_t *)&value;
    size_t index;

    for (index = 0U; index < sizeof(value); ++index) {
        if (bytes[index] != 0U) {
            return false;
        }
    }
    return true;
}

static void delivery_configure(
    test_fixture_t *value,
    uint8_t transport_mode,
    uint8_t interaction,
    uint8_t traffic_class,
    const uint8_t *body,
    size_t body_bytes,
    ucn_i_composition_delivery_request_t *delivery)
{
    ucn_i_sha256_workspace_t digest_workspace;
    uint8_t flow_delivery = transport_mode ==
            UCN_I_COMPOSITION_TRANSPORT_RELIABLE ?
        UCN_DELIVERY_RELIABLE : UCN_DELIVERY_BEST_EFFORT;

    memset(delivery, 0, sizeof(*delivery));
    delivery->struct_size = (uint16_t)sizeof(*delivery);
    delivery->schema = UCN_I_COMPOSITION_DELIVERY_SCHEMA;
    delivery->transport_mode = transport_mode;
    if (interaction != UCN_INTERACTION_ONE_WAY) {
        delivery->operation_id = 901U;
    }
    if (interaction == UCN_INTERACTION_REQUEST) {
        delivery->request_deadline_us = 5000U;
    }
    if (transport_mode == UCN_I_COMPOSITION_TRANSPORT_RELIABLE) {
        memset(delivery->aad_digest, 0xA1,
               sizeof(delivery->aad_digest));
        memset(delivery->payload_digest, 0xB2,
               sizeof(delivery->payload_digest));
    } else if (transport_mode == UCN_I_COMPOSITION_TRANSPORT_TRANSFER) {
        memset(&digest_workspace, 0, sizeof(digest_workspace));
        CHECK(UCN_I_DIGEST_CANONICAL("UCN6-TRANSFER-MESSAGE-V1", body, body_bytes,
                               delivery->payload_digest,
                               &digest_workspace) == UCN_OK);
        delivery->transfer_template.total_length = (uint32_t)body_bytes;
        delivery->transfer_template.fragment_budget = 16U;
        delivery->transfer_template.fragment_count =
            (uint16_t)((body_bytes + 15U) / 16U);
        delivery->transfer_template.lifetime_ms = 1000U;
        if (interaction == UCN_INTERACTION_REQUEST) {
            delivery->transfer_template.operation_flags = 1U;
        }
    }

    value->request.flow_request.payload_bytes =
        body_bytes == 0U ? 64U : (uint16_t)body_bytes;
    value->request.flow_request.traffic_class = traffic_class;
    value->request.flow_request.delivery = flow_delivery;
    value->request.flow_request.interaction = interaction;
    value->flow_owner->flows[0].proposal.requirements.traffic_ceiling =
        traffic_class;
    value->flow_owner->flows[0].proposal.requirements.delivery =
        flow_delivery;
    value->flow_owner->flows[0].proposal.requirements.interaction =
        interaction;
}

static void test_reliable_request_q1_atomic_bundle(void)
{
    uint8_t body[48];
    ucn_i_composition_delivery_request_t delivery;
    ucn_i_composition_delivery_workspace_t workspace;
    ucn_i_composition_delivery_cancel_workspace_t cancel_workspace;
    ucn_i_composition_delivery_plan_t plan;
    ucn_i_composition_delivery_plan_t forged;
    ucn_i_transport_reliable_view_t transport_view;
    ucn_i_service_request_view_t service_view;
    ucn_i_service_qos_view_t qos_view;

    memset(body, 0x6A, sizeof(body));
    fixture_init(&fixture);
    delivery_configure(&fixture, UCN_I_COMPOSITION_TRANSPORT_RELIABLE,
                       UCN_INTERACTION_REQUEST, 1U,
                       body, sizeof(body), &delivery);
    memset(&workspace, 0, sizeof(workspace));
    memset(&plan, 0, sizeof(plan));
    CHECK(ucn_i_composition_delivery_prepare(
              fixture.runtime, &fixture.request, &delivery,
              body, sizeof(body), &workspace, &plan) == UCN_OK);
    CHECK(plan.transport_mode == UCN_I_COMPOSITION_TRANSPORT_RELIABLE);
    CHECK(plan.interaction == UCN_INTERACTION_REQUEST);
    CHECK(plan.service_reserved == 1U);
    CHECK(plan.use.flow_plan.flow_generation == 1U);
    CHECK(plan.use.flow_plan.sequence == 1U);
    CHECK(plan.queue_deadline_us == 1100U);
    CHECK(ucn_i_transport_reliable_view(
              fixture.transport_owner, plan.transport,
              &transport_view) == UCN_OK);
    CHECK(transport_view.phase == UCN_I_TRANSPORT_RELIABLE_READY);
    CHECK(transport_view.key.origin_sequence == 1U);
    CHECK(ucn_i_service_request_view(
              fixture.service_owner, plan.service,
              &service_view) == UCN_OK);
    CHECK(service_view.phase == UCN_I_SERVICE_REQUEST_WAIT_SEND);
    CHECK(service_view.key.operation_id == 901U);
    CHECK(ucn_i_service_qos_view(fixture.service_owner, plan.qos,
                                 &qos_view) == UCN_OK);
    CHECK(qos_view.item.traffic_class == 1U);
    CHECK(memcmp(&qos_view.item.sendable, &plan.transport,
                 sizeof(plan.transport)) == 0);

    forged = plan;
    forged.service_reserved = 0U;
    memset(&forged.service, 0, sizeof(forged.service));
    CHECK(ucn_i_composition_delivery_cancel(
              fixture.runtime, &forged, &cancel_workspace) ==
          UCN_ERR_ARGUMENT);
    CHECK(ucn_i_transport_reliable_view(
              fixture.transport_owner, plan.transport,
              &transport_view) == UCN_OK);
    CHECK(ucn_i_service_request_view(
              fixture.service_owner, plan.service,
              &service_view) == UCN_OK);
    CHECK(ucn_i_service_qos_view(fixture.service_owner, plan.qos,
                                 &qos_view) == UCN_OK);
    CHECK(fixture.flow_owner->flows[0].sequence_reserved == 1U);

    CHECK(ucn_i_composition_delivery_cancel(
              fixture.runtime, &plan, &cancel_workspace) == UCN_OK);
    CHECK(ucn_i_transport_reliable_view(
              fixture.transport_owner, plan.transport,
              &transport_view) == UCN_ERR_NOT_FOUND);
    CHECK(ucn_i_service_request_view(
              fixture.service_owner, plan.service,
              &service_view) == UCN_ERR_NOT_FOUND);
    CHECK(ucn_i_service_qos_view(fixture.service_owner, plan.qos,
                                 &qos_view) == UCN_ERR_NOT_FOUND);
    CHECK(fixture.flow_owner->flows[0].sequence_reserved == 0U);
    CHECK(fixture.flow_owner->flows[0].next_sequence == 1U);
}

static void test_transfer_request_q3_flow_binding(void)
{
    uint8_t body[48];
    ucn_i_composition_delivery_request_t delivery;
    ucn_i_composition_delivery_workspace_t workspace;
    ucn_i_composition_delivery_cancel_workspace_t cancel_workspace;
    ucn_i_composition_delivery_plan_t plan;
    ucn_i_transport_transfer_view_t transfer_view;
    ucn_i_service_qos_view_t qos_view;

    memset(body, 0x3C, sizeof(body));
    fixture_init(&fixture);
    delivery_configure(&fixture, UCN_I_COMPOSITION_TRANSPORT_TRANSFER,
                       UCN_INTERACTION_REQUEST, 3U,
                       body, sizeof(body), &delivery);
    memset(&workspace, 0, sizeof(workspace));
    memset(&plan, 0, sizeof(plan));
    CHECK(ucn_i_composition_delivery_prepare(
              fixture.runtime, &fixture.request, &delivery,
              body, sizeof(body), &workspace, &plan) == UCN_OK);
    CHECK(plan.transport_mode == UCN_I_COMPOSITION_TRANSPORT_TRANSFER);
    CHECK(plan.service_reserved == 1U);
    CHECK(ucn_i_transport_transfer_view(
              fixture.transport_owner, plan.transport,
              &transfer_view) == UCN_OK);
    CHECK(transfer_view.phase == UCN_I_TRANSPORT_TRANSFER_SETUP_PENDING);
    CHECK(transfer_view.setup.parent_kind ==
          UCN_I_TRANSPORT_PARENT_KIND_FLOW);
    CHECK(transfer_view.setup.parent_id == 9U);
    CHECK(transfer_view.setup.parent_generation == 1U);
    CHECK(transfer_view.setup.transfer_id == 1U);
    CHECK(transfer_view.setup.service_id == 11U);
    CHECK(transfer_view.setup.interaction == UCN_INTERACTION_REQUEST);
    CHECK(memcmp(transfer_view.setup.message_digest,
                 delivery.payload_digest,
                 sizeof(delivery.payload_digest)) == 0);
    CHECK(ucn_i_service_qos_view(fixture.service_owner, plan.qos,
                                 &qos_view) == UCN_OK);
    CHECK(qos_view.item.traffic_class == 3U);
    CHECK(ucn_i_composition_delivery_cancel(
              fixture.runtime, &plan, &cancel_workspace) == UCN_OK);
}

static void test_q2_modes_do_not_mix_transport_and_service(void)
{
    uint8_t body[32];
    size_t index;
    ucn_i_composition_delivery_request_t delivery;
    ucn_i_composition_delivery_workspace_t workspace;
    ucn_i_composition_delivery_cancel_workspace_t cancel_workspace;
    ucn_i_composition_delivery_plan_t plan;
    ucn_i_service_qos_view_t qos_view;

    memset(body, 0x4D, sizeof(body));
    fixture_init(&fixture);
    delivery_configure(&fixture, UCN_I_COMPOSITION_TRANSPORT_RELIABLE,
                       UCN_INTERACTION_ONE_WAY, 2U,
                       body, sizeof(body), &delivery);
    CHECK(ucn_i_composition_delivery_prepare(
              fixture.runtime, &fixture.request, &delivery,
              body, sizeof(body), &workspace, &plan) == UCN_OK);
    CHECK(plan.service_reserved == 0U);
    CHECK(handle_zero(plan.service));
    CHECK(!handle_zero(plan.transport));
    for (index = 0U; index < UCN_I_SERVICE_REQUEST_COUNT; ++index) {
        CHECK(fixture.service_owner->requests[index].occupied == 0U);
    }
    CHECK(ucn_i_service_qos_view(fixture.service_owner, plan.qos,
                                 &qos_view) == UCN_OK);
    CHECK(qos_view.item.traffic_class == 2U);
    CHECK(memcmp(&qos_view.item.sendable, &plan.transport,
                 sizeof(plan.transport)) == 0);
    CHECK(ucn_i_composition_delivery_cancel(
              fixture.runtime, &plan, &cancel_workspace) == UCN_OK);

    fixture_init(&fixture);
    delivery_configure(&fixture, UCN_I_COMPOSITION_TRANSPORT_NONE,
                       UCN_INTERACTION_REQUEST, 2U,
                       NULL, 0U, &delivery);
    CHECK(ucn_i_composition_delivery_prepare(
              fixture.runtime, &fixture.request, &delivery,
              NULL, 0U, &workspace, &plan) == UCN_OK);
    CHECK(plan.service_reserved == 1U);
    CHECK(handle_zero(plan.transport));
    CHECK(!handle_zero(plan.service));
    CHECK(ucn_i_service_qos_view(fixture.service_owner, plan.qos,
                                 &qos_view) == UCN_OK);
    CHECK(qos_view.item.traffic_class == 2U);
    CHECK(memcmp(&qos_view.item.sendable, &plan.service,
                 sizeof(plan.service)) == 0);
    CHECK(ucn_i_composition_delivery_cancel(
              fixture.runtime, &plan, &cancel_workspace) == UCN_OK);
}

static void test_qos_failure_rolls_back_all_reservations(void)
{
    uint8_t body[48];
    size_t index;
    ucn_i_composition_delivery_request_t delivery;
    ucn_i_composition_delivery_workspace_t workspace;
    ucn_i_composition_delivery_plan_t plan;
    ucn_i_composition_delivery_plan_t before;

    memset(body, 0xC4, sizeof(body));
    fixture_init(&fixture);
    delivery_configure(&fixture, UCN_I_COMPOSITION_TRANSPORT_RELIABLE,
                       UCN_INTERACTION_REQUEST, 2U,
                       body, sizeof(body), &delivery);
    for (index = 0U; index < UCN_I_SERVICE_QOS_COUNT; ++index) {
        fixture.service_owner->qos[index].occupied = 1U;
        fixture.service_owner->qos[index].generation = 1U;
    }
    memset(&workspace, 0, sizeof(workspace));
    memset(&plan, 0xA5, sizeof(plan));
    before = plan;
    CHECK(ucn_i_composition_delivery_prepare(
              fixture.runtime, &fixture.request, &delivery,
              body, sizeof(body), &workspace, &plan) == UCN_ERR_NO_SPACE);
    CHECK(memcmp(&plan, &before, sizeof(plan)) == 0);
    CHECK(fixture.flow_owner->flows[0].sequence_reserved == 0U);
    for (index = 0U; index < UCN_I_TRANSPORT_RELIABLE_TX_COUNT; ++index) {
        CHECK(fixture.transport_owner->reliable_tx[index].occupied == 0U);
    }
    for (index = 0U; index < UCN_I_SERVICE_REQUEST_COUNT; ++index) {
        CHECK(fixture.service_owner->requests[index].occupied == 0U);
    }
}

static void test_q0_and_mixed_delivery_semantics_rejected(void)
{
    uint8_t body[48];
    ucn_i_composition_delivery_request_t delivery;
    ucn_i_composition_delivery_workspace_t workspace;
    ucn_i_composition_delivery_plan_t plan;
    ucn_i_composition_delivery_plan_t before;

    memset(body, 0xD5, sizeof(body));
    fixture_init(&fixture);
    delivery_configure(&fixture, UCN_I_COMPOSITION_TRANSPORT_RELIABLE,
                       UCN_INTERACTION_REQUEST, 0U,
                       body, sizeof(body), &delivery);
    memset(&plan, 0xA5, sizeof(plan));
    before = plan;
    CHECK(ucn_i_composition_delivery_prepare(
              fixture.runtime, &fixture.request, &delivery,
              body, sizeof(body), &workspace, &plan) == UCN_ERR_ARGUMENT);
    CHECK(memcmp(&plan, &before, sizeof(plan)) == 0);
    CHECK(fixture.flow_owner->flows[0].sequence_reserved == 0U);

    delivery_configure(&fixture, UCN_I_COMPOSITION_TRANSPORT_RELIABLE,
                       UCN_INTERACTION_REQUEST, 1U,
                       body, sizeof(body), &delivery);
    delivery.transfer_template.lifetime_ms = 1U;
    CHECK(ucn_i_composition_delivery_prepare(
              fixture.runtime, &fixture.request, &delivery,
              body, sizeof(body), &workspace, &plan) == UCN_ERR_ARGUMENT);
    CHECK(memcmp(&plan, &before, sizeof(plan)) == 0);
    CHECK(fixture.flow_owner->flows[0].sequence_reserved == 0U);

    delivery_configure(&fixture, UCN_I_COMPOSITION_TRANSPORT_RELIABLE,
                       UCN_INTERACTION_REQUEST, 1U,
                       body, sizeof(body), &delivery);
    fixture.request.flow_request.payload_bytes =
        (uint16_t)(sizeof(body) - 1U);
    CHECK(ucn_i_composition_delivery_prepare(
              fixture.runtime, &fixture.request, &delivery,
              body, sizeof(body), &workspace, &plan) == UCN_ERR_ARGUMENT);
    CHECK(memcmp(&plan, &before, sizeof(plan)) == 0);
    CHECK(fixture.flow_owner->flows[0].sequence_reserved == 0U);
}

static void test_durable_operation_stays_out_of_qos(void)
{
    size_t index;
    ucn_i_service_operation_key_t key;
    ucn_i_service_operation_durability_t durability;
    ucn_i_service_operation_requirement_t requirement;
    ucn_i_service_operation_requirement_t requirement_before;
    ucn_i_composition_durable_operation_workspace_t workspace;
    ucn_handle_t operation;
    ucn_handle_t operation_before;

    fixture_init(&fixture);
    memset(&key, 0, sizeof(key));
    key.service.client.address = 1U;
    key.service.client.generation = 10U;
    memset(key.service.client.principal, 0x11,
           sizeof(key.service.client.principal));
    key.service.server.address = 2U;
    key.service.server.generation = 20U;
    memset(key.service.server.principal, 0x22,
           sizeof(key.service.server.principal));
    key.service.security.session_generation = 40U;
    key.service.security.key_generation = 61U;
    key.service.security.policy_generation = 60U;
    key.service.security.origin_security = 1U;
    key.service.security.acl_authorized = 1U;
    key.service.operation_id = 777U;
    key.service.realm = 7U;
    key.service.service_id = 11U;
    key.service.opcode = UCN_I_OPCODE_OPERATION_QUERY;
    memset(key.request_digest, 0x77, sizeof(key.request_digest));

    memset(&durability, 0, sizeof(durability));
    durability.domain_id = 501U;
    durability.foundation_transaction_id = 1U;
    durability.expected_record_generation = 0U;
    durability.absolute_deadline_us = 5000U;
    durability.volatile_continuation.runtime_instance =
        fixture.runtime->runtime_instance;
    durability.volatile_continuation.owner_instance =
        fixture.service_owner->owner_instance;
    durability.volatile_continuation.slot = 1U;
    durability.volatile_continuation.generation = 1U;
    durability.volatile_continuation.object_kind = UCN_OBJECT_KIND_SEND;
    durability.domain_generation = 1U;
    durability.schema_id = 1U;
    durability.schema_version = 1U;

    memset(&operation, 0xA5, sizeof(operation));
    memset(&requirement, 0x5A, sizeof(requirement));
    operation_before = operation;
    requirement_before = requirement;
    key.service.security.authenticated_replay_candidate = 1U;
    CHECK(ucn_i_composition_durable_operation_begin(
              fixture.runtime, &key, &durability, 100U,
              &workspace, &operation, &requirement) == UCN_ERR_ARGUMENT);
    CHECK(memcmp(&operation, &operation_before, sizeof(operation)) == 0);
    CHECK(memcmp(&requirement, &requirement_before,
                 sizeof(requirement)) == 0);
    key.service.security.authenticated_replay_candidate = 0U;
    CHECK(ucn_i_composition_durable_operation_begin(
              fixture.runtime, &key, &durability, 100U,
              &workspace, &operation, &requirement) == UCN_OK);
    CHECK(!handle_zero(operation));
    CHECK(requirement.next_phase == UCN_I_SERVICE_OPERATION_PREPARED);
    CHECK(requirement.durability.domain_id == durability.domain_id);
    CHECK(requirement.body_bytes != 0U);
    CHECK(fixture.flow_owner->flows[0].sequence_reserved == 0U);
    for (index = 0U; index < UCN_I_SERVICE_QOS_COUNT; ++index) {
        CHECK(fixture.service_owner->qos[index].occupied == 0U);
    }
}

static size_t control_packet_build_profile(test_fixture_t *value,
                                           uint16_t opcode,
                                           uint8_t traffic_class,
                                           uint8_t delivery_guarantee,
                                           uint8_t interaction_role,
                                           uint64_t transaction_id,
                                           const uint8_t *body,
                                           size_t body_bytes,
                                           uint8_t *packet,
                                           size_t packet_capacity)
{
    ucn_i_security_slot_t *session =
        &value->security_owner->sessions[0];
    ucn_i_c0_frame_t frame;
    uint8_t aad[UCN_I_C0_ORIGIN_AAD_MAX_BYTES];
    uint8_t origin_tag[UCN_I_SECURITY_ORIGIN_TAG_BYTES];
    size_t aad_bytes = 0U;
    size_t packet_bytes = 0U;

    memset(&frame, 0, sizeof(frame));
    frame.payload = body;
    frame.payload_bytes = body_bytes;
    frame.origin_tag = origin_tag;
    frame.transaction_id = transaction_id;
    frame.realm_id = 7U;
    frame.source_address = session->candidate.peer.address;
    frame.destination_address = session->candidate.local.address;
    frame.source_binding_generation =
        session->candidate.peer.binding_generation;
    frame.destination_binding_generation =
        session->candidate.local.binding_generation;
    frame.opcode = opcode;
    frame.traffic_class = traffic_class;
    frame.delivery_guarantee = delivery_guarantee;
    frame.interaction_role = interaction_role;
    frame.payload_kind = (opcode >> 8U) == UCN_I_OPCODE_DIAGNOSTICS ?
        UCN_I_PAYLOAD_DIAGNOSTIC : UCN_I_PAYLOAD_CONTROL;
    frame.origin_security = UCN_I_ORIGIN_O1;
    frame.hop_limit = 1U;
    CHECK(ucn_i_c0_origin_aad_build(&frame, 1U, aad, &aad_bytes) ==
          UCN_OK);
    security_test_tag(&session->candidate.origin_rx, aad, aad_bytes,
                      body, body_bytes, origin_tag);
    CHECK(ucn_i_c0_encode(&frame, 1U, UCN_I_CONTROL_H0,
                           packet, packet_capacity, &packet_bytes) ==
          UCN_OK);
    return packet_bytes;
}

static size_t control_packet_build_role(test_fixture_t *value,
                                        uint16_t opcode,
                                        uint8_t interaction_role,
                                        uint64_t transaction_id,
                                        const uint8_t *body,
                                        size_t body_bytes,
                                        uint8_t *packet,
                                        size_t packet_capacity)
{
    return control_packet_build_profile(
        value, opcode, UCN_TRAFFIC_Q0, UCN_DELIVERY_BEST_EFFORT,
        interaction_role, transaction_id, body, body_bytes, packet,
        packet_capacity);
}

static size_t control_packet_build(test_fixture_t *value,
                                   uint16_t opcode,
                                   uint64_t transaction_id,
                                   const uint8_t *body,
                                   size_t body_bytes,
                                   uint8_t *packet,
                                   size_t packet_capacity)
{
    return control_packet_build_role(
        value, opcode, UCN_INTERACTION_REQUEST, transaction_id, body,
        body_bytes, packet, packet_capacity);
}

static size_t bootstrap_packet_build(uint16_t opcode,
                                     uint8_t interaction_role,
                                     uint64_t transaction_id,
                                     const uint8_t *body,
                                     size_t body_bytes,
                                     uint8_t *packet,
                                     size_t packet_capacity)
{
    ucn_i_c0_frame_t frame;
    size_t packet_bytes = 0U;

    memset(&frame, 0, sizeof(frame));
    frame.payload = body;
    frame.payload_bytes = body_bytes;
    frame.transaction_id = transaction_id;
    frame.realm_id = 7U;
    frame.source_address = 0U;
    frame.destination_address = UINT8_MAX;
    frame.opcode = opcode;
    frame.traffic_class = UCN_TRAFFIC_Q0;
    frame.delivery_guarantee = UCN_DELIVERY_BEST_EFFORT;
    frame.interaction_role = interaction_role;
    frame.payload_kind = UCN_I_PAYLOAD_CONTROL;
    frame.origin_security = UCN_I_ORIGIN_O0;
    frame.hop_limit = 1U;
    CHECK(ucn_i_c0_encode(&frame, 1U, UCN_I_CONTROL_H0,
                           packet, packet_capacity, &packet_bytes) ==
          UCN_OK);
    return packet_bytes;
}

static void publish_packet(test_fixture_t *value,
                           const uint8_t *packet,
                           size_t packet_bytes)
{
    ucn_link_handle_t link;
    ucn_rx_meta_t rx_meta;

    memset(&rx_meta, 0, sizeof(rx_meta));
    rx_meta.struct_size = (uint16_t)sizeof(rx_meta);
    rx_meta.api_version = UCN_API_VERSION;
    rx_meta.timestamp_us = 100U;
    CHECK(ucn_i_composition_link_get(value->runtime, 0U, &link) == UCN_OK);
    CHECK(ucn_i_composition_driver_rx_publish(
              value->runtime, link, packet, packet_bytes,
              &rx_meta) == UCN_OK);
}

static void test_o0_bootstrap_typed_dispatch_and_origin_separation(void)
{
    ucn_i_admission_hello_t hello;
    ucn_i_admission_cookie_challenge_t challenge;
    ucn_i_admission_hello_cookie_t hello_cookie;
    ucn_i_composition_bootstrap_rx_workspace_t bootstrap_workspace;
    ucn_i_composition_bootstrap_rx_plan_t bootstrap_plan;
    ucn_i_composition_bootstrap_rx_plan_t bootstrap_before;
    ucn_i_composition_bootstrap_message_t tx_message;
    ucn_i_composition_bootstrap_tx_workspace_t tx_workspace;
    ucn_i_composition_control_tx_workspace_t retry_workspace;
    ucn_i_composition_control_tx_plan_t tx_plan;
    ucn_i_composition_control_tx_plan_t retry_plan;
    ucn_i_composition_control_rx_workspace_t control_rx_workspace;
    ucn_i_composition_control_rx_plan_t control_rx_plan;
    ucn_i_sha256_workspace_t finish_workspace;
    ucn_i_composition_owner_record_t *admission_record;
    uint8_t body[UCN_I_ADMISSION_HELLO_COOKIE_MAX_BYTES];
    uint8_t opened[8];
    uint8_t packet[UCN_ADAPTER_FRAME_BYTES];
    size_t body_bytes = 0U;
    size_t opened_bytes = SIZE_MAX;
    size_t packet_bytes;

    fixture_init(&fixture);
    admission_record = record_for(
        fixture.runtime, UCN_I_COMPOSITION_MODULE_ADMISSION);
    CHECK(admission_record != NULL);
    memset(&hello, 0, sizeof(hello));
    hello.device_nonce = UINT64_C(0x0102030405060708);
    hello.transaction_id = UINT64_C(3001);
    memset(hello.identity_digest, 0x31, sizeof(hello.identity_digest));
    CHECK(ucn_i_admission_hello_encode(&hello, body) == UCN_OK);
    packet_bytes = bootstrap_packet_build(
        UCN_I_OPCODE_BOOTSTRAP_HELLO, UCN_INTERACTION_REQUEST,
        hello.transaction_id, body, UCN_I_ADMISSION_HELLO_BYTES,
        packet, sizeof(packet));
    publish_packet(&fixture, packet, packet_bytes);

    CHECK(ucn_i_composition_control_rx_claim_prepare(
              fixture.runtime, 100U, &control_rx_workspace,
              opened, sizeof(opened), &opened_bytes,
              &control_rx_plan) == UCN_ERR_NOT_FOUND);
    CHECK(ucn_i_composition_bootstrap_rx_claim_prepare(
              fixture.runtime, &bootstrap_workspace,
              &bootstrap_plan) == UCN_OK);
    CHECK(bootstrap_plan.opcode == UCN_I_OPCODE_BOOTSTRAP_HELLO);
    CHECK(bootstrap_plan.transaction_id == hello.transaction_id);
    CHECK(bootstrap_plan.ingress_link.id == 1U);
    CHECK(bootstrap_plan.ingress_link.generation == 1U);
    CHECK(memcmp(&bootstrap_plan.target, &admission_record->ref,
                 sizeof(bootstrap_plan.target)) == 0);
    CHECK(memcmp(&bootstrap_plan.message.hello, &hello,
                 sizeof(hello)) == 0);
    CHECK(ucn_i_composition_bootstrap_rx_finish(
              fixture.runtime, &bootstrap_plan, &finish_workspace,
              true) == UCN_OK);

    memset(&tx_message, 0, sizeof(tx_message));
    tx_message.hello = hello;
    CHECK(ucn_i_composition_bootstrap_tx_submit(
              fixture.runtime, 0U, UCN_I_OPCODE_BOOTSTRAP_HELLO,
              &tx_message, &tx_workspace, &tx_plan) == UCN_OK);
    CHECK(ucn_i_c0_decode(tx_plan.frame, tx_plan.frame_bytes, 1U,
                           UCN_I_CONTROL_H0,
                           &bootstrap_workspace.frame) == UCN_OK);
    CHECK(bootstrap_workspace.frame.origin_security == UCN_I_ORIGIN_O0);
    CHECK(bootstrap_workspace.frame.opcode ==
          UCN_I_OPCODE_BOOTSTRAP_HELLO);
    CHECK(bootstrap_workspace.frame.transaction_id == hello.transaction_id);
    CHECK(bootstrap_workspace.frame.payload_bytes ==
          UCN_I_ADMISSION_HELLO_BYTES);
    CHECK(memcmp(bootstrap_workspace.frame.payload, body,
                 UCN_I_ADMISSION_HELLO_BYTES) == 0);
    CHECK(ucn_i_composition_control_tx_finish(
              fixture.runtime, &tx_plan, &finish_workspace) == UCN_OK);

    memset(&challenge, 0, sizeof(challenge));
    challenge.transaction_id = UINT64_C(3002);
    challenge.cookie_time_bucket = 9U;
    challenge.issuer_link.id = 1U;
    challenge.issuer_link.generation = 1U;
    challenge.cookie_bytes = 4U;
    challenge.cookie[0] = 0xC1U;
    challenge.cookie[1] = 0xC2U;
    challenge.cookie[2] = 0xC3U;
    challenge.cookie[3] = 0xC4U;
    CHECK(ucn_i_admission_cookie_challenge_encode(
              &challenge, body) == UCN_OK);
    packet_bytes = bootstrap_packet_build(
        UCN_I_OPCODE_BOOTSTRAP_COOKIE_CHALLENGE,
        UCN_INTERACTION_RESULT, challenge.transaction_id, body,
        UCN_I_ADMISSION_COOKIE_CHALLENGE_BYTES,
        packet, sizeof(packet));
    publish_packet(&fixture, packet, packet_bytes);
    CHECK(ucn_i_composition_bootstrap_rx_claim_prepare(
              fixture.runtime, &bootstrap_workspace,
              &bootstrap_plan) == UCN_OK);
    CHECK(memcmp(&bootstrap_plan.message.cookie_challenge, &challenge,
                 sizeof(challenge)) == 0);
    CHECK(ucn_i_composition_bootstrap_rx_finish(
              fixture.runtime, &bootstrap_plan, &finish_workspace,
              true) == UCN_OK);

    memset(&tx_message, 0, sizeof(tx_message));
    tx_message.cookie_challenge = challenge;
    CHECK(ucn_i_composition_bootstrap_tx_submit(
              fixture.runtime, 0U,
              UCN_I_OPCODE_BOOTSTRAP_COOKIE_CHALLENGE,
              &tx_message, &tx_workspace, &tx_plan) == UCN_OK);
    CHECK(ucn_i_composition_control_tx_finish(
              fixture.runtime, &tx_plan, &finish_workspace) == UCN_OK);

    memset(&hello_cookie, 0, sizeof(hello_cookie));
    hello_cookie.key.link.id = 1U;
    hello_cookie.key.link.generation = 1U;
    hello_cookie.key.local_peer_discriminator = 1U;
    hello_cookie.key.transaction_id = UINT64_C(3003);
    memset(hello_cookie.key.identity_digest, 0x31,
           sizeof(hello_cookie.key.identity_digest));
    hello_cookie.device_nonce = hello.device_nonce;
    hello_cookie.lease_freshness_challenge_nonce = UINT64_C(77);
    memset(hello_cookie.prior_messages_hash, 0x44,
           sizeof(hello_cookie.prior_messages_hash));
    hello_cookie.cookie_bytes = 4U;
    memcpy(hello_cookie.cookie, challenge.cookie, 4U);
    CHECK(ucn_i_admission_hello_cookie_encode(
              &hello_cookie, body, sizeof(body), &body_bytes) == UCN_OK);
    packet_bytes = bootstrap_packet_build(
        UCN_I_OPCODE_BOOTSTRAP_HELLO_COOKIE,
        UCN_INTERACTION_REQUEST, hello_cookie.key.transaction_id,
        body, body_bytes, packet, sizeof(packet));
    publish_packet(&fixture, packet, packet_bytes);
    CHECK(ucn_i_composition_bootstrap_rx_claim_prepare(
              fixture.runtime, &bootstrap_workspace,
              &bootstrap_plan) == UCN_OK);
    CHECK(memcmp(&bootstrap_plan.message.hello_cookie, &hello_cookie,
                 sizeof(hello_cookie)) == 0);
    CHECK(ucn_i_composition_bootstrap_rx_finish(
              fixture.runtime, &bootstrap_plan, &finish_workspace,
              true) == UCN_OK);

    memset(&tx_message, 0, sizeof(tx_message));
    tx_message.hello_cookie = hello_cookie;
    fixture.driver_submit_result = UCN_DRIVER_NOT_SUBMITTED;
    CHECK(ucn_i_composition_bootstrap_tx_submit(
              fixture.runtime, 0U,
              UCN_I_OPCODE_BOOTSTRAP_HELLO_COOKIE,
              &tx_message, &tx_workspace, &tx_plan) == UCN_OK);
    fixture.driver_submit_result = UCN_DRIVER_COMPLETE;
    CHECK(ucn_i_composition_control_tx_retry(
              fixture.runtime, &tx_plan, &retry_workspace,
              &retry_plan) == UCN_OK);
    CHECK(retry_plan.frame == tx_plan.frame);
    CHECK(retry_plan.frame_bytes == tx_plan.frame_bytes);
    CHECK(ucn_i_composition_control_tx_finish(
              fixture.runtime, &retry_plan, &finish_workspace) == UCN_OK);

    /* Client TX Link=1/1 must not overwrite or reject issuer-local scope=7/9.
     * 客户端本地1/1发送时必须保留签发方7/9；接收端仍必须匹配其自身真实域。 */
    tx_message.hello_cookie.key.link.id=7U;
    tx_message.hello_cookie.key.link.generation=9U;
    CHECK(ucn_i_composition_bootstrap_tx_submit(fixture.runtime,0U,
              UCN_I_OPCODE_BOOTSTRAP_HELLO_COOKIE,&tx_message,&tx_workspace,&tx_plan)==UCN_OK);
    CHECK(ucn_i_c0_decode(tx_plan.frame,tx_plan.frame_bytes,1U,UCN_I_CONTROL_H0,&bootstrap_workspace.frame)==UCN_OK);
    CHECK(ucn_i_admission_hello_cookie_decode(bootstrap_workspace.frame.payload,
              bootstrap_workspace.frame.payload_bytes,1U,&hello_cookie)==UCN_OK);
    CHECK(hello_cookie.key.link.id==7U && hello_cookie.key.link.generation==9U);
    memcpy(packet,tx_plan.frame,tx_plan.frame_bytes);packet_bytes=tx_plan.frame_bytes;
    CHECK(ucn_i_composition_control_tx_finish(fixture.runtime,&tx_plan,&finish_workspace)==UCN_OK);
    publish_packet(&fixture,packet,packet_bytes);
    CHECK(ucn_i_composition_bootstrap_rx_claim_prepare(fixture.runtime,&bootstrap_workspace,&bootstrap_plan)==UCN_ERR_MALFORMED);

    CHECK(ucn_i_admission_hello_encode(&hello, body) == UCN_OK);
    packet_bytes = bootstrap_packet_build(
        UCN_I_OPCODE_BOOTSTRAP_HELLO, UCN_INTERACTION_REQUEST,
        hello.transaction_id + 1U, body, UCN_I_ADMISSION_HELLO_BYTES,
        packet, sizeof(packet));
    publish_packet(&fixture, packet, packet_bytes);
    memset(&bootstrap_plan, 0xA5, sizeof(bootstrap_plan));
    bootstrap_before = bootstrap_plan;
    CHECK(ucn_i_composition_bootstrap_rx_claim_prepare(
              fixture.runtime, &bootstrap_workspace,
              &bootstrap_plan) == UCN_ERR_MALFORMED);
    CHECK(memcmp(&bootstrap_plan, &bootstrap_before,
                 sizeof(bootstrap_plan)) == 0);
    CHECK(ucn_i_composition_bootstrap_rx_claim_prepare(
              fixture.runtime, &bootstrap_workspace,
              &bootstrap_plan) == UCN_ERR_NOT_FOUND);

    packet_bytes = bootstrap_packet_build(
        UCN_I_OPCODE_ADDRESS_OFFER, UCN_INTERACTION_RESULT,
        UINT64_C(3004), NULL, 0U, packet, sizeof(packet));
    publish_packet(&fixture, packet, packet_bytes);
    CHECK(ucn_i_composition_bootstrap_rx_claim_prepare(
              fixture.runtime, &bootstrap_workspace,
              &bootstrap_plan) == UCN_ERR_UNSUPPORTED);
    CHECK(ucn_i_composition_bootstrap_rx_claim_prepare(
              fixture.runtime, &bootstrap_workspace,
              &bootstrap_plan) == UCN_ERR_NOT_FOUND);

    CHECK(ucn_i_admission_hello_encode(&hello, body) == UCN_OK);
    packet_bytes = bootstrap_packet_build(
        UCN_I_OPCODE_BOOTSTRAP_HELLO, UCN_INTERACTION_REQUEST,
        hello.transaction_id, body, UCN_I_ADMISSION_HELLO_BYTES,
        packet, sizeof(packet));
    publish_packet(&fixture, packet, packet_bytes);
    CHECK(ucn_i_composition_bootstrap_rx_claim_prepare(
              fixture.runtime, &bootstrap_workspace,
              &bootstrap_plan) == UCN_OK);
    bootstrap_plan.message.hello.device_nonce ^= UINT64_C(1);
    CHECK(ucn_i_composition_bootstrap_rx_finish(
              fixture.runtime, &bootstrap_plan, &finish_workspace,
              false) == UCN_ERR_SECURITY);
}

static void test_authenticated_control_dispatch_and_feature_gate(void)
{
    ucn_i_security_control_access_request_t access;
    ucn_i_security_c0_protect_request_t protect;
    ucn_i_security_packet_workspace_t protect_workspace;
    ucn_i_composition_control_rx_workspace_t rx_workspace;
    ucn_i_composition_control_rx_plan_t rx_plan;
    ucn_i_composition_control_rx_plan_t forged_rx_plan;
    ucn_i_composition_control_tx_workspace_t tx_workspace;
    ucn_i_composition_control_tx_workspace_t retry_workspace;
    ucn_i_composition_control_tx_plan_t tx_plan;
    ucn_i_composition_control_tx_plan_t retry_plan;
    ucn_i_composition_control_tx_plan_t tx_plan_before;
    ucn_i_composition_diagnostic_control_workspace_t diagnostic_workspace;
    ucn_i_composition_diagnostic_control_result_t diagnostic_result;
    ucn_i_diagnostic_echo_t diagnostic_echo;
    ucn_i_sha256_workspace_t tx_finish_workspace;
    ucn_i_composition_control_dispatch_plan_t plan;
    ucn_i_composition_control_dispatch_plan_t forged;
    ucn_i_composition_control_dispatch_plan_t plan_before;
    ucn_i_security_control_validate_workspace_t finish_workspace;
    ucn_i_c0_frame_t protected_frame;
    ucn_i_composition_owner_record_t *coordinator;
    ucn_link_handle_t link;
    ucn_rx_meta_t rx_meta;
    uint8_t packet[UCN_ADAPTER_FRAME_BYTES];
    uint8_t body[UCN_I_DIAGNOSTIC_ECHO_BYTES];
    uint8_t invalid_body[UCN_I_DIAGNOSTIC_ECHO_BYTES] = {0};
    uint8_t opened[sizeof(body)];
    uint8_t opened_before[sizeof(opened)];
    size_t packet_bytes;
    size_t opened_bytes;
    uint32_t protect_calls;

    fixture_init(&fixture);
    diagnostic_echo.nonce = UINT64_C(0x1020304050607080);
    CHECK(ucn_i_diagnostic_echo_encode(&diagnostic_echo, body) == UCN_OK);
    fixture.security_owner->sessions[0].candidate.address_width = 1U;
    fixture.security_owner->sessions[0].candidate.hop_profile =
        UCN_I_HOP_PROFILE_H0;
    memset(&access, 0, sizeof(access));
    access.session = fixture.request.security.session;
    memset(access.context_fingerprint, 0x55,
           sizeof(access.context_fingerprint));
    access.protocol_opcode = UCN_I_OPCODE_DIAGNOSTIC_PING;
    access.direction = UCN_I_SECURITY_ACCESS_INBOUND;
    packet_bytes = control_packet_build(
        &fixture, access.protocol_opcode, UINT64_C(1001), body,
        sizeof(body), packet, sizeof(packet));
    opened_bytes = SIZE_MAX;
    memset(&plan, 0xA5, sizeof(plan));
    CHECK(ucn_i_composition_control_receive_prepare(
              fixture.runtime, &access, &fixture.request.security_facts,
              packet, packet_bytes, &control_workspace,
              opened, sizeof(opened), &opened_bytes, &plan) == UCN_OK);
    CHECK(opened_bytes == sizeof(body));
    CHECK(memcmp(opened, body, sizeof(body)) == 0);
    CHECK(plan.authenticated.body == opened);
    CHECK(plan.authenticated.traffic_class == UCN_TRAFFIC_Q0);
    CHECK(plan.authenticated.delivery_guarantee ==
          UCN_DELIVERY_BEST_EFFORT);
    CHECK(plan.authenticated.interaction_role == UCN_INTERACTION_REQUEST);
    CHECK(plan.authenticated.payload_kind == UCN_I_PAYLOAD_DIAGNOSTIC);
    CHECK(plan.authenticated.hop_limit == 1U);
    CHECK(plan.target_module == UCN_I_COMPOSITION_MODULE_COORDINATOR);
    coordinator = record_for(
        fixture.runtime, UCN_I_COMPOSITION_MODULE_COORDINATOR);
    CHECK(coordinator != NULL);
    CHECK(memcmp(&plan.target, &coordinator->ref, sizeof(plan.target)) == 0);

    forged = plan;
    forged.authenticated.interaction_role = UCN_INTERACTION_RESULT;
    CHECK(ucn_i_security_control_fact_validate(
              fixture.security_owner, &forged.authenticated,
              &fixture.request.security_facts, &finish_workspace) ==
          UCN_ERR_STATE);

    forged = plan;
    forged.target.generation++;
    CHECK(ucn_i_composition_control_dispatch_finish(
              fixture.runtime, &forged, &fixture.request.security_facts,
              &finish_workspace, false) == UCN_ERR_STATE);
    CHECK(ucn_i_composition_control_dispatch_finish(
              fixture.runtime, &plan, &fixture.request.security_facts,
              &finish_workspace, false) == UCN_OK);

    CHECK(ucn_i_composition_control_receive_prepare(
              fixture.runtime, &access, &fixture.request.security_facts,
              packet, packet_bytes, &control_workspace,
              opened, sizeof(opened), &opened_bytes, &plan) == UCN_OK);
    CHECK(ucn_i_composition_control_dispatch_finish(
              fixture.runtime, &plan, &fixture.request.security_facts,
              &finish_workspace, true) == UCN_ERR_STATE);

    CHECK(ucn_i_composition_control_receive_prepare(
              fixture.runtime, &access, &fixture.request.security_facts,
              packet, packet_bytes, &control_workspace,
              opened, sizeof(opened), &opened_bytes, &plan) == UCN_OK);
    opened[0] ^= 1U;
    CHECK(ucn_i_composition_control_dispatch_finish(
              fixture.runtime, &plan, &fixture.request.security_facts,
              &finish_workspace, true) == UCN_ERR_SECURITY);
    opened[0] ^= 1U;

    CHECK(ucn_i_composition_control_receive_prepare(
              fixture.runtime, &access, &fixture.request.security_facts,
              packet, packet_bytes, &control_workspace,
              opened, sizeof(opened), &opened_bytes, &plan) == UCN_OK);
    CHECK(ucn_i_composition_diagnostic_control_handle(
              fixture.runtime, &plan, &fixture.request.security_facts,
              &diagnostic_workspace, &diagnostic_result) == UCN_OK);
    CHECK(diagnostic_result.nonce == diagnostic_echo.nonce);
    CHECK(diagnostic_result.response_opcode == UCN_I_OPCODE_DIAGNOSTIC_PONG);
    CHECK(ucn_i_composition_control_dispatch_finish(
              fixture.runtime, &plan, &fixture.request.security_facts,
              &finish_workspace, true) == UCN_OK);
    memcpy(opened_before, opened, sizeof(opened));
    plan_before = plan;
    opened_bytes = SIZE_MAX;
    CHECK(ucn_i_composition_control_receive_prepare(
              fixture.runtime, &access, &fixture.request.security_facts,
              packet, packet_bytes, &control_workspace,
              opened, sizeof(opened), &opened_bytes, &plan) ==
          UCN_ERR_REPLAY);
    CHECK(memcmp(opened, opened_before, sizeof(opened)) == 0);
    CHECK(opened_bytes == SIZE_MAX);
    CHECK(memcmp(&plan, &plan_before, sizeof(plan)) == 0);

    fixture.security_acl[1].protocol_opcode = UCN_I_OPCODE_TIME_SYNC;
    access.protocol_opcode = UCN_I_OPCODE_TIME_SYNC;
    packet_bytes = control_packet_build(
        &fixture, access.protocol_opcode, UINT64_C(1002), body,
        sizeof(body), packet, sizeof(packet));
    memset(opened, 0x5A, sizeof(opened));
    memcpy(opened_before, opened, sizeof(opened));
    memset(&plan, 0x3C, sizeof(plan));
    plan_before = plan;
    opened_bytes = SIZE_MAX;
    CHECK(ucn_i_composition_control_receive_prepare(
              fixture.runtime, &access, &fixture.request.security_facts,
              packet, packet_bytes, &control_workspace,
              opened, sizeof(opened), &opened_bytes, &plan) ==
          UCN_ERR_UNSUPPORTED);
    CHECK(memcmp(opened, opened_before, sizeof(opened)) == 0);
    CHECK(opened_bytes == SIZE_MAX);
    CHECK(memcmp(&plan, &plan_before, sizeof(plan)) == 0);
    CHECK(ucn_i_composition_control_receive_prepare(
              fixture.runtime, &access, &fixture.request.security_facts,
              packet, packet_bytes, &control_workspace,
              opened, sizeof(opened), &opened_bytes, &plan) ==
          UCN_ERR_UNSUPPORTED);

    fixture.security_acl[0].service_id = 0U;
    memset(&protect, 0, sizeof(protect));
    protect.session = fixture.request.security.session;
    protect.facts = fixture.request.security_facts;
    protect.transaction_id = UINT64_C(2001);
    protect.protocol_opcode = UCN_I_OPCODE_OPERATION_QUERY;
    memset(protect.context_fingerprint, 0x55,
           sizeof(protect.context_fingerprint));
    protect.traffic_class = UCN_TRAFFIC_Q0;
    protect.delivery_guarantee = UCN_DELIVERY_RELIABLE;
    protect.interaction_role = UCN_INTERACTION_REQUEST;
    protect.payload_kind = UCN_I_PAYLOAD_CONTROL;
    protect.hop_limit = 1U;
    protect.direction = UCN_I_SECURITY_ACCESS_OUTBOUND;
    packet_bytes = SIZE_MAX;
    memset(packet, 0xA5, sizeof(packet));
    protect_calls = fixture.security_protect_calls;
    CHECK(ucn_i_composition_control_protect(
              fixture.runtime, &protect, body, sizeof(body),
              &protect_workspace, packet, sizeof(packet),
              &packet_bytes) == UCN_ERR_ARGUMENT);
    CHECK(packet[0] == 0xA5U);
    CHECK(packet_bytes == SIZE_MAX);
    CHECK(fixture.security_protect_calls == protect_calls);

    fixture.security_acl[0].protocol_opcode =
        UCN_I_OPCODE_DIAGNOSTIC_PING;
    protect.protocol_opcode = UCN_I_OPCODE_DIAGNOSTIC_PING;
    protect.delivery_guarantee = UCN_DELIVERY_BEST_EFFORT;
    protect.payload_kind = UCN_I_PAYLOAD_DIAGNOSTIC;
    CHECK(ucn_i_composition_control_protect(
              fixture.runtime, &protect, body, sizeof(body),
              &protect_workspace, packet, sizeof(packet),
              &packet_bytes) == UCN_OK);
    CHECK(packet_bytes > sizeof(body));
    CHECK(packet[0] == UINT8_C(0x60));
    CHECK(ucn_i_c0_decode(packet, packet_bytes, 1U, UCN_I_CONTROL_H0,
                           &protected_frame) == UCN_OK);
    CHECK(protected_frame.source_address == 1U);
    CHECK(protected_frame.destination_address == 2U);
    CHECK(protected_frame.source_binding_generation == 10U);
    CHECK(protected_frame.destination_binding_generation == 20U);
    CHECK(protected_frame.opcode == UCN_I_OPCODE_DIAGNOSTIC_PING);

    protect.transaction_id = UINT64_C(2002);
    packet_bytes = SIZE_MAX;
    memset(packet, 0xA5, sizeof(packet));
    protect_calls = fixture.security_protect_calls;
    CHECK(ucn_i_composition_control_protect(
              fixture.runtime, &protect, invalid_body,
              sizeof(invalid_body), &protect_workspace, packet,
              sizeof(packet), &packet_bytes) == UCN_ERR_MALFORMED);
    CHECK(packet[0] == 0xA5U);
    CHECK(packet_bytes == SIZE_MAX);
    CHECK(fixture.security_protect_calls == protect_calls);

    fixture.security_acl[0].protocol_opcode = UCN_I_OPCODE_TIME_SYNC;
    protect.protocol_opcode = UCN_I_OPCODE_TIME_SYNC;
    packet_bytes = SIZE_MAX;
    memset(packet, 0xA5, sizeof(packet));
    CHECK(ucn_i_composition_control_protect(
              fixture.runtime, &protect, body, sizeof(body),
              &protect_workspace, packet, sizeof(packet),
              &packet_bytes) == UCN_ERR_UNSUPPORTED);
    CHECK(packet[0] == 0xA5U);
    CHECK(packet_bytes == SIZE_MAX);

    protect.protocol_opcode = UCN_I_OPCODE_ROUTE_FRAGMENT;
    protect.payload_kind = UCN_I_PAYLOAD_CONTROL;
    protect.interaction_role = UCN_INTERACTION_ONE_WAY;
    protect_calls = fixture.security_protect_calls;
    CHECK(ucn_i_composition_control_protect(
              fixture.runtime, &protect, body, sizeof(body),
              &protect_workspace, packet, sizeof(packet),
              &packet_bytes) == UCN_ERR_UNSUPPORTED);
    CHECK(packet[0] == 0xA5U && packet_bytes == SIZE_MAX);
    CHECK(fixture.security_protect_calls == protect_calls);

    fixture.security_acl[1].protocol_opcode = UCN_I_OPCODE_DIAGNOSTIC_PING;
    fixture.security_owner->sessions[0].candidate.link_generation = 1U;
    packet_bytes = control_packet_build(
        &fixture, UCN_I_OPCODE_DIAGNOSTIC_PING, UINT64_C(1010), body,
        sizeof(body), packet, sizeof(packet));
    memset(&rx_meta, 0, sizeof(rx_meta));
    rx_meta.struct_size = (uint16_t)sizeof(rx_meta);
    rx_meta.api_version = UCN_API_VERSION;
    rx_meta.timestamp_us = 100U;
    CHECK(ucn_i_composition_link_get(fixture.runtime, 0U, &link) == UCN_OK);
    CHECK(ucn_i_composition_driver_rx_publish(
              fixture.runtime, link, packet, packet_bytes,
              &rx_meta) == UCN_OK);
    opened_bytes = SIZE_MAX;
    CHECK(ucn_i_composition_control_rx_claim_prepare(
              fixture.runtime, 100U, &rx_workspace, opened,
              sizeof(opened), &opened_bytes, &rx_plan) == UCN_OK);
    CHECK(opened_bytes == sizeof(body));
    CHECK(memcmp(opened, body, sizeof(body)) == 0);
    CHECK(rx_plan.dispatch.target_module ==
          UCN_I_COMPOSITION_MODULE_COORDINATOR);
    CHECK(rx_plan.link_instance_generation == 1U);
    CHECK(rx_plan.has_hop_replay == 0U);
    forged_rx_plan = rx_plan;
    forged_rx_plan.hop_facts.link_generation = 1U;
    CHECK(ucn_i_composition_control_rx_finish(
              fixture.runtime, &forged_rx_plan, &finish_workspace,
              false) == UCN_ERR_ARGUMENT);
    CHECK(ucn_i_composition_diagnostic_control_handle(
              fixture.runtime, &rx_plan.dispatch, &rx_plan.facts,
              &diagnostic_workspace, &diagnostic_result) == UCN_OK);
    CHECK(ucn_i_composition_control_rx_finish(
              fixture.runtime, &rx_plan, &finish_workspace,
              true) == UCN_OK);

    CHECK(ucn_i_composition_driver_rx_publish(
              fixture.runtime, link, packet, packet_bytes,
              &rx_meta) == UCN_OK);
    opened_bytes = SIZE_MAX;
    CHECK(ucn_i_composition_control_rx_claim_prepare(
              fixture.runtime, 101U, &rx_workspace, opened,
              sizeof(opened), &opened_bytes, &rx_plan) == UCN_ERR_REPLAY);
    CHECK(ucn_i_composition_control_rx_claim_prepare(
              fixture.runtime, 101U, &rx_workspace, opened,
              sizeof(opened), &opened_bytes, &rx_plan) ==
          UCN_ERR_NOT_FOUND);

    fixture.security_acl[1].protocol_opcode = UCN_I_OPCODE_TIME_SYNC;
    packet_bytes = control_packet_build(
        &fixture, UCN_I_OPCODE_TIME_SYNC, UINT64_C(1011), body,
        sizeof(body), packet, sizeof(packet));
    CHECK(ucn_i_composition_driver_rx_publish(
              fixture.runtime, link, packet, packet_bytes,
              &rx_meta) == UCN_OK);
    memset(opened, 0x3C, sizeof(opened));
    memcpy(opened_before, opened, sizeof(opened));
    memset(&rx_plan, 0xA5, sizeof(rx_plan));
    opened_bytes = SIZE_MAX;
    CHECK(ucn_i_composition_control_rx_claim_prepare(
              fixture.runtime, 102U, &rx_workspace, opened,
              sizeof(opened), &opened_bytes, &rx_plan) ==
          UCN_ERR_UNSUPPORTED);
    CHECK(memcmp(opened, opened_before, sizeof(opened)) == 0);
    CHECK(opened_bytes == SIZE_MAX);
    CHECK(ucn_i_composition_control_rx_claim_prepare(
              fixture.runtime, 102U, &rx_workspace, opened,
              sizeof(opened), &opened_bytes, &rx_plan) ==
          UCN_ERR_NOT_FOUND);

    fixture.security_acl[0].protocol_opcode = UCN_I_OPCODE_DIAGNOSTIC_PING;
    protect.protocol_opcode = UCN_I_OPCODE_DIAGNOSTIC_PING;
    protect.delivery_guarantee = UCN_DELIVERY_BEST_EFFORT;
    protect.interaction_role = UCN_INTERACTION_REQUEST;
    protect.payload_kind = UCN_I_PAYLOAD_DIAGNOSTIC;
    protect.facts.link_generation = 1U;
    protect.transaction_id = UINT64_C(2010);
    CHECK(ucn_i_composition_control_tx_submit(
              fixture.runtime, 0U, &protect, body, sizeof(body),
              &tx_workspace, &tx_plan) == UCN_OK);
    CHECK(tx_plan.frame == tx_workspace.frame);
    CHECK(tx_plan.frame_bytes > sizeof(body));
    CHECK(ucn_i_c0_decode(tx_plan.frame, tx_plan.frame_bytes, 1U,
                           UCN_I_CONTROL_H0, &protected_frame) == UCN_OK);
    CHECK(protected_frame.transaction_id == UINT64_C(2010));
    CHECK(protected_frame.opcode == UCN_I_OPCODE_DIAGNOSTIC_PING);
    CHECK(ucn_i_composition_control_tx_finish(
              fixture.runtime, &tx_plan, &tx_finish_workspace) == UCN_OK);
    CHECK(ucn_i_composition_control_tx_finish(
              fixture.runtime, &tx_plan, &tx_finish_workspace) ==
          UCN_ERR_NOT_FOUND);

    fixture.driver_submit_result = UCN_DRIVER_NOT_SUBMITTED;
    protect.transaction_id = UINT64_C(2011);
    CHECK(ucn_i_composition_control_tx_submit(
              fixture.runtime, 0U, &protect, body, sizeof(body),
              &tx_workspace, &tx_plan) == UCN_OK);
    protect_calls = fixture.security_protect_calls;
    fixture.driver_submit_result = UCN_DRIVER_COMPLETE;
    CHECK(ucn_i_composition_control_tx_retry(
              fixture.runtime, &tx_plan, &retry_workspace,
              &retry_plan) == UCN_OK);
    CHECK(retry_plan.frame == tx_plan.frame);
    CHECK(retry_plan.frame_bytes == tx_plan.frame_bytes);
    CHECK(fixture.security_protect_calls == protect_calls);
    CHECK(ucn_i_composition_control_tx_submit(
              fixture.runtime, 0U, &protect, body, sizeof(body),
              &retry_workspace, &tx_plan_before) == UCN_ERR_REPLAY);
    CHECK(fixture.security_protect_calls == protect_calls);
    CHECK(ucn_i_composition_control_tx_finish(
              fixture.runtime, &retry_plan, &tx_finish_workspace) == UCN_OK);

    fixture.driver_submit_result = UCN_DRIVER_SUBMITTED;
    protect.transaction_id = UINT64_C(2012);
    CHECK(ucn_i_composition_control_tx_submit(
              fixture.runtime, 0U, &protect, body, sizeof(body),
              &tx_workspace, &tx_plan) == UCN_OK);
    CHECK(ucn_i_composition_control_tx_finish(
              fixture.runtime, &tx_plan, &tx_finish_workspace) ==
          UCN_ERR_STATE);
    CHECK(ucn_i_composition_driver_tx_complete(
              fixture.runtime, tx_plan.tx_token, UCN_OK, NULL) == UCN_OK);
    CHECK(ucn_i_composition_control_tx_finish(
              fixture.runtime, &tx_plan, &tx_finish_workspace) == UCN_OK);
    fixture.driver_submit_result = UCN_DRIVER_COMPLETE;

    fixture.security_acl[0].protocol_opcode = UCN_I_OPCODE_TIME_SYNC;
    protect.protocol_opcode = UCN_I_OPCODE_TIME_SYNC;
    memset(&tx_plan, 0xA5, sizeof(tx_plan));
    tx_plan_before = tx_plan;
    CHECK(ucn_i_composition_control_tx_submit(
              fixture.runtime, 0U, &protect, body, sizeof(body),
              &tx_workspace, &tx_plan) == UCN_ERR_UNSUPPORTED);
    CHECK(memcmp(&tx_plan, &tx_plan_before, sizeof(tx_plan)) == 0);
}

static ucn_i_route_link_ref_t direct_route_ingress(void)
{
    ucn_i_route_link_ref_t ingress;

    memset(&ingress, 0, sizeof(ingress));
    ingress.peer = route_binding(2U, 20U, 0x22U);
    ingress.link_generation = 30U;
    ingress.cost = 3U;
    ingress.link_id = 2U;
    ingress.frame_mtu = 256U;
    ingress.capability_bits = 3U;
    return ingress;
}

static ucn_i_capability_record_t direct_capability_record(
    uint32_t generation,
    uint32_t link_generation)
{
    ucn_i_capability_record_t record;

    memset(&record, 0, sizeof(record));
    record.capability_generation = generation;
    record.link.link_instance_generation = link_generation;
    record.link.carrier_mtu = 256U;
    record.link.link_frame_mtu = 240U;
    record.link.processing_frame_mtu = 224U;
    record.link.carrier_header_bytes = 14U;
    record.link.carrier_padding_bytes = 2U;
    record.link.carrier_crc_bytes = 4U;
    record.link.carrier_tag_bytes = 12U;
    record.link.carrier_max_fragments = 8U;
    record.link.link_flags = UINT16_C(0x000C);
    record.link.nominal_rate_bps = 1000000U;
    record.link.hardware_priority_count = 4U;
    record.peer.feature_bits = UCN_I_CAPABILITY_REQUIRED_BASE_FEATURES;
    record.peer.hop_suite_bits = UCN_I_CAPABILITY_KNOWN_HOP_SUITES;
    record.peer.e2e_suite_bits = UCN_I_CAPABILITY_KNOWN_E2E_SUITES;
    record.peer.max_rx_window = 16U;
    record.peer.max_concurrent_transfers = 2U;
    record.peer.max_message_class = UCN_I_MESSAGE_T512;
    return record;
}

static void capability_packet_prepare(
    uint16_t opcode,
    uint8_t delivery,
    uint8_t role,
    uint64_t transaction_id,
    const uint8_t *body,
    size_t body_bytes,
    uint64_t now_us,
    ucn_i_composition_control_rx_workspace_t *rx_workspace,
    uint8_t *opened,
    size_t opened_capacity,
    size_t *opened_bytes,
    ucn_i_composition_control_rx_plan_t *rx_plan)
{
    uint8_t packet[UCN_ADAPTER_FRAME_BYTES];
    size_t packet_bytes;

    fixture.security_acl[1].protocol_opcode = opcode;
    packet_bytes = control_packet_build_profile(
        &fixture, opcode, UCN_TRAFFIC_Q2, delivery, role,
        transaction_id, body, body_bytes, packet, sizeof(packet));
    publish_packet(&fixture, packet, packet_bytes);
    *opened_bytes = SIZE_MAX;
    CHECK(ucn_i_composition_control_rx_claim_prepare(
              fixture.runtime, now_us, rx_workspace, opened,
              opened_capacity, opened_bytes, rx_plan) == UCN_OK);
    CHECK(*opened_bytes == body_bytes);
}

static void test_authenticated_capability_advertise_typed_handler(void)
{
    ucn_i_composition_control_rx_workspace_t rx_workspace;
    ucn_i_composition_control_rx_plan_t rx_plan;
    ucn_i_composition_capability_control_workspace_t capability_workspace;
    ucn_i_composition_capability_control_result_t capability_result;
    ucn_i_composition_capability_control_result_t result_before;
    ucn_i_security_control_validate_workspace_t finish_workspace;
    ucn_i_capability_ref_t good_reference;
    ucn_i_capability_record_t record;
    ucn_i_cached_capability_t cached;
    ucn_link_handle_t link;
    ucn_rx_meta_t rx_meta;
    uint8_t body[UCN_I_CAPABILITY_RECORD_BYTES];
    uint8_t opened[UCN_I_CAPABILITY_RECORD_BYTES];
    uint8_t packet[UCN_ADAPTER_FRAME_BYTES];
    size_t opened_bytes;
    size_t packet_bytes;

    fixture_init(&fixture);
    fixture.security_owner->sessions[0].candidate.address_width = 1U;
    fixture.security_owner->sessions[0].candidate.hop_profile =
        UCN_I_HOP_PROFILE_H0;
    fixture.security_owner->sessions[0].candidate.link_generation = 1U;
    fixture.request.security_facts.link_generation = 1U;
    memset(fixture.capability_owner->peers, 0,
           sizeof(fixture.capability_owner->peers));
    fixture.security_acl[1].protocol_opcode =
        UCN_I_OPCODE_CAPABILITY_ADVERTISE;
    record = direct_capability_record(1U, 1U);
    CHECK(ucn_i_capability_record_encode(&record, body) == UCN_OK);
    packet_bytes = control_packet_build_profile(
        &fixture, UCN_I_OPCODE_CAPABILITY_ADVERTISE, UCN_TRAFFIC_Q2,
        UCN_DELIVERY_BEST_EFFORT, UCN_INTERACTION_ONE_WAY,
        UINT64_C(4001), body, sizeof(body), packet, sizeof(packet));
    publish_packet(&fixture, packet, packet_bytes);
    opened_bytes = SIZE_MAX;
    CHECK(ucn_i_composition_control_rx_claim_prepare(
              fixture.runtime, 100U, &rx_workspace, opened,
              sizeof(opened), &opened_bytes, &rx_plan) == UCN_OK);
    CHECK(opened_bytes == sizeof(body));
    CHECK(rx_plan.dispatch.target_module ==
          UCN_I_COMPOSITION_MODULE_CAPABILITY);
    memset(&capability_result, 0, sizeof(capability_result));
    CHECK(ucn_i_composition_capability_control_handle(
              fixture.runtime, &rx_plan, 100U, &capability_workspace,
              &capability_result) == UCN_OK);
    CHECK(capability_result.kind ==
          UCN_I_COMPOSITION_CAPABILITY_CONTROL_ADVERTISE);
    CHECK(capability_result.reference.address == 2U);
    CHECK(capability_result.reference.binding_generation == 20U);
    CHECK(capability_result.reference.session_generation == 40U);
    CHECK(capability_result.reference.link_id == 1U);
    CHECK(capability_result.reference.link_generation == 1U);
    good_reference = capability_result.reference;
    CHECK(ucn_i_capability_get(
              fixture.capability_owner, &capability_result.reference,
              101U, &cached) == UCN_OK);
    CHECK(cached.record.capability_generation == 1U);
    CHECK(memcmp(&cached.record, &record, sizeof(record)) == 0);
    CHECK(ucn_i_composition_control_rx_finish(
              fixture.runtime, &rx_plan, &finish_workspace, true) == UCN_OK);

    packet_bytes = control_packet_build_profile(
        &fixture, UCN_I_OPCODE_CAPABILITY_ADVERTISE, UCN_TRAFFIC_Q2,
        UCN_DELIVERY_BEST_EFFORT, UCN_INTERACTION_REQUEST,
        UINT64_C(4002), body, sizeof(body), packet, sizeof(packet));
    publish_packet(&fixture, packet, packet_bytes);
    opened_bytes = SIZE_MAX;
    CHECK(ucn_i_composition_control_rx_claim_prepare(
              fixture.runtime, 102U, &rx_workspace, opened,
              sizeof(opened), &opened_bytes, &rx_plan) == UCN_OK);
    memset(&capability_result, 0xA5, sizeof(capability_result));
    result_before = capability_result;
    CHECK(ucn_i_composition_capability_control_handle(
              fixture.runtime, &rx_plan, 102U, &capability_workspace,
              &capability_result) == UCN_ERR_ARGUMENT);
    CHECK(memcmp(&capability_result, &result_before,
                 sizeof(capability_result)) == 0);
    CHECK(ucn_i_capability_get(
              fixture.capability_owner, &good_reference,
              102U, &cached) == UCN_OK);
    CHECK(cached.record.capability_generation == 1U);
    CHECK(ucn_i_composition_control_rx_finish(
              fixture.runtime, &rx_plan, &finish_workspace, false) == UCN_OK);

    record = direct_capability_record(2U, 2U);
    CHECK(ucn_i_capability_record_encode(&record, body) == UCN_OK);
    packet_bytes = control_packet_build_profile(
        &fixture, UCN_I_OPCODE_CAPABILITY_ADVERTISE, UCN_TRAFFIC_Q2,
        UCN_DELIVERY_BEST_EFFORT, UCN_INTERACTION_ONE_WAY,
        UINT64_C(4003), body, sizeof(body), packet, sizeof(packet));
    publish_packet(&fixture, packet, packet_bytes);
    opened_bytes = SIZE_MAX;
    CHECK(ucn_i_composition_control_rx_claim_prepare(
              fixture.runtime, 103U, &rx_workspace, opened,
              sizeof(opened), &opened_bytes, &rx_plan) == UCN_OK);
    memset(&capability_result, 0x5A, sizeof(capability_result));
    result_before = capability_result;
    CHECK(ucn_i_composition_capability_control_handle(
              fixture.runtime, &rx_plan, 103U, &capability_workspace,
              &capability_result) == UCN_ERR_SECURITY);
    CHECK(memcmp(&capability_result, &result_before,
                 sizeof(capability_result)) == 0);
    CHECK(ucn_i_capability_get(
              fixture.capability_owner, &good_reference,
              103U, &cached) == UCN_OK);
    CHECK(cached.record.capability_generation == 1U);
    CHECK(ucn_i_composition_control_rx_finish(
              fixture.runtime, &rx_plan, &finish_workspace, false) == UCN_OK);

    memset(&rx_meta, 0, sizeof(rx_meta));
    rx_meta.struct_size = (uint16_t)sizeof(rx_meta);
    rx_meta.api_version = UCN_API_VERSION;
    rx_meta.timestamp_us = 104U;
    CHECK(ucn_i_composition_link_get(fixture.runtime, 0U, &link) == UCN_OK);
    packet_bytes = control_packet_build_profile(
        &fixture, UCN_I_OPCODE_CAPABILITY_ADVERTISE, UCN_TRAFFIC_Q2,
        UCN_DELIVERY_BEST_EFFORT, UCN_INTERACTION_ONE_WAY,
        UINT64_C(4004), body, sizeof(body) - 1U, packet, sizeof(packet));
    CHECK(ucn_i_composition_driver_rx_publish(
              fixture.runtime, link, packet, packet_bytes, &rx_meta) ==
          UCN_OK);
    opened_bytes = SIZE_MAX;
    CHECK(ucn_i_composition_control_rx_claim_prepare(
              fixture.runtime, 104U, &rx_workspace, opened,
              sizeof(opened), &opened_bytes, &rx_plan) == UCN_OK);
    memset(&capability_result, 0x3C, sizeof(capability_result));
    result_before = capability_result;
    CHECK(ucn_i_composition_capability_control_handle(
              fixture.runtime, &rx_plan, 104U, &capability_workspace,
              &capability_result) == UCN_ERR_ARGUMENT);
    CHECK(memcmp(&capability_result, &result_before,
                 sizeof(capability_result)) == 0);
    CHECK(ucn_i_composition_control_rx_finish(
              fixture.runtime, &rx_plan, &finish_workspace, false) == UCN_OK);
}

static void test_authenticated_capability_control_family(void)
{
    ucn_i_composition_control_rx_workspace_t rx_workspace;
    ucn_i_composition_control_rx_plan_t rx_plan;
    ucn_i_composition_capability_control_workspace_t capability_workspace;
    ucn_i_composition_capability_control_result_t capability_result;
    ucn_i_composition_capability_control_context_t context;
    ucn_i_security_control_validate_workspace_t finish_workspace;
    ucn_i_capability_record_t peer_record;
    ucn_i_capability_record_t local_record;
    ucn_i_cached_capability_t cached;
    ucn_i_capability_summary_t summary;
    ucn_i_profile_select_t selected;
    ucn_i_profile_ack_t ack;
    ucn_i_capability_query_t query;
    ucn_i_capability_ref_t cached_reference;
    uint8_t body[UCN_I_PROFILE_ACK_BYTES];
    uint8_t opened[UCN_I_PROFILE_ACK_BYTES];
    uint8_t local_digest[UCN_I_CAPABILITY_DIGEST_BYTES];
    size_t opened_bytes;

    fixture_init(&fixture);
    fixture.security_owner->sessions[0].candidate.address_width = 1U;
    fixture.security_owner->sessions[0].candidate.hop_profile =
        UCN_I_HOP_PROFILE_H0;
    fixture.security_owner->sessions[0].candidate.link_generation = 1U;
    fixture.request.security_facts.link_generation = 1U;
    memset(fixture.capability_owner->peers, 0,
           sizeof(fixture.capability_owner->peers));

    peer_record = direct_capability_record(1U, 1U);
    CHECK(ucn_i_capability_record_encode(&peer_record, body) == UCN_OK);
    capability_packet_prepare(
        UCN_I_OPCODE_CAPABILITY_ADVERTISE, UCN_DELIVERY_BEST_EFFORT,
        UCN_INTERACTION_ONE_WAY, UINT64_C(4101), body,
        UCN_I_CAPABILITY_RECORD_BYTES, 100U, &rx_workspace, opened,
        sizeof(opened), &opened_bytes, &rx_plan);
    CHECK(ucn_i_composition_capability_control_handle(
              fixture.runtime, &rx_plan, 100U, &capability_workspace,
              &capability_result) == UCN_OK);
    CHECK(ucn_i_composition_control_rx_finish(
              fixture.runtime, &rx_plan, &finish_workspace, true) == UCN_OK);
    CHECK(ucn_i_capability_get(
              fixture.capability_owner, &capability_result.reference,
              101U, &cached) == UCN_OK);
    cached_reference = cached.reference;

    memset(&summary, 0, sizeof(summary));
    summary.capability_generation = cached.record.capability_generation;
    summary.link_instance_generation =
        cached.reference.link_generation;
    memcpy(summary.digest, cached.digest, sizeof(summary.digest));
    CHECK(ucn_i_capability_summary_encode(&summary, body) == UCN_OK);
    capability_packet_prepare(
        UCN_I_OPCODE_NEIGHBOR_HELLO, UCN_DELIVERY_BEST_EFFORT,
        UCN_INTERACTION_ONE_WAY, UINT64_C(4102), body,
        UCN_I_CAPABILITY_SUMMARY_BYTES, 102U, &rx_workspace, opened,
        sizeof(opened), &opened_bytes, &rx_plan);
    CHECK(ucn_i_composition_capability_control_handle(
              fixture.runtime, &rx_plan, 102U, &capability_workspace,
              &capability_result) == UCN_OK);
    CHECK(capability_result.kind ==
          UCN_I_COMPOSITION_CAPABILITY_CONTROL_NEIGHBOR_HELLO);
    CHECK(capability_result.summary_result ==
          UCN_I_CAPABILITY_SUMMARY_MATCHED);
    CHECK(ucn_i_composition_control_rx_finish(
              fixture.runtime, &rx_plan, &finish_workspace, true) == UCN_OK);

    summary.digest[0] ^= UINT8_C(0x01);
    CHECK(ucn_i_capability_summary_encode(&summary, body) == UCN_OK);
    capability_packet_prepare(
        UCN_I_OPCODE_NEIGHBOR_KEEPALIVE, UCN_DELIVERY_BEST_EFFORT,
        UCN_INTERACTION_ONE_WAY, UINT64_C(4103), body,
        UCN_I_CAPABILITY_SUMMARY_BYTES, 103U, &rx_workspace, opened,
        sizeof(opened), &opened_bytes, &rx_plan);
    CHECK(ucn_i_composition_capability_control_handle(
              fixture.runtime, &rx_plan, 103U, &capability_workspace,
              &capability_result) == UCN_OK);
    CHECK(capability_result.kind ==
          UCN_I_COMPOSITION_CAPABILITY_CONTROL_NEIGHBOR_KEEPALIVE);
    CHECK(capability_result.summary_result ==
          UCN_I_CAPABILITY_SUMMARY_QUERY_REQUIRED);
    CHECK(ucn_i_composition_control_rx_finish(
              fixture.runtime, &rx_plan, &finish_workspace, true) == UCN_OK);

    local_record = direct_capability_record(9U, 1U);
    CHECK(ucn_i_capability_digest(&local_record, local_digest) == UCN_OK);
    memset(&context, 0, sizeof(context));
    context.struct_size = (uint16_t)sizeof(context);
    context.schema = UCN_I_COMPOSITION_CAPABILITY_CONTROL_SCHEMA;
    context.local_record = local_record;
    context.requirements.required_feature_bits =
        UCN_I_CAPABILITY_REQUIRED_BASE_FEATURES;
    context.requirements.required_hop_suite_bits =
        UCN_I_CAPABILITY_KNOWN_HOP_SUITES;
    context.requirements.required_e2e_suite_bits =
        UCN_I_CAPABILITY_KNOWN_E2E_SUITES;
    context.requirements.minimum_rx_window = 1U;
    context.requirements.minimum_concurrent_transfers = 1U;
    context.requirements.minimum_message_class = UCN_I_MESSAGE_T32;
    context.local_record_valid = 1U;
    context.requirements_valid = 1U;
    CHECK(ucn_i_profile_select_build(
              &local_record, local_digest, &cached.record, cached.digest,
              &context.requirements, &selected) == UCN_OK);
    {
        uint32_t generation=selected.local_generation;
        uint8_t digest[16];
        selected.local_generation=selected.peer_generation;selected.peer_generation=generation;
        memcpy(digest,selected.local_digest,16U);memcpy(selected.local_digest,selected.peer_digest,16U);memcpy(selected.peer_digest,digest,16U);
    }
    CHECK(ucn_i_profile_select_encode(&selected, body) == UCN_OK);
    capability_packet_prepare(
        UCN_I_OPCODE_CAPABILITY_SELECT, UCN_DELIVERY_BEST_EFFORT,
        UCN_INTERACTION_REQUEST, UINT64_C(4104), body,
        UCN_I_PROFILE_SELECT_BYTES, 104U, &rx_workspace, opened,
        sizeof(opened), &opened_bytes, &rx_plan);
    memset(&capability_result, 0xC3, sizeof(capability_result));
    CHECK(ucn_i_composition_capability_control_handle(
              fixture.runtime, &rx_plan, 104U, &capability_workspace,
              &capability_result) == UCN_ERR_ARGUMENT);
    {
        ucn_i_composition_capability_control_result_t sentinel;

        memset(&sentinel, 0xC3, sizeof(sentinel));
        CHECK(memcmp(&capability_result, &sentinel,
                     sizeof(sentinel)) == 0);
    }
    CHECK(ucn_i_composition_capability_control_handle_with_context(
              fixture.runtime, &rx_plan, 104U, &context,
              &capability_workspace, &capability_result) == UCN_OK);
    CHECK(capability_result.kind ==
          UCN_I_COMPOSITION_CAPABILITY_CONTROL_SELECT);
    CHECK(memcmp(&capability_result.selected, &selected,
                 sizeof(selected)) == 0);
    CHECK(ucn_i_composition_control_rx_finish(
              fixture.runtime, &rx_plan, &finish_workspace, true) == UCN_OK);

    memset(&ack, 0, sizeof(ack));
    ack.selected = selected;
    memset(ack.transcript_digest, 0xA6,
           sizeof(ack.transcript_digest));
    context.expected_select = selected;
    memcpy(context.expected_transcript_digest, ack.transcript_digest,
           sizeof(context.expected_transcript_digest));
    context.expected_select_valid = 1U;
    context.expected_transcript_valid = 1U;
    CHECK(ucn_i_profile_ack_encode(&ack, body) == UCN_OK);
    capability_packet_prepare(
        UCN_I_OPCODE_CAPABILITY_ACK, UCN_DELIVERY_BEST_EFFORT,
        UCN_INTERACTION_RESULT, UINT64_C(4105), body,
        UCN_I_PROFILE_ACK_BYTES, 105U, &rx_workspace, opened,
        sizeof(opened), &opened_bytes, &rx_plan);
    CHECK(ucn_i_composition_capability_control_handle_with_context(
              fixture.runtime, &rx_plan, 105U, &context,
              &capability_workspace, &capability_result) == UCN_OK);
    CHECK(capability_result.kind ==
          UCN_I_COMPOSITION_CAPABILITY_CONTROL_ACK);
    CHECK(memcmp(&capability_result.acknowledged, &ack,
                 sizeof(ack)) == 0);
    CHECK(ucn_i_composition_control_rx_finish(
              fixture.runtime, &rx_plan, &finish_workspace, true) == UCN_OK);

    memset(&query, 0, sizeof(query));
    query.requested_generation = cached.record.capability_generation;
    memcpy(query.known_digest, cached.digest, sizeof(query.known_digest));
    CHECK(ucn_i_capability_query_encode(&query, body) == UCN_OK);
    capability_packet_prepare(
        UCN_I_OPCODE_CAPABILITY_QUERY, UCN_DELIVERY_BEST_EFFORT,
        UCN_INTERACTION_REQUEST, UINT64_C(4106), body,
        UCN_I_CAPABILITY_QUERY_BYTES, 106U, &rx_workspace, opened,
        sizeof(opened), &opened_bytes, &rx_plan);
    CHECK(ucn_i_composition_capability_control_handle(
              fixture.runtime, &rx_plan, 106U, &capability_workspace,
              &capability_result) == UCN_OK);
    CHECK(capability_result.kind == UCN_I_COMPOSITION_CAPABILITY_CONTROL_QUERY);
    CHECK(memcmp(&capability_result.query,&query,sizeof(query))==0);
    CHECK(ucn_i_composition_control_rx_finish(
              fixture.runtime,&rx_plan,&finish_workspace,true)==UCN_OK);
    CHECK(ucn_i_capability_get(fixture.capability_owner,&cached_reference,106U,&cached)==UCN_OK);
    capability_packet_prepare(
        UCN_I_OPCODE_CAPABILITY_REVOKE, UCN_DELIVERY_BEST_EFFORT,
        UCN_INTERACTION_ONE_WAY, UINT64_C(4107), body,
        UCN_I_CAPABILITY_QUERY_BYTES, 106U, &rx_workspace, opened,
        sizeof(opened), &opened_bytes, &rx_plan);
    CHECK(ucn_i_composition_capability_control_handle(
              fixture.runtime, &rx_plan, 106U, &capability_workspace,
              &capability_result) == UCN_OK);
    CHECK(capability_result.kind ==
          UCN_I_COMPOSITION_CAPABILITY_CONTROL_REVOKE);
    CHECK(memcmp(&capability_result.revoked, &query,
                 sizeof(query)) == 0);
    CHECK(ucn_i_composition_control_rx_finish(
              fixture.runtime, &rx_plan, &finish_workspace, true) == UCN_OK);
    CHECK(ucn_i_capability_get(
              fixture.capability_owner, &cached_reference, 107U,
              &cached) == UCN_ERR_STATE);
}

static void test_bounded_ingress_scheduler_rotation_and_obligation(void)
{
    ucn_i_admission_hello_t hello;
    ucn_i_capability_record_t record;
    ucn_i_composition_ingress_result_t ingress_result = {0};
    ucn_i_composition_ingress_result_t result_before;
    ucn_i_composition_capability_control_workspace_t capability_workspace;
    ucn_i_composition_capability_control_result_t capability_result;
    ucn_i_cached_capability_t cached;
    ucn_step_budget_t budget;
    ucn_step_result_t foundation_result;
    uint8_t bootstrap_body[UCN_I_ADMISSION_HELLO_BYTES];
    uint8_t capability_body[UCN_I_CAPABILITY_RECORD_BYTES];
    uint8_t packet[UCN_ADAPTER_FRAME_BYTES];
    uint8_t attempts;
    uint32_t active_generation;
    size_t packet_bytes;

    fixture_init(&fixture);
    fixture.runtime->foundation_node->external_ingress_demux = 1U;
    fixture.security_owner->sessions[0].candidate.address_width = 1U;
    fixture.security_owner->sessions[0].candidate.hop_profile =
        UCN_I_HOP_PROFILE_H0;
    fixture.security_owner->sessions[0].candidate.link_generation = 1U;
    fixture.request.security_facts.link_generation = 1U;
    memset(fixture.capability_owner->peers, 0,
           sizeof(fixture.capability_owner->peers));
    fixture.security_acl[1].protocol_opcode =
        UCN_I_OPCODE_CAPABILITY_ADVERTISE;

    memset(&budget, 0, sizeof(budget));
    budget.struct_size = (uint16_t)sizeof(budget);
    budget.api_version = UCN_API_VERSION;
    budget.max_work = 16U;
    do {
        memset(&foundation_result, 0, sizeof(foundation_result));
        CHECK(ucn_step(fixture.runtime->foundation_node, 99U, &budget,
                       &foundation_result) == UCN_OK);
    } while (foundation_result.more_work != 0U);

    memset(&hello, 0, sizeof(hello));
    hello.device_nonce = UINT64_C(0x0102030405060708);
    hello.transaction_id = UINT64_C(5001);
    memset(hello.identity_digest, 0x42, sizeof(hello.identity_digest));
    CHECK(ucn_i_admission_hello_encode(&hello, bootstrap_body) == UCN_OK);
    packet_bytes = bootstrap_packet_build(
        UCN_I_OPCODE_BOOTSTRAP_HELLO, UCN_INTERACTION_REQUEST,
        hello.transaction_id, bootstrap_body, sizeof(bootstrap_body),
        packet, sizeof(packet));
    publish_packet(&fixture, packet, packet_bytes);

    record = direct_capability_record(1U, 1U);
    CHECK(ucn_i_capability_record_encode(&record, capability_body) == UCN_OK);
    packet_bytes = control_packet_build_profile(
        &fixture, UCN_I_OPCODE_CAPABILITY_ADVERTISE, UCN_TRAFFIC_Q2,
        UCN_DELIVERY_BEST_EFFORT, UCN_INTERACTION_ONE_WAY,
        UINT64_C(5002), capability_body, sizeof(capability_body),
        packet, sizeof(packet));
    publish_packet(&fixture, packet, packet_bytes);

    hello.transaction_id = UINT64_C(5003);
    CHECK(ucn_i_admission_hello_encode(&hello, bootstrap_body) == UCN_OK);
    packet_bytes = bootstrap_packet_build(
        UCN_I_OPCODE_BOOTSTRAP_HELLO, UCN_INTERACTION_REQUEST,
        hello.transaction_id, bootstrap_body, sizeof(bootstrap_body),
        packet, sizeof(packet));
    publish_packet(&fixture, packet, packet_bytes);

    budget.max_work = 1U;
    memset(&ingress_workspace, 0, sizeof(ingress_workspace));
    for (attempts = 0U; attempts < UCN_I_COMPOSITION_INGRESS_CLASS_COUNT;
         ++attempts) {
        memset(&ingress_result, 0, sizeof(ingress_result));
        CHECK(ucn_i_composition_ingress_step(
                  fixture.runtime, 100U, &budget, &ingress_workspace,
                  &ingress_result) == UCN_OK);
        if (ingress_result.selected_kind ==
            UCN_I_COMPOSITION_INGRESS_BOOTSTRAP) {
            break;
        }
    }
    CHECK(attempts < UCN_I_COMPOSITION_INGRESS_CLASS_COUNT);
    CHECK(ingress_result.operations == 1U);
    CHECK(ingress_result.selected_kind ==
          UCN_I_COMPOSITION_INGRESS_BOOTSTRAP);
    CHECK(ingress_result.next_cursor == 2U);
    CHECK(ingress_workspace.bootstrap_plan.message.hello.transaction_id ==
          UINT64_C(5001));
    active_generation = ingress_workspace.generation;

    memset(&ingress_result, 0xA5, sizeof(ingress_result));
    result_before = ingress_result;
    CHECK(ucn_i_composition_ingress_step(
              fixture.runtime, 101U, &budget, &ingress_workspace,
              &ingress_result) == UCN_ERR_STATE);
    CHECK(memcmp(&ingress_result, &result_before,
                 sizeof(ingress_result)) == 0);
    CHECK(ucn_i_composition_stop_begin(fixture.runtime) == UCN_ERR_STATE);
    ++ingress_workspace.generation;
    CHECK(ucn_i_composition_ingress_finish(
              fixture.runtime, &ingress_workspace, false) ==
          UCN_ERR_ARGUMENT);
    ingress_workspace.generation = active_generation;
    CHECK(ucn_i_composition_ingress_finish(
              fixture.runtime, &ingress_workspace, false) == UCN_OK);

    memset(&ingress_result, 0, sizeof(ingress_result));
    CHECK(ucn_i_composition_ingress_step(
              fixture.runtime, 102U, &budget, &ingress_workspace,
              &ingress_result) == UCN_OK);
    CHECK(ingress_result.operations == 1U);
    CHECK(ingress_result.selected_kind ==
          UCN_I_COMPOSITION_INGRESS_AUTHENTICATED_CONTROL);
    CHECK(ingress_result.next_cursor == 0U);
    CHECK(ingress_workspace.control_body_bytes == sizeof(capability_body));
    CHECK(ingress_workspace.control_plan.dispatch.target_module ==
          UCN_I_COMPOSITION_MODULE_CAPABILITY);
    memset(&capability_result, 0, sizeof(capability_result));
    CHECK(ucn_i_composition_capability_control_handle(
              fixture.runtime, &ingress_workspace.control_plan, 102U,
              &capability_workspace, &capability_result) == UCN_OK);
    CHECK(ucn_i_composition_ingress_finish(
              fixture.runtime, &ingress_workspace, true) == UCN_OK);
    CHECK(ucn_i_capability_get(
              fixture.capability_owner, &capability_result.reference,
              102U, &cached) == UCN_OK);
    CHECK(cached.record.capability_generation == 1U);

    for (attempts = 0U; attempts < UCN_I_COMPOSITION_INGRESS_CLASS_COUNT;
         ++attempts) {
        memset(&ingress_result, 0, sizeof(ingress_result));
        CHECK(ucn_i_composition_ingress_step(
                  fixture.runtime, 103U, &budget, &ingress_workspace,
                  &ingress_result) == UCN_OK);
        if (ingress_result.selected_kind ==
            UCN_I_COMPOSITION_INGRESS_BOOTSTRAP) {
            break;
        }
    }
    CHECK(attempts < UCN_I_COMPOSITION_INGRESS_CLASS_COUNT);
    CHECK(ingress_result.operations == 1U);
    CHECK(ingress_result.selected_kind ==
          UCN_I_COMPOSITION_INGRESS_BOOTSTRAP);
    CHECK(ingress_result.next_cursor == 2U);
    CHECK(ingress_workspace.bootstrap_plan.message.hello.transaction_id ==
          UINT64_C(5003));
    CHECK(ucn_i_composition_ingress_finish(
              fixture.runtime, &ingress_workspace, false) == UCN_OK);

    do {
        memset(&ingress_result, 0, sizeof(ingress_result));
        CHECK(ucn_i_composition_ingress_step(
                  fixture.runtime, 104U, &budget, &ingress_workspace,
                  &ingress_result) == UCN_OK);
    } while (ingress_result.operations != 0U);
    CHECK(ingress_result.selected_kind == UCN_I_COMPOSITION_INGRESS_NONE);
}

static void test_multihop_control_relay_uses_independent_hop_sessions(void)
{
    const ucn_i_route_binding_t binding_a =
        route_binding(1U, 10U, 0x11U);
    const ucn_i_route_binding_t binding_b =
        route_binding(2U, 20U, 0x22U);
    const ucn_i_route_binding_t binding_d =
        route_binding(4U, 40U, 0x44U);
    ucn_i_security_candidate_t origin_candidate;
    ucn_i_security_candidate_t source_hop_candidate;
    ucn_i_security_candidate_t relay_in_candidate;
    ucn_i_security_candidate_t relay_out_candidate;
    ucn_i_security_handle_t origin_session;
    ucn_i_security_handle_t source_hop_session;
    ucn_i_security_current_facts_t origin_facts;
    ucn_i_security_current_facts_t source_hop_facts;
    ucn_i_security_c0_protect_request_t protect;
    ucn_i_composition_control_tx_plan_t source_tx_plan;
    ucn_i_composition_ingress_result_t ingress_result = {0};
    ucn_link_handle_t expected_source_link;
    ucn_link_handle_t expected_egress_link;
    ucn_i_c0_frame_t source_frame;
    ucn_i_c0_frame_t relayed_frame;
    ucn_step_budget_t budget;
    ucn_step_result_t foundation_result;
    uint8_t body[UCN_I_DIAGNOSTIC_ECHO_BYTES] = {
        0x91U, 0x82U, 0x73U, 0x64U,
        0x55U, 0x46U, 0x37U, 0x28U
    };
    uint8_t source_packet[UCN_ADAPTER_FRAME_BYTES];
    uint32_t submit_calls;
    size_t source_packet_bytes = 0U;
    uint8_t attempts;
    ucn_result_t replay_result = UCN_OK;

    fixture_init(&fixture);
    fixture.runtime->foundation_node->external_ingress_demux = 1U;

    origin_candidate = security_path_candidate(
        binding_a, binding_d, 101U, 77U, UCN_I_HOP_PROFILE_H0,
        1U, 1U, 0x81U, 0x82U);
    source_hop_candidate = security_path_candidate(
        binding_a, binding_b, 102U, 1U, UCN_I_HOP_PROFILE_H1,
        31U, 32U, 0x83U, 0x91U);
    relay_in_candidate = security_path_candidate(
        binding_b, binding_a, 103U, 1U, UCN_I_HOP_PROFILE_H1,
        32U, 31U, 0x84U, 0x91U);
    relay_out_candidate = security_path_candidate(
        binding_b, binding_d, 104U, 2U, UCN_I_HOP_PROFILE_H1,
        41U, 42U, 0x85U, 0xA1U);
    origin_session = security_active_slot_install(
        &fixture, 0U, &origin_candidate);
    source_hop_session = security_active_slot_install(
        &fixture, 1U, &source_hop_candidate);

    memset(fixture.security_acl, 0, sizeof(fixture.security_acl));
    memcpy(fixture.security_acl[0].peer_principal,
           binding_d.principal, sizeof(binding_d.principal));
    memset(fixture.security_acl[0].context_fingerprint, 0x81,
           sizeof(fixture.security_acl[0].context_fingerprint));
    fixture.security_acl[0].peer_binding_generation = binding_d.generation;
    fixture.security_acl[0].protocol_opcode = UCN_I_OPCODE_DIAGNOSTIC_PING;
    fixture.security_acl[0].direction = UCN_I_SECURITY_ACCESS_OUTBOUND;
    memcpy(fixture.security_acl[1].peer_principal,
           binding_b.principal, sizeof(binding_b.principal));
    memset(fixture.security_acl[1].context_fingerprint, 0x91,
           sizeof(fixture.security_acl[1].context_fingerprint));
    fixture.security_acl[1].peer_binding_generation = binding_b.generation;
    fixture.security_acl[1].protocol_opcode = UCN_I_OPCODE_DIAGNOSTIC_PING;
    fixture.security_acl[1].direction = UCN_I_SECURITY_ACCESS_OUTBOUND;

    memset(fixture.route_owner->routes, 0,
           sizeof(fixture.route_owner->routes));
    fixture.route_owner->routes[0].value.domain.origin = binding_a;
    fixture.route_owner->routes[0].value.domain.destination = binding_d;
    fixture.route_owner->routes[0].value.domain.realm = 7U;
    fixture.route_owner->routes[0].value.domain.origin_session_generation =
        origin_candidate.session_generation;
    fixture.route_owner->routes[0].value.next_hop.peer = binding_d;
    fixture.route_owner->routes[0].value.next_hop.link_id = 2U;
    fixture.route_owner->routes[0].value.next_hop.link_generation = 2U;
    fixture.route_owner->routes[0].value.next_hop.frame_mtu = 256U;
    fixture.route_owner->routes[0].value.route_causal_id = 6000U;
    fixture.route_owner->routes[0].value.expires_at_us = 9000U;
    fixture.route_owner->routes[0].value.runtime_instance =
        fixture.runtime->runtime_instance;
    fixture.route_owner->routes[0].value.route_generation = 1U;
    fixture.route_owner->routes[0].value.owner_instance =
        fixture.route_owner->owner_instance;
    fixture.route_owner->routes[0].value.path_frame_mtu = 256U;
    fixture.route_owner->routes[0].value.hop_count = 1U;
    fixture.route_owner->routes[0].occupied = 1U;
    CHECK(ucn_i_composition_link_get(
              fixture.runtime, 0U, &expected_source_link) == UCN_OK);
    CHECK(ucn_i_composition_link_get(
              fixture.runtime, 1U, &expected_egress_link) == UCN_OK);

    origin_facts = security_candidate_facts(&origin_candidate, 100U);
    source_hop_facts = security_candidate_facts(
        &source_hop_candidate, 100U);
    memset(&protect, 0, sizeof(protect));
    protect.session = origin_session;
    protect.facts = origin_facts;
    protect.transaction_id = 6001U;
    protect.protocol_opcode = UCN_I_OPCODE_DIAGNOSTIC_PING;
    memset(protect.context_fingerprint, 0x81,
           sizeof(protect.context_fingerprint));
    protect.traffic_class = UCN_TRAFFIC_Q0;
    protect.delivery_guarantee = UCN_DELIVERY_BEST_EFFORT;
    protect.interaction_role = UCN_INTERACTION_REQUEST;
    protect.payload_kind = UCN_I_PAYLOAD_DIAGNOSTIC;
    protect.hop_limit = 3U;
    protect.direction = UCN_I_SECURITY_ACCESS_OUTBOUND;
    memset(&source_tx_workspace, 0, sizeof(source_tx_workspace));
    memset(&source_tx_plan, 0, sizeof(source_tx_plan));
    CHECK(ucn_i_composition_control_path_tx_submit(
              fixture.runtime, 0U, &protect, source_hop_session,
              &source_hop_facts, body, sizeof(body),
              &source_tx_workspace, &source_tx_plan) == UCN_OK);
    CHECK(memcmp(&fixture.driver_last_link, &expected_source_link,
                 sizeof(fixture.driver_last_link)) == 0);
    CHECK(fixture.driver_last_bytes <= sizeof(source_packet));
    source_packet_bytes = fixture.driver_last_bytes;
    memcpy(source_packet, fixture.driver_last_frame,
           source_packet_bytes);
    memset(&source_tx_finish_workspace, 0,
           sizeof(source_tx_finish_workspace));
    CHECK(ucn_i_composition_control_tx_finish(
              fixture.runtime, &source_tx_plan,
              &source_tx_finish_workspace) == UCN_OK);
    CHECK(ucn_i_c0_decode(source_packet, source_packet_bytes, 1U,
                          UCN_I_CONTROL_H1, &source_frame) == UCN_OK);
    CHECK(source_frame.hop_limit == 3U);

    /* EN: Source and relay are two logical nodes.  Reuse the two Nano
     * session slots only after the source packet and its Adapter obligation
     * are terminal, rather than requiring one test Owner to hold four peer
     * sessions simultaneously.
     * 中文：Source 与 Relay 是两个逻辑节点。源报文及其 Adapter obligation
     * 终结后再复用 Nano 的两个 Session 槽，避免测试错误要求单个 Owner 同时
     * 容纳四个 Peer Session。 */
    (void)security_active_slot_install(&fixture, 0U, &relay_in_candidate);
    (void)security_active_slot_install(&fixture, 1U, &relay_out_candidate);
    memset(fixture.security_acl, 0, sizeof(fixture.security_acl));
    memcpy(fixture.security_acl[0].peer_principal,
           binding_a.principal, sizeof(binding_a.principal));
    memset(fixture.security_acl[0].context_fingerprint, 0x91,
           sizeof(fixture.security_acl[0].context_fingerprint));
    fixture.security_acl[0].peer_binding_generation = binding_a.generation;
    fixture.security_acl[0].protocol_opcode = UCN_I_OPCODE_DIAGNOSTIC_PING;
    fixture.security_acl[0].direction = UCN_I_SECURITY_ACCESS_INBOUND;
    memcpy(fixture.security_acl[1].peer_principal,
           binding_d.principal, sizeof(binding_d.principal));
    memset(fixture.security_acl[1].context_fingerprint, 0xA1,
           sizeof(fixture.security_acl[1].context_fingerprint));
    fixture.security_acl[1].peer_binding_generation = binding_d.generation;
    fixture.security_acl[1].protocol_opcode = UCN_I_OPCODE_DIAGNOSTIC_PING;
    fixture.security_acl[1].direction = UCN_I_SECURITY_ACCESS_OUTBOUND;

    memset(&budget, 0, sizeof(budget));
    budget.struct_size = (uint16_t)sizeof(budget);
    budget.api_version = UCN_API_VERSION;
    budget.max_work = 16U;
    do {
        memset(&foundation_result, 0, sizeof(foundation_result));
        CHECK(ucn_step(fixture.runtime->foundation_node, 99U, &budget,
                       &foundation_result) == UCN_OK);
    } while (foundation_result.more_work != 0U);
    publish_packet(&fixture, source_packet, source_packet_bytes);
    budget.max_work = 1U;
    memset(&ingress_workspace, 0, sizeof(ingress_workspace));
    for (attempts = 0U; attempts < UCN_I_COMPOSITION_INGRESS_CLASS_COUNT;
         ++attempts) {
        memset(&ingress_result, 0, sizeof(ingress_result));
        replay_result = ucn_i_composition_ingress_step(
            fixture.runtime, 101U, &budget, &ingress_workspace,
            &ingress_result);
        CHECK(replay_result == UCN_OK);
        if (ingress_result.selected_kind ==
            UCN_I_COMPOSITION_INGRESS_CONTROL_RELAY) {
            break;
        }
    }
    CHECK(attempts < UCN_I_COMPOSITION_INGRESS_CLASS_COUNT);
    CHECK(ingress_result.selected_kind ==
          UCN_I_COMPOSITION_INGRESS_CONTROL_RELAY);
    CHECK(fixture.driver_submit_calls == 2U);
    CHECK(memcmp(&fixture.driver_last_link,
                 &expected_egress_link,
                 sizeof(fixture.driver_last_link)) == 0);
    CHECK(ucn_i_c0_decode(fixture.driver_last_frame,
                          fixture.driver_last_bytes, 1U,
                          UCN_I_CONTROL_H1, &relayed_frame) == UCN_OK);
    CHECK(relayed_frame.hop_limit == 2U);
    CHECK(relayed_frame.payload_bytes == source_frame.payload_bytes);
    CHECK(memcmp(relayed_frame.payload, source_frame.payload,
                 source_frame.payload_bytes) == 0);
    CHECK(memcmp(relayed_frame.origin_tag, source_frame.origin_tag,
                 UCN_I_SECURITY_ORIGIN_TAG_BYTES) == 0);
    CHECK(ucn_i_composition_ingress_finish(
              fixture.runtime, &ingress_workspace, true) == UCN_OK);

    submit_calls = fixture.driver_submit_calls;
    publish_packet(&fixture, source_packet, source_packet_bytes);
    for (attempts = 0U; attempts < UCN_I_COMPOSITION_INGRESS_CLASS_COUNT;
         ++attempts) {
        memset(&ingress_result, 0, sizeof(ingress_result));
        replay_result = ucn_i_composition_ingress_step(
            fixture.runtime, 102U, &budget, &ingress_workspace,
            &ingress_result);
        if (replay_result != UCN_OK) {
            break;
        }
    }
    CHECK(replay_result == UCN_ERR_REPLAY);
    CHECK(fixture.driver_submit_calls == submit_calls);
    CHECK(fixture.runtime->ingress_pending_kind ==
          UCN_I_COMPOSITION_INGRESS_NONE);
}

static void test_authenticated_route_control_typed_handler(void)
{
    ucn_i_security_control_access_request_t access;
    ucn_i_composition_control_dispatch_plan_t plan;
    ucn_i_composition_route_control_workspace_t route_workspace;
    ucn_i_composition_route_control_result_t route_result;
    ucn_i_composition_route_control_result_t route_before;
    ucn_i_security_control_validate_workspace_t finish_workspace;
    ucn_i_route_link_ref_t ingress = direct_route_ingress();
    ucn_i_route_link_ref_t stale_ingress;
    ucn_i_security_current_facts_t stale_facts;
    ucn_i_route_rreq_payload_t rreq_payload;
    ucn_i_route_rreq_message_t discovery;
    ucn_i_route_rreq_message_t incoming;
    ucn_i_route_rrep_message_t reply;
    ucn_i_route_rrep_payload_t rrep_payload;
    ucn_i_route_rerr_payload_t rerr_payload;
    ucn_i_route_rerr_payload_t spoofed_rerr;
    ucn_i_route_soft_view_t rerr_route;
    ucn_handle_t discovery_handle;
    uint8_t created = 0U;
    uint8_t body[UCN_I_ROUTE_RERR_BYTES];
    uint8_t packet[UCN_ADAPTER_FRAME_BYTES];
    uint8_t opened[UCN_I_ROUTE_RERR_BYTES];
    size_t packet_bytes;
    size_t opened_bytes;

    fixture_init(&fixture);
    fixture.security_owner->sessions[0].candidate.address_width = 1U;
    fixture.security_owner->sessions[0].candidate.hop_profile =
        UCN_I_HOP_PROFILE_H0;
    fixture.security_acl[1].protocol_opcode = UCN_I_OPCODE_ROUTE_RREQ;
    memset(&access, 0, sizeof(access));
    access.session = fixture.request.security.session;
    memset(access.context_fingerprint, 0x55,
           sizeof(access.context_fingerprint));
    access.protocol_opcode = UCN_I_OPCODE_ROUTE_RREQ;
    access.direction = UCN_I_SECURITY_ACCESS_INBOUND;
    memset(&rreq_payload, 0, sizeof(rreq_payload));
    rreq_payload.minimum_frame_mtu = 32U;
    rreq_payload.required_capability_bits = 1U;
    memset(&incoming, 0, sizeof(incoming));
    incoming.key.origin.address = 2U;
    incoming.key.origin.generation = 20U;
    memcpy(incoming.key.origin.principal,
           fixture.request.security_facts.peer.principal, 16U);
    incoming.key.realm = fixture.route_owner->realm;
    incoming.key.origin_session_generation = 1U;
    incoming.key.target_address = 1U;
    incoming.key.transaction_id = UINT64_C(7001);
    incoming.remaining_hops = 1U;
    incoming.payload = rreq_payload;
    CHECK(ucn_i_route_control_request_encode(&incoming, 1U, body) == UCN_OK);
    packet_bytes = control_packet_build_role(
        &fixture, UCN_I_OPCODE_ROUTE_RREQ, UCN_INTERACTION_REQUEST,
        UINT64_C(3001), body, UCN_I_ROUTE_CONTROL_REQUEST_BYTES, packet,
        sizeof(packet));
    opened_bytes = SIZE_MAX;
    CHECK(ucn_i_composition_control_receive_prepare(
              fixture.runtime, &access, &fixture.request.security_facts,
              packet, packet_bytes, &control_workspace, opened,
              sizeof(opened), &opened_bytes, &plan) == UCN_OK);
    CHECK(plan.target_module == UCN_I_COMPOSITION_MODULE_ROUTE);
    CHECK(opened_bytes == UCN_I_ROUTE_CONTROL_REQUEST_BYTES);
    memset(&route_result, 0xA5, sizeof(route_result));
    route_before = route_result;
    stale_ingress = ingress;
    stale_ingress.link_generation++;
    CHECK(ucn_i_composition_route_control_handle(
              fixture.runtime, &plan, &fixture.request.security_facts,
              &stale_ingress, 100U, &route_workspace, &route_result) ==
          UCN_ERR_ARGUMENT);
    CHECK(memcmp(&route_result, &route_before, sizeof(route_result)) == 0);
    CHECK(fixture.route_owner->reverse[0].occupied == 0U);

    stale_facts = fixture.request.security_facts;
    stale_facts.local.binding_generation++;
    CHECK(ucn_i_composition_route_control_handle(
              fixture.runtime, &plan, &stale_facts, &ingress, 100U,
              &route_workspace, &route_result) == UCN_ERR_STATE);
    CHECK(memcmp(&route_result, &route_before, sizeof(route_result)) == 0);
    CHECK(fixture.route_owner->reverse[0].occupied == 0U);

    memset(&route_result, 0, sizeof(route_result));
    CHECK(ucn_i_composition_route_control_handle(
              fixture.runtime, &plan, &fixture.request.security_facts,
              &ingress, 100U, &route_workspace, &route_result) == UCN_OK);
    CHECK(route_result.kind == UCN_I_COMPOSITION_ROUTE_CONTROL_RREQ);
    CHECK(route_result.action.request.kind ==
          UCN_I_ROUTE_REQUEST_LOCAL_TARGET);
    CHECK(route_workspace.message.rreq.key.origin.address == 2U);
    CHECK(route_workspace.message.rreq.key.origin.generation == 20U);
    CHECK(route_workspace.message.rreq.key.target_address == 1U);
    CHECK(route_workspace.message.rreq.remaining_hops == 1U);
    CHECK(ucn_i_composition_control_dispatch_finish(
              fixture.runtime, &plan, &fixture.request.security_facts,
              &finish_workspace, true) == UCN_OK);

    fixture_init(&fixture);
    fixture.security_owner->sessions[0].candidate.address_width = 1U;
    fixture.security_owner->sessions[0].candidate.hop_profile =
        UCN_I_HOP_PROFILE_H0;
    fixture.security_acl[1].protocol_opcode = UCN_I_OPCODE_ROUTE_RREP;
    access.session = fixture.request.security.session;
    access.protocol_opcode = UCN_I_OPCODE_ROUTE_RREP;
    CHECK(ucn_i_route_ensure_discovery(
              fixture.route_owner, 2U, 1U, 32U, 0U, 90U,
              &discovery_handle, &discovery, &created) == UCN_OK);
    CHECK(created == 1U);
    memset(&rrep_payload, 0, sizeof(rrep_payload));
    memset(rrep_payload.destination_principal, 0x22,
           sizeof(rrep_payload.destination_principal));
    rrep_payload.destination_binding_generation = 20U;
    rrep_payload.hop_count = 1U;
    rrep_payload.path_frame_mtu = 128U;
    rrep_payload.capability_bits = 1U;
    memset(&reply, 0, sizeof(reply));
    reply.key = discovery.key;
    reply.payload = rrep_payload;
    CHECK(ucn_i_route_control_reply_encode(&reply, 900U, 1U, body) == UCN_OK);
    packet_bytes = control_packet_build_role(
        &fixture, UCN_I_OPCODE_ROUTE_RREP, UCN_INTERACTION_RESULT,
        UINT64_C(8002), body, UCN_I_ROUTE_CONTROL_REPLY_BYTES,
        packet, sizeof(packet));
    opened_bytes = SIZE_MAX;
    CHECK(ucn_i_composition_control_receive_prepare(
              fixture.runtime, &access, &fixture.request.security_facts,
              packet, packet_bytes, &control_workspace, opened,
              sizeof(opened), &opened_bytes, &plan) == UCN_OK);
    memset(&route_result, 0, sizeof(route_result));
    CHECK(ucn_i_composition_route_control_handle(
              fixture.runtime, &plan, &fixture.request.security_facts,
              &ingress, 100U, &route_workspace, &route_result) == UCN_OK);
    CHECK(route_result.kind == UCN_I_COMPOSITION_ROUTE_CONTROL_RREP);
    CHECK(route_result.action.reply.kind ==
          UCN_I_ROUTE_REPLY_REACHED_ORIGIN);
    CHECK(route_result.action.reply.installed.domain.origin.address == 1U);
    CHECK(route_result.action.reply.installed.domain.destination.address ==
          2U);
    CHECK(ucn_i_composition_control_dispatch_finish(
              fixture.runtime, &plan, &fixture.request.security_facts,
              &finish_workspace, true) == UCN_OK);

    fixture.security_acl[1].protocol_opcode = UCN_I_OPCODE_ROUTE_RERR;
    access.session = fixture.request.security.session;
    access.protocol_opcode = UCN_I_OPCODE_ROUTE_RERR;
    rerr_route = fixture.route_owner->routes[0].value;
    CHECK(ucn_i_route_make_rerr(
              fixture.route_owner, &rerr_route,
              UCN_I_ROUTE_RERR_LINK_INVALID, &rerr_payload) == UCN_OK);
    /* Reporter's route/link generations are not our local namespace. */
    rerr_payload.route_generation = 900U;
    rerr_payload.failed_link_id = 42U;
    rerr_payload.failed_link_generation = 99U;
    spoofed_rerr = rerr_payload;
    spoofed_rerr.reporter_address = 3U;
    CHECK(ucn_i_route_rerr_encode(&spoofed_rerr, body) == UCN_OK);
    packet_bytes = control_packet_build_role(
        &fixture, UCN_I_OPCODE_ROUTE_RERR, UCN_INTERACTION_ERROR,
        UINT64_C(9002), body, UCN_I_ROUTE_RERR_BYTES, packet,
        sizeof(packet));
    opened_bytes = SIZE_MAX;
    CHECK(ucn_i_composition_control_receive_prepare(
              fixture.runtime, &access, &fixture.request.security_facts,
              packet, packet_bytes, &control_workspace, opened,
              sizeof(opened), &opened_bytes, &plan) == UCN_OK);
    memset(&route_result, 0xA5, sizeof(route_result));
    route_before = route_result;
    CHECK(ucn_i_composition_route_control_handle(
              fixture.runtime, &plan, &fixture.request.security_facts,
              &ingress, 100U, &route_workspace, &route_result) ==
          UCN_ERR_SECURITY);
    CHECK(memcmp(&route_result, &route_before, sizeof(route_result)) == 0);
    CHECK(fixture.route_owner->routes[0].occupied == 1U);
    CHECK(ucn_i_composition_control_dispatch_finish(
              fixture.runtime, &plan, &fixture.request.security_facts,
              &finish_workspace, false) == UCN_OK);

    CHECK(ucn_i_route_rerr_encode(&rerr_payload, body) == UCN_OK);
    packet_bytes = control_packet_build_role(
        &fixture, UCN_I_OPCODE_ROUTE_RERR, UCN_INTERACTION_ERROR,
        UINT64_C(9003), body, UCN_I_ROUTE_RERR_BYTES, packet,
        sizeof(packet));
    opened_bytes = SIZE_MAX;
    CHECK(ucn_i_composition_control_receive_prepare(
              fixture.runtime, &access, &fixture.request.security_facts,
              packet, packet_bytes, &control_workspace, opened,
              sizeof(opened), &opened_bytes, &plan) == UCN_OK);
    memset(&route_result, 0, sizeof(route_result));
    CHECK(ucn_i_composition_route_control_handle(
              fixture.runtime, &plan, &fixture.request.security_facts,
              &ingress, 100U, &route_workspace, &route_result) == UCN_OK);
    CHECK(route_result.kind == UCN_I_COMPOSITION_ROUTE_CONTROL_RERR);
    CHECK(route_result.invalidated == 1U);
    CHECK(fixture.route_owner->routes[0].occupied == 0U);
    CHECK(ucn_i_composition_control_dispatch_finish(
              fixture.runtime, &plan, &fixture.request.security_facts,
              &finish_workspace, true) == UCN_OK);

    fixture_init(&fixture);
    fixture.security_owner->sessions[0].candidate.address_width = 1U;
    fixture.security_owner->sessions[0].candidate.hop_profile =
        UCN_I_HOP_PROFILE_H0;
    fixture.security_acl[1].protocol_opcode = UCN_I_OPCODE_ROUTE_RREQ;
    access.session = fixture.request.security.session;
    access.protocol_opcode = UCN_I_OPCODE_ROUTE_RREQ;
    CHECK(ucn_i_route_control_request_encode(&incoming, 1U, body) == UCN_OK);
    packet_bytes = control_packet_build_role(
        &fixture, UCN_I_OPCODE_ROUTE_RREQ, UCN_INTERACTION_RESULT,
        UINT64_C(3004), body, UCN_I_ROUTE_CONTROL_REQUEST_BYTES, packet,
        sizeof(packet));
    opened_bytes = SIZE_MAX;
    CHECK(ucn_i_composition_control_receive_prepare(
              fixture.runtime, &access, &fixture.request.security_facts,
              packet, packet_bytes, &control_workspace, opened,
              sizeof(opened), &opened_bytes, &plan) == UCN_OK);
    memset(&route_result, 0xA5, sizeof(route_result));
    route_before = route_result;
    CHECK(ucn_i_composition_route_control_handle(
              fixture.runtime, &plan, &fixture.request.security_facts,
              &ingress, 100U, &route_workspace, &route_result) ==
          UCN_ERR_ARGUMENT);
    CHECK(memcmp(&route_result, &route_before, sizeof(route_result)) == 0);
    CHECK(ucn_i_composition_control_dispatch_finish(
              fixture.runtime, &plan, &fixture.request.security_facts,
              &finish_workspace, false) == UCN_OK);
}

static void test_authenticated_transport_setup_typed_handler(void)
{
    ucn_i_composition_control_rx_workspace_t rx_workspace;
    ucn_i_composition_control_rx_plan_t rx_plan;
    ucn_i_composition_transport_control_context_t context;
    ucn_i_composition_transport_control_workspace_t transport_workspace;
    ucn_i_composition_transport_control_result_t transport_result;
    ucn_i_security_control_validate_workspace_t finish_workspace;
    ucn_i_transport_transfer_setup_t setup;
    ucn_i_transport_transfer_view_t view;
    uint8_t body[UCN_I_TRANSPORT_TRANSFER_SETUP_RESULT_BYTES];
    uint8_t opened[UCN_I_TRANSPORT_TRANSFER_SETUP_RESULT_BYTES];
    uint8_t packet[UCN_ADAPTER_FRAME_BYTES];
    size_t body_bytes = 0U;
    size_t opened_bytes = SIZE_MAX;
    size_t packet_bytes;

    fixture_init(&fixture);
    fixture.security_owner->sessions[0].candidate.address_width = 1U;
    fixture.security_owner->sessions[0].candidate.hop_profile =
        UCN_I_HOP_PROFILE_H0;
    fixture.security_owner->sessions[0].candidate.link_generation = 1U;
    fixture.request.security_facts.link_generation = 1U;
    fixture.security_acl[1].protocol_opcode = UCN_I_OPCODE_TRANSFER_SETUP;

    memset(&setup, 0, sizeof(setup));
    setup.parent_kind = UCN_I_TRANSPORT_PARENT_KIND_FLOW;
    setup.parent_id = 9U;
    setup.parent_generation = 10U;
    setup.transfer_id = 11U;
    setup.service_id = 12U;
    setup.total_length = 8U;
    memset(setup.message_digest, 0x4AU, sizeof(setup.message_digest));
    setup.delivery = UCN_DELIVERY_RELIABLE;
    setup.interaction = UCN_INTERACTION_ONE_WAY;
    setup.fragment_count = 2U;
    setup.fragment_budget = 4U;
    setup.lifetime_ms = 1000U;
    CHECK(ucn_i_transport_transfer_setup_encode(
              &setup, body, sizeof(body), &body_bytes) == UCN_OK);
    CHECK(body_bytes == UCN_I_TRANSPORT_TRANSFER_SETUP_ONE_WAY_BYTES);
    packet_bytes = control_packet_build_profile(
        &fixture, UCN_I_OPCODE_TRANSFER_SETUP, UCN_TRAFFIC_Q1,
        UCN_DELIVERY_BEST_EFFORT, UCN_INTERACTION_REQUEST,
        UINT64_C(5001), body, body_bytes, packet, sizeof(packet));
    publish_packet(&fixture, packet, packet_bytes);
    CHECK(ucn_i_composition_control_rx_claim_prepare(
              fixture.runtime, 100U, &rx_workspace, opened,
              sizeof(opened), &opened_bytes, &rx_plan) == UCN_OK);
    CHECK(rx_plan.dispatch.target_module ==
          UCN_I_COMPOSITION_MODULE_TRANSPORT);

    memset(&context, 0, sizeof(context));
    context.struct_size = (uint16_t)sizeof(context);
    context.schema = UCN_I_COMPOSITION_TRANSPORT_CONTROL_SCHEMA;
    memset(context.parent_fingerprint, 0x7BU,
           sizeof(context.parent_fingerprint));
    context.transport_policy_generation = 13U;
    context.receive_credit = 2U;
    memset(&transport_result, 0, sizeof(transport_result));
    CHECK(ucn_i_composition_transport_control_handle(
              fixture.runtime, &rx_plan, 100U, &context,
              &transport_workspace, &transport_result) == UCN_OK);
    CHECK(transport_result.kind ==
          UCN_I_COMPOSITION_TRANSPORT_CONTROL_SETUP);
    CHECK(transport_result.exact_duplicate == 0U);
    CHECK(transport_result.setup_ack.accepted_fragment_budget == 4U);
    CHECK(transport_result.setup_ack.receive_credit == 2U);
    CHECK(ucn_i_transport_transfer_view(
              fixture.transport_owner, transport_result.transfer,
              &view) == UCN_OK);
    CHECK(view.setup.transfer_id == setup.transfer_id &&
          view.phase == UCN_I_TRANSPORT_TRANSFER_RECEIVING);
    CHECK(ucn_i_composition_control_rx_finish(
              fixture.runtime, &rx_plan, &finish_workspace, true) == UCN_OK);

    /* A new authenticated Control transaction carrying the identical Setup
     * must reuse the existing receive obligation without refreshing it. */
    packet_bytes = control_packet_build_profile(
        &fixture, UCN_I_OPCODE_TRANSFER_SETUP, UCN_TRAFFIC_Q1,
        UCN_DELIVERY_BEST_EFFORT, UCN_INTERACTION_REQUEST,
        UINT64_C(5002), body, body_bytes, packet, sizeof(packet));
    publish_packet(&fixture, packet, packet_bytes);
    opened_bytes = SIZE_MAX;
    CHECK(ucn_i_composition_control_rx_claim_prepare(
              fixture.runtime, 101U, &rx_workspace, opened,
              sizeof(opened), &opened_bytes, &rx_plan) == UCN_OK);
    CHECK(ucn_i_composition_transport_control_handle(
              fixture.runtime, &rx_plan, 101U, &context,
              &transport_workspace, &transport_result) == UCN_OK);
    CHECK(transport_result.exact_duplicate == 1U);
    CHECK(ucn_i_composition_control_rx_finish(
              fixture.runtime, &rx_plan, &finish_workspace, true) == UCN_OK);
}

int main(void)
{
    test_success_abort_commit_and_acl_binding();
    test_each_stale_layer_and_order();
    test_request_zero_write_and_lifecycle_gate();
    test_reliable_request_q1_atomic_bundle();
    test_transfer_request_q3_flow_binding();
    test_q2_modes_do_not_mix_transport_and_service();
    test_qos_failure_rolls_back_all_reservations();
    test_q0_and_mixed_delivery_semantics_rejected();
    test_durable_operation_stays_out_of_qos();
    test_o0_bootstrap_typed_dispatch_and_origin_separation();
    test_authenticated_control_dispatch_and_feature_gate();
    test_authenticated_capability_advertise_typed_handler();
    test_authenticated_capability_control_family();
    test_bounded_ingress_scheduler_rotation_and_obligation();
    test_multihop_control_relay_uses_independent_hop_sessions();
    test_authenticated_route_control_typed_handler();
    test_authenticated_transport_setup_typed_handler();

    if (failures != 0) {
        fprintf(stderr, "%d composition advanced-use test(s) failed\n",
                failures);
        return 1;
    }
    puts("composition advanced-use tests passed");
    return 0;
}
