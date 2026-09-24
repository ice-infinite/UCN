#include "internal/ucn_route.h"

#include "rust/tests/conformance/v6s_routing_flow_v1.h"

#include <stdio.h>
#include <string.h>

#define CHECK(expr_)                                                         \
    do {                                                                     \
        if (!(expr_)) {                                                      \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,       \
                    __LINE__, #expr_);                                       \
            return 1;                                                        \
        }                                                                    \
    } while (0)

typedef struct test_lock {
    uint8_t held;
} test_lock_t;

static ucn_result_t lock_enter(void *context)
{
    test_lock_t *lock = context;

    if (lock == NULL || lock->held != 0U) {
        return UCN_ERR_STATE;
    }
    lock->held = 1U;
    return UCN_OK;
}

static void lock_leave(void *context)
{
    test_lock_t *lock = context;
    lock->held = 0U;
}

static ucn_i_lock_ops_t lock_ops(test_lock_t *lock)
{
    ucn_i_lock_ops_t ops;

    memset(&ops, 0, sizeof(ops));
    ops.struct_size = sizeof(ops);
    ops.api_version = UCN_I_LOCK_OPS_VERSION;
    ops.context = lock;
    ops.enter = lock_enter;
    ops.leave = lock_leave;
    return ops;
}

static ucn_i_route_binding_t binding(uint32_t address, uint8_t byte)
{
    ucn_i_route_binding_t value;

    memset(&value, 0, sizeof(value));
    value.address = address;
    value.generation = 1U;
    memset(value.principal, byte, sizeof(value.principal));
    return value;
}

static ucn_i_route_link_ref_t link_ref(uint16_t id,
                                       uint32_t generation,
                                       ucn_i_route_binding_t peer,
                                       uint32_t cost)
{
    ucn_i_route_link_ref_t value;

    memset(&value, 0, sizeof(value));
    value.peer = peer;
    value.link_generation = generation;
    value.cost = cost;
    value.link_id = id;
    value.frame_mtu = 512U;
    value.capability_bits = UINT16_C(0x0003);
    return value;
}

static ucn_i_route_config_t config(uint32_t runtime,
                                   uint16_t owner,
                                   ucn_i_route_binding_t local)
{
    ucn_i_route_config_t value;

    memset(&value, 0, sizeof(value));
    value.local = local;
    value.discovery_lifetime_us = 1000U;
    value.discovery_retry_us = 100U;
    value.reverse_lifetime_us = 1200U;
    value.route_lifetime_us = 2000U;
    value.first_transaction_id = 100U;
    value.runtime_instance = runtime;
    value.realm = 7U;
    value.local_session_generation = 3U;
    value.owner_instance = owner;
    value.address_width = 2U;
    value.discovery_max_attempts = 3U;
    value.maximum_hops = 8U;
    return value;
}

static int test_codec_golden_and_zero_write(void)
{
    const uint8_t rreq_golden[] = { UCN_V6S_ROUTE_RREQ_BYTES };
    const uint8_t rrep_golden[] = { UCN_V6S_ROUTE_RREP_BYTES };
    const uint8_t rerr_golden[] = { UCN_V6S_ROUTE_RERR_BYTES };
    uint8_t bytes[UCN_I_ROUTE_RERR_BYTES];
    uint8_t sentinel[UCN_I_ROUTE_RERR_BYTES];
    ucn_i_route_rreq_payload_t rreq;
    ucn_i_route_rrep_payload_t rrep;
    ucn_i_route_rerr_payload_t rerr;

    memset(&rreq, 0, sizeof(rreq));
    rreq.accumulated_cost = UINT32_C(0x01020304);
    rreq.minimum_payload_budget = UINT16_C(0x1122);
    rreq.required_capability_bits = UINT16_C(0x3344);
    rreq.flags = 3U;
    CHECK(ucn_i_route_rreq_encode(&rreq, bytes) == UCN_OK);
    CHECK(memcmp(bytes, rreq_golden, sizeof(rreq_golden)) == 0);
    memset(&rreq, 0, sizeof(rreq));
    CHECK(ucn_i_route_rreq_decode(rreq_golden, &rreq) == UCN_OK);
    CHECK(rreq.accumulated_cost == UINT32_C(0x01020304));

    memset(&rrep, 0, sizeof(rrep));
    memset(rrep.destination_principal, 0xA5, 16U);
    rrep.destination_binding_generation = UINT32_C(0x01020304);
    rrep.hop_count = 0x00U;
    rrep.accumulated_cost = UINT32_C(0x11223344);
    rrep.path_frame_mtu = UINT16_C(0x5566);
    rrep.capability_bits = UINT16_C(0x7788);
    CHECK(ucn_i_route_rrep_encode(&rrep, bytes) == UCN_OK);
    CHECK(memcmp(bytes, rrep_golden, sizeof(rrep_golden)) == 0);
    memset(&rrep, 0, sizeof(rrep));
    CHECK(ucn_i_route_rrep_decode(rrep_golden, &rrep) == UCN_OK);
    CHECK(rrep.path_frame_mtu == UINT16_C(0x5566));

    memset(&rerr, 0, sizeof(rerr));
    rerr.realm = UINT32_C(0x01020304);
    memset(rerr.origin_principal, 0x11, 16U);
    rerr.origin_address = UINT32_C(0x11223344);
    rerr.origin_binding_generation = UINT32_C(0x01020304);
    rerr.origin_session_generation = UINT32_C(0x05060708);
    memset(rerr.destination_principal, 0x22, 16U);
    rerr.destination_address = UINT32_C(0x55667788);
    rerr.destination_binding_generation = UINT32_C(0x11121314);
    rerr.route_generation = UINT32_C(0x21222324);
    rerr.route_causal_id = UINT64_C(0x3132333435363738);
    memset(rerr.reporter_principal, 0x33, 16U);
    rerr.reporter_address = UINT32_C(0x99AABBCC);
    rerr.reporter_binding_generation = UINT32_C(0x41424344);
    rerr.failed_link_id = UINT16_C(0x5152);
    rerr.failed_link_generation = UINT32_C(0x61626364);
    rerr.reason = UCN_I_ROUTE_RERR_PATH_CONTRACT_CHANGED;
    CHECK(ucn_i_route_rerr_encode(&rerr, bytes) == UCN_OK);
    CHECK(memcmp(bytes, rerr_golden, sizeof(rerr_golden)) == 0);
    memset(&rerr, 0, sizeof(rerr));
    CHECK(ucn_i_route_rerr_decode(rerr_golden, &rerr) == UCN_OK);
    CHECK(rerr.route_causal_id == UINT64_C(0x3132333435363738));

    memset(sentinel, 0x5A, sizeof(sentinel));
    memcpy(bytes, sentinel, sizeof(bytes));
    memset(&rerr, 0, sizeof(rerr));
    rerr.reason = 9U;
    CHECK(ucn_i_route_rerr_encode(&rerr, bytes) == UCN_ERR_ARGUMENT);
    CHECK(memcmp(bytes, sentinel, sizeof(bytes)) == 0);
    return 0;
}

static int test_three_node_route_and_exact_rerr(void)
{
    ucn_i_route_owner_t a;
    ucn_i_route_owner_t b;
    ucn_i_route_owner_t c;
    test_lock_t lock_a = {0};
    test_lock_t lock_b = {0};
    test_lock_t lock_c = {0};
    ucn_i_lock_ops_t ops_a = lock_ops(&lock_a);
    ucn_i_lock_ops_t ops_b = lock_ops(&lock_b);
    ucn_i_lock_ops_t ops_c = lock_ops(&lock_c);
    ucn_i_route_binding_t bind_a = binding(1U, 0xA1U);
    ucn_i_route_binding_t bind_b = binding(2U, 0xB2U);
    ucn_i_route_binding_t bind_c = binding(3U, 0xC3U);
    ucn_i_route_config_t cfg_a = config(1U, 11U, bind_a);
    ucn_i_route_config_t cfg_b = config(2U, 12U, bind_b);
    ucn_i_route_config_t cfg_c = config(3U, 13U, bind_c);
    ucn_i_route_link_ref_t b_from_a = link_ref(1U, 10U, bind_a, 5U);
    ucn_i_route_link_ref_t c_from_b = link_ref(2U, 20U, bind_b, 7U);
    ucn_i_route_link_ref_t b_from_c = link_ref(2U, 21U, bind_c, 7U);
    ucn_i_route_link_ref_t a_from_b = link_ref(1U, 11U, bind_b, 5U);
    ucn_i_route_rreq_message_t request;
    ucn_i_route_rreq_message_t mutated_request;
    ucn_i_route_rrep_message_t reply;
    ucn_i_route_request_action_t request_action;
    ucn_i_route_reply_action_t reply_action;
    ucn_i_route_domain_t domain;
    ucn_i_route_use_facts_t facts;
    ucn_i_route_resolved_t resolved;
    ucn_i_route_rerr_payload_t rerr;
    ucn_i_route_static_entry_t static_route;
    ucn_handle_t discovery;
    uint16_t invalidated_count;
    uint8_t invalidated;
    uint8_t created;

    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    memset(&c, 0, sizeof(c));
    CHECK(ucn_i_route_owner_init(&a, &cfg_a, &ops_a) == UCN_OK);
    CHECK(ucn_i_route_owner_init(&b, &cfg_b, &ops_b) == UCN_OK);
    CHECK(ucn_i_route_owner_init(&c, &cfg_c, &ops_c) == UCN_OK);
    CHECK(ucn_i_route_ensure_discovery(&a, bind_c.address, 1U, 32U,
                                       0U, 10U, &discovery, &request,
                                       &created) == UCN_OK);
    CHECK(created == 1U);
    CHECK(ucn_i_route_on_rreq(&b, &request, &b_from_a, 20U,
                              &request_action) == UCN_OK);
    CHECK(request_action.kind == UCN_I_ROUTE_REQUEST_FORWARD);
    CHECK(request_action.forwarded.remaining_hops == 7U);
    request = request_action.forwarded;
    CHECK(ucn_i_route_on_rreq(&c, &request, &c_from_b, 30U,
                              &request_action) == UCN_OK);
    CHECK(request_action.kind == UCN_I_ROUTE_REQUEST_LOCAL_TARGET);
    mutated_request = request;
    mutated_request.payload.flags ^= 1U;
    CHECK(ucn_i_route_make_rrep(&c, &mutated_request, 3U, 512U, 31U,
                                &reply) == UCN_ERR_SECURITY);
    CHECK(ucn_i_route_make_rrep(&c, &request, 3U, 512U, 31U,
                                &reply) == UCN_OK);
    CHECK(ucn_i_route_on_rrep(&b, &reply, &b_from_c, 40U,
                              &reply_action) == UCN_OK);
    CHECK(reply_action.kind == UCN_I_ROUTE_REPLY_FORWARD);
    CHECK(ucn_i_route_complete_rrep_forward(&b, reply_action.completion,
                                            1U) == UCN_OK);
    reply = reply_action.forwarded;
    CHECK(ucn_i_route_on_rrep(&a, &reply, &a_from_b, 50U,
                              &reply_action) == UCN_OK);
    CHECK(reply_action.kind == UCN_I_ROUTE_REPLY_REACHED_ORIGIN);
    CHECK(reply_action.installed.hop_count == 2U);
    CHECK(reply_action.installed.cost == 12U);

    memset(&domain, 0, sizeof(domain));
    domain.origin = bind_a;
    domain.destination = bind_c;
    domain.realm = 7U;
    domain.origin_session_generation = 3U;
    memset(&facts, 0, sizeof(facts));
    facts.now_us = 100U;
    facts.origin_session_generation = 3U;
    facts.destination_binding_generation = bind_c.generation;
    facts.link_generation = a_from_b.link_generation;
    CHECK(ucn_i_route_resolve(&a, &domain, &facts, &resolved) == UCN_OK);
    CHECK(resolved.kind == UCN_I_ROUTE_RESOLVED_DYNAMIC);
    CHECK(resolved.dynamic.next_hop.link_id == 1U);

    CHECK(ucn_i_route_make_rerr(&a, &resolved.dynamic,
                                UCN_I_ROUTE_RERR_LINK_INVALID,
                                &rerr) == UCN_OK);
    rerr.route_causal_id++;
    CHECK(ucn_i_route_on_rerr(&a, &rerr, &invalidated) == UCN_OK);
    CHECK(invalidated == 0U);
    rerr.route_causal_id--;
    CHECK(ucn_i_route_on_rerr(&a, &rerr, &invalidated) == UCN_OK);
    CHECK(invalidated == 1U);

    memset(&static_route, 0, sizeof(static_route));
    static_route.destination = bind_c;
    static_route.next_hop = a_from_b;
    static_route.path_frame_mtu = 256U;
    CHECK(ucn_i_route_install_static(&a, &static_route) == UCN_OK);
    CHECK(ucn_i_route_resolve(&a, &domain, &facts, &resolved) == UCN_OK);
    CHECK(resolved.kind == UCN_I_ROUTE_RESOLVED_STATIC);
    CHECK(ucn_i_route_invalidate_link(&a, a_from_b.link_id,
                                      a_from_b.link_generation,
                                      &invalidated_count) == UCN_OK);
    CHECK(invalidated_count == 0U);
    CHECK(ucn_i_route_resolve(&a, &domain, &facts, &resolved) == UCN_OK);
    CHECK(resolved.kind == UCN_I_ROUTE_RESOLVED_STATIC);
    return 0;
}

static int test_capacity_and_deadline_are_fail_closed(void)
{
    ucn_i_route_owner_t owner;
    test_lock_t lock = {0};
    ucn_i_lock_ops_t ops = lock_ops(&lock);
    ucn_i_route_binding_t local = binding(1U, 0xA1U);
    ucn_i_route_config_t cfg = config(10U, 20U, local);
    ucn_handle_t handles[UCN_I_ROUTE_DISCOVERY_COUNT];
    ucn_i_route_rreq_message_t request;
    ucn_i_route_rreq_message_t retry;
    ucn_i_route_rrep_message_t late_reply;
    ucn_i_route_reply_action_t late_action;
    ucn_i_route_link_ref_t late_ingress;
    uint16_t before;
    uint16_t reverse;
    uint16_t routes;
    uint16_t statics;
    uint16_t inspected;
    uint16_t expired;
    uint16_t cursor_before;
    uint8_t created;
    ucn_handle_t expired_handle;
    ucn_handle_t replacement;
    size_t index;

    memset(&owner, 0, sizeof(owner));
    CHECK(ucn_i_route_owner_init(&owner, &cfg, &ops) == UCN_OK);
    for (index = 0U; index < UCN_I_ROUTE_DISCOVERY_COUNT; ++index) {
        CHECK(ucn_i_route_ensure_discovery(
                  &owner, (uint32_t)(index + 2U), 0U, 1U, 0U, 10U,
                  &handles[index], &request, &created) == UCN_OK);
    }
    CHECK(ucn_i_route_counts(&owner, &before, &reverse, &routes,
                             &statics) == UCN_OK);
    CHECK(before == UCN_I_ROUTE_DISCOVERY_COUNT);
    CHECK(ucn_i_route_ensure_discovery(
              &owner, 100U, 0U, 1U, 0U, 10U, &handles[0], &request,
              &created) == UCN_ERR_NO_SPACE);
    CHECK(ucn_i_route_retry_discovery(&owner, handles[1], 109U,
                                      &retry) == UCN_ERR_STATE);
    CHECK(ucn_i_route_retry_discovery(&owner, handles[1], 110U,
                                      &retry) == UCN_OK);
    CHECK(retry.key.transaction_id == UINT64_C(101));
    expired_handle = handles[0];
    CHECK(ucn_i_route_maintain(&owner, 1010U,
                               UCN_I_ROUTE_DISCOVERY_COUNT,
                               &inspected, &expired) == UCN_OK);
    CHECK(expired == UCN_I_ROUTE_DISCOVERY_COUNT);
    CHECK(ucn_i_route_ensure_discovery(
              &owner, 100U, 0U, 1U, 0U, 1020U, &replacement, &request,
              &created) == UCN_OK);
    CHECK(created == 1U);
    CHECK(replacement.slot == expired_handle.slot);
    CHECK(replacement.generation != expired_handle.generation);
    CHECK(ucn_i_route_retry_discovery(&owner, expired_handle, 1120U,
                                      &retry) == UCN_ERR_NOT_FOUND);
    memset(&late_reply, 0, sizeof(late_reply));
    late_reply.key = request.key;
    memset(late_reply.payload.destination_principal, 0x77,
           sizeof(late_reply.payload.destination_principal));
    late_reply.payload.destination_binding_generation = 1U;
    late_reply.payload.path_frame_mtu = 512U;
    late_reply.payload.capability_bits = 0U;
    late_ingress = link_ref(1U, 1U, binding(2U, 0x22U), 1U);
    CHECK(ucn_i_route_on_rrep(&owner, &late_reply, &late_ingress, 2020U,
                              &late_action) == UCN_ERR_TIMEOUT);
    cursor_before = owner.maintenance_cursor;
    CHECK(ucn_i_route_counts(&owner, &owner.maintenance_cursor, &reverse,
                             &routes, &statics) == UCN_ERR_ARGUMENT);
    CHECK(owner.maintenance_cursor == cursor_before);
    return 0;
}

int main(void)
{
    CHECK(test_codec_golden_and_zero_write() == 0);
    CHECK(test_three_node_route_and_exact_rerr() == 0);
    CHECK(test_capacity_and_deadline_are_fail_closed() == 0);
    puts("ucn_v6s_route_tests: PASS");
    return 0;
}
