#include <string.h>

#include "test_support.h"
#include "ucn/ucn_frame.h"
#include "ucn/ucn_node.h"

typedef struct duplicate_link_context {
    bool is_up;
} duplicate_link_context_t;

typedef struct duplicate_receive_state {
    uint32_t delivered;
    ucn_sequence_t last_sequence;
    ucn_session_id_t last_session;
} duplicate_receive_state_t;

static ucn_result_t duplicate_link_send(ucn_link_t *link,
                                        const uint8_t *frame,
                                        size_t length)
{
    (void)link;
    (void)frame;
    (void)length;
    return UCN_OK;
}

static ucn_result_t duplicate_link_status(const ucn_link_t *link,
                                          ucn_link_status_t *status)
{
    const duplicate_link_context_t *context =
        (const duplicate_link_context_t *)link->context;

    status->is_up = context->is_up;
    status->mtu = link->mtu;
    status->tx_errors = 0U;
    status->rx_errors = 0U;
    return UCN_OK;
}

static const ucn_link_ops_t DUPLICATE_LINK_OPS = {
    NULL, duplicate_link_send, NULL, duplicate_link_status, NULL, NULL
};

static void duplicate_receive(void *context, const ucn_frame_t *frame)
{
    duplicate_receive_state_t *state = (duplicate_receive_state_t *)context;

    state->delivered++;
    state->last_sequence = frame->sequence;
    state->last_session = frame->session_id;
}

static ucn_result_t duplicate_inject(ucn_node_t *node,
                                     ucn_link_t *ingress,
                                     ucn_node_id_t source,
                                     ucn_session_id_t session_id,
                                     ucn_sequence_t sequence)
{
    uint8_t encoded[UCN_MAX_FRAME_BYTES];
    uint8_t payload = (uint8_t)sequence;
    ucn_frame_t frame;
    size_t encoded_length = 0U;
    ucn_result_t result;

    (void)memset(&frame, 0, sizeof(frame));
    frame.message_type = (uint8_t)0x40U;
    frame.traffic_class = UCN_TRAFFIC_Q1_REALTIME;
    frame.hop_limit = 4U;
    frame.network_id = UINT32_C(0x44555045);
    frame.source = source;
    frame.destination = UINT32_C(1);
    frame.sequence = sequence;
    frame.session_id = session_id;
    frame.payload = &payload;
    frame.payload_length = 1U;
    result = ucn_frame_encode(&frame, encoded, sizeof(encoded), &encoded_length);
    if (result != UCN_OK) {
        return result;
    }
    return ucn_node_receive(node, ingress, encoded, encoded_length);
}

int test_duplicate_window(void)
{
    ucn_config_t config;
    ucn_node_t node;
    ucn_link_t ingress;
    duplicate_link_context_t link_context;
    duplicate_receive_state_t received;
    ucn_node_id_t source;
    size_t index;
    size_t used_windows = 0U;

    (void)memset(&config, 0, sizeof(config));
    (void)memset(&node, 0, sizeof(node));
    (void)memset(&ingress, 0, sizeof(ingress));
    (void)memset(&link_context, 0, sizeof(link_context));
    (void)memset(&received, 0, sizeof(received));
    config.network_id = UINT32_C(0x44555045);
    config.node_id = UINT32_C(1);
    config.default_hop_limit = 4U;
    TEST_ASSERT(ucn_node_init(&node, &config) == UCN_OK);
    TEST_ASSERT(ucn_node_set_plain_session_id(&node, 0U) == UCN_ERR_ARGUMENT);
    TEST_ASSERT(ucn_node_set_plain_session_id(&node, UINT32_C(0xB007)) == UCN_OK);
    TEST_ASSERT(node.session_id == UINT32_C(0xB007));

    link_context.is_up = true;
    ingress.ops = &DUPLICATE_LINK_OPS;
    ingress.context = &link_context;
    ingress.link_id = 1U;
    ingress.mtu = UCN_MAX_FRAME_BYTES;
    ingress.peer_node_id = UINT32_C(2);
    TEST_ASSERT(ucn_node_register_link(&node, &ingress) == UCN_OK);
    ucn_node_set_rx_handler(&node, duplicate_receive, &received);

    /* Session zero remains wire-compatible, but a plain peer that reboots
     * and reuses the same Sequence is deliberately indistinguishable from a
     * delayed duplicate until this source window expires. */
    TEST_ASSERT(duplicate_inject(&node, &ingress, UINT32_C(2), 0U,
                                 UINT32_C(1)) == UCN_OK);
    used_windows++;
    TEST_ASSERT(duplicate_inject(&node, &ingress, UINT32_C(2), 0U,
                                 UINT32_C(1)) == UCN_ERR_REPLAY);

    TEST_ASSERT(duplicate_inject(&node, &ingress, UINT32_C(2), UINT32_C(11),
                                 UINT32_C(100)) == UCN_OK);
    used_windows++;
    TEST_ASSERT(duplicate_inject(&node, &ingress, UINT32_C(2), UINT32_C(11),
                                 UINT32_C(100)) == UCN_ERR_REPLAY);
    TEST_ASSERT(duplicate_inject(&node, &ingress, UINT32_C(2), UINT32_C(11),
                                 UINT32_C(98)) == UCN_OK);
    TEST_ASSERT(duplicate_inject(&node, &ingress, UINT32_C(2), UINT32_C(11),
                                 UINT32_C(98)) == UCN_ERR_REPLAY);
    TEST_ASSERT(duplicate_inject(&node, &ingress, UINT32_C(2), UINT32_C(11),
                                 UINT32_C(100) - UCN_DUPLICATE_WINDOW_BITS) ==
                UCN_ERR_REPLAY);
    TEST_ASSERT(duplicate_inject(&node, &ingress, UINT32_C(2), UINT32_C(11),
                                 UINT32_C(105)) == UCN_OK);
    TEST_ASSERT(duplicate_inject(&node, &ingress, UINT32_C(2), UINT32_C(11),
                                 UINT32_C(104)) == UCN_OK);

    /* A new authenticated/plain boot Session is a separate sequence space. */
    TEST_ASSERT(duplicate_inject(&node, &ingress, UINT32_C(2), UINT32_C(12),
                                 UINT32_C(1)) == UCN_OK);
    used_windows++;

    /* Serial arithmetic accepts the legal wrap without clearing the window. */
    TEST_ASSERT(duplicate_inject(&node, &ingress, UINT32_C(3), UINT32_C(13),
                                 UINT32_MAX - 2U) == UCN_OK);
    used_windows++;
    TEST_ASSERT(duplicate_inject(&node, &ingress, UINT32_C(3), UINT32_C(13),
                                 UINT32_C(1)) == UCN_OK);
    TEST_ASSERT(duplicate_inject(&node, &ingress, UINT32_C(3), UINT32_C(13),
                                 UINT32_MAX - 2U) == UCN_ERR_REPLAY);

    source = UINT32_C(4);
    while (used_windows < UCN_DUPLICATE_SOURCE_WINDOWS) {
        TEST_ASSERT(duplicate_inject(&node, &ingress, source, UINT32_C(20),
                                     UINT32_C(1)) == UCN_OK);
        source++;
        used_windows++;
    }
    TEST_ASSERT(duplicate_inject(&node, &ingress, source, UINT32_C(20),
                                 UINT32_C(1)) == UCN_ERR_NO_SPACE);
    TEST_ASSERT(node.stats.duplicate_source_window_full == 1U);

    /* Replays must not keep a fixed Source/Session slot alive forever.  A
     * replay storm immediately before expiry still leaves every old slot
     * reclaimable at the original accepted-frame deadline. */
    node.now_ms = UCN_DUPLICATE_SOURCE_TIMEOUT_MS - 1U;
    for (index = 0U; index < UCN_DUPLICATE_SOURCE_WINDOWS; ++index) {
        TEST_ASSERT(duplicate_inject(
                        &node, &ingress,
                        node.duplicate_windows[index].source,
                        node.duplicate_windows[index].session_id,
                        node.duplicate_windows[index].highest_sequence) ==
                    UCN_ERR_REPLAY);
    }
    node.now_ms = UCN_DUPLICATE_SOURCE_TIMEOUT_MS;
    TEST_ASSERT(duplicate_inject(&node, &ingress, source, UINT32_C(20),
                                 UINT32_C(1)) == UCN_OK);
    TEST_ASSERT(node.stats.duplicate_frames_dropped >= 4U);
    TEST_ASSERT(received.delivered ==
                (uint32_t)(UCN_DUPLICATE_SOURCE_WINDOWS + 5U));
    return 0;
}
