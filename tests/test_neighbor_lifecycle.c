#include <string.h>

#include "test_support.h"
#include "ucn/ucn_node.h"

typedef struct neighbor_link_context {
    bool is_up;
    uint32_t open_count;
} neighbor_link_context_t;

typedef struct neighbor_provider_context {
    ucn_node_id_t allowed_peer;
    uint32_t calls;
} neighbor_provider_context_t;

static ucn_result_t neighbor_link_open(ucn_link_t *link)
{
    neighbor_link_context_t *context = (neighbor_link_context_t *)link->context;
    context->open_count++;
    return UCN_OK;
}

static ucn_result_t neighbor_link_send(ucn_link_t *link,
                                       const uint8_t *frame,
                                       size_t length)
{
    (void)link;
    (void)frame;
    (void)length;
    return UCN_OK;
}

static ucn_result_t neighbor_link_status(const ucn_link_t *link,
                                         ucn_link_status_t *status)
{
    const neighbor_link_context_t *context = (const neighbor_link_context_t *)link->context;
    status->is_up = context->is_up;
    status->mtu = link->mtu;
    status->tx_errors = 0U;
    status->rx_errors = 0U;
    return UCN_OK;
}

static const ucn_link_ops_t NEIGHBOR_LINK_OPS = {
    neighbor_link_open, neighbor_link_send, NULL, neighbor_link_status, NULL, NULL
};

static ucn_result_t neighbor_authorize(void *context,
                                       ucn_node_id_t local_node_id,
                                       ucn_node_id_t peer_node_id,
                                       const ucn_link_t *link)
{
    neighbor_provider_context_t *provider = (neighbor_provider_context_t *)context;

    (void)local_node_id;
    (void)link;
    provider->calls++;
    return peer_node_id == provider->allowed_peer ? UCN_OK : UCN_ERR_ACCESS;
}

static int neighbor_init_node(ucn_node_t *node, ucn_node_id_t id)
{
    ucn_config_t config;

    config.network_id = UINT32_C(0x13572468);
    config.node_id = id;
    config.default_hop_limit = 3U;
    return ucn_node_init(node, &config) == UCN_OK ? 0 : 1;
}

static void neighbor_setup_link(ucn_link_t *link,
                                neighbor_link_context_t *context,
                                uint8_t link_id,
                                ucn_node_id_t peer_node_id)
{
    link->ops = &NEIGHBOR_LINK_OPS;
    link->context = context;
    link->link_id = link_id;
    link->mtu = UCN_MAX_FRAME_BYTES;
    link->peer_node_id = peer_node_id;
    context->is_up = true;
}

int test_neighbor_lifecycle(void)
{
    ucn_node_t manual_node, provider_node, capacity_node;
    ucn_link_t manual_link, provider_denied_link, provider_allowed_link;
    ucn_link_t candidate_links[UCN_MAX_NEIGHBORS + 1U];
    neighbor_link_context_t manual_context, denied_context, allowed_context;
    neighbor_link_context_t candidate_contexts[UCN_MAX_NEIGHBORS + 1U];
    neighbor_provider_context_t provider_context;
    size_t index;

    (void)memset(&manual_node, 0, sizeof(manual_node));
    (void)memset(&provider_node, 0, sizeof(provider_node));
    (void)memset(&capacity_node, 0, sizeof(capacity_node));
    (void)memset(&manual_link, 0, sizeof(manual_link));
    (void)memset(&provider_denied_link, 0, sizeof(provider_denied_link));
    (void)memset(&provider_allowed_link, 0, sizeof(provider_allowed_link));
    (void)memset(candidate_links, 0, sizeof(candidate_links));
    (void)memset(&manual_context, 0, sizeof(manual_context));
    (void)memset(&denied_context, 0, sizeof(denied_context));
    (void)memset(&allowed_context, 0, sizeof(allowed_context));
    (void)memset(candidate_contexts, 0, sizeof(candidate_contexts));
    (void)memset(&provider_context, 0, sizeof(provider_context));
    TEST_ASSERT(neighbor_init_node(&manual_node, UINT32_C(1)) == 0);
    neighbor_setup_link(&manual_link, &manual_context, 1U, UINT32_C(2));
    TEST_ASSERT(ucn_node_observe_neighbor(&manual_node, &manual_link, 10U) == UCN_OK);
    TEST_ASSERT(ucn_node_neighbor_count(&manual_node, UCN_NEIGHBOR_CANDIDATE) == 1U);
    TEST_ASSERT(manual_node.link_count == 0U);
    TEST_ASSERT(ucn_node_observe_neighbor(&manual_node, &manual_link, 11U) == UCN_OK);
    TEST_ASSERT(ucn_node_neighbor_count(&manual_node, UCN_NEIGHBOR_CANDIDATE) == 1U);
    TEST_ASSERT(ucn_node_reject_neighbor(&manual_node, UINT32_C(2)) == UCN_OK);
    TEST_ASSERT(ucn_node_neighbor_count(&manual_node, UCN_NEIGHBOR_REJECTED) == 1U);
    TEST_ASSERT(ucn_node_admit_neighbor(&manual_node, UINT32_C(2)) == UCN_ERR_NOT_FOUND);
    TEST_ASSERT(ucn_node_observe_neighbor(&manual_node, &manual_link, 12U) == UCN_OK);
    TEST_ASSERT(ucn_node_admit_neighbor(&manual_node, UINT32_C(2)) == UCN_OK);
    TEST_ASSERT(ucn_node_neighbor_count(&manual_node, UCN_NEIGHBOR_ADMITTED) == 1U);
    TEST_ASSERT(manual_node.link_count == 1U);
    TEST_ASSERT(manual_context.open_count == 1U);

    TEST_ASSERT(neighbor_init_node(&provider_node, UINT32_C(10)) == 0);
    TEST_ASSERT(ucn_node_set_join_policy(&provider_node, UCN_JOIN_PROVIDER,
                                         NULL, NULL) == UCN_ERR_ARGUMENT);
    TEST_ASSERT(ucn_node_set_join_policy(&provider_node, UCN_JOIN_OPEN,
                                         neighbor_authorize, &provider_context) == UCN_ERR_ARGUMENT);
    provider_context.allowed_peer = UINT32_C(12);
    TEST_ASSERT(ucn_node_set_join_policy(&provider_node, UCN_JOIN_PROVIDER,
                                         neighbor_authorize, &provider_context) == UCN_OK);
    neighbor_setup_link(&provider_denied_link, &denied_context, 2U, UINT32_C(11));
    neighbor_setup_link(&provider_allowed_link, &allowed_context, 3U, UINT32_C(12));
    TEST_ASSERT(ucn_node_observe_neighbor(&provider_node, &provider_denied_link, 20U) == UCN_ERR_ACCESS);
    TEST_ASSERT(ucn_node_neighbor_count(&provider_node, UCN_NEIGHBOR_REJECTED) == 1U);
    TEST_ASSERT(ucn_node_observe_neighbor(&provider_node, &provider_allowed_link, 21U) == UCN_OK);
    TEST_ASSERT(ucn_node_neighbor_count(&provider_node, UCN_NEIGHBOR_ADMITTED) == 1U);
    TEST_ASSERT(provider_node.link_count == 1U);
    TEST_ASSERT(provider_context.calls == 2U);

    TEST_ASSERT(neighbor_init_node(&capacity_node, UINT32_C(20)) == 0);
    for (index = 0U; index < UCN_MAX_NEIGHBORS; ++index) {
        neighbor_setup_link(&candidate_links[index], &candidate_contexts[index],
                            (uint8_t)(10U + index), (ucn_node_id_t)(30U + index));
        TEST_ASSERT(ucn_node_observe_neighbor(&capacity_node, &candidate_links[index], 100U) == UCN_OK);
    }
    neighbor_setup_link(&candidate_links[UCN_MAX_NEIGHBORS],
                        &candidate_contexts[UCN_MAX_NEIGHBORS], 30U, UINT32_C(99));
    TEST_ASSERT(ucn_node_observe_neighbor(&capacity_node,
                                          &candidate_links[UCN_MAX_NEIGHBORS], 100U) == UCN_ERR_NO_SPACE);
    TEST_ASSERT(ucn_node_step(&capacity_node,
                              100U + UCN_NEIGHBOR_CANDIDATE_TIMEOUT_MS) == UCN_ERR_NOT_FOUND);
    TEST_ASSERT(ucn_node_neighbor_count(&capacity_node, UCN_NEIGHBOR_EXPIRED) == UCN_MAX_NEIGHBORS);
    return 0;
}
