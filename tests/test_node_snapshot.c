#include <string.h>

#include "test_support.h"
#include "ucn/ucn_frame.h"
#include "ucn/ucn_node.h"

typedef struct snapshot_link_context {
    ucn_node_t *peer;
    ucn_link_t *peer_ingress;
    bool deliver;
    uint32_t send_count;
    uint8_t last_message_type;
    uint8_t last_flags;
} snapshot_link_context_t;

typedef struct snapshot_callback_state {
    uint32_t count;
    ucn_node_snapshot_result_t result;
} snapshot_callback_state_t;

static ucn_result_t snapshot_link_send(ucn_link_t *link,
                                       const uint8_t *frame,
                                       size_t length)
{
    snapshot_link_context_t *context = (snapshot_link_context_t *)link->context;
    ucn_frame_t decoded;

    context->send_count++;
    if (ucn_frame_decode(frame, length, &decoded) == UCN_OK) {
        context->last_message_type = decoded.message_type;
        context->last_flags = decoded.flags;
    }
    if (!context->deliver) {
        return UCN_OK;
    }
    return ucn_node_receive(context->peer, context->peer_ingress, frame, length);
}

static ucn_result_t snapshot_link_status(const ucn_link_t *link,
                                         ucn_link_status_t *status)
{
    status->is_up = true;
    status->mtu = link->mtu;
    status->tx_errors = 0U;
    status->rx_errors = 0U;
    return UCN_OK;
}

static const ucn_link_ops_t SNAPSHOT_LINK_OPS = {
    NULL, snapshot_link_send, NULL, snapshot_link_status, NULL, NULL
};

static int snapshot_init_node(ucn_node_t *node, ucn_node_id_t node_id)
{
    ucn_config_t config;

    config.network_id = UINT32_C(0x534E4150);
    config.node_id = node_id;
    config.default_hop_limit = 5U;
    return ucn_node_init(node, &config) == UCN_OK ? 0 : 1;
}

static void snapshot_setup_link(ucn_link_t *link,
                                snapshot_link_context_t *context,
                                uint8_t link_id,
                                ucn_node_id_t peer_node_id)
{
    link->ops = &SNAPSHOT_LINK_OPS;
    link->context = context;
    link->link_id = link_id;
    link->mtu = UCN_MAX_FRAME_BYTES;
    link->peer_node_id = peer_node_id;
    context->deliver = true;
}

static bool snapshot_allow_origin(void *context, ucn_node_id_t requester)
{
    return requester == *(const ucn_node_id_t *)context;
}

static void snapshot_callback(void *context,
                              const ucn_node_snapshot_result_t *result)
{
    snapshot_callback_state_t *state = (snapshot_callback_state_t *)context;

    state->count++;
    state->result = *result;
}

static const ucn_node_snapshot_entry_t *snapshot_find_entry(
    const ucn_node_snapshot_result_t *result,
    ucn_node_id_t node_id)
{
    size_t index;

    for (index = 0U; index < result->node_count; ++index) {
        if (result->entries[index].node_id == node_id) {
            return &result->entries[index];
        }
    }
    return NULL;
}

static void snapshot_run_replies(ucn_node_t *a,
                                 ucn_node_t *b,
                                 ucn_node_t *c,
                                 ucn_node_t *d,
                                 uint32_t now_ms)
{
    (void)ucn_node_step(b, now_ms);
    (void)ucn_node_step(c, now_ms);
    (void)ucn_node_step(d, now_ms);
    (void)ucn_node_step(a, now_ms + UCN_NODE_SNAPSHOT_TIMEOUT_MS);
}

static int snapshot_test_collect_and_truncate(void)
{
    const ucn_node_id_t origin = UINT32_C(1);
    ucn_node_t a, b, c, d;
    ucn_link_t ab, ba, bc, cb, bd, db;
    snapshot_link_context_t cab, cba, cbc, ccb, cbd, cdb;
    snapshot_callback_state_t complete, truncated;
    const ucn_node_snapshot_entry_t *entry;

    (void)memset(&a, 0, sizeof(a)); (void)memset(&b, 0, sizeof(b));
    (void)memset(&c, 0, sizeof(c)); (void)memset(&d, 0, sizeof(d));
    (void)memset(&ab, 0, sizeof(ab)); (void)memset(&ba, 0, sizeof(ba));
    (void)memset(&bc, 0, sizeof(bc)); (void)memset(&cb, 0, sizeof(cb));
    (void)memset(&bd, 0, sizeof(bd)); (void)memset(&db, 0, sizeof(db));
    (void)memset(&cab, 0, sizeof(cab)); (void)memset(&cba, 0, sizeof(cba));
    (void)memset(&cbc, 0, sizeof(cbc)); (void)memset(&ccb, 0, sizeof(ccb));
    (void)memset(&cbd, 0, sizeof(cbd)); (void)memset(&cdb, 0, sizeof(cdb));
    (void)memset(&complete, 0, sizeof(complete));
    (void)memset(&truncated, 0, sizeof(truncated));

    TEST_ASSERT(snapshot_init_node(&a, origin) == 0);
    TEST_ASSERT(snapshot_init_node(&b, UINT32_C(2)) == 0);
    TEST_ASSERT(snapshot_init_node(&c, UINT32_C(3)) == 0);
    TEST_ASSERT(snapshot_init_node(&d, UINT32_C(4)) == 0);
    snapshot_setup_link(&ab, &cab, 1U, UINT32_C(2));
    snapshot_setup_link(&ba, &cba, 2U, origin);
    snapshot_setup_link(&bc, &cbc, 3U, UINT32_C(3));
    snapshot_setup_link(&cb, &ccb, 4U, UINT32_C(2));
    snapshot_setup_link(&bd, &cbd, 5U, UINT32_C(4));
    snapshot_setup_link(&db, &cdb, 6U, UINT32_C(2));
    cab.peer = &b; cab.peer_ingress = &ba;
    cba.peer = &a; cba.peer_ingress = &ab;
    cbc.peer = &c; cbc.peer_ingress = &cb;
    ccb.peer = &b; ccb.peer_ingress = &bc;
    cbd.peer = &d; cbd.peer_ingress = &db;
    cdb.peer = &b; cdb.peer_ingress = &bd;
    TEST_ASSERT(ucn_node_register_link(&a, &ab) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&b, &ba) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&b, &bc) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&b, &bd) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&c, &cb) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&d, &db) == UCN_OK);
    TEST_ASSERT(ucn_node_set_node_snapshot_authorizer(&b, snapshot_allow_origin,
                                                       (void *)&origin) == UCN_OK);
    TEST_ASSERT(ucn_node_set_node_snapshot_authorizer(&c, snapshot_allow_origin,
                                                       (void *)&origin) == UCN_OK);
    TEST_ASSERT(ucn_node_set_node_snapshot_authorizer(&d, snapshot_allow_origin,
                                                       (void *)&origin) == UCN_OK);

    TEST_ASSERT(ucn_node_request_node_snapshot(&a, 0U, snapshot_callback,
                                                &complete) == UCN_OK);
    TEST_ASSERT(cab.last_message_type == UCN_MSG_NODE_SNAPSHOT_REQ &&
                cab.last_flags == UCN_FRAME_FLAG_DIAGNOSTIC &&
                cbc.last_message_type == UCN_MSG_NODE_SNAPSHOT_REQ &&
                cbd.last_message_type == UCN_MSG_NODE_SNAPSHOT_REQ);
    snapshot_run_replies(&a, &b, &c, &d, UCN_NODE_SNAPSHOT_REPLY_JITTER_MS);
    TEST_ASSERT(complete.count == 1U &&
                complete.result.status == UCN_NODE_SNAPSHOT_STATUS_COMPLETE &&
                complete.result.node_count == 4U);
    entry = snapshot_find_entry(&complete.result, origin);
    TEST_ASSERT(entry != NULL && entry->direct_link_count == 1U);
    entry = snapshot_find_entry(&complete.result, UINT32_C(2));
    TEST_ASSERT(entry != NULL && entry->direct_link_count == 3U);
    entry = snapshot_find_entry(&complete.result, UINT32_C(3));
    TEST_ASSERT(entry != NULL && entry->direct_link_count == 1U);
    entry = snapshot_find_entry(&complete.result, UINT32_C(4));
    TEST_ASSERT(entry != NULL && entry->direct_link_count == 1U);
    TEST_ASSERT(a.stats.node_snapshot_requests_sent == 1U &&
                a.stats.node_snapshot_replies_received == 3U &&
                b.stats.node_snapshot_requests_received == 1U &&
                c.stats.node_snapshot_replies_sent == 1U &&
                d.stats.node_snapshot_replies_sent == 1U);

    (void)ucn_node_step(&b, UCN_NODE_SNAPSHOT_TOKEN_REFILL_MS);
    (void)ucn_node_step(&c, UCN_NODE_SNAPSHOT_TOKEN_REFILL_MS);
    (void)ucn_node_step(&d, UCN_NODE_SNAPSHOT_TOKEN_REFILL_MS);
    (void)ucn_node_step(&a, UCN_NODE_SNAPSHOT_TOKEN_REFILL_MS);
    TEST_ASSERT(ucn_node_request_node_snapshot(&a, 2U, snapshot_callback,
                                                &truncated) == UCN_OK);
    snapshot_run_replies(&a, &b, &c, &d,
                         UCN_NODE_SNAPSHOT_TOKEN_REFILL_MS +
                         UCN_NODE_SNAPSHOT_REPLY_JITTER_MS);
    TEST_ASSERT(truncated.count == 1U &&
                truncated.result.status == UCN_NODE_SNAPSHOT_STATUS_TRUNCATED &&
                truncated.result.node_count == 2U &&
                a.stats.node_snapshot_result_truncated == 1U);
    return 0;
}

static int snapshot_test_default_deny(void)
{
    ucn_node_t a, b;
    ucn_link_t ab, ba;
    snapshot_link_context_t cab, cba;
    snapshot_callback_state_t result;

    (void)memset(&a, 0, sizeof(a)); (void)memset(&b, 0, sizeof(b));
    (void)memset(&ab, 0, sizeof(ab)); (void)memset(&ba, 0, sizeof(ba));
    (void)memset(&cab, 0, sizeof(cab)); (void)memset(&cba, 0, sizeof(cba));
    (void)memset(&result, 0, sizeof(result));
    TEST_ASSERT(snapshot_init_node(&a, UINT32_C(11)) == 0);
    TEST_ASSERT(snapshot_init_node(&b, UINT32_C(12)) == 0);
    snapshot_setup_link(&ab, &cab, 11U, UINT32_C(12));
    snapshot_setup_link(&ba, &cba, 12U, UINT32_C(11));
    cab.peer = &b; cab.peer_ingress = &ba;
    cba.peer = &a; cba.peer_ingress = &ab;
    TEST_ASSERT(ucn_node_register_link(&a, &ab) == UCN_OK);
    TEST_ASSERT(ucn_node_register_link(&b, &ba) == UCN_OK);

    TEST_ASSERT(ucn_node_request_node_snapshot(&a, 0U, snapshot_callback,
                                                &result) == UCN_ERR_ACCESS);
    TEST_ASSERT(result.count == 0U && b.stats.node_snapshot_rejected == 1U);
    return 0;
}

int test_node_snapshot(void)
{
    int result = 0;

    result |= snapshot_test_collect_and_truncate();
    result |= snapshot_test_default_deny();
    return result;
}
