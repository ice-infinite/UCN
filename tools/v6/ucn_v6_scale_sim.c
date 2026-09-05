#include "ucn/v6/ucn_v6_cluster.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct scale_store {
    bool valid;
    uint64_t generation_witness;
    uint8_t bytes[UCN_V6_CLUSTER_RECORD_BYTES];
} scale_store_t;

typedef struct scale_cluster {
    ucn_v6_cluster_owner_storage_t storage;
    ucn_v6_capability_owner_storage_t capability_storage;
    ucn_v6_capability_owner_t *capability_owner;
    ucn_v6_route_owner_storage_t route_storage;
    ucn_v6_route_owner_t *route_owner;
    ucn_v6_callback_gate_t gate;
    scale_store_t store;
} scale_cluster_t;

static void gate_lock(void *context) { (void)context; }
static void gate_unlock(void *context) { (void)context; }

static ucn_v6_result_t store_load_witness(void *context,
                                          uint64_t *generation)
{
    scale_store_t *store = (scale_store_t *)context;
    if (store->generation_witness == 0U) return UCN_V6_ERR_NOT_FOUND;
    *generation = store->generation_witness;
    return UCN_V6_OK;
}

static ucn_v6_result_t store_reserve_witness(void *context,
                                             uint64_t generation)
{
    scale_store_t *store = (scale_store_t *)context;
    if (generation == 0U || generation <= store->generation_witness) {
        return UCN_V6_ERR_REPLAY;
    }
    store->generation_witness = generation;
    return UCN_V6_OK;
}

static ucn_v6_result_t store_load(void *context, uint8_t *record,
                                  size_t capacity, size_t *length)
{
    scale_store_t *store = (scale_store_t *)context;
    if (!store->valid) return UCN_V6_ERR_NOT_FOUND;
    if (capacity < sizeof(store->bytes)) return UCN_V6_ERR_NO_SPACE;
    memcpy(record, store->bytes, sizeof(store->bytes));
    *length = sizeof(store->bytes);
    return UCN_V6_OK;
}

static ucn_v6_result_t store_submit(void *context, const uint8_t *record,
                                    size_t length)
{
    scale_store_t *store = (scale_store_t *)context;
    if (length != sizeof(store->bytes)) return UCN_V6_ERR_MALFORMED;
    memcpy(store->bytes, record, sizeof(store->bytes));
    store->valid = true;
    return UCN_V6_OK;
}

static ucn_v6_principal_t principal(uint32_t node_address)
{
    ucn_v6_principal_t value;
    memset(&value, 0, sizeof(value));
    value.bytes[12] = (uint8_t)(node_address >> 24U);
    value.bytes[13] = (uint8_t)(node_address >> 16U);
    value.bytes[14] = (uint8_t)(node_address >> 8U);
    value.bytes[15] = (uint8_t)node_address;
    return value;
}

static ucn_v6_binding_key_t binding(uint32_t address)
{
    ucn_v6_binding_key_t value = { 1U, address, 1U };
    return value;
}

static ucn_v6_cluster_voter_t voter(uint32_t address)
{
    ucn_v6_cluster_voter_t value;
    value.principal = principal(address);
    value.binding = binding(address);
    return value;
}

static ucn_v6_security_open_result_t opened(uint32_t address)
{
    ucn_v6_security_open_result_t value;
    memset(&value, 0, sizeof(value));
    value.authenticated_principal = principal(address);
    value.ingress_peer_session.principal = value.authenticated_principal;
    value.ingress_peer_session.binding = binding(address);
    value.ingress_peer_session.session_generation = 1U;
    value.ingress_link_instance_id = 1U;
    value.ingress_link_instance_generation = 1U;
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

static ucn_v6_cached_peer_capability_t capability(uint32_t address)
{
    ucn_v6_cached_peer_capability_t value;
    memset(&value, 0, sizeof(value));
    value.valid = true;
    value.principal = principal(address);
    value.binding = binding(address);
    value.session_generation = 1U;
    value.ingress_link_id = 1U;
    value.ingress_link_generation = 1U;
    value.record.capability_generation = 1U;
    value.record.link.link_instance_generation = 1U;
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

static ucn_v6_result_t cache_capability(scale_cluster_t *fixture,
                                        uint32_t address)
{
    ucn_v6_cached_peer_capability_t peer = capability(address);
    ucn_v6_security_open_result_t authenticated = opened(address);
    uint8_t payload[UCN_V6_CAPABILITY_RECORD_BYTES];
    if (ucn_v6_capability_record_encode(&peer.record, payload) != UCN_V6_OK) {
        return UCN_V6_ERR_STATE;
    }
    authenticated.frame.flags = UCN_V6_FLAG_PEER_HOP_CONTEXT |
                                UCN_V6_FLAG_PROTOCOL_CONTEXT;
    authenticated.frame.frame_type = UCN_V6_FRAME_CONTROL;
    authenticated.frame.realm_id = peer.binding.realm_id;
    authenticated.frame.source_address = peer.binding.node_address;
    authenticated.frame.source_binding_generation =
        peer.binding.binding_generation;
    authenticated.frame.session_generation = peer.session_generation;
    authenticated.frame.protocol_opcode =
        UCN_V6_PROTOCOL_OPCODE_CAPABILITY_ADVERTISE;
    authenticated.frame.payload = payload;
    authenticated.frame.payload_length = sizeof(payload);
    authenticated.endpoint_authorized = false;
    return ucn_v6_capability_ingest_advertise(
        fixture->capability_owner, 0U, &authenticated, &peer.record);
}

static int initialize_cluster(
    scale_cluster_t *fixture,
    uint32_t cluster_id,
    uint32_t first_address,
    bool reload)
{
    ucn_v6_cluster_store_ops_t store_ops;
    ucn_v6_cluster_owner_t *owner = NULL;
    ucn_v6_principal_t local = principal(first_address);
    ucn_v6_binding_key_t local_binding = binding(first_address);
    ucn_v6_cluster_config_t config;
    ucn_v6_cluster_view_t view;
    ucn_v6_security_open_result_t peer;
    ucn_v6_cached_peer_capability_t peer_capability;
    ucn_v6_capability_peer_ref_t peer_ref;
    memset(&store_ops, 0, sizeof(store_ops));
    store_ops.context = &fixture->store;
    store_ops.load_generation_witness = store_load_witness;
    store_ops.reserve_generation_witness = store_reserve_witness;
    store_ops.load = store_load;
    store_ops.submit = store_submit;
    if (!reload &&
        ucn_v6_callback_gate_init(
            &fixture->gate, NULL, gate_lock, gate_unlock) != UCN_V6_OK) return 1;
    if (!reload) {
        ucn_v6_cached_peer_capability_t local_capability =
            capability(first_address);
        if (ucn_v6_capability_owner_init_in_place(
                fixture->capability_storage.bytes,
                sizeof(fixture->capability_storage),
                ucn_v6_compiled_manifest(), &local_capability.record,
                UINT64_C(1000000000), UINT64_C(1000000000),
                &fixture->capability_owner) != UCN_V6_OK ||
            ucn_v6_route_owner_init_in_place(
                fixture->route_storage.bytes,
                sizeof(fixture->route_storage),
                ucn_v6_compiled_manifest(), fixture->capability_owner,
                1000U, 100U, 4U, 100U, 100U,
                &fixture->route_owner) != UCN_V6_OK ||
            cache_capability(fixture, first_address + 1U) != UCN_V6_OK) {
            return 1;
        }
    }
    if (ucn_v6_cluster_owner_init_in_place(
            &fixture->storage, sizeof(fixture->storage),
            ucn_v6_compiled_manifest(), &local, &local_binding, 1U,
            fixture->capability_owner, fixture->route_owner, NULL,
            &store_ops, &fixture->gate, &owner) != UCN_V6_OK) return 1;
    if (reload) {
        if (ucn_v6_cluster_copy_view(owner, 0U, &view) != UCN_V6_OK ||
            view.authority_active || view.members != 0U) return 1;
        return 0;
    }
    memset(&config, 0, sizeof(config));
    config.valid = true;
    config.config_id = 1U;
    config.generation = 1U;
    config.voter_count = 3U;
    config.voters[0] = voter(first_address);
    config.voters[1] = voter(first_address + 1U);
    config.voters[2] = voter(first_address + 2U);
    if (ucn_v6_cluster_create(owner, cluster_id, &config, 0U) != UCN_V6_OK) {
        return 1;
    }
    peer = opened(first_address + 1U);
    peer_capability = capability(first_address + 1U);
    memset(&peer_ref, 0, sizeof(peer_ref));
    peer_ref.principal = peer_capability.principal;
    peer_ref.binding = peer_capability.binding;
    peer_ref.session_generation = peer_capability.session_generation;
    peer_ref.ingress_link_id = peer_capability.ingress_link_id;
    peer_ref.ingress_link_generation =
        peer_capability.ingress_link_generation;
    if (ucn_v6_cluster_admit_member(
            owner, &peer, &peer_ref, 1U, 1000000U) != UCN_V6_OK) {
        return 1;
    }
    if (ucn_v6_cluster_copy_view(owner, 1U, &view) != UCN_V6_OK ||
        !view.authority_active || !view.quorum_met) return 1;
    return 0;
}

int main(int argc, char **argv)
{
    unsigned long requested;
    size_t nodes;
    size_t clusters;
    size_t index;
    size_t reloads = 0U;
    scale_cluster_t *fixtures;
    char *end = NULL;
    if (argc != 2) return 2;
    requested = strtoul(argv[1], &end, 10);
    if (end == argv[1] || *end != '\0' || requested < 3UL ||
        requested > 10000UL) return 2;
    nodes = (size_t)requested;
    clusters = (nodes + UCN_V6_CONFIG_CLUSTER_MEMBERS - 1U) /
               UCN_V6_CONFIG_CLUSTER_MEMBERS;
    fixtures = (scale_cluster_t *)calloc(clusters, sizeof(*fixtures));
    if (fixtures == NULL) return 3;
    for (index = 0U; index < clusters; ++index) {
        uint32_t first_address =
            (uint32_t)(index * UCN_V6_CONFIG_CLUSTER_MEMBERS + 1U);
        if (initialize_cluster(&fixtures[index], (uint32_t)index + 1U,
                               first_address, false) != 0) {
            free(fixtures);
            return 4;
        }
        if ((index % 17U) == 0U) {
            if (initialize_cluster(&fixtures[index], (uint32_t)index + 1U,
                                   first_address, true) != 0) {
                free(fixtures);
                return 5;
            }
            ++reloads;
        }
    }
    printf("v6_scale nodes=%zu clusters=%zu max_members=%u reloads=%zu "
           "max_cluster_hops=%zu\n",
           nodes, clusters, (unsigned)UCN_V6_CONFIG_CLUSTER_MEMBERS,
           reloads, clusters > 0U ? clusters - 1U : 0U);
    free(fixtures);
    return 0;
}
