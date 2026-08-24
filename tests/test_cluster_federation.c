#include "test_support.h"

#include <string.h>

#include "ucn/ucn_cluster_authority.h"
#include "ucn/ucn_cluster_federation.h"
#include "ucn/ucn_cluster_storage.h"

static void federation_test_common(ucn_cluster_federation_message_t *message,
                                   ucn_cluster_federation_kind_t kind)
{
    (void)memset(message, 0, sizeof(*message));
    message->kind = kind;
    message->hop_limit = 3U;
    message->transaction_id = UINT32_C(0x10203040);
}

static int federation_test_round_trip(
    const ucn_cluster_federation_message_t *message)
{
    uint8_t output[UCN_MAX_PAYLOAD_BYTES];
    ucn_cluster_federation_message_t decoded;
    size_t output_length = 0U;
    size_t expected = ucn_cluster_federation_message_encoded_size(message);

    TEST_ASSERT(expected != 0U && expected <= sizeof(output));
    TEST_ASSERT(ucn_cluster_federation_message_encode(
                    message, output, sizeof(output), &output_length) == UCN_OK);
    TEST_ASSERT(output_length == expected);
    TEST_ASSERT(ucn_cluster_federation_message_decode(output, output_length,
                                                       &decoded) == UCN_OK);
    TEST_ASSERT(decoded.kind == message->kind &&
                decoded.hop_limit == message->hop_limit &&
                decoded.transaction_id == message->transaction_id);
    return 0;
}

static int federation_test_locator_and_query(void)
{
    ucn_cluster_federation_message_t message;
    uint8_t output[UCN_CLUSTER_FEDERATION_LOCATOR_BYTES];
    size_t output_length = 0U;

    federation_test_common(&message, UCN_CLUSTER_FED_KIND_LOCATOR_REGISTER);
    message.body.locator.node_id = UINT32_C(0x10000004);
    message.body.locator.cluster_id = UINT32_C(0x10000002);
    message.body.locator.head_node_id = UINT32_C(0x10000002);
    message.body.locator.term = 3U;
    message.body.locator.lease_ms = 1000U;
    message.body.locator.record_nonce = 7U;
    TEST_ASSERT(federation_test_round_trip(&message) == 0);
    TEST_ASSERT(ucn_cluster_federation_message_encode(
                    &message, output, sizeof(output) - 1U, &output_length) ==
                UCN_ERR_TOO_LARGE);

    message.kind = UCN_CLUSTER_FED_KIND_LOCATOR_WITHDRAW;
    message.body.locator.lease_ms = 0U;
    TEST_ASSERT(federation_test_round_trip(&message) == 0);
    message.kind = UCN_CLUSTER_FED_KIND_LOCATOR_REPLY;
    message.body.locator.lease_ms = 1000U;
    TEST_ASSERT(federation_test_round_trip(&message) == 0);

    federation_test_common(&message, UCN_CLUSTER_FED_KIND_LOCATOR_QUERY);
    message.body.query.target_node_id = UINT32_C(0x10000004);
    message.body.query.requester_cluster_id = UINT32_C(0x10000001);
    message.body.query.requester_head_node_id = UINT32_C(0x10000001);
    TEST_ASSERT(federation_test_round_trip(&message) == 0);
    return 0;
}

static int federation_test_tunnel_messages(void)
{
    static const uint8_t payload[] = { 0xA5U, 0x5AU, 0x11U };
    ucn_cluster_federation_message_t message;
    uint8_t output[UCN_MAX_PAYLOAD_BYTES];
    ucn_cluster_federation_message_t decoded;
    size_t output_length = 0U;

    federation_test_common(&message, UCN_CLUSTER_FED_KIND_TUNNEL_SUBMIT);
    message.body.submit.final_node_id = UINT32_C(0x10000004);
    message.body.submit.endpoint = 0x40U;
    message.body.submit.traffic_class = UCN_TRAFFIC_Q1_REALTIME;
    message.body.submit.inner_payload = payload;
    message.body.submit.inner_length = (uint16_t)sizeof(payload);
    TEST_ASSERT(federation_test_round_trip(&message) == 0);

    federation_test_common(&message, UCN_CLUSTER_FED_KIND_TUNNEL_DATA);
    message.body.tunnel.origin_node_id = UINT32_C(0x10000001);
    message.body.tunnel.final_node_id = UINT32_C(0x10000004);
    message.body.tunnel.origin_cluster_id = UINT32_C(0x10000002);
    message.body.tunnel.destination_cluster_id = UINT32_C(0x10000003);
    message.body.tunnel.endpoint = 0x41U;
    message.body.tunnel.traffic_class = UCN_TRAFFIC_Q0_CRITICAL;
    message.body.tunnel.inner_payload = payload;
    message.body.tunnel.inner_length = (uint16_t)sizeof(payload);
    TEST_ASSERT(federation_test_round_trip(&message) == 0);
    TEST_ASSERT(ucn_cluster_federation_message_encode(
                    &message, output, sizeof(output), &output_length) == UCN_OK);
    TEST_ASSERT(ucn_cluster_federation_message_decode(output, output_length,
                                                       &decoded) == UCN_OK);
    TEST_ASSERT(decoded.body.tunnel.inner_length == sizeof(payload) &&
                memcmp(decoded.body.tunnel.inner_payload, payload,
                       sizeof(payload)) == 0);
    output[2] = 1U;
    TEST_ASSERT(ucn_cluster_federation_message_decode(output, output_length,
                                                       &decoded) == UCN_ERR_MALFORMED);

    federation_test_common(&message, UCN_CLUSTER_FED_KIND_TUNNEL_DELIVER);
    message.body.delivery.origin_node_id = UINT32_C(0x10000001);
    message.body.delivery.final_node_id = UINT32_C(0x10000004);
    message.body.delivery.origin_cluster_id = UINT32_C(0x10000002);
    message.body.delivery.destination_cluster_id = UINT32_C(0x10000003);
    message.body.delivery.endpoint = 0x42U;
    message.body.delivery.traffic_class = UCN_TRAFFIC_Q1_REALTIME;
    message.body.delivery.inner_payload = payload;
    message.body.delivery.inner_length = (uint16_t)sizeof(payload);
    TEST_ASSERT(federation_test_round_trip(&message) == 0);

    federation_test_common(&message, UCN_CLUSTER_FED_KIND_TUNNEL_ERROR);
    message.body.error.error = UCN_CLUSTER_FED_ERROR_DIRECTORY_STALE;
    message.body.error.origin_node_id = UINT32_C(0x10000001);
    message.body.error.final_node_id = UINT32_C(0x10000004);
    TEST_ASSERT(federation_test_round_trip(&message) == 0);
    return 0;
}

static int federation_test_malformed_and_cluster_view(void)
{
    ucn_cluster_federation_message_t message;
    ucn_cluster_federation_message_t decoded;
    ucn_cluster_t cluster;
    ucn_cluster_config_t config;
    ucn_cluster_view_t view;
    ucn_cluster_member_summary_t summary[1];
    uint8_t output[UCN_CLUSTER_FEDERATION_QUERY_BYTES];
    uint8_t oversized_payload[UCN_MAX_PAYLOAD_BYTES];
    size_t output_length = 0U;

    federation_test_common(&message, UCN_CLUSTER_FED_KIND_LOCATOR_QUERY);
    message.body.query.target_node_id = UINT32_C(0x10000004);
    message.body.query.requester_cluster_id = UINT32_C(0x10000001);
    message.body.query.requester_head_node_id = UINT32_C(0x10000001);
    TEST_ASSERT(ucn_cluster_federation_message_encode(
                    &message, output, sizeof(output), &output_length) == UCN_OK);
    output[0]++;
    TEST_ASSERT(ucn_cluster_federation_message_decode(output, output_length,
                                                       &decoded) == UCN_ERR_MALFORMED);
    output[0]--;
    output[3] = 0U;
    TEST_ASSERT(ucn_cluster_federation_message_decode(output, output_length,
                                                       &decoded) == UCN_ERR_MALFORMED);
    output[3] = 3U;
    output[1] = UCN_CLUSTER_FED_KIND_NEXT_CLUSTER_ANNOUNCE;
    TEST_ASSERT(ucn_cluster_federation_message_decode(output, output_length,
                                                       &decoded) == UCN_ERR_UNSUPPORTED);
    output[1] = UCN_CLUSTER_FED_KIND_LOCATOR_QUERY;
    TEST_ASSERT(ucn_cluster_federation_message_decode(output, output_length - 1U,
                                                       &decoded) == UCN_ERR_MALFORMED);

    federation_test_common(&message, UCN_CLUSTER_FED_KIND_TUNNEL_SUBMIT);
    message.body.submit.final_node_id = UINT32_C(0x10000004);
    message.body.submit.endpoint = 0x40U;
    message.body.submit.traffic_class = UCN_TRAFFIC_Q1_REALTIME;
    message.body.submit.inner_payload = oversized_payload;
    message.body.submit.inner_length = (uint16_t)sizeof(oversized_payload);
    TEST_ASSERT(ucn_cluster_federation_message_encoded_size(&message) == 0U);

    (void)memset(&config, 0, sizeof(config));
    config.local_node_id = UINT32_C(0x10000001);
    config.enabled = false;
    config.persistence_mode = UCN_CLUSTER_PERSISTENCE_VOLATILE_TEST;
    TEST_ASSERT(ucn_cluster_init(&cluster, &config) == UCN_OK);
    TEST_ASSERT(ucn_cluster_get_view(NULL, &view) == UCN_ERR_ARGUMENT);
    TEST_ASSERT(ucn_cluster_get_view(&cluster, &view) == UCN_OK &&
                !view.enabled && view.local_node_id == config.local_node_id);
    cluster.phase = UCN_CLUSTER_PHASE_HEAD_NO_BACKUP;
    cluster.cluster_id = UINT32_C(0x10000001);
    cluster.term = 2U;
    cluster.head_node_id = config.local_node_id;
    cluster.current_head_score = 9000U;
    cluster.primary_members.slots[0].occupied = true;
    cluster.primary_members.slots[0].node_id = UINT32_C(0x10000004);
    cluster.primary_members.slots[0].lease_expires_at_ms = 1000U;
    TEST_ASSERT(ucn_cluster_get_view(&cluster, &view) == UCN_OK &&
                view.role == UCN_CLUSTER_ROLE_HEAD && view.term == 2U);
    TEST_ASSERT(ucn_cluster_copy_member_summaries(&cluster, NULL, 0U) == 1U);
    TEST_ASSERT(ucn_cluster_copy_member_summaries(&cluster, summary, 1U) == 1U &&
                summary[0].node_id == UINT32_C(0x10000004) &&
                summary[0].lease_expires_at_ms == 1000U);
    TEST_ASSERT(ucn_cluster_get_member_summary_at(&cluster, 0U, &summary[0]) ==
                    UCN_OK &&
                summary[0].node_id == UINT32_C(0x10000004));
    TEST_ASSERT(ucn_cluster_get_member_summary_at(&cluster, 1U, &summary[0]) ==
                UCN_ERR_NOT_FOUND);
    return 0;
}

#define FEDERATION_RUNTIME_NODES ((size_t)4U)
#define FEDERATION_RUNTIME_QUEUE ((size_t)32U)
#define FEDERATION_CLIENT_HEAD UINT32_C(0x10000011)
#define FEDERATION_AUTHORITY_HEAD UINT32_C(0x10000012)
#define FEDERATION_REMOTE_HEAD UINT32_C(0x10000013)
#define FEDERATION_REMOTE_MEMBER UINT32_C(0x10000014)
#define FEDERATION_AUTHORITY_MEMBER UINT32_C(0x10000015)
#define FEDERATION_UNAVAILABLE_AUTHORITY UINT32_C(0x10000016)
#define FEDERATION_SILENT_AUTHORITY UINT32_C(0x10000017)
#define FEDERATION_TUNNEL_MEMBER_A UINT32_C(0x10000021)
#define FEDERATION_TUNNEL_HEAD_A UINT32_C(0x10000022)
#define FEDERATION_TUNNEL_HEAD_B UINT32_C(0x10000023)
#define FEDERATION_TUNNEL_MEMBER_C UINT32_C(0x10000024)

typedef struct federation_runtime_network federation_runtime_network_t;

typedef struct federation_runtime_node {
    federation_runtime_network_t *network;
    ucn_node_id_t node_id;
    ucn_cluster_t cluster;
    ucn_cluster_federation_t federation;
    uint32_t deliveries;
    uint32_t errors;
    ucn_cluster_federation_inner_aad_t last_delivery_aad;
    ucn_cluster_federation_error_message_t last_error;
    uint8_t delivered_payload[UCN_CLUSTER_FEDERATION_MAX_INNER_PAYLOAD_BYTES];
    uint16_t delivered_length;
} federation_runtime_node_t;

typedef struct federation_runtime_packet {
    ucn_node_id_t source;
    ucn_node_id_t destination;
    ucn_traffic_class_t traffic_class;
    uint16_t payload_length;
    uint8_t payload[UCN_MAX_PAYLOAD_BYTES];
} federation_runtime_packet_t;

struct federation_runtime_network {
    uint32_t now_ms;
    federation_runtime_node_t nodes[FEDERATION_RUNTIME_NODES];
    federation_runtime_packet_t queue[FEDERATION_RUNTIME_QUEUE];
    size_t queue_count;
    ucn_node_id_t drop_destination;
    uint32_t q0_packets;
    uint32_t q1_packets;
    uint32_t direct_head_a_to_member_c_packets;
};

static const ucn_node_id_t FEDERATION_TUNNEL_AUTHORITIES[] = {
    FEDERATION_TUNNEL_HEAD_A
};

static const ucn_node_id_t FEDERATION_CLIENT_AUTHORITIES[] = {
    FEDERATION_SILENT_AUTHORITY,
    FEDERATION_AUTHORITY_HEAD
};
static const ucn_node_id_t FEDERATION_AUTHORITY_AUTHORITIES[] = {
    FEDERATION_AUTHORITY_HEAD,
    FEDERATION_UNAVAILABLE_AUTHORITY
};

static uint32_t federation_runtime_now(void *context)
{
    federation_runtime_node_t *node = (federation_runtime_node_t *)context;

    return node->network->now_ms;
}

static federation_runtime_node_t *federation_runtime_find_node(
    federation_runtime_network_t *network,
    ucn_node_id_t node_id)
{
    size_t index;

    for (index = 0U; index < FEDERATION_RUNTIME_NODES; ++index) {
        if (network->nodes[index].node_id == node_id) {
            return &network->nodes[index];
        }
    }
    return NULL;
}

static ucn_result_t federation_runtime_send(
    void *context,
    ucn_node_id_t destination,
    ucn_endpoint_t endpoint,
    ucn_traffic_class_t traffic_class,
    const uint8_t *payload,
    uint16_t payload_length)
{
    federation_runtime_node_t *source = (federation_runtime_node_t *)context;
    federation_runtime_network_t *network = source->network;
    federation_runtime_node_t *target = federation_runtime_find_node(network,
                                                                       destination);
    federation_runtime_packet_t *packet;

    if (endpoint != UCN_CLUSTER_FEDERATION_ENDPOINT ||
        (traffic_class != UCN_TRAFFIC_Q0_CRITICAL &&
         traffic_class != UCN_TRAFFIC_Q1_REALTIME) || payload == NULL ||
        payload_length == 0U || payload_length > UCN_MAX_PAYLOAD_BYTES) {
        return UCN_ERR_ARGUMENT;
    }
    if (network->drop_destination != 0U &&
        destination == network->drop_destination) {
        return UCN_ERR_LINK_DOWN;
    }
    if (target == NULL) {
        if (destination == FEDERATION_SILENT_AUTHORITY) {
            return UCN_OK;
        }
        return UCN_ERR_LINK_DOWN;
    }
    if (network->queue_count >= FEDERATION_RUNTIME_QUEUE) {
        return UCN_ERR_NO_SPACE;
    }
    packet = &network->queue[network->queue_count++];
    packet->source = source->node_id;
    packet->destination = destination;
    packet->traffic_class = traffic_class;
    packet->payload_length = payload_length;
    (void)memcpy(packet->payload, payload, payload_length);
    if (traffic_class == UCN_TRAFFIC_Q0_CRITICAL) {
        network->q0_packets++;
    } else {
        network->q1_packets++;
    }
    if (source->node_id == FEDERATION_TUNNEL_HEAD_A &&
        destination == FEDERATION_TUNNEL_MEMBER_C) {
        network->direct_head_a_to_member_c_packets++;
    }
    return UCN_OK;
}

static bool federation_runtime_authorize(void *context, ucn_node_id_t source)
{
    (void)context;
    return source == FEDERATION_CLIENT_HEAD ||
           source == FEDERATION_AUTHORITY_HEAD ||
           source == FEDERATION_REMOTE_HEAD ||
           source == FEDERATION_TUNNEL_HEAD_A ||
           source == FEDERATION_TUNNEL_HEAD_B;
}

static bool federation_runtime_authorize_handover(
    void *context,
    const ucn_cluster_federation_handover_t *handover)
{
    uint32_t *allow = (uint32_t *)context;

    (void)handover;
    return allow != NULL && (*allow & 1U) != 0U;
}

static bool federation_runtime_authorize_handover_default(
    void *context,
    const ucn_cluster_federation_handover_t *handover)
{
    return context != NULL && handover != NULL && handover->proof[0] != 0U;
}

static ucn_result_t federation_runtime_build_handover_proof(
    void *context,
    const ucn_cluster_federation_handover_t *handover,
    uint8_t proof[UCN_CLUSTER_FED_HANDOVER_PROOF_BYTES])
{
    const federation_runtime_node_t *node =
        (const federation_runtime_node_t *)context;
    size_t index;
    uint32_t folded;

    if (node == NULL || handover == NULL || proof == NULL) {
        return UCN_ERR_ARGUMENT;
    }
    folded = handover->cluster_id ^ handover->new_head_node_id ^
             handover->new_term ^ handover->backup_generation ^ node->node_id;
    for (index = 0U; index < UCN_CLUSTER_FED_HANDOVER_PROOF_BYTES; ++index) {
        proof[index] = (uint8_t)(folded >> ((index % 4U) * 8U));
        folded = (folded << 5U) | (folded >> 27U);
    }
    /* The test-side authorizer deliberately distinguishes constructed proof
     * from the all-zero unauthenticated control value. */
    proof[0] |= 1U;
    return UCN_OK;
}

static ucn_result_t federation_runtime_inner_seal(
    void *context,
    const ucn_cluster_federation_inner_aad_t *aad,
    const uint8_t *plaintext,
    uint16_t plaintext_length,
    uint8_t *ciphertext,
    uint16_t ciphertext_capacity,
    uint16_t *ciphertext_length)
{
    (void)context;
    if (aad == NULL || ciphertext == NULL || ciphertext_length == NULL ||
        (plaintext_length != 0U && plaintext == NULL) ||
        plaintext_length > ciphertext_capacity) {
        return UCN_ERR_ARGUMENT;
    }
    if (plaintext_length != 0U) {
        (void)memcpy(ciphertext, plaintext, plaintext_length);
    }
    *ciphertext_length = plaintext_length;
    return UCN_OK;
}

static ucn_result_t federation_runtime_inner_open(
    void *context,
    const ucn_cluster_federation_inner_aad_t *aad,
    const uint8_t *ciphertext,
    uint16_t ciphertext_length,
    uint8_t *plaintext,
    uint16_t plaintext_capacity,
    uint16_t *plaintext_length)
{
    return federation_runtime_inner_seal(context, aad, ciphertext,
                                         ciphertext_length, plaintext,
                                         plaintext_capacity, plaintext_length);
}

static ucn_result_t federation_runtime_inner_deliver(
    void *context,
    const ucn_cluster_federation_inner_aad_t *aad,
    const uint8_t *payload,
    uint16_t payload_length)
{
    federation_runtime_node_t *node = (federation_runtime_node_t *)context;

    if (node == NULL || aad == NULL ||
        (payload_length != 0U && payload == NULL) ||
        payload_length > sizeof(node->delivered_payload)) {
        return UCN_ERR_ARGUMENT;
    }
    node->last_delivery_aad = *aad;
    node->delivered_length = payload_length;
    if (payload_length != 0U) {
        (void)memcpy(node->delivered_payload, payload, payload_length);
    }
    node->deliveries++;
    return UCN_OK;
}

static void federation_runtime_on_error(
    void *context,
    uint32_t transaction_id,
    const ucn_cluster_federation_error_message_t *error)
{
    federation_runtime_node_t *node = (federation_runtime_node_t *)context;

    (void)transaction_id;
    if (node != NULL && error != NULL) {
        node->last_error = *error;
        node->errors++;
    }
}

static int federation_runtime_deliver(federation_runtime_network_t *network)
{
    size_t index = 0U;

    while (index < network->queue_count) {
        federation_runtime_packet_t packet = network->queue[index++];
        federation_runtime_node_t *target = federation_runtime_find_node(
            network, packet.destination);

        TEST_ASSERT(target != NULL);
        TEST_ASSERT(ucn_cluster_federation_receive(
                        &target->federation, packet.source, true,
                        packet.payload, packet.payload_length) == UCN_OK);
    }
    network->queue_count = 0U;
    return 0;
}

static int federation_runtime_init_cluster(ucn_cluster_t *cluster,
                                           ucn_node_id_t local_node_id,
                                           uint32_t cluster_id,
                                           uint32_t term,
                                           ucn_node_id_t member_node_id)
{
    ucn_cluster_config_t config;

    (void)memset(&config, 0, sizeof(config));
    config.local_node_id = local_node_id;
    config.enabled = false;
    config.persistence_mode = UCN_CLUSTER_PERSISTENCE_VOLATILE_TEST;
    TEST_ASSERT(ucn_cluster_init(cluster, &config) == UCN_OK);
    /* Federation only consumes the documented owner-context view; this test
     * fixture intentionally bypasses election to isolate C06.2. */
    cluster->config.enabled = true;
    cluster->phase = UCN_CLUSTER_PHASE_HEAD_NO_BACKUP;
    cluster->cluster_id = cluster_id;
    cluster->term = term;
    cluster->head_node_id = local_node_id;
    if (member_node_id != 0U) {
        cluster->primary_members.slots[0].occupied = true;
        cluster->primary_members.slots[0].node_id = member_node_id;
        cluster->primary_members.slots[0].lease_expires_at_ms = UINT32_C(1000);
    }
    return 0;
}

static int federation_runtime_init_node(
    federation_runtime_node_t *node,
    federation_runtime_network_t *network,
    ucn_node_id_t node_id,
    uint32_t cluster_id,
    uint32_t term,
    ucn_node_id_t member_node_id,
    bool directory_authority,
    bool enable_tunnel,
    const ucn_node_id_t *authorities,
    size_t authority_count)
{
    ucn_cluster_federation_config_t config;

    node->network = network;
    node->node_id = node_id;
    TEST_ASSERT(federation_runtime_init_cluster(&node->cluster, node_id,
                                                cluster_id, term,
                                                member_node_id) == 0);
    (void)memset(&config, 0, sizeof(config));
    config.local_node_id = node_id;
    config.cluster = &node->cluster;
    config.enabled = true;
    config.directory_authority = directory_authority;
    config.require_protected_control = true;
    config.enable_tunnel = enable_tunnel;
    config.default_hop_limit = 3U;
    config.directory_lease_ms = 90U;
    config.locator_refresh_ms = 30U;
    config.query_timeout_ms = 20U;
    config.directory_authorities = authorities;
    config.directory_authority_count = authority_count;
    config.now_ms = federation_runtime_now;
    config.now_context = node;
    config.send = federation_runtime_send;
    config.send_context = node;
    config.authorize_head = federation_runtime_authorize;
    config.authorize_context = network;
    config.authorize_handover = federation_runtime_authorize_handover_default;
    config.authorize_handover_context = network;
    config.build_handover_proof = federation_runtime_build_handover_proof;
    config.handover_proof_context = node;
    if (enable_tunnel) {
        config.seal_inner = federation_runtime_inner_seal;
        config.open_inner = federation_runtime_inner_open;
        config.inner_security_context = node;
        config.deliver = federation_runtime_inner_deliver;
        config.deliver_context = node;
        config.on_error = federation_runtime_on_error;
        config.error_context = node;
    }
    TEST_ASSERT(ucn_cluster_federation_init(&node->federation, &config) == UCN_OK);
    return 0;
}

static int federation_runtime_init_member_node(
    federation_runtime_node_t *node,
    federation_runtime_network_t *network,
    ucn_node_id_t node_id,
    uint32_t cluster_id,
    uint32_t term,
    ucn_node_id_t head_node_id,
    const ucn_node_id_t *authorities,
    size_t authority_count)
{
    ucn_cluster_config_t cluster_config;
    ucn_cluster_federation_config_t config;

    node->network = network;
    node->node_id = node_id;
    (void)memset(&cluster_config, 0, sizeof(cluster_config));
    cluster_config.local_node_id = node_id;
    cluster_config.enabled = false;
    cluster_config.persistence_mode = UCN_CLUSTER_PERSISTENCE_VOLATILE_TEST;
    TEST_ASSERT(ucn_cluster_init(&node->cluster, &cluster_config) == UCN_OK);
    node->cluster.config.enabled = true;
    node->cluster.phase = UCN_CLUSTER_PHASE_MEMBER_ACTIVE;
    node->cluster.cluster_id = cluster_id;
    node->cluster.term = term;
    node->cluster.head_node_id = head_node_id;
    (void)memset(&config, 0, sizeof(config));
    config.local_node_id = node_id;
    config.cluster = &node->cluster;
    config.enabled = true;
    config.require_protected_control = true;
    config.enable_tunnel = true;
    config.default_hop_limit = 3U;
    config.directory_lease_ms = 90U;
    config.locator_refresh_ms = 30U;
    config.query_timeout_ms = 20U;
    config.directory_authorities = authorities;
    config.directory_authority_count = authority_count;
    config.now_ms = federation_runtime_now;
    config.now_context = node;
    config.send = federation_runtime_send;
    config.send_context = node;
    config.authorize_head = federation_runtime_authorize;
    config.authorize_context = network;
    config.authorize_handover = federation_runtime_authorize_handover_default;
    config.authorize_handover_context = network;
    config.build_handover_proof = federation_runtime_build_handover_proof;
    config.handover_proof_context = node;
    config.seal_inner = federation_runtime_inner_seal;
    config.open_inner = federation_runtime_inner_open;
    config.inner_security_context = node;
    config.deliver = federation_runtime_inner_deliver;
    config.deliver_context = node;
    config.on_error = federation_runtime_on_error;
    config.error_context = node;
    TEST_ASSERT(ucn_cluster_federation_init(&node->federation, &config) == UCN_OK);
    return 0;
}

static int federation_runtime_register_remote(
    federation_runtime_node_t *authority)
{
    ucn_cluster_federation_message_t message;
    uint8_t payload[UCN_CLUSTER_FEDERATION_LOCATOR_BYTES];
    size_t payload_length = 0U;

    federation_test_common(&message, UCN_CLUSTER_FED_KIND_LOCATOR_REGISTER);
    message.body.locator.node_id = FEDERATION_REMOTE_MEMBER;
    message.body.locator.cluster_id = UINT32_C(0x1003);
    message.body.locator.head_node_id = FEDERATION_REMOTE_HEAD;
    message.body.locator.term = 7U;
    message.body.locator.lease_ms = 90U;
    message.body.locator.record_nonce = 1U;
    TEST_ASSERT(ucn_cluster_federation_message_encode(
                    &message, payload, sizeof(payload), &payload_length) == UCN_OK);
    TEST_ASSERT(ucn_cluster_federation_receive(
                    &authority->federation, FEDERATION_REMOTE_HEAD, true,
                    payload, payload_length) == UCN_OK);
    message.body.locator.record_nonce = 1U;
    TEST_ASSERT(ucn_cluster_federation_message_encode(
                    &message, payload, sizeof(payload), &payload_length) == UCN_OK);
    TEST_ASSERT(ucn_cluster_federation_receive(
                    &authority->federation, FEDERATION_REMOTE_HEAD, true,
                    payload, payload_length) == UCN_ERR_REPLAY);
    message.body.locator.term = 6U;
    message.body.locator.record_nonce = 99U;
    TEST_ASSERT(ucn_cluster_federation_message_encode(
                    &message, payload, sizeof(payload), &payload_length) == UCN_OK);
    TEST_ASSERT(ucn_cluster_federation_receive(
                    &authority->federation, FEDERATION_REMOTE_HEAD, true,
                    payload, payload_length) == UCN_ERR_REPLAY);
    message.body.locator.term = 7U;
    message.body.locator.record_nonce = 1U;
    TEST_ASSERT(ucn_cluster_federation_message_encode(
                    &message, payload, sizeof(payload), &payload_length) == UCN_OK);
    TEST_ASSERT(ucn_cluster_federation_receive(
                    &authority->federation, FEDERATION_REMOTE_HEAD, false,
                    payload, payload_length) == UCN_ERR_SECURITY);
    return 0;
}

/* CLV2-03-R05: a protected Directory reply can still arrive late.  A live
 * client cache must therefore reject older same-identity records and all
 * incomparable foreign identities; only expiry may open a new identity. */
static int federation_test_locator_cache_monotonicity(void)
{
    federation_runtime_network_t network;
    federation_runtime_node_t *client;
    ucn_cluster_federation_locator_cache_entry_t *cache;
    ucn_cluster_federation_pending_query_t *pending;
    ucn_cluster_federation_message_t message;
    const ucn_cluster_locator_t *locator;
    uint8_t payload[UCN_CLUSTER_FEDERATION_LOCATOR_BYTES];
    size_t payload_length = 0U;

    (void)memset(&network, 0, sizeof(network));
    client = &network.nodes[0];
    TEST_ASSERT(federation_runtime_init_node(
                    client, &network, FEDERATION_CLIENT_HEAD,
                    UINT32_C(0x1001), 3U, 0U, false, false,
                    FEDERATION_CLIENT_AUTHORITIES,
                    sizeof(FEDERATION_CLIENT_AUTHORITIES) /
                        sizeof(FEDERATION_CLIENT_AUTHORITIES[0])) == 0);
    network.now_ms = 10U;
    cache = &client->federation.locator_cache[0];
    cache->occupied = true;
    cache->locator.node_id = FEDERATION_REMOTE_MEMBER;
    cache->locator.cluster_id = UINT32_C(0x1003);
    cache->locator.head_node_id = FEDERATION_REMOTE_HEAD;
    cache->locator.term = 7U;
    cache->locator.lease_ms = 90U;
    cache->locator.record_nonce = 5U;
    cache->expires_at_ms = 100U;
    pending = &client->federation.pending[0];
    pending->occupied = true;
    pending->transaction_id = UINT32_C(0x10203040);
    pending->target_node_id = FEDERATION_REMOTE_MEMBER;
    pending->authority_node_id = FEDERATION_AUTHORITY_HEAD;
    pending->deadline_ms = 200U;

    federation_test_common(&message, UCN_CLUSTER_FED_KIND_LOCATOR_REPLY);
    message.body.locator.node_id = FEDERATION_REMOTE_MEMBER;
    message.body.locator.cluster_id = UINT32_C(0x1003);
    message.body.locator.head_node_id = FEDERATION_REMOTE_HEAD;
    message.body.locator.term = 6U;
    message.body.locator.lease_ms = 90U;
    message.body.locator.record_nonce = 99U;
    TEST_ASSERT(ucn_cluster_federation_message_encode(
                    &message, payload, sizeof(payload), &payload_length) == UCN_OK);
    TEST_ASSERT(ucn_cluster_federation_receive(
                    &client->federation, FEDERATION_AUTHORITY_HEAD, true,
                    payload, payload_length) == UCN_ERR_REPLAY);
    TEST_ASSERT(cache->locator.term == 7U && cache->locator.record_nonce == 5U);

    message.body.locator.cluster_id = UINT32_C(0x2003);
    message.body.locator.head_node_id = FEDERATION_CLIENT_HEAD;
    message.body.locator.term = 1U;
    message.body.locator.record_nonce = 1U;
    TEST_ASSERT(ucn_cluster_federation_message_encode(
                    &message, payload, sizeof(payload), &payload_length) == UCN_OK);
    TEST_ASSERT(ucn_cluster_federation_receive(
                    &client->federation, FEDERATION_AUTHORITY_HEAD, true,
                    payload, payload_length) == UCN_ERR_REPLAY);
    TEST_ASSERT(cache->locator.cluster_id == UINT32_C(0x1003));

    /* The old lease has expired; the same authorized reply may now establish
     * the new Cluster identity and update the next-cluster projection. */
    network.now_ms = 101U;
    TEST_ASSERT(ucn_cluster_federation_receive(
                    &client->federation, FEDERATION_AUTHORITY_HEAD, true,
                    payload, payload_length) == UCN_OK);
    locator = ucn_cluster_federation_find_locator(&client->federation,
                                                  FEDERATION_REMOTE_MEMBER);
    TEST_ASSERT(locator != NULL && locator->cluster_id == UINT32_C(0x2003) &&
                locator->head_node_id == FEDERATION_CLIENT_HEAD &&
                locator->term == 1U);
    return 0;
}

static int federation_test_directory_runtime(void)
{
    federation_runtime_network_t network;
    federation_runtime_node_t *client;
    federation_runtime_node_t *authority;
    const ucn_cluster_locator_t *locator;
    const ucn_cluster_federation_next_cluster_entry_t *next;
    ucn_cluster_federation_t invalid;
    ucn_cluster_federation_config_t invalid_config;
    ucn_cluster_federation_message_t message;
    uint8_t payload[UCN_CLUSTER_FEDERATION_LOCATOR_BYTES];
    size_t payload_length = 0U;
    size_t index;
    bool member_record_present = false;
    bool table_full = false;

    (void)memset(&network, 0, sizeof(network));
    client = &network.nodes[0];
    authority = &network.nodes[1];
    TEST_ASSERT(federation_runtime_init_node(
                    client, &network, FEDERATION_CLIENT_HEAD,
                    UINT32_C(0x1001), 3U, 0U, false,
                    false,
                    FEDERATION_CLIENT_AUTHORITIES,
                    sizeof(FEDERATION_CLIENT_AUTHORITIES) /
                        sizeof(FEDERATION_CLIENT_AUTHORITIES[0])) == 0);
    TEST_ASSERT(federation_runtime_init_node(
                    authority, &network, FEDERATION_AUTHORITY_HEAD,
                    UINT32_C(0x1002), 4U, FEDERATION_AUTHORITY_MEMBER, true,
                    false,
                    FEDERATION_AUTHORITY_AUTHORITIES,
                    sizeof(FEDERATION_AUTHORITY_AUTHORITIES) /
                        sizeof(FEDERATION_AUTHORITY_AUTHORITIES[0])) == 0);
    invalid_config = client->federation.config;
    invalid_config.directory_authorities = NULL;
    invalid_config.directory_authority_count = 0U;
    TEST_ASSERT(ucn_cluster_federation_init(&invalid, &invalid_config) ==
                UCN_ERR_CONFIG);
    invalid_config = client->federation.config;
    invalid_config.build_handover_proof = NULL;
    TEST_ASSERT(ucn_cluster_federation_init(&invalid, &invalid_config) ==
                UCN_ERR_CONFIG);

    TEST_ASSERT(federation_runtime_register_remote(authority) == 0);
    TEST_ASSERT(ucn_cluster_federation_query_locator(
                    &client->federation, FEDERATION_REMOTE_MEMBER) == UCN_OK);
    TEST_ASSERT(network.queue_count == 0U);
    network.now_ms = 21U;
    TEST_ASSERT(ucn_cluster_federation_step(&client->federation) == UCN_OK);
    TEST_ASSERT(federation_runtime_deliver(&network) == 0);
    locator = ucn_cluster_federation_find_locator(&client->federation,
                                                   FEDERATION_REMOTE_MEMBER);
    TEST_ASSERT(locator != NULL && locator->cluster_id == UINT32_C(0x1003) &&
                locator->head_node_id == FEDERATION_REMOTE_HEAD &&
                locator->term == 7U);
    next = ucn_cluster_federation_find_next_cluster(&client->federation,
                                                     UINT32_C(0x1003));
    TEST_ASSERT(next != NULL && next->head_node_id == FEDERATION_REMOTE_HEAD &&
                next->term == 7U);
    TEST_ASSERT(ucn_cluster_federation_query_locator(
                    &client->federation, FEDERATION_REMOTE_MEMBER) == UCN_OK);
    TEST_ASSERT(ucn_cluster_federation_get_stats(&client->federation)->cache_hits ==
                1U);
    TEST_ASSERT(ucn_cluster_federation_get_stats(&client->federation)
                    ->query_timeouts == 1U);

    /* A Head publishes itself and every current member, then removes the
     * departed member from the local Authority without waiting for lease expiry. */
    TEST_ASSERT(ucn_cluster_federation_step(&authority->federation) == UCN_OK);
    network.now_ms = 37U;
    TEST_ASSERT(ucn_cluster_federation_step(&authority->federation) ==
                UCN_ERR_LINK_DOWN);
    network.now_ms = 53U;
    TEST_ASSERT(ucn_cluster_federation_step(&authority->federation) == UCN_OK);
    network.now_ms = 69U;
    TEST_ASSERT(ucn_cluster_federation_step(&authority->federation) ==
                UCN_ERR_LINK_DOWN);
    for (index = 0U; index < UCN_CLUSTER_FED_MAX_DIRECTORY_RECORDS; ++index) {
        if (authority->federation.directory_records[index].occupied &&
            authority->federation.directory_records[index].locator.node_id ==
                FEDERATION_AUTHORITY_MEMBER) {
            member_record_present = true;
        }
    }
    TEST_ASSERT(member_record_present);
    authority->cluster.primary_members.slots[0].occupied = false;
    network.now_ms = 85U;
    TEST_ASSERT(ucn_cluster_federation_step(&authority->federation) == UCN_OK);
    network.now_ms = 101U;
    TEST_ASSERT(ucn_cluster_federation_step(&authority->federation) ==
                UCN_ERR_LINK_DOWN);
    TEST_ASSERT(ucn_cluster_federation_get_stats(&authority->federation)
                    ->records_withdrawn == 1U);
    for (index = 0U; index < UCN_CLUSTER_FED_MAX_DIRECTORY_RECORDS; ++index) {
        TEST_ASSERT(!authority->federation.directory_records[index].occupied ||
                    authority->federation.directory_records[index].locator.node_id !=
                        FEDERATION_AUTHORITY_MEMBER);
    }

    /* Reply/cache/record expiry is bounded. A second query receives the
     * directory negative response, not an old Locator. */
    network.now_ms = 121U;
    (void)ucn_cluster_federation_step(&authority->federation);
    client->federation.query_authority_cursor = 1U;
    TEST_ASSERT(ucn_cluster_federation_query_locator(
                    &client->federation, FEDERATION_REMOTE_MEMBER) == UCN_OK);
    TEST_ASSERT(federation_runtime_deliver(&network) == 0);
    TEST_ASSERT(ucn_cluster_federation_find_locator(&client->federation,
                                                     FEDERATION_REMOTE_MEMBER) == NULL);
    TEST_ASSERT(ucn_cluster_federation_get_stats(&client->federation)->query_errors ==
                1U);

    /* A local Term change withdraws the old Head ownership before the new
     * identity is advertised.  The unavailable replica cannot stall it. */
    authority->cluster.term = 5U;
    network.now_ms = 137U;
    TEST_ASSERT(ucn_cluster_federation_step(&authority->federation) ==
                UCN_ERR_LINK_DOWN);
    network.now_ms = 153U;
    TEST_ASSERT(ucn_cluster_federation_step(&authority->federation) == UCN_OK);
    network.now_ms = 169U;
    TEST_ASSERT(ucn_cluster_federation_step(&authority->federation) ==
                UCN_ERR_LINK_DOWN);
    network.now_ms = 185U;
    TEST_ASSERT(ucn_cluster_federation_step(&authority->federation) == UCN_OK);
    for (index = 0U; index < UCN_CLUSTER_FED_MAX_DIRECTORY_RECORDS; ++index) {
        TEST_ASSERT(!authority->federation.directory_records[index].occupied ||
                    authority->federation.directory_records[index].locator.node_id !=
                        FEDERATION_AUTHORITY_HEAD ||
                    authority->federation.directory_records[index].locator.term == 5U);
    }

    /* A full fixed Authority table rejects the next distinct lease instead of
     * replacing a live Locator. */
    federation_test_common(&message, UCN_CLUSTER_FED_KIND_LOCATOR_REGISTER);
    message.body.locator.cluster_id = UINT32_C(0x2001);
    message.body.locator.head_node_id = FEDERATION_REMOTE_HEAD;
    message.body.locator.term = 8U;
    message.body.locator.lease_ms = 90U;
    for (index = 0U;
         index <= UCN_CLUSTER_FED_MAX_DIRECTORY_RECORDS;
         ++index) {
        ucn_result_t result;

        message.body.locator.node_id = UINT32_C(0x20000000) +
                                       (ucn_node_id_t)index;
        message.body.locator.record_nonce = (uint32_t)(index + 1U);
        TEST_ASSERT(ucn_cluster_federation_message_encode(
                        &message, payload, sizeof(payload), &payload_length) ==
                    UCN_OK);
        result = ucn_cluster_federation_receive(
            &authority->federation, FEDERATION_REMOTE_HEAD, true,
            payload, payload_length);
        if (result == UCN_ERR_NO_SPACE) {
            table_full = true;
            break;
        }
        TEST_ASSERT(result == UCN_OK);
    }
    TEST_ASSERT(table_full);
    return 0;
}

static int federation_test_authority_fence_stops_locator_publish(void)
{
    static const ucn_node_id_t authority_ids[] = { FEDERATION_AUTHORITY_HEAD };
    static const ucn_node_id_t voters[] = {
        FEDERATION_AUTHORITY_HEAD, UINT32_C(0x10000032)
    };
    const ucn_cluster_timing_budget_t budget = {
        5U, 5U, 5U, 5U, 5U, 5U
    };
    federation_runtime_network_t network;
    federation_runtime_node_t *head;
    ucn_cluster_authority_runtime_t runtime;
    ucn_cluster_authority_timing_t timing;
    ucn_cluster_config_state_t config_state;
    const ucn_cluster_federation_stats_t *stats;
    uint32_t messages_before_fence;
    size_t index;

    (void)memset(&network, 0, sizeof(network));
    head = &network.nodes[0];
    TEST_ASSERT(federation_runtime_init_node(
                    head, &network, authority_ids[0], UINT32_C(0x1031), 5U,
                    0U, true, false, authority_ids,
                    sizeof(authority_ids) / sizeof(authority_ids[0])) == 0);
    TEST_ASSERT(ucn_cluster_config_state_init_stable(
                    &config_state, 9U, voters,
                    sizeof(voters) / sizeof(voters[0])) &&
                ucn_cluster_authority_timing_derive(&budget, &timing) ==
                    UCN_OK &&
                ucn_cluster_authority_runtime_init(
                    &runtime, &head->cluster, &config_state, &timing, 0U) ==
                    UCN_OK &&
                ucn_cluster_authority_runtime_note_voter_keepalive(
                    &runtime, voters[1], 0U) == UCN_OK &&
                ucn_cluster_authority_runtime_step(&runtime, 0U) == UCN_OK &&
                ucn_cluster_authority_active(&head->cluster));
    TEST_ASSERT(ucn_cluster_federation_step(&head->federation) == UCN_OK);
    stats = ucn_cluster_federation_get_stats(&head->federation);
    messages_before_fence = stats->messages_sent;
    TEST_ASSERT(messages_before_fence != 0U);
    network.now_ms = 91U;
    /* Federation-first at an expired Owner clock must run M08 preflight
     * itself; do not manually refresh the runtime before this assertion. */
    TEST_ASSERT(ucn_cluster_federation_publish_handover(&head->federation) ==
                    UCN_ERR_ACCESS &&
                !ucn_cluster_authority_active(&head->cluster) &&
                ucn_cluster_federation_get_stats(&head->federation)
                        ->messages_sent == messages_before_fence);
    TEST_ASSERT(ucn_cluster_federation_step(&head->federation) == UCN_OK &&
                ucn_cluster_federation_get_stats(&head->federation)
                        ->messages_sent == messages_before_fence);
    for (index = 0U; index < UCN_CLUSTER_FED_MAX_LOCAL_LOCATORS; ++index) {
        TEST_ASSERT(!head->federation.local_locators[index].withdrawal_pending);
    }
    return 0;
}

/* C07.4: a bounded ClusterHeadLease makes Backup takeover an atomic single
 * record replacement on the Authority, then blocks the stale former Head. */
static int federation_test_handover_runtime(void)
{
    federation_runtime_network_t network;
    federation_runtime_node_t *authority;
    ucn_cluster_federation_message_t message;
    ucn_cluster_federation_message_t reply;
    uint8_t payload[UCN_MAX_PAYLOAD_BYTES];
    size_t payload_length = 0U;
    const ucn_cluster_federation_cluster_head_lease_t *lease;
    uint32_t allow = 0U;

    (void)memset(&network, 0, sizeof(network));
    authority = &network.nodes[0];
    TEST_ASSERT(federation_runtime_init_node(
                    authority, &network, FEDERATION_AUTHORITY_HEAD,
                    UINT32_C(0x1002), 4U, FEDERATION_AUTHORITY_MEMBER, true,
                    false, FEDERATION_AUTHORITY_AUTHORITIES,
                    sizeof(FEDERATION_AUTHORITY_AUTHORITIES) /
                        sizeof(FEDERATION_AUTHORITY_AUTHORITIES[0])) == 0);
    /* The former Head is a real node so the Directory Reply can be queued. */
    TEST_ASSERT(federation_runtime_init_node(
                    &network.nodes[1], &network, FEDERATION_REMOTE_HEAD,
                    UINT32_C(0x1003), 7U, FEDERATION_REMOTE_MEMBER, false,
                    false, FEDERATION_CLIENT_AUTHORITIES,
                    sizeof(FEDERATION_CLIENT_AUTHORITIES) /
                        sizeof(FEDERATION_CLIENT_AUTHORITIES[0])) == 0);

    /* Codec round-trip with a non-zero opaque proof. */
    federation_test_common(&message, UCN_CLUSTER_FED_KIND_LOCATOR_HANDOVER);
    message.body.handover.cluster_id = UINT32_C(0x1003);
    message.body.handover.new_head_node_id = FEDERATION_CLIENT_HEAD;
    message.body.handover.new_term = 8U;
    message.body.handover.backup_generation = 2U;
    message.body.handover.proof[0] = 0xA5U;
    message.body.handover.proof[15] = 0x5AU;
    TEST_ASSERT(federation_test_round_trip(&message) == 0);

    /* Old Head registers its member before any takeover (no lease yet). */
    federation_test_common(&message, UCN_CLUSTER_FED_KIND_LOCATOR_REGISTER);
    message.body.locator.node_id = FEDERATION_REMOTE_MEMBER;
    message.body.locator.cluster_id = UINT32_C(0x1003);
    message.body.locator.head_node_id = FEDERATION_REMOTE_HEAD;
    message.body.locator.term = 7U;
    message.body.locator.lease_ms = 90U;
    message.body.locator.record_nonce = 1U;
    TEST_ASSERT(ucn_cluster_federation_message_encode(
                    &message, payload, sizeof(payload), &payload_length) == UCN_OK);
    TEST_ASSERT(ucn_cluster_federation_receive(
                    &authority->federation, FEDERATION_REMOTE_HEAD, true,
                    payload, payload_length) == UCN_OK);

    /* Backup takeover: the new Head publishes one atomic handover. */
    federation_test_common(&message, UCN_CLUSTER_FED_KIND_LOCATOR_HANDOVER);
    message.body.handover.cluster_id = UINT32_C(0x1003);
    message.body.handover.new_head_node_id = FEDERATION_CLIENT_HEAD;
    message.body.handover.new_term = 8U;
    message.body.handover.backup_generation = 2U;
    message.body.handover.proof[0] = 0xA5U;
    TEST_ASSERT(ucn_cluster_federation_message_encode(
                    &message, payload, sizeof(payload), &payload_length) == UCN_OK);
    TEST_ASSERT(ucn_cluster_federation_receive(
                    &authority->federation, FEDERATION_CLIENT_HEAD, true,
                    payload, payload_length) == UCN_OK);
    lease = ucn_cluster_federation_find_head_lease(&authority->federation,
                                                    UINT32_C(0x1003));
    TEST_ASSERT(lease != NULL &&
                lease->head_node_id == FEDERATION_CLIENT_HEAD &&
                lease->term == 8U && lease->backup_generation == 2U);
    TEST_ASSERT(ucn_cluster_federation_get_stats(&authority->federation)
                    ->handovers_accepted == 1U);

    /* An identical same-Term handover is idempotent: its successful replay
     * refreshes the Directory lease, whereas conflicting same-Term state is
     * still rejected by the Authority. */
    message.body.handover.new_term = 8U;
    TEST_ASSERT(ucn_cluster_federation_message_encode(
                    &message, payload, sizeof(payload), &payload_length) == UCN_OK);
    TEST_ASSERT(ucn_cluster_federation_receive(
                    &authority->federation, FEDERATION_CLIENT_HEAD, true,
                    payload, payload_length) == UCN_OK);

    /* The stale former Head is denied before any member record comparison. */
    federation_test_common(&message, UCN_CLUSTER_FED_KIND_LOCATOR_REGISTER);
    message.body.locator.node_id = FEDERATION_REMOTE_MEMBER;
    message.body.locator.cluster_id = UINT32_C(0x1003);
    message.body.locator.head_node_id = FEDERATION_REMOTE_HEAD;
    message.body.locator.term = 7U;
    message.body.locator.lease_ms = 90U;
    message.body.locator.record_nonce = 2U;
    TEST_ASSERT(ucn_cluster_federation_message_encode(
                    &message, payload, sizeof(payload), &payload_length) == UCN_OK);
    TEST_ASSERT(ucn_cluster_federation_receive(
                    &authority->federation, FEDERATION_REMOTE_HEAD, true,
                    payload, payload_length) == UCN_ERR_REPLAY);

    /* The Directory Reply resolves the still-old member record to the new
     * Head through the ClusterHeadLease (single atomic rewiring). */
    federation_test_common(&message, UCN_CLUSTER_FED_KIND_LOCATOR_QUERY);
    message.body.query.target_node_id = FEDERATION_REMOTE_MEMBER;
    message.body.query.requester_cluster_id = UINT32_C(0x1004);
    message.body.query.requester_head_node_id = FEDERATION_REMOTE_HEAD;
    TEST_ASSERT(ucn_cluster_federation_message_encode(
                    &message, payload, sizeof(payload), &payload_length) == UCN_OK);
    network.queue_count = 0U;
    TEST_ASSERT(ucn_cluster_federation_receive(
                    &authority->federation, FEDERATION_REMOTE_HEAD, true,
                    payload, payload_length) == UCN_OK);
    TEST_ASSERT(network.queue_count == 1U);
    TEST_ASSERT(ucn_cluster_federation_message_decode(
                    network.queue[0].payload, network.queue[0].payload_length,
                    &reply) == UCN_OK);
    TEST_ASSERT(reply.kind == UCN_CLUSTER_FED_KIND_LOCATOR_REPLY &&
                reply.body.locator.node_id == FEDERATION_REMOTE_MEMBER &&
                reply.body.locator.head_node_id == FEDERATION_CLIENT_HEAD &&
                reply.body.locator.term == 8U);

    /* The new Head may register the member; the higher Term supersedes the
     * old record even though its record_nonce restarts. */
    federation_test_common(&message, UCN_CLUSTER_FED_KIND_LOCATOR_REGISTER);
    message.body.locator.node_id = FEDERATION_REMOTE_MEMBER;
    message.body.locator.cluster_id = UINT32_C(0x1003);
    message.body.locator.head_node_id = FEDERATION_CLIENT_HEAD;
    message.body.locator.term = 8U;
    message.body.locator.lease_ms = 90U;
    message.body.locator.record_nonce = 1U;
    TEST_ASSERT(ucn_cluster_federation_message_encode(
                    &message, payload, sizeof(payload), &payload_length) == UCN_OK);
    TEST_ASSERT(ucn_cluster_federation_receive(
                    &authority->federation, FEDERATION_CLIENT_HEAD, true,
                    payload, payload_length) == UCN_OK);

    /* authorize_handover() veto: a configured product callback can reject
     * the takeover even when authorize_head() and the Term are valid. */
    authority->federation.config.authorize_handover =
        federation_runtime_authorize_handover;
    authority->federation.config.authorize_handover_context = &allow;
    federation_test_common(&message, UCN_CLUSTER_FED_KIND_LOCATOR_HANDOVER);
    message.body.handover.cluster_id = UINT32_C(0x1003);
    message.body.handover.new_head_node_id = FEDERATION_CLIENT_HEAD;
    message.body.handover.new_term = 9U;
    message.body.handover.backup_generation = 3U;
    TEST_ASSERT(ucn_cluster_federation_message_encode(
                    &message, payload, sizeof(payload), &payload_length) == UCN_OK);
    TEST_ASSERT(ucn_cluster_federation_receive(
                    &authority->federation, FEDERATION_CLIENT_HEAD, true,
                    payload, payload_length) == UCN_ERR_ACCESS);
    allow = 1U;
    TEST_ASSERT(ucn_cluster_federation_receive(
                    &authority->federation, FEDERATION_CLIENT_HEAD, true,
                    payload, payload_length) == UCN_OK);
    TEST_ASSERT(ucn_cluster_federation_find_head_lease(
                    &authority->federation, UINT32_C(0x1003))->term == 9U);
    return 0;
}

static int federation_test_tunnel_runtime(void)
{
    static const uint8_t payload[] = { 0x11U, 0x22U, 0x33U, 0x44U };
    federation_runtime_network_t network;
    federation_runtime_node_t *member_a;
    federation_runtime_node_t *head_a;
    federation_runtime_node_t *head_b;
    federation_runtime_node_t *member_c;
    ucn_cluster_federation_message_t message;
    ucn_cluster_federation_config_t invalid_config;
    ucn_cluster_federation_t invalid;
    uint8_t encoded[UCN_MAX_PAYLOAD_BYTES];
    size_t encoded_length = 0U;

    (void)memset(&network, 0, sizeof(network));
    member_a = &network.nodes[0];
    head_a = &network.nodes[1];
    head_b = &network.nodes[2];
    member_c = &network.nodes[3];
    TEST_ASSERT(federation_runtime_init_member_node(
                    member_a, &network, FEDERATION_TUNNEL_MEMBER_A,
                    UINT32_C(0x2001), 3U, FEDERATION_TUNNEL_HEAD_A,
                    FEDERATION_TUNNEL_AUTHORITIES,
                    sizeof(FEDERATION_TUNNEL_AUTHORITIES) /
                        sizeof(FEDERATION_TUNNEL_AUTHORITIES[0])) == 0);
    TEST_ASSERT(federation_runtime_init_node(
                    head_a, &network, FEDERATION_TUNNEL_HEAD_A,
                    UINT32_C(0x2001), 3U, FEDERATION_TUNNEL_MEMBER_A, true,
                    true, FEDERATION_TUNNEL_AUTHORITIES,
                    sizeof(FEDERATION_TUNNEL_AUTHORITIES) /
                        sizeof(FEDERATION_TUNNEL_AUTHORITIES[0])) == 0);
    TEST_ASSERT(federation_runtime_init_node(
                    head_b, &network, FEDERATION_TUNNEL_HEAD_B,
                    UINT32_C(0x2002), 4U, FEDERATION_TUNNEL_MEMBER_C, false,
                    true, FEDERATION_TUNNEL_AUTHORITIES,
                    sizeof(FEDERATION_TUNNEL_AUTHORITIES) /
                        sizeof(FEDERATION_TUNNEL_AUTHORITIES[0])) == 0);
    TEST_ASSERT(federation_runtime_init_member_node(
                    member_c, &network, FEDERATION_TUNNEL_MEMBER_C,
                    UINT32_C(0x2002), 4U, FEDERATION_TUNNEL_HEAD_B,
                    FEDERATION_TUNNEL_AUTHORITIES,
                    sizeof(FEDERATION_TUNNEL_AUTHORITIES) /
                        sizeof(FEDERATION_TUNNEL_AUTHORITIES[0])) == 0);

    /* The safe default is a required inner provider whenever C06.3 is on. */
    invalid_config = head_a->federation.config;
    invalid_config.seal_inner = NULL;
    TEST_ASSERT(ucn_cluster_federation_init(&invalid, &invalid_config) ==
                UCN_ERR_CONFIG);

    /* Register C through the configured Authority and have H1 resolve it.
     * No direct H1->C Core path is ever installed or used by this fixture. */
    federation_test_common(&message, UCN_CLUSTER_FED_KIND_LOCATOR_REGISTER);
    message.body.locator.node_id = FEDERATION_TUNNEL_MEMBER_C;
    message.body.locator.cluster_id = UINT32_C(0x2002);
    message.body.locator.head_node_id = FEDERATION_TUNNEL_HEAD_B;
    message.body.locator.term = 4U;
    message.body.locator.lease_ms = 90U;
    message.body.locator.record_nonce = 1U;
    TEST_ASSERT(ucn_cluster_federation_message_encode(
                    &message, encoded, sizeof(encoded), &encoded_length) == UCN_OK);
    TEST_ASSERT(ucn_cluster_federation_receive(
                    &head_a->federation, FEDERATION_TUNNEL_HEAD_B, true,
                    encoded, encoded_length) == UCN_OK);
    TEST_ASSERT(ucn_cluster_federation_query_locator(
                    &head_a->federation, FEDERATION_TUNNEL_MEMBER_C) == UCN_OK);
    TEST_ASSERT(federation_runtime_deliver(&network) == 0);
    TEST_ASSERT(ucn_cluster_federation_find_locator(
                    &head_a->federation, FEDERATION_TUNNEL_MEMBER_C) != NULL);

    network.q0_packets = 0U;
    network.q1_packets = 0U;
    network.direct_head_a_to_member_c_packets = 0U;
    TEST_ASSERT(ucn_cluster_federation_send(
                    &member_a->federation, FEDERATION_TUNNEL_MEMBER_C, 0x50U,
                    UCN_TRAFFIC_Q0_CRITICAL, payload, sizeof(payload)) == UCN_OK);
    TEST_ASSERT(federation_runtime_deliver(&network) == 0);
    TEST_ASSERT(member_c->deliveries == 1U &&
                member_c->delivered_length == sizeof(payload) &&
                memcmp(member_c->delivered_payload, payload, sizeof(payload)) == 0);
    TEST_ASSERT(member_c->last_delivery_aad.origin_node_id ==
                    FEDERATION_TUNNEL_MEMBER_A &&
                member_c->last_delivery_aad.final_node_id ==
                    FEDERATION_TUNNEL_MEMBER_C &&
                member_c->last_delivery_aad.origin_cluster_id == UINT32_C(0x2001) &&
                member_c->last_delivery_aad.destination_cluster_id == UINT32_C(0x2002) &&
                member_c->last_delivery_aad.endpoint == 0x50U &&
                member_c->last_delivery_aad.traffic_class == UCN_TRAFFIC_Q0_CRITICAL);
    TEST_ASSERT(network.q0_packets == 3U && network.q1_packets == 0U &&
                network.direct_head_a_to_member_c_packets == 0U);

    /* An untrusted node cannot inject a Submit into H1, and replayed Data is
     * rejected at H2 by its bounded Seen table. */
    federation_test_common(&message, UCN_CLUSTER_FED_KIND_TUNNEL_SUBMIT);
    message.transaction_id = UINT32_C(0x1001);
    message.body.submit.final_node_id = FEDERATION_TUNNEL_MEMBER_C;
    message.body.submit.endpoint = 0x50U;
    message.body.submit.traffic_class = UCN_TRAFFIC_Q0_CRITICAL;
    message.body.submit.inner_payload = payload;
    message.body.submit.inner_length = (uint16_t)sizeof(payload);
    TEST_ASSERT(ucn_cluster_federation_message_encode(
                    &message, encoded, sizeof(encoded), &encoded_length) == UCN_OK);
    TEST_ASSERT(ucn_cluster_federation_receive(
                    &head_a->federation, FEDERATION_AUTHORITY_MEMBER, true,
                    encoded, encoded_length) == UCN_ERR_ACCESS);
    federation_test_common(&message, UCN_CLUSTER_FED_KIND_TUNNEL_DATA);
    message.transaction_id = 1U;
    message.hop_limit = 2U;
    message.body.tunnel.origin_node_id = FEDERATION_TUNNEL_MEMBER_A;
    message.body.tunnel.final_node_id = FEDERATION_TUNNEL_MEMBER_C;
    message.body.tunnel.origin_cluster_id = UINT32_C(0x2001);
    message.body.tunnel.destination_cluster_id = UINT32_C(0x2002);
    message.body.tunnel.endpoint = 0x50U;
    message.body.tunnel.traffic_class = UCN_TRAFFIC_Q0_CRITICAL;
    message.body.tunnel.inner_payload = payload;
    message.body.tunnel.inner_length = (uint16_t)sizeof(payload);
    TEST_ASSERT(ucn_cluster_federation_message_encode(
                    &message, encoded, sizeof(encoded), &encoded_length) == UCN_OK);
    TEST_ASSERT(ucn_cluster_federation_receive(
                    &head_b->federation, FEDERATION_TUNNEL_HEAD_A, true,
                    encoded, encoded_length) == UCN_ERR_REPLAY);

    /* H2 no longer owns C: it returns DIRECTORY_STALE via H1, which relays
     * the bounded error to A without ever falling back to a direct C Route. */
    head_b->cluster.primary_members.slots[0].occupied = false;
    TEST_ASSERT(ucn_cluster_federation_send(
                    &member_a->federation, FEDERATION_TUNNEL_MEMBER_C, 0x51U,
                    UCN_TRAFFIC_Q1_REALTIME, payload, sizeof(payload)) == UCN_OK);
    TEST_ASSERT(federation_runtime_deliver(&network) == 0);
    TEST_ASSERT(member_a->errors == 1U &&
                member_a->last_error.error == UCN_CLUSTER_FED_ERROR_DIRECTORY_STALE);

    /* A Head-to-Head route failure is surfaced as DOWNSTREAM instead of
     * causing Core retries of the same Submit. */
    head_b->cluster.primary_members.slots[0].occupied = true;
    head_b->cluster.primary_members.slots[0].node_id = FEDERATION_TUNNEL_MEMBER_C;
    head_b->cluster.primary_members.slots[0].lease_expires_at_ms = UINT32_C(1000);
    network.drop_destination = FEDERATION_TUNNEL_HEAD_B;
    TEST_ASSERT(ucn_cluster_federation_send(
                    &member_a->federation, FEDERATION_TUNNEL_MEMBER_C, 0x52U,
                    UCN_TRAFFIC_Q1_REALTIME, payload, sizeof(payload)) == UCN_OK);
    TEST_ASSERT(federation_runtime_deliver(&network) == 0);
    TEST_ASSERT(member_a->errors == 2U &&
                member_a->last_error.error == UCN_CLUSTER_FED_ERROR_DOWNSTREAM);
    network.drop_destination = 0U;

    /* A stale/looped Data frame cannot be forwarded by H2 once its
     * Federation TTL is exhausted. */
    message.transaction_id = UINT32_C(0x2001);
    message.hop_limit = 1U;
    TEST_ASSERT(ucn_cluster_federation_message_encode(
                    &message, encoded, sizeof(encoded), &encoded_length) == UCN_OK);
    TEST_ASSERT(ucn_cluster_federation_receive(
                    &head_b->federation, FEDERATION_TUNNEL_HEAD_A, true,
                    encoded, encoded_length) == UCN_OK);
    TEST_ASSERT(ucn_cluster_federation_get_stats(&head_b->federation)
                    ->tunnel_stale_rejected >= 1U);
    network.queue_count = 0U;
    return 0;
}

/* C07.4 integration: a Cluster takeover (Member -> Head with a Term bump)
 * auto-publishes one ClusterHeadLease handover to the Authority. */
static int federation_test_handover_autopublish(void)
{
    static const ucn_node_id_t HANDOVER_AUTH[] = {
        FEDERATION_AUTHORITY_HEAD
    };
    federation_runtime_network_t network;
    federation_runtime_node_t *authority;
    federation_runtime_node_t *promoted;
    const ucn_cluster_federation_cluster_head_lease_t *lease;

    (void)memset(&network, 0, sizeof(network));
    authority = &network.nodes[0];
    promoted = &network.nodes[1];
    TEST_ASSERT(federation_runtime_init_node(
                    authority, &network, FEDERATION_AUTHORITY_HEAD,
                    UINT32_C(0x1002), 4U, FEDERATION_AUTHORITY_MEMBER, true,
                    false, FEDERATION_AUTHORITY_AUTHORITIES,
                    sizeof(FEDERATION_AUTHORITY_AUTHORITIES) /
                        sizeof(FEDERATION_AUTHORITY_AUTHORITIES[0])) == 0);
    /* node 1 starts as a Member of 0x1001 (was_local_head=false). */
    TEST_ASSERT(federation_runtime_init_member_node(
                    promoted, &network, FEDERATION_TUNNEL_HEAD_A,
                    UINT32_C(0x1001), 3U, FEDERATION_AUTHORITY_HEAD,
                    HANDOVER_AUTH, sizeof(HANDOVER_AUTH) /
                        sizeof(HANDOVER_AUTH[0])) == 0);

    /* Promote to Head (takeover): role/head/term change. */
    promoted->cluster.phase = UCN_CLUSTER_PHASE_HEAD_NO_BACKUP;
    promoted->cluster.head_node_id = FEDERATION_TUNNEL_HEAD_A;
    promoted->cluster.term = 4U;
    /* The first send is refused by the transport.  C07.4 must retain the
     * handover and retry it rather than recording the Term as published. */
    network.drop_destination = FEDERATION_AUTHORITY_HEAD;
    network.now_ms = 100U;
    TEST_ASSERT(ucn_cluster_federation_step(&promoted->federation) ==
                UCN_ERR_LINK_DOWN);
    TEST_ASSERT(promoted->federation.handover_pending == true &&
                promoted->federation.handover_attempts == 1U);
    network.drop_destination = 0U;
    network.now_ms = 100U + UCN_CLUSTER_FED_HANDOVER_RETRY_MS;
    TEST_ASSERT(ucn_cluster_federation_step(&promoted->federation) == UCN_OK);
    TEST_ASSERT(federation_runtime_deliver(&network) == 0);

    lease = ucn_cluster_federation_find_head_lease(&authority->federation,
                                                    UINT32_C(0x1001));
    TEST_ASSERT(lease != NULL &&
                lease->head_node_id == FEDERATION_TUNNEL_HEAD_A &&
                lease->term == 4U && lease->handover_proof[0] != 0U);
    return 0;
}

int test_cluster_federation(void)
{
    TEST_ASSERT(federation_test_locator_and_query() == 0);
    TEST_ASSERT(federation_test_tunnel_messages() == 0);
    TEST_ASSERT(federation_test_malformed_and_cluster_view() == 0);
    TEST_ASSERT(federation_test_locator_cache_monotonicity() == 0);
    TEST_ASSERT(federation_test_directory_runtime() == 0);
    TEST_ASSERT(federation_test_authority_fence_stops_locator_publish() == 0);
    TEST_ASSERT(federation_test_handover_runtime() == 0);
    TEST_ASSERT(federation_test_handover_autopublish() == 0);
    TEST_ASSERT(federation_test_tunnel_runtime() == 0);
    return 0;
}
