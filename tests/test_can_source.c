#include <string.h>

#include "test_support.h"
#include "ucn/adapters/ucn_can_source.h"
#include "ucn/ucn_frame.h"
#include "ucn/ucn_node.h"

#define CAN_NETWORK_ID UINT32_C(0x43)
#define CAN_LOCAL_NODE UINT32_C(2)
#define CAN_REMOTE_A UINT32_C(1)
#define CAN_REMOTE_B UINT32_C(3)
#define CAN_ENDPOINT ((ucn_endpoint_t)0x44U)

typedef struct can_fake_port {
    uint32_t now_ms;
    uint32_t task_enters;
    uint32_t task_exits;
    uint32_t isr_enters;
    uint32_t isr_exits;
    ucn_port_critical_token_t next_token;
    ucn_port_critical_token_t last_exit_token;
} can_fake_port_t;

typedef struct can_receive_state {
    uint32_t count;
    uint32_t last_source;
    uint8_t last_value;
} can_receive_state_t;

typedef struct can_resolver {
    ucn_link_t *link_a;
    ucn_link_t *link_b;
} can_resolver_t;

typedef struct can_fixture {
    ucn_event_runtime_t runtime;
    ucn_node_t node;
    ucn_adapter_rx_queue_t queue;
    ucn_can_source_t source_a;
    ucn_can_source_t source_b;
    ucn_can_source_default_storage_t storage_a;
    ucn_can_source_default_storage_t storage_b;
    ucn_link_t link_a;
    ucn_link_t link_b;
    can_fake_port_t port;
    can_receive_state_t received;
    can_resolver_t resolver;
} can_fixture_t;

static uint32_t can_now_ms(void *context)
{
    return ((const can_fake_port_t *)context)->now_ms;
}

static void can_enter_task(void *context)
{
    ((can_fake_port_t *)context)->task_enters++;
}

static void can_exit_task(void *context)
{
    ((can_fake_port_t *)context)->task_exits++;
}

static ucn_port_critical_token_t can_enter_isr(void *context)
{
    can_fake_port_t *port = (can_fake_port_t *)context;

    port->isr_enters++;
    port->next_token++;
    return port->next_token;
}

static void can_exit_isr(void *context, ucn_port_critical_token_t token)
{
    can_fake_port_t *port = (can_fake_port_t *)context;

    port->isr_exits++;
    port->last_exit_token = token;
}

static const ucn_port_ops_t CAN_PORT_OPS = {
    .struct_size = (uint16_t)sizeof(ucn_port_ops_t),
    .api_version = UCN_PORT_OPS_API_VERSION,
    .now_ms = can_now_ms,
    .enter_critical = can_enter_task,
    .exit_critical = can_exit_task,
    .enter_critical_from_isr = can_enter_isr,
    .exit_critical_from_isr = can_exit_isr
};

static const ucn_port_ops_t CAN_PORT_OPS_NO_ISR = {
    .struct_size = (uint16_t)sizeof(ucn_port_ops_t),
    .api_version = UCN_PORT_OPS_API_VERSION,
    .now_ms = can_now_ms,
    .enter_critical = can_enter_task,
    .exit_critical = can_exit_task
};

static ucn_result_t can_link_send(ucn_link_t *link,
                                  const uint8_t *data,
                                  size_t length)
{
    (void)link;
    (void)data;
    (void)length;
    return UCN_OK;
}

static ucn_result_t can_link_status(const ucn_link_t *link,
                                    ucn_link_status_t *status)
{
    (void)memset(status, 0, sizeof(*status));
    status->is_up = true;
    status->mtu = link->mtu;
    return UCN_OK;
}

static const ucn_link_ops_t CAN_LINK_OPS = {
    NULL, can_link_send, NULL, can_link_status, NULL, NULL
};

static void can_receive(void *context, const ucn_frame_t *frame)
{
    can_receive_state_t *state = (can_receive_state_t *)context;

    state->count++;
    state->last_source = frame->source;
    state->last_value = frame->payload_length == 0U ? 0U : frame->payload[0];
}

static ucn_result_t can_resolve(void *context,
                                uint32_t identifier,
                                bool extended,
                                ucn_link_t **ingress_link)
{
    can_resolver_t *resolver = (can_resolver_t *)context;

    if (extended || ingress_link == NULL) {
        return UCN_ERR_NOT_FOUND;
    }
    if (identifier == UINT32_C(0x100) ||
        identifier == UINT32_C(0x102)) {
        *ingress_link = resolver->link_a;
        return UCN_OK;
    }
    if (identifier == UINT32_C(0x101)) {
        *ingress_link = resolver->link_b;
        return UCN_OK;
    }
    return UCN_ERR_NOT_FOUND;
}

static ucn_result_t can_fixture_init_with_port(
    can_fixture_t *fixture,
    const ucn_port_ops_t *port_ops,
    uint8_t max_rounds,
    size_t source_budget,
    ucn_can_source_mode_t mode)
{
    const ucn_config_t node_config = {
        CAN_NETWORK_ID, CAN_LOCAL_NODE, 4U
    };
    ucn_event_runtime_config_t runtime_config;
    ucn_can_source_config_t source_config;
    ucn_result_t result;

    (void)memset(fixture, 0, sizeof(*fixture));
    result = ucn_node_init(&fixture->node, &node_config);
    if (result != UCN_OK) {
        return result;
    }
    fixture->link_a.ops = &CAN_LINK_OPS;
    fixture->link_a.link_id = 1U;
    fixture->link_a.mtu = UCN_MAX_FRAME_BYTES;
    fixture->link_a.peer_node_id = CAN_REMOTE_A;
    fixture->link_b.ops = &CAN_LINK_OPS;
    fixture->link_b.link_id = 2U;
    fixture->link_b.mtu = UCN_MAX_FRAME_BYTES;
    fixture->link_b.peer_node_id = CAN_REMOTE_B;
    result = ucn_node_register_link(&fixture->node, &fixture->link_a);
    if (result != UCN_OK) {
        return result;
    }
    result = ucn_node_register_link(&fixture->node, &fixture->link_b);
    if (result != UCN_OK) {
        return result;
    }
    result = ucn_node_set_endpoint_handler(
        &fixture->node, CAN_ENDPOINT, can_receive, &fixture->received);
    if (result != UCN_OK) {
        return result;
    }
    result = ucn_adapter_rx_queue_init(&fixture->queue, port_ops,
                                       &fixture->port);
    if (result != UCN_OK) {
        return result;
    }
    (void)memset(&runtime_config, 0, sizeof(runtime_config));
    runtime_config.owner.node = &fixture->node;
    runtime_config.owner.rx_queue = &fixture->queue;
    runtime_config.owner.port_ops = port_ops;
    runtime_config.owner.port_context = &fixture->port;
    runtime_config.owner.max_rx_frames_per_step = 1U;
#if UCN_FEATURE_SERVICE
    runtime_config.owner.max_bridge_requests_per_step = 1U;
#endif
    runtime_config.max_drain_rounds = max_rounds;
    runtime_config.max_source_work_per_round = source_budget;
    result = ucn_event_runtime_init(&fixture->runtime, &runtime_config);
    if (result != UCN_OK) {
        return result;
    }
    fixture->resolver.link_a = &fixture->link_a;
    fixture->resolver.link_b = &fixture->link_b;

    (void)memset(&source_config, 0, sizeof(source_config));
    source_config.runtime = &fixture->runtime;
    source_config.source_id = 0U;
    source_config.mode = mode;
    source_config.ring_storage = fixture->storage_a.ring;
    source_config.ring_capacity =
        sizeof(fixture->storage_a.ring) / sizeof(fixture->storage_a.ring[0]);
    source_config.reassembly_slots = fixture->storage_a.slots;
    source_config.reassembly_slot_count =
        sizeof(fixture->storage_a.slots) /
        sizeof(fixture->storage_a.slots[0]);
    source_config.reassembly_storage = &fixture->storage_a.reassembly[0][0];
    source_config.reassembly_storage_capacity =
        sizeof(fixture->storage_a.reassembly);
    source_config.max_frame_bytes =
        mode == UCN_CAN_SOURCE_CAN_FD_DIRECT ? 64U : UCN_MAX_FRAME_BYTES;
    source_config.reassembly_timeout_ms = 50U;
    source_config.resolve_ingress = can_resolve;
    source_config.resolve_context = &fixture->resolver;
    return ucn_can_source_init(&fixture->source_a, &source_config);
}

static ucn_result_t can_fixture_init(can_fixture_t *fixture,
                                     uint8_t max_rounds,
                                     size_t source_budget,
                                     ucn_can_source_mode_t mode)
{
    return can_fixture_init_with_port(fixture, &CAN_PORT_OPS, max_rounds,
                                      source_budget, mode);
}

static ucn_result_t can_fixture_add_second_source(can_fixture_t *fixture)
{
    ucn_can_source_config_t config;

    (void)memset(&config, 0, sizeof(config));
    config.runtime = &fixture->runtime;
    config.source_id = 1U;
    config.mode = UCN_CAN_SOURCE_CAN_FD_DIRECT;
    config.ring_storage = fixture->storage_b.ring;
    config.ring_capacity =
        sizeof(fixture->storage_b.ring) / sizeof(fixture->storage_b.ring[0]);
    config.max_frame_bytes = 64U;
    config.resolve_ingress = can_resolve;
    config.resolve_context = &fixture->resolver;
    return ucn_can_source_init(&fixture->source_b, &config);
}

static ucn_result_t can_make_encoded(uint32_t source,
                                     uint32_t sequence,
                                     size_t payload_length,
                                     uint8_t value,
                                     uint8_t *encoded,
                                     size_t *encoded_length)
{
    uint8_t payload[64];
    ucn_frame_t frame;
    size_t index;

    if (payload_length > sizeof(payload)) {
        return UCN_ERR_ARGUMENT;
    }
    for (index = 0U; index < payload_length; ++index) {
        payload[index] = (uint8_t)(value + (uint8_t)index);
    }
    (void)memset(&frame, 0, sizeof(frame));
    frame.message_type = CAN_ENDPOINT;
    frame.wire_profile = UCN_WIRE_PROFILE_W0_LOCAL;
    frame.traffic_class = UCN_TRAFFIC_Q1_REALTIME;
    frame.hop_limit = 1U;
    frame.network_id = CAN_NETWORK_ID;
    frame.source = source;
    frame.destination = CAN_LOCAL_NODE;
    frame.sequence = sequence;
    frame.session_id = 1U;
    frame.payload = payload;
    frame.payload_length = (uint16_t)payload_length;
    return ucn_frame_encode(&frame, encoded, UCN_MAX_FRAME_BYTES,
                            encoded_length);
}

static ucn_result_t can_make_fd(uint32_t identifier,
                                uint32_t source,
                                uint32_t sequence,
                                size_t payload_length,
                                uint8_t value,
                                ucn_can_frame_t *physical)
{
    uint8_t encoded[UCN_MAX_FRAME_BYTES];
    size_t encoded_length = 0U;
    size_t fd_length = 0U;
    ucn_result_t result;

    result = can_make_encoded(source, sequence, payload_length, value,
                              encoded, &encoded_length);
    if (result != UCN_OK) {
        return result;
    }
    (void)memset(physical, 0, sizeof(*physical));
    physical->identifier = identifier;
    physical->flags = UCN_CAN_FRAME_FLAG_FD | UCN_CAN_FRAME_FLAG_BRS;
    result = ucn_can_fd_carrier_encode(encoded, encoded_length,
                                       physical->data, &fd_length);
    physical->length = (uint8_t)fd_length;
    return result;
}

static int can_drain(can_fixture_t *fixture, size_t limit)
{
    size_t index;

    for (index = 0U; index < limit &&
                         ucn_event_runtime_has_pending(&fixture->runtime);
         ++index) {
        ucn_event_runtime_run_result_t run;

        if (ucn_event_runtime_run(&fixture->runtime, false, &run) != UCN_OK) {
            return 1;
        }
    }
    return ucn_event_runtime_has_pending(&fixture->runtime) ? 1 : 0;
}

static int test_can_carrier_contracts(void)
{
    static const size_t frame_lengths[] = {
        17U, 18U, 20U, 21U, 24U, 25U, 32U, 33U, 48U, 49U, 64U
    };
    static const size_t rounded_lengths[] = {
        20U, 20U, 20U, 24U, 24U, 32U, 32U, 48U, 48U, 64U, 64U
    };
    size_t index;

    for (index = 0U;
         index < sizeof(frame_lengths) / sizeof(frame_lengths[0]); ++index) {
        uint8_t encoded[UCN_MAX_FRAME_BYTES];
        uint8_t fd[UCN_CAN_FD_MAX_DATA_BYTES];
        size_t encoded_length = 0U;
        size_t fd_length = 0U;
        size_t peeked_length = 0U;
        size_t pad;

        TEST_ASSERT(can_make_encoded(
                        CAN_REMOTE_A, (uint32_t)(index + 1U),
                        frame_lengths[index] - UCN_FRAME_W0_HEADER_SIZE,
                        (uint8_t)index, encoded, &encoded_length) == UCN_OK);
        TEST_ASSERT(encoded_length == frame_lengths[index]);
        TEST_ASSERT(ucn_can_fd_carrier_encode(
                        encoded, encoded_length, fd, &fd_length) == UCN_OK);
        TEST_ASSERT(fd_length == rounded_lengths[index]);
        TEST_ASSERT(ucn_frame_peek_encoded_size(
                        fd, fd_length, &peeked_length) == UCN_OK);
        TEST_ASSERT(peeked_length == encoded_length);
        for (pad = encoded_length; pad < fd_length; ++pad) {
            TEST_ASSERT(fd[pad] == 0U);
        }
    }

    {
        uint8_t encoded[UCN_MAX_FRAME_BYTES];
        uint8_t segment[UCN_CAN_CLASSIC_MAX_DATA_BYTES];
        size_t encoded_length = 0U;
        size_t segment_length = 0U;
        size_t segment_count;

        TEST_ASSERT(can_make_encoded(CAN_REMOTE_A, 50U, 40U, 0x50U,
                                     encoded, &encoded_length) == UCN_OK);
        segment_count =
            ucn_can_classic_carrier_segment_count(encoded_length);
        TEST_ASSERT(segment_count > 2U && segment_count <= 256U);
        TEST_ASSERT(ucn_can_classic_carrier_encode_segment(
                        encoded, encoded_length, 7U, 0U, segment,
                        &segment_length) == UCN_OK);
        TEST_ASSERT(segment_length == 8U);
        TEST_ASSERT(segment[0] == UCN_CAN_CLASSIC_CARRIER_START);
        TEST_ASSERT(ucn_can_classic_carrier_encode_segment(
                        encoded, encoded_length, 7U,
                        (uint16_t)(segment_count - 1U), segment,
                        &segment_length) == UCN_OK);
        TEST_ASSERT(segment_length >= 4U && segment_length <= 8U);
        TEST_ASSERT(ucn_can_classic_carrier_encode_segment(
                        encoded, encoded_length, 7U,
                        (uint16_t)segment_count, segment,
                        &segment_length) == UCN_ERR_NOT_FOUND);
    }
    return 0;
}

static int test_can_fd_multi_source_and_padding(void)
{
    can_fixture_t fixture;
    ucn_can_frame_t first;
    ucn_can_frame_t second;
    ucn_can_frame_t invalid_padding;
    ucn_can_frame_t filtered;
    const ucn_can_source_stats_t *stats;

    TEST_ASSERT(can_fixture_init(&fixture, 8U, 4U,
                                 UCN_CAN_SOURCE_MIXED) == UCN_OK);
    TEST_ASSERT(can_fixture_add_second_source(&fixture) == UCN_OK);
    TEST_ASSERT(can_make_fd(0x100U, CAN_REMOTE_A, 1U, 1U, 0xA1U,
                            &first) == UCN_OK);
    TEST_ASSERT(can_make_fd(0x101U, CAN_REMOTE_B, 2U, 4U, 0xB2U,
                            &second) == UCN_OK);
    TEST_ASSERT(ucn_can_source_write(&fixture.source_a, &first) == UCN_OK);
    TEST_ASSERT(ucn_can_source_write_from_isr(&fixture.source_b, &second) ==
                UCN_OK);
    TEST_ASSERT(can_drain(&fixture, 16U) == 0);
    TEST_ASSERT(fixture.received.count == 2U);
    TEST_ASSERT(fixture.received.last_source == CAN_REMOTE_B);
    TEST_ASSERT(fixture.port.isr_enters != 0U);
    TEST_ASSERT(fixture.port.isr_enters == fixture.port.isr_exits);
    TEST_ASSERT(fixture.port.last_exit_token != 0U);

    TEST_ASSERT(can_make_fd(0x100U, CAN_REMOTE_A, 3U, 1U, 0xC3U,
                            &invalid_padding) == UCN_OK);
    TEST_ASSERT(invalid_padding.length >
                UCN_FRAME_W0_HEADER_SIZE + 1U);
    invalid_padding.data[invalid_padding.length - 1U] = 1U;
    TEST_ASSERT(ucn_can_source_write(&fixture.source_a, &invalid_padding) ==
                UCN_OK);
    filtered = first;
    filtered.identifier = 0x555U;
    TEST_ASSERT(ucn_can_source_write(&fixture.source_a, &filtered) == UCN_OK);
    TEST_ASSERT(can_drain(&fixture, 16U) == 0);
    TEST_ASSERT(fixture.received.count == 2U);
    stats = ucn_can_source_get_stats(&fixture.source_a);
    TEST_ASSERT(stats != NULL);
    TEST_ASSERT(stats->fd_padding_errors == 1U);
    TEST_ASSERT(stats->filtered_frames == 1U);
    return 0;
}

static int test_can_classic_reassembly_and_faults(void)
{
    can_fixture_t fixture;
    uint8_t encoded[UCN_MAX_FRAME_BYTES];
    size_t encoded_length = 0U;
    size_t segment_count;
    size_t index;

    TEST_ASSERT(can_fixture_init(&fixture, 8U, 4U,
                                 UCN_CAN_SOURCE_CLASSIC_CARRIER) == UCN_OK);
    TEST_ASSERT(can_make_encoded(CAN_REMOTE_A, 10U, 40U, 0x61U,
                                 encoded, &encoded_length) == UCN_OK);
    segment_count = ucn_can_classic_carrier_segment_count(encoded_length);
    for (index = 0U; index < segment_count; ++index) {
        ucn_can_frame_t physical;
        size_t length = 0U;

        (void)memset(&physical, 0, sizeof(physical));
        physical.identifier = 0x100U;
        TEST_ASSERT(ucn_can_classic_carrier_encode_segment(
                        encoded, encoded_length, 9U, (uint16_t)index,
                        physical.data, &length) == UCN_OK);
        physical.length = (uint8_t)length;
        TEST_ASSERT(ucn_can_source_write(&fixture.source_a, &physical) ==
                    UCN_OK);
        TEST_ASSERT(can_drain(&fixture, 8U) == 0);
    }
    TEST_ASSERT(fixture.received.count == 1U);
    TEST_ASSERT(fixture.received.last_value == 0x61U);

    /* START followed by segment 2 invalidates the slot.  Sending segment 1
     * afterwards must not resurrect or splice the abandoned transfer. */
    {
        ucn_can_frame_t start;
        ucn_can_frame_t segment_two;
        ucn_can_frame_t segment_one;
        size_t length = 0U;

        (void)memset(&start, 0, sizeof(start));
        (void)memset(&segment_two, 0, sizeof(segment_two));
        (void)memset(&segment_one, 0, sizeof(segment_one));
        start.identifier = segment_two.identifier = segment_one.identifier =
            0x100U;
        TEST_ASSERT(ucn_can_classic_carrier_encode_segment(
                        encoded, encoded_length, 10U, 0U, start.data,
                        &length) == UCN_OK);
        start.length = (uint8_t)length;
        TEST_ASSERT(ucn_can_classic_carrier_encode_segment(
                        encoded, encoded_length, 10U, 2U, segment_two.data,
                        &length) == UCN_OK);
        segment_two.length = (uint8_t)length;
        TEST_ASSERT(ucn_can_classic_carrier_encode_segment(
                        encoded, encoded_length, 10U, 1U, segment_one.data,
                        &length) == UCN_OK);
        segment_one.length = (uint8_t)length;
        TEST_ASSERT(ucn_can_source_write(&fixture.source_a, &start) == UCN_OK);
        TEST_ASSERT(ucn_can_source_write(&fixture.source_a, &segment_two) ==
                    UCN_OK);
        TEST_ASSERT(ucn_can_source_write(&fixture.source_a, &segment_one) ==
                    UCN_OK);
        TEST_ASSERT(can_drain(&fixture, 16U) == 0);
        TEST_ASSERT(fixture.received.count == 1U);
        TEST_ASSERT(ucn_can_source_get_stats(&fixture.source_a)
                        ->carrier_order_errors >= 2U);
    }

    /* A missing continuation is reclaimed only by the bounded timeout scan. */
    {
        ucn_can_frame_t start;
        ucn_can_frame_t restart;
        ucn_event_runtime_run_result_t run;
        size_t length = 0U;

        (void)memset(&start, 0, sizeof(start));
        (void)memset(&restart, 0, sizeof(restart));
        start.identifier = 0x100U;
        restart.identifier = 0x100U;
        TEST_ASSERT(ucn_can_classic_carrier_encode_segment(
                        encoded, encoded_length, 11U, 0U, start.data,
                        &length) == UCN_OK);
        start.length = (uint8_t)length;
        TEST_ASSERT(ucn_can_classic_carrier_encode_segment(
                        encoded, encoded_length, 12U, 0U, restart.data,
                        &length) == UCN_OK);
        restart.length = (uint8_t)length;
        TEST_ASSERT(ucn_can_source_write(&fixture.source_a, &start) == UCN_OK);
        TEST_ASSERT(ucn_can_source_write(&fixture.source_a, &restart) ==
                    UCN_OK);
        TEST_ASSERT(can_drain(&fixture, 8U) == 0);
        TEST_ASSERT(ucn_can_source_get_stats(&fixture.source_a)
                        ->carrier_restarts == 1U);
        fixture.port.now_ms = 51U;
        TEST_ASSERT(ucn_event_runtime_run(&fixture.runtime, true, &run) ==
                    UCN_OK);
        TEST_ASSERT(ucn_can_source_get_stats(&fixture.source_a)
                        ->carrier_timeouts == 1U);
    }

    /* Carrier completion is not a trust boundary.  Corrupting one final data
     * byte still reaches the normal v5 CRC decoder and never the Endpoint. */
    {
        uint8_t corrupt_encoded[UCN_MAX_FRAME_BYTES];
        size_t corrupt_length = 0U;
        size_t corrupt_segments;
        const ucn_adapter_rx_stats_t *queue_stats;

        TEST_ASSERT(can_make_encoded(CAN_REMOTE_A, 12U, 18U, 0x6AU,
                                     corrupt_encoded, &corrupt_length) ==
                    UCN_OK);
        corrupt_segments =
            ucn_can_classic_carrier_segment_count(corrupt_length);
        for (index = 0U; index < corrupt_segments; ++index) {
            ucn_can_frame_t physical;
            size_t length = 0U;

            (void)memset(&physical, 0, sizeof(physical));
            physical.identifier = 0x100U;
            TEST_ASSERT(ucn_can_classic_carrier_encode_segment(
                            corrupt_encoded, corrupt_length, 13U,
                            (uint16_t)index, physical.data, &length) == UCN_OK);
            physical.length = (uint8_t)length;
            if (index + 1U == corrupt_segments) {
                physical.data[physical.length - 1U] ^= 0x01U;
            }
            TEST_ASSERT(ucn_can_source_write(&fixture.source_a, &physical) ==
                        UCN_OK);
            TEST_ASSERT(can_drain(&fixture, 8U) == 0);
        }
        TEST_ASSERT(fixture.received.count == 1U);
        queue_stats = ucn_adapter_rx_get_stats(&fixture.queue);
        TEST_ASSERT(queue_stats != NULL);
        TEST_ASSERT(queue_stats->rejected_by_core >= 1U);
    }
    /* Two minimum W0 Carriers fit exactly in the default eight-frame Driver
     * Ring.  They intentionally arrive before one Runtime drain.  Completion
     * of the first Carrier must stop the round so the following same-ID START
     * cannot clear the completed slot before submission. */
    if (UCN_CAN_SOURCE_DEFAULT_RING_FRAMES >= 8U) {
        uint8_t first_encoded[UCN_MAX_FRAME_BYTES];
        uint8_t second_encoded[UCN_MAX_FRAME_BYTES];
        size_t first_length = 0U;
        size_t second_length = 0U;
        size_t first_segments;
        size_t second_segments;
        uint32_t restarts_before = ucn_can_source_get_stats(
            &fixture.source_a)->carrier_restarts;

        TEST_ASSERT(can_make_encoded(CAN_REMOTE_A, 60U, 1U, 0xA6U,
                                     first_encoded, &first_length) == UCN_OK);
        TEST_ASSERT(can_make_encoded(CAN_REMOTE_A, 61U, 1U, 0xB7U,
                                     second_encoded, &second_length) == UCN_OK);
        first_segments = ucn_can_classic_carrier_segment_count(first_length);
        second_segments = ucn_can_classic_carrier_segment_count(second_length);
        TEST_ASSERT(first_segments + second_segments == 8U);
        for (index = 0U; index < first_segments + second_segments; ++index) {
            const bool second = index >= first_segments;
            const size_t segment_index = second ? index - first_segments : index;
            const uint8_t *carrier = second ? second_encoded : first_encoded;
            const size_t carrier_length = second ? second_length : first_length;
            ucn_can_frame_t physical;
            size_t length = 0U;

            (void)memset(&physical, 0, sizeof(physical));
            physical.identifier = 0x100U;
            TEST_ASSERT(ucn_can_classic_carrier_encode_segment(
                            carrier, carrier_length, second ? 31U : 30U,
                            (uint16_t)segment_index, physical.data, &length) ==
                        UCN_OK);
            physical.length = (uint8_t)length;
            TEST_ASSERT(ucn_can_source_write(&fixture.source_a, &physical) ==
                        UCN_OK);
        }
        TEST_ASSERT(can_drain(&fixture, 16U) == 0);
        TEST_ASSERT(fixture.received.count == 3U);
        TEST_ASSERT(fixture.received.last_value == 0xB7U);
        TEST_ASSERT(ucn_can_source_get_stats(&fixture.source_a)
                        ->carrier_restarts == restarts_before);
        TEST_ASSERT(ucn_can_source_get_stats(&fixture.source_a)
                        ->frames_submitted >= 3U);
    }
    return 0;
}

static int test_can_slots_bus_state_and_backpressure(void)
{
    can_fixture_t fixture;
    uint8_t encoded[UCN_MAX_FRAME_BYTES];
    size_t encoded_length = 0U;
    ucn_can_frame_t starts[UCN_CAN_SOURCE_DEFAULT_REASSEMBLY_SLOTS + 1U];
    size_t index;
    ucn_can_source_health_t health;

    TEST_ASSERT(can_fixture_init(&fixture, 1U, 4U,
                                 UCN_CAN_SOURCE_MIXED) == UCN_OK);
    TEST_ASSERT(can_make_encoded(CAN_REMOTE_A, 20U, 20U, 0x71U,
                                 encoded, &encoded_length) == UCN_OK);
    for (index = 0U;
         index < UCN_CAN_SOURCE_DEFAULT_REASSEMBLY_SLOTS + 1U; ++index) {
        size_t length = 0U;

        (void)memset(&starts[index], 0, sizeof(starts[index]));
        starts[index].identifier = (uint32_t)(0x100U + index);
        TEST_ASSERT(ucn_can_classic_carrier_encode_segment(
                        encoded, encoded_length, (uint8_t)(20U + index), 0U,
                        starts[index].data, &length) == UCN_OK);
        starts[index].length = (uint8_t)length;
        TEST_ASSERT(ucn_can_source_write(&fixture.source_a, &starts[index]) ==
                    UCN_OK);
    }
    TEST_ASSERT(can_drain(&fixture, 8U) == 0);
    TEST_ASSERT(ucn_can_source_get_stats(&fixture.source_a)->carrier_no_slot ==
                1U);
    TEST_ASSERT(ucn_can_source_get_health(&fixture.source_a, &health) ==
                UCN_OK);
    TEST_ASSERT(health.active_reassembly_slots ==
                UCN_CAN_SOURCE_DEFAULT_REASSEMBLY_SLOTS);

    /* Down clears both incomplete slots, rejects RX, and requires an explicit
     * ACTIVE transition after hardware recovery. */
    TEST_ASSERT(ucn_can_source_set_bus_state(
                    &fixture.source_a, UCN_CAN_BUS_ERROR_PASSIVE) == UCN_OK);
    TEST_ASSERT(ucn_can_source_write(&fixture.source_a, &starts[0]) == UCN_OK);
    TEST_ASSERT(ucn_can_source_set_bus_state_from_isr(
                    &fixture.source_a, UCN_CAN_BUS_OFF) == UCN_OK);
    TEST_ASSERT(ucn_can_source_write(&fixture.source_a, &starts[0]) ==
                UCN_ERR_LINK_DOWN);
    TEST_ASSERT(ucn_can_source_set_bus_state(
                    &fixture.source_a, UCN_CAN_BUS_RECOVERING) == UCN_OK);
    TEST_ASSERT(ucn_can_source_write(&fixture.source_a, &starts[0]) ==
                UCN_ERR_LINK_DOWN);
    TEST_ASSERT(ucn_can_source_set_bus_state(
                    &fixture.source_a, UCN_CAN_BUS_ACTIVE) == UCN_OK);
    TEST_ASSERT(can_drain(&fixture, 8U) == 0);
    TEST_ASSERT(ucn_can_source_get_health(&fixture.source_a, &health) ==
                UCN_OK);
    TEST_ASSERT(health.active_reassembly_slots == 0U);
    TEST_ASSERT(health.bus_state == UCN_CAN_BUS_ACTIVE);
    TEST_ASSERT(ucn_can_source_get_stats(&fixture.source_a)->bus_off_events ==
                1U);
    TEST_ASSERT(ucn_can_source_get_stats(&fixture.source_a)
                    ->error_passive_events == 1U);
    TEST_ASSERT(ucn_can_source_get_stats(&fixture.source_a)->recoveries == 1U);

    /* Four FD frames in one source round overflow the two-frame Adapter Queue.
     * The current physical frame stays at Ring head and is retried later. */
    for (index = 0U; index < 4U; ++index) {
        ucn_can_frame_t fd;

        TEST_ASSERT(can_make_fd(0x100U, CAN_REMOTE_A,
                                (uint32_t)(30U + index), 1U,
                                (uint8_t)(0x80U + index), &fd) == UCN_OK);
        TEST_ASSERT(ucn_can_source_write(&fixture.source_a, &fd) == UCN_OK);
    }
    {
        ucn_event_runtime_run_result_t run;

        TEST_ASSERT(ucn_event_runtime_run(&fixture.runtime, false, &run) ==
                    UCN_OK);
    }
    TEST_ASSERT(ucn_can_source_get_stats(&fixture.source_a)
                    ->adapter_queue_backpressure >= 1U);
    TEST_ASSERT(can_drain(&fixture, 16U) == 0);
    TEST_ASSERT(fixture.received.count == 4U);
    TEST_ASSERT(fixture.received.last_value == 0x83U);
    return 0;
}

static int test_can_ring_and_isr_contract(void)
{
    can_fixture_t fixture;
    ucn_can_frame_t fd;
    size_t index;
    ucn_can_source_health_t health;

    TEST_ASSERT(can_fixture_init_with_port(
                    &fixture, &CAN_PORT_OPS_NO_ISR, 1U, 1U,
                    UCN_CAN_SOURCE_CAN_FD_DIRECT) == UCN_OK);
    TEST_ASSERT(can_make_fd(0x100U, CAN_REMOTE_A, 1U, 1U, 0x91U,
                            &fd) == UCN_OK);
    TEST_ASSERT(ucn_can_source_write_from_isr(&fixture.source_a, &fd) ==
                UCN_ERR_CONFIG);
    for (index = 0U; index < UCN_CAN_SOURCE_DEFAULT_RING_FRAMES; ++index) {
        TEST_ASSERT(ucn_can_source_write(&fixture.source_a, &fd) == UCN_OK);
    }
    TEST_ASSERT(ucn_can_source_write(&fixture.source_a, &fd) ==
                UCN_ERR_NO_SPACE);
    TEST_ASSERT(ucn_can_source_get_health(&fixture.source_a, &health) ==
                UCN_OK);
    TEST_ASSERT(health.pending_physical_frames ==
                UCN_CAN_SOURCE_DEFAULT_RING_FRAMES);
    TEST_ASSERT(health.queue_pressure_per_mille == 1000U);
    TEST_ASSERT(health.receive_failure_count >= 1U);
    TEST_ASSERT(ucn_can_source_reset(&fixture.source_a) == UCN_OK);
    TEST_ASSERT(ucn_can_source_get_health(&fixture.source_a, &health) ==
                UCN_OK);
    TEST_ASSERT(health.pending_physical_frames == 0U);
    return 0;
}

int test_can_source(void)
{
    int result = 0;

    result |= test_can_carrier_contracts();
    result |= test_can_fd_multi_source_and_padding();
    result |= test_can_classic_reassembly_and_faults();
    result |= test_can_slots_bus_state_and_backpressure();
    result |= test_can_ring_and_isr_contract();
    return result;
}
