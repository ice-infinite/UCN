#include <string.h>

#include "test_support.h"
#include "ucn/ucn_node.h"

typedef struct endpoint_link_context {
    ucn_node_t *peer;
    ucn_link_t *peer_ingress;
} endpoint_link_context_t;

typedef struct endpoint_receive_state {
    uint32_t count;
    uint8_t last_payload;
} endpoint_receive_state_t;

static ucn_result_t endpoint_link_send(ucn_link_t *link,
                                       const uint8_t *frame,
                                       size_t length)
{
    endpoint_link_context_t *context = (endpoint_link_context_t *)link->context;

    return ucn_node_receive(context->peer, context->peer_ingress, frame, length);
}

static ucn_result_t endpoint_link_status(const ucn_link_t *link,
                                         ucn_link_status_t *status)
{
    status->is_up = true;
    status->mtu = link->mtu;
    status->tx_errors = 0U;
    status->rx_errors = 0U;
    return UCN_OK;
}

static const ucn_link_ops_t ENDPOINT_LINK_OPS = {
    NULL, endpoint_link_send, NULL, endpoint_link_status, NULL, NULL
};

static int endpoint_init_node(ucn_node_t *node, ucn_node_id_t id)
{
    ucn_config_t config;

    config.network_id = UINT32_C(0xBADC0DE1);
    config.node_id = id;
    config.default_hop_limit = 3U;
    return ucn_node_init(node, &config) == UCN_OK ? 0 : 1;
}

static void endpoint_receive(void *context, const ucn_frame_t *frame)
{
    endpoint_receive_state_t *state = (endpoint_receive_state_t *)context;

    state->count++;
    state->last_payload = frame->payload[0];
}

static void endpoint_generic_receive(void *context, const ucn_frame_t *frame)
{
    endpoint_receive_state_t *state = (endpoint_receive_state_t *)context;

    (void)frame;
    state->count++;
}

int test_endpoint(void)
{
    static const ucn_endpoint_t IMU_ENDPOINT = (ucn_endpoint_t)0x40U;
    static const ucn_endpoint_t BARO_ENDPOINT = (ucn_endpoint_t)0x41U;
    uint8_t imu_first = 1U;
    uint8_t imu_latest = 3U;
    uint8_t baro_value = 2U;
    uint8_t legacy_value = 4U;
    ucn_node_t a, b;
    ucn_link_t ab, ba;
    endpoint_link_context_t cab, cba;
    endpoint_receive_state_t imu_received, baro_received, generic_received;
    ucn_send_request_t request;

    (void)memset(&a, 0, sizeof(a));
    (void)memset(&b, 0, sizeof(b));
    (void)memset(&ab, 0, sizeof(ab));
    (void)memset(&ba, 0, sizeof(ba));
    (void)memset(&cab, 0, sizeof(cab));
    (void)memset(&cba, 0, sizeof(cba));
    (void)memset(&imu_received, 0, sizeof(imu_received));
    (void)memset(&baro_received, 0, sizeof(baro_received));
    (void)memset(&generic_received, 0, sizeof(generic_received));
    (void)memset(&request, 0, sizeof(request));
    TEST_ASSERT(endpoint_init_node(&a, UINT32_C(1)) == 0);
    TEST_ASSERT(endpoint_init_node(&b, UINT32_C(2)) == 0);
    ab.ops = &ENDPOINT_LINK_OPS; ab.context = &cab; ab.link_id = 1U;
    ab.mtu = UCN_MAX_FRAME_BYTES; ab.peer_node_id = UINT32_C(2);
    ba.ops = &ENDPOINT_LINK_OPS; ba.context = &cba; ba.link_id = 2U;
    ba.mtu = UCN_MAX_FRAME_BYTES; ba.peer_node_id = UINT32_C(1);
    cab.peer = &b; cab.peer_ingress = &ba;
    cba.peer = &a; cba.peer_ingress = &ab;
    TEST_ASSERT(ucn_node_register_link(&a, &ab) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&b, &ba) == UCN_OK);

    TEST_ASSERT(ucn_endpoint_is_static(IMU_ENDPOINT));
    TEST_ASSERT(!ucn_endpoint_is_static(UCN_MSG_DATA_Q1));
    TEST_ASSERT(ucn_message_type_is_control(UCN_MSG_PATH_PROBE));
    TEST_ASSERT(!ucn_message_type_is_control(IMU_ENDPOINT));
    TEST_ASSERT(ucn_node_set_endpoint_handler(&b, IMU_ENDPOINT,
                                              endpoint_receive, &imu_received) == UCN_OK);
    TEST_ASSERT(ucn_node_set_endpoint_handler(&b, BARO_ENDPOINT,
                                              endpoint_receive, &baro_received) == UCN_OK);
    TEST_ASSERT(ucn_node_set_endpoint_handler(&b, UCN_MSG_DATA_Q1,
                                              endpoint_receive, &imu_received) == UCN_ERR_ARGUMENT);
    ucn_node_set_rx_handler(&b, endpoint_generic_receive, &generic_received);

    TEST_ASSERT(ucn_node_send_endpoint(&a, UINT32_C(2), IMU_ENDPOINT,
                                       UCN_TRAFFIC_Q1_REALTIME, &imu_first, 1U) == UCN_OK);
    TEST_ASSERT(imu_received.count == 1U && imu_received.last_payload == imu_first);
    TEST_ASSERT(baro_received.count == 0U && generic_received.count == 0U);
    TEST_ASSERT(ucn_node_send_endpoint(&a, UINT32_C(2), UCN_MSG_DATA_Q1,
                                       UCN_TRAFFIC_Q1_REALTIME, &legacy_value, 1U) ==
                UCN_ERR_ARGUMENT);
    TEST_ASSERT(ucn_node_send(&a, UINT32_C(2), UCN_MSG_ROUTE_REQ,
                              UCN_TRAFFIC_Q1_REALTIME, &legacy_value, 1U) ==
                UCN_ERR_ARGUMENT);
    TEST_ASSERT(ucn_node_send(&a, UINT32_C(2), UCN_MSG_DATA_Q1,
                              UCN_TRAFFIC_Q1_REALTIME, &legacy_value, 1U) == UCN_OK);
    TEST_ASSERT(generic_received.count == 1U);

    request.destination = UINT32_C(2);
    request.message_type = IMU_ENDPOINT;
    request.traffic_class = UCN_TRAFFIC_Q1_REALTIME;
    request.delivery = UCN_DELIVERY_LATEST_VALUE;
    request.payload = &imu_first;
    request.payload_length = 1U;
    TEST_ASSERT(ucn_node_enqueue(&a, &request) == UCN_OK);
    request.message_type = BARO_ENDPOINT;
    request.payload = &baro_value;
    TEST_ASSERT(ucn_node_enqueue(&a, &request) == UCN_OK);
    request.message_type = IMU_ENDPOINT;
    request.payload = &imu_latest;
    TEST_ASSERT(ucn_node_enqueue(&a, &request) == UCN_OK);
    TEST_ASSERT(ucn_node_step(&a, 10U) == UCN_OK);
    TEST_ASSERT(ucn_node_step(&a, 11U) == UCN_OK);
    TEST_ASSERT(imu_received.count == 2U && imu_received.last_payload == imu_latest);
    TEST_ASSERT(baro_received.count == 1U && baro_received.last_payload == baro_value);
    return 0;
}
