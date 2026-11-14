/* Host-only crypto and persistence-proof fixture; not production algorithms. */
#define main security_regression_main
#include "test_security_session.c"
#undef main
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include "internal/ucn_native_routed.h"

/* Initial RED: independent Hop SPI must exist; it must not use Origin keys. */
ucn_result_t ucn_i_security_c1_hop_wrap(ucn_i_security_owner_t *,
    ucn_i_security_handle_t, const ucn_i_security_current_facts_t *,
    const uint8_t *, size_t, ucn_i_security_packet_workspace_t *,
    uint8_t *, size_t, size_t *);
ucn_result_t ucn_i_security_c1_hop_open(ucn_i_security_owner_t *,
    ucn_i_security_handle_t, const ucn_i_security_current_facts_t *,
    const uint8_t *, size_t, ucn_i_security_packet_workspace_t *,
    ucn_i_security_hop_replay_handle_t *);

static session_fixture_t hs, hr;
static ucn_i_security_packet_workspace_t hw;
static uint8_t hpacket[80];
static session_fixture_t ac, ca, ab, ba, bc, cb;
static session_fixture_t conflicting_neighbor;
static ucn_i_security_acl_rule_t pair_acl[6][2];
static ucn_i_native_routed_t nodes[3];
static ucn_i_adapter_t adapters[3];
static ucn_i_route_owner_t routes[3];
static test_lock_t alocks[3], dlocks[3], rlocks[3], nlocks[3];
static ucn_native_storage_t nstorage[3];
static ucn_native_t *native[3];
#ifdef UCN_NATIVE_HANDOFF_TEST
static ucn_native_lifecycle_storage_t hot_storage[3];
#endif
static ucn_i_native_endpoint_t eps[3][2];
static ucn_i_native_neighbor_t neighbors[3][2];
static ucn_i_route_domain_t flow;
static unsigned captured_count[3], blocked, callbacks[3];
static bool uncertain;
static bool probe_reentry;
static uint8_t capture[3][UCN_ADAPTER_FRAME_BYTES];
static size_t capture_bytes[3];
static uint16_t capture_link[3];
static uint8_t retry_first[UCN_ADAPTER_FRAME_BYTES];
static ucn_i_native_routed_t rejected, zero_routed;
static uint8_t routed_address_width = 1U;

static ucn_driver_submit_result_t routed_submit(void *ctx, ucn_link_handle_t link,
    const uint8_t *p, size_t n, ucn_driver_token_t token)
{
    const size_t i = (size_t)(uintptr_t)ctx;
    (void)token;
    if (probe_reentry && i == 0U) {
        const ucn_native_scope_t local_scope = {0}; uint8_t principal[16];
        memset(principal, 0xA5, sizeof(principal));
        assert(nodes[0].port.validate(&nodes[0], &local_scope, &nodes[0].local,
            0x1201U, UCN_NATIVE_ORIGIN_AUTH, nodes[0].now, principal) == UCN_ERR_STATE);
        assert(principal[0] == 0xA5U);
        assert(nodes[0].port.stop(&nodes[0]) == UCN_ERR_STATE);
#ifdef UCN_NATIVE_HANDOFF_TEST
        assert(ucn_i_native_routed_fence_session(&nodes[0], native[0], &ab.owner,
            ab.handle, nodes[0].now) == UCN_ERR_STATE);
#endif
    }
    if (i == 0U && captured_count[i] == 0U) { memcpy(retry_first, p, n); }
    memcpy(capture[i], p, n); capture_bytes[i] = n; capture_link[i] = link.slot;
    ++captured_count[i];
    if (i == 0U && blocked) { --blocked; return UCN_DRIVER_NOT_SUBMITTED; }
    return uncertain ? UCN_DRIVER_SUBMIT_UNKNOWN : UCN_DRIVER_COMPLETE;
}
static ucn_result_t routed_cancel(void *ctx, ucn_driver_token_t token)
{ (void)ctx; (void)token; return uncertain ? UCN_ERR_STATE : UCN_OK; }
static void route_binding(ucn_i_route_binding_t *to, const ucn_i_security_binding_t *from)
{ to->address = from->address; to->generation = from->binding_generation; memcpy(to->principal, from->principal, 16U); }
static void fixture_pair(session_fixture_t *s, session_fixture_t *r,
    ucn_i_security_candidate_t *sc, unsigned ix)
{
    ucn_i_security_candidate_t rc;
    sc->address_width = routed_address_width; rc = *sc;
    assert(ix < sizeof(pair_acl) / sizeof(pair_acl[0]) - 1U);
    rc.local = sc->peer; rc.peer = sc->local; rc.origin_rx = sc->origin_tx; rc.origin_tx = sc->origin_rx;
    rc.hop_rx = sc->hop_tx; rc.hop_tx = sc->hop_rx;
    assert(fixture_activate_profile(s, sc, UCN_I_SECURITY_ACCESS_OUTBOUND, 100U + ix, (uint16_t)(150U + ix), 0x1201, 0,
        UINT32_C(0x12345678), routed_address_width) == 0);
    assert(fixture_activate_profile(r, &rc, UCN_I_SECURITY_ACCESS_INBOUND, 101U + ix, (uint16_t)(151U + ix), 0x1201, 0,
        UINT32_C(0x12345678), routed_address_width) == 0);
    pair_acl[ix][0] = s->acl; pair_acl[ix][1] = s->acl;
    memcpy(pair_acl[ix][1].context_fingerprint, sc->hop_fingerprint, 16U);
    s->owner.acl_rules = pair_acl[ix]; s->owner.acl_rule_count = 2U;
    pair_acl[ix + 1U][0] = r->acl; pair_acl[ix + 1U][1] = r->acl;
    memcpy(pair_acl[ix + 1U][1].context_fingerprint, rc.hop_fingerprint, 16U);
    r->owner.acl_rules = pair_acl[ix + 1U]; r->owner.acl_rule_count = 2U;
}
static void endpoint(unsigned node, unsigned slot, session_fixture_t *f)
{
    ucn_i_native_endpoint_t *e = &eps[node][slot];
    e->security = &f->owner; e->session = f->handle; e->facts = f->facts;
    e->service = 0x1201U; e->minimum = UCN_NATIVE_ORIGIN_AUTH;
    e->source.address = f->facts.peer.address; e->source.binding_generation = f->facts.peer.binding_generation;
    e->source.principal_digest = f->facts.peer.address;
    memcpy(e->fingerprint, f->candidate.origin_fingerprint, 16U);
}
static void neighbor(unsigned node, unsigned slot, session_fixture_t *f)
{
    ucn_i_native_neighbor_t *e = &neighbors[node][slot];
    e->security = &f->owner; e->session = f->handle; e->facts = f->facts;
    e->link_slot = (uint16_t)slot; e->sender_discriminator = f->facts.peer.address; e->capability_bits = 3U;
    memcpy(e->fingerprint, f->candidate.hop_fingerprint, 16U);
}
static void setup_node(unsigned i, const ucn_i_security_binding_t *local)
{
    ucn_ports_t p = {0}; ucn_link_port_t links[2] = {0}; unsigned j;
    ucn_i_route_config_t rc = {0}; ucn_i_lock_ops_t rl = lock_ops(&rlocks[i]);
    p.struct_size = sizeof(p); p.api_version = UCN_API_VERSION;
    p.link_count = i == 1U ? 2U : 1U; p.links = links;
    p.state_lock.struct_size = sizeof(p.state_lock); p.state_lock.api_version = UCN_API_VERSION;
    p.state_lock.enter = lock_enter; p.state_lock.leave = lock_leave; p.state_lock.context = &alocks[i];
    p.driver_callback_gate = p.state_lock; p.driver_callback_gate.context = &dlocks[i];
    for (j = 0U; j < p.link_count; ++j) {
        links[j].struct_size = sizeof(links[j]); links[j].api_version = UCN_API_VERSION;
        links[j].link_instance = 1U; links[j].frame_mtu = UCN_ADAPTER_FRAME_BYTES;
        links[j].tx.struct_size = sizeof(links[j].tx); links[j].tx.api_version = UCN_API_VERSION;
        links[j].tx.submit = routed_submit; links[j].tx.cancel = routed_cancel;
        links[j].context = (void *)(uintptr_t)i;
    }
    assert(ucn_i_adapter_init(&adapters[i], 201U + i, 9U, &p) == UCN_OK);
    route_binding(&rc.local, local); rc.runtime_instance = 201U + i; rc.owner_instance = 12U;
    rc.realm = 0x12345678U; rc.local_session_generation = 17U; rc.address_width = routed_address_width;
    rc.discovery_lifetime_us = 500U; rc.discovery_retry_us = 100U;
    rc.reverse_lifetime_us = 500U; rc.route_lifetime_us = 1000U;
    rc.first_transaction_id = 1U; rc.discovery_max_attempts = 3U; rc.maximum_hops = 4U;
    assert(ucn_i_route_owner_init(&routes[i], &rc, &rl) == UCN_OK);
}
static void install_mtu(unsigned i, const ucn_i_security_binding_t *dest, unsigned neighbor_slot, uint16_t mtu)
{
    ucn_i_route_static_entry_t entry = {0};
    route_binding(&entry.destination, dest);
    route_binding(&entry.next_hop.peer, &neighbors[i][neighbor_slot].facts.peer);
    entry.next_hop.link_id = (uint16_t)(neighbor_slot + 1U); entry.next_hop.link_generation = 1U;
    entry.next_hop.cost = 1U; entry.next_hop.frame_mtu = UCN_ADAPTER_FRAME_BYTES;
    entry.next_hop.capability_bits = 3U; entry.path_frame_mtu = mtu;
    assert(ucn_i_route_install_static(&routes[i], &entry) == UCN_OK);
}
static void install(unsigned i, const ucn_i_security_binding_t *dest, unsigned neighbor_slot)
{ install_mtu(i, dest, neighbor_slot, UCN_ADAPTER_FRAME_BYTES); }
static ucn_i_route_link_ref_t route_ingress(unsigned node, unsigned slot)
{
    ucn_i_route_link_ref_t link = {0};
    route_binding(&link.peer, &neighbors[node][slot].facts.peer);
    link.link_generation = 1U; link.link_id = (uint16_t)(slot + 1U);
    link.frame_mtu = UCN_ADAPTER_FRAME_BYTES; link.capability_bits = 3U; link.cost = 1U;
    return link;
}
/* Consume a route established through the actual Route Owner protocol APIs.
 * This fixture supplies accepted control facts, not C automatic discovery. */
static void install_dynamic(uint16_t destination_capabilities)
{
    ucn_i_route_rreq_message_t request;
    ucn_i_route_rrep_message_t reply;
    ucn_i_route_request_action_t request_action;
    ucn_i_route_reply_action_t reply_action;
    ucn_i_route_link_ref_t ingress;
    ucn_handle_t discovery; uint8_t created = 0U;
    assert(ucn_i_route_ensure_discovery(&routes[0], 3U, 1U, 47U, 0U, 1500U,
        &discovery, &request, &created) == UCN_OK && created == 1U);
    assert(request.key.origin_session_generation == 17U && ac.handle.session_generation == 1U);
    ingress = route_ingress(1U, 0U);
    assert(ucn_i_route_on_rreq(&routes[1], &request, &ingress, 1500U, &request_action) == UCN_OK);
    request = request_action.forwarded; ingress = route_ingress(2U, 0U);
    assert(ucn_i_route_on_rreq(&routes[2], &request, &ingress, 1500U, &request_action) == UCN_OK);
    assert(ucn_i_route_make_rrep(&routes[2], &request, destination_capabilities,
        UCN_ADAPTER_FRAME_BYTES, 1500U, &reply) == UCN_OK);
    ingress = route_ingress(1U, 1U);
    assert(ucn_i_route_on_rrep(&routes[1], &reply, &ingress, 1500U, &reply_action) == UCN_OK);
    assert(ucn_i_route_complete_rrep_forward(&routes[1], reply_action.completion, 1U) == UCN_OK);
    reply = reply_action.forwarded; ingress = route_ingress(0U, 0U);
    assert(ucn_i_route_on_rrep(&routes[0], &reply, &ingress, 1500U, &reply_action) == UCN_OK);
    assert(reply_action.kind == UCN_I_ROUTE_REPLY_REACHED_ORIGIN);
    assert(reply_action.installed.domain.origin_session_generation == 17U);
    assert(reply_action.installed.capability_bits == destination_capabilities);
}
static void received_sample(void *ctx, const ucn_native_sample_t *s)
{
    const size_t i = (size_t)(uintptr_t)ctx;
    assert(s->payload_bytes == 4U && s->payload[0] == 0x11U);
    assert(memcmp(s->principal, s->source.address == 1U ? ac.facts.local.principal : bc.facts.local.principal, 16U) == 0);
    ++callbacks[i];
}
static void inject(unsigned i, unsigned slot, unsigned from, uint32_t sender, uint64_t at)
{
    ucn_link_handle_t link; ucn_rx_meta_t meta = {0};
    assert(ucn_i_adapter_link_handle(&adapters[i], (uint16_t)slot, &link) == UCN_OK);
    meta.struct_size = sizeof(meta); meta.api_version = UCN_API_VERSION;
    meta.timestamp_us = at; meta.sender_discriminator = sender;
    assert(ucn_i_adapter_rx_publish(&adapters[i], link, capture[from], capture_bytes[from], &meta) == UCN_OK);
}
static int routed_roundtrip(void)
{
    ucn_i_security_candidate_t sc; ucn_i_security_binding_t a, b, c;
    ucn_i_native_routed_config_t cfg = {0};
    ucn_native_channel_t channel = {0}; ucn_native_config_t nc = {0};
    ucn_native_handle_t pub, sub; uint16_t delivered; unsigned i;
    const uint8_t payload[4] = {0x11, 0x22, 0x33, 0x44};
    binding_set(&a, 1U, 101U, 0x10U); binding_set(&b, 2U, 102U, 0x30U); binding_set(&c, 3U, 103U, 0x50U);
    make_h1_candidate(&sc, &a, &c, 1U, 1U, 21U, 22U, 0x80U);
    sc.origin_level = UCN_I_SECURITY_CONFIDENTIAL; sc.origin_tx.suite_id = 2U; sc.origin_rx.suite_id = 2U;
    fixture_pair(&ac, &ca, &sc, 0U);
    make_h1_candidate(&sc, &a, &b, 2U, 1U, 31U, 32U, 0x90U); fixture_pair(&ab, &ba, &sc, 2U);
    make_h1_candidate(&sc, &b, &c, 3U, 1U, 41U, 42U, 0xA0U); fixture_pair(&bc, &cb, &sc, 4U);
    ab.owner.provider_gate = ac.owner.provider_gate; bc.owner.provider_gate = ba.owner.provider_gate;
    cb.owner.provider_gate = ca.owner.provider_gate;
    endpoint(0, 0, &ac); endpoint(0, 1, &ab); endpoint(1, 0, &ba); endpoint(1, 1, &bc);
    endpoint(2, 0, &ca); endpoint(2, 1, &cb);
    neighbor(0, 0, &ab); neighbor(1, 0, &ba); neighbor(1, 1, &bc); neighbor(2, 0, &cb);
    setup_node(0, &a); setup_node(1, &b); setup_node(2, &c);
    install(0, &b, 0); install(1, &c, 1);
    route_binding(&flow.origin, &a); route_binding(&flow.destination, &c);
    flow.realm = 0x12345678U; flow.origin_session_generation = 17U;
    for (i = 0U; i < 3U; ++i) {
        cfg.adapter = &adapters[i]; cfg.routes = &routes[i];
        cfg.gate = i == 0U ? ac.owner.provider_gate : i == 1U ? ba.owner.provider_gate : ca.owner.provider_gate;
        cfg.local.address = i + 1U; cfg.local.binding_generation = 101U + i; cfg.local.principal_digest = i + 1U;
        cfg.now_us = 1500U; cfg.expires_at_us = 3000U; cfg.relay_lifetime_us = 200U;
        cfg.endpoints = eps[i]; cfg.endpoint_count = 2U;
        cfg.neighbors = neighbors[i]; cfg.neighbor_count = i == 1U ? 2U : 1U;
        cfg.forward_domains = i == 1U ? &flow : NULL; cfg.forward_count = i == 1U ? 1U : 0U; cfg.hop_limit = 3U;
        {
            uint16_t saved_count = cfg.endpoint_count;
            cfg.endpoint_count = UCN_I_NATIVE_ENDPOINTS + 1U;
            assert(ucn_i_native_routed_init(&rejected, &cfg) == UCN_ERR_ARGUMENT);
            assert(memcmp(&rejected, &zero_routed, sizeof(rejected)) == 0);
            cfg.endpoint_count = saved_count;
        }
        if (i == 0U) {
            ucn_i_native_neighbor_t conflict = neighbors[0][0];
            memcpy(eps[0][0].fingerprint, ac.candidate.hop_fingerprint, 16U);
            assert(ucn_i_native_routed_init(&rejected, &cfg) != UCN_OK);
            assert(memcmp(&rejected, &zero_routed, sizeof(rejected)) == 0);
            memcpy(eps[0][0].fingerprint, ac.candidate.origin_fingerprint, 16U);
            memcpy(neighbors[0][0].fingerprint, ab.candidate.origin_fingerprint, 16U);
            assert(ucn_i_native_routed_init(&rejected, &cfg) != UCN_OK);
            assert(memcmp(&rejected, &zero_routed, sizeof(rejected)) == 0);
            memcpy(neighbors[0][0].fingerprint, ab.candidate.hop_fingerprint, 16U);
            sc = ab.candidate; sc.peer.address = 3U; /* Valid Session, conflicting immutable Realm binding. */
            assert(fixture_activate(&conflicting_neighbor, &sc, UCN_I_SECURITY_ACCESS_OUTBOUND,
                106U, 156U, 0x1201U, 0U) == 0);
            conflicting_neighbor.owner.provider_gate = ac.owner.provider_gate;
            conflict.security = &conflicting_neighbor.owner; conflict.session = conflicting_neighbor.handle;
            conflict.facts = conflicting_neighbor.facts; cfg.neighbors = &conflict;
            assert(ucn_i_native_routed_init(&rejected, &cfg) != UCN_OK);
            assert(memcmp(&rejected, &zero_routed, sizeof(rejected)) == 0);
            cfg.neighbors = neighbors[0];
        }
        if (i == 1U) {
            ucn_i_route_domain_t bad_domains[2] = {flow, flow};
            bad_domains[1].origin.generation++;
            bad_domains[1].destination.address = 4U;
            cfg.forward_domains = bad_domains; cfg.forward_count = 2U;
            assert(ucn_i_native_routed_init(&rejected, &cfg) != UCN_OK);
            assert(memcmp(&rejected, &zero_routed, sizeof(rejected)) == 0);
            cfg.forward_domains = &flow; cfg.forward_count = 1U;
        }
        assert(ucn_i_native_routed_init(&nodes[i], &cfg) == UCN_OK);
    }
    channel.id = 1U; channel.service_id = 0x1201U; channel.schema_id = 1U;
    channel.schema_revision = 1U; channel.payload_bytes = 4U; channel.traffic_class = UCN_TRAFFIC_Q1;
    channel.queue_policy = UCN_NATIVE_KEEP_LATEST; channel.security_minimum = UCN_NATIVE_ORIGIN_AUTH;
    channel.max_local_age_us = 100U; channel.source_count = 1U;
    channel.sources[0] = nodes[0].local; channel.target_count = 2U;
    channel.targets[0].address = 3U; channel.targets[0].binding_generation = 103U;
    channel.targets[1].address = 2U; channel.targets[1].binding_generation = 102U;
    nc.api_version = UCN_NATIVE_API_VERSION; nc.storage_layout = UCN_NATIVE_LAYOUT;
    nc.manifest_hash = UCN_NATIVE_MANIFEST_HASH; nc.channels = &channel; nc.channel_count = 1U;
    nc.lock.struct_size = sizeof(nc.lock); nc.lock.api_version = UCN_API_VERSION;
    nc.lock.enter = lock_enter; nc.lock.leave = lock_leave;
    for (i = 0U; i < 3U; ++i) {
        if (i == 1U) { channel.source_count = 2U; channel.sources[1] = nodes[1].local; channel.target_count = 1U; }
        if (i == 2U) { channel.target_count = 0U; }
        nc.instance = 301U + i; nc.lock.context = &nlocks[i];
        assert(ucn_native_init_backend(&nstorage[i], sizeof(nstorage[i]), &nodes[i].port, &nc, &native[i]) == UCN_OK);
#ifdef UCN_NATIVE_HANDOFF_TEST
        assert(ucn_native_backend_enable_lifecycle(native[i], &hot_storage[i], sizeof(hot_storage[i])) == UCN_OK);
#endif
        if (i) { assert(ucn_subscription_create(native[i], 1U, received_sample, (void *)(uintptr_t)i, &sub) == UCN_OK); }
        assert(ucn_native_start(native[i], 1500U) == UCN_OK); channel.target_count = 0U;
    }
    assert(ucn_publisher_create(native[0], 1U, &pub) == UCN_OK);
    {
        const ucn_native_scope_t local_scope = {0};
        ucn_i_callback_claim_t held = {987U, 654U, 1U, 1U};
        ucn_native_backend_message_t message;
        uint8_t local_principal[16]; const uint8_t no_principal[16] = {0};
        memset(local_principal, 0xA5, sizeof(local_principal));
        assert(nodes[0].port.validate(&nodes[0], &local_scope, &nodes[0].local, 0x1201U,
            UCN_NATIVE_ORIGIN_AUTH, 1500U, local_principal) == UCN_OK);
        assert(memcmp(local_principal, no_principal, sizeof(local_principal)) == 0);
        assert(nodes[0].port.local_view(&nodes[0], (ucn_node_view_t *)&nodes[0]) == UCN_ERR_ARGUMENT);
        assert(nodes[0].port.binding(&nodes[0], 1U, &nodes[0].local) == UCN_ERR_ARGUMENT);
        assert(nodes[0].port.poll(&nodes[0], &message, nodes[0].plaintext, sizeof(nodes[0].plaintext)) == UCN_ERR_ARGUMENT);
        assert(nodes[0].port.validate(&nodes[0], &local_scope, &nodes[0].local, 0x1201U,
            UCN_NATIVE_ORIGIN_AUTH, 1500U, (uint8_t *)&ac.owner) == UCN_ERR_ARGUMENT);
        assert(ucn_i_callback_gate_enter(ac.owner.provider_gate, &held) == UCN_OK);
        assert(nodes[0].port.send(&nodes[0], &channel, channel.targets[0], payload, 1500U, 1600U) == UCN_ERR_STATE);
        assert(ucn_i_callback_gate_leave(ac.owner.provider_gate, &held) == UCN_OK);
    }
    /* Missing route/short MTU never consumes an Origin sequence. */
    assert(nodes[0].port.send(&nodes[0], &channel, channel.targets[0], payload, 1500U, 1600U) == UCN_ERR_NOT_FOUND);
    install_mtu(0, &c, 0, 46U);
    assert(nodes[0].port.send(&nodes[0], &channel, channel.targets[0], payload, 1500U, 1600U) == UCN_ERR_NO_SPACE);
    assert(ac.provider.protect_origin_calls == 0U);
    install(0, &c, 0);
    install_dynamic(3U);
    blocked = 2U; probe_reentry = true;
    assert(ucn_publish(native[0], pub, payload, sizeof(payload)) == UCN_OK);
    assert(ucn_native_step(native[0], 1501U, 1U) == UCN_OK);
    assert(captured_count[0] == 1U && capture_bytes[0] == 47U);
    assert(capture[0][0] == 0x61U && capture[0][1] == 0x40U && capture[0][2] == 0x83U);
    assert(capture[0][3] == 1U && capture[0][4] == 3U);
    assert(ucn_native_step(native[0], 1502U, 1U) == UCN_OK);
    assert(captured_count[0] == 2U && memcmp(retry_first, capture[0], 47U) == 0);
    assert(ucn_native_step(native[0], 1502U, 1U) == UCN_OK);
    assert(captured_count[0] == 3U && memcmp(retry_first, capture[0], 47U) == 0);
    inject(1, 0, 0, 99U, 1502U); /* exact physical source discriminator */
    assert(ucn_native_step(native[1], 1503U, 1U) == UCN_OK && captured_count[1] == 0U);
    inject(1, 0, 0, 1U, 1502U);
    assert(ucn_native_step(native[1], 1503U, 1U) == UCN_OK && captured_count[1] == 1U);
    assert(capture_link[1] == 2U && capture_bytes[1] == 47U && capture[1][2] == 0x82U);
    assert(memcmp(capture[0] + 3U, capture[1] + 3U, 28U) == 0);
    assert(ba.provider.open_origin_calls == 0U && bc.provider.open_origin_calls == 0U);
    inject(2, 0, 1, 2U, 1503U);
    assert(ucn_native_step(native[2], 1504U, 1U) == UCN_OK);
    assert(ucn_native_dispatch(native[2], 1505U, 2U, &delivered) == UCN_OK && delivered == 1U && callbacks[2] == 1U);
    inject(2, 0, 1, 2U, 1503U);
    assert(ucn_native_step(native[2], 1506U, 1U) == UCN_OK);
    assert(ucn_native_dispatch(native[2], 1506U, 2U, &delivered) == UCN_OK && delivered == 0U);
    assert(ucn_native_step(native[0], 1507U, UCN_NATIVE_CAPACITY) == UCN_OK);
    assert(ucn_native_step(native[0], 1507U, UCN_NATIVE_CAPACITY) == UCN_OK);
    assert(capture[0][4] == 2U); /* same Service second independent endpoint */
    inject(1, 0, 0, 1U, 1507U);
    assert(ucn_native_step(native[1], 1508U, 2U) == UCN_OK);
    assert(ucn_native_dispatch(native[1], 1509U, 2U, &delivered) == UCN_OK && delivered == 1U && callbacks[1] == 1U);
    /* New Hop sequences do not revive an already committed Origin sequence. */
    memcpy(hpacket, retry_first, 31U); ab.facts.now_us = 1509U;
    assert(ucn_i_security_c1_hop_wrap(&ab.owner, ab.handle, &ab.facts, hpacket, 31U,
        &hw, capture[0], sizeof(capture[0]), &capture_bytes[0]) == UCN_OK);
    inject(1, 0, 0, 1U, 1509U);
    assert(ucn_native_step(native[1], 1509U, 2U) == UCN_OK);
    inject(2, 0, 1, 2U, 1509U);
    assert(ucn_native_step(native[2], 1509U, 2U) == UCN_OK);
    assert(ucn_native_dispatch(native[2], 1509U, 2U, &delivered) == UCN_OK && delivered == 0U);
    /* Same Channel/Service receives a second Origin's seq=1 independently. */
    assert(ucn_native_step(native[1], 1509U, UCN_NATIVE_CAPACITY) == UCN_OK);
    assert(ucn_native_send(native[1], 1U, channel.targets[0], payload, sizeof(payload)) == UCN_OK);
    assert(ucn_native_step(native[1], 1509U, UCN_NATIVE_CAPACITY) == UCN_OK);
    assert(capture[1][3] == 2U && capture[1][10] == 1U);
    inject(2, 0, 1, 2U, 1509U);
    assert(ucn_native_step(native[2], 1509U, 2U) == UCN_OK);
    assert(ucn_native_dispatch(native[2], 1509U, 2U, &delivered) == UCN_OK && delivered == 1U && callbacks[2] == 2U);
    /* Authenticated TTL=1 reaches B but must never be forwarded. */
    memcpy(hpacket, retry_first, 31U); hpacket[2] = 0x81U;
    ab.facts.now_us = 1510U;
    assert(ucn_i_security_c1_hop_wrap(&ab.owner, ab.handle, &ab.facts, hpacket, 31U,
        &hw, capture[0], sizeof(capture[0]), &capture_bytes[0]) == UCN_OK);
    i = captured_count[1]; inject(1, 0, 0, 1U, 1510U);
    assert(ucn_native_step(native[1], 1510U, 2U) == UCN_OK && captured_count[1] == i);
    /* A malformed ring is drained; the next valid packet is not starved. */
    capture[0][0] = 0U; capture_bytes[0] = 1U;
    for (i = 0U; i < UCN_ADAPTER_RX_SLOT_COUNT; ++i) { inject(1, 0, 0, 1U, 1510U); }
    assert(ucn_native_step(native[1], 1510U, UCN_ADAPTER_RX_SLOT_COUNT) == UCN_OK);
    /* Original RX age, not poll time: valid old input expires at dispatch. */
    assert(nodes[0].port.step(&nodes[0], 1511U, 2U) == UCN_OK);
    assert(nodes[0].port.send(&nodes[0], &channel, channel.targets[0], payload, 1511U, 1700U) == UCN_OK);
    assert(nodes[0].tx_pending && !nodes[0].tx_live);
    assert(nodes[0].port.step(&nodes[0], 1511U, 2U) == UCN_OK);
    inject(1, 0, 0, 1U, 1511U);
    assert(ucn_native_step(native[1], 1511U, 2U) == UCN_OK);
    inject(2, 0, 1, 2U, 1512U);
    assert(ucn_native_step(native[2], 1611U, 2U) == UCN_OK);
    assert(ucn_native_dispatch(native[2], 1612U, 2U, &delivered) == UCN_OK && delivered == 0U);
    /* Queued Origin sample loses authority before callback. */
    assert(nodes[0].port.step(&nodes[0], 1613U, 2U) == UCN_OK);
    assert(nodes[0].port.send(&nodes[0], &channel, channel.targets[0], payload, 1613U, 1800U) == UCN_OK);
    assert(nodes[0].port.step(&nodes[0], 1613U, 2U) == UCN_OK);
    inject(1, 0, 0, 1U, 1613U);
    assert(ucn_native_step(native[1], 1614U, 2U) == UCN_OK);
    inject(2, 0, 1, 2U, 1614U);
    assert(ucn_native_step(native[2], 1615U, 2U) == UCN_OK);
#ifdef UCN_NATIVE_HANDOFF_TEST
    {
        bool drained = true;
        assert(ucn_i_native_routed_fence_session(&nodes[2], native[1], &ca.owner, ca.handle, 1615U) == UCN_ERR_STATE);
        assert(ucn_i_native_routed_fence_session(&nodes[2], native[2], &ca.owner, ca.handle, 1615U) == UCN_OK);
        assert(ucn_i_native_routed_session_drained(&nodes[2], native[2], &ca.owner, ca.handle, &drained) == UCN_OK && drained);
        /* Barrier removes the queued old sample without closing Subscription. */
        assert(ucn_i_native_routed_fence_session(&nodes[2], native[2], &ca.owner, ca.handle, 1615U) == UCN_OK);
    }
#else
    assert(ucn_i_security_fence(&ca.owner, ca.handle) == UCN_OK);
#endif
    assert(ucn_native_dispatch(native[2], 1616U, 2U, &delivered) == UCN_OK && delivered == 0U);
    /* Absolute Static assembly expiry is never extended by poll/dispatch. */
    {
        const ucn_native_scope_t scope = {nodes[1].instance, 1U, 1U, 0U};
        uint8_t principal[16], before[16]; memset(principal, 0xA5, sizeof(principal)); memcpy(before, principal, 16U);
        assert(nodes[1].port.validate(&nodes[1], &scope, &eps[1][0].source, 0x1201U,
            UCN_NATIVE_ORIGIN_AUTH, 3000U, principal) != UCN_OK);
        assert(memcmp(before, principal, 16U) == 0);
    }
    /* Unknown physical holder survives Stop, logical deadline and Root drain. */
    assert(nodes[0].port.step(&nodes[0], 1620U, 2U) == UCN_OK);
    uncertain = true;
    assert(nodes[0].port.send(&nodes[0], &channel, channel.targets[0], payload, 1620U, 1621U) == UCN_OK);
    assert(nodes[0].port.step(&nodes[0], 1620U, 2U) == UCN_OK);
    assert(nodes[0].tx_live && nodes[0].frame_bytes == 47U);
#ifdef UCN_NATIVE_HANDOFF_TEST
    {
        bool drained = true;
        assert(ucn_i_native_routed_fence_session(&nodes[0], native[0], &ab.owner, ab.handle, 1620U) == UCN_OK);
        assert(ucn_i_native_routed_session_drained(&nodes[0], native[0], &ab.owner, ab.handle, &drained) == UCN_OK && !drained);
        assert(ucn_i_native_routed_session_drained(&nodes[0], native[0], &ab.owner, ab.handle, (bool *)native[0]) == UCN_ERR_ARGUMENT);
    }
#endif
    memcpy(retry_first, nodes[0].frame, 47U);
    assert(ucn_native_stop(native[0]) == UCN_OK);
    assert(ucn_native_step(native[0], 3001U, UCN_NATIVE_CAPACITY) == UCN_OK);
    assert(nodes[0].tx_live && nodes[0].lifecycle != UCN_LIFECYCLE_QUIESCENT);
    assert(memcmp(retry_first, nodes[0].frame, 47U) == 0);
    assert(ucn_native_deinit(native[0]) == UCN_ERR_STATE);
    assert(ucn_native_stop(native[1]) == UCN_OK);
    assert(ucn_native_step(native[1], 3001U, UCN_NATIVE_CAPACITY) == UCN_OK);
    assert(nodes[1].lifecycle == UCN_LIFECYCLE_QUIESCENT);
    return 0;
}
static int independent_hop(void)
{
    ucn_i_security_candidate_t sc = candidate_make(0x20U, 1U), rc;
    ucn_i_security_hop_replay_handle_t replay;
    /* 11-byte C1 header + 4-byte opaque payload + 16-byte Origin Tag.
     * Addresses 1 -> 4 are deliberately NOT Hop peer addresses. */
    const uint8_t origin[31] = {0x61, 0x40, 0x43, 1, 4, 0x12, 1, 0, 0, 0, 1, 1, 2, 3, 4};
    /* Independent C/Rust contract vector: domain11 + BE32(51) + C1Origin31
     * + BE32(HopSeq1) + fingerprint16. Not derived with the implementation. */
    static const uint8_t hop_aad_vector[66] = {
        0x55, 0x43, 0x4E, 0x36, 0x2D, 0x48, 0x4F, 0x50, 0x2D, 0x56, 0x31,
        0x00, 0x00, 0x00, 0x33,
        0x61, 0x40, 0x43, 0x01, 0x04, 0x12, 0x01, 0x00, 0x00, 0x00, 0x01, 0x01, 0x02, 0x03, 0x04,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x01,
        0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
        0x59, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F, 0x60
    };
    size_t n = 0U;
    sc.hop_profile = UCN_I_HOP_PROFILE_H1;
    sc.hop_tx.suite_id = 16U; sc.hop_tx.key_id = 31U; sc.hop_tx.key_generation = 51U;
    sc.hop_rx = sc.hop_tx; sc.hop_rx.key_id = 32U;
    rc = sc; rc.local = sc.peer; rc.peer = sc.local;
    rc.hop_rx = sc.hop_tx; rc.hop_tx = sc.hop_rx;
    assert(fixture_activate(&hs, &sc, UCN_I_SECURITY_ACCESS_OUTBOUND, 71, 81, 0x1201, 0) == 0);
    assert(fixture_activate(&hr, &rc, UCN_I_SECURITY_ACCESS_INBOUND, 72, 82, 0x1201, 0) == 0);
    memcpy(hs.acl.context_fingerprint, sc.hop_fingerprint, 16);
    memcpy(hr.acl.context_fingerprint, rc.hop_fingerprint, 16);
    n = 777U; memset(hpacket, 0xA5, sizeof(hpacket));
    assert(ucn_i_security_c1_hop_wrap(&hs.owner, hs.handle, &hs.facts,
        origin, sizeof(origin), &hw, hpacket, 46U, &n) == UCN_ERR_NO_SPACE);
    assert(n == 777U && hpacket[0] == 0xA5U && hs.provider.protect_hop_calls == 0U);
    assert(ucn_i_security_c1_hop_wrap(&hs.owner, hs.handle, &hs.facts,
        origin, sizeof(origin), &hw, hw.packet, sizeof(hw.packet), &n) == UCN_ERR_ARGUMENT);
    assert(n == 777U && hs.provider.protect_hop_calls == 0U);
    assert(ucn_i_security_c1_hop_wrap(&hs.owner, hs.handle, &hs.facts,
        origin, sizeof(origin), &hw, hpacket, sizeof(hpacket), &n) == UCN_OK);
    assert(n == 47U && memcmp(hpacket, origin, 31U) == 0);
    assert(hw.hop_protect.aad_bytes == 66U && memcmp(hw.hop_aad, hop_aad_vector, 66U) == 0);
    assert(hpacket[31] == 0U && hpacket[32] == 0U && hpacket[33] == 0U && hpacket[34] == 1U);
    assert(hs.provider.protect_origin_calls == 0U && hs.provider.protect_hop_calls == 1U);
    hpacket[46] ^= 1U;
    assert(ucn_i_security_c1_hop_open(&hr.owner, hr.handle, &hr.facts,
        hpacket, n, &hw, &replay) != UCN_OK);
    hpacket[46] ^= 1U;
    assert(ucn_i_security_c1_hop_open(&hr.owner, hr.handle, &hr.facts,
        hpacket, n, &hw, &replay) == UCN_OK);
    assert(ucn_i_security_hop_replay_commit(&hr.owner, &replay, &hr.facts) == UCN_OK);
    assert(ucn_i_security_c1_hop_open(&hr.owner, hr.handle, &hr.facts,
        hpacket, n, &hw, &replay) != UCN_OK);
    assert(hr.provider.open_origin_calls == 0U);
    return 0;
}
#ifndef UCN_ROUTED_FIXTURE_ONLY
int main(void)
{
    static ucn_i_native_routed_t backend;
    ucn_i_native_routed_config_t invalid = {0};
    assert(independent_hop() == 0);
    assert(ucn_i_native_routed_init(&backend, &invalid) == UCN_ERR_ARGUMENT);
    assert(routed_roundtrip() == 0);
    printf("Native routed A-B-C / same-Service fanout / independent Origin-H1 / boundaries passed; backend=%zu bytes\n", sizeof(ucn_i_native_routed_t));
    return 0;
}
#endif
