#include "ucn/v6/ucn_v6_realtime.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "check failed: %s:%d: %s\n", __FILE__, __LINE__, \
                    #condition);                                                \
            return 1;                                                           \
        }                                                                       \
    } while (0)

/* Test-only compact description used to synthesize physically distinct
 * T1/T2/T3/T4 events. It is deliberately not a public or Wire DTO. */
typedef struct synthetic_exchange {
    uint16_t clock_domain_id;
    uint32_t domain_generation;
    uint64_t local_sample_us;
    int64_t offset_us;
    ucn_v6_realtime_uncertainty_t uncertainty;
} synthetic_exchange_t;

typedef struct fake_store {
    bool valid;
    ucn_v6_realtime_domain_record_t record;
    unsigned loads;
    unsigned reserves;
    bool fail_reserve;
    bool false_success;
    ucn_v6_realtime_owner_t *reenter_owner;
    ucn_v6_result_t reenter_result;
    ucn_v6_callback_gate_t *callback_gate;
    const void *callback_scope_owner;
    bool force_gate_leave_on_load;
    bool force_gate_leave_on_reserve;
    ucn_v6_result_t forced_gate_leave_result;
    ucn_v6_capability_owner_storage_t capability_storage;
    ucn_v6_capability_owner_t *capability_owner;
    ucn_v6_route_owner_storage_t route_storage;
    ucn_v6_route_owner_t *route_owner;
} fake_store_t;

static void gate_lock(void *context)
{
    (void)context;
}

static void gate_unlock(void *context)
{
    (void)context;
}

static ucn_v6_principal_t principal(uint8_t seed)
{
    ucn_v6_principal_t value;
    size_t index;
    for (index = 0U; index < sizeof(value.bytes); ++index) {
        value.bytes[index] = (uint8_t)(seed + index);
    }
    return value;
}

static ucn_v6_result_t load_domain_record(
    void *context,
    uint16_t domain,
    ucn_v6_realtime_domain_record_t *record)
{
    fake_store_t *store = (fake_store_t *)context;
    ++store->loads;
    if (store->reenter_owner != NULL) {
        store->reenter_result =
            ucn_v6_realtime_step(store->reenter_owner, 0U);
    }
    if (store->force_gate_leave_on_load && store->callback_gate != NULL &&
        store->callback_scope_owner != NULL) {
        store->forced_gate_leave_result = ucn_v6_callback_gate_leave(
            store->callback_gate, store->callback_scope_owner);
    }
    if (!store->valid ||
        store->record.config.clock_domain_id != domain) {
        return UCN_V6_ERR_NOT_FOUND;
    }
    *record = store->record;
    return UCN_V6_OK;
}

static ucn_v6_result_t reserve_domain_record(
    void *context, const ucn_v6_realtime_domain_record_t *record)
{
    fake_store_t *store = (fake_store_t *)context;
    ++store->reserves;
    if (store->reenter_owner != NULL) {
        store->reenter_result =
            ucn_v6_realtime_step(store->reenter_owner, 0U);
    }
    if (store->force_gate_leave_on_reserve && store->callback_gate != NULL &&
        store->callback_scope_owner != NULL) {
        store->forced_gate_leave_result = ucn_v6_callback_gate_leave(
            store->callback_gate, store->callback_scope_owner);
    }
    if (store->fail_reserve) {
        return UCN_V6_ERR_STATE;
    }
    if (!store->false_success) {
        store->valid = true;
        store->record = *record;
    }
    return UCN_V6_OK;
}

static ucn_v6_cached_peer_capability_t master_capability(
    const ucn_v6_principal_t *master,
    const ucn_v6_binding_key_t *binding,
    uint32_t generation)
{
    ucn_v6_cached_peer_capability_t value;
    memset(&value, 0, sizeof(value));
    value.valid = true;
    value.principal = *master;
    value.binding = *binding;
    value.session_generation = 3U;
    value.ingress_link_id = 5U;
    value.ingress_link_generation = 6U;
    value.record.capability_generation = 2U;
    value.record.link.link_instance_generation = 6U;
    value.record.link.carrier_mtu = 64U;
    value.record.link.link_frame_mtu = 256U;
    value.record.link.processing_frame_mtu = 248U;
    value.record.link.carrier_header_bytes = 2U;
    value.record.link.carrier_crc_bytes = 2U;
    value.record.link.carrier_max_fragments = 8U;
    value.record.link.link_flags = UCN_V6_LINK_ORDERED |
                                   UCN_V6_LINK_RELIABLE |
                                   UCN_V6_LINK_UNICAST |
                                   UCN_V6_LINK_SECURITY;
    value.record.link.nominal_rate_bps = 3000000U;
    value.record.link.hardware_priority_count = 4U;
    value.record.link.timestamp_capability_bits =
        UCN_V6_TIMESTAMP_RX_HARDWARE | UCN_V6_TIMESTAMP_TX_HARDWARE;
    value.record.link.timestamp_uncertainty_us = 4U;
    value.record.peer.feature_bits = UCN_V6_FEATURE_IDENTITY |
                                     UCN_V6_FEATURE_WIRE |
                                     UCN_V6_FEATURE_MESSAGE |
                                     UCN_V6_FEATURE_SECURITY |
                                     UCN_V6_FEATURE_CAPABILITY |
                                     UCN_V6_FEATURE_ROUTE |
                                     UCN_V6_FEATURE_REALTIME;
    value.record.peer.hop_suite_bits =
        UINT32_C(1) << UCN_V6_SUITE_HMAC_SHA256_128;
    value.record.peer.e2e_suite_bits =
        UINT32_C(1) << UCN_V6_SUITE_AES_GCM_128;
    value.record.peer.max_message_class = UCN_V6_MESSAGE_T256;
    value.record.peer.max_rx_window = 4U;
    value.record.peer.max_concurrent_transfers = 2U;
    value.record.peer.realtime_mode_bits =
        UCN_V6_REALTIME_MODE_SYNCED | UCN_V6_REALTIME_MODE_DEADLINE;
    value.record.peer.clock_domain_id = 7U;
    value.record.peer.clock_domain_generation = generation;
    (void)ucn_v6_capability_digest(&value.record, value.digest);
    value.discovery_deadline_us = UINT64_MAX;
    value.capability_deadline_us = UINT64_MAX;
    return value;
}

static ucn_v6_path_capability_t fixed_path(
    const ucn_v6_principal_t *master,
    const ucn_v6_binding_key_t *binding,
    const ucn_v6_cached_peer_capability_t *capability)
{
    ucn_v6_path_capability_t value;
    memset(&value, 0, sizeof(value));
    value.valid = true;
    value.immutable_for_realtime = true;
    value.destination_principal = *master;
    value.destination_binding = *binding;
    value.destination_session_generation = capability->session_generation;
    value.destination_capability_generation =
        capability->record.capability_generation;
    memcpy(value.destination_capability_digest, capability->digest,
           sizeof(value.destination_capability_digest));
    value.destination_realtime_mode_bits =
        capability->record.peer.realtime_mode_bits;
    value.destination_clock_domain_id =
        capability->record.peer.clock_domain_id;
    value.destination_clock_domain_generation =
        capability->record.peer.clock_domain_generation;
    value.local_parent_session.principal = *master;
    value.local_parent_session.binding = *binding;
    value.local_parent_session.session_generation =
        capability->session_generation;
    value.local_parent_link_id = capability->ingress_link_id;
    value.local_parent_link_generation = capability->ingress_link_generation;
    value.local_parent_capability_generation =
        capability->record.capability_generation;
    memcpy(value.local_parent_capability_digest, capability->digest,
           sizeof(value.local_parent_capability_digest));
    value.route_generation = 1U;
    value.path_id = 2U;
    value.path_generation = 4U;
    value.hop_count = 1U;
    value.path_frame_mtu = 240U;
    value.payload_budget = 192U;
    value.fragment_data_budget = 160U;
    value.feature_bits = UCN_V6_FEATURE_REALTIME;
    value.hop_suite_bits = UINT32_C(1) << UCN_V6_SUITE_HMAC_SHA256_128;
    value.e2e_suite_bits = UINT32_C(1) << UCN_V6_SUITE_AES_GCM_128;
    value.max_message_class = UCN_V6_MESSAGE_T256;
    value.max_window = 4U;
    value.max_concurrency = 2U;
    value.timestamp_capability_bits =
        UCN_V6_TIMESTAMP_RX_HARDWARE | UCN_V6_TIMESTAMP_TX_HARDWARE;
    value.timestamp_uncertainty_us = 4U;
    value.deadline_us = UINT64_MAX;
    return value;
}

static void refresh_capability_binding(
    ucn_v6_cached_peer_capability_t *capability,
    ucn_v6_path_capability_t *path)
{
    (void)ucn_v6_capability_digest(&capability->record,
                                   capability->digest);
    if (path != NULL) {
        path->destination_session_generation = capability->session_generation;
        path->destination_capability_generation =
            capability->record.capability_generation;
        memcpy(path->destination_capability_digest, capability->digest,
               sizeof(path->destination_capability_digest));
        path->destination_realtime_mode_bits =
            capability->record.peer.realtime_mode_bits;
        path->destination_clock_domain_id =
            capability->record.peer.clock_domain_id;
        path->destination_clock_domain_generation =
            capability->record.peer.clock_domain_generation;
        path->local_parent_session.principal = capability->principal;
        path->local_parent_session.binding = capability->binding;
        path->local_parent_session.session_generation =
            capability->session_generation;
        path->local_parent_link_id = capability->ingress_link_id;
        path->local_parent_link_generation = capability->ingress_link_generation;
        path->local_parent_capability_generation =
            capability->record.capability_generation;
        memcpy(path->local_parent_capability_digest, capability->digest,
               sizeof(path->local_parent_capability_digest));
    }
}

static ucn_v6_time_domain_config_t domain_config(
    const ucn_v6_principal_t *master,
    const ucn_v6_binding_key_t *binding,
    uint32_t generation)
{
    ucn_v6_time_domain_config_t value;
    memset(&value, 0, sizeof(value));
    value.clock_domain_id = 7U;
    value.domain_generation = generation;
    value.master_principal = *master;
    value.master_binding = *binding;
    value.master_session_generation = 3U;
    value.lock_sample_count = 2U;
    value.sync_timeout_us = 1000U;
    value.max_holdover_us = 2000U;
    value.max_offset_jump_us = 500U;
    value.oscillator_uncertainty_ppb = 1000U;
    value.timer_resolution_bound_us = 1U;
    value.filter_residual_bound_us = 1U;
    value.arithmetic_rounding_bound_us = 1U;
    value.sample_capture_bound_us = 1U;
    return value;
}

static ucn_v6_realtime_domain_record_t domain_record(
    const ucn_v6_time_domain_config_t *config,
    const ucn_v6_path_capability_t *path,
    const ucn_v6_cached_peer_capability_t *capability)
{
    ucn_v6_realtime_domain_record_t value;
    ucn_v6_principal_t local_principal = principal(0x01U);
    (void)capability;
    memset(&value, 0, sizeof(value));
    value.config = *config;
    value.route_ref.domain.origin_principal = local_principal;
    value.route_ref.domain.origin_binding.realm_id = 1U;
    value.route_ref.domain.origin_binding.node_address = 1U;
    value.route_ref.domain.origin_binding.binding_generation = 1U;
    value.route_ref.domain.origin_session_generation = 1U;
    value.route_ref.domain.destination_principal =
        path->destination_principal;
    value.route_ref.domain.destination_binding = path->destination_binding;
    value.route_ref.domain.destination_session_generation =
        path->destination_session_generation;
    value.route_ref.route_generation = path->route_generation;
    value.route_ref.path_id = path->path_id;
    value.route_ref.path_generation = path->path_generation;
    value.route_dependency.type = UCN_V6_STACK_INVALIDATE_PATH;
    value.route_dependency.link_id = path->local_parent_link_id;
    value.route_dependency.link_generation =
        path->local_parent_link_generation;
    value.route_dependency.session = path->local_parent_session;
    value.route_dependency.capability_generation =
        path->local_parent_capability_generation;
    value.route_dependency.path_id = path->path_id;
    value.route_dependency.path_generation = path->path_generation;
    value.path_proof.destination_capability_generation =
        path->destination_capability_generation;
    memcpy(value.path_proof.destination_capability_digest,
           path->destination_capability_digest,
           UCN_V6_CAPABILITY_DIGEST_BYTES);
    memcpy(value.path_proof.local_parent_capability_digest,
           path->local_parent_capability_digest,
           UCN_V6_CAPABILITY_DIGEST_BYTES);
    value.path_proof.destination_realtime_mode_bits =
        path->destination_realtime_mode_bits;
    value.path_proof.destination_clock_domain_id =
        path->destination_clock_domain_id;
    value.path_proof.destination_clock_domain_generation =
        path->destination_clock_domain_generation;
    value.path_proof.feature_bits = path->feature_bits;
    value.path_proof.timestamp_capability_bits =
        path->timestamp_capability_bits;
    value.path_proof.timestamp_uncertainty_us =
        path->timestamp_uncertainty_us;
    return value;
}

static ucn_v6_realtime_uncertainty_t uncertainty(void)
{
    ucn_v6_realtime_uncertainty_t value;
    memset(&value, 0, sizeof(value));
    value.timer_resolution_bound_us = 1U;
    value.link_timestamp_capture_bound_us = 1U;
    value.filter_residual_bound_us = 1U;
    value.arithmetic_rounding_bound_us = 1U;
    value.sample_capture_bound_us = 1U;
    value.path_asymmetry_bound_us = 1U;
    value.known_mask = UCN_V6_REALTIME_KN_ALL;
    return value;
}

static ucn_v6_security_open_result_t opened_sample(
    const ucn_v6_principal_t *master,
    const ucn_v6_binding_key_t *binding)
{
    ucn_v6_security_open_result_t value;
    memset(&value, 0, sizeof(value));
    value.authenticated_principal = *master;
    value.hop_authenticated = true;
    value.endpoint_authorized = true;
    value.frame.flags = UCN_V6_FLAG_E2E_CONTEXT |
                        UCN_V6_FLAG_PROTOCOL_CONTEXT |
                        UCN_V6_FLAG_ROUTE_CONTEXT |
                        UCN_V6_FLAG_PATH_CONTEXT |
                        UCN_V6_FLAG_MESSAGE_CONTEXT;
    value.frame.realm_id = binding->realm_id;
    value.frame.source_address = binding->node_address;
    value.frame.frame_type = UCN_V6_FRAME_CONTROL;
    value.frame.protocol_opcode = UCN_V6_PROTOCOL_OPCODE_TIME_DELAY_RESPONSE;
    value.frame.source_binding_generation = binding->binding_generation;
    value.frame.session_generation = 3U;
    value.frame.route_generation = 1U;
    value.frame.path.path_id = 2U;
    value.frame.path.path_generation = 4U;
    return value;
}

typedef struct realtime_test_owner_binding {
    ucn_v6_realtime_owner_t *realtime;
    fake_store_t *store;
} realtime_test_owner_binding_t;

static realtime_test_owner_binding_t realtime_test_owners[16];

static ucn_v6_result_t ensure_test_capability_owner(fake_store_t *store)
{
    ucn_v6_principal_t local_principal = principal(0x01U);
    ucn_v6_binding_key_t local_binding = { 1U, 1U, 1U };
    ucn_v6_cached_peer_capability_t local = master_capability(
        &local_principal, &local_binding, 1U);
    ucn_v6_result_t result;
    if (store->capability_owner != NULL && store->route_owner != NULL) {
        return UCN_V6_OK;
    }
    result = ucn_v6_capability_owner_init_in_place(
        store->capability_storage.bytes, sizeof(store->capability_storage),
        ucn_v6_compiled_manifest(), &local.record,
        UINT64_C(1000000000), UINT64_C(1000000000),
        &store->capability_owner);
    if (result != UCN_V6_OK) {
        return result;
    }
    return ucn_v6_route_owner_init_in_place(
        store->route_storage.bytes, sizeof(store->route_storage),
        ucn_v6_compiled_manifest(), store->capability_owner,
        1000U, 100U, 4U, 100U, 100U, &store->route_owner);
}

static ucn_v6_result_t test_realtime_owner_init(
    void *storage, size_t storage_bytes,
    const ucn_v6_feature_manifest_t *manifest,
    const ucn_v6_realtime_generation_store_ops_t *generation_store,
    ucn_v6_callback_gate_t *callback_gate,
    ucn_v6_realtime_owner_t **owner)
{
    fake_store_t *store;
    size_t index;
    ucn_v6_result_t result;
    if (generation_store == NULL || generation_store->context == NULL) {
        return UCN_V6_ERR_CONFIG;
    }
    store = (fake_store_t *)generation_store->context;
    result = ensure_test_capability_owner(store);
    if (result != UCN_V6_OK) {
        return result;
    }
    result = ucn_v6_realtime_owner_init_in_place(
        storage, storage_bytes, manifest, store->route_owner,
        generation_store, callback_gate, owner);
    if (result != UCN_V6_OK) {
        return result;
    }
    for (index = 0U; index <
             sizeof(realtime_test_owners) / sizeof(realtime_test_owners[0]);
         ++index) {
        if (realtime_test_owners[index].realtime == NULL ||
            realtime_test_owners[index].realtime == *owner ||
            realtime_test_owners[index].store == store) {
            realtime_test_owners[index].realtime = *owner;
            realtime_test_owners[index].store = store;
            return UCN_V6_OK;
        }
    }
    return UCN_V6_ERR_NO_SPACE;
}

static fake_store_t *test_store_for_owner(ucn_v6_realtime_owner_t *owner)
{
    size_t index;
    for (index = 0U; index <
             sizeof(realtime_test_owners) / sizeof(realtime_test_owners[0]);
         ++index) {
        if (realtime_test_owners[index].realtime == owner) {
            return realtime_test_owners[index].store;
        }
    }
    return NULL;
}

static ucn_v6_result_t test_install_realtime_dependency(
    fake_store_t *store,
    const ucn_v6_cached_peer_capability_t *capability,
    const ucn_v6_path_capability_t *path,
    uint64_t now_us)
{
    ucn_v6_security_open_result_t authenticated;
    ucn_v6_cached_peer_capability_t advertised;
    ucn_v6_path_capability_t installed_path;
    ucn_v6_route_domain_t route_domain;
    ucn_v6_route_path_t route_path;
    ucn_v6_route_activation_t activation;
    ucn_v6_principal_t local_principal = principal(0x01U);
    uint8_t payload[UCN_V6_CAPABILITY_RECORD_BYTES];
    if (store == NULL || store->capability_owner == NULL ||
        store->route_owner == NULL || capability == NULL || path == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    advertised = *capability;
    installed_path = *path;
    if (ucn_v6_capability_record_encode(&advertised.record, payload) !=
        UCN_V6_OK) {
        return UCN_V6_ERR_ARGUMENT;
    }
    memset(&authenticated, 0, sizeof(authenticated));
    authenticated.authenticated_principal = advertised.principal;
    authenticated.ingress_peer_session.principal = advertised.principal;
    authenticated.ingress_peer_session.binding = advertised.binding;
    authenticated.ingress_peer_session.session_generation =
        advertised.session_generation;
    authenticated.ingress_link_instance_id = advertised.ingress_link_id;
    authenticated.ingress_link_instance_generation =
        advertised.ingress_link_generation;
    authenticated.hop_authenticated = true;
    authenticated.frame.frame_type = UCN_V6_FRAME_CONTROL;
    authenticated.frame.flags = UCN_V6_FLAG_PEER_HOP_CONTEXT |
                                UCN_V6_FLAG_PROTOCOL_CONTEXT;
    authenticated.frame.realm_id = advertised.binding.realm_id;
    authenticated.frame.source_address = advertised.binding.node_address;
    authenticated.frame.source_binding_generation =
        advertised.binding.binding_generation;
    authenticated.frame.session_generation = advertised.session_generation;
    authenticated.frame.protocol_opcode =
        UCN_V6_PROTOCOL_OPCODE_CAPABILITY_ADVERTISE;
    authenticated.frame.payload = payload;
    authenticated.frame.payload_length = sizeof(payload);
    {
        ucn_v6_result_t result = ucn_v6_capability_ingest_advertise(
            store->capability_owner, now_us, &authenticated,
            &advertised.record);
        if (result != UCN_V6_OK) {
            return result;
        }
    }
    {
        ucn_v6_result_t result = ucn_v6_capability_install_path(
            store->capability_owner, now_us, &installed_path);
        if (result != UCN_V6_OK) {
            return result;
        }
    }
    memset(&route_domain, 0, sizeof(route_domain));
    route_domain.origin_principal = local_principal;
    route_domain.origin_binding.realm_id = 1U;
    route_domain.origin_binding.node_address = 1U;
    route_domain.origin_binding.binding_generation = 1U;
    route_domain.origin_session_generation = 1U;
    route_domain.destination_principal = path->destination_principal;
    route_domain.destination_binding = path->destination_binding;
    route_domain.destination_session_generation =
        path->destination_session_generation;
    memset(&route_path, 0, sizeof(route_path));
    route_path.path_id = path->path_id;
    route_path.path_generation = path->path_generation;
    route_path.next_hop = path->local_parent_session;
    route_path.egress_link_id = path->local_parent_link_id;
    route_path.egress_link_generation = path->local_parent_link_generation;
    route_path.next_hop_capability_generation =
        path->local_parent_capability_generation;
    route_path.hop_count = path->hop_count;
    route_path.priority = 1U;
    route_path.weight = 1U;
    route_path.available = true;
    route_path.capability = *path;
    {
        ucn_v6_route_path_ref_t route_ref;
        ucn_v6_route_resolution_t resolution;
        memset(&route_ref, 0, sizeof(route_ref));
        route_ref.domain = route_domain;
        route_ref.route_generation = path->route_generation;
        route_ref.path_id = path->path_id;
        route_ref.path_generation = path->path_generation;
        if (ucn_v6_route_resolve_ref(
                store->route_owner, now_us, &route_ref,
                &resolution) == UCN_V6_OK) {
            return UCN_V6_OK;
        }
    }
    memset(&route_path, 0, sizeof(route_path));
    route_path.path_id = path->path_id;
    route_path.path_generation = path->path_generation;
    route_path.next_hop = path->local_parent_session;
    route_path.egress_link_id = path->local_parent_link_id;
    route_path.egress_link_generation = path->local_parent_link_generation;
    route_path.next_hop_capability_generation =
        path->local_parent_capability_generation;
    route_path.hop_count = path->hop_count;
    route_path.priority = 1U;
    route_path.weight = 1U;
    route_path.available = true;
    route_path.capability = *path;
    if (ucn_v6_route_candidate_begin(
            store->route_owner, now_us,
            ((uint64_t)path->route_generation << 32U) |
                path->path_generation,
            &route_domain, path->route_generation) != UCN_V6_OK ||
        ucn_v6_route_candidate_add_path(
            store->route_owner, now_us,
            ((uint64_t)path->route_generation << 32U) |
                path->path_generation,
            &route_domain, &route_path) != UCN_V6_OK ||
        ucn_v6_route_candidate_record_probe(
            store->route_owner, now_us,
            ((uint64_t)path->route_generation << 32U) |
                path->path_generation,
            &route_domain, path->path_id,
            path->path_generation) != UCN_V6_OK ||
        ucn_v6_route_candidate_prepare_activation(
            store->route_owner, now_us,
            ((uint64_t)path->route_generation << 32U) |
                path->path_generation,
            &route_domain, &activation) != UCN_V6_OK ||
        ucn_v6_route_candidate_record_activation_send(
            store->route_owner, now_us,
            ((uint64_t)path->route_generation << 32U) |
                path->path_generation,
            &route_domain, true) != UCN_V6_OK) {
        return UCN_V6_ERR_STATE;
    }
    return ucn_v6_route_candidate_commit_ack(store->route_owner, now_us,
                                              &activation);
}

static ucn_v6_result_t test_retire_realtime_session(
    fake_store_t *store,
    const ucn_v6_cached_peer_capability_t *capability,
    ucn_v6_realtime_owner_t *realtime)
{
    ucn_v6_stack_invalidation_t invalidation;
    ucn_v6_result_t result;
    if (store == NULL || store->capability_owner == NULL ||
        capability == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    memset(&invalidation, 0, sizeof(invalidation));
    invalidation.type = UCN_V6_STACK_INVALIDATE_SESSION;
    invalidation.link_id = capability->ingress_link_id;
    invalidation.link_generation = capability->ingress_link_generation;
    invalidation.session.principal = capability->principal;
    invalidation.session.binding = capability->binding;
    invalidation.session.session_generation =
        capability->session_generation;
    result = ucn_v6_capability_apply_invalidation(
        store->capability_owner, &invalidation);
    if (result != UCN_V6_OK || realtime == NULL) {
        return result;
    }
    return ucn_v6_realtime_apply_invalidation(realtime, &invalidation);
}

static ucn_v6_result_t test_realtime_bind_domain(
    ucn_v6_realtime_owner_t *owner,
    const ucn_v6_time_domain_config_t *config,
    const ucn_v6_path_capability_t *fixed_path,
    const ucn_v6_cached_peer_capability_t *master_capability,
    uint64_t now_us)
{
    fake_store_t *store = test_store_for_owner(owner);
    ucn_v6_route_path_ref_t path_ref;
    ucn_v6_principal_t local_principal = principal(0x01U);
    memset(&path_ref, 0, sizeof(path_ref));
    if (fixed_path == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    if (store != NULL && fixed_path != NULL && fixed_path->valid &&
        fixed_path->immutable_for_realtime) {
        ucn_v6_result_t result = test_install_realtime_dependency(
            store, master_capability, fixed_path, now_us);
        if (result != UCN_V6_OK) {
            return result;
        }
    }
    path_ref.domain.origin_principal = local_principal;
    path_ref.domain.origin_binding.realm_id = 1U;
    path_ref.domain.origin_binding.node_address = 1U;
    path_ref.domain.origin_binding.binding_generation = 1U;
    path_ref.domain.origin_session_generation = 1U;
    path_ref.domain.destination_principal = fixed_path->destination_principal;
    path_ref.domain.destination_binding = fixed_path->destination_binding;
    path_ref.domain.destination_session_generation =
        fixed_path->destination_session_generation;
    path_ref.route_generation = fixed_path->route_generation;
    path_ref.path_id = fixed_path->path_id;
    path_ref.path_generation = fixed_path->path_generation;
    return ucn_v6_realtime_bind_domain(owner, config, &path_ref, now_us);
}

#define ucn_v6_realtime_owner_init_in_place(storage, storage_bytes, manifest, \
                                             generation_store, gate, owner)  \
    test_realtime_owner_init((storage), (storage_bytes), (manifest),          \
                             (generation_store), (gate), (owner))
#define ucn_v6_realtime_bind_domain(owner, config, path, capability, now_us) \
    test_realtime_bind_domain((owner), (config), (path), (capability),        \
                              (now_us))

static ucn_v6_result_t ingest_verified_exchange(
    ucn_v6_realtime_owner_t *owner,
    const ucn_v6_security_open_result_t *opened,
    const synthetic_exchange_t *sample,
    uint16_t opcode,
    uint64_t now_us)
{
    fake_store_t *store = test_store_for_owner(owner);
    ucn_v6_security_open_result_t response_opened;
    ucn_v6_time_sync_response_t response;
    ucn_v6_time_sync_observation_t observation;
    uint8_t payload[UCN_V6_TIME_SYNC_RESPONSE_BYTES];
    uint64_t magnitude;
    if (store == NULL || !store->valid || opened == NULL || sample == NULL ||
        sample->local_sample_us == 0U ||
        sample->local_sample_us > UCN_V6_SERIAL_ROTATION_THRESHOLD) {
        return UCN_V6_ERR_ARGUMENT;
    }
    memset(&response, 0, sizeof(response));
    response.clock_domain_id = sample->clock_domain_id;
    response.domain_generation = sample->domain_generation;
    response.sync_sequence = (uint32_t)sample->local_sample_us;
    response.t1_uncertainty_us =
        sample->uncertainty.link_timestamp_capture_bound_us;
    response.t4_uncertainty_us = response.t1_uncertainty_us;
    if (sample->offset_us >= 0) {
        magnitude = (uint64_t)sample->offset_us;
        if (UINT64_MAX - sample->local_sample_us < magnitude) {
            return UCN_V6_ERR_EXHAUSTED;
        }
        response.t1_master_tx_us = sample->local_sample_us + magnitude;
        response.t4_master_rx_us = response.t1_master_tx_us;
    } else {
        magnitude = (uint64_t)(-(sample->offset_us + 1)) + 1U;
        if (sample->local_sample_us <= magnitude) {
            return UCN_V6_ERR_ARGUMENT;
        }
        response.t1_master_tx_us = sample->local_sample_us - magnitude;
        response.t4_master_rx_us = response.t1_master_tx_us;
    }
    if (ucn_v6_time_sync_response_encode(&response, payload) != UCN_V6_OK) {
        return UCN_V6_ERR_ARGUMENT;
    }
    memset(&observation, 0, sizeof(observation));
    observation.sync_sequence = response.sync_sequence;
    observation.forward_route_ref = store->record.route_ref;
    observation.forward_route_ref.domain.origin_principal =
        store->record.route_ref.domain.destination_principal;
    observation.forward_route_ref.domain.origin_binding =
        store->record.route_ref.domain.destination_binding;
    observation.forward_route_ref.domain.origin_session_generation =
        store->record.route_ref.domain.destination_session_generation;
    observation.forward_route_ref.domain.destination_principal =
        store->record.route_ref.domain.origin_principal;
    observation.forward_route_ref.domain.destination_binding =
        store->record.route_ref.domain.origin_binding;
    observation.forward_route_ref.domain.destination_session_generation =
        store->record.route_ref.domain.origin_session_generation;
    observation.reverse_route_ref = store->record.route_ref;
    observation.t2_member_rx.link_id = 5U;
    observation.t2_member_rx.link_generation = 6U;
    observation.t2_member_rx.event_token = response.sync_sequence * 2U;
    observation.t2_member_rx.timestamp_us = sample->local_sample_us;
    observation.t2_member_rx.uncertainty_us = 1U;
    observation.t2_member_rx.hardware = true;
    observation.t3_member_tx = observation.t2_member_rx;
    ++observation.t3_member_tx.event_token;
    response_opened = *opened;
    response_opened.ingress_link_instance_id = 5U;
    response_opened.ingress_link_instance_generation = 6U;
    response_opened.frame.protocol_opcode = opcode;
    response_opened.frame.payload = payload;
    response_opened.frame.payload_length = sizeof(payload);
    return ucn_v6_realtime_ingest_exchange(
        owner, &response_opened, &observation, now_us);
}

static int test_codec_and_uncertainty(void)
{
    ucn_v6_realtime_envelope_t envelope;
    ucn_v6_realtime_envelope_t decoded;
    ucn_v6_realtime_uncertainty_t components = uncertainty();
    uint8_t bytes[UCN_V6_REALTIME_ENVELOPE_BYTES];
    uint8_t before[UCN_V6_REALTIME_ENVELOPE_BYTES];
    uint32_t bound = 0U;
    ucn_v6_time_sync_response_t response;
    ucn_v6_time_sync_response_t response_decoded;
    ucn_v6_time_sync_announce_t announce;
    ucn_v6_time_sync_announce_t announce_decoded;
    uint8_t announce_bytes[UCN_V6_TIME_SYNC_ANNOUNCE_BYTES];
    uint8_t response_bytes[UCN_V6_TIME_SYNC_RESPONSE_BYTES];
    memset(&envelope, 0, sizeof(envelope));
    envelope.mode = UCN_V6_REALTIME_DEADLINE;
    envelope.uncertainty_class = 4U;
    envelope.sample_capture_hardware = true;
    envelope.domain_time_valid = true;
    envelope.clock_domain_id = 7U;
    envelope.domain_generation = 2U;
    envelope.capture_time_us = 1234U;
    CHECK(ucn_v6_realtime_envelope_encode(&envelope, bytes) == UCN_V6_OK);
    CHECK(ucn_v6_realtime_envelope_decode(bytes, sizeof(bytes), &decoded) ==
          UCN_V6_OK);
    CHECK(memcmp(&decoded, &envelope, sizeof(envelope)) == 0);
    CHECK(ucn_v6_realtime_uncertainty_aggregate(&components, &bound) ==
          UCN_V6_OK && bound == 6U);
    components.sample_capture_bound_us = 0U;
    CHECK(ucn_v6_realtime_uncertainty_aggregate(&components, &bound) ==
          UCN_V6_ERR_ARGUMENT);
    memset(bytes, 0xA5, sizeof(bytes));
    memcpy(before, bytes, sizeof(before));
    envelope.domain_generation = 0U;
    CHECK(ucn_v6_realtime_envelope_encode(&envelope, bytes) ==
          UCN_V6_ERR_ARGUMENT);
    CHECK(memcmp(bytes, before, sizeof(bytes)) == 0);
    envelope.domain_generation = 2U;
    envelope.mode = (ucn_v6_realtime_mode_t)-1;
    CHECK(ucn_v6_realtime_envelope_encode(&envelope, bytes) ==
          UCN_V6_ERR_ARGUMENT);
    CHECK(memcmp(bytes, before, sizeof(bytes)) == 0);
    memset(&announce, 0, sizeof(announce));
    announce.clock_domain_id = 7U;
    announce.domain_generation = 2U;
    announce.sync_sequence = 3U;
    CHECK(ucn_v6_time_sync_announce_encode(&announce, announce_bytes) ==
          UCN_V6_OK);
    CHECK(ucn_v6_time_sync_announce_decode(
              announce_bytes, sizeof(announce_bytes), &announce_decoded) ==
          UCN_V6_OK);
    CHECK(announce_decoded.clock_domain_id == announce.clock_domain_id &&
          announce_decoded.domain_generation == announce.domain_generation &&
          announce_decoded.sync_sequence == announce.sync_sequence);
    announce_bytes[1] = 1U;
    memset(&announce_decoded, 0xA5, sizeof(announce_decoded));
    CHECK(ucn_v6_time_sync_announce_decode(
              announce_bytes, sizeof(announce_bytes), &announce_decoded) ==
          UCN_V6_ERR_MALFORMED);
    {
        ucn_v6_time_sync_announce_t sentinel;
        memset(&sentinel, 0xA5, sizeof(sentinel));
        CHECK(memcmp(&announce_decoded, &sentinel, sizeof(sentinel)) == 0);
    }
    memset(&response, 0, sizeof(response));
    response.clock_domain_id = 7U;
    response.domain_generation = 2U;
    response.sync_sequence = 3U;
    response.t1_master_tx_us = 123U;
    response.t4_master_rx_us = 456U;
    response.t1_uncertainty_us = 2U;
    response.t4_uncertainty_us = 3U;
    CHECK(ucn_v6_time_sync_response_encode(&response, response_bytes) ==
          UCN_V6_OK);
    CHECK(ucn_v6_time_sync_response_decode(
              response_bytes, sizeof(response_bytes), &response_decoded) ==
          UCN_V6_OK);
    CHECK(memcmp(&response, &response_decoded, sizeof(response)) == 0);
    response_bytes[39] = 1U;
    CHECK(ucn_v6_time_sync_response_decode(
              response_bytes, sizeof(response_bytes), &response_decoded) ==
          UCN_V6_ERR_MALFORMED);
    return 0;
}

static int test_fixed_path_domain_and_dual_gate(void)
{
    ucn_v6_realtime_owner_storage_t storage;
    ucn_v6_realtime_owner_t *owner = NULL;
    fake_store_t fake;
    ucn_v6_realtime_generation_store_ops_t store;
    ucn_v6_callback_gate_t gate = {0};
    ucn_v6_principal_t master = principal(0x20U);
    ucn_v6_binding_key_t binding = { 1U, 9U, 2U };
    ucn_v6_cached_peer_capability_t capability =
        master_capability(&master, &binding, 2U);
    ucn_v6_path_capability_t path =
        fixed_path(&master, &binding, &capability);
    ucn_v6_time_domain_config_t config =
        domain_config(&master, &binding, 2U);
    ucn_v6_security_open_result_t opened = opened_sample(&master, &binding);
    ucn_v6_security_open_result_t bad_source;
    synthetic_exchange_t sample;
    ucn_v6_realtime_endpoint_policy_t policy;
    ucn_v6_realtime_send_result_t send;
    ucn_v6_realtime_receive_view_t receive;
    ucn_v6_realtime_clock_view_t clock;
    ucn_v6_realtime_view_t owner_view;
    ucn_v6_realtime_view_t owner_view_before;
    ucn_v6_stack_invalidation_t invalidation;
    const uint8_t business[] = { 1U, 2U, 3U, 4U };
    const uint8_t *admitted = NULL;
    size_t admitted_length = 0U;
    uint8_t payload[64];
    uint8_t sentinel[64];

    opened.ingress_peer_session.principal = principal(0x70U);
    opened.ingress_peer_session.binding.realm_id = 1U;
    opened.ingress_peer_session.binding.node_address = 70U;
    opened.ingress_peer_session.binding.binding_generation = 1U;
    opened.ingress_peer_session.session_generation = 7U;

    memset(&fake, 0, sizeof(fake));
    store.context = &fake;
    store.load_domain_record = load_domain_record;
    store.reserve_domain_record = reserve_domain_record;
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, gate_lock, gate_unlock) ==
          UCN_V6_OK);
    CHECK(ucn_v6_realtime_owner_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              &store, &gate, &owner) == UCN_V6_OK);
    path.immutable_for_realtime = false;
    CHECK(ucn_v6_realtime_bind_domain(owner, &config, &path, &capability, 0U) ==
          UCN_V6_ERR_ACCESS);
    CHECK(fake.loads == 0U && fake.reserves == 0U);
    path.immutable_for_realtime = true;
    fake.fail_reserve = true;
    CHECK(ucn_v6_realtime_bind_domain(owner, &config, &path, &capability, 0U) ==
          UCN_V6_ERR_STATE);
    CHECK(fake.reserves == 1U);
    fake.fail_reserve = false;
    fake.false_success = true;
    CHECK(ucn_v6_realtime_bind_domain(owner, &config, &path, &capability, 0U) ==
          UCN_V6_ERR_STATE);
    fake.false_success = false;
    CHECK(ucn_v6_realtime_bind_domain(owner, &config, &path, &capability, 0U) ==
          UCN_V6_OK);
    CHECK(fake.record.config.domain_generation == 2U &&
          fake.reserves == 3U && fake.loads == 5U);

    memset(&sample, 0, sizeof(sample));
    sample.clock_domain_id = 7U;
    sample.domain_generation = 2U;
    sample.uncertainty = uncertainty();
    sample.local_sample_us = 100U;
    sample.offset_us = 1000;
    CHECK(ucn_v6_realtime_copy_view(owner, &owner_view_before) == UCN_V6_OK);
    CHECK(ingest_verified_exchange(
              owner, &opened, &sample, UCN_V6_PROTOCOL_OPCODE_PEER_HELLO,
              100U) ==
          UCN_V6_ERR_ARGUMENT);
    CHECK(ucn_v6_realtime_copy_view(owner, &owner_view) == UCN_V6_OK);
    CHECK(owner_view.domains == owner_view_before.domains &&
          owner_view.locked_domains == owner_view_before.locked_domains &&
          owner_view.accepted_samples == owner_view_before.accepted_samples &&
          owner_view.rejected_samples == owner_view_before.rejected_samples &&
          owner_view.faulted == owner_view_before.faulted);
    bad_source = opened;
    bad_source.frame.realm_id = binding.realm_id + 1U;
    CHECK(ingest_verified_exchange(
              owner, &bad_source, &sample,
              UCN_V6_PROTOCOL_OPCODE_TIME_DELAY_RESPONSE, 100U) ==
          UCN_V6_ERR_ACCESS);
    CHECK(ingest_verified_exchange(
              owner, &opened, &sample,
              UCN_V6_PROTOCOL_OPCODE_TIME_DELAY_RESPONSE, 100U) == UCN_V6_OK);
    CHECK(ucn_v6_realtime_get_clock(owner, 7U, 100U, &clock) ==
          UCN_V6_ERR_STATE);
    sample.local_sample_us = 200U;
    CHECK(ingest_verified_exchange(
              owner, &opened, &sample,
              UCN_V6_PROTOCOL_OPCODE_TIME_DELAY_RESPONSE, 200U) == UCN_V6_OK);
    CHECK(ucn_v6_realtime_get_clock(owner, 7U, 250U, &clock) == UCN_V6_OK);
    CHECK(clock.domain_time_us == 1250U && clock.uncertainty_us == 17U);

    memset(&policy, 0, sizeof(policy));
    policy.destination_endpoint = 20U;
    policy.mode = UCN_V6_REALTIME_DEADLINE;
    policy.requirement = UCN_V6_REALTIME_REQUIRED;
    policy.clock_domain_id = 7U;
    policy.max_age_us = 200U;
    policy.max_uncertainty_us = 64U;
    policy.max_local_holdover_us = 500U;
    policy.require_hardware_capture = true;
    {
        ucn_v6_realtime_endpoint_policy_t invalid = policy;
        invalid.mode = (ucn_v6_realtime_mode_t)-1;
        CHECK(ucn_v6_realtime_set_endpoint_policy(owner, &invalid) ==
              UCN_V6_ERR_ARGUMENT);
        invalid = policy;
        invalid.requirement = (ucn_v6_realtime_requirement_t)-1;
        CHECK(ucn_v6_realtime_set_endpoint_policy(owner, &invalid) ==
              UCN_V6_ERR_ARGUMENT);
        invalid = policy;
        invalid.destination_endpoint = UINT16_MAX;
        CHECK(ucn_v6_realtime_set_endpoint_policy(owner, &invalid) ==
              UCN_V6_ERR_ARGUMENT);
    }
    CHECK(ucn_v6_realtime_set_endpoint_policy(owner, &policy) == UCN_V6_OK);
    CHECK(ucn_v6_realtime_prepare_payload(
              owner, 20U, 260U, 4U, true, business, sizeof(business),
              payload, sizeof(payload), &send) == UCN_V6_OK);
    CHECK(send.payload_length == 20U && send.business_offset == 16U);
    opened.frame.message.destination_endpoint = 20U;
    opened.frame.payload = payload;
    opened.frame.payload_length = (uint16_t)send.payload_length;
    CHECK(ucn_v6_realtime_receive_admit(owner, 300U, &opened, &receive) ==
          UCN_V6_OK && receive.accepted);
    CHECK(ucn_v6_realtime_execution_admit(
              owner, 301U, &opened, &receive, &admitted,
              &admitted_length) == UCN_V6_OK);
    CHECK(admitted_length == sizeof(business) &&
          memcmp(admitted, business, sizeof(business)) == 0);

    opened.endpoint_authorized = false;
    CHECK(ucn_v6_realtime_receive_admit(owner, 302U, &opened, &receive) ==
          UCN_V6_OK && !receive.accepted &&
          receive.reason == UCN_V6_REALTIME_REJECT_SECURITY);
    opened.endpoint_authorized = true;
    CHECK(ucn_v6_realtime_receive_admit(owner, 500U, &opened, &receive) ==
          UCN_V6_OK && !receive.accepted &&
          receive.reason == UCN_V6_REALTIME_REJECT_EXPIRED);

    memset(sentinel, 0x5A, sizeof(sentinel));
    memcpy(payload, sentinel, sizeof(payload));
    CHECK(ucn_v6_realtime_prepare_payload(
              owner, 20U, 1200U, 4U, false, business, sizeof(business),
              payload, sizeof(payload), &send) == UCN_V6_ERR_ACCESS);
    CHECK(memcmp(payload, sentinel, sizeof(payload)) == 0);
    CHECK(ucn_v6_realtime_copy_view(owner, &owner_view) == UCN_V6_OK);
    CHECK(owner_view.domains == 1U && owner_view.locked_domains == 1U &&
          owner_view.accepted_samples == 2U &&
          owner_view.rejected_messages >= 2U);
    memset(&invalidation, 0, sizeof(invalidation));
    invalidation.type = UCN_V6_STACK_INVALIDATE_PATH;
    invalidation.link_id = capability.ingress_link_id;
    invalidation.link_generation = capability.ingress_link_generation;
    invalidation.session.principal = master;
    invalidation.session.binding = binding;
    invalidation.session.session_generation = config.master_session_generation;
    invalidation.capability_generation =
        capability.record.capability_generation;
    invalidation.path_id = path.path_id;
    invalidation.path_generation = path.path_generation;
    CHECK(ucn_v6_realtime_apply_invalidation(owner, &invalidation) ==
          UCN_V6_OK);
    CHECK(ucn_v6_realtime_copy_view(owner, &owner_view) == UCN_V6_OK &&
          owner_view.locked_domains == 0U);
    memset(sentinel, 0x5A, sizeof(sentinel));
    memcpy(payload, sentinel, sizeof(payload));
    CHECK(ucn_v6_realtime_prepare_payload(
              owner, 20U, 1201U, 4U, false, business, sizeof(business),
              payload, sizeof(payload), &send) == UCN_V6_ERR_ACCESS);
    CHECK(memcmp(payload, sentinel, sizeof(payload)) == 0);
    CHECK(ingest_verified_exchange(
              owner, &opened, &sample,
              UCN_V6_PROTOCOL_OPCODE_TIME_DELAY_RESPONSE, 1201U) ==
          UCN_V6_ERR_ACCESS);

    ++config.domain_generation;
    ++capability.record.capability_generation;
    capability.record.peer.clock_domain_generation = config.domain_generation;
    ++path.route_generation;
    ++path.path_generation;
    refresh_capability_binding(&capability, &path);
    CHECK(ucn_v6_realtime_bind_domain(owner, &config, &path, &capability,
                                      1202U) == UCN_V6_OK);
    return 0;
}

static int test_none_has_zero_overhead_and_generation_replay(void)
{
    ucn_v6_realtime_owner_storage_t storage;
    ucn_v6_realtime_owner_t *owner = NULL;
    fake_store_t fake;
    ucn_v6_realtime_generation_store_ops_t store;
    ucn_v6_callback_gate_t gate = {0};
    ucn_v6_realtime_endpoint_policy_t policy;
    ucn_v6_realtime_send_result_t result;
    ucn_v6_principal_t master = principal(0x50U);
    ucn_v6_binding_key_t binding = { 1U, 8U, 1U };
    ucn_v6_cached_peer_capability_t capability =
        master_capability(&master, &binding, 1U);
    ucn_v6_path_capability_t path =
        fixed_path(&master, &binding, &capability);
    ucn_v6_time_domain_config_t config =
        domain_config(&master, &binding, 1U);
    const uint8_t business[] = { 9U, 8U, 7U };
    uint8_t output[8];
    memset(&fake, 0, sizeof(fake));
    fake.valid = true;
    fake.record = domain_record(&config, &path, &capability);
    fake.record.config.domain_generation = 2U;
    fake.record.path_proof.destination_clock_domain_generation = 2U;
    store.context = &fake;
    store.load_domain_record = load_domain_record;
    store.reserve_domain_record = reserve_domain_record;
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, gate_lock, gate_unlock) ==
          UCN_V6_OK);
    CHECK(ucn_v6_realtime_owner_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              &store, &gate, &owner) == UCN_V6_OK);
    CHECK(ucn_v6_realtime_bind_domain(owner, &config, &path, &capability, 0U) ==
          UCN_V6_ERR_REPLAY);
    memset(&policy, 0, sizeof(policy));
    policy.destination_endpoint = 30U;
    policy.mode = UCN_V6_REALTIME_NONE;
    policy.requirement = UCN_V6_REALTIME_DISABLED;
    CHECK(ucn_v6_realtime_set_endpoint_policy(owner, &policy) == UCN_V6_OK);
    CHECK(ucn_v6_realtime_prepare_payload(
              owner, 30U, 1U, 0U, false, business, sizeof(business),
              output, sizeof(output), &result) == UCN_V6_OK);
    CHECK(result.payload_length == sizeof(business) &&
          result.business_offset == 0U &&
          memcmp(output, business, sizeof(business)) == 0);
    return 0;
}

static int test_generation_store_callback_scope_is_fail_closed(void)
{
    ucn_v6_realtime_owner_storage_t storage;
    ucn_v6_realtime_owner_storage_t leave_storage;
    ucn_v6_realtime_owner_t *owner = NULL;
    ucn_v6_realtime_owner_t *leave_owner = NULL;
    fake_store_t fake;
    fake_store_t leave_fake;
    ucn_v6_realtime_generation_store_ops_t store;
    ucn_v6_realtime_generation_store_ops_t leave_store;
    ucn_v6_callback_gate_t gate = {0};
    ucn_v6_callback_gate_t leave_gate = {0};
    ucn_v6_principal_t master = principal(0x52U);
    ucn_v6_binding_key_t binding = { 1U, 82U, 1U };
    ucn_v6_cached_peer_capability_t capability =
        master_capability(&master, &binding, 1U);
    ucn_v6_path_capability_t path =
        fixed_path(&master, &binding, &capability);
    ucn_v6_time_domain_config_t config =
        domain_config(&master, &binding, 1U);
    ucn_v6_realtime_view_t view;

    memset(&fake, 0, sizeof(fake));
    memset(&store, 0, sizeof(store));
    store.context = &fake;
    store.load_domain_record = load_domain_record;
    store.reserve_domain_record = reserve_domain_record;
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, gate_lock, gate_unlock) ==
          UCN_V6_OK);
    CHECK(ucn_v6_realtime_owner_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              &store, &gate, &owner) == UCN_V6_OK);
    fake.reenter_owner = owner;
    CHECK(ucn_v6_realtime_bind_domain(owner, &config, &path, &capability,
                                      1U) == UCN_V6_ERR_STATE);
    CHECK(fake.reenter_result == UCN_V6_ERR_STATE);
    CHECK(ucn_v6_realtime_copy_view(owner, &view) == UCN_V6_OK &&
          view.domains == 0U && view.locked_domains == 0U && view.faulted);

    memset(&leave_fake, 0, sizeof(leave_fake));
    memset(&leave_store, 0, sizeof(leave_store));
    leave_store.context = &leave_fake;
    leave_store.load_domain_record = load_domain_record;
    leave_store.reserve_domain_record = reserve_domain_record;
    CHECK(ucn_v6_callback_gate_init(&leave_gate, NULL, gate_lock,
                                    gate_unlock) == UCN_V6_OK);
    CHECK(ucn_v6_realtime_owner_init_in_place(
              leave_storage.bytes, sizeof(leave_storage),
              ucn_v6_compiled_manifest(), &leave_store, &leave_gate,
              &leave_owner) == UCN_V6_OK);
    leave_fake.callback_gate = &leave_gate;
    leave_fake.callback_scope_owner = leave_owner;
    leave_fake.force_gate_leave_on_reserve = true;
    CHECK(ucn_v6_realtime_bind_domain(
              leave_owner, &config, &path, &capability, 1U) ==
          UCN_V6_ERR_STATE);
    CHECK(leave_fake.forced_gate_leave_result == UCN_V6_OK);
    CHECK(ucn_v6_realtime_copy_view(leave_owner, &view) == UCN_V6_OK &&
          view.domains == 0U && view.locked_domains == 0U && view.faulted);
    return 0;
}

static int test_restart_requires_fresh_peer_session(void)
{
    ucn_v6_realtime_owner_storage_t first_storage;
    ucn_v6_realtime_owner_storage_t restarted_storage;
    ucn_v6_realtime_owner_t *first = NULL;
    ucn_v6_realtime_owner_t *restarted = NULL;
    fake_store_t fake;
    ucn_v6_realtime_generation_store_ops_t store;
    ucn_v6_callback_gate_t first_gate = {0};
    ucn_v6_callback_gate_t restarted_gate = {0};
    ucn_v6_principal_t master = principal(0x53U);
    ucn_v6_binding_key_t binding = { 1U, 83U, 1U };
    ucn_v6_cached_peer_capability_t capability =
        master_capability(&master, &binding, 2U);
    ucn_v6_path_capability_t path =
        fixed_path(&master, &binding, &capability);
    ucn_v6_time_domain_config_t config =
        domain_config(&master, &binding, 2U);

    memset(&fake, 0, sizeof(fake));
    memset(&store, 0, sizeof(store));
    store.context = &fake;
    store.load_domain_record = load_domain_record;
    store.reserve_domain_record = reserve_domain_record;
    CHECK(ucn_v6_callback_gate_init(&first_gate, NULL, gate_lock,
                                    gate_unlock) == UCN_V6_OK);
    CHECK(ucn_v6_realtime_owner_init_in_place(
              first_storage.bytes, sizeof(first_storage),
              ucn_v6_compiled_manifest(), &store, &first_gate,
              &first) == UCN_V6_OK);
    CHECK(ucn_v6_realtime_bind_domain(first, &config, &path, &capability,
                                      1U) == UCN_V6_OK);

    /* A reboot also reconstructs the Capability cache.  Durable Security
     * state, not this volatile cache, owns Session anti-rollback. */
    fake.capability_owner = NULL;
    fake.route_owner = NULL;
    CHECK(ucn_v6_callback_gate_init(&restarted_gate, NULL, gate_lock,
                                    gate_unlock) == UCN_V6_OK);
    CHECK(ucn_v6_realtime_owner_init_in_place(
              restarted_storage.bytes, sizeof(restarted_storage),
              ucn_v6_compiled_manifest(), &store, &restarted_gate,
              &restarted) == UCN_V6_OK);
    CHECK(ucn_v6_realtime_bind_domain(
              restarted, &config, &path, &capability, 2U) ==
          UCN_V6_ERR_REPLAY);

    CHECK(test_retire_realtime_session(&fake, &capability, restarted) ==
          UCN_V6_OK);
    ++config.master_session_generation;
    ++capability.session_generation;
    capability.record.capability_generation = 1U;
    ++path.destination_session_generation;
    path.route_generation = 1U;
    path.path_generation = 1U;
    refresh_capability_binding(&capability, &path);
    CHECK(ucn_v6_realtime_bind_domain(
              restarted, &config, &path, &capability, 3U) == UCN_V6_OK);
    CHECK(fake.record.config.domain_generation == 2U &&
          fake.record.config.master_session_generation == 4U);
    return 0;
}

static int test_same_domain_session_rebind_preserves_runtime_high_water(void)
{
    ucn_v6_realtime_owner_storage_t storage;
    ucn_v6_realtime_owner_t *owner = NULL;
    fake_store_t fake;
    ucn_v6_realtime_generation_store_ops_t store;
    ucn_v6_callback_gate_t gate = {0};
    ucn_v6_principal_t master = principal(0x59U);
    ucn_v6_binding_key_t binding = { 1U, 89U, 1U };
    ucn_v6_cached_peer_capability_t capability =
        master_capability(&master, &binding, 2U);
    ucn_v6_path_capability_t path =
        fixed_path(&master, &binding, &capability);
    ucn_v6_time_domain_config_t config =
        domain_config(&master, &binding, 2U);
    ucn_v6_security_open_result_t opened = opened_sample(&master, &binding);
    synthetic_exchange_t sample;
    ucn_v6_realtime_clock_view_t clock;
    ucn_v6_realtime_view_t view;

    memset(&fake, 0, sizeof(fake));
    memset(&store, 0, sizeof(store));
    store.context = &fake;
    store.load_domain_record = load_domain_record;
    store.reserve_domain_record = reserve_domain_record;
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, gate_lock, gate_unlock) ==
          UCN_V6_OK);
    CHECK(ucn_v6_realtime_owner_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              &store, &gate, &owner) == UCN_V6_OK);
    CHECK(ucn_v6_realtime_bind_domain(owner, &config, &path, &capability,
                                      1U) == UCN_V6_OK);

    memset(&sample, 0, sizeof(sample));
    sample.clock_domain_id = config.clock_domain_id;
    sample.domain_generation = config.domain_generation;
    sample.uncertainty = uncertainty();
    sample.offset_us = 1000;
    sample.local_sample_us = 100U;
    CHECK(ingest_verified_exchange(
              owner, &opened, &sample,
              UCN_V6_PROTOCOL_OPCODE_TIME_DELAY_RESPONSE, 100U) == UCN_V6_OK);
    sample.local_sample_us = 200U;
    CHECK(ingest_verified_exchange(
              owner, &opened, &sample,
              UCN_V6_PROTOCOL_OPCODE_TIME_DELAY_RESPONSE, 200U) == UCN_V6_OK);
    CHECK(ucn_v6_realtime_get_clock(owner, config.clock_domain_id, 250U,
                                    &clock) == UCN_V6_OK &&
          clock.domain_time_us == 1250U);

    CHECK(test_retire_realtime_session(&fake, &capability, owner) ==
          UCN_V6_OK);
    ++config.master_session_generation;
    ++capability.session_generation;
    capability.record.capability_generation = 1U;
    ++path.destination_session_generation;
    path.route_generation = 1U;
    path.path_generation = 1U;
    refresh_capability_binding(&capability, &path);
    CHECK(ucn_v6_realtime_bind_domain(owner, &config, &path, &capability,
                                      260U) == UCN_V6_OK);

    opened.frame.session_generation = config.master_session_generation;
    opened.frame.route_generation = path.route_generation;
    opened.frame.path.path_generation = path.path_generation;
    sample.local_sample_us = 300U;
    sample.offset_us = 0;
    CHECK(ingest_verified_exchange(
              owner, &opened, &sample,
              UCN_V6_PROTOCOL_OPCODE_TIME_DELAY_RESPONSE, 300U) ==
          UCN_V6_ERR_STATE);
    CHECK(ucn_v6_realtime_copy_view(owner, &view) == UCN_V6_OK &&
          view.faulted && view.locked_domains == 0U);
    return 0;
}

static int test_live_capability_owner_precedes_bind_and_domain_switch(void)
{
    ucn_v6_realtime_owner_storage_t storage;
    ucn_v6_realtime_owner_t *owner = NULL;
    fake_store_t fake;
    ucn_v6_realtime_generation_store_ops_t store;
    ucn_v6_callback_gate_t gate = {0};
    ucn_v6_principal_t master_d1 = principal(0x31U);
    ucn_v6_principal_t master_d2 = principal(0x71U);
    ucn_v6_binding_key_t binding_d1 = { 1U, 31U, 1U };
    ucn_v6_binding_key_t binding_d2 = { 1U, 71U, 1U };
    ucn_v6_cached_peer_capability_t capability_d1 =
        master_capability(&master_d1, &binding_d1, 1U);
    ucn_v6_cached_peer_capability_t capability_d2 =
        master_capability(&master_d2, &binding_d2, 2U);
    ucn_v6_path_capability_t path_d1 =
        fixed_path(&master_d1, &binding_d1, &capability_d1);
    ucn_v6_path_capability_t path_d2 =
        fixed_path(&master_d2, &binding_d2, &capability_d2);
    ucn_v6_time_domain_config_t config_d1 =
        domain_config(&master_d1, &binding_d1, 1U);
    ucn_v6_time_domain_config_t config_d2 =
        domain_config(&master_d2, &binding_d2, 2U);
    ucn_v6_route_path_ref_t path_ref;

    memset(&fake, 0, sizeof(fake));
    memset(&store, 0, sizeof(store));
    store.context = &fake;
    store.load_domain_record = load_domain_record;
    store.reserve_domain_record = reserve_domain_record;
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, gate_lock, gate_unlock) ==
          UCN_V6_OK);
    CHECK(ucn_v6_realtime_owner_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              &store, &gate, &owner) == UCN_V6_OK);

    memset(&path_ref, 0, sizeof(path_ref));
    path_ref.domain.origin_principal = principal(0x01U);
    path_ref.domain.origin_binding.realm_id = 1U;
    path_ref.domain.origin_binding.node_address = 1U;
    path_ref.domain.origin_binding.binding_generation = 1U;
    path_ref.domain.origin_session_generation = 1U;
    path_ref.domain.destination_principal = master_d1;
    path_ref.domain.destination_binding = binding_d1;
    path_ref.domain.destination_session_generation =
        config_d1.master_session_generation;
    path_ref.route_generation = path_d1.route_generation;
    path_ref.path_id = path_d1.path_id;
    path_ref.path_generation = path_d1.path_generation;
    /* A caller-held key cannot create authority before Capability Owner has
     * authenticated and installed the referenced Peer and Path. */
    CHECK((ucn_v6_realtime_bind_domain)(owner, &config_d1, &path_ref, 1U) ==
          UCN_V6_ERR_ACCESS);
    CHECK(fake.loads == 0U && fake.reserves == 0U && !fake.valid);
    CHECK(ucn_v6_realtime_bind_domain(owner, &config_d1, &path_d1,
                                      &capability_d1, 2U) == UCN_V6_OK);
    CHECK(fake.valid && fake.reserves == 1U);

    /* Domain generations, rather than an unbounded consumer-local invalidation
     * history, order D1 -> D2 -> D1.  Each bind resolves the live parent from
     * Capability Owner before touching durable state. */
    CHECK(ucn_v6_realtime_bind_domain(owner, &config_d2, &path_d2,
                                      &capability_d2, 3U) == UCN_V6_OK);
    config_d1.domain_generation = 3U;
    ++capability_d1.record.capability_generation;
    capability_d1.record.peer.clock_domain_generation = 3U;
    ++path_d1.route_generation;
    ++path_d1.path_generation;
    refresh_capability_binding(&capability_d1, &path_d1);
    CHECK(ucn_v6_realtime_bind_domain(owner, &config_d1, &path_d1,
                                      &capability_d1, 4U) == UCN_V6_OK);
    CHECK(fake.record.config.domain_generation == 3U &&
          fake.reserves == 3U);
    return 0;
}

static int test_bind_preflight_has_zero_durable_side_effect(void)
{
    ucn_v6_realtime_owner_storage_t storage;
    ucn_v6_realtime_owner_t *owner = NULL;
    fake_store_t fake;
    ucn_v6_realtime_generation_store_ops_t store;
    ucn_v6_callback_gate_t gate = {0};
    ucn_v6_principal_t master = principal(0x60U);
    ucn_v6_binding_key_t binding = { 1U, 10U, 1U };
    ucn_v6_cached_peer_capability_t capability =
        master_capability(&master, &binding, 1U);
    ucn_v6_path_capability_t path =
        fixed_path(&master, &binding, &capability);
    ucn_v6_time_domain_config_t config =
        domain_config(&master, &binding, 1U);
    unsigned loads_before;
    unsigned reserves_before;
    uint32_t generation_before;
    size_t index;

    memset(&fake, 0, sizeof(fake));
    store.context = &fake;
    store.load_domain_record = load_domain_record;
    store.reserve_domain_record = reserve_domain_record;
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, gate_lock, gate_unlock) ==
          UCN_V6_OK);
    CHECK(ucn_v6_realtime_owner_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              &store, &gate, &owner) == UCN_V6_OK);
    CHECK(ucn_v6_realtime_bind_domain(owner, &config, &path, &capability, 0U) ==
          UCN_V6_OK);

    loads_before = fake.loads;
    reserves_before = fake.reserves;
    generation_before = fake.record.config.domain_generation;
    CHECK(ucn_v6_realtime_bind_domain(owner, &config, &path, &capability, 0U) ==
          UCN_V6_ERR_REPLAY);
    CHECK(fake.loads == loads_before && fake.reserves == reserves_before &&
          fake.record.config.domain_generation == generation_before);

    for (index = 1U; index < UCN_V6_CONFIG_TIME_DOMAINS; ++index) {
        config.clock_domain_id = (uint16_t)(7U + index);
        config.domain_generation = (uint32_t)(1U + index);
        ++capability.record.capability_generation;
        capability.record.peer.clock_domain_id = config.clock_domain_id;
        capability.record.peer.clock_domain_generation =
            config.domain_generation;
        ++path.route_generation;
        ++path.path_generation;
        refresh_capability_binding(&capability, &path);
        CHECK(ucn_v6_realtime_bind_domain(owner, &config, &path,
                                           &capability, 0U) == UCN_V6_OK);
    }

    config.clock_domain_id = 100U;
    config.domain_generation =
        (uint32_t)(UCN_V6_CONFIG_TIME_DOMAINS + 1U);
    capability.record.peer.clock_domain_id = config.clock_domain_id;
    capability.record.peer.clock_domain_generation = config.domain_generation;
    ++capability.record.capability_generation;
    ++path.route_generation;
    ++path.path_generation;
    refresh_capability_binding(&capability, &path);
    loads_before = fake.loads;
    reserves_before = fake.reserves;
    generation_before = fake.record.config.domain_generation;
    CHECK(ucn_v6_realtime_bind_domain(owner, &config, &path, &capability, 0U) ==
          UCN_V6_ERR_NO_SPACE);
    CHECK(fake.loads == loads_before && fake.reserves == reserves_before &&
          fake.record.config.domain_generation == generation_before);
    return 0;
}

static int test_generation_rejects_cross_restart_domain_aba(void)
{
    ucn_v6_realtime_owner_storage_t first_storage;
    ucn_v6_realtime_owner_storage_t restarted_storage;
    ucn_v6_realtime_owner_t *first = NULL;
    ucn_v6_realtime_owner_t *restarted = NULL;
    fake_store_t fake;
    ucn_v6_realtime_generation_store_ops_t store;
    ucn_v6_callback_gate_t gate = {0};
    ucn_v6_principal_t master = principal(0x70U);
    ucn_v6_binding_key_t binding = { 1U, 11U, 3U };
    ucn_v6_cached_peer_capability_t capability =
        master_capability(&master, &binding, 5U);
    ucn_v6_path_capability_t path =
        fixed_path(&master, &binding, &capability);
    ucn_v6_time_domain_config_t config =
        domain_config(&master, &binding, 5U);
    ucn_v6_time_domain_config_t changed;
    ucn_v6_path_capability_t changed_path;
    ucn_v6_cached_peer_capability_t changed_capability;
    ucn_v6_principal_t alternate_master = principal(0x71U);
    ucn_v6_binding_key_t alternate_binding = { 1U, 14U, 1U };

    memset(&fake, 0, sizeof(fake));
    memset(&store, 0, sizeof(store));
    store.context = &fake;
    store.load_domain_record = load_domain_record;
    store.reserve_domain_record = reserve_domain_record;
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, gate_lock, gate_unlock) ==
          UCN_V6_OK);
    CHECK(ucn_v6_realtime_owner_init_in_place(
              first_storage.bytes, sizeof(first_storage),
              ucn_v6_compiled_manifest(), &store, &gate,
              &first) == UCN_V6_OK);
    CHECK(ucn_v6_realtime_bind_domain(
              first, &config, &path, &capability, 0U) == UCN_V6_OK);

    CHECK(ucn_v6_realtime_owner_init_in_place(
              restarted_storage.bytes, sizeof(restarted_storage),
              ucn_v6_compiled_manifest(), &store, &gate,
              &restarted) == UCN_V6_OK);
    changed = config;
    changed_path = path;
    ++changed_path.route_generation;
    changed_path.path_generation = 9U;
    CHECK(ucn_v6_realtime_bind_domain(
              restarted, &changed, &changed_path, &capability, 0U) ==
          UCN_V6_ERR_REPLAY);
    CHECK(fake.record.route_ref.path_generation == path.path_generation);

    changed_capability = capability;
    changed_path = path;
    ++changed_capability.record.link.nominal_rate_bps;
    refresh_capability_binding(&changed_capability, &changed_path);
    CHECK(ucn_v6_realtime_bind_domain(
              restarted, &config, &changed_path, &changed_capability, 0U) ==
          UCN_V6_ERR_REPLAY);

    changed_path = path;
    ++changed_path.timestamp_uncertainty_us;
    CHECK(ucn_v6_realtime_bind_domain(
              restarted, &config, &changed_path, &capability, 0U) ==
          UCN_V6_ERR_REPLAY);

    changed = domain_config(&alternate_master, &alternate_binding, 4U);
    changed_capability = master_capability(
        &alternate_master, &alternate_binding, 4U);
    changed_path = fixed_path(&alternate_master, &alternate_binding,
                              &changed_capability);
    CHECK(ucn_v6_realtime_bind_domain(
              restarted, &changed, &changed_path, &changed_capability, 0U) ==
          UCN_V6_ERR_REPLAY);

    /* Advancing only the outer Domain generation cannot roll back a child
     * generation while its RFC parent domain is unchanged. */
    changed = config;
    changed.domain_generation = 6U;
    changed_capability = capability;
    changed_capability.record.peer.clock_domain_generation = 6U;
    changed_capability.record.capability_generation = 1U;
    changed_path = path;
    refresh_capability_binding(&changed_capability, &changed_path);
    CHECK(ucn_v6_realtime_bind_domain(
              restarted, &changed, &changed_path, &changed_capability, 0U) ==
          UCN_V6_ERR_REPLAY);

    changed_capability = capability;
    changed_capability.record.peer.clock_domain_generation = 6U;
    changed_capability.ingress_link_generation = 5U;
    changed_path = path;
    changed_path.local_parent_link_generation = 5U;
    refresh_capability_binding(&changed_capability, &changed_path);
    CHECK(ucn_v6_realtime_bind_domain(
              restarted, &changed, &changed_path, &changed_capability, 0U) ==
          UCN_V6_ERR_REPLAY);

    changed_capability = capability;
    changed_capability.record.peer.clock_domain_generation = 6U;
    changed_path = path;
    changed_path.route_generation = 4U;
    refresh_capability_binding(&changed_capability, &changed_path);
    CHECK(ucn_v6_realtime_bind_domain(
              restarted, &changed, &changed_path, &changed_capability, 0U) ==
          UCN_V6_ERR_REPLAY);

    changed = config;
    changed.domain_generation = 6U;
    changed_capability = capability;
    changed_capability.record.peer.clock_domain_generation = 6U;
    changed_path = path;
    changed_path.path_generation = 3U;
    refresh_capability_binding(&changed_capability, &changed_path);
    CHECK(ucn_v6_realtime_bind_domain(
              restarted, &changed, &changed_path, &changed_capability, 0U) ==
          UCN_V6_ERR_REPLAY);

    changed = config;
    changed.domain_generation = 6U;
    changed.master_session_generation = 2U;
    changed_capability = capability;
    changed_capability.session_generation = 2U;
    changed_capability.record.peer.clock_domain_generation = 6U;
    changed_path = path;
    changed_path.destination_session_generation = 2U;
    refresh_capability_binding(&changed_capability, &changed_path);
    CHECK(ucn_v6_realtime_bind_domain(
              restarted, &changed, &changed_path, &changed_capability, 0U) ==
          UCN_V6_ERR_REPLAY);

    /* A restarted Owner has lost its RAM watermark, so the exact old Security
     * Session cannot distinguish recovery from replay.  The checked-next
     * Session recovery path is covered separately. */
    CHECK(ucn_v6_realtime_bind_domain(
              restarted, &config, &path, &capability, 0U) ==
          UCN_V6_ERR_REPLAY);
    return 0;
}

static int test_dependency_expiry_and_holdover_rebind(void)
{
    ucn_v6_realtime_owner_storage_t storage;
    ucn_v6_realtime_owner_t *owner = NULL;
    fake_store_t fake;
    ucn_v6_realtime_generation_store_ops_t store;
    ucn_v6_callback_gate_t gate = {0};
    ucn_v6_principal_t master = principal(0x80U);
    ucn_v6_binding_key_t binding = { 1U, 12U, 1U };
    ucn_v6_cached_peer_capability_t capability =
        master_capability(&master, &binding, 1U);
    ucn_v6_path_capability_t path =
        fixed_path(&master, &binding, &capability);
    ucn_v6_time_domain_config_t config =
        domain_config(&master, &binding, 1U);
    ucn_v6_security_open_result_t opened = opened_sample(&master, &binding);
    synthetic_exchange_t sample;
    ucn_v6_realtime_clock_view_t clock;
    ucn_v6_realtime_view_t view;

    memset(&fake, 0, sizeof(fake));
    memset(&store, 0, sizeof(store));
    store.context = &fake;
    store.load_domain_record = load_domain_record;
    store.reserve_domain_record = reserve_domain_record;
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, gate_lock, gate_unlock) ==
          UCN_V6_OK);
    CHECK(ucn_v6_realtime_owner_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              &store, &gate, &owner) == UCN_V6_OK);

    path.deadline_us = 400U;
    CHECK(fake.loads == 0U && fake.reserves == 0U);
    CHECK(ucn_v6_realtime_bind_domain(owner, &config, &path, &capability,
                                      1U) == UCN_V6_OK);

    memset(&sample, 0, sizeof(sample));
    sample.clock_domain_id = config.clock_domain_id;
    sample.domain_generation = config.domain_generation;
    sample.uncertainty = uncertainty();
    sample.offset_us = 1000;
    sample.local_sample_us = 100U;
    CHECK(ingest_verified_exchange(
              owner, &opened, &sample,
              UCN_V6_PROTOCOL_OPCODE_TIME_DELAY_RESPONSE, 100U) == UCN_V6_OK);
    sample.local_sample_us = 200U;
    CHECK(ingest_verified_exchange(
              owner, &opened, &sample,
              UCN_V6_PROTOCOL_OPCODE_TIME_DELAY_RESPONSE, 200U) == UCN_V6_OK);
    CHECK(ucn_v6_realtime_get_clock(owner, config.clock_domain_id, 399U,
                                    &clock) == UCN_V6_OK);
    CHECK(ucn_v6_realtime_step(owner, 400U) == UCN_V6_OK);
    CHECK(ucn_v6_realtime_copy_view(owner, &view) == UCN_V6_OK &&
          view.locked_domains == 0U);
    CHECK(ucn_v6_realtime_get_clock(owner, config.clock_domain_id, 400U,
                                    &clock) == UCN_V6_ERR_TIMEOUT);
    CHECK(ucn_v6_realtime_copy_view(owner, &view) == UCN_V6_OK &&
          view.locked_domains == 0U);
    sample.local_sample_us = 401U;
    CHECK(ingest_verified_exchange(
              owner, &opened, &sample,
              UCN_V6_PROTOCOL_OPCODE_TIME_DELAY_RESPONSE, 401U) ==
          UCN_V6_ERR_ACCESS);

    ++config.domain_generation;
    ++capability.record.capability_generation;
    capability.record.peer.clock_domain_generation = config.domain_generation;
    ++path.route_generation;
    ++path.path_generation;
    path.deadline_us = UINT64_MAX;
    refresh_capability_binding(&capability, &path);
    CHECK(ucn_v6_realtime_bind_domain(owner, &config, &path, &capability,
                                       450U) == UCN_V6_OK);
    opened.frame.route_generation = path.route_generation;
    opened.frame.path.path_generation = path.path_generation;
    sample.domain_generation = config.domain_generation;
    sample.local_sample_us = 460U;
    CHECK(ingest_verified_exchange(
              owner, &opened, &sample,
              UCN_V6_PROTOCOL_OPCODE_TIME_DELAY_RESPONSE, 460U) == UCN_V6_OK);
    sample.local_sample_us = 470U;
    CHECK(ingest_verified_exchange(
              owner, &opened, &sample,
              UCN_V6_PROTOCOL_OPCODE_TIME_DELAY_RESPONSE, 470U) == UCN_V6_OK);
    CHECK(ucn_v6_realtime_get_clock(owner, config.clock_domain_id, 1470U,
                                    &clock) == UCN_V6_OK && clock.holdover);
    CHECK(ucn_v6_realtime_copy_view(owner, &view) == UCN_V6_OK &&
          view.locked_domains == 1U);

    ++config.domain_generation;
    ++capability.record.capability_generation;
    capability.record.peer.clock_domain_generation = config.domain_generation;
    ++path.route_generation;
    ++path.path_generation;
    refresh_capability_binding(&capability, &path);
    CHECK(ucn_v6_realtime_bind_domain(owner, &config, &path, &capability,
                                       1471U) == UCN_V6_OK);
    CHECK(ucn_v6_realtime_copy_view(owner, &view) == UCN_V6_OK &&
          view.locked_domains == 0U);
    return 0;
}

static int test_fault_releases_locked_domain_count(void)
{
    ucn_v6_realtime_owner_storage_t storage;
    ucn_v6_realtime_owner_t *owner = NULL;
    fake_store_t fake;
    ucn_v6_realtime_generation_store_ops_t store;
    ucn_v6_callback_gate_t gate = {0};
    ucn_v6_principal_t master = principal(0x90U);
    ucn_v6_binding_key_t binding = { 1U, 13U, 1U };
    ucn_v6_cached_peer_capability_t capability =
        master_capability(&master, &binding, 1U);
    ucn_v6_path_capability_t path =
        fixed_path(&master, &binding, &capability);
    ucn_v6_time_domain_config_t config =
        domain_config(&master, &binding, 1U);
    ucn_v6_security_open_result_t opened = opened_sample(&master, &binding);
    synthetic_exchange_t sample;
    ucn_v6_realtime_view_t view;
    ucn_v6_realtime_clock_view_t clock;

    memset(&fake, 0, sizeof(fake));
    memset(&store, 0, sizeof(store));
    store.context = &fake;
    store.load_domain_record = load_domain_record;
    store.reserve_domain_record = reserve_domain_record;
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, gate_lock, gate_unlock) ==
          UCN_V6_OK);
    CHECK(ucn_v6_realtime_owner_init_in_place(
              storage.bytes, sizeof(storage), ucn_v6_compiled_manifest(),
              &store, &gate, &owner) == UCN_V6_OK);
    CHECK(ucn_v6_realtime_bind_domain(owner, &config, &path, &capability,
                                      1U) == UCN_V6_OK);

    memset(&sample, 0, sizeof(sample));
    sample.clock_domain_id = config.clock_domain_id;
    sample.domain_generation = config.domain_generation;
    sample.uncertainty = uncertainty();
    sample.offset_us = 1000;
    sample.local_sample_us = 100U;
    CHECK(ingest_verified_exchange(
              owner, &opened, &sample,
              UCN_V6_PROTOCOL_OPCODE_TIME_DELAY_RESPONSE, 100U) == UCN_V6_OK);
    sample.local_sample_us = 200U;
    CHECK(ingest_verified_exchange(
              owner, &opened, &sample,
              UCN_V6_PROTOCOL_OPCODE_TIME_DELAY_RESPONSE, 200U) == UCN_V6_OK);
    CHECK(ucn_v6_realtime_copy_view(owner, &view) == UCN_V6_OK &&
          view.locked_domains == 1U && !view.faulted);

    sample.local_sample_us = 300U;
    sample.offset_us = 2000;
    CHECK(ingest_verified_exchange(
              owner, &opened, &sample,
              UCN_V6_PROTOCOL_OPCODE_TIME_DELAY_RESPONSE, 300U) ==
          UCN_V6_ERR_STATE);
    CHECK(ucn_v6_realtime_copy_view(owner, &view) == UCN_V6_OK &&
          view.locked_domains == 0U && view.faulted &&
          view.accepted_samples == 2U);
    CHECK(ucn_v6_realtime_get_clock(owner, config.clock_domain_id, 300U,
                                    &clock) == UCN_V6_ERR_STATE);
    return 0;
}

int main(void)
{
    CHECK(test_codec_and_uncertainty() == 0);
    CHECK(test_fixed_path_domain_and_dual_gate() == 0);
    CHECK(test_live_capability_owner_precedes_bind_and_domain_switch() ==
          0);
    CHECK(test_none_has_zero_overhead_and_generation_replay() == 0);
    CHECK(test_generation_store_callback_scope_is_fail_closed() == 0);
    CHECK(test_restart_requires_fresh_peer_session() == 0);
    CHECK(test_same_domain_session_rebind_preserves_runtime_high_water() ==
          0);
    CHECK(test_bind_preflight_has_zero_durable_side_effect() == 0);
    CHECK(test_generation_rejects_cross_restart_domain_aba() == 0);
    CHECK(test_dependency_expiry_and_holdover_rebind() == 0);
    CHECK(test_fault_releases_locked_domain_count() == 0);
    puts("ucn v6 realtime tests passed");
    return 0;
}
