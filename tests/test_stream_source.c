#include <string.h>

#include "test_support.h"
#include "ucn/adapters/ucn_stream_source.h"
#include "ucn/ucn_frame.h"
#include "ucn/ucn_node.h"

#define STREAM_NETWORK_ID UINT32_C(0x53)
#define STREAM_LOCAL_NODE UINT32_C(2)
#define STREAM_REMOTE_NODE UINT32_C(1)
#define STREAM_ENDPOINT ((ucn_endpoint_t)0x40U)

typedef struct stream_fake_port {
    uint32_t now_ms;
    uint32_t task_enters;
    uint32_t task_exits;
    uint32_t isr_enters;
    uint32_t isr_exits;
    ucn_port_critical_token_t next_token;
    ucn_port_critical_token_t last_exit_token;
} stream_fake_port_t;

typedef struct stream_receive_state {
    uint32_t count;
    uint8_t last_value;
} stream_receive_state_t;

typedef struct stream_fixture {
    ucn_event_runtime_t runtime;
    ucn_node_t node;
    ucn_adapter_rx_queue_t queue;
    ucn_stream_source_t source;
    ucn_stream_source_default_storage_t storage;
    ucn_link_t link;
    stream_fake_port_t port;
    stream_receive_state_t received;
} stream_fixture_t;

static uint32_t stream_now_ms(void *context)
{
    return ((const stream_fake_port_t *)context)->now_ms;
}

static void stream_enter_task(void *context)
{
    ((stream_fake_port_t *)context)->task_enters++;
}

static void stream_exit_task(void *context)
{
    ((stream_fake_port_t *)context)->task_exits++;
}

static ucn_port_critical_token_t stream_enter_isr(void *context)
{
    stream_fake_port_t *port = (stream_fake_port_t *)context;

    port->isr_enters++;
    port->next_token++;
    return port->next_token;
}

static void stream_exit_isr(void *context, ucn_port_critical_token_t token)
{
    stream_fake_port_t *port = (stream_fake_port_t *)context;

    port->isr_exits++;
    port->last_exit_token = token;
}

static const ucn_port_ops_t STREAM_PORT_OPS = {
    .struct_size = (uint16_t)sizeof(ucn_port_ops_t),
    .api_version = UCN_PORT_OPS_API_VERSION,
    .now_ms = stream_now_ms,
    .enter_critical = stream_enter_task,
    .exit_critical = stream_exit_task,
    .enter_critical_from_isr = stream_enter_isr,
    .exit_critical_from_isr = stream_exit_isr
};

static const ucn_port_ops_t STREAM_PORT_OPS_NO_ISR = {
    .struct_size = (uint16_t)sizeof(ucn_port_ops_t),
    .api_version = UCN_PORT_OPS_API_VERSION,
    .now_ms = stream_now_ms,
    .enter_critical = stream_enter_task,
    .exit_critical = stream_exit_task
};

static ucn_result_t stream_link_send(ucn_link_t *link,
                                     const uint8_t *data,
                                     size_t length)
{
    (void)link;
    (void)data;
    (void)length;
    return UCN_OK;
}

static ucn_result_t stream_link_status(const ucn_link_t *link,
                                       ucn_link_status_t *status)
{
    (void)memset(status, 0, sizeof(*status));
    status->is_up = true;
    status->mtu = link->mtu;
    return UCN_OK;
}

static const ucn_link_ops_t STREAM_LINK_OPS = {
    NULL, stream_link_send, NULL, stream_link_status, NULL, NULL
};

static void stream_receive(void *context, const ucn_frame_t *frame)
{
    stream_receive_state_t *state = (stream_receive_state_t *)context;

    state->count++;
    state->last_value = frame->payload_length == 0U ? 0U : frame->payload[0];
}

static ucn_result_t stream_fixture_init_with(
    stream_fixture_t *fixture,
    const ucn_port_ops_t *port_ops,
    uint8_t max_rounds,
    size_t source_budget,
    uint8_t *ring_storage,
    size_t ring_capacity,
    ucn_event_source_id_t source_id,
    size_t byte_budget,
    size_t error_budget)
{
    const ucn_config_t node_config = {
        STREAM_NETWORK_ID, STREAM_LOCAL_NODE, 4U
    };
    ucn_event_runtime_config_t runtime_config;
    ucn_stream_source_config_t source_config;
    ucn_result_t result;

    (void)memset(fixture, 0, sizeof(*fixture));
    result = ucn_node_init(&fixture->node, &node_config);
    if (result != UCN_OK) {
        return result;
    }
    fixture->link.ops = &STREAM_LINK_OPS;
    fixture->link.link_id = 1U;
    fixture->link.mtu = UCN_MAX_FRAME_BYTES;
    fixture->link.peer_node_id = STREAM_REMOTE_NODE;
    result = ucn_node_register_link(&fixture->node, &fixture->link);
    if (result != UCN_OK) {
        return result;
    }
    result = ucn_node_set_endpoint_handler(
        &fixture->node, STREAM_ENDPOINT, stream_receive, &fixture->received);
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

    (void)memset(&source_config, 0, sizeof(source_config));
    source_config.runtime = &fixture->runtime;
    source_config.source_id = source_id;
    source_config.ingress_link = &fixture->link;
    source_config.ring_storage = ring_storage == NULL ?
        fixture->storage.ring : ring_storage;
    source_config.ring_capacity = ring_storage == NULL ?
        sizeof(fixture->storage.ring) : ring_capacity;
    source_config.frame_storage = fixture->storage.frame;
    source_config.frame_capacity = sizeof(fixture->storage.frame);
    source_config.max_bytes_per_service = byte_budget;
    source_config.max_errors_per_service = error_budget;
    return ucn_stream_source_init(&fixture->source, &source_config);
}

static ucn_result_t stream_fixture_init(stream_fixture_t *fixture,
                                        uint8_t max_rounds,
                                        size_t source_budget)
{
    return stream_fixture_init_with(
        fixture, &STREAM_PORT_OPS, max_rounds, source_budget, NULL, 0U, 0U,
        0U, 0U);
}

static ucn_result_t stream_make_carrier(uint32_t sequence,
                                        uint8_t value,
                                        uint8_t *wire,
                                        size_t wire_capacity,
                                        size_t *wire_length)
{
    const uint8_t payload[] = { value, 0U, (uint8_t)(value ^ UINT8_C(0xA5)) };
    ucn_frame_t frame;
    uint8_t encoded[UCN_MAX_FRAME_BYTES];
    size_t encoded_length = 0U;
    ucn_result_t result;

    (void)memset(&frame, 0, sizeof(frame));
    frame.message_type = STREAM_ENDPOINT;
    frame.wire_profile = UCN_WIRE_PROFILE_W0_LOCAL;
    frame.traffic_class = UCN_TRAFFIC_Q1_REALTIME;
    frame.hop_limit = 1U;
    frame.network_id = STREAM_NETWORK_ID;
    frame.source = STREAM_REMOTE_NODE;
    frame.destination = STREAM_LOCAL_NODE;
    frame.sequence = sequence;
    frame.session_id = 1U;
    frame.payload = payload;
    frame.payload_length = (uint16_t)sizeof(payload);
    result = ucn_frame_encode(&frame, encoded, sizeof(encoded),
                              &encoded_length);
    if (result != UCN_OK) {
        return result;
    }
    return ucn_stream_carrier_encode(encoded, encoded_length, wire,
                                     wire_capacity, wire_length);
}

static int stream_drain(stream_fixture_t *fixture, size_t limit)
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

static int test_stream_carrier_encode_contract(void)
{
    uint8_t frame[UCN_MAX_FRAME_BYTES];
    uint8_t wire[UCN_STREAM_CARRIER_MAX_WIRE_BYTES(UCN_MAX_FRAME_BYTES)];
    size_t wire_length = 99U;
    size_t exact_length;

    (void)memset(frame, 0x5AU, sizeof(frame));
    TEST_ASSERT(ucn_stream_carrier_encode(NULL, sizeof(frame), wire,
                                          sizeof(wire), &wire_length) ==
                UCN_ERR_ARGUMENT);
    TEST_ASSERT(ucn_stream_carrier_encode(frame, UCN_FRAME_W0_HEADER_SIZE - 1U,
                                          wire, sizeof(wire), &wire_length) ==
                UCN_ERR_MALFORMED);
    TEST_ASSERT(wire_length == 0U);
    TEST_ASSERT(ucn_stream_carrier_encode(frame, sizeof(frame), wire,
                                          sizeof(wire), &wire_length) ==
                UCN_OK);
    TEST_ASSERT(wire_length >= sizeof(frame) + 2U &&
                wire[wire_length - 1U] == 0U);
    exact_length = wire_length;
    TEST_ASSERT(ucn_stream_carrier_encode(frame, sizeof(frame), wire,
                                          exact_length, &wire_length) ==
                UCN_OK);
    TEST_ASSERT(wire_length == exact_length);
    TEST_ASSERT(ucn_stream_carrier_encode(frame, sizeof(frame), wire,
                                          exact_length - 1U, &wire_length) ==
                UCN_ERR_TOO_LARGE);
    (void)memset(frame, 0, sizeof(frame));
    TEST_ASSERT(ucn_stream_carrier_encode(frame, sizeof(frame), wire,
                                          sizeof(wire), &wire_length) ==
                UCN_OK);
    TEST_ASSERT(wire_length == sizeof(frame) + 2U &&
                wire[wire_length - 1U] == 0U);
    return 0;
}

static int test_stream_split_wrap_isr_and_reset(void)
{
    stream_fixture_t fixture;
    uint8_t ring[40U];
    uint8_t wire[UCN_STREAM_CARRIER_MAX_WIRE_BYTES(UCN_MAX_FRAME_BYTES)];
    size_t wire_length;
    ucn_event_runtime_run_result_t fallback;
    const ucn_stream_source_stats_t *stats;

    TEST_ASSERT(stream_fixture_init_with(
                    &fixture, &STREAM_PORT_OPS, 8U, 4U, ring, sizeof(ring),
                    0U, 0U, 0U) == UCN_OK);
    TEST_ASSERT(stream_make_carrier(1U, 0x11U, wire, sizeof(wire),
                                    &wire_length) == UCN_OK);
    TEST_ASSERT(wire_length < sizeof(ring));
    TEST_ASSERT(ucn_stream_source_write(&fixture.source, wire, 1U) == UCN_OK);
    TEST_ASSERT(ucn_stream_source_write_from_isr(
                    &fixture.source, &wire[1], wire_length - 1U) == UCN_OK);
    TEST_ASSERT(stream_drain(&fixture, 4U) == 0);
    TEST_ASSERT(fixture.received.count == 1U &&
                fixture.received.last_value == 0x11U);

    /* The second carrier starts near the physical end of the small Ring and
     * therefore proves wrap-around without exposing Ring internals. */
    TEST_ASSERT(stream_make_carrier(2U, 0x22U, wire, sizeof(wire),
                                    &wire_length) == UCN_OK);
    TEST_ASSERT(ucn_stream_source_write(&fixture.source, wire, wire_length) ==
                UCN_OK);
    TEST_ASSERT(stream_drain(&fixture, 4U) == 0);
    TEST_ASSERT(fixture.received.count == 2U &&
                fixture.received.last_value == 0x22U);

    TEST_ASSERT(ucn_stream_source_write(&fixture.source, wire, 3U) == UCN_OK);
    TEST_ASSERT(ucn_stream_source_pending_bytes(&fixture.source) == 3U);
    TEST_ASSERT(ucn_stream_source_reset(&fixture.source) == UCN_OK);
    TEST_ASSERT(ucn_stream_source_pending_bytes(&fixture.source) == 0U &&
                ucn_stream_source_free_bytes(&fixture.source) == sizeof(ring));
    TEST_ASSERT(ucn_event_runtime_run(&fixture.runtime, true, &fallback) ==
                UCN_OK);
    stats = ucn_stream_source_get_stats(&fixture.source);
    TEST_ASSERT(stats != NULL && stats->task_writes == 3U &&
                stats->isr_writes == 1U && stats->frames_submitted == 2U &&
                stats->fallback_services >= 1U);
    TEST_ASSERT(fixture.port.task_enters == fixture.port.task_exits &&
                fixture.port.isr_enters == fixture.port.isr_exits &&
                fixture.port.last_exit_token != 0U);
    return 0;
}

static int test_stream_backpressure_and_multi_source(void)
{
    stream_fixture_t fixture;
    ucn_stream_source_t second_source;
    ucn_stream_source_default_storage_t second_storage;
    ucn_stream_source_config_t second_config;
    uint8_t wire[UCN_STREAM_CARRIER_MAX_WIRE_BYTES(UCN_MAX_FRAME_BYTES)];
    uint8_t carriers[(UCN_ADAPTER_RX_QUEUE_DEPTH + 3U) *
                     UCN_STREAM_CARRIER_MAX_WIRE_BYTES(UCN_MAX_FRAME_BYTES)];
    uint8_t ring[sizeof(carriers)];
    size_t carrier_length;
    size_t total = 0U;
    size_t index;
    const size_t frame_count = UCN_ADAPTER_RX_QUEUE_DEPTH + 3U;
    const ucn_stream_source_stats_t *stats;

    TEST_ASSERT(stream_fixture_init_with(
                    &fixture, &STREAM_PORT_OPS, 1U, frame_count, ring,
                    sizeof(ring), 0U, 0U, 0U) == UCN_OK);
    for (index = 0U; index < frame_count; ++index) {
        TEST_ASSERT(stream_make_carrier((uint32_t)(index + 1U),
                                        (uint8_t)(index + 1U), wire,
                                        sizeof(wire), &carrier_length) ==
                    UCN_OK);
        TEST_ASSERT(total + carrier_length <= sizeof(carriers));
        (void)memcpy(&carriers[total], wire, carrier_length);
        total += carrier_length;
    }
    TEST_ASSERT(ucn_stream_source_write(&fixture.source, carriers, total) ==
                UCN_OK);
    TEST_ASSERT(stream_drain(&fixture, frame_count + 4U) == 0);
    TEST_ASSERT(fixture.received.count == frame_count);
    stats = ucn_stream_source_get_stats(&fixture.source);
    TEST_ASSERT(stats->frames_decoded == frame_count &&
                stats->frames_submitted == frame_count &&
                stats->adapter_queue_backpressure >= 1U);

    (void)memset(&second_source, 0, sizeof(second_source));
    (void)memset(&second_storage, 0, sizeof(second_storage));
    (void)memset(&second_config, 0, sizeof(second_config));
    second_config.runtime = &fixture.runtime;
    second_config.source_id = 1U;
    second_config.ingress_link = &fixture.link;
    second_config.ring_storage = second_storage.ring;
    second_config.ring_capacity = sizeof(second_storage.ring);
    second_config.frame_storage = second_storage.frame;
    second_config.frame_capacity = sizeof(second_storage.frame);
    TEST_ASSERT(ucn_stream_source_init(&second_source, &second_config) ==
                UCN_OK);
    TEST_ASSERT(stream_make_carrier((uint32_t)(frame_count + 1U), 0x77U,
                                    wire, sizeof(wire), &carrier_length) ==
                UCN_OK);
    TEST_ASSERT(ucn_stream_source_write_from_isr(
                    &second_source, wire, carrier_length) == UCN_OK);
    TEST_ASSERT(stream_drain(&fixture, 4U) == 0);
    TEST_ASSERT(fixture.received.count == frame_count + 1U &&
                fixture.received.last_value == 0x77U);
    return 0;
}

static int test_stream_error_recovery_and_gap_order(void)
{
    static const uint8_t malformed_cobs[] = { 5U, 0x11U, 0U };
    static const uint8_t short_frame[] = { 6U, 1U, 2U, 3U, 4U, 5U, 0U };
    static const uint8_t empty[] = { 0U };
    static const uint8_t after_gap[] = { 0x11U, 0x22U, 0U };
    stream_fixture_t fixture;
    uint8_t ring[64U];
    uint8_t wire[UCN_STREAM_CARRIER_MAX_WIRE_BYTES(UCN_MAX_FRAME_BYTES)];
    uint8_t input[UCN_STREAM_SOURCE_DEFAULT_RING_BYTES];
    uint8_t rejected[65U];
    size_t wire_length;
    size_t total = 0U;
    const ucn_stream_source_stats_t *stats;

    TEST_ASSERT(stream_fixture_init(&fixture, 8U, 8U) == UCN_OK);
    TEST_ASSERT(stream_make_carrier(1U, 0x31U, wire, sizeof(wire),
                                    &wire_length) == UCN_OK);
    (void)memcpy(&input[total], malformed_cobs, sizeof(malformed_cobs));
    total += sizeof(malformed_cobs);
    (void)memcpy(&input[total], short_frame, sizeof(short_frame));
    total += sizeof(short_frame);
    (void)memcpy(&input[total], empty, sizeof(empty));
    total += sizeof(empty);
    (void)memcpy(&input[total], wire, wire_length);
    total += wire_length;
    TEST_ASSERT(ucn_stream_source_write(&fixture.source, input, total) ==
                UCN_OK);
    TEST_ASSERT(stream_drain(&fixture, 4U) == 0);
    TEST_ASSERT(fixture.received.count == 1U);
    stats = ucn_stream_source_get_stats(&fixture.source);
    TEST_ASSERT(stats->cobs_decode_errors == 1U &&
                stats->frame_length_errors == 1U &&
                stats->empty_delimiters == 1U);

    /* A rejected chunk occurs after one valid carrier already in the Ring.
     * That carrier must be delivered; recovery starts only at the real gap. */
    TEST_ASSERT(stream_fixture_init_with(
                    &fixture, &STREAM_PORT_OPS, 8U, 8U, ring, sizeof(ring),
                    0U, 0U, 1U) == UCN_OK);
    TEST_ASSERT(stream_make_carrier(1U, 0x41U, wire, sizeof(wire),
                                    &wire_length) == UCN_OK);
    TEST_ASSERT(ucn_stream_source_write(&fixture.source, wire, wire_length) ==
                UCN_OK);
    (void)memset(rejected, 0x7EU, sizeof(rejected));
    TEST_ASSERT(ucn_stream_source_write(&fixture.source, rejected,
                                        sizeof(rejected)) == UCN_ERR_NO_SPACE);
    TEST_ASSERT(ucn_stream_source_write(&fixture.source, after_gap,
                                        sizeof(after_gap)) == UCN_OK);
    TEST_ASSERT(stream_drain(&fixture, 6U) == 0);
    TEST_ASSERT(fixture.received.count == 1U &&
                fixture.received.last_value == 0x41U);
    stats = ucn_stream_source_get_stats(&fixture.source);
    TEST_ASSERT(stats->ring_no_space == 1U && stats->ring_gaps == 1U &&
                stats->resynchronizations == 1U &&
                stats->error_budget_hits >= 1U);

    /* A byte budget smaller than one Carrier must resume the same candidate
     * over bounded Runtime rounds rather than wait for another interrupt. */
    TEST_ASSERT(stream_fixture_init_with(
                    &fixture, &STREAM_PORT_OPS, 2U, 2U, NULL, 0U, 0U, 2U,
                    0U) == UCN_OK);
    TEST_ASSERT(stream_make_carrier(1U, 0x42U, wire, sizeof(wire),
                                    &wire_length) == UCN_OK);
    TEST_ASSERT(ucn_stream_source_write(&fixture.source, wire, wire_length) ==
                UCN_OK);
    TEST_ASSERT(stream_drain(&fixture, wire_length) == 0);
    TEST_ASSERT(fixture.received.count == 1U &&
                fixture.received.last_value == 0x42U);
    return 0;
}

static int test_stream_overflow_budget_and_isr_gate(void)
{
    stream_fixture_t fixture;
    uint8_t input[2U *
                  UCN_STREAM_CARRIER_MAX_WIRE_BYTES(UCN_MAX_FRAME_BYTES)];
    uint8_t ring[sizeof(input)];
    uint8_t wire[UCN_STREAM_CARRIER_MAX_WIRE_BYTES(UCN_MAX_FRAME_BYTES)];
    size_t wire_length;
    size_t overflow_length =
        UCN_STREAM_COBS_MAX_ENCODED_BYTES(UCN_MAX_FRAME_BYTES) + 1U;
    const ucn_stream_source_stats_t *stats;

    TEST_ASSERT(stream_fixture_init_with(
                    &fixture, &STREAM_PORT_OPS, 8U, 8U, ring, sizeof(ring), 0U,
                    0U, 1U) == UCN_OK);
    TEST_ASSERT(overflow_length + 1U < sizeof(input));
    (void)memset(input, 0x33U, overflow_length);
    input[overflow_length] = 0U;
    TEST_ASSERT(stream_make_carrier(1U, 0x51U, wire, sizeof(wire),
                                    &wire_length) == UCN_OK);
    (void)memcpy(&input[overflow_length + 1U], wire, wire_length);
    TEST_ASSERT(ucn_stream_source_write(
                    &fixture.source, input,
                    overflow_length + 1U + wire_length) == UCN_OK);
    TEST_ASSERT(stream_drain(&fixture, 8U) == 0);
    TEST_ASSERT(fixture.received.count == 1U);
    stats = ucn_stream_source_get_stats(&fixture.source);
    TEST_ASSERT(stats->candidate_overflows == 1U &&
                stats->resynchronizations == 1U &&
                stats->error_budget_hits >= 1U);

    TEST_ASSERT(stream_fixture_init_with(
                    &fixture, &STREAM_PORT_OPS_NO_ISR, 2U, 1U, NULL, 0U, 0U,
                    0U, 0U) == UCN_OK);
    TEST_ASSERT(ucn_stream_source_write_from_isr(
                    &fixture.source, wire, wire_length) == UCN_ERR_CONFIG);
    TEST_ASSERT(ucn_stream_source_write(NULL, wire, wire_length) ==
                UCN_ERR_ARGUMENT);
    return 0;
}

int test_stream_source(void)
{
    ucn_stream_source_t source;
    ucn_stream_source_config_t config;

    (void)memset(&source, 0, sizeof(source));
    (void)memset(&config, 0, sizeof(config));
    TEST_ASSERT(ucn_stream_source_init(NULL, &config) == UCN_ERR_ARGUMENT);
    TEST_ASSERT(ucn_stream_source_reset(&source) == UCN_ERR_ARGUMENT);
    TEST_ASSERT(ucn_stream_source_get_stats(&source) == NULL);
    TEST_ASSERT(test_stream_carrier_encode_contract() == 0);
    TEST_ASSERT(test_stream_split_wrap_isr_and_reset() == 0);
    TEST_ASSERT(test_stream_backpressure_and_multi_source() == 0);
    TEST_ASSERT(test_stream_error_recovery_and_gap_order() == 0);
    TEST_ASSERT(test_stream_overflow_budget_and_isr_gate() == 0);
    return 0;
}
