#include <string.h>

#include "test_support.h"
#include "ucn/ucn_adapter.h"
#include "ucn/ucn_frame.h"

typedef struct adapter_link_context {
    bool is_up;
} adapter_link_context_t;

typedef struct adapter_receive_state {
    uint32_t count;
    ucn_node_id_t source;
} adapter_receive_state_t;

typedef struct adapter_lock_state {
    uint32_t enter_count;
    uint32_t exit_count;
    uint32_t isr_enter_count;
    uint32_t isr_exit_count;
    ucn_port_critical_token_t next_isr_token;
    ucn_port_critical_token_t last_isr_enter_token;
    ucn_port_critical_token_t last_isr_exit_token;
} adapter_lock_state_t;

static ucn_result_t adapter_link_send(ucn_link_t *link,
                                      const uint8_t *frame,
                                      size_t length)
{
    (void)link;
    (void)frame;
    (void)length;
    return UCN_OK;
}

static ucn_result_t adapter_link_status(const ucn_link_t *link,
                                        ucn_link_status_t *status)
{
    const adapter_link_context_t *context = (const adapter_link_context_t *)link->context;

    status->is_up = context->is_up;
    status->mtu = link->mtu;
    status->tx_errors = 0U;
    status->rx_errors = 0U;
    return UCN_OK;
}

static const ucn_link_ops_t ADAPTER_LINK_OPS = {
    NULL, adapter_link_send, NULL, adapter_link_status, NULL, NULL
};

static void adapter_enter_critical(void *context)
{
    adapter_lock_state_t *state = (adapter_lock_state_t *)context;
    state->enter_count++;
}

static void adapter_exit_critical(void *context)
{
    adapter_lock_state_t *state = (adapter_lock_state_t *)context;
    state->exit_count++;
}

static ucn_port_critical_token_t adapter_enter_critical_from_isr(void *context)
{
    adapter_lock_state_t *state = (adapter_lock_state_t *)context;

    state->isr_enter_count++;
    state->next_isr_token++;
    state->last_isr_enter_token = state->next_isr_token;
    return state->last_isr_enter_token;
}

static void adapter_exit_critical_from_isr(
    void *context,
    ucn_port_critical_token_t token)
{
    adapter_lock_state_t *state = (adapter_lock_state_t *)context;

    state->isr_exit_count++;
    state->last_isr_exit_token = token;
}

static const ucn_port_ops_t ADAPTER_PORT_OPS = {
    NULL, NULL, NULL, NULL, adapter_enter_critical, adapter_exit_critical,
    adapter_enter_critical_from_isr, adapter_exit_critical_from_isr
};

static void adapter_receive_callback(void *context, const ucn_frame_t *frame)
{
    adapter_receive_state_t *state = (adapter_receive_state_t *)context;

    state->count++;
    state->source = frame->source;
}

static ucn_result_t adapter_encode_frame(uint8_t message_type,
                                         ucn_network_id_t network_id,
                                         ucn_node_id_t source,
                                         ucn_node_id_t destination,
                                         ucn_wire_profile_t hello_rx_profile,
                                         ucn_sequence_t sequence,
                                         uint8_t *encoded,
                                         size_t *encoded_length)
{
    uint8_t hello_payload;
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
        hello_payload = hello_rx_profile;
        frame.payload = &hello_payload;
        frame.payload_length = 1U;
    }
    return ucn_frame_encode(&frame, encoded, UCN_MAX_FRAME_BYTES, encoded_length);
}

int test_adapter(void)
{
    static const ucn_adapter_address_t ADDRESS_A = {
        6U, { 0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U, 0U, 0U }
    };
    static const ucn_adapter_address_t ADDRESS_B = {
        6U, { 0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x02U, 0U, 0U }
    };
    ucn_node_t node;
    ucn_config_t config;
    ucn_link_t candidate_link, conflicting_link;
    adapter_link_context_t link_context, conflicting_context;
    ucn_adapter_peer_binding_t bindings[2];
    ucn_adapter_rx_queue_t queue, overflow_queue;
    ucn_port_ops_t invalid_port_ops;
    adapter_receive_state_t receive_state;
    adapter_lock_state_t lock_state;
    uint8_t encoded[UCN_MAX_FRAME_BYTES];
    size_t encoded_length = 0U;
    size_t pumped = 0U;
    size_t index;

    (void)memset(&node, 0, sizeof(node));
    (void)memset(&candidate_link, 0, sizeof(candidate_link));
    (void)memset(&conflicting_link, 0, sizeof(conflicting_link));
    (void)memset(&link_context, 0, sizeof(link_context));
    (void)memset(&conflicting_context, 0, sizeof(conflicting_context));
    (void)memset(bindings, 0, sizeof(bindings));
    (void)memset(&queue, 0, sizeof(queue));
    (void)memset(&overflow_queue, 0, sizeof(overflow_queue));
    (void)memset(&receive_state, 0, sizeof(receive_state));
    (void)memset(&lock_state, 0, sizeof(lock_state));
    config.network_id = UINT32_C(0x0A0B0C0D);
    config.node_id = UINT32_C(9);
    config.default_hop_limit = 3U;
    candidate_link.ops = &ADAPTER_LINK_OPS;
    candidate_link.context = &link_context;
    candidate_link.link_id = 1U;
    candidate_link.mtu = UCN_MAX_FRAME_BYTES;
    conflicting_link.ops = &ADAPTER_LINK_OPS;
    conflicting_link.context = &conflicting_context;
    conflicting_link.link_id = 2U;
    conflicting_link.mtu = UCN_MAX_FRAME_BYTES;
    link_context.is_up = true;
    conflicting_context.is_up = true;

    TEST_ASSERT(ucn_node_init(&node, &config) == UCN_OK);
#if UCN_FEATURE_DYNAMIC_MESH
    TEST_ASSERT(ucn_node_set_join_policy(&node, UCN_JOIN_OPEN, NULL, NULL) == UCN_OK);
#endif
    TEST_ASSERT(ucn_adapter_address_is_valid(&ADDRESS_A));
    TEST_ASSERT(ucn_adapter_address_equal(&ADDRESS_A, &ADDRESS_A));
    TEST_ASSERT(!ucn_adapter_address_equal(&ADDRESS_A, &ADDRESS_B));
    TEST_ASSERT(ucn_adapter_bind_peer(bindings, 2U, &ADDRESS_A,
                                      &candidate_link) == UCN_OK);
    TEST_ASSERT(ucn_adapter_find_peer(bindings, 2U, &ADDRESS_A) == &bindings[0]);
    TEST_ASSERT(ucn_adapter_bind_peer(bindings, 2U, &ADDRESS_A,
                                      &candidate_link) == UCN_OK);
    TEST_ASSERT(ucn_adapter_bind_peer(bindings, 2U, &ADDRESS_A,
                                      &conflicting_link) == UCN_ERR_CONFIG);
    TEST_ASSERT(ucn_adapter_bind_peer(bindings, 2U, &ADDRESS_B,
                                      &conflicting_link) == UCN_OK);

    TEST_ASSERT(ucn_adapter_rx_queue_init(&queue, &ADAPTER_PORT_OPS,
                                           &lock_state) == UCN_OK);
    invalid_port_ops = ADAPTER_PORT_OPS;
    invalid_port_ops.enter_critical_from_isr = NULL;
    TEST_ASSERT(ucn_adapter_rx_queue_init(&overflow_queue, &invalid_port_ops,
                                          &lock_state) == UCN_ERR_ARGUMENT);
#if UCN_FEATURE_DYNAMIC_MESH
    TEST_ASSERT(adapter_encode_frame(UCN_MSG_DATA_Q1, config.network_id,
                                     UINT32_C(1), config.node_id, 0U, 1U,
                                     encoded, &encoded_length) == UCN_OK);
    TEST_ASSERT(ucn_adapter_rx_enqueue_from_isr(&queue, &candidate_link, encoded,
                                                encoded_length) == UCN_OK);
    TEST_ASSERT(ucn_adapter_rx_pump(&queue, &node, 1U, &pumped) == UCN_OK);
    TEST_ASSERT(pumped == 1U);
    TEST_ASSERT(ucn_adapter_rx_get_stats(&queue)->rejected_by_core == 1U);

    TEST_ASSERT(adapter_encode_frame(UCN_MSG_HELLO, config.network_id,
                                     UINT32_C(1), config.node_id,
                                     UCN_WIRE_PROFILE_W3_BACKBONE,
                                     2U, encoded, &encoded_length) == UCN_OK);
    TEST_ASSERT(ucn_adapter_rx_enqueue(&queue, &candidate_link, encoded,
                                       encoded_length) == UCN_OK);
    TEST_ASSERT(ucn_adapter_rx_pump(&queue, &node, 1U, &pumped) == UCN_OK);
    TEST_ASSERT(pumped == 1U);
    TEST_ASSERT(candidate_link.peer_node_id == UINT32_C(1));
    TEST_ASSERT(node.link_count == 1U);
    TEST_ASSERT(ucn_node_neighbor_count(&node, UCN_NEIGHBOR_ADMITTED) == 1U);

    ucn_node_set_rx_handler(&node, adapter_receive_callback, &receive_state);
    TEST_ASSERT(adapter_encode_frame(UCN_MSG_DATA_Q1, config.network_id,
                                     UINT32_C(1), config.node_id, 0U, 3U,
                                     encoded, &encoded_length) == UCN_OK);
    TEST_ASSERT(ucn_adapter_rx_enqueue(&queue, &candidate_link, encoded,
                                       encoded_length) == UCN_OK);
    TEST_ASSERT(ucn_adapter_rx_pump(&queue, &node, 1U, &pumped) == UCN_OK);
    TEST_ASSERT(receive_state.count == 1U);
    TEST_ASSERT(receive_state.source == UINT32_C(1));
    TEST_ASSERT(lock_state.enter_count == lock_state.exit_count);
    TEST_ASSERT(lock_state.isr_enter_count == lock_state.isr_exit_count);
    TEST_ASSERT(lock_state.isr_enter_count == 1U);
    TEST_ASSERT(lock_state.last_isr_enter_token == lock_state.last_isr_exit_token);
#else
    candidate_link.peer_node_id = UINT32_C(1);
    TEST_ASSERT(ucn_node_register_link(&node, &candidate_link) == UCN_OK);
    ucn_node_set_rx_handler(&node, adapter_receive_callback, &receive_state);
    TEST_ASSERT(adapter_encode_frame(UCN_MSG_DATA_Q1, config.network_id,
                                     UINT32_C(1), config.node_id, 0U, 1U,
                                     encoded, &encoded_length) == UCN_OK);
    TEST_ASSERT(ucn_adapter_rx_enqueue(&queue, &candidate_link, encoded,
                                       encoded_length) == UCN_OK);
    TEST_ASSERT(ucn_adapter_rx_pump(&queue, &node, 1U, &pumped) == UCN_OK);
    TEST_ASSERT(pumped == 1U && receive_state.count == 1U);
    TEST_ASSERT(receive_state.source == UINT32_C(1));
    TEST_ASSERT(lock_state.enter_count == lock_state.exit_count);
    TEST_ASSERT(lock_state.isr_enter_count == 0U);
#endif

    TEST_ASSERT(ucn_adapter_rx_queue_init(&overflow_queue, NULL, NULL) == UCN_OK);
    TEST_ASSERT(ucn_adapter_rx_enqueue_from_isr(&overflow_queue, &candidate_link,
                                                encoded, encoded_length) ==
                UCN_ERR_CONFIG);
    for (index = 0U; index < UCN_ADAPTER_RX_QUEUE_DEPTH; ++index) {
        TEST_ASSERT(ucn_adapter_rx_enqueue(&overflow_queue, &candidate_link, encoded,
                                           encoded_length) == UCN_OK);
    }
    TEST_ASSERT(ucn_adapter_rx_enqueue(&overflow_queue, &candidate_link, encoded,
                                       encoded_length) == UCN_ERR_NO_SPACE);
    TEST_ASSERT(ucn_adapter_rx_get_stats(&overflow_queue)->dropped_full == 1U);
    return 0;
}
