#include <string.h>

#include "test_support.h"
#include "ucn/ucn_frame.h"
#include "ucn/ucn_node.h"

typedef struct wire_link_context {
    bool is_up;
    uint8_t last_frame[UCN_MAX_FRAME_BYTES];
    size_t last_length;
} wire_link_context_t;

static ucn_result_t wire_link_send(ucn_link_t *link,
                                   const uint8_t *frame,
                                   size_t length)
{
    wire_link_context_t *context = (wire_link_context_t *)link->context;

    if (length > sizeof(context->last_frame)) {
        return UCN_ERR_TOO_LARGE;
    }
    (void)memcpy(context->last_frame, frame, length);
    context->last_length = length;
    return UCN_OK;
}

static ucn_result_t wire_link_status(const ucn_link_t *link,
                                     ucn_link_status_t *status)
{
    const wire_link_context_t *context =
        (const wire_link_context_t *)link->context;

    status->is_up = context->is_up;
    status->mtu = link->mtu;
    status->tx_errors = 0U;
    status->rx_errors = 0U;
    return UCN_OK;
}

static const ucn_link_ops_t WIRE_LINK_OPS = {
    NULL,
    wire_link_send,
    NULL,
    wire_link_status,
    NULL,
    NULL
};

static void setup_link(ucn_link_t *link,
                       wire_link_context_t *context,
                       uint32_t link_id,
                       ucn_node_id_t peer,
                       size_t mtu)
{
    (void)memset(link, 0, sizeof(*link));
    (void)memset(context, 0, sizeof(*context));
    context->is_up = true;
    link->ops = &WIRE_LINK_OPS;
    link->context = context;
    link->link_id = link_id;
    link->peer_node_id = peer;
    link->mtu = mtu;
}

#if UCN_FEATURE_DYNAMIC_MESH
static uint32_t read_width_be(const uint8_t *data, uint8_t width)
{
    uint8_t index;
    uint32_t value = 0U;

    for (index = 0U; index < width; ++index) {
        value = (value << 8U) | data[index];
    }
    return value;
}

static int verify_control_profile(ucn_wire_profile_t profile,
                                  uint32_t network_id,
                                  ucn_node_id_t source,
                                  ucn_node_id_t target,
                                  uint8_t hop_limit)
{
    const ucn_wire_profile_descriptor_t *descriptor =
        ucn_wire_profile_get_descriptor(profile);
    ucn_config_t config = { network_id, source, hop_limit };
    ucn_node_t node;
    ucn_link_t link;
    wire_link_context_t context;
    ucn_frame_t decoded;

    TEST_ASSERT(descriptor != NULL);
    TEST_ASSERT(ucn_node_init(&node, &config) == UCN_OK);
    TEST_ASSERT(ucn_node_set_wire_profiles(&node, profile, profile) == UCN_OK);
    setup_link(&link, &context, (uint32_t)profile + 100U, 0U,
               UCN_MAX_FRAME_BYTES);

    TEST_ASSERT(ucn_node_broadcast_hello(&node, &link, 1U) == UCN_OK);
    TEST_ASSERT(ucn_frame_decode(context.last_frame, context.last_length,
                                 &decoded) == UCN_OK);
    TEST_ASSERT(decoded.message_type == UCN_MSG_HELLO);
    TEST_ASSERT(decoded.wire_profile == profile);
    TEST_ASSERT(decoded.payload_length == 0U);
    TEST_ASSERT(context.last_length ==
                ucn_frame_header_size_for_profile(profile, 0U));

    TEST_ASSERT(ucn_node_register_link(&node, &link) == UCN_OK);
    TEST_ASSERT(ucn_node_discover_route(&node, target, 10U) == UCN_OK);
    TEST_ASSERT(ucn_frame_decode(context.last_frame, context.last_length,
                                 &decoded) == UCN_OK);
    TEST_ASSERT(decoded.message_type == UCN_MSG_ROUTE_REQ);
    TEST_ASSERT(decoded.wire_profile == profile);
    TEST_ASSERT(decoded.payload_length ==
                (uint16_t)(descriptor->address_bytes + 8U));
    TEST_ASSERT(read_width_be(decoded.payload, descriptor->address_bytes) ==
                target);
    TEST_ASSERT(read_width_be(decoded.payload + descriptor->address_bytes, 4U) !=
                0U);
    return 0;
}
#endif

static int verify_profile_send(ucn_wire_profile_t profile,
                               uint32_t network_id,
                               ucn_node_id_t source,
                               ucn_node_id_t destination,
                               uint8_t hop_limit,
                               size_t expected_header)
{
    const uint8_t payload = 0x5AU;
    ucn_config_t config = { network_id, source, hop_limit };
    ucn_node_t node;
    ucn_link_t link;
    wire_link_context_t context;
    ucn_frame_t decoded;

    TEST_ASSERT(ucn_node_init(&node, &config) == UCN_OK);
    TEST_ASSERT(ucn_node_set_wire_profiles(&node, profile, profile) == UCN_OK);
    TEST_ASSERT(ucn_node_get_tx_wire_profile(&node) == profile);
    TEST_ASSERT(ucn_node_get_max_receive_wire_profile(&node) == profile);
    setup_link(&link, &context, (uint32_t)profile, destination,
               UCN_MAX_FRAME_BYTES);
    TEST_ASSERT(ucn_node_register_link(&node, &link) == UCN_OK);
    TEST_ASSERT(ucn_node_send(&node, destination, UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME, &payload, 1U) == UCN_OK);
    TEST_ASSERT(context.last_length == expected_header + 1U);
    TEST_ASSERT(ucn_frame_decode(context.last_frame, context.last_length,
                                 &decoded) == UCN_OK);
    TEST_ASSERT(decoded.wire_profile == profile);
    TEST_ASSERT(decoded.network_id == network_id);
    TEST_ASSERT(decoded.source == source);
    TEST_ASSERT(decoded.destination == destination);
    return 0;
}

static int verify_node_automatic_profile(void)
{
    static const uint8_t payload = 0xA6U;
    ucn_config_t config = { 42U, 1U, 4U };
    ucn_node_t node;
    ucn_link_t link;
    wire_link_context_t context;
    ucn_frame_t decoded;

    TEST_ASSERT(ucn_node_init(&node, &config) == UCN_OK);
    setup_link(&link, &context, 201U, 2U, UCN_MAX_FRAME_BYTES);
    TEST_ASSERT(ucn_node_register_link(&node, &link) == UCN_OK);
    TEST_ASSERT(ucn_node_set_link_wire_profile_limit(
                    &node, &link, UCN_WIRE_PROFILE_W0_LOCAL) == UCN_OK);
    TEST_ASSERT(ucn_node_get_link_wire_profile_limit(&node, &link) ==
                UCN_WIRE_PROFILE_W0_LOCAL);
    TEST_ASSERT(ucn_node_send(&node, 2U, UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME, &payload, 1U) ==
                UCN_ERR_UNSUPPORTED);
    TEST_ASSERT(!ucn_node_wire_profile_auto(&node));
    TEST_ASSERT(ucn_node_set_wire_profile_auto(&node, true) == UCN_OK);
    TEST_ASSERT(ucn_node_wire_profile_auto(&node));
    TEST_ASSERT(ucn_node_send(&node, 2U, UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME, &payload, 1U) == UCN_OK);
    TEST_ASSERT(ucn_frame_decode(context.last_frame, context.last_length,
                                 &decoded) == UCN_OK);
    TEST_ASSERT(decoded.wire_profile == UCN_WIRE_PROFILE_W0_LOCAL);
    TEST_ASSERT(decoded.hop_limit == 1U);

    TEST_ASSERT(ucn_node_init(&node, &config) == UCN_OK);
    TEST_ASSERT(ucn_node_set_wire_profile_auto(&node, true) == UCN_OK);
    setup_link(&link, &context, 202U, 300U, UCN_MAX_FRAME_BYTES);
    TEST_ASSERT(ucn_node_register_link(&node, &link) == UCN_OK);
    TEST_ASSERT(ucn_node_send(&node, 300U, UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME, &payload, 1U) == UCN_OK);
    TEST_ASSERT(ucn_frame_decode(context.last_frame, context.last_length,
                                 &decoded) == UCN_OK);
    TEST_ASSERT(decoded.wire_profile == UCN_WIRE_PROFILE_W1_EDGE);
    TEST_ASSERT(ucn_node_set_link_wire_profile_limit(
                    &node, &link, UCN_WIRE_PROFILE_W0_LOCAL) == UCN_OK);
    TEST_ASSERT(ucn_node_send(&node, 300U, UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME, &payload, 1U) ==
                UCN_ERR_TOO_LARGE);
    return 0;
}

int test_node_wire_profile(void)
{
    ucn_config_t config = { UINT32_C(42), UINT32_C(1), 4U };
    ucn_node_t node;
    ucn_node_t wide_node;
    ucn_link_t link;
    wire_link_context_t context;
    ucn_frame_t frame;
    uint8_t encoded[UCN_MAX_FRAME_BYTES];
    size_t encoded_length = 0U;

    TEST_ASSERT(verify_profile_send(UCN_WIRE_PROFILE_W0_LOCAL, 42U, 1U, 2U,
                                    4U, UCN_FRAME_W0_HEADER_SIZE) == 0);
    TEST_ASSERT(verify_profile_send(UCN_WIRE_PROFILE_W1_EDGE, 300U, 300U, 301U,
                                    16U, UCN_FRAME_W1_HEADER_SIZE) == 0);
    TEST_ASSERT(verify_profile_send(UCN_WIRE_PROFILE_W2_MESH, 70000U, 70000U,
                                    70001U, 16U,
                                    UCN_FRAME_W2_HEADER_SIZE) == 0);
    TEST_ASSERT(verify_profile_send(UCN_WIRE_PROFILE_W3_BACKBONE,
                                    UINT32_C(0xAABBCCDD), UINT32_C(0x1000001),
                                    UINT32_C(0x1000002), 16U,
                                    UCN_FRAME_W3_HEADER_SIZE) == 0);
    TEST_ASSERT(verify_node_automatic_profile() == 0);
#if UCN_FEATURE_DYNAMIC_MESH
    TEST_ASSERT(verify_control_profile(UCN_WIRE_PROFILE_W0_LOCAL, 42U, 1U, 2U,
                                       4U) == 0);
    TEST_ASSERT(verify_control_profile(UCN_WIRE_PROFILE_W1_EDGE, 300U, 300U,
                                       301U, 16U) == 0);
    TEST_ASSERT(verify_control_profile(UCN_WIRE_PROFILE_W2_MESH, 70000U, 70000U,
                                       70001U, 16U) == 0);
    TEST_ASSERT(verify_control_profile(UCN_WIRE_PROFILE_W3_BACKBONE,
                                       UINT32_C(0xAABBCCDD),
                                       UINT32_C(0x1000001),
                                       UINT32_C(0x1000002), 16U) == 0);
#endif

    TEST_ASSERT(ucn_node_init(&node, &config) == UCN_OK);
    TEST_ASSERT(ucn_node_get_tx_wire_profile(&node) ==
                UCN_WIRE_PROFILE_W3_BACKBONE);
    TEST_ASSERT(ucn_node_set_wire_profiles(
                    &node, UCN_WIRE_PROFILE_W1_EDGE,
                    UCN_WIRE_PROFILE_W0_LOCAL) == UCN_ERR_ARGUMENT);
    TEST_ASSERT(ucn_node_set_wire_profiles(
                    &node, UCN_WIRE_PROFILE_UNSPECIFIED,
                    UCN_WIRE_PROFILE_W3_BACKBONE) == UCN_ERR_ARGUMENT);
    TEST_ASSERT(ucn_node_set_wire_profiles(
                    &node, UCN_WIRE_PROFILE_W0_LOCAL,
                    UCN_WIRE_PROFILE_W0_LOCAL) == UCN_OK);
    TEST_ASSERT(ucn_node_set_plain_session_id(&node, 255U) == UCN_OK);
    TEST_ASSERT(ucn_node_set_plain_session_id(&node, 256U) ==
                UCN_ERR_TOO_LARGE);

    setup_link(&link, &context, 10U, 2U, UCN_FRAME_W0_HEADER_SIZE - 1U);
    TEST_ASSERT(ucn_node_register_link(&node, &link) == UCN_ERR_ARGUMENT);
    link.mtu = UCN_FRAME_W0_HEADER_SIZE;
    TEST_ASSERT(ucn_node_register_link(&node, &link) == UCN_OK);
    TEST_ASSERT(ucn_node_set_wire_profiles(
                    &node, UCN_WIRE_PROFILE_W1_EDGE,
                    UCN_WIRE_PROFILE_W1_EDGE) == UCN_ERR_CONFIG);

    (void)memset(&frame, 0, sizeof(frame));
    frame.message_type = UCN_MSG_DATA_Q1;
    frame.wire_profile = UCN_WIRE_PROFILE_W1_EDGE;
    frame.traffic_class = UCN_TRAFFIC_Q1_REALTIME;
    frame.hop_limit = 4U;
    frame.network_id = config.network_id;
    frame.source = 2U;
    frame.destination = config.node_id;
    frame.sequence = 1U;
    TEST_ASSERT(ucn_frame_encode(&frame, encoded, sizeof(encoded),
                                 &encoded_length) == UCN_OK);
    TEST_ASSERT(ucn_node_receive(&node, &link, encoded, encoded_length) ==
                UCN_ERR_UNSUPPORTED);

    TEST_ASSERT(ucn_node_init(&wide_node, &config) == UCN_OK);
    TEST_ASSERT(ucn_node_set_wire_profiles(
                    &wide_node, UCN_WIRE_PROFILE_W0_LOCAL,
                    UCN_WIRE_PROFILE_W3_BACKBONE) == UCN_OK);
    setup_link(&link, &context, 11U, 2U, UCN_FRAME_W3_HEADER_SIZE);
    TEST_ASSERT(ucn_node_register_link(&wide_node, &link) == UCN_OK);
    frame.wire_profile = UCN_WIRE_PROFILE_W0_LOCAL;
    TEST_ASSERT(ucn_frame_encode(&frame, encoded, sizeof(encoded),
                                 &encoded_length) == UCN_OK);
    {
        const ucn_result_t receive_result =
            ucn_node_receive(&wide_node, &link, encoded, encoded_length);

#if UCN_PROFILE == UCN_PROFILE_NANO
        TEST_ASSERT(receive_result == UCN_ERR_NOT_FOUND);
#else
        TEST_ASSERT(receive_result == UCN_OK);
#endif
    }

    config.network_id = 256U;
    TEST_ASSERT(ucn_node_init(&node, &config) == UCN_OK);
    TEST_ASSERT(ucn_node_set_wire_profiles(
                    &node, UCN_WIRE_PROFILE_W0_LOCAL,
                    UCN_WIRE_PROFILE_W0_LOCAL) == UCN_ERR_TOO_LARGE);
    config.network_id = 42U;
    config.node_id = 255U;
    TEST_ASSERT(ucn_node_init(&node, &config) == UCN_OK);
    TEST_ASSERT(ucn_node_set_wire_profiles(
                    &node, UCN_WIRE_PROFILE_W0_LOCAL,
                    UCN_WIRE_PROFILE_W0_LOCAL) == UCN_ERR_TOO_LARGE);
    config.node_id = 1U;
    config.default_hop_limit = 5U;
    TEST_ASSERT(ucn_node_init(&node, &config) == UCN_OK);
    TEST_ASSERT(ucn_node_set_wire_profiles(
                    &node, UCN_WIRE_PROFILE_W0_LOCAL,
                    UCN_WIRE_PROFILE_W0_LOCAL) == UCN_ERR_TOO_LARGE);
    return 0;
}
