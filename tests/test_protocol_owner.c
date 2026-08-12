#include <string.h>

#include "test_support.h"
#include "ucn/ucn_frame.h"
#include "ucn/ports/ucn_port_bare_metal.h"
#include "ucn/ports/ucn_port_freertos.h"
#include "ucn/ports/ucn_port_host_fake.h"
#include "ucn/ports/ucn_port_nuttx.h"
#include "ucn/ports/ucn_port_rtthread.h"
#include "ucn/ports/ucn_port_zephyr.h"

typedef struct protocol_owner_fake_context {
    uint32_t now_ms;
    uint32_t critical_enters;
    uint32_t critical_exits;
    uint32_t isr_critical_enters;
    uint32_t isr_critical_exits;
    ucn_port_critical_token_t next_isr_token;
    ucn_port_critical_token_t last_isr_enter_token;
    ucn_port_critical_token_t last_isr_exit_token;
    uint32_t notifications;
    uint32_t notifications_from_isr;
    uint32_t waits;
    uint32_t last_wait_ms;
    uint32_t sends;
    bool link_is_up;
} protocol_owner_fake_context_t;

typedef struct protocol_owner_receive_state {
    uint32_t count;
    ucn_node_id_t source;
} protocol_owner_receive_state_t;

static uint32_t protocol_owner_now_ms(void *context)
{
    const protocol_owner_fake_context_t *fake =
        (const protocol_owner_fake_context_t *)context;

    return fake->now_ms;
}

static void protocol_owner_enter_critical(void *context)
{
    protocol_owner_fake_context_t *fake =
        (protocol_owner_fake_context_t *)context;

    fake->critical_enters++;
}

static void protocol_owner_exit_critical(void *context)
{
    protocol_owner_fake_context_t *fake =
        (protocol_owner_fake_context_t *)context;

    fake->critical_exits++;
}

static ucn_port_critical_token_t protocol_owner_enter_critical_from_isr(
    void *context)
{
    protocol_owner_fake_context_t *fake =
        (protocol_owner_fake_context_t *)context;

    fake->isr_critical_enters++;
    fake->next_isr_token++;
    fake->last_isr_enter_token = fake->next_isr_token;
    return fake->last_isr_enter_token;
}

static void protocol_owner_exit_critical_from_isr(
    void *context,
    ucn_port_critical_token_t token)
{
    protocol_owner_fake_context_t *fake =
        (protocol_owner_fake_context_t *)context;

    fake->isr_critical_exits++;
    fake->last_isr_exit_token = token;
}

static void protocol_owner_notify(void *context, bool from_isr)
{
    protocol_owner_fake_context_t *fake =
        (protocol_owner_fake_context_t *)context;

    fake->notifications++;
    if (from_isr) {
        fake->notifications_from_isr++;
    }
}

static void protocol_owner_wait(void *context, uint32_t max_wait_ms)
{
    protocol_owner_fake_context_t *fake =
        (protocol_owner_fake_context_t *)context;

    fake->waits++;
    fake->last_wait_ms = max_wait_ms;
}

static ucn_result_t protocol_owner_link_send(ucn_link_t *link,
                                              const uint8_t *frame,
                                              size_t length)
{
    protocol_owner_fake_context_t *fake =
        (protocol_owner_fake_context_t *)link->context;

    (void)frame;
    (void)length;
    fake->sends++;
    return UCN_OK;
}

static ucn_result_t protocol_owner_link_status(const ucn_link_t *link,
                                                ucn_link_status_t *status)
{
    const protocol_owner_fake_context_t *fake =
        (const protocol_owner_fake_context_t *)link->context;

    status->is_up = fake->link_is_up;
    status->mtu = link->mtu;
    status->tx_errors = 0U;
    status->rx_errors = 0U;
    return UCN_OK;
}

static const ucn_link_ops_t PROTOCOL_OWNER_LINK_OPS = {
    NULL, protocol_owner_link_send, NULL, protocol_owner_link_status, NULL, NULL
};

static const ucn_port_ops_t PROTOCOL_OWNER_OPS = {
    protocol_owner_now_ms, NULL, NULL, NULL,
    protocol_owner_enter_critical, protocol_owner_exit_critical,
    protocol_owner_enter_critical_from_isr,
    protocol_owner_exit_critical_from_isr
};

static const ucn_freertos_port_ops_t FREERTOS_OPS = {
    protocol_owner_notify, protocol_owner_wait
};
static const ucn_zephyr_port_ops_t ZEPHYR_OPS = {
    protocol_owner_notify, protocol_owner_wait
};
static const ucn_nuttx_port_ops_t NUTTX_OPS = {
    protocol_owner_notify, protocol_owner_wait
};
static const ucn_rtthread_port_ops_t RTTHREAD_OPS = {
    protocol_owner_notify, protocol_owner_wait
};
static const ucn_host_fake_port_ops_t HOST_FAKE_OPS = {
    protocol_owner_notify, protocol_owner_wait
};

#if UCN_FEATURE_DYNAMIC_MESH
#define UCN_TEST_PROTOCOL_OWNER_ISR_NOTIFICATIONS 2U
#else
#define UCN_TEST_PROTOCOL_OWNER_ISR_NOTIFICATIONS 1U
#endif
#define UCN_TEST_PROTOCOL_OWNER_PORT_ISR_ENQUEUES 5U

static void protocol_owner_receive(void *context, const ucn_frame_t *frame)
{
    protocol_owner_receive_state_t *state =
        (protocol_owner_receive_state_t *)context;

    state->count++;
    state->source = frame->source;
}

static ucn_result_t protocol_owner_encode_frame(
    uint8_t message_type,
    ucn_network_id_t network_id,
    ucn_node_id_t source,
    ucn_node_id_t destination,
    ucn_sequence_t sequence,
    uint8_t *encoded,
    size_t *encoded_length)
{
    uint8_t hello_payload = UCN_WIRE_PROFILE_W3_BACKBONE;
    ucn_frame_t frame;

    (void)memset(&frame, 0, sizeof(frame));
    frame.message_type = message_type;
    frame.traffic_class = message_type == UCN_MSG_HELLO ?
                              UCN_TRAFFIC_Q0_CRITICAL : UCN_TRAFFIC_Q1_REALTIME;
    frame.hop_limit = 1U;
    frame.network_id = network_id;
    frame.source = source;
    frame.destination = destination;
    frame.sequence = sequence;
    if (message_type == UCN_MSG_HELLO) {
        frame.payload = &hello_payload;
        frame.payload_length = 1U;
    }
    return ucn_frame_encode(&frame, encoded, UCN_MAX_FRAME_BYTES, encoded_length);
}

static void protocol_owner_config_init(ucn_protocol_owner_config_t *config,
                                       ucn_node_t *node,
                                       ucn_adapter_rx_queue_t *queue,
                                       protocol_owner_fake_context_t *fake)
{
    (void)memset(config, 0, sizeof(*config));
    config->node = node;
    config->rx_queue = queue;
    config->port_ops = &PROTOCOL_OWNER_OPS;
    config->port_context = fake;
    config->max_rx_frames_per_step = 1U;
#if UCN_FEATURE_SERVICE
    config->max_bridge_requests_per_step = 1U;
#endif
}

static int test_platform_port_split(
    const ucn_protocol_owner_config_t *owner_config,
    ucn_link_t *link,
    protocol_owner_fake_context_t *fake)
{
    ucn_bare_metal_port_t bare_metal;
    ucn_freertos_port_t freertos;
    ucn_zephyr_port_t zephyr;
    ucn_nuttx_port_t nuttx;
    ucn_rtthread_port_t rtthread;
    ucn_host_fake_port_t host_fake;
    const ucn_freertos_port_config_t freertos_config = {
        *owner_config, &FREERTOS_OPS, fake
    };
    const ucn_zephyr_port_config_t zephyr_config = {
        *owner_config, &ZEPHYR_OPS, fake
    };
    const ucn_nuttx_port_config_t nuttx_config = {
        *owner_config, &NUTTX_OPS, fake
    };
    const ucn_rtthread_port_config_t rtthread_config = {
        *owner_config, &RTTHREAD_OPS, fake
    };
    const ucn_host_fake_port_config_t host_fake_config = {
        *owner_config, &HOST_FAKE_OPS, fake
    };
    ucn_freertos_port_config_t invalid_freertos = freertos_config;
    const uint8_t malformed_frame[] = { 0U };
    size_t pumped = 0U;
    uint8_t bridged = 0U;
    uint32_t isr_enters_before = fake->isr_critical_enters;
    uint32_t isr_exits_before = fake->isr_critical_exits;

    (void)memset(&bare_metal, 0, sizeof(bare_metal));
    (void)memset(&freertos, 0, sizeof(freertos));
    (void)memset(&zephyr, 0, sizeof(zephyr));
    (void)memset(&nuttx, 0, sizeof(nuttx));
    (void)memset(&rtthread, 0, sizeof(rtthread));
    (void)memset(&host_fake, 0, sizeof(host_fake));

    TEST_ASSERT(ucn_bare_metal_port_init(NULL, owner_config) == UCN_ERR_ARGUMENT);
    TEST_ASSERT(ucn_bare_metal_port_init(&bare_metal, owner_config) == UCN_OK);
    TEST_ASSERT(ucn_bare_metal_port_poll(&bare_metal, NULL, NULL) == UCN_OK);
    TEST_ASSERT(ucn_bare_metal_port_get_stats(&bare_metal) != NULL);
    invalid_freertos.ops = NULL;
    TEST_ASSERT(ucn_freertos_port_init(&freertos, &invalid_freertos) ==
                UCN_ERR_ARGUMENT);

    TEST_ASSERT(ucn_freertos_port_init(&freertos, &freertos_config) == UCN_OK);
    TEST_ASSERT(ucn_freertos_port_task_wait(
                    &freertos, UCN_MAX_STEP_INTERVAL_MS + 1U) == UCN_OK);
    TEST_ASSERT(ucn_freertos_port_get_runtime_stats(&freertos)->last_wait_ms ==
                UCN_MAX_STEP_INTERVAL_MS);
    TEST_ASSERT(ucn_zephyr_port_init(&zephyr, &zephyr_config) == UCN_OK);
    TEST_ASSERT(ucn_zephyr_port_thread_wait(&zephyr, 1U) == UCN_OK);
    TEST_ASSERT(ucn_nuttx_port_init(&nuttx, &nuttx_config) == UCN_OK);
    TEST_ASSERT(ucn_nuttx_port_worker_wait(&nuttx, 1U) == UCN_OK);
    TEST_ASSERT(ucn_rtthread_port_init(&rtthread, &rtthread_config) == UCN_OK);
    TEST_ASSERT(ucn_rtthread_port_thread_wait(&rtthread, 1U) == UCN_OK);
    TEST_ASSERT(ucn_host_fake_port_init(&host_fake, &host_fake_config) == UCN_OK);
    TEST_ASSERT(ucn_host_fake_port_wait(&host_fake, 1U) == UCN_OK);
    TEST_ASSERT(fake->waits == 5U);
    TEST_ASSERT(ucn_freertos_port_rx_enqueue(&freertos, link, malformed_frame,
                                             sizeof(malformed_frame), true) == UCN_OK);
    TEST_ASSERT(ucn_freertos_port_task_step(&freertos, &pumped, &bridged) == UCN_OK);
    TEST_ASSERT(pumped == 1U);
    TEST_ASSERT(ucn_zephyr_port_rx_enqueue(&zephyr, link, malformed_frame,
                                           sizeof(malformed_frame), true) == UCN_OK);
    TEST_ASSERT(ucn_zephyr_port_thread_step(&zephyr, &pumped, &bridged) == UCN_OK);
    TEST_ASSERT(pumped == 1U);
    TEST_ASSERT(ucn_nuttx_port_rx_enqueue(&nuttx, link, malformed_frame,
                                          sizeof(malformed_frame), true) == UCN_OK);
    TEST_ASSERT(ucn_nuttx_port_worker_step(&nuttx, &pumped, &bridged) == UCN_OK);
    TEST_ASSERT(pumped == 1U);
    TEST_ASSERT(ucn_rtthread_port_rx_enqueue(&rtthread, link, malformed_frame,
                                             sizeof(malformed_frame), true) == UCN_OK);
    TEST_ASSERT(ucn_rtthread_port_thread_step(&rtthread, &pumped, &bridged) == UCN_OK);
    TEST_ASSERT(pumped == 1U);
    TEST_ASSERT(ucn_host_fake_port_rx_enqueue(&host_fake, link, malformed_frame,
                                              sizeof(malformed_frame), true) == UCN_OK);
    TEST_ASSERT(ucn_host_fake_port_step(&host_fake, &pumped, &bridged) == UCN_OK);
    TEST_ASSERT(pumped == 1U);
    TEST_ASSERT(fake->isr_critical_enters ==
                isr_enters_before + UCN_TEST_PROTOCOL_OWNER_PORT_ISR_ENQUEUES);
    TEST_ASSERT(fake->isr_critical_exits ==
                isr_exits_before + UCN_TEST_PROTOCOL_OWNER_PORT_ISR_ENQUEUES);
    TEST_ASSERT(fake->last_isr_enter_token == fake->last_isr_exit_token);
    return 0;
}

#if UCN_FEATURE_SERVICE
static int test_protocol_owner_service_bridge(
    ucn_protocol_owner_config_t *owner_config,
    protocol_owner_fake_context_t *fake)
{
    static const ucn_service_binding_t BINDINGS[] = {
        { 0x40U, 1U, 8U,
          UCN_SERVICE_TRAFFIC_MASK(UCN_TRAFFIC_Q1_REALTIME),
          UCN_SERVICE_DELIVERY_Q1_LATEST, UCN_SERVICE_SOURCE_MASK(1U),
          true, true, false }
    };
    const ucn_service_router_config_t router_config = {
        UINT32_C(1), BINDINGS, 1U
    };
    ucn_host_fake_port_config_t port_config;
    uint8_t payload = 0x5AU;
    ucn_host_fake_port_t port;
    ucn_service_router_t router;
    ucn_service_protocol_bridge_t bridge;
    size_t pumped = 0U;
    uint8_t bridged = 0U;
    uint32_t sends_before;

    (void)memset(&port, 0, sizeof(port));
    (void)memset(&router, 0, sizeof(router));
    (void)memset(&bridge, 0, sizeof(bridge));
    TEST_ASSERT(ucn_service_router_init(&router, &router_config) == UCN_OK);
    TEST_ASSERT(ucn_service_protocol_bridge_init(&bridge, &router,
                                                 owner_config->node) == UCN_OK);
    TEST_ASSERT(ucn_service_protocol_bridge_install_endpoint_handlers(&bridge) ==
                UCN_OK);
    owner_config->bridge = &bridge;
    owner_config->max_bridge_requests_per_step = 1U;
    port_config.owner = *owner_config;
    port_config.ops = &HOST_FAKE_OPS;
    port_config.runtime_context = fake;
    TEST_ASSERT(ucn_host_fake_port_init(&port, &port_config) == UCN_OK);
    TEST_ASSERT(ucn_service_send(&router, UINT32_C(2), 1U, 0x40U,
                                 UCN_TRAFFIC_Q1_REALTIME, &payload, 1U) == UCN_OK);
    sends_before = fake->sends;
    fake->now_ms = UINT32_C(37);
    TEST_ASSERT(ucn_host_fake_port_step(&port, &pumped, &bridged) == UCN_OK);
    TEST_ASSERT(pumped == 0U && bridged == 1U && fake->sends > sends_before);
    TEST_ASSERT(owner_config->node->now_ms == UINT32_C(37));
    TEST_ASSERT(ucn_host_fake_port_get_stats(&port)->
                    bridge_requests_processed == 1U);
    owner_config->bridge = NULL;
    return 0;
}
#endif

int test_protocol_owner(void)
{
    const ucn_config_t node_config = {
        UINT32_C(0x534F5254), UINT32_C(1), 4U
    };
    ucn_node_t node;
    ucn_link_t link;
    ucn_adapter_rx_queue_t queue;
    ucn_host_fake_port_t port;
    ucn_protocol_owner_config_t owner_config;
    ucn_host_fake_port_config_t port_config;
    protocol_owner_fake_context_t fake;
    protocol_owner_receive_state_t receive_state;
    uint8_t encoded[UCN_MAX_FRAME_BYTES];
    size_t encoded_length = 0U;
    size_t pumped = 0U;
    uint8_t bridged = 0U;

    (void)memset(&node, 0, sizeof(node));
    (void)memset(&link, 0, sizeof(link));
    (void)memset(&queue, 0, sizeof(queue));
    (void)memset(&port, 0, sizeof(port));
    (void)memset(&fake, 0, sizeof(fake));
    (void)memset(&receive_state, 0, sizeof(receive_state));
    fake.link_is_up = true;
    link.ops = &PROTOCOL_OWNER_LINK_OPS;
    link.context = &fake;
    link.link_id = 1U;
    link.mtu = UCN_MAX_FRAME_BYTES;

    TEST_ASSERT(ucn_node_init(&node, &node_config) == UCN_OK);
#if UCN_FEATURE_DYNAMIC_MESH
    TEST_ASSERT(ucn_node_set_join_policy(&node, UCN_JOIN_OPEN, NULL, NULL) == UCN_OK);
#else
    link.peer_node_id = UINT32_C(2);
    TEST_ASSERT(ucn_node_register_link(&node, &link) == UCN_OK);
#endif
    TEST_ASSERT(ucn_adapter_rx_queue_init(&queue, &PROTOCOL_OWNER_OPS, &fake) == UCN_OK);
    protocol_owner_config_init(&owner_config, &node, &queue, &fake);
    TEST_ASSERT(ucn_protocol_owner_init(NULL, &owner_config) == UCN_ERR_ARGUMENT);
    TEST_ASSERT(test_platform_port_split(&owner_config, &link, &fake) == 0);

    port_config.owner = owner_config;
    port_config.ops = &HOST_FAKE_OPS;
    port_config.runtime_context = &fake;
    TEST_ASSERT(ucn_host_fake_port_init(&port, &port_config) == UCN_OK);

#if UCN_FEATURE_DYNAMIC_MESH
    TEST_ASSERT(protocol_owner_encode_frame(UCN_MSG_HELLO, node_config.network_id,
                                            UINT32_C(2), node_config.node_id, 1U,
                                            encoded, &encoded_length) == UCN_OK);
    TEST_ASSERT(ucn_host_fake_port_rx_enqueue(&port, &link, encoded,
                                               encoded_length, true) == UCN_OK);
    TEST_ASSERT(ucn_host_fake_port_step(&port, &pumped, &bridged) == UCN_OK);
    TEST_ASSERT(pumped == 1U && bridged == 0U && link.peer_node_id == UINT32_C(2));
#endif

    ucn_node_set_rx_handler(&node, protocol_owner_receive, &receive_state);
    TEST_ASSERT(protocol_owner_encode_frame(UCN_MSG_DATA_Q1, node_config.network_id,
                                            UINT32_C(2), node_config.node_id, 2U,
                                            encoded, &encoded_length) == UCN_OK);
    TEST_ASSERT(ucn_host_fake_port_rx_enqueue(&port, &link, encoded,
                                               encoded_length, true) == UCN_OK);
    TEST_ASSERT(protocol_owner_encode_frame(UCN_MSG_DATA_Q1, node_config.network_id,
                                            UINT32_C(2), node_config.node_id, 3U,
                                            encoded, &encoded_length) == UCN_OK);
    TEST_ASSERT(ucn_host_fake_port_rx_enqueue(&port, &link, encoded,
                                               encoded_length, false) == UCN_OK);
    TEST_ASSERT(receive_state.count == 0U && fake.notifications >= 2U);
    TEST_ASSERT(ucn_host_fake_port_step(&port, &pumped, &bridged) == UCN_OK);
    TEST_ASSERT(pumped == 1U && bridged == 0U && receive_state.count == 1U);
    TEST_ASSERT(ucn_host_fake_port_step(&port, &pumped, &bridged) == UCN_OK);
    TEST_ASSERT(pumped == 1U && receive_state.count == 2U &&
                receive_state.source == UINT32_C(2));
    TEST_ASSERT(ucn_host_fake_port_rx_enqueue(
                    &port, &link, encoded, UCN_MAX_FRAME_BYTES + 1U, false) ==
                UCN_ERR_TOO_LARGE);
    TEST_ASSERT(ucn_host_fake_port_get_stats(&port)->rx_rejected == 1U);
    TEST_ASSERT(ucn_host_fake_port_get_runtime_stats(&port)->
                    notifications_from_isr ==
                    UCN_TEST_PROTOCOL_OWNER_ISR_NOTIFICATIONS);
    TEST_ASSERT(fake.critical_enters == fake.critical_exits);
    TEST_ASSERT(fake.isr_critical_enters ==
                UCN_TEST_PROTOCOL_OWNER_PORT_ISR_ENQUEUES +
                    UCN_TEST_PROTOCOL_OWNER_ISR_NOTIFICATIONS);
    TEST_ASSERT(fake.isr_critical_enters == fake.isr_critical_exits);
    TEST_ASSERT(fake.last_isr_enter_token == fake.last_isr_exit_token);

#if UCN_FEATURE_SERVICE
    TEST_ASSERT(test_protocol_owner_service_bridge(&owner_config, &fake) == 0);
#endif
    return 0;
}
