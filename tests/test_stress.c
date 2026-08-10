#include <string.h>

#include "test_support.h"
#include "ucn/ucn_node.h"

#define STRESS_NODE_COUNT ((size_t)32U)
#define STRESS_LINKS_PER_NODE ((size_t)2U)
#define STRESS_ROUTE_SPAN ((size_t)UCN_MAX_ROUTES)
#define STRESS_ITERATIONS UINT32_C(5000)

typedef struct stress_link_context {
    ucn_node_t *peer;
    ucn_link_t *peer_ingress;
    bool *edge_up;
} stress_link_context_t;

typedef struct stress_rx_state {
    uint32_t delivered;
} stress_rx_state_t;

static ucn_result_t stress_link_send(ucn_link_t *link,
                                     const uint8_t *frame,
                                     size_t length)
{
    stress_link_context_t *context = (stress_link_context_t *)link->context;

    if (!*context->edge_up) {
        return UCN_ERR_LINK_DOWN;
    }
    return ucn_node_receive(context->peer, context->peer_ingress, frame, length);
}

static ucn_result_t stress_link_status(const ucn_link_t *link,
                                       ucn_link_status_t *status)
{
    const stress_link_context_t *context =
        (const stress_link_context_t *)link->context;

    status->is_up = *context->edge_up;
    status->mtu = link->mtu;
    status->tx_errors = 0U;
    status->rx_errors = 0U;
    return UCN_OK;
}

static const ucn_link_ops_t STRESS_LINK_OPS = {
    NULL, stress_link_send, NULL, stress_link_status, NULL, NULL
};

static void stress_receive(void *context, const ucn_frame_t *frame)
{
    stress_rx_state_t *state = (stress_rx_state_t *)context;

    if (frame->message_type == UCN_MSG_DATA_Q1) {
        state->delivered++;
    }
}

static uint32_t stress_random(uint32_t *state)
{
    uint32_t value = *state;

    value ^= value << 13U;
    value ^= value >> 17U;
    value ^= value << 5U;
    *state = value;
    return value;
}

int test_stress(void)
{
    static ucn_node_t nodes[STRESS_NODE_COUNT];
    static ucn_link_t links[STRESS_NODE_COUNT][STRESS_LINKS_PER_NODE];
    static stress_link_context_t
        contexts[STRESS_NODE_COUNT][STRESS_LINKS_PER_NODE];
    static stress_rx_state_t receive_states[STRESS_NODE_COUNT];
    bool edge_up[STRESS_NODE_COUNT];
    uint32_t seed = UINT32_C(0xC0DEF00D);
    uint32_t successful = 0U;
    uint32_t iteration;
    size_t index;

    (void)memset(nodes, 0, sizeof(nodes));
    (void)memset(links, 0, sizeof(links));
    (void)memset(contexts, 0, sizeof(contexts));
    (void)memset(receive_states, 0, sizeof(receive_states));
    for (index = 0U; index < STRESS_NODE_COUNT; ++index) {
        ucn_config_t config;
        size_t clockwise = (index + 1U) % STRESS_NODE_COUNT;
        size_t counterclockwise =
            (index + STRESS_NODE_COUNT - 1U) % STRESS_NODE_COUNT;

        edge_up[index] = true;
        config.network_id = UINT32_C(0x53545253);
        config.node_id = (ucn_node_id_t)(index + 1U);
        config.default_hop_limit = UCN_MAX_HOPS;
        TEST_ASSERT(ucn_node_init(&nodes[index], &config) == UCN_OK);
        ucn_node_set_rx_handler(&nodes[index], stress_receive,
                                &receive_states[index]);

        links[index][0].ops = &STRESS_LINK_OPS;
        links[index][0].context = &contexts[index][0];
        links[index][0].link_id = 1U;
        links[index][0].mtu = UCN_MAX_FRAME_BYTES;
        links[index][0].peer_node_id = (ucn_node_id_t)(clockwise + 1U);
        links[index][1].ops = &STRESS_LINK_OPS;
        links[index][1].context = &contexts[index][1];
        links[index][1].link_id = 2U;
        links[index][1].mtu = UCN_MAX_FRAME_BYTES;
        links[index][1].peer_node_id = (ucn_node_id_t)(counterclockwise + 1U);
    }
    for (index = 0U; index < STRESS_NODE_COUNT; ++index) {
        size_t clockwise = (index + 1U) % STRESS_NODE_COUNT;
        size_t counterclockwise =
            (index + STRESS_NODE_COUNT - 1U) % STRESS_NODE_COUNT;
        size_t offset;

        contexts[index][0].peer = &nodes[clockwise];
        contexts[index][0].peer_ingress = &links[clockwise][1];
        contexts[index][0].edge_up = &edge_up[index];
        contexts[index][1].peer = &nodes[counterclockwise];
        contexts[index][1].peer_ingress = &links[counterclockwise][0];
        contexts[index][1].edge_up = &edge_up[counterclockwise];
        TEST_ASSERT(ucn_node_register_link(&nodes[index], &links[index][0]) == UCN_OK);
        TEST_ASSERT(ucn_node_register_link(&nodes[index], &links[index][1]) == UCN_OK);
        for (offset = 1U; offset <= STRESS_ROUTE_SPAN; ++offset) {
            size_t destination = (index + offset) % STRESS_NODE_COUNT;

            TEST_ASSERT(ucn_node_add_route(
                            &nodes[index], (ucn_node_id_t)(destination + 1U),
                            &links[index][0]) == UCN_OK);
        }
    }

    for (iteration = 0U; iteration < STRESS_ITERATIONS; ++iteration) {
        uint8_t payload[4];
        size_t source;
        size_t destination;
        uint32_t delivered_before = 0U;
        ucn_result_t result;

        if ((iteration % UINT32_C(17)) == 0U) {
            size_t edge = stress_random(&seed) % STRESS_NODE_COUNT;
            edge_up[edge] = !edge_up[edge];
        }
        if ((iteration % UINT32_C(101)) == 0U) {
            for (index = 0U; index < STRESS_NODE_COUNT; ++index) {
                edge_up[index] = true;
            }
        }
        source = stress_random(&seed) % STRESS_NODE_COUNT;
        destination = (source + 1U +
                       (stress_random(&seed) % STRESS_ROUTE_SPAN)) %
                      STRESS_NODE_COUNT;
        for (index = 0U; index < STRESS_NODE_COUNT; ++index) {
            delivered_before += receive_states[index].delivered;
            (void)ucn_node_step(&nodes[index],
                                UINT32_MAX - UINT32_C(1024) + iteration);
        }
        payload[0] = (uint8_t)(iteration >> 24U);
        payload[1] = (uint8_t)(iteration >> 16U);
        payload[2] = (uint8_t)(iteration >> 8U);
        payload[3] = (uint8_t)iteration;
        result = ucn_node_send(&nodes[source],
                               (ucn_node_id_t)(destination + 1U),
                               UCN_MSG_DATA_Q1, UCN_TRAFFIC_Q1_REALTIME,
                               payload, (uint16_t)sizeof(payload));
        if (result == UCN_OK) {
            successful++;
            TEST_ASSERT(receive_states[destination].delivered > 0U);
        } else {
            TEST_ASSERT(result == UCN_ERR_NOT_FOUND ||
                        result == UCN_ERR_LINK_DOWN ||
                        result == UCN_ERR_NO_SPACE);
        }
        {
            uint32_t delivered_after = 0U;

            for (index = 0U; index < STRESS_NODE_COUNT; ++index) {
                delivered_after += receive_states[index].delivered;
            }
            TEST_ASSERT(delivered_after == delivered_before +
                        (result == UCN_OK ? 1U : 0U));
        }
    }
    TEST_ASSERT(successful > STRESS_ITERATIONS / UINT32_C(4));
    return 0;
}
