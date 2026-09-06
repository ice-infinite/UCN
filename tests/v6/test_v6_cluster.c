#include "ucn/v6/ucn_v6_cluster.h"

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

typedef struct fake_store {
    bool valid;
    bool fail_submit;
    uint64_t witness;
    uint8_t bytes[UCN_V6_CLUSTER_RECORD_BYTES];
    unsigned loads;
    unsigned submits;
    ucn_v6_cluster_owner_t *reenter_owner;
    ucn_v6_result_t reenter_result;
    ucn_v6_callback_gate_t *callback_gate;
    const void *callback_scope_owner;
    bool force_gate_leave_on_load;
    bool force_gate_leave_on_submit;
    bool force_gate_leave_on_proof;
    ucn_v6_result_t forced_gate_leave_result;
    ucn_v6_capability_owner_storage_t capability_storage;
    ucn_v6_capability_owner_t *capability_owner;
    ucn_v6_route_owner_storage_t route_storage;
    ucn_v6_route_owner_t *route_owner;
    bool proof_valid;
    bool proof_manual;
    bool fail_proof_resolve;
    unsigned proof_resolves;
    ucn_v6_cluster_authority_proof_view_t proof;
    ucn_v6_result_t proof_reenter_result;
} fake_store_t;

typedef struct cluster_test_owner_binding {
    ucn_v6_cluster_owner_t *cluster;
    fake_store_t *store;
} cluster_test_owner_binding_t;

static cluster_test_owner_binding_t cluster_test_owners[16];

static ucn_v6_result_t store_load_witness(void *context,
                                           uint64_t *generation)
{
    fake_store_t *store = (fake_store_t *)context;
    if (generation == NULL) return UCN_V6_ERR_ARGUMENT;
    if (store->witness == 0U) return UCN_V6_ERR_NOT_FOUND;
    *generation = store->witness;
    return UCN_V6_OK;
}

static ucn_v6_result_t store_reserve_witness(void *context,
                                              uint64_t generation)
{
    fake_store_t *store = (fake_store_t *)context;
    if (generation == 0U || generation <= store->witness) {
        return UCN_V6_ERR_REPLAY;
    }
    store->witness = generation;
    return UCN_V6_OK;
}

static void gate_lock(void *context)
{
    (void)context;
}

static void gate_unlock(void *context)
{
    (void)context;
}

static ucn_v6_result_t store_load(void *context, uint8_t *record,
                                  size_t capacity, size_t *length)
{
    fake_store_t *store = (fake_store_t *)context;
    ++store->loads;
    if (store->force_gate_leave_on_load && store->callback_gate != NULL &&
        store->callback_scope_owner != NULL) {
        store->forced_gate_leave_result = ucn_v6_callback_gate_leave(
            store->callback_gate, store->callback_scope_owner);
    }
    if (!store->valid) return UCN_V6_ERR_NOT_FOUND;
    if (capacity < sizeof(store->bytes)) return UCN_V6_ERR_NO_SPACE;
    memcpy(record, store->bytes, sizeof(store->bytes));
    *length = sizeof(store->bytes);
    return UCN_V6_OK;
}

static ucn_v6_result_t store_submit(void *context, const uint8_t *record,
                                    size_t length)
{
    fake_store_t *store = (fake_store_t *)context;
    ++store->submits;
    if (store->reenter_owner != NULL) {
        store->reenter_result =
            ucn_v6_cluster_step(store->reenter_owner, 0U);
    }
    if (store->force_gate_leave_on_submit && store->callback_gate != NULL &&
        store->callback_scope_owner != NULL) {
        store->forced_gate_leave_result = ucn_v6_callback_gate_leave(
            store->callback_gate, store->callback_scope_owner);
    }
    if (store->fail_submit) return UCN_V6_ERR_STATE;
    if (length != sizeof(store->bytes)) return UCN_V6_ERR_MALFORMED;
    memcpy(store->bytes, record, sizeof(store->bytes));
    store->valid = true;
    return UCN_V6_OK;
}

static bool proof_ref_equal(
    const ucn_v6_cluster_authority_proof_ref_t *left,
    const ucn_v6_cluster_authority_proof_ref_t *right)
{
    return left != NULL && right != NULL &&
           left->proof_id == right->proof_id &&
           left->generation == right->generation;
}

static ucn_v6_result_t proof_resolve_verified(
    void *context,
    const ucn_v6_cluster_authority_proof_ref_t *ref,
    uint64_t now_us,
    ucn_v6_cluster_authority_proof_view_t *view)
{
    fake_store_t *store = (fake_store_t *)context;
    ++store->proof_resolves;
    if (store->reenter_owner != NULL) {
        store->proof_reenter_result =
            ucn_v6_cluster_admit_member(store->reenter_owner, NULL, NULL,
                                        now_us, 1U);
    }
    if (store->force_gate_leave_on_proof && store->callback_gate != NULL &&
        store->callback_scope_owner != NULL) {
        store->forced_gate_leave_result = ucn_v6_callback_gate_leave(
            store->callback_gate, store->callback_scope_owner);
    }
    if (store->fail_proof_resolve || !store->proof_valid || view == NULL ||
        !proof_ref_equal(ref, &store->proof.ref) ||
        store->proof.lease_deadline_us <= now_us) {
        return UCN_V6_ERR_NOT_FOUND;
    }
    *view = store->proof;
    return UCN_V6_OK;
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

static ucn_v6_binding_key_t binding(uint32_t address)
{
    ucn_v6_binding_key_t value = { 1U, address, 1U };
    return value;
}

static ucn_v6_cluster_voter_t voter(uint8_t seed, uint32_t address)
{
    ucn_v6_cluster_voter_t value;
    memset(&value, 0, sizeof(value));
    value.principal = principal(seed);
    value.binding = binding(address);
    return value;
}

static ucn_v6_cluster_config_t config3(void)
{
    ucn_v6_cluster_config_t value;
    memset(&value, 0, sizeof(value));
    value.valid = true;
    value.config_id = 1U;
    value.generation = 1U;
    value.voter_count = 3U;
    value.voters[0] = voter(0x10U, 1U);
    value.voters[1] = voter(0x20U, 2U);
    value.voters[2] = voter(0x30U, 3U);
    return value;
}

static ucn_v6_security_open_result_t opened(uint8_t seed, uint32_t address)
{
    ucn_v6_security_open_result_t value;
    memset(&value, 0, sizeof(value));
    value.authenticated_principal = principal(seed);
    value.ingress_peer_session.principal = value.authenticated_principal;
    value.ingress_peer_session.binding = binding(address);
    value.ingress_peer_session.session_generation = 1U;
    value.ingress_link_instance_id = 5U;
    value.ingress_link_instance_generation = 6U;
    value.frame.realm_id = value.ingress_peer_session.binding.realm_id;
    value.frame.source_address =
        value.ingress_peer_session.binding.node_address;
    value.frame.source_binding_generation =
        value.ingress_peer_session.binding.binding_generation;
    value.frame.session_generation =
        value.ingress_peer_session.session_generation;
    value.frame.flags = UCN_V6_FLAG_E2E_CONTEXT |
                        UCN_V6_FLAG_PEER_HOP_CONTEXT;
    value.frame.origin_sequence = 1U;
    value.hop_authenticated = true;
    value.endpoint_authorized = true;
    return value;
}

static ucn_v6_cached_peer_capability_t capability(uint8_t seed,
                                                   uint32_t address)
{
    ucn_v6_cached_peer_capability_t value;
    memset(&value, 0, sizeof(value));
    value.valid = true;
    value.principal = principal(seed);
    value.binding = binding(address);
    value.session_generation = 1U;
    value.ingress_link_id = 5U;
    value.ingress_link_generation = 6U;
    value.record.capability_generation = 1U;
    value.record.link.link_instance_generation = 6U;
    value.record.link.carrier_mtu = 512U;
    value.record.link.link_frame_mtu = 256U;
    value.record.link.processing_frame_mtu = 220U;
    value.record.link.carrier_header_bytes = 2U;
    value.record.link.carrier_padding_bytes = 1U;
    value.record.link.carrier_crc_bytes = 2U;
    value.record.link.carrier_tag_bytes = 8U;
    value.record.link.carrier_max_fragments = 32U;
    value.record.link.link_flags = UCN_V6_LINK_ORDERED |
                                   UCN_V6_LINK_RELIABLE |
                                   UCN_V6_LINK_UNICAST |
                                   UCN_V6_LINK_SECURITY;
    value.record.link.nominal_rate_bps = 3000000U;
    value.record.link.hardware_priority_count = 4U;
    value.record.peer.feature_bits = UCN_V6_FEATURE_IDENTITY |
                                     UCN_V6_FEATURE_WIRE |
                                     UCN_V6_FEATURE_MESSAGE |
                                     UCN_V6_FEATURE_SECURITY |
                                     UCN_V6_FEATURE_CAPABILITY |
                                     UCN_V6_FEATURE_ROUTE |
                                     UCN_V6_FEATURE_CLUSTER;
    value.record.peer.hop_suite_bits =
        UINT32_C(1) << UCN_V6_SUITE_HMAC_SHA256_128;
    value.record.peer.e2e_suite_bits =
        UINT32_C(1) << UCN_V6_SUITE_AES_GCM_128;
    value.record.peer.max_message_class = UCN_V6_MESSAGE_T256;
    value.record.peer.max_rx_window = 4U;
    value.record.peer.max_concurrent_transfers = 2U;
    (void)ucn_v6_capability_digest(&value.record, value.digest);
    value.discovery_deadline_us = UINT64_MAX;
    value.capability_deadline_us = UINT64_MAX;
    return value;
}

static ucn_v6_result_t ensure_capability_peer(
    fake_store_t *fake, uint8_t seed, uint32_t address)
{
    ucn_v6_cached_peer_capability_t candidate = capability(seed, address);
    ucn_v6_cached_peer_capability_t current;
    ucn_v6_security_open_result_t authenticated = opened(seed, address);
    uint8_t payload[UCN_V6_CAPABILITY_RECORD_BYTES];
    if (ucn_v6_capability_copy_peer(
            fake->capability_owner, 0U, &candidate.principal,
            &candidate.binding, candidate.session_generation,
            candidate.ingress_link_generation, &current) ==
        UCN_V6_OK) {
        return UCN_V6_OK;
    }
    if (ucn_v6_capability_record_encode(&candidate.record, payload) !=
        UCN_V6_OK) {
        return UCN_V6_ERR_STATE;
    }
    authenticated.frame.flags = UCN_V6_FLAG_PEER_HOP_CONTEXT |
                                UCN_V6_FLAG_PROTOCOL_CONTEXT;
    authenticated.frame.frame_type = UCN_V6_FRAME_CONTROL;
    authenticated.frame.realm_id = candidate.binding.realm_id;
    authenticated.frame.source_address = candidate.binding.node_address;
    authenticated.frame.source_binding_generation =
        candidate.binding.binding_generation;
    authenticated.frame.session_generation = candidate.session_generation;
    authenticated.frame.protocol_opcode =
        UCN_V6_PROTOCOL_OPCODE_CAPABILITY_ADVERTISE;
    authenticated.frame.payload = payload;
    authenticated.frame.payload_length = sizeof(payload);
    authenticated.endpoint_authorized = false;
    return ucn_v6_capability_ingest_advertise(
        fake->capability_owner, 0U, &authenticated, &candidate.record);
}

static ucn_v6_result_t ensure_capability_owner(fake_store_t *fake)
{
    ucn_v6_cached_peer_capability_t local = capability(0x10U, 1U);
    if (fake->capability_owner == NULL) {
        ucn_v6_result_t result = ucn_v6_capability_owner_init_in_place(
            fake->capability_storage.bytes,
            sizeof(fake->capability_storage), ucn_v6_compiled_manifest(),
            &local.record, UINT64_C(1000000000), UINT64_C(1000000000),
            &fake->capability_owner);
        if (result != UCN_V6_OK) {
            return result;
        }
    }
    if (ensure_capability_peer(fake, 0x10U, 1U) != UCN_V6_OK ||
        ensure_capability_peer(fake, 0x20U, 2U) != UCN_V6_OK ||
        ensure_capability_peer(fake, 0x30U, 3U) != UCN_V6_OK) {
        return UCN_V6_ERR_STATE;
    }
    if (fake->route_owner == NULL) {
        ucn_v6_result_t result = ucn_v6_route_owner_init_in_place(
            fake->route_storage.bytes, sizeof(fake->route_storage),
            ucn_v6_compiled_manifest(), fake->capability_owner,
            1000U, 100U, 4U, 100U, 100U, &fake->route_owner);
        if (result != UCN_V6_OK) {
            return result;
        }
    }
    return UCN_V6_OK;
}

static ucn_v6_result_t bind_control_payload(
    ucn_v6_security_open_result_t *opened_value,
    const ucn_v6_cluster_control_t *control,
    uint8_t payload[UCN_V6_CLUSTER_CONTROL_BYTES])
{
    ucn_v6_result_t result = ucn_v6_cluster_control_encode(control, payload);
    if (result != UCN_V6_OK) return result;
    opened_value->frame.frame_type = UCN_V6_FRAME_CONTROL;
    opened_value->frame.flags |= UCN_V6_FLAG_PROTOCOL_CONTEXT;
    opened_value->frame.protocol_opcode = UCN_V6_PROTOCOL_OPCODE_CLUSTER_CONTROL;
    opened_value->frame.payload = payload;
    opened_value->frame.payload_length = UCN_V6_CLUSTER_CONTROL_BYTES;
    return UCN_V6_OK;
}

static ucn_v6_result_t bind_directory_payload(
    ucn_v6_security_open_result_t *opened_value,
    const ucn_v6_cluster_directory_entry_t *entry,
    uint8_t payload[UCN_V6_CLUSTER_DIRECTORY_BYTES])
{
    ucn_v6_cluster_directory_entry_t canonical;
    ucn_v6_result_t result;
    if (entry == NULL) return UCN_V6_ERR_ARGUMENT;
    canonical = *entry;
    if (canonical.authority_proof.proof_id == 0U) {
        canonical.authority_proof.proof_id = canonical.remote_cluster_id;
    }
    canonical.authority_proof.generation = canonical.path_generation;
    result = ucn_v6_cluster_directory_encode(&canonical, payload);
    if (result != UCN_V6_OK) return result;
    if (opened_value->frame.origin_sequence == 0U) {
        opened_value->frame.origin_sequence = 1U;
    } else if (opened_value->frame.origin_sequence <
               UCN_V6_SERIAL_ROTATION_THRESHOLD) {
        ++opened_value->frame.origin_sequence;
    } else {
        return UCN_V6_ERR_EXHAUSTED;
    }
    opened_value->frame.frame_type = UCN_V6_FRAME_CONTROL;
    opened_value->frame.flags |= UCN_V6_FLAG_PROTOCOL_CONTEXT;
    opened_value->frame.protocol_opcode =
        UCN_V6_PROTOCOL_OPCODE_CLUSTER_DIRECTORY;
    opened_value->frame.payload = payload;
    opened_value->frame.payload_length = UCN_V6_CLUSTER_DIRECTORY_BYTES;
    return UCN_V6_OK;
}

static ucn_v6_cluster_directory_entry_t directory_entry(
    const ucn_v6_security_open_result_t *head,
    uint32_t remote_cluster_id,
    uint16_t path_id,
    uint32_t path_generation,
    uint64_t lease_duration_us)
{
    ucn_v6_cluster_directory_entry_t value;
    memset(&value, 0, sizeof(value));
    value.occupied = true;
    value.remote_cluster_id = remote_cluster_id;
    value.remote_epoch.cluster_id = remote_cluster_id;
    value.remote_epoch.term = 1U;
    value.remote_epoch.head_principal = head->authenticated_principal;
    value.remote_epoch.head_binding.realm_id = head->frame.realm_id;
    value.remote_epoch.head_binding.node_address =
        head->frame.source_address;
    value.remote_epoch.head_binding.binding_generation =
        head->frame.source_binding_generation;
    value.next_hop = head->ingress_peer_session;
    value.route_generation = 1U;
    value.path_id = path_id;
    value.path_generation = path_generation;
    value.authority_proof.proof_id = remote_cluster_id;
    value.authority_proof.generation = 1U;
    value.lease_duration_us = lease_duration_us;
    return value;
}

static void set_fake_authority_proof(
    fake_store_t *store,
    const ucn_v6_cluster_directory_entry_t *entry,
    uint64_t lease_deadline_us)
{
    memset(&store->proof, 0, sizeof(store->proof));
    store->proof.valid = true;
    store->proof.ref = entry->authority_proof;
    store->proof.epoch = entry->remote_epoch;
    store->proof.stable_config_id = 1U;
    store->proof.stable_config_generation = 1U;
    store->proof.stable_quorum_verified = true;
    store->proof.route_generation = entry->route_generation;
    store->proof.path_id = entry->path_id;
    store->proof.path_generation = entry->path_generation;
    memset(store->proof.evidence_digest, 0x5AU,
           sizeof(store->proof.evidence_digest));
    store->proof.lease_deadline_us = lease_deadline_us;
    store->proof_valid = true;
}

typedef struct test_cluster_tunnel {
    bool occupied;
    uint64_t tunnel_id;
    uint32_t source_cluster_id;
    uint32_t destination_cluster_id;
    ucn_v6_route_domain_t route_domain;
    ucn_v6_path_capability_t path;
    uint64_t deadline_us;
} test_cluster_tunnel_t;

static test_cluster_tunnel_t cluster_tunnel(
    const ucn_v6_cluster_config_t *config,
    const ucn_v6_cached_peer_capability_t *capability_value,
    uint64_t tunnel_id,
    uint16_t path_id,
    uint32_t path_generation,
    uint64_t deadline_us)
{
    test_cluster_tunnel_t value;
    memset(&value, 0, sizeof(value));
    value.occupied = true;
    value.tunnel_id = tunnel_id;
    value.source_cluster_id = 1U;
    value.destination_cluster_id = 9U;
    value.route_domain.origin_principal = config->voters[0].principal;
    value.route_domain.origin_binding = config->voters[0].binding;
    value.route_domain.origin_session_generation = 1U;
    value.route_domain.destination_principal = capability_value->principal;
    value.route_domain.destination_binding = capability_value->binding;
    value.route_domain.destination_session_generation =
        capability_value->session_generation;
    value.path.valid = true;
    value.path.destination_principal = capability_value->principal;
    value.path.destination_binding = capability_value->binding;
    value.path.destination_session_generation =
        capability_value->session_generation;
    value.path.destination_capability_generation =
        capability_value->record.capability_generation;
    memcpy(value.path.destination_capability_digest, capability_value->digest,
           sizeof(value.path.destination_capability_digest));
    value.path.destination_realtime_mode_bits =
        capability_value->record.peer.realtime_mode_bits;
    value.path.destination_clock_domain_id =
        capability_value->record.peer.clock_domain_id;
    value.path.destination_clock_domain_generation =
        capability_value->record.peer.clock_domain_generation;
    value.path.local_parent_session.principal = capability_value->principal;
    value.path.local_parent_session.binding = capability_value->binding;
    value.path.local_parent_session.session_generation =
        capability_value->session_generation;
    value.path.local_parent_link_id = capability_value->ingress_link_id;
    value.path.local_parent_link_generation =
        capability_value->ingress_link_generation;
    value.path.local_parent_capability_generation =
        capability_value->record.capability_generation;
    memcpy(value.path.local_parent_capability_digest, capability_value->digest,
           sizeof(value.path.local_parent_capability_digest));
    value.path.route_generation = 1U;
    value.path.path_id = path_id;
    value.path.path_generation = path_generation;
    value.path.hop_count = 1U;
    value.path.path_frame_mtu = 192U;
    value.path.payload_budget = 160U;
    value.path.fragment_data_budget = 128U;
    value.path.feature_bits = UCN_V6_FEATURE_CLUSTER;
    value.path.hop_suite_bits =
        UINT32_C(1) << UCN_V6_SUITE_HMAC_SHA256_128;
    value.path.e2e_suite_bits =
        UINT32_C(1) << UCN_V6_SUITE_AES_GCM_128;
    value.path.max_message_class = UCN_V6_MESSAGE_T256;
    value.path.max_window = 4U;
    value.path.max_concurrency = 2U;
    value.path.deadline_us = deadline_us;
    value.deadline_us = deadline_us;
    return value;
}

static ucn_v6_cluster_control_t transition_control(
    const ucn_v6_cluster_snapshot_t *snapshot,
    ucn_v6_cluster_control_kind_t kind)
{
    ucn_v6_cluster_control_t value;
    memset(&value, 0, sizeof(value));
    value.kind = kind;
    value.transaction_id = snapshot->transition.transaction_id;
    value.old_epoch = snapshot->transition.old_epoch;
    value.target_epoch = snapshot->transition.target_epoch;
    value.config_id = snapshot->transition.target_config_id;
    value.config_generation = snapshot->transition.target_config_generation;
    if (snapshot->last_vote.valid) {
        value.backup_generation = snapshot->last_vote.backup_generation;
    }
    if (kind == UCN_V6_CLUSTER_CTL_HANDOVER_READY) {
        value.authority_proof.proof_id = 77U;
        value.authority_proof.generation = 1U;
    }
    return value;
}

static ucn_v6_result_t init_owner(
    ucn_v6_cluster_owner_storage_t *storage, fake_store_t *fake,
    ucn_v6_callback_gate_t *gate, uint8_t seed, uint32_t address,
    ucn_v6_cluster_owner_t **owner)
{
    ucn_v6_cluster_store_ops_t ops;
    ucn_v6_cluster_authority_proof_owner_ops_t proof_ops;
    ucn_v6_principal_t local = principal(seed);
    ucn_v6_binding_key_t local_binding = binding(address);
    memset(&ops, 0, sizeof(ops));
    memset(&proof_ops, 0, sizeof(proof_ops));
    ops.context = fake;
    ops.load_generation_witness = store_load_witness;
    ops.reserve_generation_witness = store_reserve_witness;
    ops.load = store_load;
    ops.submit = store_submit;
    proof_ops.context = fake;
    proof_ops.resolve_verified = proof_resolve_verified;
    if (ensure_capability_owner(fake) != UCN_V6_OK) {
        return UCN_V6_ERR_STATE;
    }
    ucn_v6_result_t result = ucn_v6_cluster_owner_init_in_place(
        storage, sizeof(*storage), ucn_v6_compiled_manifest(), &local,
        &local_binding, 1U, fake->capability_owner, fake->route_owner,
        &proof_ops, &ops, gate, owner);
    if (result == UCN_V6_OK) {
        size_t index;
        for (index = 0U; index <
                 sizeof(cluster_test_owners) / sizeof(cluster_test_owners[0]);
             ++index) {
            if (cluster_test_owners[index].cluster == NULL ||
                cluster_test_owners[index].cluster == *owner ||
                cluster_test_owners[index].store == fake) {
                cluster_test_owners[index].cluster = *owner;
                cluster_test_owners[index].store = fake;
                return UCN_V6_OK;
            }
        }
        return UCN_V6_ERR_NO_SPACE;
    }
    return result;
}

static ucn_v6_result_t init_owner_without_authority_proof(
    ucn_v6_cluster_owner_storage_t *storage, fake_store_t *fake,
    ucn_v6_callback_gate_t *gate, ucn_v6_cluster_owner_t **owner)
{
    ucn_v6_cluster_store_ops_t ops;
    ucn_v6_principal_t local = principal(0x10U);
    ucn_v6_binding_key_t local_binding = binding(1U);
    memset(&ops, 0, sizeof(ops));
    ops.context = fake;
    ops.load_generation_witness = store_load_witness;
    ops.reserve_generation_witness = store_reserve_witness;
    ops.load = store_load;
    ops.submit = store_submit;
    if (ensure_capability_owner(fake) != UCN_V6_OK) {
        return UCN_V6_ERR_STATE;
    }
    return ucn_v6_cluster_owner_init_in_place(
        storage, sizeof(*storage), ucn_v6_compiled_manifest(), &local,
        &local_binding, 1U, fake->capability_owner, fake->route_owner,
        NULL, &ops, gate, owner);
}

static fake_store_t *test_store_for_cluster(ucn_v6_cluster_owner_t *owner)
{
    size_t index;
    for (index = 0U; index <
             sizeof(cluster_test_owners) / sizeof(cluster_test_owners[0]);
         ++index) {
        if (cluster_test_owners[index].cluster == owner) {
            return cluster_test_owners[index].store;
        }
    }
    return NULL;
}

static ucn_v6_capability_peer_ref_t peer_ref_from_capability(
    const ucn_v6_cached_peer_capability_t *capability_value)
{
    ucn_v6_capability_peer_ref_t ref;
    memset(&ref, 0, sizeof(ref));
    if (capability_value != NULL) {
        ref.principal = capability_value->principal;
        ref.binding = capability_value->binding;
        ref.session_generation = capability_value->session_generation;
        ref.ingress_link_id = capability_value->ingress_link_id;
        ref.ingress_link_generation =
            capability_value->ingress_link_generation;
    }
    return ref;
}

static ucn_v6_result_t test_cluster_admit_member(
    ucn_v6_cluster_owner_t *owner,
    const ucn_v6_security_open_result_t *opened_value,
    const ucn_v6_cached_peer_capability_t *capability_value,
    uint64_t now_us, uint64_t lease_duration_us)
{
    ucn_v6_capability_peer_ref_t ref =
        peer_ref_from_capability(capability_value);
    return ucn_v6_cluster_admit_member(owner, opened_value, &ref, now_us,
                                       lease_duration_us);
}

static ucn_v6_result_t test_cluster_directory_install(
    ucn_v6_cluster_owner_t *owner,
    const ucn_v6_security_open_result_t *opened_value,
    const ucn_v6_cached_peer_capability_t *capability_value,
    uint64_t now_us)
{
    fake_store_t *store = test_store_for_cluster(owner);
    ucn_v6_cluster_directory_entry_t entry;
    ucn_v6_capability_peer_ref_t ref =
        peer_ref_from_capability(capability_value);
    if (store != NULL && !store->proof_manual && opened_value != NULL &&
        ucn_v6_cluster_directory_decode(opened_value->frame.payload,
                                        opened_value->frame.payload_length,
                                        &entry) == UCN_V6_OK) {
        set_fake_authority_proof(store, &entry, UINT64_MAX);
    }
    return ucn_v6_cluster_directory_install(owner, opened_value, &ref, now_us);
}

static ucn_v6_result_t test_cluster_tunnel_install(
    ucn_v6_cluster_owner_t *owner,
    const test_cluster_tunnel_t *tunnel,
    const ucn_v6_cached_peer_capability_t *capability_value,
    uint64_t now_us)
{
    fake_store_t *store = test_store_for_cluster(owner);
    ucn_v6_cluster_tunnel_request_t request;
    ucn_v6_route_path_t route_path;
    ucn_v6_route_resolution_t resolution;
    ucn_v6_route_activation_t activation;
    uint64_t transaction_id;
    if (store != NULL && tunnel != NULL) {
        (void)ucn_v6_capability_install_path(
            store->capability_owner, now_us, &tunnel->path);
    }
    (void)capability_value;
    if (tunnel == NULL) {
        return UCN_V6_ERR_ARGUMENT;
    }
    memset(&request, 0, sizeof(request));
    request.tunnel_id = tunnel->tunnel_id;
    request.source_cluster_id = tunnel->source_cluster_id;
    request.destination_cluster_id = tunnel->destination_cluster_id;
    request.route_ref.domain = tunnel->route_domain;
    request.route_ref.route_generation = tunnel->path.route_generation;
    request.route_ref.path_id = tunnel->path.path_id;
    request.route_ref.path_generation = tunnel->path.path_generation;
    request.deadline_us = tunnel->deadline_us;
    if (store == NULL || store->route_owner == NULL) {
        return UCN_V6_ERR_STATE;
    }
    if (ucn_v6_route_resolve_ref(store->route_owner, now_us,
                                 &request.route_ref,
                                 &resolution) != UCN_V6_OK) {
        memset(&route_path, 0, sizeof(route_path));
        route_path.path_id = tunnel->path.path_id;
        route_path.path_generation = tunnel->path.path_generation;
        route_path.next_hop = tunnel->path.local_parent_session;
        route_path.egress_link_id = tunnel->path.local_parent_link_id;
        route_path.egress_link_generation =
            tunnel->path.local_parent_link_generation;
        route_path.next_hop_capability_generation =
            tunnel->path.local_parent_capability_generation;
        route_path.hop_count = tunnel->path.hop_count;
        route_path.priority = 1U;
        route_path.weight = 1U;
        route_path.available = true;
        route_path.capability = tunnel->path;
        transaction_id = ((uint64_t)tunnel->path.route_generation << 32U) |
                         tunnel->path.path_generation;
        if (ucn_v6_route_candidate_begin(
                store->route_owner, now_us, transaction_id,
                &tunnel->route_domain,
                tunnel->path.route_generation) != UCN_V6_OK ||
            ucn_v6_route_candidate_add_path(
                store->route_owner, now_us, transaction_id,
                &tunnel->route_domain, &route_path) != UCN_V6_OK ||
            ucn_v6_route_candidate_record_probe(
                store->route_owner, now_us, transaction_id,
                &tunnel->route_domain, tunnel->path.path_id,
                tunnel->path.path_generation) != UCN_V6_OK ||
            ucn_v6_route_candidate_prepare_activation(
                store->route_owner, now_us, transaction_id,
                &tunnel->route_domain, &activation) != UCN_V6_OK ||
            ucn_v6_route_candidate_record_activation_send(
                store->route_owner, now_us, transaction_id,
                &tunnel->route_domain, true) != UCN_V6_OK ||
            ucn_v6_route_candidate_commit_ack(
                store->route_owner, now_us, &activation) != UCN_V6_OK) {
            return UCN_V6_ERR_ACCESS;
        }
    }
    return ucn_v6_cluster_tunnel_install(owner, &request, now_us);
}

#define ucn_v6_cluster_admit_member(owner, opened_value, capability_value,   \
                                    now_us, lease_duration_us)               \
    test_cluster_admit_member((owner), (opened_value), (capability_value),   \
                              (now_us), (lease_duration_us))
#define ucn_v6_cluster_directory_install(owner, opened_value,                \
                                         capability_value, now_us)           \
    test_cluster_directory_install((owner), (opened_value),                  \
                                   (capability_value), (now_us))
#define ucn_v6_cluster_tunnel_install(owner, tunnel, capability, now_us) \
    test_cluster_tunnel_install((owner), (tunnel), (capability), (now_us))

static int test_record_and_control_codec(void)
{
    ucn_v6_cluster_snapshot_t snapshot;
    ucn_v6_cluster_snapshot_t decoded;
    ucn_v6_cluster_snapshot_t scratch;
    ucn_v6_cluster_control_t control;
    ucn_v6_cluster_control_t control_decoded;
    uint8_t bytes[UCN_V6_CLUSTER_RECORD_BYTES];
    uint8_t control_bytes[UCN_V6_CLUSTER_CONTROL_BYTES];
    uint8_t before[UCN_V6_CLUSTER_RECORD_BYTES];
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.record_generation = 1U;
    snapshot.boot_incarnation = 1U;
    snapshot.role = UCN_V6_CLUSTER_OBSERVER;
    snapshot.phase = UCN_V6_CLUSTER_PHASE_STABLE;
    CHECK(ucn_v6_cluster_snapshot_encode(&snapshot, bytes) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_snapshot_decode(bytes, sizeof(bytes), &scratch,
                                         &decoded) ==
          UCN_V6_OK);
    CHECK(decoded.record_generation == 1U && decoded.boot_incarnation == 1U);
    snapshot = decoded;
    CHECK(ucn_v6_cluster_snapshot_decode(bytes, sizeof(bytes), &snapshot,
                                         &snapshot) ==
          UCN_V6_ERR_MALFORMED);
    CHECK(memcmp(&snapshot, &decoded, sizeof(snapshot)) == 0);
    bytes[17] ^= 1U;
    CHECK(ucn_v6_cluster_snapshot_decode(bytes, sizeof(bytes), &scratch,
                                         &decoded) ==
          UCN_V6_ERR_MALFORMED);
    CHECK(ucn_v6_cluster_snapshot_encode(&snapshot, bytes) == UCN_V6_OK);
    bytes[4] = 0U;
    bytes[5] = 0U;
    {
        uint32_t crc = ucn_v6_crc32c(bytes,
            UCN_V6_CLUSTER_RECORD_BYTES - 4U);
        bytes[UCN_V6_CLUSTER_RECORD_BYTES - 4U] = (uint8_t)(crc >> 24U);
        bytes[UCN_V6_CLUSTER_RECORD_BYTES - 3U] = (uint8_t)(crc >> 16U);
        bytes[UCN_V6_CLUSTER_RECORD_BYTES - 2U] = (uint8_t)(crc >> 8U);
        bytes[UCN_V6_CLUSTER_RECORD_BYTES - 1U] = (uint8_t)crc;
    }
    memset(&decoded, 0xA5, sizeof(decoded));
    CHECK(ucn_v6_cluster_snapshot_decode(bytes, sizeof(bytes), &scratch,
                                         &decoded) ==
          UCN_V6_ERR_MALFORMED);
    memset(bytes, 0xA5, sizeof(bytes));
    memcpy(before, bytes, sizeof(bytes));
    snapshot.record_generation = 0U;
    CHECK(ucn_v6_cluster_snapshot_encode(&snapshot, bytes) ==
          UCN_V6_ERR_ARGUMENT);
    CHECK(memcmp(bytes, before, sizeof(bytes)) == 0);
    snapshot.record_generation =
        UCN_V6_SERIAL64_ROTATION_THRESHOLD + UINT64_C(1);
    CHECK(ucn_v6_cluster_snapshot_encode(&snapshot, bytes) ==
          UCN_V6_ERR_ARGUMENT);
    snapshot.record_generation = 1U;
    snapshot.transaction_high_water =
        UCN_V6_SERIAL64_ROTATION_THRESHOLD + UINT64_C(1);
    CHECK(ucn_v6_cluster_snapshot_encode(&snapshot, bytes) ==
          UCN_V6_ERR_ARGUMENT);
    snapshot.transaction_high_water = 0U;
    snapshot.role = (ucn_v6_cluster_role_t)-1;
    CHECK(ucn_v6_cluster_snapshot_encode(&snapshot, bytes) ==
          UCN_V6_ERR_ARGUMENT);
    snapshot.role = UCN_V6_CLUSTER_OBSERVER;
    snapshot.phase = (ucn_v6_cluster_phase_t)-1;
    CHECK(ucn_v6_cluster_snapshot_encode(&snapshot, bytes) ==
          UCN_V6_ERR_ARGUMENT);
    snapshot.phase = UCN_V6_CLUSTER_PHASE_STABLE;

    memset(&control, 0, sizeof(control));
    control.kind = UCN_V6_CLUSTER_CTL_HANDOVER_COMMIT;
    control.transaction_id = 7U;
    control.old_epoch.cluster_id = 1U;
    control.old_epoch.term = 2U;
    control.old_epoch.head_principal = principal(0x10U);
    control.old_epoch.head_binding = binding(1U);
    control.target_epoch.cluster_id = 1U;
    control.target_epoch.term = 3U;
    control.target_epoch.head_principal = principal(0x20U);
    control.target_epoch.head_binding = binding(2U);
    control.config_id = 1U;
    control.config_generation = 1U;
    CHECK(ucn_v6_cluster_control_encode(&control, control_bytes) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_control_decode(control_bytes, sizeof(control_bytes),
                                        &control_decoded) == UCN_V6_OK);
    CHECK(control_decoded.kind == control.kind &&
          control_decoded.transaction_id == control.transaction_id &&
          control_decoded.target_epoch.term == 3U);
    control.kind = (ucn_v6_cluster_control_kind_t)-1;
    CHECK(ucn_v6_cluster_control_encode(&control, control_bytes) ==
          UCN_V6_ERR_ARGUMENT);
    control.kind = UCN_V6_CLUSTER_CTL_HANDOVER_COMMIT;
    control.transaction_id =
        UCN_V6_SERIAL64_ROTATION_THRESHOLD + UINT64_C(1);
    CHECK(ucn_v6_cluster_control_encode(&control, control_bytes) ==
          UCN_V6_ERR_ARGUMENT);
    control_bytes[80] ^= 1U;
    CHECK(ucn_v6_cluster_control_decode(control_bytes, sizeof(control_bytes),
                                        &control_decoded) ==
          UCN_V6_ERR_MALFORMED);
    return 0;
}

static void semantic_zero_epoch_with_dirty_padding(
    ucn_v6_cluster_epoch_t *epoch)
{
    memset(epoch, 0xA5, sizeof(*epoch));
    epoch->cluster_id = 0U;
    epoch->term = 0U;
    memset(epoch->head_principal.bytes, 0,
           sizeof(epoch->head_principal.bytes));
    epoch->head_binding.realm_id = 0U;
    epoch->head_binding.node_address = 0U;
    epoch->head_binding.binding_generation = 0U;
}

static void semantic_zero_config_with_dirty_padding(
    ucn_v6_cluster_config_t *config)
{
    size_t index;
    memset(config, 0xA5, sizeof(*config));
    config->valid = false;
    config->config_id = 0U;
    config->generation = 0U;
    config->voter_count = 0U;
    for (index = 0U; index < UCN_V6_CONFIG_CLUSTER_VOTERS; ++index) {
        memset(config->voters[index].principal.bytes, 0,
               sizeof(config->voters[index].principal.bytes));
        config->voters[index].binding.realm_id = 0U;
        config->voters[index].binding.node_address = 0U;
        config->voters[index].binding.binding_generation = 0U;
    }
}

static int test_record_validation_ignores_c_padding(void)
{
    ucn_v6_cluster_snapshot_t canonical;
    ucn_v6_cluster_snapshot_t padded;
    ucn_v6_cluster_config_t padded_config;
    size_t index;
    uint8_t canonical_bytes[UCN_V6_CLUSTER_RECORD_BYTES];
    uint8_t padded_bytes[UCN_V6_CLUSTER_RECORD_BYTES];

    memset(&canonical, 0, sizeof(canonical));
    canonical.record_generation = 1U;
    canonical.boot_incarnation = 1U;
    canonical.role = UCN_V6_CLUSTER_OBSERVER;
    canonical.phase = UCN_V6_CLUSTER_PHASE_STABLE;
    padded = canonical;
    semantic_zero_epoch_with_dirty_padding(&padded.active_epoch);
    semantic_zero_epoch_with_dirty_padding(&padded.max_epoch);
    semantic_zero_config_with_dirty_padding(&padded.stable_config);
    semantic_zero_config_with_dirty_padding(&padded.joint_new_config);
    semantic_zero_epoch_with_dirty_padding(&padded.last_vote.source_epoch);
    semantic_zero_epoch_with_dirty_padding(&padded.transition.old_epoch);
    semantic_zero_epoch_with_dirty_padding(&padded.transition.target_epoch);
    semantic_zero_config_with_dirty_padding(
        &padded.transition.target_config);
    padded.transition.target_authority_proof.proof_id = 0U;
    padded.transition.target_authority_proof.generation = 0U;
    CHECK(ucn_v6_cluster_snapshot_encode(&canonical, canonical_bytes) ==
          UCN_V6_OK);
    CHECK(ucn_v6_cluster_snapshot_encode(&padded, padded_bytes) ==
          UCN_V6_OK);
    CHECK(memcmp(canonical_bytes, padded_bytes, sizeof(canonical_bytes)) == 0);

    padded_config = config3();
    /* Rebuild unused voters field by field over dirty storage.  Any C padding
     * remains nonzero, while every protocol field is canonical zero. */
    for (index = padded_config.voter_count;
         index < UCN_V6_CONFIG_CLUSTER_VOTERS; ++index) {
        memset(&padded_config.voters[index], 0xA5,
               sizeof(padded_config.voters[index]));
        memset(padded_config.voters[index].principal.bytes, 0,
               sizeof(padded_config.voters[index].principal.bytes));
        padded_config.voters[index].binding.realm_id = 0U;
        padded_config.voters[index].binding.node_address = 0U;
        padded_config.voters[index].binding.binding_generation = 0U;
    }
    CHECK(ucn_v6_cluster_config_is_valid(&padded_config));
    canonical.active_epoch_valid = true;
    canonical.active_epoch.cluster_id = 1U;
    canonical.active_epoch.term = 1U;
    canonical.active_epoch.head_principal = padded_config.voters[0].principal;
    canonical.active_epoch.head_binding = padded_config.voters[0].binding;
    canonical.max_epoch_valid = true;
    canonical.max_epoch = canonical.active_epoch;
    canonical.stable_config = config3();
    canonical.role = UCN_V6_CLUSTER_HEAD;
    padded = canonical;
    padded.stable_config = padded_config;
    CHECK(ucn_v6_cluster_snapshot_encode(&canonical, canonical_bytes) ==
          UCN_V6_OK);
    CHECK(ucn_v6_cluster_snapshot_encode(&padded, padded_bytes) ==
          UCN_V6_OK);
    CHECK(memcmp(canonical_bytes, padded_bytes, sizeof(canonical_bytes)) == 0);
    return 0;
}

static int test_joint_authority_directory_and_tunnel(void)
{
    ucn_v6_cluster_owner_storage_t storage;
    ucn_v6_cluster_owner_t *owner = NULL;
    ucn_v6_callback_gate_t gate = {0};
    fake_store_t fake;
    ucn_v6_cluster_config_t old_config = config3();
    ucn_v6_cluster_config_t new_config = old_config;
    ucn_v6_security_open_result_t open2 = opened(0x20U, 2U);
    ucn_v6_security_open_result_t open3 = opened(0x30U, 3U);
    ucn_v6_cached_peer_capability_t cap2 = capability(0x20U, 2U);
    ucn_v6_cached_peer_capability_t cap3 = capability(0x30U, 3U);
    ucn_v6_cluster_view_t view;
    ucn_v6_cluster_directory_entry_t directory;
    test_cluster_tunnel_t tunnel;
    ucn_v6_cluster_control_t message;
    ucn_v6_stack_invalidation_t invalidation;
    uint8_t control_payload[UCN_V6_CLUSTER_CONTROL_BYTES];
    uint8_t directory_payload[UCN_V6_CLUSTER_DIRECTORY_BYTES];
    memset(&fake, 0, sizeof(fake));
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, gate_lock, gate_unlock) ==
          UCN_V6_OK);
    CHECK(init_owner(&storage, &fake, &gate, 0x10U, 1U, &owner) == UCN_V6_OK);
    CHECK(fake.valid && fake.submits == 1U);
    CHECK(ucn_v6_cluster_create(owner, 1U, &old_config, 0U) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_copy_view(owner, 0U, &view) == UCN_V6_OK);
    CHECK(!view.authority_active && !view.quorum_met);
    CHECK(ucn_v6_cluster_admit_member(owner, &open2, &cap2, 1U, 100U) ==
          UCN_V6_OK);
    CHECK(ucn_v6_cluster_copy_view(owner, 1U, &view) == UCN_V6_OK);
    CHECK(view.authority_active && view.quorum_met);
    CHECK(ucn_v6_cluster_admit_member(owner, &open3, &cap3, 1U, 100U) ==
          UCN_V6_OK);
    CHECK(ucn_v6_cluster_assign_backup(owner, &old_config.voters[1], 1U, 1U) ==
          UCN_V6_OK);
    new_config.config_id = 2U;
    new_config.generation = 2U;
    CHECK(ucn_v6_cluster_prepare_joint(owner, 100U, &new_config, 2U) ==
          UCN_V6_OK);
    CHECK(ucn_v6_cluster_commit_joint(owner, 100U, 2U) == UCN_V6_ERR_ACCESS);
    memset(&message, 0, sizeof(message));
    message.kind = UCN_V6_CLUSTER_CTL_CONFIG_ACK;
    message.transaction_id = 100U;
    {
        ucn_v6_cluster_snapshot_t current;
        CHECK(ucn_v6_cluster_copy_snapshot(owner, &current) == UCN_V6_OK);
        message.old_epoch = current.active_epoch;
    }
    message.config_id = 2U;
    message.config_generation = 2U;
    CHECK(bind_control_payload(&open2, &message, control_payload) == UCN_V6_OK);
    control_payload[UCN_V6_CLUSTER_CONTROL_BYTES - 1U] ^= 1U;
    CHECK(ucn_v6_cluster_backup_ack_config(owner, &open2, 2U) ==
          UCN_V6_ERR_ACCESS);
    CHECK(bind_control_payload(&open2, &message, control_payload) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_backup_ack_config(owner, &open2, 2U) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_commit_joint(owner, 100U, 2U) == UCN_V6_OK);
    new_config.config_id = 3U;
    new_config.generation = 3U;
    CHECK(ucn_v6_cluster_prepare_joint(owner, 101U, &new_config, 3U) ==
          UCN_V6_OK);
    {
        ucn_v6_cluster_config_t wrong = new_config;
        wrong.config_id = 4U;
        CHECK(ucn_v6_cluster_abort_joint(owner, 101U, &wrong, 3U) ==
              UCN_V6_ERR_STATE);
    }
    CHECK(ucn_v6_cluster_abort_joint(owner, 101U, &new_config, 3U) ==
          UCN_V6_OK);
    CHECK(ucn_v6_cluster_prepare_joint(owner, 101U, &new_config, 3U) ==
          UCN_V6_ERR_STATE);
    CHECK(ucn_v6_cluster_assign_backup(owner, &old_config.voters[1], 2U,
                                       101U) == UCN_V6_ERR_STATE);

    memset(&directory, 0, sizeof(directory));
    directory.occupied = true;
    directory.remote_cluster_id = 9U;
    directory.remote_epoch.cluster_id = 9U;
    directory.remote_epoch.term = 1U;
    directory.remote_epoch.head_principal = open2.authenticated_principal;
    directory.remote_epoch.head_binding = open2.ingress_peer_session.binding;
    directory.next_hop = open2.ingress_peer_session;
    directory.route_generation = 1U;
    directory.path_id = 1U;
    directory.path_generation = 1U;
    directory.lease_duration_us = 50U;
    {
        ucn_v6_cluster_directory_entry_t invalid = directory;
        uint8_t unchanged[UCN_V6_CLUSTER_DIRECTORY_BYTES];
        memset(directory_payload, 0xA5, sizeof(directory_payload));
        memcpy(unchanged, directory_payload, sizeof(unchanged));
        invalid.path_id = UINT16_MAX;
        CHECK(ucn_v6_cluster_directory_encode(&invalid, directory_payload) ==
              UCN_V6_ERR_ARGUMENT);
        CHECK(memcmp(directory_payload, unchanged, sizeof(unchanged)) == 0);
    }
    CHECK(bind_directory_payload(&open2, &directory, directory_payload) ==
          UCN_V6_OK);
    {
        ucn_v6_cached_peer_capability_t invalid_capability = cap2;
        ucn_v6_cluster_view_t before;
        ucn_v6_cluster_view_t after;
        invalid_capability.ingress_link_id = 0U;
        CHECK(ucn_v6_cluster_copy_view(owner, 2U, &before) == UCN_V6_OK);
        CHECK(ucn_v6_cluster_directory_install(
                  owner, &open2, &invalid_capability, 2U) ==
              UCN_V6_ERR_SECURITY);
        CHECK(ucn_v6_cluster_copy_view(owner, 2U, &after) == UCN_V6_OK);
        CHECK(after.directory_entries == before.directory_entries &&
              after.tunnels == before.tunnels &&
              after.rejected_security == before.rejected_security + 1U);
    }
    directory_payload[UCN_V6_CLUSTER_DIRECTORY_BYTES - 1U] ^= 1U;
    CHECK(ucn_v6_cluster_directory_install(owner, &open2, &cap2, 2U) ==
          UCN_V6_ERR_SECURITY);
    CHECK(bind_directory_payload(&open2, &directory, directory_payload) ==
          UCN_V6_OK);
    CHECK(ucn_v6_cluster_directory_install(owner, &open2, &cap2, 2U) ==
          UCN_V6_OK);
    directory.route_generation = 2U;
    directory.path_generation = 2U;
    directory.lease_duration_us = 60U;
    CHECK(bind_directory_payload(&open2, &directory, directory_payload) ==
          UCN_V6_OK);
    CHECK(ucn_v6_cluster_directory_install(owner, &open2, &cap2, 3U) ==
          UCN_V6_OK);
    {
        ucn_v6_cluster_directory_entry_t stale = directory;
        stale.route_generation = 1U;
        stale.path_generation = 1U;
        stale.lease_duration_us = 70U;
        CHECK(bind_directory_payload(&open2, &stale, directory_payload) ==
              UCN_V6_OK);
        CHECK(ucn_v6_cluster_directory_install(owner, &open2, &cap2, 3U) ==
              UCN_V6_ERR_REPLAY);
        stale = directory;
        ++stale.next_hop.session_generation;
        CHECK(bind_directory_payload(&open2, &stale, directory_payload) ==
              UCN_V6_OK);
        CHECK(ucn_v6_cluster_directory_install(owner, &open2, &cap2, 3U) ==
              UCN_V6_ERR_SECURITY);
        stale = directory;
        stale.lease_duration_us = 70U;
        CHECK(bind_directory_payload(&open2, &stale, directory_payload) ==
              UCN_V6_OK);
        CHECK(ucn_v6_cluster_directory_install(owner, &open2, &cap2, 3U) ==
              UCN_V6_ERR_REPLAY);
        stale = directory;
        ++stale.route_generation;
        ++stale.path_id;
        CHECK(bind_directory_payload(&open2, &stale, directory_payload) ==
              UCN_V6_OK);
        CHECK(ucn_v6_cluster_directory_install(owner, &open2, &cap2, 3U) ==
              UCN_V6_ERR_SECURITY);
        stale = directory;
        stale.lease_duration_us = 50U;
        CHECK(bind_directory_payload(&open2, &stale, directory_payload) ==
              UCN_V6_OK);
        CHECK(ucn_v6_cluster_directory_install(owner, &open2, &cap2, 3U) ==
              UCN_V6_ERR_REPLAY);
    }
    memset(&tunnel, 0, sizeof(tunnel));
    tunnel.occupied = true;
    tunnel.tunnel_id = 1U;
    tunnel.source_cluster_id = 1U;
    tunnel.destination_cluster_id = 9U;
    tunnel.route_domain.origin_principal = principal(0x10U);
    tunnel.route_domain.origin_binding = binding(1U);
    tunnel.route_domain.origin_session_generation = 1U;
    tunnel.route_domain.destination_principal = principal(0x20U);
    tunnel.route_domain.destination_binding = binding(2U);
    tunnel.route_domain.destination_session_generation =
        cap2.session_generation;
    tunnel.path.valid = true;
    tunnel.path.destination_principal = principal(0x20U);
    tunnel.path.destination_binding = binding(2U);
    tunnel.path.destination_session_generation = 1U;
    tunnel.path.destination_capability_generation =
        cap2.record.capability_generation;
    memcpy(tunnel.path.destination_capability_digest, cap2.digest,
           sizeof(tunnel.path.destination_capability_digest));
    tunnel.path.local_parent_session.principal = cap2.principal;
    tunnel.path.local_parent_session.binding = cap2.binding;
    tunnel.path.local_parent_session.session_generation =
        cap2.session_generation;
    tunnel.path.local_parent_link_id = cap2.ingress_link_id;
    tunnel.path.local_parent_link_generation = cap2.ingress_link_generation;
    tunnel.path.local_parent_capability_generation =
        cap2.record.capability_generation;
    memcpy(tunnel.path.local_parent_capability_digest, cap2.digest,
           sizeof(tunnel.path.local_parent_capability_digest));
    tunnel.path.route_generation = 1U;
    tunnel.path.path_id = 1U;
    tunnel.path.path_generation = 1U;
    tunnel.path.hop_count = 1U;
    tunnel.path.path_frame_mtu = 192U;
    tunnel.path.payload_budget = 160U;
    tunnel.path.fragment_data_budget = 128U;
    tunnel.path.feature_bits = UCN_V6_FEATURE_CLUSTER;
    tunnel.path.hop_suite_bits =
        UINT32_C(1) << UCN_V6_SUITE_HMAC_SHA256_128;
    tunnel.path.e2e_suite_bits =
        UINT32_C(1) << UCN_V6_SUITE_AES_GCM_128;
    tunnel.path.max_message_class = UCN_V6_MESSAGE_T256;
    tunnel.path.max_window = 4U;
    tunnel.path.max_concurrency = 2U;
    tunnel.path.deadline_us = 50U;
    tunnel.deadline_us = 50U;
    {
        test_cluster_tunnel_t wrong_link_generation = tunnel;
        ucn_v6_cluster_view_t before;
        ucn_v6_cluster_view_t after;
        ++wrong_link_generation.path.local_parent_link_generation;
        CHECK(ucn_v6_cluster_copy_view(owner, 2U, &before) == UCN_V6_OK);
        CHECK(ucn_v6_cluster_tunnel_install(
                  owner, &wrong_link_generation, &cap2, 2U) ==
              UCN_V6_ERR_ACCESS);
        CHECK(ucn_v6_cluster_copy_view(owner, 2U, &after) == UCN_V6_OK);
        CHECK(after.members == before.members &&
              after.directory_entries == before.directory_entries &&
              after.tunnels == before.tunnels &&
              after.persistence_commits == before.persistence_commits &&
              after.rejected_security == before.rejected_security &&
              after.rejected_quorum == before.rejected_quorum &&
              after.rejected_replay == before.rejected_replay);
    }
    CHECK(ucn_v6_cluster_tunnel_install(owner, &tunnel, &cap2, 2U) ==
          UCN_V6_OK);
    {
        test_cluster_tunnel_t semantic_replay;
        memset(&semantic_replay, 0xA5, sizeof(semantic_replay));
        semantic_replay.occupied = tunnel.occupied;
        semantic_replay.tunnel_id = tunnel.tunnel_id;
        semantic_replay.source_cluster_id = tunnel.source_cluster_id;
        semantic_replay.destination_cluster_id =
            tunnel.destination_cluster_id;
        semantic_replay.route_domain = tunnel.route_domain;
        semantic_replay.path = tunnel.path;
        semantic_replay.deadline_us = tunnel.deadline_us;
        CHECK(memcmp(&semantic_replay, &tunnel, sizeof(tunnel)) != 0);
        CHECK(ucn_v6_cluster_tunnel_install(owner, &semantic_replay, &cap2,
                                             2U) ==
              UCN_V6_OK);
        semantic_replay.destination_cluster_id = 8U;
        CHECK(ucn_v6_cluster_tunnel_install(owner, &semantic_replay, &cap2,
                                             2U) ==
              UCN_V6_ERR_REPLAY);
    }
    {
        ucn_v6_cluster_tunnel_t copied;
        memset(&copied, 0, sizeof(copied));
        CHECK(ucn_v6_cluster_copy_tunnel(owner, 1U, 2U, &copied) == UCN_V6_OK);
        CHECK(copied.route_ref.path_id == 1U &&
              copied.route_ref.domain.destination_binding.node_address == 2U);
    }
    CHECK(ucn_v6_cluster_copy_view(owner, 2U, &view) == UCN_V6_OK);
    CHECK(view.directory_entries == 1U && view.tunnels == 1U);
    memset(&invalidation, 0, sizeof(invalidation));
    invalidation.type = UCN_V6_STACK_INVALIDATE_PATH;
    invalidation.link_id = cap2.ingress_link_id;
    invalidation.link_generation = cap2.ingress_link_generation;
    invalidation.session = open2.ingress_peer_session;
    invalidation.capability_generation = cap2.record.capability_generation;
    invalidation.path_id = 1U;
    invalidation.path_generation = 1U;
    CHECK(ucn_v6_cluster_apply_invalidation(owner, &invalidation) ==
          UCN_V6_OK);
    CHECK(ucn_v6_cluster_copy_view(owner, 2U, &view) == UCN_V6_OK);
    CHECK(view.directory_entries == 1U && view.tunnels == 0U &&
          view.members == 2U);
    invalidation.type = UCN_V6_STACK_INVALIDATE_CAPABILITY;
    invalidation.path_id = 0U;
    invalidation.path_generation = 0U;
    CHECK(ucn_v6_cluster_apply_invalidation(owner, &invalidation) ==
          UCN_V6_OK);
    CHECK(ucn_v6_cluster_copy_view(owner, 2U, &view) == UCN_V6_OK);
    CHECK(!view.authority_active && view.directory_entries == 0U &&
          view.tunnels == 0U && view.members == 1U);
    {
        ucn_v6_cluster_directory_entry_t other_directory = directory;
        test_cluster_tunnel_t other_tunnel = tunnel;
        other_directory.remote_cluster_id = 10U;
        other_directory.remote_epoch.cluster_id = 10U;
        CHECK(bind_directory_payload(&open2, &other_directory,
                                     directory_payload) == UCN_V6_OK);
        CHECK(ucn_v6_cluster_directory_install(
                  owner, &open2, &cap2, 2U) == UCN_V6_OK);
        other_tunnel.tunnel_id = 2U;
        other_tunnel.destination_cluster_id = 10U;
        ++other_tunnel.path.route_generation;
        other_tunnel.path.path_generation = 2U;
        CHECK(ucn_v6_cluster_tunnel_install(
                  owner, &other_tunnel, &cap2, 2U) == UCN_V6_OK);
    }
    CHECK(ucn_v6_cluster_admit_member(owner, &open2, &cap2, 2U, 100U) ==
          UCN_V6_OK);
    CHECK(bind_directory_payload(&open2, &directory, directory_payload) ==
          UCN_V6_OK);
    CHECK(ucn_v6_cluster_directory_install(owner, &open2, &cap2, 2U) ==
          UCN_V6_OK);
    CHECK(ucn_v6_cluster_tunnel_install(owner, &tunnel, &cap2, 2U) ==
          UCN_V6_ERR_ACCESS);
    {
        ucn_v6_cached_peer_capability_t next_capability = cap2;
        ++next_capability.record.capability_generation;
        CHECK(ucn_v6_capability_digest(&next_capability.record,
                                        next_capability.digest) ==
              UCN_V6_OK);
        CHECK(ucn_v6_cluster_admit_member(
                  owner, &open2, &next_capability, 2U, 100U) == UCN_V6_OK);
        CHECK(ucn_v6_cluster_copy_view(owner, 2U, &view) == UCN_V6_OK);
        CHECK(view.authority_active && view.members == 2U);
        invalidation.capability_generation =
            cap2.record.capability_generation;
        CHECK(ucn_v6_cluster_apply_invalidation(owner, &invalidation) ==
              UCN_V6_OK);
        CHECK(ucn_v6_cluster_copy_view(owner, 2U, &view) == UCN_V6_OK);
        CHECK(!view.authority_active && view.members == 1U);
    }
    CHECK(ucn_v6_cluster_step(owner, 101U) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_copy_view(owner, 101U, &view) == UCN_V6_OK);
    CHECK(!view.authority_active && view.members == 0U &&
          view.directory_entries == 0U && view.tunnels == 0U);
    return 0;
}

static int test_takeover_recovery_rekey_and_handover(void)
{
    ucn_v6_cluster_owner_storage_t storage;
    ucn_v6_cluster_owner_storage_t reload_storage;
    ucn_v6_cluster_owner_storage_t recovery_storage;
    ucn_v6_cluster_owner_storage_t crash_reload_storage;
    ucn_v6_cluster_owner_t *owner = NULL;
    ucn_v6_cluster_owner_t *reloaded = NULL;
    ucn_v6_cluster_owner_t *recovery_reloaded = NULL;
    ucn_v6_cluster_owner_t *crash_reloaded = NULL;
    ucn_v6_callback_gate_t gate = {0};
    ucn_v6_callback_gate_t recovery_gate;
    ucn_v6_callback_gate_t crash_reload_gate = {0};
    fake_store_t fake;
    ucn_v6_cluster_config_t config = config3();
    ucn_v6_security_open_result_t open1 = opened(0x10U, 1U);
    ucn_v6_security_open_result_t open2 = opened(0x20U, 2U);
    ucn_v6_security_open_result_t open3 = opened(0x30U, 3U);
    ucn_v6_cached_peer_capability_t cap1 = capability(0x10U, 1U);
    ucn_v6_cached_peer_capability_t cap2 = capability(0x20U, 2U);
    ucn_v6_cached_peer_capability_t cap3 = capability(0x30U, 3U);
    ucn_v6_cluster_snapshot_t snapshot;
    ucn_v6_cluster_epoch_t target;
    ucn_v6_cluster_control_t message;
    uint8_t control_payload[UCN_V6_CLUSTER_CONTROL_BYTES];
    uint64_t expired_at = 0U;
    memset(&fake, 0, sizeof(fake));
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, gate_lock, gate_unlock) ==
          UCN_V6_OK);
    CHECK(init_owner(&storage, &fake, &gate, 0x20U, 2U, &owner) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_create(owner, 1U, &config, 0U) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_admit_member(owner, &open1, &cap1, 1U, 100U) ==
          UCN_V6_OK);
    CHECK(ucn_v6_cluster_admit_member(owner, &open3, &cap3, 1U, 100U) ==
          UCN_V6_OK);
    CHECK(ucn_v6_cluster_assign_backup(owner, &config.voters[0], 1U, 1U) ==
          UCN_V6_OK);
    /* Install a durable self-Backup state exactly as a remote Head would. */
    CHECK(ucn_v6_cluster_copy_snapshot(owner, &snapshot) == UCN_V6_OK);
    snapshot.role = UCN_V6_CLUSTER_BACKUP;
    snapshot.active_epoch.head_principal = principal(0x10U);
    snapshot.active_epoch.head_binding = binding(1U);
    snapshot.max_epoch = snapshot.active_epoch;
    snapshot.backup.principal = principal(0x20U);
    snapshot.backup.binding = binding(2U);
    snapshot.backup.generation = 2U;
    snapshot.record_generation++;
    CHECK(ucn_v6_cluster_snapshot_encode(&snapshot, fake.bytes) == UCN_V6_OK);
    fake.witness = snapshot.record_generation;
    memset(&gate, 0, sizeof(gate));
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, gate_lock, gate_unlock) ==
          UCN_V6_OK);
    CHECK(init_owner(&reload_storage, &fake, &gate, 0x20U, 2U, &reloaded) ==
          UCN_V6_OK);
    CHECK(ucn_v6_cluster_begin_takeover(reloaded, 200U, 2U, 200U) ==
          UCN_V6_ERR_ACCESS);
    CHECK(ucn_v6_cluster_admit_member(reloaded, &open1, &cap1, 1U, 10U) ==
          UCN_V6_OK);
    CHECK(ucn_v6_cluster_admit_member(reloaded, &open3, &cap3, 199U, 100U) ==
          UCN_V6_OK);
    CHECK(ucn_v6_cluster_begin_takeover(reloaded, 200U, 2U, 2U) ==
          UCN_V6_ERR_ACCESS);
    /* Replaying the same Head capability with a shorter membership lease must
     * not shorten the already observed remote-Head authority lease. */
    CHECK(ucn_v6_cluster_admit_member(reloaded, &open1, &cap1, 2U, 1U) ==
          UCN_V6_OK);
    CHECK(ucn_v6_cluster_begin_takeover(reloaded, 200U, 2U, 4U) ==
          UCN_V6_ERR_ACCESS);
    CHECK(ucn_v6_cluster_begin_takeover(reloaded, 200U, 2U, 200U) ==
          UCN_V6_OK);
    CHECK(ucn_v6_cluster_copy_snapshot(reloaded, &snapshot) == UCN_V6_OK);
    message = transition_control(&snapshot, UCN_V6_CLUSTER_CTL_TAKEOVER_VOTE);
    {
        ucn_v6_cluster_snapshot_t before_vote = snapshot;
        ucn_v6_cluster_snapshot_t after_vote;
        ++message.target_epoch.term;
        CHECK(bind_control_payload(&open3, &message, control_payload) ==
              UCN_V6_OK);
        CHECK(ucn_v6_cluster_record_transition_vote(reloaded, &open3, 200U) ==
              UCN_V6_ERR_ACCESS);
        CHECK(ucn_v6_cluster_copy_snapshot(reloaded, &after_vote) == UCN_V6_OK);
        CHECK(memcmp(&before_vote, &after_vote, sizeof(before_vote)) == 0);
        --message.target_epoch.term;
    }
    CHECK(bind_control_payload(&open3, &message, control_payload) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_copy_snapshot(reloaded, &snapshot) == UCN_V6_OK);
    {
        ucn_v6_cluster_snapshot_t after_expiry;
        CHECK(ucn_v6_cluster_record_transition_vote(reloaded, &open3, 299U) ==
              UCN_V6_ERR_ACCESS);
        CHECK(ucn_v6_cluster_copy_snapshot(reloaded, &after_expiry) ==
              UCN_V6_OK);
        CHECK(memcmp(&snapshot, &after_expiry, sizeof(snapshot)) == 0);
    }
    CHECK(ucn_v6_cluster_admit_member(reloaded, &open3, &cap3, 299U, 100U) ==
          UCN_V6_OK);
    CHECK(ucn_v6_cluster_record_transition_vote(reloaded, &open3, 299U) ==
          UCN_V6_OK);
    /* Re-admission is a new promise domain even when the caller replays the
     * same visible Session/Capability values.  It must durably clear the old
     * vote before Commit can observe the refreshed lease. */
    CHECK(ucn_v6_cluster_admit_member(reloaded, &open3, &cap3, 300U, 100U) ==
          UCN_V6_OK);
    CHECK(ucn_v6_cluster_copy_snapshot(reloaded, &snapshot) == UCN_V6_OK);
    CHECK(snapshot.transition.old_voter_bitmap == 2U &&
          snapshot.transition.new_voter_bitmap == 2U);
    CHECK(ucn_v6_cluster_commit_takeover(reloaded, 200U, 300U) ==
          UCN_V6_ERR_ACCESS);
    CHECK(ucn_v6_cluster_record_transition_vote(reloaded, &open3, 300U) ==
          UCN_V6_OK);
    CHECK(ucn_v6_cluster_commit_takeover(reloaded, 200U, 400U) ==
          UCN_V6_ERR_ACCESS);
    CHECK(ucn_v6_cluster_copy_snapshot(reloaded, &snapshot) == UCN_V6_OK);
    CHECK(snapshot.transition.old_voter_bitmap == 2U &&
          snapshot.transition.new_voter_bitmap == 2U);
    CHECK(ucn_v6_cluster_admit_member(reloaded, &open3, &cap3, 400U, 100U) ==
          UCN_V6_OK);
    CHECK(ucn_v6_cluster_record_transition_vote(reloaded, &open3, 400U) ==
          UCN_V6_OK);
    CHECK(ucn_v6_cluster_commit_takeover(reloaded, 200U, 400U) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_copy_snapshot(reloaded, &snapshot) == UCN_V6_OK);
    CHECK(snapshot.role == UCN_V6_CLUSTER_HEAD &&
          snapshot.active_epoch.term == 2U);

    CHECK(ucn_v6_cluster_admit_member(reloaded, &open1, &cap1, 500U, 10U) ==
          UCN_V6_OK);
    CHECK(ucn_v6_cluster_rekey(reloaded, 201U, 2U, 500U) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_copy_snapshot(reloaded, &snapshot) == UCN_V6_OK);
    CHECK(snapshot.active_epoch.cluster_id == 2U &&
          snapshot.tombstone_count == 1U &&
          ucn_v6_cluster_rekey(reloaded, 202U, 1U, 201U) == UCN_V6_ERR_REPLAY);

    target = snapshot.active_epoch;
    target.term = 2U;
    target.head_principal = principal(0x10U);
    target.head_binding = binding(1U);
    {
        ucn_v6_cluster_epoch_t nonvoter = target;
        ucn_v6_cluster_snapshot_t before;
        ucn_v6_cluster_snapshot_t after;
        nonvoter.head_principal = principal(0x40U);
        nonvoter.head_binding = binding(4U);
        CHECK(ucn_v6_cluster_copy_snapshot(reloaded, &before) == UCN_V6_OK);
        CHECK(ucn_v6_cluster_begin_handover(
          reloaded, 300U, &nonvoter, &snapshot.stable_config,
                  500U) == UCN_V6_ERR_STATE);
        CHECK(ucn_v6_cluster_copy_snapshot(reloaded, &after) == UCN_V6_OK);
        CHECK(memcmp(&before, &after, sizeof(before)) == 0);
    }
    /* An exact durable abort retires all partial proof before a transaction
     * number can be reused; before READY the old Head remains authoritative. */
    CHECK(ucn_v6_cluster_begin_handover(
              reloaded, 299U, &target, &snapshot.stable_config, 500U) ==
          UCN_V6_OK);
    CHECK(ucn_v6_cluster_copy_snapshot(reloaded, &snapshot) == UCN_V6_OK);
    CHECK(snapshot.transition.deadline_us > 500U);
    CHECK(ucn_v6_cluster_abort_transition(
              reloaded, UCN_V6_CLUSTER_TRANSITION_HANDOVER, 299U,
              &target, &snapshot.stable_config, 500U) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_copy_snapshot(reloaded, &snapshot) == UCN_V6_OK);
    CHECK(snapshot.transition.kind == UCN_V6_CLUSTER_TRANSITION_NONE &&
          snapshot.role == UCN_V6_CLUSTER_HEAD &&
          !snapshot.authority_fenced);
    CHECK(ucn_v6_cluster_commit_handover(reloaded, 299U, 500U) ==
          UCN_V6_ERR_STATE);
    CHECK(ucn_v6_cluster_begin_handover(
              reloaded, 300U, &target, &snapshot.stable_config, 500U) ==
          UCN_V6_OK);
    CHECK(ucn_v6_cluster_copy_snapshot(reloaded, &snapshot) == UCN_V6_OK);
    message = transition_control(&snapshot, UCN_V6_CLUSTER_CTL_HANDOVER_READY);
    memset(&fake.proof, 0, sizeof(fake.proof));
    fake.proof.valid = true;
    fake.proof.ref = message.authority_proof;
    fake.proof.epoch = snapshot.transition.target_epoch;
    fake.proof.stable_config_id = snapshot.transition.target_config_id;
    fake.proof.stable_config_generation =
        snapshot.transition.target_config_generation;
    fake.proof.stable_quorum_verified = true;
    memset(fake.proof.evidence_digest, 0x5AU,
           sizeof(fake.proof.evidence_digest));
    fake.proof.lease_deadline_us = 700U;
    fake.proof_valid = true;
    {
        ucn_v6_cluster_snapshot_t before_ready;
        ucn_v6_cluster_snapshot_t after_ready;
        CHECK(bind_control_payload(&open1, &message, control_payload) ==
              UCN_V6_OK);
        CHECK(ucn_v6_cluster_copy_snapshot(reloaded, &before_ready) ==
              UCN_V6_OK);
        CHECK(ucn_v6_cluster_handover_ready(reloaded, &open1, 511U) ==
              UCN_V6_ERR_ACCESS);
        CHECK(ucn_v6_cluster_copy_snapshot(reloaded, &after_ready) ==
              UCN_V6_OK);
        CHECK(memcmp(&before_ready, &after_ready, sizeof(before_ready)) == 0);
    }
    CHECK(ucn_v6_cluster_admit_member(reloaded, &open1, &cap1, 512U, 100U) ==
          UCN_V6_OK);
    ++message.old_epoch.term;
    CHECK(bind_control_payload(&open1, &message, control_payload) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_handover_ready(reloaded, &open1, 512U) ==
          UCN_V6_ERR_ACCESS);
    --message.old_epoch.term;
    CHECK(bind_control_payload(&open1, &message, control_payload) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_handover_ready(reloaded, &open1, 512U) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_commit_handover(reloaded, 300U, 512U) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_copy_snapshot(reloaded, &snapshot) == UCN_V6_OK);
    CHECK(snapshot.role == UCN_V6_CLUSTER_FENCED &&
          snapshot.authority_fenced);

    memset(&recovery_gate, 0, sizeof(recovery_gate));
    CHECK(ucn_v6_callback_gate_init(&recovery_gate, NULL, gate_lock,
                                    gate_unlock) == UCN_V6_OK);
    CHECK(init_owner(&recovery_storage, &fake, &recovery_gate, 0x20U, 2U,
                     &recovery_reloaded) == UCN_V6_OK);
    target.cluster_id = 3U;
    target.term = 1U;
    target.head_principal = principal(0x20U);
    target.head_binding = binding(2U);
    CHECK(ucn_v6_cluster_begin_recovery(recovery_reloaded, 400U, &target,
                                        600U) == UCN_V6_ERR_ACCESS);
    CHECK(ucn_v6_cluster_admit_member(recovery_reloaded, &open1, &cap1,
                                      600U, 10U) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_begin_recovery(recovery_reloaded, 400U, &target,
                                        600U) == UCN_V6_ERR_ACCESS);
    CHECK(ucn_v6_cluster_admit_member(recovery_reloaded, &open3, &cap3,
                                      611U, 100U) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_begin_recovery(recovery_reloaded, 400U, &target,
                                        611U) ==
          UCN_V6_OK);
    CHECK(ucn_v6_cluster_copy_snapshot(recovery_reloaded, &snapshot) ==
          UCN_V6_OK);
    message = transition_control(&snapshot, UCN_V6_CLUSTER_CTL_RECOVERY_VOTE);
    CHECK(bind_control_payload(&open3, &message, control_payload) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_record_transition_vote(recovery_reloaded, &open3,
                                                611U) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_admit_member(recovery_reloaded, &open3, &cap3,
                                      612U, 99U) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_commit_recovery(recovery_reloaded, 400U, 612U) ==
          UCN_V6_ERR_ACCESS);
    CHECK(ucn_v6_cluster_record_transition_vote(recovery_reloaded, &open3,
                                                612U) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_commit_recovery(recovery_reloaded, 400U, 711U) ==
          UCN_V6_ERR_ACCESS);
    CHECK(ucn_v6_cluster_admit_member(recovery_reloaded, &open3, &cap3,
                                      711U, 100U) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_record_transition_vote(recovery_reloaded, &open3,
                                                711U) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_commit_recovery(recovery_reloaded, 400U, 711U) ==
          UCN_V6_OK);
    CHECK(ucn_v6_cluster_copy_snapshot(recovery_reloaded, &snapshot) ==
          UCN_V6_OK);
    CHECK(snapshot.active_epoch.cluster_id == 3U &&
          snapshot.role == UCN_V6_CLUSTER_HEAD &&
          !snapshot.authority_fenced && snapshot.tombstone_count == 2U);
    /* The exact half-open deadline is fail-closed and durable. A late commit
     * cannot resurrect the expired Recovery Head's next Handover proof. */
    target = snapshot.active_epoch;
    target.term = snapshot.active_epoch.term + 1U;
    target.head_principal = snapshot.stable_config.voters[0].principal;
    target.head_binding = snapshot.stable_config.voters[0].binding;
    CHECK(ucn_v6_cluster_admit_member(
              recovery_reloaded, &open1, &cap1, 800U,
              UCN_V6_CONFIG_CLUSTER_TRANSITION_TIMEOUT_US + 100U) ==
          UCN_V6_OK);
    CHECK(ucn_v6_cluster_copy_snapshot(recovery_reloaded, &snapshot) ==
          UCN_V6_OK);
    CHECK(ucn_v6_cluster_begin_handover(
              recovery_reloaded, 401U, &target, &snapshot.stable_config,
              800U) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_copy_snapshot(recovery_reloaded, &snapshot) ==
          UCN_V6_OK);
    {
        expired_at = snapshot.transition.deadline_us;
        CHECK(ucn_v6_cluster_step(recovery_reloaded, expired_at) == UCN_V6_OK);
        CHECK(ucn_v6_cluster_copy_snapshot(recovery_reloaded, &snapshot) ==
              UCN_V6_OK);
        CHECK(snapshot.transition.kind == UCN_V6_CLUSTER_TRANSITION_NONE &&
              snapshot.role == UCN_V6_CLUSTER_HEAD &&
              !snapshot.authority_fenced);
        CHECK(ucn_v6_cluster_commit_handover(
                  recovery_reloaded, 401U, expired_at) == UCN_V6_ERR_STATE);
    }
    /* A crash/restart advances boot incarnation. The pre-restart durable
     * transition is abort-only: no late commit may promote it. */
    target = snapshot.active_epoch;
    target.term = snapshot.active_epoch.term + 1U;
    target.head_principal = snapshot.stable_config.voters[0].principal;
    target.head_binding = snapshot.stable_config.voters[0].binding;
    CHECK(ucn_v6_cluster_begin_handover(
              recovery_reloaded, 402U, &target, &snapshot.stable_config,
              expired_at + 1U) == UCN_V6_OK);
    CHECK(ucn_v6_callback_gate_init(&crash_reload_gate, NULL, gate_lock,
                                    gate_unlock) == UCN_V6_OK);
    CHECK(init_owner(&crash_reload_storage, &fake, &crash_reload_gate,
                     0x20U, 2U, &crash_reloaded) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_commit_handover(
              crash_reloaded, 402U, expired_at + 2U) == UCN_V6_ERR_STATE);
    CHECK(ucn_v6_cluster_step(crash_reloaded, expired_at + 2U) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_copy_snapshot(crash_reloaded, &snapshot) ==
          UCN_V6_OK);
    CHECK(snapshot.transition.kind == UCN_V6_CLUSTER_TRANSITION_NONE &&
          snapshot.role == UCN_V6_CLUSTER_HEAD &&
          !snapshot.authority_fenced);
    (void)open2;
    (void)cap2;
    return 0;
}

static int test_callback_reentry_and_failure_close(void)
{
    ucn_v6_cluster_owner_storage_t storage;
    ucn_v6_cluster_owner_storage_t leave_storage;
    ucn_v6_cluster_owner_storage_t init_storage;
    ucn_v6_cluster_owner_t *owner = NULL;
    ucn_v6_cluster_owner_t *leave_owner = NULL;
    ucn_v6_cluster_owner_t *init_owner_result = NULL;
    ucn_v6_callback_gate_t gate = {0};
    ucn_v6_callback_gate_t leave_gate = {0};
    ucn_v6_callback_gate_t init_gate = {0};
    fake_store_t fake;
    fake_store_t leave_fake;
    fake_store_t init_fake;
    ucn_v6_cluster_config_t config = config3();
    ucn_v6_cluster_snapshot_t before;
    ucn_v6_cluster_snapshot_t after;
    size_t index;

    memset(&fake, 0, sizeof(fake));
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, gate_lock, gate_unlock) ==
          UCN_V6_OK);
    CHECK(init_owner(&storage, &fake, &gate, 0x10U, 1U, &owner) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_copy_snapshot(owner, &before) == UCN_V6_OK);
    fake.reenter_owner = owner;
    CHECK(ucn_v6_cluster_create(owner, 1U, &config, 0U) ==
          UCN_V6_ERR_STATE);
    CHECK(fake.reenter_result == UCN_V6_ERR_STATE);
    CHECK(ucn_v6_cluster_copy_snapshot(owner, &after) == UCN_V6_OK);
    CHECK(memcmp(&before, &after, sizeof(before)) == 0);

    /* A callback that prematurely releases/corrupts the gate cannot turn a
     * successful Store write into a committed RAM authority state. */
    memset(&leave_fake, 0, sizeof(leave_fake));
    CHECK(ucn_v6_callback_gate_init(&leave_gate, NULL, gate_lock,
                                    gate_unlock) == UCN_V6_OK);
    CHECK(init_owner(&leave_storage, &leave_fake, &leave_gate,
                     0x10U, 1U, &leave_owner) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_copy_snapshot(leave_owner, &before) == UCN_V6_OK);
    leave_fake.callback_gate = &leave_gate;
    leave_fake.callback_scope_owner = leave_owner;
    leave_fake.force_gate_leave_on_submit = true;
    CHECK(ucn_v6_cluster_create(leave_owner, 1U, &config, 0U) ==
          UCN_V6_ERR_STATE);
    CHECK(leave_fake.forced_gate_leave_result == UCN_V6_OK);
    CHECK(ucn_v6_cluster_copy_snapshot(leave_owner, &after) == UCN_V6_OK);
    CHECK(memcmp(&before, &after, sizeof(before)) == 0);

    /* Init performs its first callback before the Owner is publishable.  A
     * broken leave must clear storage and never publish the output pointer. */
    memset(&init_fake, 0, sizeof(init_fake));
    memset(&init_storage, 0xA5, sizeof(init_storage));
    CHECK(ucn_v6_callback_gate_init(&init_gate, NULL, gate_lock,
                                    gate_unlock) == UCN_V6_OK);
    init_fake.callback_gate = &init_gate;
    init_fake.callback_scope_owner = &init_storage;
    init_fake.force_gate_leave_on_load = true;
    CHECK(init_owner(&init_storage, &init_fake, &init_gate,
                     0x10U, 1U, &init_owner_result) == UCN_V6_ERR_STATE);
    CHECK(init_owner_result == NULL &&
          init_fake.forced_gate_leave_result == UCN_V6_OK);
    for (index = 0U; index < sizeof(init_storage.bytes); ++index) {
        CHECK(init_storage.bytes[index] == 0U);
    }
    return 0;
}

static int test_witness_rollback_torn_write_and_capability_expiry(void)
{
    fake_store_t fake;
    fake_store_t torn;
    ucn_v6_callback_gate_t gate = {0};
    ucn_v6_callback_gate_t reload_gate = {0};
    ucn_v6_cluster_owner_storage_t storage;
    ucn_v6_cluster_owner_storage_t reload_storage;
    ucn_v6_cluster_owner_t *owner = NULL;
    ucn_v6_cluster_owner_t *reloaded = NULL;
    ucn_v6_cluster_config_t config = config3();
    ucn_v6_security_open_result_t open2 = opened(0x20U, 2U);
    ucn_v6_cached_peer_capability_t cap2 = capability(0x20U, 2U);
    uint8_t old_record[UCN_V6_CLUSTER_RECORD_BYTES];

    memset(&fake, 0, sizeof(fake));
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, gate_lock, gate_unlock) ==
          UCN_V6_OK);
    CHECK(init_owner(&storage, &fake, &gate, 0x10U, 1U, &owner) == UCN_V6_OK);
    memcpy(old_record, fake.bytes, sizeof(old_record));
    CHECK(ucn_v6_cluster_create(owner, 1U, &config, 0U) == UCN_V6_OK);
    CHECK(fake.witness == 2U);
    memcpy(fake.bytes, old_record, sizeof(fake.bytes));
    CHECK(ucn_v6_callback_gate_init(&reload_gate, NULL, gate_lock,
                                    gate_unlock) == UCN_V6_OK);
    CHECK(init_owner(&reload_storage, &fake, &reload_gate, 0x10U, 1U,
                     &reloaded) != UCN_V6_OK);
    CHECK(reloaded == NULL);

    memset(&torn, 0, sizeof(torn));
    memset(&gate, 0, sizeof(gate));
    memset(&reload_gate, 0, sizeof(reload_gate));
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, gate_lock, gate_unlock) ==
          UCN_V6_OK);
    CHECK(init_owner(&storage, &torn, &gate, 0x10U, 1U, &owner) == UCN_V6_OK);
    torn.fail_submit = true;
    CHECK(ucn_v6_cluster_create(owner, 1U, &config, 0U) == UCN_V6_ERR_STATE);
    CHECK(torn.witness == 2U);
    torn.fail_submit = false;
    CHECK(ucn_v6_callback_gate_init(&reload_gate, NULL, gate_lock,
                                    gate_unlock) == UCN_V6_OK);
    CHECK(init_owner(&reload_storage, &torn, &reload_gate, 0x10U, 1U,
                     &reloaded) != UCN_V6_OK);
    CHECK(reloaded == NULL);

    memset(&fake, 0, sizeof(fake));
    memset(&gate, 0, sizeof(gate));
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, gate_lock, gate_unlock) ==
          UCN_V6_OK);
    CHECK(init_owner(&storage, &fake, &gate, 0x10U, 1U, &owner) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_create(owner, 1U, &config, 0U) == UCN_V6_OK);
    CHECK(ucn_v6_capability_expire(fake.capability_owner,
                                   UINT64_C(1000000000)) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_admit_member(
              owner, &open2, &cap2, UINT64_C(1000000000), 100U) ==
          UCN_V6_ERR_SECURITY);
    return 0;
}

static int test_member_dependency_expiry_revokes_all_uses(void)
{
    ucn_v6_cluster_owner_storage_t storage;
    ucn_v6_cluster_owner_t *owner = NULL;
    ucn_v6_callback_gate_t gate = {0};
    fake_store_t fake;
    ucn_v6_cluster_config_t config = config3();
    ucn_v6_security_open_result_t open2 = opened(0x20U, 2U);
    ucn_v6_cached_peer_capability_t cap2 = capability(0x20U, 2U);
    ucn_v6_cluster_config_t new_config = config;
    ucn_v6_cluster_control_t message;
    ucn_v6_cluster_snapshot_t before_expiry;
    ucn_v6_cluster_snapshot_t after_expiry;
    test_cluster_tunnel_t tunnel;
    ucn_v6_cluster_tunnel_t copied;
    ucn_v6_cluster_view_t view;
    uint8_t control_payload[UCN_V6_CLUSTER_CONTROL_BYTES];

    memset(&fake, 0, sizeof(fake));
    memset(&gate, 0, sizeof(gate));
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, gate_lock, gate_unlock) ==
          UCN_V6_OK);
    CHECK(init_owner(&storage, &fake, &gate, 0x10U, 1U, &owner) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_create(owner, 1U, &config, 0U) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_admit_member(owner, &open2, &cap2, 1U, 100U) ==
          UCN_V6_OK);
    CHECK(ucn_v6_cluster_assign_backup(owner, &config.voters[1], 1U, 1U) ==
          UCN_V6_OK);

    memset(&tunnel, 0, sizeof(tunnel));
    tunnel.occupied = true;
    tunnel.tunnel_id = 1U;
    tunnel.source_cluster_id = 1U;
    tunnel.destination_cluster_id = 9U;
    tunnel.route_domain.origin_principal = config.voters[0].principal;
    tunnel.route_domain.origin_binding = config.voters[0].binding;
    tunnel.route_domain.origin_session_generation = 1U;
    tunnel.route_domain.destination_principal = config.voters[1].principal;
    tunnel.route_domain.destination_binding = config.voters[1].binding;
    tunnel.route_domain.destination_session_generation =
        cap2.session_generation;
    tunnel.path.valid = true;
    tunnel.path.destination_principal = config.voters[1].principal;
    tunnel.path.destination_binding = config.voters[1].binding;
    tunnel.path.destination_session_generation = 1U;
    tunnel.path.destination_capability_generation =
        cap2.record.capability_generation;
    memcpy(tunnel.path.destination_capability_digest, cap2.digest,
           sizeof(tunnel.path.destination_capability_digest));
    tunnel.path.local_parent_session.principal = cap2.principal;
    tunnel.path.local_parent_session.binding = cap2.binding;
    tunnel.path.local_parent_session.session_generation =
        cap2.session_generation;
    tunnel.path.local_parent_link_id = cap2.ingress_link_id;
    tunnel.path.local_parent_link_generation = cap2.ingress_link_generation;
    tunnel.path.local_parent_capability_generation =
        cap2.record.capability_generation;
    memcpy(tunnel.path.local_parent_capability_digest, cap2.digest,
           sizeof(tunnel.path.local_parent_capability_digest));
    tunnel.path.route_generation = 1U;
    tunnel.path.path_id = 1U;
    tunnel.path.path_generation = 1U;
    tunnel.path.hop_count = 1U;
    tunnel.path.path_frame_mtu = 192U;
    tunnel.path.payload_budget = 160U;
    tunnel.path.fragment_data_budget = 128U;
    tunnel.path.feature_bits = UCN_V6_FEATURE_CLUSTER;
    tunnel.path.hop_suite_bits =
        UINT32_C(1) << UCN_V6_SUITE_HMAC_SHA256_128;
    tunnel.path.e2e_suite_bits =
        UINT32_C(1) << UCN_V6_SUITE_AES_GCM_128;
    tunnel.path.max_message_class = UCN_V6_MESSAGE_T256;
    tunnel.path.max_window = 4U;
    tunnel.path.max_concurrency = 2U;
    tunnel.path.deadline_us = 100U;
    tunnel.deadline_us = 100U;
    CHECK(ucn_v6_cluster_tunnel_install(owner, &tunnel, &cap2, 1U) ==
          UCN_V6_OK);
    new_config.config_id = 2U;
    new_config.generation = 2U;
    CHECK(ucn_v6_cluster_prepare_joint(owner, 10U, &new_config, 2U) ==
          UCN_V6_OK);
    CHECK(ucn_v6_cluster_copy_snapshot(owner, &before_expiry) == UCN_V6_OK);
    memset(&message, 0, sizeof(message));
    message.kind = UCN_V6_CLUSTER_CTL_CONFIG_ACK;
    message.transaction_id = 10U;
    message.old_epoch = before_expiry.active_epoch;
    message.config_id = new_config.config_id;
    message.config_generation = new_config.generation;
    CHECK(bind_control_payload(&open2, &message, control_payload) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_copy_view(owner, 9U, &view) == UCN_V6_OK &&
          view.authority_active && view.members == 1U);
    CHECK(ucn_v6_capability_expire(fake.capability_owner,
                                   UINT64_C(1000000000)) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_copy_view(owner, UINT64_C(1000000000), &view) ==
              UCN_V6_OK &&
          !view.authority_active && view.members == 0U);
    CHECK(ucn_v6_cluster_backup_ack_config(
              owner, &open2, UINT64_C(1000000000)) ==
          UCN_V6_ERR_ACCESS);
    CHECK(ucn_v6_cluster_commit_joint(
              owner, 10U, UINT64_C(1000000000)) ==
          UCN_V6_ERR_ACCESS);
    CHECK(ucn_v6_cluster_copy_snapshot(owner, &after_expiry) == UCN_V6_OK);
    CHECK(memcmp(&before_expiry, &after_expiry, sizeof(before_expiry)) == 0);
    CHECK(ucn_v6_cluster_assign_backup(
              owner, &config.voters[1], 2U, UINT64_C(1000000000)) ==
          UCN_V6_ERR_STATE);
    memset(&copied, 0xA5, sizeof(copied));
    CHECK(ucn_v6_cluster_copy_tunnel(
              owner, 1U, UINT64_C(1000000000), &copied) ==
          UCN_V6_ERR_ACCESS);
    {
        ucn_v6_cluster_tunnel_t sentinel;
        memset(&sentinel, 0xA5, sizeof(sentinel));
        CHECK(memcmp(&copied, &sentinel, sizeof(copied)) == 0);
    }
    CHECK(ucn_v6_cluster_step(owner, UINT64_C(1000000000)) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_copy_view(
              owner, UINT64_C(1000000000), &view) == UCN_V6_OK &&
          view.members == 0U && !view.authority_active);
    return 0;
}

static int test_directory_authenticated_renewal_sequence(void)
{
    ucn_v6_cluster_owner_storage_t storage;
    ucn_v6_cluster_owner_t *owner = NULL;
    ucn_v6_callback_gate_t gate = {0};
    fake_store_t fake;
    ucn_v6_cluster_config_t config = config3();
    ucn_v6_security_open_result_t open2 = opened(0x20U, 2U);
    ucn_v6_cached_peer_capability_t cap2 = capability(0x20U, 2U);
    ucn_v6_cluster_directory_entry_t directory;
    ucn_v6_cluster_view_t view;
    uint8_t payload[UCN_V6_CLUSTER_DIRECTORY_BYTES];
    uint32_t accepted_sequence;

    memset(&fake, 0, sizeof(fake));
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, gate_lock, gate_unlock) ==
          UCN_V6_OK);
    CHECK(init_owner(&storage, &fake, &gate, 0x10U, 1U, &owner) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_create(owner, 1U, &config, 0U) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_admit_member(owner, &open2, &cap2, 1U, 100U) ==
          UCN_V6_OK);
    directory = directory_entry(&open2, 9U, 1U, 1U, 50U);
    CHECK(bind_directory_payload(&open2, &directory, payload) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_directory_install(owner, &open2, &cap2, 1U) ==
          UCN_V6_OK);

    CHECK(bind_directory_payload(&open2, &directory, payload) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_directory_install(owner, &open2, &cap2, 20U) ==
          UCN_V6_OK);
    accepted_sequence = open2.frame.origin_sequence;
    CHECK(ucn_v6_cluster_directory_install(owner, &open2, &cap2, 40U) ==
          UCN_V6_OK);
    open2.frame.origin_sequence = accepted_sequence - 1U;
    CHECK(ucn_v6_cluster_directory_install(owner, &open2, &cap2, 41U) ==
          UCN_V6_ERR_REPLAY);
    open2.frame.origin_sequence = accepted_sequence;
    CHECK(ucn_v6_cluster_copy_view(owner, 69U, &view) == UCN_V6_OK &&
          view.directory_entries == 1U);
    CHECK(ucn_v6_cluster_copy_view(owner, 70U, &view) == UCN_V6_OK &&
          view.directory_entries == 0U);
    return 0;
}

static int test_expired_member_slot_reclaims_after_invalidation(
    ucn_v6_stack_invalidation_type_t type)
{
    ucn_v6_cluster_owner_storage_t storage;
    ucn_v6_cluster_owner_t *owner = NULL;
    ucn_v6_callback_gate_t gate = {0};
    fake_store_t fake;
    ucn_v6_cluster_config_t config = config3();
    ucn_v6_security_open_result_t open2 = opened(0x20U, 2U);
    ucn_v6_cached_peer_capability_t cap2 = capability(0x20U, 2U);
    ucn_v6_cluster_directory_entry_t directory;
    ucn_v6_stack_invalidation_t invalidation;
    ucn_v6_cluster_view_t view;
    uint8_t payload[UCN_V6_CLUSTER_DIRECTORY_BYTES];

    memset(&fake, 0, sizeof(fake));
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, gate_lock, gate_unlock) ==
          UCN_V6_OK);
    CHECK(init_owner(&storage, &fake, &gate, 0x10U, 1U, &owner) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_create(owner, 1U, &config, 0U) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_admit_member(owner, &open2, &cap2, 1U, 5U) ==
          UCN_V6_OK);
    directory = directory_entry(&open2, 9U, 1U, 1U, 100U);
    CHECK(bind_directory_payload(&open2, &directory, payload) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_directory_install(owner, &open2, &cap2, 1U) ==
          UCN_V6_OK);
    CHECK(ucn_v6_cluster_step(owner, 6U) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_copy_view(owner, 6U, &view) == UCN_V6_OK &&
          view.members == 0U && view.directory_entries == 1U);

    memset(&invalidation, 0, sizeof(invalidation));
    invalidation.type = type;
    invalidation.link_id = cap2.ingress_link_id;
    invalidation.link_generation = cap2.ingress_link_generation;
    if (type == UCN_V6_STACK_INVALIDATE_SESSION) {
        invalidation.session = open2.ingress_peer_session;
    }
    CHECK(ucn_v6_cluster_apply_invalidation(owner, &invalidation) ==
          UCN_V6_OK);
    CHECK(ucn_v6_cluster_admit_member(owner, &open2, &cap2, 7U, 50U) ==
          UCN_V6_OK);
    directory.remote_cluster_id = 10U;
    directory.remote_epoch.cluster_id = 10U;
    CHECK(bind_directory_payload(&open2, &directory, payload) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_directory_install(owner, &open2, &cap2, 7U) ==
          UCN_V6_OK);

    ++open2.ingress_peer_session.session_generation;
    cap2.session_generation = open2.ingress_peer_session.session_generation;
    ++open2.ingress_link_instance_generation;
    cap2.ingress_link_generation =
        open2.ingress_link_instance_generation;
    cap2.record.capability_generation = 1U;
    CHECK(ucn_v6_capability_digest(&cap2.record, cap2.digest) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_admit_member(owner, &open2, &cap2, 7U, 50U) ==
          UCN_V6_ERR_SECURITY);
    return 0;
}

static int test_invalidation_without_derived_slot_is_not_a_permanent_fence(
    ucn_v6_stack_invalidation_type_t type)
{
    ucn_v6_cluster_owner_storage_t storage;
    ucn_v6_cluster_owner_t *owner = NULL;
    ucn_v6_callback_gate_t gate = {0};
    fake_store_t fake;
    ucn_v6_cluster_config_t config = config3();
    ucn_v6_security_open_result_t open2 = opened(0x20U, 2U);
    ucn_v6_cached_peer_capability_t cap2 = capability(0x20U, 2U);
    ucn_v6_stack_invalidation_t invalidation;

    memset(&fake, 0, sizeof(fake));
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, gate_lock, gate_unlock) ==
          UCN_V6_OK);
    CHECK(init_owner(&storage, &fake, &gate, 0x10U, 1U, &owner) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_create(owner, 1U, &config, 0U) == UCN_V6_OK);
    memset(&invalidation, 0, sizeof(invalidation));
    invalidation.type = type;
    invalidation.link_id = cap2.ingress_link_id;
    invalidation.link_generation = cap2.ingress_link_generation;
    if (type >= UCN_V6_STACK_INVALIDATE_SESSION) {
        invalidation.session = open2.ingress_peer_session;
    }
    if (type >= UCN_V6_STACK_INVALIDATE_CAPABILITY) {
        invalidation.capability_generation =
            cap2.record.capability_generation;
    }
    CHECK(ucn_v6_cluster_apply_invalidation(owner, &invalidation) ==
          UCN_V6_OK);
    CHECK(ucn_v6_cluster_admit_member(owner, &open2, &cap2, 1U, 50U) ==
          UCN_V6_OK);

    if (type == UCN_V6_STACK_INVALIDATE_CAPABILITY) {
        ++cap2.record.capability_generation;
    } else {
        ++open2.ingress_link_instance_generation;
        cap2.ingress_link_generation =
            open2.ingress_link_instance_generation;
        if (type == UCN_V6_STACK_INVALIDATE_SESSION) {
            ++open2.ingress_peer_session.session_generation;
            cap2.session_generation =
                open2.ingress_peer_session.session_generation;
        }
    }
    CHECK(ucn_v6_capability_digest(&cap2.record, cap2.digest) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_admit_member(owner, &open2, &cap2, 2U, 50U) ==
          (type == UCN_V6_STACK_INVALIDATE_CAPABILITY ?
               UCN_V6_OK : UCN_V6_ERR_SECURITY));
    return 0;
}

static int test_invalidation_is_local_and_live_owner_is_authority(void)
{
    ucn_v6_cluster_owner_storage_t storage;
    ucn_v6_cluster_owner_t *owner = NULL;
    ucn_v6_callback_gate_t gate = {0};
    fake_store_t fake;
    ucn_v6_cluster_config_t config = config3();
    ucn_v6_security_open_result_t open2 = opened(0x20U, 2U);
    ucn_v6_cached_peer_capability_t cap2 = capability(0x20U, 2U);
    test_cluster_tunnel_t tunnel;
    ucn_v6_cluster_directory_entry_t directory;
    ucn_v6_stack_invalidation_t invalidation;
    uint8_t payload[UCN_V6_CLUSTER_DIRECTORY_BYTES];

    memset(&fake, 0, sizeof(fake));
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, gate_lock, gate_unlock) ==
          UCN_V6_OK);
    CHECK(init_owner(&storage, &fake, &gate, 0x10U, 1U, &owner) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_create(owner, 1U, &config, 0U) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_admit_member(owner, &open2, &cap2, 1U, 100U) ==
          UCN_V6_OK);
    directory = directory_entry(&open2, 9U, 1U, 1U, 100U);
    CHECK(bind_directory_payload(&open2, &directory, payload) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_directory_install(owner, &open2, &cap2, 1U) ==
          UCN_V6_OK);
    tunnel = cluster_tunnel(&config, &cap2, 1U, 1U, 1U, 100U);
    CHECK(ucn_v6_cluster_tunnel_install(owner, &tunnel, &cap2, 1U) ==
          UCN_V6_OK);

    memset(&invalidation, 0, sizeof(invalidation));
    invalidation.type = UCN_V6_STACK_INVALIDATE_PATH;
    invalidation.link_id = cap2.ingress_link_id;
    invalidation.link_generation = cap2.ingress_link_generation;
    invalidation.session = open2.ingress_peer_session;
    invalidation.capability_generation = cap2.record.capability_generation;
    invalidation.path_id = 1U;
    invalidation.path_generation = 1U;
    CHECK(ucn_v6_cluster_apply_invalidation(owner, &invalidation) ==
          UCN_V6_OK);

    tunnel.tunnel_id = 2U;
    CHECK(ucn_v6_cluster_tunnel_install(owner, &tunnel, &cap2, 2U) ==
          UCN_V6_OK);
    directory.remote_cluster_id = 10U;
    directory.remote_epoch.cluster_id = 10U;
    CHECK(bind_directory_payload(&open2, &directory, payload) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_directory_install(owner, &open2, &cap2, 2U) ==
          UCN_V6_OK);

    ++tunnel.path.route_generation;
    tunnel.path.path_generation = 2U;
    tunnel.tunnel_id = 3U;
    CHECK(ucn_v6_cluster_tunnel_install(owner, &tunnel, &cap2, 2U) ==
          UCN_V6_OK);
    directory.path_generation = 2U;
    CHECK(bind_directory_payload(&open2, &directory, payload) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_directory_install(owner, &open2, &cap2, 2U) ==
          UCN_V6_OK);

    invalidation.path_id = 3U;
    invalidation.path_generation = 3U;
    CHECK(ucn_v6_cluster_apply_invalidation(owner, &invalidation) ==
          UCN_V6_OK);
    directory.remote_cluster_id = 11U;
    directory.remote_epoch.cluster_id = 11U;
    directory.path_id = 3U;
    directory.path_generation = 3U;
    CHECK(bind_directory_payload(&open2, &directory, payload) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_directory_install(owner, &open2, &cap2, 3U) ==
          UCN_V6_OK);
    directory.path_generation = 4U;
    CHECK(bind_directory_payload(&open2, &directory, payload) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_directory_install(owner, &open2, &cap2, 3U) ==
          UCN_V6_OK);
    return 0;
}

static int test_expired_directory_and_tunnel_slots_are_reclaimable(void)
{
    ucn_v6_cluster_owner_storage_t storage;
    ucn_v6_cluster_owner_t *owner = NULL;
    ucn_v6_callback_gate_t gate = {0};
    fake_store_t fake;
    ucn_v6_cluster_config_t config = config3();
    ucn_v6_security_open_result_t open2 = opened(0x20U, 2U);
    ucn_v6_cached_peer_capability_t cap2 = capability(0x20U, 2U);
    ucn_v6_cluster_directory_entry_t directory;
    test_cluster_tunnel_t tunnel;
    ucn_v6_stack_invalidation_t invalidation;
    ucn_v6_cluster_view_t view;
    uint8_t payload[UCN_V6_CLUSTER_DIRECTORY_BYTES];

    memset(&fake, 0, sizeof(fake));
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, gate_lock, gate_unlock) ==
          UCN_V6_OK);
    CHECK(init_owner(&storage, &fake, &gate, 0x10U, 1U, &owner) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_create(owner, 1U, &config, 0U) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_admit_member(owner, &open2, &cap2, 1U, 100U) ==
          UCN_V6_OK);
    directory = directory_entry(&open2, 9U, 1U, 1U, 4U);
    CHECK(bind_directory_payload(&open2, &directory, payload) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_directory_install(owner, &open2, &cap2, 1U) ==
          UCN_V6_OK);
    tunnel = cluster_tunnel(&config, &cap2, 1U, 1U, 1U, 5U);
    CHECK(ucn_v6_cluster_tunnel_install(owner, &tunnel, &cap2, 1U) ==
          UCN_V6_OK);
    CHECK(ucn_v6_cluster_step(owner, 5U) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_copy_view(owner, 5U, &view) == UCN_V6_OK &&
          view.directory_entries == 0U && view.tunnels == 0U);

    memset(&invalidation, 0, sizeof(invalidation));
    invalidation.type = UCN_V6_STACK_INVALIDATE_PATH;
    invalidation.link_id = cap2.ingress_link_id;
    invalidation.link_generation = cap2.ingress_link_generation;
    invalidation.session = open2.ingress_peer_session;
    invalidation.capability_generation = cap2.record.capability_generation;
    invalidation.path_id = 1U;
    invalidation.path_generation = 1U;
    CHECK(ucn_v6_cluster_apply_invalidation(owner, &invalidation) ==
          UCN_V6_OK);

    directory = directory_entry(&open2, 10U, 1U, 1U, 50U);
    CHECK(bind_directory_payload(&open2, &directory, payload) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_directory_install(owner, &open2, &cap2, 6U) ==
          UCN_V6_OK);
    tunnel = cluster_tunnel(&config, &cap2, 2U, 1U, 1U, 50U);
    tunnel.destination_cluster_id = 10U;
    CHECK(ucn_v6_cluster_tunnel_install(owner, &tunnel, &cap2, 6U) ==
          UCN_V6_OK);

    directory.path_generation = 2U;
    CHECK(bind_directory_payload(&open2, &directory, payload) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_directory_install(owner, &open2, &cap2, 6U) ==
          UCN_V6_OK);
    ++tunnel.path.route_generation;
    tunnel.path.path_generation = 2U;
    tunnel.tunnel_id = 3U;
    CHECK(ucn_v6_cluster_tunnel_install(owner, &tunnel, &cap2, 6U) ==
          UCN_V6_OK);
    return 0;
}

static int test_directory_requires_bound_authority_proof(void)
{
    ucn_v6_cluster_owner_storage_t storage;
    ucn_v6_cluster_owner_t *owner = NULL;
    ucn_v6_cluster_owner_storage_t reload_storage;
    ucn_v6_cluster_owner_t *reloaded = NULL;
    ucn_v6_cluster_owner_storage_t no_proof_storage;
    ucn_v6_cluster_owner_t *no_proof_owner = NULL;
    ucn_v6_callback_gate_t gate = {0};
    ucn_v6_callback_gate_t reload_gate = {0};
    ucn_v6_callback_gate_t no_proof_gate = {0};
    fake_store_t fake;
    fake_store_t no_proof_fake;
    ucn_v6_cluster_config_t config = config3();
    ucn_v6_security_open_result_t open2 = opened(0x20U, 2U);
    ucn_v6_cached_peer_capability_t cap2 = capability(0x20U, 2U);
    ucn_v6_cluster_directory_entry_t directory =
        directory_entry(&open2, 9U, 2U, 3U, 100U);
    ucn_v6_cluster_directory_entry_t decoded;
    ucn_v6_cluster_directory_entry_t replay;
    ucn_v6_cluster_view_t before;
    ucn_v6_cluster_view_t view;
    uint8_t payload[UCN_V6_CLUSTER_DIRECTORY_BYTES];
    ucn_v6_capability_peer_ref_t peer_ref;

    memset(&fake, 0, sizeof(fake));
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, gate_lock, gate_unlock) ==
          UCN_V6_OK);
    CHECK(init_owner(&storage, &fake, &gate, 0x10U, 1U, &owner) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_create(owner, 1U, &config, 0U) == UCN_V6_OK);
    directory.route_generation = 3U;
    CHECK(bind_directory_payload(&open2, &directory, payload) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_directory_decode(payload, sizeof(payload),
                                           &decoded) == UCN_V6_OK);
    fake.proof_manual = true;

    /* NOT_FOUND is fail-closed and must not create a hint-like entry. */
    fake.proof_valid = false;
    CHECK(ucn_v6_cluster_directory_install(owner, &open2, &cap2, 1U) ==
          UCN_V6_ERR_SECURITY);
    CHECK(ucn_v6_cluster_copy_view(owner, 1U, &view) == UCN_V6_OK &&
          view.directory_entries == 0U);

    /* A valid certificate handle cannot be transplanted to another Epoch,
     * Route or Path, and unverified quorum never grants Directory authority. */
    set_fake_authority_proof(&fake, &decoded, 10U);
    ++fake.proof.epoch.term;
    CHECK(ucn_v6_cluster_directory_install(owner, &open2, &cap2, 1U) ==
          UCN_V6_ERR_SECURITY);
    set_fake_authority_proof(&fake, &decoded, 10U);
    ++fake.proof.path_generation;
    CHECK(ucn_v6_cluster_directory_install(owner, &open2, &cap2, 1U) ==
          UCN_V6_ERR_SECURITY);
    set_fake_authority_proof(&fake, &decoded, 10U);
    fake.proof.stable_quorum_verified = false;
    CHECK(ucn_v6_cluster_directory_install(owner, &open2, &cap2, 1U) ==
          UCN_V6_ERR_SECURITY);

    set_fake_authority_proof(&fake, &decoded, 10U);
    CHECK(ucn_v6_cluster_copy_view(owner, 1U, &before) == UCN_V6_OK);
    fake.reenter_owner = owner;
    CHECK(ucn_v6_cluster_directory_install(owner, &open2, &cap2, 1U) ==
          UCN_V6_ERR_SECURITY);
    CHECK(fake.proof_reenter_result == UCN_V6_ERR_STATE);
    fake.reenter_owner = NULL;
    CHECK(ucn_v6_cluster_copy_view(owner, 1U, &view) == UCN_V6_OK &&
          view.directory_entries == 0U &&
          view.rejected_security == before.rejected_security + 1U &&
          view.rejected_quorum == before.rejected_quorum &&
          view.rejected_replay == before.rejected_replay);

    fake.callback_gate = &gate;
    fake.callback_scope_owner = owner;
    fake.force_gate_leave_on_proof = true;
    CHECK(ucn_v6_cluster_directory_install(owner, &open2, &cap2, 1U) ==
          UCN_V6_ERR_SECURITY);
    CHECK(fake.forced_gate_leave_result == UCN_V6_OK);
    CHECK(ucn_v6_cluster_copy_view(owner, 1U, &view) == UCN_V6_OK &&
          view.directory_entries == 0U);
    fake.force_gate_leave_on_proof = false;
    fake.callback_gate = NULL;
    fake.callback_scope_owner = NULL;
    CHECK(ucn_v6_cluster_directory_install(owner, &open2, &cap2, 1U) ==
          UCN_V6_OK);

    /* Same-Epoch Route and Path generations have independent non-rollback
     * rules.  A higher value in the other axis cannot mask either rollback. */
    replay = decoded;
    replay.route_generation = 2U;
    replay.path_generation = 4U;
    replay.authority_proof.generation = 4U;
    CHECK(bind_directory_payload(&open2, &replay, payload) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_directory_decode(payload, sizeof(payload),
                                           &replay) == UCN_V6_OK);
    set_fake_authority_proof(&fake, &replay, 10U);
    CHECK(ucn_v6_cluster_directory_install(owner, &open2, &cap2, 1U) ==
          UCN_V6_ERR_REPLAY);
    replay = decoded;
    replay.route_generation = 4U;
    replay.path_generation = 2U;
    replay.authority_proof.generation = 2U;
    CHECK(bind_directory_payload(&open2, &replay, payload) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_directory_decode(payload, sizeof(payload),
                                           &replay) == UCN_V6_OK);
    set_fake_authority_proof(&fake, &replay, 10U);
    CHECK(ucn_v6_cluster_directory_install(owner, &open2, &cap2, 1U) ==
          UCN_V6_ERR_REPLAY);
    replay = decoded;
    replay.route_generation = 4U;
    replay.path_generation = 4U;
    ++replay.authority_proof.proof_id;
    replay.authority_proof.generation = 4U;
    CHECK(bind_directory_payload(&open2, &replay, payload) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_directory_decode(payload, sizeof(payload),
                                           &replay) == UCN_V6_OK);
    set_fake_authority_proof(&fake, &replay, 10U);
    CHECK(ucn_v6_cluster_directory_install(owner, &open2, &cap2, 1U) ==
          UCN_V6_ERR_REPLAY);

    CHECK(ucn_v6_cluster_copy_view(owner, 9U, &view) == UCN_V6_OK &&
          view.directory_entries == 1U);
    CHECK(ucn_v6_cluster_copy_view(owner, 10U, &view) == UCN_V6_OK &&
          view.directory_entries == 0U);

    /* Directory state is deliberately volatile.  Restart never resurrects a
     * prior entry and a missing proof after restart cannot fall back to it. */
    CHECK(ucn_v6_callback_gate_init(&reload_gate, NULL, gate_lock,
                                    gate_unlock) == UCN_V6_OK);
    CHECK(init_owner(&reload_storage, &fake, &reload_gate, 0x10U, 1U,
                     &reloaded) == UCN_V6_OK);
    fake.proof_valid = false;
    CHECK(ucn_v6_cluster_directory_install(reloaded, &open2, &cap2, 1U) ==
          UCN_V6_ERR_SECURITY);
    CHECK(ucn_v6_cluster_copy_view(reloaded, 1U, &view) == UCN_V6_OK &&
          view.directory_entries == 0U);

    /* Cluster can exist without a Directory verifier, but authority-bearing
     * Directory input is then unconditionally fail-closed. */
    memset(&no_proof_fake, 0, sizeof(no_proof_fake));
    CHECK(ucn_v6_callback_gate_init(&no_proof_gate, NULL, gate_lock,
                                    gate_unlock) == UCN_V6_OK);
    CHECK(init_owner_without_authority_proof(
              &no_proof_storage, &no_proof_fake, &no_proof_gate,
              &no_proof_owner) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_create(no_proof_owner, 1U, &config, 0U) ==
          UCN_V6_OK);
    peer_ref = peer_ref_from_capability(&cap2);
    CHECK((ucn_v6_cluster_directory_install)(
              no_proof_owner, &open2, &peer_ref, 1U) ==
          UCN_V6_ERR_SECURITY);
    CHECK(ucn_v6_cluster_copy_view(no_proof_owner, 1U, &view) == UCN_V6_OK &&
          view.directory_entries == 0U);
    return 0;
}

static int test_multihop_source_identity_is_not_last_hop_identity(void)
{
    ucn_v6_cluster_owner_storage_t storage;
    ucn_v6_cluster_owner_t *owner = NULL;
    ucn_v6_callback_gate_t gate = {0};
    fake_store_t fake;
    ucn_v6_cluster_config_t config = config3();
    ucn_v6_security_open_result_t source = opened(0x20U, 2U);
    ucn_v6_security_open_result_t bad;
    ucn_v6_cached_peer_capability_t source_capability =
        capability(0x20U, 2U);
    ucn_v6_cached_peer_capability_t relay_capability =
        capability(0x70U, 70U);
    ucn_v6_capability_peer_ref_t relay_ref;
    ucn_v6_cluster_directory_entry_t directory;
    ucn_v6_cluster_directory_entry_t decoded_directory;
    ucn_v6_cluster_view_t view;
    uint8_t payload[UCN_V6_CLUSTER_DIRECTORY_BYTES];

    memset(&fake, 0, sizeof(fake));
    CHECK(ucn_v6_callback_gate_init(&gate, NULL, gate_lock, gate_unlock) ==
          UCN_V6_OK);
    CHECK(init_owner(&storage, &fake, &gate, 0x10U, 1U, &owner) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_create(owner, 1U, &config, 0U) == UCN_V6_OK);
    CHECK(ensure_capability_peer(&fake, 0x70U, 70U) == UCN_V6_OK);
    relay_ref = peer_ref_from_capability(&relay_capability);

    /* Security authenticated the original E2E Source, while Hop Auth belongs
     * to a distinct last-hop Relay.  Cluster membership must use the immutable
     * Wire Source identity and retain the Relay only as ingress/next-hop. */
    source.ingress_peer_session.principal = principal(0x70U);
    source.ingress_peer_session.binding = binding(70U);
    source.ingress_peer_session.session_generation =
        relay_capability.session_generation;
    CHECK(ucn_v6_cluster_admit_member(
              owner, &source, &source_capability, 1U, 100U) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_copy_view(owner, 1U, &view) == UCN_V6_OK &&
          view.members == 1U);

    bad = source;
    bad.frame.realm_id = 2U;
    CHECK(ucn_v6_cluster_admit_member(
              owner, &bad, &source_capability, 1U, 100U) ==
          UCN_V6_ERR_SECURITY);
    CHECK(ucn_v6_cluster_copy_view(owner, 1U, &view) == UCN_V6_OK &&
          view.members == 1U);

    directory = directory_entry(&source, 9U, 2U, 3U, 100U);
    CHECK(bind_directory_payload(&source, &directory, payload) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_directory_decode(
              payload, sizeof(payload), &decoded_directory) == UCN_V6_OK);
    set_fake_authority_proof(&fake, &decoded_directory, UINT64_MAX);
    CHECK((ucn_v6_cluster_directory_install)(
              owner, &source, &relay_ref, 2U) == UCN_V6_OK);
    CHECK(ucn_v6_cluster_copy_view(owner, 2U, &view) == UCN_V6_OK &&
          view.directory_entries == 1U);

    bad = source;
    ++bad.frame.source_binding_generation;
    CHECK((ucn_v6_cluster_directory_install)(
              owner, &bad, &relay_ref, 2U) ==
          UCN_V6_ERR_SECURITY);
    CHECK(ucn_v6_cluster_copy_view(owner, 2U, &view) == UCN_V6_OK &&
          view.directory_entries == 1U);
    return 0;
}

int main(void)
{
    CHECK(test_record_and_control_codec() == 0);
    CHECK(test_record_validation_ignores_c_padding() == 0);
    CHECK(test_joint_authority_directory_and_tunnel() == 0);
    CHECK(test_takeover_recovery_rekey_and_handover() == 0);
    CHECK(test_callback_reentry_and_failure_close() == 0);
    CHECK(test_witness_rollback_torn_write_and_capability_expiry() == 0);
    CHECK(test_member_dependency_expiry_revokes_all_uses() == 0);
    CHECK(test_directory_authenticated_renewal_sequence() == 0);
    CHECK(test_expired_member_slot_reclaims_after_invalidation(
              UCN_V6_STACK_INVALIDATE_SESSION) == 0);
    CHECK(test_expired_member_slot_reclaims_after_invalidation(
              UCN_V6_STACK_INVALIDATE_LINK) == 0);
    CHECK(test_invalidation_without_derived_slot_is_not_a_permanent_fence(
              UCN_V6_STACK_INVALIDATE_CAPABILITY) == 0);
    CHECK(test_invalidation_without_derived_slot_is_not_a_permanent_fence(
              UCN_V6_STACK_INVALIDATE_SESSION) == 0);
    CHECK(test_invalidation_without_derived_slot_is_not_a_permanent_fence(
              UCN_V6_STACK_INVALIDATE_LINK) == 0);
    CHECK(test_invalidation_is_local_and_live_owner_is_authority() == 0);
    CHECK(test_expired_directory_and_tunnel_slots_are_reclaimable() == 0);
    CHECK(test_directory_requires_bound_authority_proof() == 0);
    CHECK(test_multihop_source_identity_is_not_last_hop_identity() == 0);
    puts("ucn v6 cluster tests passed");
    return 0;
}
