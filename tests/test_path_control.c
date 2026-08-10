#include <string.h>

#include "test_support.h"
#include "ucn/ucn_frame.h"
#include "ucn/ucn_node.h"

typedef struct path_security_state {
    ucn_sequence_t next_sequence;
    ucn_session_id_t session_id;
    uint32_t seal_calls;
    uint32_t open_calls;
} path_security_state_t;

typedef struct path_authorize_state {
    bool allow;
    uint32_t calls;
} path_authorize_state_t;

typedef struct path_link_context {
    ucn_node_t *peer;
    ucn_link_t *peer_ingress;
    bool is_up;
    ucn_link_metrics_t metrics;
    uint32_t sent_count;
    ucn_result_t last_receive_result;
    bool last_has_path_id;
    bool last_protected;
    ucn_path_id_t last_path_id;
} path_link_context_t;

typedef struct path_receive_state {
    uint32_t count;
    uint8_t last_payload;
    uint8_t last_message_type;
    bool last_has_path_id;
    ucn_path_id_t last_path_id;
} path_receive_state_t;

static ucn_result_t path_security_load(void *context,
                                       ucn_sequence_t *next_sequence)
{
    *next_sequence = ((path_security_state_t *)context)->next_sequence;
    return UCN_OK;
}

static ucn_result_t path_security_store(void *context,
                                        ucn_sequence_t next_sequence)
{
    ((path_security_state_t *)context)->next_sequence = next_sequence;
    return UCN_OK;
}

static ucn_result_t path_security_session(void *context,
                                          ucn_session_id_t *session_id)
{
    *session_id = ((path_security_state_t *)context)->session_id;
    return UCN_OK;
}

static ucn_result_t path_security_authorize_tx(void *context,
                                               const ucn_frame_t *frame)
{
    return frame->session_id == ((path_security_state_t *)context)->session_id ?
           UCN_OK : UCN_ERR_ACCESS;
}

static ucn_result_t path_security_authorize_rx(void *context,
                                               const ucn_link_t *ingress_link,
                                               const ucn_frame_t *frame)
{
    (void)ingress_link;
    return frame->session_id == ((path_security_state_t *)context)->session_id ?
           UCN_OK : UCN_ERR_ACCESS;
}

static uint8_t path_security_tag_value(const ucn_frame_t *frame,
                                       const uint8_t *ciphertext,
                                       uint16_t ciphertext_length)
{
    uint8_t aad[32];
    size_t aad_length = 0U;
    uint8_t value = 0xA7U;
    size_t index;

    if (ucn_frame_write_e2e_aad(frame, aad, sizeof(aad), &aad_length) != UCN_OK) {
        return 0U;
    }
    for (index = 0U; index < aad_length; ++index) {
        value = (uint8_t)(value + aad[index]);
    }
    for (index = 0U; index < ciphertext_length; ++index) {
        value = (uint8_t)(value + ciphertext[index]);
    }
    return value;
}

static ucn_result_t path_security_seal(void *context,
                                       const ucn_frame_t *frame,
                                       const uint8_t *plaintext,
                                       uint16_t plaintext_length,
                                       uint8_t *ciphertext,
                                       uint8_t auth_tag[UCN_E2E_TAG_SIZE])
{
    uint16_t index;
    uint8_t value;

    ((path_security_state_t *)context)->seal_calls++;
    for (index = 0U; index < plaintext_length; ++index) {
        ciphertext[index] = (uint8_t)(plaintext[index] ^ 0x5AU);
    }
    value = path_security_tag_value(frame, ciphertext, plaintext_length);
    for (index = 0U; index < UCN_E2E_TAG_SIZE; ++index) {
        auth_tag[index] = (uint8_t)(value + index);
    }
    return UCN_OK;
}

static ucn_result_t path_security_open(void *context,
                                       const ucn_link_t *ingress_link,
                                       const ucn_frame_t *frame,
                                       const uint8_t *ciphertext,
                                       uint16_t ciphertext_length,
                                       const uint8_t auth_tag[UCN_E2E_TAG_SIZE],
                                       uint8_t *plaintext)
{
    uint16_t index;
    uint8_t value;

    (void)ingress_link;
    ((path_security_state_t *)context)->open_calls++;
    value = path_security_tag_value(frame, ciphertext, ciphertext_length);
    for (index = 0U; index < UCN_E2E_TAG_SIZE; ++index) {
        if (auth_tag[index] != (uint8_t)(value + index)) {
            return UCN_ERR_SECURITY;
        }
    }
    for (index = 0U; index < ciphertext_length; ++index) {
        plaintext[index] = (uint8_t)(ciphertext[index] ^ 0x5AU);
    }
    return UCN_OK;
}

static const ucn_security_ops_t PATH_SECURITY_OPS = {
    path_security_load,
    path_security_store,
    path_security_session,
    path_security_authorize_tx,
    path_security_authorize_rx,
    NULL,
    path_security_seal,
    path_security_open,
    NULL
};

static ucn_result_t path_authorize(void *context,
                                   const ucn_link_t *ingress_link,
                                   const ucn_frame_t *frame,
                                   ucn_path_control_operation_t operation,
                                   ucn_path_id_t path_id,
                                   ucn_node_id_t destination,
                                   ucn_node_id_t next_hop)
{
    path_authorize_state_t *state = (path_authorize_state_t *)context;

    (void)ingress_link;
    (void)operation;
    (void)path_id;
    (void)destination;
    (void)next_hop;
    state->calls++;
    return state->allow && frame->source == UINT32_C(1) ? UCN_OK : UCN_ERR_ACCESS;
}

static ucn_result_t path_link_send(ucn_link_t *link,
                                   const uint8_t *frame,
                                   size_t length)
{
    path_link_context_t *context = (path_link_context_t *)link->context;
    ucn_frame_t decoded;

    context->sent_count++;
    if (!context->is_up) {
        context->last_receive_result = UCN_ERR_LINK_DOWN;
        return UCN_ERR_LINK_DOWN;
    }
    if (ucn_frame_decode(frame, length, &decoded) == UCN_OK) {
        context->last_has_path_id = decoded.has_path_id;
        context->last_protected =
            (decoded.flags & UCN_FRAME_FLAG_E2E_PROTECTED) != 0U;
        context->last_path_id = decoded.path_id;
    }
    context->last_receive_result = ucn_node_receive(context->peer,
                                                    context->peer_ingress,
                                                    frame, length);
    return context->last_receive_result;
}

static ucn_result_t path_link_status(const ucn_link_t *link,
                                     ucn_link_status_t *status)
{
    const path_link_context_t *context =
        (const path_link_context_t *)link->context;

    status->is_up = context->is_up;
    status->mtu = link->mtu;
    status->tx_errors = 0U;
    status->rx_errors = 0U;
    return UCN_OK;
}

static ucn_result_t path_link_metrics(const ucn_link_t *link,
                                      ucn_link_metrics_t *metrics)
{
    *metrics = ((const path_link_context_t *)link->context)->metrics;
    return UCN_OK;
}

static const ucn_link_ops_t PATH_LINK_OPS = {
    NULL, path_link_send, NULL, path_link_status, NULL, path_link_metrics
};

static int path_init_node(ucn_node_t *node,
                          ucn_node_id_t node_id,
                          path_security_state_t *security)
{
    ucn_config_t config;
    ucn_security_policy_t policy;

    config.network_id = UINT32_C(0xA1B2C3D4);
    config.node_id = node_id;
    config.default_hop_limit = 5U;
    security->next_sequence = 1U;
    security->session_id = UINT32_C(0x99);
    security->seal_calls = 0U;
    security->open_calls = 0U;
    (void)memset(node, 0, sizeof(*node));
    TEST_ASSERT(ucn_node_init(node, &config) == UCN_OK);
    TEST_ASSERT(ucn_node_set_security(node, &PATH_SECURITY_OPS, security) == UCN_OK);
    policy.tx_mode = UCN_SECURITY_TX_E2E_PROTECTED;
    policy.rx_mode = UCN_SECURITY_RX_BOTH;
    policy.forward_mode = UCN_SECURITY_FORWARD_PLAIN_AND_OPAQUE_E2E;
    TEST_ASSERT(ucn_node_set_security_policy(node, &policy) == UCN_OK);
    return 0;
}

static void path_prepare_pair(ucn_node_t *left,
                              ucn_node_id_t left_id,
                              ucn_node_t *right,
                              ucn_node_id_t right_id,
                              uint8_t left_link_id,
                              ucn_link_t *left_link,
                              ucn_link_t *right_link,
                              path_link_context_t *left_context,
                              path_link_context_t *right_context)
{
    (void)memset(left_link, 0, sizeof(*left_link));
    (void)memset(right_link, 0, sizeof(*right_link));
    (void)memset(left_context, 0, sizeof(*left_context));
    (void)memset(right_context, 0, sizeof(*right_context));
    left_link->ops = &PATH_LINK_OPS;
    left_link->context = left_context;
    left_link->link_id = left_link_id;
    left_link->mtu = UCN_MAX_FRAME_BYTES;
    left_link->peer_node_id = right_id;
    right_link->ops = &PATH_LINK_OPS;
    right_link->context = right_context;
    right_link->link_id = (uint8_t)(left_link_id + 1U);
    right_link->mtu = UCN_MAX_FRAME_BYTES;
    right_link->peer_node_id = left_id;
    left_context->peer = right;
    left_context->peer_ingress = right_link;
    left_context->is_up = true;
    right_context->peer = left;
    right_context->peer_ingress = left_link;
    right_context->is_up = true;
}

static int path_connect_pair(ucn_node_t *left,
                             ucn_node_id_t left_id,
                             ucn_node_t *right,
                             ucn_node_id_t right_id,
                             uint8_t left_link_id,
                             ucn_link_t *left_link,
                             ucn_link_t *right_link,
                             path_link_context_t *left_context,
                             path_link_context_t *right_context)
{
    path_prepare_pair(left, left_id, right, right_id, left_link_id, left_link,
                      right_link, left_context, right_context);
    TEST_ASSERT(ucn_node_register_link(left, left_link) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(right, right_link) == UCN_OK);
    return 0;
}

/* Unlike path_connect_pair(), this leaves registration to the normal
 * Neighbor admission path so two physical Links become one logical next hop
 * with Primary/Backup Bearers. */
static int path_admit_bearer_pair(ucn_node_t *left,
                                  ucn_node_id_t left_id,
                                  ucn_node_t *right,
                                  ucn_node_id_t right_id,
                                  uint8_t left_link_id,
                                  ucn_link_t *left_link,
                                  ucn_link_t *right_link,
                                  path_link_context_t *left_context,
                                  path_link_context_t *right_context)
{
    path_prepare_pair(left, left_id, right, right_id, left_link_id, left_link,
                      right_link, left_context, right_context);
    TEST_ASSERT(ucn_node_observe_neighbor(left, left_link, 0U) == UCN_OK);
    TEST_ASSERT(ucn_node_observe_neighbor(right, right_link, 0U) == UCN_OK);
    return 0;
}

static void path_receive(void *context, const ucn_frame_t *frame)
{
    path_receive_state_t *state = (path_receive_state_t *)context;

    state->count++;
    state->last_payload = frame->payload[0];
    state->last_message_type = frame->message_type;
    state->last_has_path_id = frame->has_path_id;
    state->last_path_id = frame->path_id;
}

static int path_step(ucn_node_t *node, uint32_t now_ms)
{
    const ucn_result_t result = ucn_node_step(node, now_ms);

    return result == UCN_OK || result == UCN_ERR_NOT_FOUND ? 0 : 1;
}

static int test_path_frame_extension(void)
{
    uint8_t payload = 0x55U;
    uint8_t tag[UCN_E2E_TAG_SIZE] = { 0U };
    uint8_t encoded[UCN_MAX_FRAME_BYTES];
    uint8_t aad_a[32];
    uint8_t aad_b[32];
    size_t encoded_length = 0U;
    size_t aad_length_a = 0U;
    size_t aad_length_b = 0U;
    ucn_frame_t frame;
    ucn_frame_t decoded;

    (void)memset(&frame, 0, sizeof(frame));
    frame.message_type = UCN_STATIC_ENDPOINT_FIRST;
    frame.traffic_class = UCN_TRAFFIC_Q1_REALTIME;
    frame.flags = UCN_FRAME_FLAG_ROUTE_EXTENSION | UCN_FRAME_FLAG_PATH_ID |
                  UCN_FRAME_FLAG_E2E_PROTECTED;
    frame.hop_limit = 3U;
    frame.network_id = UINT32_C(0xA1B2C3D4);
    frame.source = UINT32_C(1);
    frame.destination = UINT32_C(4);
    frame.sequence = UINT32_C(7);
    frame.session_id = UINT32_C(0x99);
    frame.has_route_extension = true;
    frame.has_path_id = true;
    frame.path_id = UINT32_C(0x1001);
    frame.payload = &payload;
    frame.payload_length = 1U;
    frame.auth_tag = tag;

    TEST_ASSERT(ucn_frame_encode(&frame, encoded, sizeof(encoded),
                                 &encoded_length) == UCN_OK);
    TEST_ASSERT(encoded_length == UCN_FRAME_PATH_HEADER_SIZE + 1U + UCN_E2E_TAG_SIZE);
    TEST_ASSERT(ucn_frame_decode(encoded, encoded_length, &decoded) == UCN_OK);
    TEST_ASSERT(decoded.has_route_extension && decoded.has_path_id &&
                decoded.path_id == frame.path_id);
    TEST_ASSERT(ucn_frame_write_e2e_aad(&frame, aad_a, sizeof(aad_a),
                                        &aad_length_a) == UCN_OK);
    decoded.path_id = UINT32_C(0x1002);
    TEST_ASSERT(ucn_frame_write_e2e_aad(&decoded, aad_b, sizeof(aad_b),
                                        &aad_length_b) == UCN_OK);
    TEST_ASSERT(aad_length_a == ucn_frame_e2e_aad_size() &&
                aad_length_a == aad_length_b &&
                memcmp(aad_a, aad_b, aad_length_a) != 0);
    encoded[4] = (uint8_t)(encoded[4] & (uint8_t)~UCN_FRAME_FLAG_PATH_ID);
    TEST_ASSERT(ucn_frame_decode(encoded, encoded_length, &decoded) == UCN_ERR_MALFORMED);
    return 0;
}

static int test_path_bearer_binding(void)
{
    const ucn_path_id_t path_p1 = UINT32_C(0x2201);
    const ucn_path_id_t path_p2 = UINT32_C(0x2202);
    const ucn_path_id_t path_p3 = UINT32_C(0x2203);
    const uint8_t payload = 0xC5U;
    ucn_node_t a, b, d;
    ucn_link_t ab_primary, ba_primary, ab_backup, ba_backup;
    ucn_link_t bd_primary, db_primary, bd_backup, db_backup, ad, da;
    path_link_context_t cab_primary, cba_primary, cab_backup, cba_backup;
    path_link_context_t cbd_primary, cdb_primary, cbd_backup, cdb_backup;
    path_link_context_t cad, cda;
    path_security_state_t sa, sb, sd;
    path_receive_state_t received;
    ucn_path_forward_config_t path_config;
    ucn_policy_path_config_t policy_path;
    ucn_route_policy_config_t policy;

    if (UCN_MAX_BEARERS_PER_NEIGHBOR < 2U) {
        return 0;
    }
    (void)memset(&received, 0, sizeof(received));
    TEST_ASSERT(path_init_node(&a, UINT32_C(1), &sa) == 0);
    TEST_ASSERT(path_init_node(&b, UINT32_C(2), &sb) == 0);
    TEST_ASSERT(path_init_node(&d, UINT32_C(4), &sd) == 0);
    TEST_ASSERT(ucn_node_set_join_policy(&a, UCN_JOIN_OPEN, NULL, NULL) == UCN_OK);
    TEST_ASSERT(ucn_node_set_join_policy(&b, UCN_JOIN_OPEN, NULL, NULL) == UCN_OK);
    TEST_ASSERT(ucn_node_set_join_policy(&d, UCN_JOIN_OPEN, NULL, NULL) == UCN_OK);

    TEST_ASSERT(path_admit_bearer_pair(&a, UINT32_C(1), &b, UINT32_C(2), 31U,
                                        &ab_primary, &ba_primary, &cab_primary,
                                        &cba_primary) == 0);
    TEST_ASSERT(path_admit_bearer_pair(&a, UINT32_C(1), &b, UINT32_C(2), 33U,
                                        &ab_backup, &ba_backup, &cab_backup,
                                        &cba_backup) == 0);
    TEST_ASSERT(path_admit_bearer_pair(&b, UINT32_C(2), &d, UINT32_C(4), 35U,
                                        &bd_primary, &db_primary, &cbd_primary,
                                        &cdb_primary) == 0);
    TEST_ASSERT(path_admit_bearer_pair(&b, UINT32_C(2), &d, UINT32_C(4), 37U,
                                        &bd_backup, &db_backup, &cbd_backup,
                                        &cdb_backup) == 0);
    TEST_ASSERT(path_connect_pair(&a, UINT32_C(1), &d, UINT32_C(4), 39U,
                                  &ad, &da, &cad, &cda) == 0);
    ucn_node_set_rx_handler(&d, path_receive, &received);

    (void)memset(&path_config, 0, sizeof(path_config));
    path_config.owner = UINT32_C(1);
    path_config.owner_session_id = sa.session_id;
    path_config.destination = UINT32_C(4);
    path_config.expires_at_ms = UINT32_C(10000);
    path_config.path_id = path_p1;
    path_config.next_hop = UINT32_C(2);
    path_config.egress_link = &ab_primary;
    TEST_ASSERT(ucn_path_install(&a.path_state, &path_config) == UCN_OK);
    path_config.next_hop = UINT32_C(4);
    path_config.egress_link = &bd_primary;
    TEST_ASSERT(ucn_path_install(&b.path_state, &path_config) == UCN_OK);
    path_config.next_hop = 0U;
    path_config.egress_link = NULL;
    TEST_ASSERT(ucn_path_install(&d.path_state, &path_config) == UCN_OK);

    /* P2 never uses either logical Bearer set.  It proves that a complete
     * Neighbor failure only removes the Paths that actually use that hop. */
    path_config.path_id = path_p2;
    path_config.next_hop = UINT32_C(4);
    path_config.egress_link = &ad;
    TEST_ASSERT(ucn_path_install(&a.path_state, &path_config) == UCN_OK);
    path_config.next_hop = 0U;
    path_config.egress_link = NULL;
    TEST_ASSERT(ucn_path_install(&d.path_state, &path_config) == UCN_OK);

    (void)memset(&policy_path, 0, sizeof(policy_path));
    policy_path.local_path_id = 1U;
    policy_path.wire_path_id = path_p1;
    policy_path.destination = UINT32_C(4);
    policy_path.egress_link = &ab_primary;
    policy_path.verified = true;
    TEST_ASSERT(ucn_node_set_policy_path(&a, &policy_path) == UCN_OK);
    policy_path.local_path_id = 2U;
    policy_path.wire_path_id = path_p2;
    policy_path.egress_link = &ad;
    TEST_ASSERT(ucn_node_set_policy_path(&a, &policy_path) == UCN_OK);
    (void)memset(&policy, 0, sizeof(policy));
    policy.key.destination = UINT32_C(4);
    policy.key.endpoint = (ucn_endpoint_t)0x50U;
    policy.key.traffic_class = (uint8_t)UCN_TRAFFIC_Q1_REALTIME;
    policy.mode = UCN_ROUTE_POLICY_PINNED_STRICT;
    policy.primary_local_path_id = 1U;
    TEST_ASSERT(ucn_node_set_route_policy(&a, &policy) == UCN_OK);

    TEST_ASSERT(ucn_node_send_endpoint(&a, UINT32_C(4), (ucn_endpoint_t)0x50U,
                                       UCN_TRAFFIC_Q1_REALTIME, &payload, 1U) == UCN_OK);
    TEST_ASSERT(received.count == 1U && received.last_path_id == path_p1);

    /* A single A-B Bearer failure only changes the physical sender Link.
     * The source Path, relay Path and local Policy handle all remain valid. */
    cab_primary.is_up = false;
    cba_primary.is_up = false;
    TEST_ASSERT(ucn_node_send_endpoint(&a, UINT32_C(4), (ucn_endpoint_t)0x50U,
                                       UCN_TRAFFIC_Q1_REALTIME, &payload, 1U) == UCN_OK);
    TEST_ASSERT(received.count == 2U && received.last_path_id == path_p1 &&
                cab_backup.last_path_id == path_p1);
    TEST_ASSERT(ucn_node_find_path_forward(&a, UINT32_C(1), sa.session_id,
                                            path_p1, UINT32_C(4)) != NULL);
    TEST_ASSERT(ucn_node_find_path_forward(&b, UINT32_C(1), sa.session_id,
                                            path_p1, UINT32_C(4)) != NULL);
    TEST_ASSERT(ucn_node_find_policy_path(&a, 1U)->state ==
                UCN_POLICY_PATH_VERIFIED);

    /* The same rule holds on the relay's B-D hop. */
    cbd_primary.is_up = false;
    cdb_primary.is_up = false;
    TEST_ASSERT(path_step(&b, 2U) == 0);
    TEST_ASSERT(ucn_node_send_endpoint(&a, UINT32_C(4), (ucn_endpoint_t)0x50U,
                                       UCN_TRAFFIC_Q1_REALTIME, &payload, 1U) == UCN_OK);
    TEST_ASSERT(received.count == 3U && received.last_path_id == path_p1 &&
                cbd_backup.last_path_id == path_p1);
    TEST_ASSERT(ucn_node_find_path_forward(&b, UINT32_C(1), sa.session_id,
                                            path_p1, UINT32_C(4)) != NULL);

    /* All B-D Bearers down revokes only B's P1 forwarding entry.  The next
     * P1 frame returns a Path-scoped RERR through healthy A-B Backup, which
     * revokes A's corresponding P1 and marks its local policy Path Down. */
    cbd_backup.is_up = false;
    cdb_backup.is_up = false;
    TEST_ASSERT(path_step(&b, 3U) == 0);
    TEST_ASSERT(ucn_node_find_path_forward(&b, UINT32_C(1), sa.session_id,
                                            path_p1, UINT32_C(4)) == NULL);
    TEST_ASSERT(ucn_node_send_endpoint(&a, UINT32_C(4), (ucn_endpoint_t)0x50U,
                                       UCN_TRAFFIC_Q1_REALTIME, &payload, 1U) ==
                UCN_ERR_NOT_FOUND);
    TEST_ASSERT(ucn_node_get_stats(&b)->path_route_errors_sent == 1U);
    TEST_ASSERT(ucn_node_find_path_forward(&a, UINT32_C(1), sa.session_id,
                                            path_p1, UINT32_C(4)) == NULL);
    TEST_ASSERT(ucn_node_find_policy_path(&a, 1U)->state == UCN_POLICY_PATH_DOWN);
    TEST_ASSERT(ucn_node_find_path_forward(&a, UINT32_C(1), sa.session_id,
                                            path_p2, UINT32_C(4)) != NULL &&
                ucn_node_find_policy_path(&a, 2U)->state ==
                    UCN_POLICY_PATH_VERIFIED);

    /* Full A-B loss also clears a local P3 that uses only that Neighbor, but
     * leaves the independent direct P2 untouched. */
    path_config.path_id = path_p3;
    path_config.next_hop = UINT32_C(2);
    path_config.egress_link = &ab_primary;
    TEST_ASSERT(ucn_path_install(&a.path_state, &path_config) == UCN_OK);
    policy_path.local_path_id = 3U;
    policy_path.wire_path_id = path_p3;
    policy_path.destination = UINT32_C(4);
    policy_path.egress_link = &ab_primary;
    policy_path.verified = true;
    TEST_ASSERT(ucn_node_set_policy_path(&a, &policy_path) == UCN_OK);
    cab_backup.is_up = false;
    cba_backup.is_up = false;
    TEST_ASSERT(path_step(&a, 4U) == 0);
    TEST_ASSERT(ucn_node_find_path_forward(&a, UINT32_C(1), sa.session_id,
                                            path_p3, UINT32_C(4)) == NULL &&
                ucn_node_find_policy_path(&a, 3U)->state == UCN_POLICY_PATH_DOWN);
    TEST_ASSERT(ucn_node_find_path_forward(&a, UINT32_C(1), sa.session_id,
                                            path_p2, UINT32_C(4)) != NULL &&
                ucn_node_find_policy_path(&a, 2U)->state ==
                    UCN_POLICY_PATH_VERIFIED);
    return 0;
}

int test_path_control(void)
{
    const ucn_path_id_t path_p1 = UINT32_C(0x1001);
    const ucn_path_id_t path_p2 = UINT32_C(0x1002);
    const ucn_path_id_t path_p3 = UINT32_C(0x1003);
    const uint8_t payload_p1 = 0xA1U;
    const uint8_t payload_p2 = 0xB2U;
    ucn_node_t a, b, c, d;
    ucn_link_t ab, ba, bd, db, ac, ca, cd, dc;
    path_link_context_t cab, cba, cbd, cdb, cac, cca, ccd, cdc;
    path_security_state_t sa, sb, sc, sd;
    path_authorize_state_t auth_b = { true, 0U };
    path_authorize_state_t auth_c = { true, 0U };
    path_authorize_state_t auth_d = { true, 0U };
    path_receive_state_t received;
    ucn_policy_path_config_t policy_path;
    ucn_route_policy_config_t policy;
    const ucn_policy_stats_t *policy_stats;
    size_t index;

    TEST_ASSERT(test_path_frame_extension() == 0);
    TEST_ASSERT(test_path_bearer_binding() == 0);
    (void)memset(&received, 0, sizeof(received));
    TEST_ASSERT(path_init_node(&a, UINT32_C(1), &sa) == 0);
    TEST_ASSERT(path_init_node(&b, UINT32_C(2), &sb) == 0);
    TEST_ASSERT(path_init_node(&c, UINT32_C(3), &sc) == 0);
    TEST_ASSERT(path_init_node(&d, UINT32_C(4), &sd) == 0);
    TEST_ASSERT(path_connect_pair(&a, UINT32_C(1), &b, UINT32_C(2), 1U,
                                  &ab, &ba, &cab, &cba) == 0);
    TEST_ASSERT(path_connect_pair(&b, UINT32_C(2), &d, UINT32_C(4), 3U,
                                  &bd, &db, &cbd, &cdb) == 0);
    TEST_ASSERT(path_connect_pair(&a, UINT32_C(1), &c, UINT32_C(3), 5U,
                                  &ac, &ca, &cac, &cca) == 0);
    TEST_ASSERT(path_connect_pair(&c, UINT32_C(3), &d, UINT32_C(4), 7U,
                                  &cd, &dc, &ccd, &cdc) == 0);
    TEST_ASSERT(ucn_node_add_route(&a, UINT32_C(4), &ab) == UCN_OK);
    ucn_node_set_rx_handler(&d, path_receive, &received);

    /* AUTO_BALANCE uses only normalized base Cost plus active-flow count.
     * Large raw RTT/failure samples on the cheaper P1 must not be added to the
     * Cost score; queue pressure remains a separate sustained-congestion gate. */
    cab.metrics.route_cost_valid = true;
    cab.metrics.route_cost = 10U;
    cab.metrics.rtt_valid = true;
    cab.metrics.rtt_ms = 1000U;
    cab.metrics.tx_failure_rate_valid = true;
    cab.metrics.tx_failure_per_mille = 1000U;
    cac.metrics.route_cost_valid = true;
    cac.metrics.route_cost = 15U;
    cac.metrics.rtt_valid = true;
    cac.metrics.rtt_ms = 1U;
    cac.metrics.tx_failure_rate_valid = true;
    cac.metrics.tx_failure_per_mille = 0U;
    TEST_ASSERT(path_step(&a, 0U) == 0);

    /* A security Provider alone is not sufficient: remote Path changes are
     * denied until the product installs an explicit authorization callback. */
    TEST_ASSERT(ucn_node_send_path_install(&a, UINT32_C(2), path_p1,
                                            UINT32_C(4), UINT32_C(4),
                                            UINT32_C(10000)) == UCN_ERR_ACCESS);
    TEST_ASSERT(ucn_node_set_path_control_authorizer(&b, path_authorize,
                                                      &auth_b) == UCN_OK);
    TEST_ASSERT(ucn_node_set_path_control_authorizer(&c, path_authorize,
                                                      &auth_c) == UCN_OK);
    TEST_ASSERT(ucn_node_set_path_control_authorizer(&d, path_authorize,
                                                      &auth_d) == UCN_OK);

    TEST_ASSERT(ucn_node_install_local_path(&a, path_p1, UINT32_C(4),
                                             UINT32_C(2), UINT32_C(10000)) == UCN_OK);
    TEST_ASSERT(ucn_node_send_path_install(&a, UINT32_C(2), path_p1,
                                            UINT32_C(4), UINT32_C(4),
                                            UINT32_C(10000)) == UCN_OK);
    TEST_ASSERT(ucn_node_send_path_install(&a, UINT32_C(2), path_p1,
                                            UINT32_C(4), UINT32_C(4),
                                            UINT32_C(10000)) == UCN_OK);
    TEST_ASSERT(ucn_node_send_path_install(&a, UINT32_C(4), path_p1,
                                            UINT32_C(4), 0U,
                                            UINT32_C(10000)) == UCN_OK);
    TEST_ASSERT(ucn_node_install_local_path(&a, path_p2, UINT32_C(4),
                                             UINT32_C(3), UINT32_C(10000)) == UCN_OK);
    TEST_ASSERT(ucn_node_send_path_install(&a, UINT32_C(3), path_p2,
                                            UINT32_C(4), UINT32_C(4),
                                            UINT32_C(10000)) == UCN_OK);
    TEST_ASSERT(ucn_node_send_path_install(&a, UINT32_C(4), path_p2,
                                            UINT32_C(4), 0U,
                                            UINT32_C(10000)) == UCN_OK);
    TEST_ASSERT(auth_b.calls >= 2U && auth_c.calls >= 1U && auth_d.calls >= 2U);
    TEST_ASSERT(ucn_node_find_path_forward(&b, UINT32_C(1), sa.session_id,
                                            path_p1, UINT32_C(4)) != NULL);
    TEST_ASSERT(ucn_node_find_path_forward(&c, UINT32_C(1), sa.session_id,
                                            path_p2, UINT32_C(4)) != NULL);

    /* T22.3 binds local policy handles to authenticated source-side Path IDs.
     * Normal Endpoint sends must then use the selected wire Path, rather than
     * silently falling back to the ordinary Route Cache. */
    (void)memset(&policy_path, 0, sizeof(policy_path));
    policy_path.local_path_id = 1U;
    policy_path.wire_path_id = path_p1;
    policy_path.destination = UINT32_C(4);
    policy_path.egress_link = &ab;
    policy_path.verified = true;
    TEST_ASSERT(ucn_node_set_policy_path(&a, &policy_path) == UCN_OK);
    policy_path.local_path_id = 2U;
    policy_path.wire_path_id = path_p2;
    policy_path.egress_link = &ac;
    TEST_ASSERT(ucn_node_set_policy_path(&a, &policy_path) == UCN_OK);

    /* T22.4 uses the existing fixed Primary/Backup set as two eligible Path
     * members.  The Flow table makes repeated Endpoint sends stay on one Path
     * instead of striping each frame. */
    (void)memset(&policy, 0, sizeof(policy));
    policy.key.destination = UINT32_C(4);
    policy.key.endpoint = (ucn_endpoint_t)0x47U;
    policy.key.traffic_class = (uint8_t)UCN_TRAFFIC_Q1_REALTIME;
    policy.mode = UCN_ROUTE_POLICY_AUTO_BALANCE;
    policy.primary_local_path_id = 1U;
    policy.backup_local_path_id = 2U;
    policy.balance_flow_lease_ms = UINT32_C(2000);
    TEST_ASSERT(ucn_node_set_route_policy(&a, &policy) == UCN_OK);
    TEST_ASSERT(ucn_node_send_endpoint(&a, UINT32_C(4), (ucn_endpoint_t)0x47U,
                                       UCN_TRAFFIC_Q1_REALTIME, &payload_p1,
                                       1U) == UCN_OK);
    TEST_ASSERT(received.count == 1U && received.last_path_id == path_p1);
    TEST_ASSERT(ucn_node_find_q1_flow(&a, UINT32_C(4), (ucn_endpoint_t)0x47U) !=
                NULL &&
                ucn_node_find_q1_flow(&a, UINT32_C(4),
                                      (ucn_endpoint_t)0x47U)->local_path_id == 1U);
    TEST_ASSERT(ucn_node_send_endpoint(&a, UINT32_C(4), (ucn_endpoint_t)0x47U,
                                       UCN_TRAFFIC_Q1_REALTIME, &payload_p2,
                                       1U) == UCN_OK);
    TEST_ASSERT(received.count == 2U && received.last_path_id == path_p1);

    policy.key.endpoint = (ucn_endpoint_t)0x48U;
    TEST_ASSERT(ucn_node_set_route_policy(&a, &policy) == UCN_OK);
    TEST_ASSERT(ucn_node_send_endpoint(&a, UINT32_C(4), (ucn_endpoint_t)0x48U,
                                       UCN_TRAFFIC_Q1_REALTIME, &payload_p1,
                                       1U) == UCN_OK);
    TEST_ASSERT(received.count == 3U && received.last_path_id == path_p2);

    /* Three 500 ms quality samples above the threshold make P1 persistently
     * congested.  The existing P1 flow then rebinds once to P2; it does not
     * send the same frame on both Paths. */
    cab.metrics.queue_pressure_valid = true;
    cab.metrics.queue_pressure_per_mille = 1000U;
    cac.metrics.queue_pressure_valid = true;
    cac.metrics.queue_pressure_per_mille = 0U;
    TEST_ASSERT(path_step(&a, UCN_POLICY_QUALITY_SAMPLE_INTERVAL_MS) == 0);
    TEST_ASSERT(path_step(&a, UCN_POLICY_QUALITY_SAMPLE_INTERVAL_MS * 2U) == 0);
    TEST_ASSERT(path_step(&a, UCN_POLICY_QUALITY_SAMPLE_INTERVAL_MS * 3U) ==
                0);
    TEST_ASSERT(ucn_node_find_policy_path(&a, 1U)->congestion_samples ==
                UCN_POLICY_BALANCE_CONGESTED_SAMPLE_LIMIT);
    TEST_ASSERT(ucn_node_send_endpoint(&a, UINT32_C(4), (ucn_endpoint_t)0x47U,
                                       UCN_TRAFFIC_Q1_REALTIME, &payload_p1,
                                       1U) == UCN_OK);
    TEST_ASSERT(received.count == 4U && received.last_path_id == path_p2 &&
                ucn_node_find_q1_flow(&a, UINT32_C(4),
                                      (ucn_endpoint_t)0x47U)->local_path_id == 2U);

    /* One healthy sample clears the sustained-congestion state.  A new flow
     * sees P1's lower active-flow count and takes P1 before any failure. */
    cab.metrics.queue_pressure_per_mille = 0U;
    TEST_ASSERT(path_step(&a, UCN_POLICY_QUALITY_SAMPLE_INTERVAL_MS * 4U) ==
                0);
    TEST_ASSERT(ucn_node_find_policy_path(&a, 1U)->congestion_samples == 0U);
    policy.key.endpoint = (ucn_endpoint_t)0x49U;
    TEST_ASSERT(ucn_node_set_route_policy(&a, &policy) == UCN_OK);
    TEST_ASSERT(ucn_node_send_endpoint(&a, UINT32_C(4), (ucn_endpoint_t)0x49U,
                                       UCN_TRAFFIC_Q1_REALTIME, &payload_p1,
                                       1U) == UCN_OK);
    TEST_ASSERT(received.count == 5U && received.last_path_id == path_p1);

    (void)memset(&policy, 0, sizeof(policy));
    policy.key.destination = UINT32_C(4);
    policy.key.endpoint = (ucn_endpoint_t)0x42U;
    policy.key.traffic_class = (uint8_t)UCN_TRAFFIC_Q1_REALTIME;
    policy.mode = UCN_ROUTE_POLICY_PINNED_STRICT;
    policy.primary_local_path_id = 1U;
    policy.allow_discovery_on_hard_failure = true;
    TEST_ASSERT(ucn_node_set_route_policy(&a, &policy) == UCN_ERR_ARGUMENT);
    policy.allow_discovery_on_hard_failure = false;
    TEST_ASSERT(ucn_node_set_route_policy(&a, &policy) == UCN_OK);
    TEST_ASSERT(ucn_node_send_endpoint(&a, UINT32_C(4), (ucn_endpoint_t)0x42U,
                                       UCN_TRAFFIC_Q1_REALTIME, &payload_p1,
                                       1U) == UCN_OK);
    TEST_ASSERT(received.count == 6U && received.last_payload == payload_p1 &&
                received.last_message_type == 0x42U && received.last_has_path_id &&
                received.last_path_id == path_p1);
    TEST_ASSERT(cbd.last_has_path_id && cbd.last_protected &&
                cbd.last_path_id == path_p1);
    TEST_ASSERT(sb.open_calls == 0U && sc.open_calls == 0U && sd.open_calls == 6U);

    policy.key.endpoint = (ucn_endpoint_t)0x43U;
    policy.mode = UCN_ROUTE_POLICY_PINNED_FAILOVER;
    policy.primary_local_path_id = 1U;
    policy.backup_local_path_id = 2U;
    TEST_ASSERT(ucn_node_set_route_policy(&a, &policy) == UCN_OK);
    TEST_ASSERT(ucn_node_send_endpoint(&a, UINT32_C(4), (ucn_endpoint_t)0x43U,
                                       UCN_TRAFFIC_Q1_REALTIME, &payload_p1,
                                       1U) == UCN_OK);
    TEST_ASSERT(received.count == 7U && received.last_message_type == 0x43U &&
                received.last_path_id == path_p1 && sd.open_calls == 7U);

    policy.key.endpoint = (ucn_endpoint_t)0x44U;
    policy.key.traffic_class = (uint8_t)UCN_TRAFFIC_Q0_CRITICAL;
    TEST_ASSERT(ucn_node_set_route_policy(&a, &policy) == UCN_OK);

    /* A P1 egress failure removes only P1 and sends a Path-scoped RERR. */
    cbd.is_up = false;
    TEST_ASSERT(ucn_node_send_endpoint(&a, UINT32_C(4), (ucn_endpoint_t)0x42U,
                                       UCN_TRAFFIC_Q1_REALTIME, &payload_p1,
                                       1U) == UCN_ERR_LINK_DOWN);
    TEST_ASSERT(received.count == 7U);
    TEST_ASSERT(ucn_node_find_path_forward(&a, UINT32_C(1), sa.session_id,
                                            path_p1, UINT32_C(4)) == NULL);
    TEST_ASSERT(ucn_node_find_path_forward(&a, UINT32_C(1), sa.session_id,
                                            path_p2, UINT32_C(4)) != NULL);
    TEST_ASSERT(ucn_node_get_stats(&a)->path_route_errors_sent == 0U);
    TEST_ASSERT(ucn_node_get_stats(&b)->path_route_errors_sent == 1U);

    TEST_ASSERT(ucn_node_send_endpoint(&a, UINT32_C(4), (ucn_endpoint_t)0x43U,
                                       UCN_TRAFFIC_Q1_REALTIME, &payload_p2,
                                       1U) == UCN_OK);
    TEST_ASSERT(received.count == 8U && received.last_payload == payload_p2 &&
                received.last_path_id == path_p2);
    TEST_ASSERT(ccd.last_has_path_id && ccd.last_protected &&
                ccd.last_path_id == path_p2);
    TEST_ASSERT(sd.open_calls == 8U);

    /* Q0 has no discovery escape hatch, but it may use a pre-installed
     * verified Backup after the Primary has a hard failure. */
    TEST_ASSERT(ucn_node_send_endpoint(&a, UINT32_C(4), (ucn_endpoint_t)0x44U,
                                       UCN_TRAFFIC_Q0_CRITICAL, &payload_p2,
                                       1U) == UCN_OK);
    TEST_ASSERT(received.count == 9U && received.last_message_type == 0x44U &&
                received.last_path_id == path_p2 && sd.open_calls == 9U);

    /* A Flow currently bound to P1 detects the same hard failure and retries
     * only once through P2. */
    TEST_ASSERT(ucn_node_send_endpoint(&a, UINT32_C(4), (ucn_endpoint_t)0x49U,
                                       UCN_TRAFFIC_Q1_REALTIME, &payload_p2,
                                       1U) == UCN_OK);
    TEST_ASSERT(received.count == 10U && received.last_message_type == 0x49U &&
                received.last_path_id == path_p2 &&
                ucn_node_find_q1_flow(&a, UINT32_C(4),
                                      (ucn_endpoint_t)0x49U)->local_path_id == 2U);

    /* With no verified Backup, Q1 may use the policy's explicit discovery
     * escape hatch after a hard Path failure.  Clear the old static route so
     * the virtual RREQ must select the healthy A-C-D branch. */
    for (index = 0U; index < UCN_MAX_ROUTES; ++index) {
        a.routes[index].valid = false;
    }
    policy.key.endpoint = (ucn_endpoint_t)0x45U;
    policy.key.traffic_class = (uint8_t)UCN_TRAFFIC_Q1_REALTIME;
    policy.primary_local_path_id = 1U;
    policy.backup_local_path_id = 0U;
    policy.allow_discovery_on_hard_failure = true;
    TEST_ASSERT(ucn_node_set_route_policy(&a, &policy) == UCN_OK);
    TEST_ASSERT(ucn_node_send_endpoint(&a, UINT32_C(4), (ucn_endpoint_t)0x45U,
                                       UCN_TRAFFIC_Q1_REALTIME, &payload_p2,
                                       1U) == UCN_OK);
    TEST_ASSERT(received.count == 11U && received.last_message_type == 0x45U &&
                !received.last_has_path_id && sd.open_calls == 11U);

    /* Q0 may use an installed Backup, but never starts the discovery escape
     * hatch.  A missing Backup therefore returns the local hard failure. */
    policy.key.endpoint = (ucn_endpoint_t)0x46U;
    policy.key.traffic_class = (uint8_t)UCN_TRAFFIC_Q0_CRITICAL;
    TEST_ASSERT(ucn_node_set_route_policy(&a, &policy) == UCN_OK);
    TEST_ASSERT(ucn_node_send_endpoint(&a, UINT32_C(4), (ucn_endpoint_t)0x46U,
                                       UCN_TRAFFIC_Q0_CRITICAL, &payload_p2,
                                       1U) == UCN_ERR_LINK_DOWN);
    TEST_ASSERT(received.count == 11U);

    TEST_ASSERT(path_step(&a, UCN_POLICY_BALANCE_FLOW_LEASE_MS +
                                  UCN_POLICY_QUALITY_SAMPLE_INTERVAL_MS * 4U + 2U) ==
                0);
    TEST_ASSERT(ucn_node_find_q1_flow(&a, UINT32_C(4), (ucn_endpoint_t)0x47U) ==
                NULL);

    policy_stats = ucn_node_get_policy_stats(&a);
    TEST_ASSERT(policy_stats != NULL && policy_stats->pinned_strict_sends == 1U &&
                policy_stats->pinned_strict_failures == 1U &&
                policy_stats->pinned_failover_primary_sends == 1U &&
                policy_stats->pinned_failover_hard_failures == 4U &&
                policy_stats->pinned_failover_backup_sends == 2U &&
                policy_stats->pinned_failover_discovery_fallbacks == 1U &&
                policy_stats->auto_balance_sends == 6U &&
                policy_stats->auto_balance_flow_bindings == 3U &&
                policy_stats->auto_balance_rebindings == 2U &&
                policy_stats->auto_balance_congestion_rebindings == 1U &&
                policy_stats->auto_balance_down_rebindings == 1U &&
                policy_stats->auto_balance_selection_failures == 0U &&
                policy_stats->flow_bindings_expired == 3U);

    TEST_ASSERT(ucn_node_install_local_path(&a, path_p3, UINT32_C(4),
                                             UINT32_C(2), 1U) == UCN_OK);
    TEST_ASSERT(ucn_node_step(&a, UCN_POLICY_BALANCE_FLOW_LEASE_MS +
                                    UCN_POLICY_QUALITY_SAMPLE_INTERVAL_MS * 4U + 3U) ==
                UCN_ERR_NOT_FOUND);
    TEST_ASSERT(ucn_node_find_path_forward(&a, UINT32_C(1), sa.session_id,
                                            path_p3, UINT32_C(4)) == NULL);
    TEST_ASSERT(ucn_node_send_path_revoke(&a, UINT32_C(3), path_p2,
                                           UINT32_C(4)) == UCN_OK);
    TEST_ASSERT(ucn_node_find_path_forward(&c, UINT32_C(1), sa.session_id,
                                            path_p2, UINT32_C(4)) == NULL);
    return 0;
}
