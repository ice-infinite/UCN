#include <string.h>

#include "test_support.h"
#include "ucn/ucn_service.h"

enum {
    TEST_SERVICE_SENSOR = 1,
    TEST_SERVICE_POWER = 2,
    TEST_SERVICE_CONTROL = 3,
    TEST_SERVICE_ACTUATOR = 4
};

#define TEST_MASK_SENSOR UCN_SERVICE_SOURCE_MASK(TEST_SERVICE_SENSOR)
#define TEST_MASK_POWER UCN_SERVICE_SOURCE_MASK(TEST_SERVICE_POWER)
#define TEST_MASK_CONTROL UCN_SERVICE_SOURCE_MASK(TEST_SERVICE_CONTROL)
#define TEST_Q0_MASK UCN_SERVICE_TRAFFIC_MASK(UCN_TRAFFIC_Q0_CRITICAL)
#define TEST_Q1_MASK UCN_SERVICE_TRAFFIC_MASK(UCN_TRAFFIC_Q1_REALTIME)

static const ucn_service_binding_t TEST_BINDINGS[] = {
    { 0x40U, TEST_SERVICE_CONTROL, 24U, TEST_Q1_MASK,
      UCN_SERVICE_DELIVERY_Q1_LATEST, TEST_MASK_SENSOR, true, true },
    { 0x42U, TEST_SERVICE_CONTROL, 16U, TEST_Q1_MASK,
      UCN_SERVICE_DELIVERY_Q1_LATEST, TEST_MASK_SENSOR, true, true },
    { 0x43U, TEST_SERVICE_CONTROL, 12U, TEST_Q1_MASK,
      UCN_SERVICE_DELIVERY_Q1_LATEST, TEST_MASK_SENSOR, true, true },
    { 0x50U, TEST_SERVICE_CONTROL, 16U, TEST_Q1_MASK,
      UCN_SERVICE_DELIVERY_Q1_LATEST, TEST_MASK_POWER, true, true },
    { 0x60U, TEST_SERVICE_ACTUATOR, 16U, TEST_Q0_MASK,
      UCN_SERVICE_DELIVERY_Q0_FIFO, TEST_MASK_CONTROL, true, true },
    { 0x61U, TEST_SERVICE_ACTUATOR, 16U, TEST_Q0_MASK,
      UCN_SERVICE_DELIVERY_Q0_FIFO, TEST_MASK_CONTROL, true, true }
};

static int service_init(ucn_service_router_t *router, ucn_node_id_t local_node_id)
{
    const ucn_service_router_config_t config = {
        local_node_id,
        TEST_BINDINGS,
        (uint8_t)(sizeof(TEST_BINDINGS) / sizeof(TEST_BINDINGS[0]))
    };

    (void)memset(router, 0, sizeof(*router));
    return ucn_service_router_init(router, &config) == UCN_OK ? 0 : 1;
}

static int test_service_configuration(void)
{
    ucn_service_router_t router;
    ucn_service_router_config_t config;
    ucn_service_binding_t bindings[2];

    (void)memset(&router, 0, sizeof(router));
    (void)memset(bindings, 0, sizeof(bindings));
    config.local_node_id = UINT32_C(1);
    config.bindings = TEST_BINDINGS;
    config.binding_count = (uint8_t)(sizeof(TEST_BINDINGS) / sizeof(TEST_BINDINGS[0]));
    TEST_ASSERT(ucn_service_router_init(NULL, &config) == UCN_ERR_ARGUMENT);
    config.binding_count = 0U;
    TEST_ASSERT(ucn_service_router_init(&router, &config) == UCN_ERR_ARGUMENT);
    config.binding_count = 1U;
    config.bindings = bindings;
    bindings[0] = TEST_BINDINGS[0];
    bindings[0].endpoint = UCN_MSG_DATA_Q1;
    TEST_ASSERT(ucn_service_router_init(&router, &config) == UCN_ERR_CONFIG);
    bindings[0] = TEST_BINDINGS[0];
    bindings[0].allowed_traffic_mask = TEST_Q0_MASK;
    TEST_ASSERT(ucn_service_router_init(&router, &config) == UCN_ERR_CONFIG);
    bindings[0] = TEST_BINDINGS[0];
    bindings[0].max_payload_length = (uint16_t)(UCN_SERVICE_MAX_PAYLOAD_BYTES + 1U);
    TEST_ASSERT(ucn_service_router_init(&router, &config) == UCN_ERR_CONFIG);
    bindings[0] = TEST_BINDINGS[0];
    bindings[1] = TEST_BINDINGS[0];
    config.binding_count = 2U;
    TEST_ASSERT(ucn_service_router_init(&router, &config) == UCN_ERR_CONFIG);
    return 0;
}

static int test_service_local_qos_and_acl(void)
{
    uint8_t first = 1U;
    uint8_t latest = 3U;
    uint8_t command;
    ucn_service_router_t router;
    ucn_service_message_t message;
    const ucn_service_stats_t *stats;
    uint8_t index;

    TEST_ASSERT(service_init(&router, UINT32_C(1)) == 0);
    TEST_ASSERT(ucn_service_send(&router, UINT32_C(1), TEST_SERVICE_SENSOR, 0x40U,
                                 UCN_TRAFFIC_Q1_REALTIME, &first, 1U) == UCN_OK);
    TEST_ASSERT(ucn_service_send(&router, UINT32_C(1), TEST_SERVICE_SENSOR, 0x40U,
                                 UCN_TRAFFIC_Q1_REALTIME, &latest, 1U) == UCN_OK);
    TEST_ASSERT(ucn_service_inbox_take(&router, TEST_SERVICE_CONTROL, 0x40U,
                                       &message) == UCN_OK);
    TEST_ASSERT(message.source_node_id == UINT32_C(1) &&
                message.source_service_id == TEST_SERVICE_SENSOR &&
                message.payload_length == 1U && message.payload[0] == latest);
    TEST_ASSERT(ucn_service_inbox_take(&router, TEST_SERVICE_CONTROL, 0x40U,
                                       &message) == UCN_ERR_NOT_FOUND);
    TEST_ASSERT(ucn_service_send(&router, UINT32_C(1), TEST_SERVICE_SENSOR, 0x60U,
                                 UCN_TRAFFIC_Q0_CRITICAL, &first, 1U) == UCN_ERR_ACCESS);
    TEST_ASSERT(ucn_service_send(&router, UINT32_C(1), TEST_SERVICE_SENSOR, 0x40U,
                                 UCN_TRAFFIC_Q0_CRITICAL, &first, 1U) == UCN_ERR_ARGUMENT);
    TEST_ASSERT(ucn_service_send(&router, UINT32_C(1), TEST_SERVICE_SENSOR, 0x40U,
                                 UCN_TRAFFIC_Q1_REALTIME, &first, 25U) == UCN_ERR_TOO_LARGE);
    TEST_ASSERT(ucn_service_set_ready(&router, 0x43U, false) == UCN_OK);
    TEST_ASSERT(ucn_service_send(&router, UINT32_C(1), TEST_SERVICE_SENSOR, 0x43U,
                                 UCN_TRAFFIC_Q1_REALTIME, &first, 1U) == UCN_ERR_NOT_FOUND);
    TEST_ASSERT(ucn_service_set_ready(&router, 0x43U, true) == UCN_OK);

    for (index = 0U; index < UCN_SERVICE_Q0_INBOX_DEPTH; ++index) {
        command = (uint8_t)(index + 10U);
        TEST_ASSERT(ucn_service_send(&router, UINT32_C(1), TEST_SERVICE_CONTROL, 0x60U,
                                     UCN_TRAFFIC_Q0_CRITICAL, &command, 1U) == UCN_OK);
    }
    command = 99U;
    TEST_ASSERT(ucn_service_send(&router, UINT32_C(1), TEST_SERVICE_CONTROL, 0x60U,
                                 UCN_TRAFFIC_Q0_CRITICAL, &command, 1U) == UCN_ERR_NO_SPACE);
    for (index = 0U; index < UCN_SERVICE_Q0_INBOX_DEPTH; ++index) {
        TEST_ASSERT(ucn_service_inbox_take(&router, TEST_SERVICE_ACTUATOR, 0x60U,
                                           &message) == UCN_OK);
        TEST_ASSERT(message.payload[0] == (uint8_t)(index + 10U));
    }
    TEST_ASSERT(ucn_service_inbox_take(&router, TEST_SERVICE_CONTROL, 0x60U,
                                       &message) == UCN_ERR_ACCESS);
    stats = ucn_service_get_stats(&router);
    TEST_ASSERT(stats != NULL && stats->local_delivered ==
                (uint32_t)(2U + UCN_SERVICE_Q0_INBOX_DEPTH) &&
                stats->q1_overwrites == 1U && stats->q0_inbox_full == 1U &&
                stats->local_acl_rejected == 1U && stats->not_ready == 1U);
    return 0;
}

static int test_service_remote_queue_and_inbound_guard(void)
{
    uint8_t q1_first = 1U;
    uint8_t q1_latest = 2U;
    uint8_t q0_command = 7U;
    uint8_t queue_value = 20U;
    ucn_service_router_t router_a, router_b, router_c;
    ucn_service_message_t message;
    ucn_frame_t frame;
    const ucn_service_stats_t *stats;

    TEST_ASSERT(service_init(&router_a, UINT32_C(1)) == 0);
    TEST_ASSERT(service_init(&router_b, UINT32_C(2)) == 0);
    TEST_ASSERT(service_init(&router_c, UINT32_C(3)) == 0);
    TEST_ASSERT(ucn_service_send(&router_a, UINT32_C(3), TEST_SERVICE_SENSOR, 0x40U,
                                 UCN_TRAFFIC_Q1_REALTIME, &q1_first, 1U) == UCN_OK);
    TEST_ASSERT(ucn_service_send(&router_a, UINT32_C(3), TEST_SERVICE_SENSOR, 0x40U,
                                 UCN_TRAFFIC_Q1_REALTIME, &q1_latest, 1U) == UCN_OK);
    TEST_ASSERT(ucn_service_send(&router_a, UINT32_C(3), TEST_SERVICE_CONTROL, 0x60U,
                                 UCN_TRAFFIC_Q0_CRITICAL, &q0_command, 1U) == UCN_OK);
    TEST_ASSERT(ucn_service_remote_tx_take(&router_a, &message) == UCN_OK);
    TEST_ASSERT(message.endpoint == 0x60U && message.traffic_class == UCN_TRAFFIC_Q0_CRITICAL &&
                message.payload[0] == q0_command);
    TEST_ASSERT(ucn_service_remote_tx_take(&router_a, &message) == UCN_OK);
    TEST_ASSERT(message.endpoint == 0x40U && message.traffic_class == UCN_TRAFFIC_Q1_REALTIME &&
                message.payload[0] == q1_latest && message.destination_node_id == UINT32_C(3));
    TEST_ASSERT(ucn_service_remote_tx_take(&router_a, &message) == UCN_ERR_NOT_FOUND);
    stats = ucn_service_get_stats(&router_a);
    TEST_ASSERT(stats != NULL && stats->remote_enqueued == 3U &&
                stats->remote_q1_overwrites == 1U && stats->remote_tx_reads == 2U);

    for (uint8_t index = 0U; index < UCN_SERVICE_REMOTE_TX_Q0_DEPTH; ++index) {
        queue_value = (uint8_t)(20U + index);
        TEST_ASSERT(ucn_service_send(&router_a, (ucn_node_id_t)(UINT32_C(10) + index),
                                     TEST_SERVICE_CONTROL, 0x60U,
                                     UCN_TRAFFIC_Q0_CRITICAL, &queue_value, 1U) == UCN_OK);
    }
    TEST_ASSERT(ucn_service_send(&router_a, UINT32_C(20), TEST_SERVICE_CONTROL, 0x60U,
                                 UCN_TRAFFIC_Q0_CRITICAL, &queue_value, 1U) == UCN_ERR_NO_SPACE);
    while (ucn_service_remote_tx_take(&router_a, &message) == UCN_OK) {
    }
    for (uint8_t index = 0U; index < UCN_SERVICE_REMOTE_TX_Q1_DEPTH; ++index) {
        queue_value = (uint8_t)(30U + index);
        TEST_ASSERT(ucn_service_send(&router_a, (ucn_node_id_t)(UINT32_C(30) + index),
                                     TEST_SERVICE_SENSOR, 0x40U,
                                     UCN_TRAFFIC_Q1_REALTIME, &queue_value, 1U) == UCN_OK);
    }
    TEST_ASSERT(ucn_service_send(&router_a, UINT32_C(40), TEST_SERVICE_SENSOR, 0x40U,
                                 UCN_TRAFFIC_Q1_REALTIME, &queue_value, 1U) == UCN_ERR_NO_SPACE);
    stats = ucn_service_get_stats(&router_a);
    TEST_ASSERT(stats != NULL && stats->remote_q0_full == 1U &&
                stats->remote_q1_full == 1U);

    (void)memset(&frame, 0, sizeof(frame));
    frame.message_type = 0x40U;
    frame.traffic_class = UCN_TRAFFIC_Q1_REALTIME;
    frame.source = UINT32_C(1);
    frame.destination = UINT32_C(3);
    frame.payload = &q1_latest;
    frame.payload_length = 1U;
    /* A relay must never dispatch an Endpoint that belongs to C. */
    TEST_ASSERT(ucn_service_deliver_remote(&router_b, &frame) == UCN_ERR_NETWORK);
    TEST_ASSERT(ucn_service_inbox_take(&router_b, TEST_SERVICE_CONTROL, 0x40U,
                                       &message) == UCN_ERR_NOT_FOUND);
    TEST_ASSERT(ucn_service_deliver_remote(&router_c, &frame) == UCN_OK);
    TEST_ASSERT(ucn_service_inbox_take(&router_c, TEST_SERVICE_CONTROL, 0x40U,
                                       &message) == UCN_OK);
    TEST_ASSERT(message.source_node_id == UINT32_C(1) &&
                message.source_service_id == UCN_SERVICE_ID_NONE &&
                message.payload[0] == q1_latest);
    return 0;
}

int test_service(void)
{
    if (test_service_configuration() != 0) {
        return 1;
    }
    if (test_service_local_qos_and_acl() != 0) {
        return 1;
    }
    return test_service_remote_queue_and_inbound_guard();
}
