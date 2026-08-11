#include <string.h>

#include "test_support.h"
#include "ucn/ucn_frame.h"
#include "ucn/ucn_node.h"

typedef struct management_security_state {
    ucn_sequence_t next_sequence;
    ucn_session_id_t local_session;
    bool allow_rx;
    bool enforce_source_session;
    ucn_node_id_t enforced_source;
    ucn_session_id_t accepted_session;
    uint32_t rx_calls;
} management_security_state_t;

typedef struct management_authorizer_state {
    bool allow;
    uint32_t calls[UCN_PATH_CONTROL_OPERATION_COUNT];
} management_authorizer_state_t;

typedef struct management_link_state {
    bool is_up;
} management_link_state_t;

static ucn_result_t management_security_load(void *context,
                                             ucn_sequence_t *next_sequence)
{
    *next_sequence = ((management_security_state_t *)context)->next_sequence;
    return UCN_OK;
}

static ucn_result_t management_security_store(void *context,
                                              ucn_sequence_t next_sequence)
{
    ((management_security_state_t *)context)->next_sequence = next_sequence;
    return UCN_OK;
}

static ucn_result_t management_security_session(void *context,
                                                ucn_session_id_t *session_id)
{
    *session_id = ((management_security_state_t *)context)->local_session;
    return UCN_OK;
}

static ucn_result_t management_security_authorize_tx(void *context,
                                                     const ucn_frame_t *frame)
{
    (void)context;
    (void)frame;
    return UCN_OK;
}

static ucn_result_t management_security_authorize_rx(
    void *context,
    const ucn_link_t *ingress_link,
    const ucn_frame_t *frame)
{
    management_security_state_t *state =
        (management_security_state_t *)context;

    (void)ingress_link;
    state->rx_calls++;
    if (!state->allow_rx) {
        return UCN_ERR_ACCESS;
    }
    if (state->enforce_source_session &&
        frame->source == state->enforced_source &&
        frame->session_id != state->accepted_session) {
        return UCN_ERR_ACCESS;
    }
    return UCN_OK;
}

static const ucn_security_ops_t MANAGEMENT_SECURITY_OPS = {
    management_security_load,
    management_security_store,
    management_security_session,
    management_security_authorize_tx,
    management_security_authorize_rx,
    NULL,
    NULL,
    NULL,
    NULL
};

static ucn_result_t management_authorize(
    void *context,
    const ucn_link_t *ingress_link,
    const ucn_frame_t *frame,
    ucn_path_control_operation_t operation,
    ucn_path_id_t path_id,
    ucn_node_id_t destination,
    ucn_node_id_t next_hop)
{
    management_authorizer_state_t *state =
        (management_authorizer_state_t *)context;

    (void)ingress_link;
    (void)frame;
    (void)path_id;
    (void)destination;
    (void)next_hop;
    if (operation >= UCN_PATH_CONTROL_OPERATION_COUNT) {
        return UCN_ERR_ARGUMENT;
    }
    state->calls[operation]++;
    return state->allow ? UCN_OK : UCN_ERR_ACCESS;
}

static ucn_result_t management_link_send(ucn_link_t *link,
                                         const uint8_t *data,
                                         size_t length)
{
    (void)link;
    (void)data;
    (void)length;
    return UCN_OK;
}

static ucn_result_t management_link_status(const ucn_link_t *link,
                                           ucn_link_status_t *status)
{
    const management_link_state_t *state =
        (const management_link_state_t *)link->context;

    status->is_up = state->is_up;
    status->mtu = link->mtu;
    status->tx_errors = 0U;
    status->rx_errors = 0U;
    return UCN_OK;
}

static const ucn_link_ops_t MANAGEMENT_LINK_OPS = {
    NULL,
    management_link_send,
    NULL,
    management_link_status,
    NULL,
    NULL
};

static void management_write_u32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)(value >> 24U);
    data[1] = (uint8_t)(value >> 16U);
    data[2] = (uint8_t)(value >> 8U);
    data[3] = (uint8_t)value;
}

static ucn_result_t management_inject(
    ucn_node_t *node,
    ucn_link_t *ingress_link,
    uint8_t message_type,
    ucn_node_id_t source,
    ucn_session_id_t session_id,
    ucn_sequence_t sequence,
    ucn_path_id_t path_id,
    uint32_t lease_ms)
{
    ucn_frame_t frame;
    uint8_t payload[20] = { 0U };
    uint8_t encoded[UCN_MAX_FRAME_BYTES];
    size_t encoded_length = 0U;
    ucn_result_t result;

    management_write_u32(payload, path_id);
    management_write_u32(payload + 4U, node->config.node_id);
    if (message_type == UCN_MSG_PATH_INSTALL) {
        management_write_u32(payload + 8U, 0U);
        management_write_u32(payload + 12U, lease_ms);
        payload[16] = 0U;
        payload[17] = (uint8_t)UCN_WIRE_PROFILE_UNSPECIFIED;
    }

    (void)memset(&frame, 0, sizeof(frame));
    frame.message_type = message_type;
    frame.traffic_class = UCN_TRAFFIC_Q0_CRITICAL;
    frame.hop_limit = 2U;
    frame.network_id = node->config.network_id;
    frame.source = source;
    frame.session_id = session_id;
    frame.destination = node->config.node_id;
    frame.sequence = sequence;
    frame.payload = payload;
    frame.payload_length = message_type == UCN_MSG_PATH_INSTALL ? 20U : 8U;

    result = ucn_frame_encode(&frame, encoded, sizeof(encoded), &encoded_length);
    if (result != UCN_OK) {
        return result;
    }
    return ucn_node_receive(node, ingress_link, encoded, encoded_length);
}

static ucn_result_t management_setup_node(
    ucn_node_t *node,
    management_security_state_t *security,
    management_authorizer_state_t *authorizer,
    ucn_link_t *ingress_a,
    management_link_state_t *link_state_a,
    ucn_link_t *ingress_b,
    management_link_state_t *link_state_b)
{
    ucn_config_t config;
    ucn_result_t result;

    (void)memset(node, 0, sizeof(*node));
    (void)memset(security, 0, sizeof(*security));
    (void)memset(authorizer, 0, sizeof(*authorizer));
    (void)memset(ingress_a, 0, sizeof(*ingress_a));
    (void)memset(ingress_b, 0, sizeof(*ingress_b));
    (void)memset(link_state_a, 0, sizeof(*link_state_a));
    (void)memset(link_state_b, 0, sizeof(*link_state_b));
    (void)memset(&config, 0, sizeof(config));

    config.network_id = UINT32_C(0x50415448);
    config.node_id = UINT32_C(2);
    config.default_hop_limit = 4U;
    result = ucn_node_init(node, &config);
    if (result != UCN_OK) {
        return result;
    }

    security->next_sequence = 1U;
    security->local_session = UINT32_C(0x9000);
    security->allow_rx = true;
    authorizer->allow = true;
    result = ucn_node_set_security(node, &MANAGEMENT_SECURITY_OPS, security);
    if (result != UCN_OK) {
        return result;
    }
    result = ucn_node_set_path_control_authorizer(node, management_authorize,
                                                   authorizer);
    if (result != UCN_OK) {
        return result;
    }

    link_state_a->is_up = true;
    ingress_a->ops = &MANAGEMENT_LINK_OPS;
    ingress_a->context = link_state_a;
    ingress_a->link_id = 1U;
    ingress_a->mtu = UCN_MAX_FRAME_BYTES;
    ingress_a->peer_node_id = UINT32_C(10);
    result = ucn_node_register_link(node, ingress_a);
    if (result != UCN_OK) {
        return result;
    }

    link_state_b->is_up = true;
    ingress_b->ops = &MANAGEMENT_LINK_OPS;
    ingress_b->context = link_state_b;
    ingress_b->link_id = 2U;
    ingress_b->mtu = UCN_MAX_FRAME_BYTES;
    ingress_b->peer_node_id = UINT32_C(10);
    return ucn_node_register_link(node, ingress_b);
}

static size_t management_budget_source_count(const ucn_node_t *node)
{
    size_t count = 0U;
    size_t index;

    for (index = 0U; index < UCN_PATH_CONTROL_RX_SOURCE_DEPTH; ++index) {
        if (node->path_control_source_budgets[index].occupied) {
            ++count;
        }
    }
    return count;
}

static const ucn_path_control_source_budget_t *management_find_budget(
    const ucn_node_t *node,
    ucn_node_id_t source)
{
    size_t index;

    for (index = 0U; index < UCN_PATH_CONTROL_RX_SOURCE_DEPTH; ++index) {
        if (node->path_control_source_budgets[index].occupied &&
            node->path_control_source_budgets[index].source == source) {
            return &node->path_control_source_budgets[index];
        }
    }
    return NULL;
}

static size_t management_path_count(const ucn_node_t *node)
{
    size_t count = 0U;
    size_t index;

    for (index = 0U; index < UCN_MAX_PATH_FORWARD_ENTRIES; ++index) {
        if (node->path_state.entries[index].occupied) {
            ++count;
        }
    }
    return count;
}

static int test_management_authorization_and_rate(void)
{
    ucn_node_t node;
    management_security_state_t security;
    management_authorizer_state_t authorizer;
    ucn_link_t ingress_a, ingress_b;
    management_link_state_t link_state_a, link_state_b;
    const ucn_path_control_source_budget_t *source_budget;
    ucn_path_state_t path_before;
    uint32_t authorizer_calls_before;
    uint32_t index;

    TEST_ASSERT(management_setup_node(&node, &security, &authorizer,
                                      &ingress_a, &link_state_a,
                                      &ingress_b, &link_state_b) == UCN_OK);
    TEST_ASSERT(sizeof(ucn_path_control_source_budget_t) <= 24U);

    path_before = node.path_state;
    security.allow_rx = false;
    TEST_ASSERT(management_inject(&node, &ingress_a, UCN_MSG_PATH_INSTALL,
                                  UINT32_C(10), UINT32_C(100), UINT32_C(1),
                                  UINT32_C(1000), UINT32_C(1000)) ==
                UCN_ERR_ACCESS);
    TEST_ASSERT(node.stats.path_install_authorization_rejected == 1U);
    TEST_ASSERT(authorizer.calls[UCN_PATH_CONTROL_INSTALL] == 0U);
    TEST_ASSERT(management_budget_source_count(&node) == 0U);
    TEST_ASSERT(memcmp(&path_before, &node.path_state,
                       sizeof(path_before)) == 0);

    security.allow_rx = true;
    authorizer.allow = false;
    TEST_ASSERT(management_inject(&node, &ingress_a, UCN_MSG_PATH_INSTALL,
                                  UINT32_C(10), UINT32_C(100), UINT32_C(2),
                                  UINT32_C(1000), UINT32_C(1000)) ==
                UCN_ERR_ACCESS);
    TEST_ASSERT(node.stats.path_install_authorization_rejected == 2U);
    TEST_ASSERT(authorizer.calls[UCN_PATH_CONTROL_INSTALL] == 1U);
    TEST_ASSERT(management_budget_source_count(&node) == 0U);
    TEST_ASSERT(memcmp(&path_before, &node.path_state,
                       sizeof(path_before)) == 0);

    authorizer.allow = true;
    for (index = 0U; index < UCN_PATH_CONTROL_RX_TOKEN_BURST; ++index) {
        ucn_link_t *ingress = (index & 1U) == 0U ? &ingress_a : &ingress_b;

        TEST_ASSERT(management_inject(
                        &node, ingress, UCN_MSG_PATH_INSTALL, UINT32_C(10),
                        UINT32_C(100), UINT32_C(10) + index, UINT32_C(1000),
                        UINT32_C(1000)) == UCN_OK);
    }
    TEST_ASSERT(management_budget_source_count(&node) == 1U);
    source_budget = management_find_budget(&node, UINT32_C(10));
    TEST_ASSERT(source_budget != NULL &&
                source_budget->tokens[UCN_PATH_CONTROL_INSTALL] == 0U);

    path_before = node.path_state;
    TEST_ASSERT(management_inject(
                    &node, &ingress_b, UCN_MSG_PATH_INSTALL, UINT32_C(10),
                    UINT32_C(100), UINT32_C(20), UINT32_C(1000),
                    UINT32_C(2000)) == UCN_ERR_NO_SPACE);
    TEST_ASSERT(node.stats.path_install_budget_rejected == 1U);
    TEST_ASSERT(management_budget_source_count(&node) == 1U);
    TEST_ASSERT(memcmp(&path_before, &node.path_state,
                       sizeof(path_before)) == 0);

    TEST_ASSERT(management_inject(
                    &node, &ingress_a, UCN_MSG_PATH_REVOKE, UINT32_C(10),
                    UINT32_C(100), UINT32_C(30), UINT32_C(1000), 0U) ==
                UCN_OK);
    TEST_ASSERT(management_path_count(&node) == 0U);
    for (index = 1U; index < UCN_PATH_CONTROL_RX_TOKEN_BURST; ++index) {
        TEST_ASSERT(management_inject(
                        &node, &ingress_b, UCN_MSG_PATH_REVOKE,
                        UINT32_C(10), UINT32_C(100), UINT32_C(30) + index,
                        UINT32_C(1000), 0U) == UCN_OK);
    }
    path_before = node.path_state;
    TEST_ASSERT(management_inject(
                    &node, &ingress_a, UCN_MSG_PATH_REVOKE, UINT32_C(10),
                    UINT32_C(100), UINT32_C(40), UINT32_C(1000), 0U) ==
                UCN_ERR_NO_SPACE);
    TEST_ASSERT(node.stats.path_revoke_budget_rejected == 1U);
    TEST_ASSERT(memcmp(&path_before, &node.path_state,
                       sizeof(path_before)) == 0);

    node.now_ms = UCN_PATH_CONTROL_RX_TOKEN_REFILL_MS;
    TEST_ASSERT(management_inject(
                    &node, &ingress_a, UCN_MSG_PATH_INSTALL, UINT32_C(10),
                    UINT32_C(100), UINT32_C(50), UINT32_C(1000),
                    UINT32_C(1000)) == UCN_OK);

    security.enforce_source_session = true;
    security.enforced_source = UINT32_C(10);
    security.accepted_session = UINT32_C(101);
    TEST_ASSERT(management_inject(
                    &node, &ingress_b, UCN_MSG_PATH_INSTALL, UINT32_C(10),
                    UINT32_C(101), UINT32_C(51), UINT32_C(1200),
                    UINT32_C(1000)) == UCN_OK);
    TEST_ASSERT(management_budget_source_count(&node) == 1U);
    TEST_ASSERT(node.stats.path_control_budget_session_rotations == 1U);
    source_budget = management_find_budget(&node, UINT32_C(10));
    TEST_ASSERT(source_budget != NULL &&
                source_budget->session_id == UINT32_C(101) &&
                source_budget->tokens[UCN_PATH_CONTROL_INSTALL] ==
                    (uint8_t)(UCN_PATH_CONTROL_RX_TOKEN_BURST - 1U));

    path_before = node.path_state;
    authorizer_calls_before = authorizer.calls[UCN_PATH_CONTROL_INSTALL];
    TEST_ASSERT(management_inject(
                    &node, &ingress_a, UCN_MSG_PATH_INSTALL, UINT32_C(10),
                    UINT32_C(100), UINT32_C(52), UINT32_C(1300),
                    UINT32_C(1000)) == UCN_ERR_ACCESS);
    TEST_ASSERT(node.stats.path_install_authorization_rejected == 3U);
    TEST_ASSERT(authorizer.calls[UCN_PATH_CONTROL_INSTALL] ==
                authorizer_calls_before);
    TEST_ASSERT(memcmp(&path_before, &node.path_state,
                       sizeof(path_before)) == 0);
    source_budget = management_find_budget(&node, UINT32_C(10));
    TEST_ASSERT(source_budget != NULL &&
                source_budget->session_id == UINT32_C(101));

    security.enforce_source_session = false;
    if (UCN_PATH_CONTROL_RX_SOURCE_DEPTH > 1U) {
        TEST_ASSERT(management_inject(
                        &node, &ingress_a, UCN_MSG_PATH_INSTALL,
                        UINT32_C(11), UINT32_C(100), UINT32_C(53),
                        UINT32_C(1100), UINT32_C(1000)) == UCN_OK);
        TEST_ASSERT(management_budget_source_count(&node) == 2U);
    }
    return 0;
}

static int test_management_source_table_and_reclaim(void)
{
    ucn_node_t node;
    management_security_state_t security;
    management_authorizer_state_t authorizer;
    ucn_link_t ingress_a, ingress_b;
    management_link_state_t link_state_a, link_state_b;
    ucn_path_state_t path_before;
    size_t index;

    TEST_ASSERT(management_setup_node(&node, &security, &authorizer,
                                      &ingress_a, &link_state_a,
                                      &ingress_b, &link_state_b) == UCN_OK);
    for (index = 0U; index < UCN_PATH_CONTROL_RX_SOURCE_DEPTH; ++index) {
        TEST_ASSERT(management_inject(
                        &node, &ingress_a, UCN_MSG_PATH_REVOKE,
                        UINT32_C(100) + (uint32_t)index, UINT32_C(200),
                        UINT32_C(100) + (uint32_t)index, UINT32_C(1), 0U) ==
                    UCN_OK);
    }
    TEST_ASSERT(management_budget_source_count(&node) ==
                UCN_PATH_CONTROL_RX_SOURCE_DEPTH);
    TEST_ASSERT(management_path_count(&node) == 0U);

    path_before = node.path_state;
    TEST_ASSERT(management_inject(
                    &node, &ingress_b, UCN_MSG_PATH_REVOKE, UINT32_C(999),
                    UINT32_C(200), UINT32_C(200), UINT32_C(1), 0U) ==
                UCN_ERR_NO_SPACE);
    TEST_ASSERT(node.stats.path_control_budget_source_full == 1U);
    TEST_ASSERT(node.stats.path_revoke_budget_rejected == 0U);
    TEST_ASSERT(memcmp(&path_before, &node.path_state,
                       sizeof(path_before)) == 0);

    node.now_ms = UCN_PATH_CONTROL_RX_SOURCE_IDLE_MS;
    TEST_ASSERT(management_inject(
                    &node, &ingress_b, UCN_MSG_PATH_REVOKE, UINT32_C(999),
                    UINT32_C(200), UINT32_C(201), UINT32_C(1), 0U) == UCN_OK);
    TEST_ASSERT(node.stats.path_control_budget_sources_reclaimed ==
                UCN_PATH_CONTROL_RX_SOURCE_DEPTH);
    TEST_ASSERT(management_budget_source_count(&node) == 1U);
    return 0;
}

static int test_management_path_table_full(void)
{
    ucn_node_t node;
    management_security_state_t security;
    management_authorizer_state_t authorizer;
    ucn_link_t ingress_a, ingress_b;
    management_link_state_t link_state_a, link_state_b;
    ucn_path_forward_config_t config;
    ucn_path_state_t path_before;
    size_t index;

    TEST_ASSERT(management_setup_node(&node, &security, &authorizer,
                                      &ingress_a, &link_state_a,
                                      &ingress_b, &link_state_b) == UCN_OK);
    (void)memset(&config, 0, sizeof(config));
    config.owner_session_id = UINT32_C(1);
    config.destination = node.config.node_id;
    config.expires_at_ms = UINT32_C(1000);
    for (index = 0U; index < UCN_MAX_PATH_FORWARD_ENTRIES; ++index) {
        config.owner = UINT32_C(200) + (uint32_t)index;
        config.path_id = UINT32_C(1) + (uint32_t)index;
        TEST_ASSERT(ucn_path_install(&node.path_state, &config) == UCN_OK);
    }
    path_before = node.path_state;
    TEST_ASSERT(management_inject(
                    &node, &ingress_a, UCN_MSG_PATH_INSTALL, UINT32_C(10),
                    UINT32_C(100), UINT32_C(300), UINT32_C(999),
                    UINT32_C(1000)) == UCN_ERR_NO_SPACE);
    TEST_ASSERT(node.stats.path_install_table_full == 1U);
    TEST_ASSERT(node.stats.path_install_budget_rejected == 0U);
    TEST_ASSERT(node.stats.path_control_budget_source_full == 0U);
    TEST_ASSERT(memcmp(&path_before, &node.path_state,
                       sizeof(path_before)) == 0);
    return 0;
}

int test_path_management_budget(void)
{
    int result = 0;

    result |= test_management_authorization_and_rate();
    result |= test_management_source_table_and_reclaim();
    result |= test_management_path_table_full();
    return result;
}
