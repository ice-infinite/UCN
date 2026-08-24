#include <stdio.h>
#include <string.h>

#include "ucn/ucn.h"
#include "ucn/ucn_cluster.h"
#include "ucn/ucn_cluster_storage.h"
#include "ucn/ucn_cluster_federation.h"
#include "ucn/ucn_node.h"
#include "ucn/ucn_transfer.h"
#include "ucn/ports/ucn_event_runtime.h"
#include "ucn/adapters/ucn_can_source.h"
#include "ucn/adapters/ucn_stream_source.h"
#if UCN_FEATURE_SERVICE
#include "ucn/ucn_service.h"
#endif

#include "test_support.h"

#define PROFILE_NETWORK_ID UINT32_C(0x50464C45)
#define PROFILE_ENDPOINT ((ucn_endpoint_t)0x40U)

typedef struct profile_link_context {
    ucn_node_t *peer;
    ucn_link_t *peer_ingress;
    bool is_up;
    ucn_result_t last_receive_result;
} profile_link_context_t;

typedef struct profile_receive_state {
    uint32_t count;
    ucn_node_id_t source;
    uint8_t payload[8];
    uint16_t payload_length;
} profile_receive_state_t;

static ucn_result_t profile_link_send(ucn_link_t *link,
                                      const uint8_t *data,
                                      size_t length)
{
    profile_link_context_t *context =
        (profile_link_context_t *)link->context;

    context->last_receive_result = ucn_node_receive(
        context->peer, context->peer_ingress, data, length);
    return context->last_receive_result;
}

static ucn_result_t profile_link_status(const ucn_link_t *link,
                                        ucn_link_status_t *status)
{
    const profile_link_context_t *context =
        (const profile_link_context_t *)link->context;

    (void)memset(status, 0, sizeof(*status));
    status->is_up = context->is_up;
    status->mtu = link->mtu;
    return UCN_OK;
}

static const ucn_link_ops_t PROFILE_LINK_OPS = {
    NULL,
    profile_link_send,
    NULL,
    profile_link_status,
    NULL,
    NULL
};

static void profile_receive(void *context, const ucn_frame_t *frame)
{
    profile_receive_state_t *state = (profile_receive_state_t *)context;

    state->count++;
    state->source = frame->source;
    state->payload_length = frame->payload_length;
    if (frame->payload_length <= sizeof(state->payload)) {
        (void)memcpy(state->payload, frame->payload, frame->payload_length);
    }
}

static void profile_init_link(ucn_link_t *link,
                              profile_link_context_t *context,
                              uint8_t link_id,
                              ucn_node_id_t peer_node_id,
                              ucn_node_t *peer,
                              ucn_link_t *peer_ingress)
{
    (void)memset(link, 0, sizeof(*link));
    (void)memset(context, 0, sizeof(*context));
    link->ops = &PROFILE_LINK_OPS;
    link->context = context;
    link->link_id = link_id;
    link->mtu = UCN_MAX_FRAME_BYTES;
    link->peer_node_id = peer_node_id;
    context->peer = peer;
    context->peer_ingress = peer_ingress;
    context->is_up = true;
}

static int test_profile_three_node_delivery(void)
{
    static const uint8_t payload[] = { 0x50U, 0x46U, 0x4CU, 0x45U };
    ucn_node_t node_a;
    ucn_node_t node_b;
    ucn_node_t node_c;
    const ucn_config_t config_a = { PROFILE_NETWORK_ID, 1U, 5U };
    const ucn_config_t config_b = { PROFILE_NETWORK_ID, 2U, 5U };
    const ucn_config_t config_c = { PROFILE_NETWORK_ID, 3U, 5U };
    ucn_link_t link_ab;
    ucn_link_t link_ba;
    ucn_link_t link_bc;
    ucn_link_t link_cb;
    profile_link_context_t context_ab;
    profile_link_context_t context_ba;
    profile_link_context_t context_bc;
    profile_link_context_t context_cb;
    profile_receive_state_t received;

    (void)memset(&node_a, 0, sizeof(node_a));
    (void)memset(&node_b, 0, sizeof(node_b));
    (void)memset(&node_c, 0, sizeof(node_c));
    (void)memset(&received, 0, sizeof(received));
    TEST_ASSERT(ucn_node_init(&node_a, &config_a) == UCN_OK);
    TEST_ASSERT(ucn_node_init(&node_b, &config_b) == UCN_OK);
    TEST_ASSERT(ucn_node_init(&node_c, &config_c) == UCN_OK);

    profile_init_link(&link_ab, &context_ab, 1U, config_b.node_id,
                      &node_b, &link_ba);
    profile_init_link(&link_ba, &context_ba, 2U, config_a.node_id,
                      &node_a, &link_ab);
    profile_init_link(&link_bc, &context_bc, 3U, config_c.node_id,
                      &node_c, &link_cb);
    profile_init_link(&link_cb, &context_cb, 4U, config_b.node_id,
                      &node_b, &link_bc);
    TEST_ASSERT(ucn_node_register_link(&node_a, &link_ab) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&node_b, &link_ba) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&node_b, &link_bc) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&node_c, &link_cb) == UCN_OK);
    TEST_ASSERT(ucn_node_set_endpoint_handler(
                    &node_c, PROFILE_ENDPOINT, profile_receive, &received) ==
                UCN_OK);

#if UCN_PROFILE == UCN_PROFILE_NANO
    TEST_ASSERT(ucn_node_add_route(&node_a, config_c.node_id, &link_ab) == UCN_OK);
#else
    {
        const ucn_result_t discovery_result =
            ucn_node_discover_route(&node_a, config_c.node_id, 10U);

        if (discovery_result != UCN_OK) {
            printf("profile discovery failed: %d ab=%d ba=%d bc=%d cb=%d\n",
                   (int)discovery_result,
                   (int)context_ab.last_receive_result,
                   (int)context_ba.last_receive_result,
                   (int)context_bc.last_receive_result,
                   (int)context_cb.last_receive_result);
        }
        TEST_ASSERT(discovery_result == UCN_OK);
    }
    TEST_ASSERT(!ucn_node_route_pending(&node_a, config_c.node_id));
#endif
    TEST_ASSERT(ucn_node_send_endpoint(
                    &node_a, config_c.node_id, PROFILE_ENDPOINT,
                    UCN_TRAFFIC_Q1_REALTIME, payload,
                    (uint16_t)sizeof(payload)) == UCN_OK);
    TEST_ASSERT(received.count == 1U);
    TEST_ASSERT(received.source == config_a.node_id);
    TEST_ASSERT(received.payload_length == sizeof(payload));
    TEST_ASSERT(memcmp(received.payload, payload, sizeof(payload)) == 0);
    return 0;
}

static int test_profile_contract(void)
{
    ucn_node_t node;
    const ucn_config_t config = { PROFILE_NETWORK_ID, 10U, 5U };
    ucn_route_policy_config_t policy;

    (void)memset(&node, 0, sizeof(node));
    (void)memset(&policy, 0, sizeof(policy));
    TEST_ASSERT(ucn_node_init(&node, &config) == UCN_OK);

#if UCN_PROFILE == UCN_PROFILE_NANO
    TEST_ASSERT(UCN_FEATURE_DYNAMIC_MESH == 0);
    TEST_ASSERT(UCN_FEATURE_SECURITY == 0);
    TEST_ASSERT(UCN_FEATURE_CANDIDATE_ROUTING == 0);
    TEST_ASSERT(UCN_FEATURE_PATH == 0);
    TEST_ASSERT(UCN_FEATURE_POLICY == 0);
    TEST_ASSERT(UCN_FEATURE_DIAGNOSTICS == 0);
    TEST_ASSERT(ucn_node_discover_route(&node, 11U, 0U) == UCN_ERR_CONFIG);
    TEST_ASSERT(ucn_node_set_security(&node, NULL, NULL) == UCN_ERR_CONFIG);
#elif UCN_PROFILE == UCN_PROFILE_LITE
    TEST_ASSERT(UCN_FEATURE_DYNAMIC_MESH == 1);
    TEST_ASSERT(UCN_FEATURE_SECURITY == 1);
    TEST_ASSERT(UCN_FEATURE_CANDIDATE_ROUTING == 0);
    TEST_ASSERT(UCN_FEATURE_PATH == 0);
    TEST_ASSERT(UCN_FEATURE_POLICY == 0);
    TEST_ASSERT(UCN_FEATURE_DIAGNOSTICS == 0);
    TEST_ASSERT(ucn_node_refresh_route(&node, 11U, 0U) == UCN_ERR_CONFIG);
    TEST_ASSERT(ucn_node_set_security(&node, NULL, NULL) == UCN_OK);
#else
    TEST_ASSERT(UCN_FEATURE_DYNAMIC_MESH == 1);
    TEST_ASSERT(UCN_FEATURE_SECURITY == 1);
    TEST_ASSERT(UCN_FEATURE_CANDIDATE_ROUTING == 1);
    TEST_ASSERT(UCN_FEATURE_PATH == 1);
    TEST_ASSERT(UCN_FEATURE_POLICY == 1);
    TEST_ASSERT(UCN_FEATURE_DIAGNOSTICS == 1);
#endif

#if UCN_PROFILE != UCN_PROFILE_FULL
    ucn_path_forward_config_t path_config;
    ucn_path_forward_entry_t path_entry;
    ucn_path_state_t path_state;
    ucn_policy_state_t policy_state;
    const ucn_path_capability_t capability = {
        UCN_WIRE_PROFILE_W0_LOCAL,
        64U
    };

    (void)memset(&path_config, 0, sizeof(path_config));
    (void)memset(&path_entry, 0, sizeof(path_entry));
    (void)memset(&path_state, 0, sizeof(path_state));
    (void)memset(&policy_state, 0, sizeof(policy_state));
    TEST_ASSERT(ucn_node_set_route_policy(&node, &policy) == UCN_ERR_CONFIG);
    TEST_ASSERT(ucn_node_install_local_path(&node, 1U, 11U, 11U, 1U, 1000U) ==
                UCN_ERR_CONFIG);
    TEST_ASSERT(ucn_node_install_local_path_capable(
                    &node, 1U, 11U, 11U, 1U, 1000U, &capability) ==
                UCN_ERR_CONFIG);
    TEST_ASSERT(ucn_node_send_path_install_capable(
                    &node, 11U, 1U, 11U, 0U, 0U, 1000U, &capability) ==
                UCN_ERR_CONFIG);
    TEST_ASSERT(ucn_node_request_path_trace(&node, 11U, 1U, NULL, NULL) ==
                UCN_ERR_CONFIG);
    TEST_ASSERT(ucn_path_is_expired(&path_entry, 0U));
    TEST_ASSERT(ucn_path_find(&path_state, 1U, 1U, 1U, 11U) == NULL);
    TEST_ASSERT(ucn_path_install(&path_state, &path_config) == UCN_ERR_CONFIG);
    TEST_ASSERT(ucn_path_install_capable(&path_state, &path_config,
                                         &capability) == UCN_ERR_CONFIG);
    TEST_ASSERT(ucn_path_revoke(&path_state, 1U, 1U, 1U, 11U) ==
                UCN_ERR_CONFIG);
    ucn_path_expire(&path_state, 0U);
    ucn_policy_refresh_link_quality(&policy_state, NULL, 0U, 0U);
    ucn_policy_expire_flows(&policy_state, 0U);
    ucn_policy_mark_path_down(&policy_state, 1U);
    ucn_policy_touch_q1_flow(&policy_state, 11U, PROFILE_ENDPOINT, 0U);
#endif
    return 0;
}

#if UCN_FEATURE_SERVICE
static int test_profile_service_switch(void)
{
    const ucn_service_binding_t binding = {
        PROFILE_ENDPOINT,
        1U,
        8U,
        UCN_SERVICE_TRAFFIC_MASK(UCN_TRAFFIC_Q1_REALTIME),
        UCN_SERVICE_DELIVERY_Q1_LATEST,
        UCN_SERVICE_SOURCE_MASK(1U),
        true,
        true,
        false
    };
    const ucn_service_router_config_t config = { 1U, &binding, 1U };
    ucn_service_router_t router;

    (void)memset(&router, 0, sizeof(router));
    TEST_ASSERT(ucn_service_router_init(&router, &config) == UCN_OK);
    return 0;
}
#endif

int test_profile(void)
{
    TEST_ASSERT(UCN_PROFILE >= UCN_PROFILE_NANO);
    TEST_ASSERT(UCN_PROFILE <= UCN_PROFILE_FULL);
    TEST_ASSERT(test_public_headers() == 0);
    TEST_ASSERT(test_node_storage_header() == 0);
    TEST_ASSERT(test_cluster_storage_header() == 0);
    TEST_ASSERT(test_profile_contract() == 0);
    TEST_ASSERT(test_profile_three_node_delivery() == 0);
#if UCN_FEATURE_SERVICE
    TEST_ASSERT(test_profile_service_switch() == 0);
#endif
    printf("UCN_PROFILE name=%s value=%d service=%d node_bytes=%zu "
           "link_bytes=%zu event_runtime_bytes=%zu stream_source_bytes=%zu "
           "stream_default_storage_bytes=%zu can_source_bytes=%zu "
           "can_default_storage_bytes=%zu transfer_bytes=%zu "
           "transfer_rx_bytes=%zu cluster_bytes=%zu federation_bytes=%zu\n",
           UCN_PROFILE_NAME, UCN_PROFILE, UCN_FEATURE_SERVICE,
           sizeof(ucn_node_t), sizeof(ucn_link_t),
           sizeof(ucn_event_runtime_t), sizeof(ucn_stream_source_t),
           sizeof(ucn_stream_source_default_storage_t),
           sizeof(ucn_can_source_t),
           sizeof(ucn_can_source_default_storage_t),
           sizeof(ucn_transfer_t),
           UCN_TRANSFER_RX_SLOTS * UCN_TRANSFER_MAX_MESSAGE_BYTES,
           sizeof(ucn_cluster_t), sizeof(ucn_cluster_federation_t));
    return 0;
}
