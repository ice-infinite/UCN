#include "ucn/v6/ucn_v6_cluster.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct scale_store {
    bool valid;
    uint8_t bytes[UCN_V6_CLUSTER_RECORD_BYTES];
} scale_store_t;

typedef struct scale_cluster {
    ucn_v6_cluster_owner_storage_t storage;
    ucn_v6_callback_gate_t gate;
    scale_store_t store;
} scale_cluster_t;

static void gate_lock(void *context) { (void)context; }
static void gate_unlock(void *context) { (void)context; }

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
    value.frame.flags = UCN_V6_FLAG_E2E_CONTEXT |
                        UCN_V6_FLAG_PEER_HOP_CONTEXT;
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
    value.record.capability_generation = 1U;
    value.record.peer.feature_bits = UCN_V6_FEATURE_CLUSTER;
    return value;
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
    memset(&store_ops, 0, sizeof(store_ops));
    store_ops.context = &fixture->store;
    store_ops.load = store_load;
    store_ops.submit = store_submit;
    if (ucn_v6_callback_gate_init(
            &fixture->gate, NULL, gate_lock, gate_unlock) != UCN_V6_OK) return 1;
    if (ucn_v6_cluster_owner_init_in_place(
            &fixture->storage, sizeof(fixture->storage),
            ucn_v6_compiled_manifest(), &local, &local_binding, 1U,
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
    if (ucn_v6_cluster_admit_member(
            owner, &peer, &peer_capability, 1U, 1000000U) != UCN_V6_OK) {
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
