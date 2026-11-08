/* Real Native -> routed -> Adapter byte transport; no typed Route injection. */
#define UCN_ROUTED_FIXTURE_ONLY
#include "test_native_routed.c"
#include "internal/ucn_native_route_control.h"

static uint64_t wire_now;
static unsigned wire_frames;
static unsigned scenario_mode, dropped, source_requests, not_submitted;
static bool business_during_control;
static uint64_t capped_bundle;
static unsigned capped_submits;
static uint8_t forwarded_request[54], delayed_reply[2][127];
static size_t delayed_reply_bytes[2];
static uint64_t reverse_expiry;
static unsigned late_submits;
static uint8_t first_request[54], first_frame[127];
static size_t first_frame_bytes;
static uint64_t first_transaction;
static ucn_driver_token_t unknown_token;
static const uint8_t *unknown_pointer;
static ucn_i_native_route_control_t controls[3];
static ucn_i_security_acl_rule_t control_acl[5][10];
static session_fixture_t *control_sessions[5] = {&ab, &ba, &bc, &cb, &ac};
static uint64_t counters[5] = {100U, 200U, 300U, 400U, 500U};
static ucn_result_t counter_reserve(void *context, ucn_i_security_owner_t *owner,
    ucn_i_security_handle_t session, uint64_t *out)
{
    unsigned i; bool active; ucn_i_callback_claim_t claim;
    (void)context;
    assert(ucn_i_callback_gate_view(owner->provider_gate, &active, &claim) == UCN_OK && active);
    for (i = 0U; i < 5U; ++i) {
        if (owner == &control_sessions[i]->owner && memcmp(&session, &control_sessions[i]->handle, sizeof(session)) == 0) {
            unsigned root = i == 0U || i == 4U ? 0U : i == 3U ? 2U : 1U;
            assert(nodes[root].port.stop(&nodes[root]) == UCN_ERR_STATE);
            assert(nodes[root].port.step(&nodes[root], nodes[root].now, 1U) == UCN_ERR_STATE);
            *out = ++counters[i]; return UCN_OK;
        }
    }
    return UCN_ERR_NOT_FOUND;
}
static void allow_controls(void)
{
    unsigned i, j; const uint16_t opcodes[] = {0x0201U, 0x0202U, 0x0203U, 0x0206U};
    for (i = 0U; i < 5U; ++i) {
        session_fixture_t *f = control_sessions[i];
        control_acl[i][0] = f->owner.acl_rules[0]; control_acl[i][1] = f->owner.acl_rules[1];
        for (j = 0U; j < 8U; ++j) {
            control_acl[i][j + 2U] = f->acl;
            control_acl[i][j + 2U].service_id = 0U;
            control_acl[i][j + 2U].protocol_opcode = opcodes[j / 2U];
            control_acl[i][j + 2U].direction = (uint8_t)(j % 2U + 1U);
        }
        f->owner.acl_rules = control_acl[i]; f->owner.acl_rule_count = 10U;
    }
}
static void authorization_checks(void)
{
    ucn_i_security_control_access_request_t access = {0};
    ucn_i_security_current_facts_t facts = ab.facts;
    access.session = ab.handle; access.protocol_opcode = 0x0201U;
    access.direction = UCN_I_SECURITY_ACCESS_OUTBOUND;
    memcpy(access.context_fingerprint, ab.candidate.origin_fingerprint, 16U);
    assert(ucn_i_security_control_authorize(&ab.owner, &access, &facts) == UCN_OK);
    access.protocol_opcode = 0x0204U;
    assert(ucn_i_security_control_authorize(&ab.owner, &access, &facts) == UCN_ERR_ACCESS);
    access.protocol_opcode = 0x0201U; access.direction = 0U;
    assert(ucn_i_security_control_authorize(&ab.owner, &access, &facts) == UCN_ERR_ARGUMENT);
    access.direction = UCN_I_SECURITY_ACCESS_OUTBOUND; facts.now_us = UINT64_MAX;
    assert(ucn_i_security_control_authorize(&ab.owner, &access, &facts) != UCN_OK);
    facts = ab.facts; ab.owner.acl_rule_count = 2U;
    assert(ucn_i_security_control_authorize(&ab.owner, &access, &facts) == UCN_ERR_ACCESS);
    ab.owner.acl_rule_count = 10U;
}
static ucn_i_security_packet_workspace_t inject_workspace;
static ucn_i_security_c0_protect_request_t inject_request;
static uint8_t inject_packet[127];
static size_t inject_bytes;
static void protected_input(unsigned sender, uint16_t opcode, uint8_t interaction,
    const uint8_t *body, size_t bytes, unsigned mutation)
{
    session_fixture_t *f = control_sessions[sender];
    ucn_link_handle_t link; ucn_rx_meta_t meta = {0};
    unsigned target = sender == 0U ? 1U : sender == 2U ? 2U : 0U;
    /* Use the same product counter bank and real C0/H1 protection. */
    assert(sender <= 2U);
    memset(&inject_request, 0, sizeof(inject_request));
    inject_request.session = f->handle; inject_request.facts = f->facts;
    inject_request.facts.now_us = wire_now; inject_request.transaction_id = ++counters[sender];
    inject_request.protocol_opcode = opcode; inject_request.interaction_role = interaction;
    inject_request.payload_kind = UCN_I_PAYLOAD_CONTROL; inject_request.hop_limit = 1U;
    inject_request.direction = UCN_I_SECURITY_ACCESS_OUTBOUND;
    memcpy(inject_request.context_fingerprint, f->candidate.origin_fingerprint, 16U);
    assert(ucn_i_security_c0_protect(&f->owner, &inject_request, body, bytes,
        &inject_workspace, inject_packet, sizeof(inject_packet), &inject_bytes) == UCN_OK);
    if (mutation == 1U) { inject_packet[inject_bytes - 1U] ^= 1U; }
    if (mutation == 2U) {
        uint8_t tag[16];
        inject_packet[inject_bytes - 17U] ^= 1U;
        /* Host signing fixture deliberately creates a VALID Hop over an
         * INVALID Origin tag; this isolates end-to-end Origin rejection. */
        memcpy(inject_workspace.hop_aad + 15U, inject_packet, inject_bytes - 16U);
        test_tag(&f->candidate.hop_tx, inject_workspace.hop_aad,
            inject_workspace.hop_protect.aad_bytes, NULL, 0U, tag);
        memcpy(inject_packet + inject_bytes - 12U, tag, 12U);
    }
    if (mutation == 3U) { inject_packet[3] = 4U; }
    assert(ucn_i_adapter_link_handle(&adapters[target], 0U, &link) == UCN_OK);
    meta.struct_size = sizeof(meta); meta.api_version = UCN_API_VERSION;
    meta.timestamp_us = wire_now; meta.sender_discriminator = sender == 0U ? 1U : 2U;
    assert(ucn_i_adapter_rx_publish(&adapters[target], link, inject_packet, inject_bytes, &meta) == UCN_OK);
    assert(ucn_native_step(native[target], wire_now, 4U) == UCN_OK);
    assert(ucn_native_step(native[target], wire_now, 4U) == UCN_OK);
    if (mutation == 4U) {
        uint64_t replay = ab.owner.sessions[ab.handle.slot].c0_rx_highest_committed;
        assert(ucn_i_adapter_rx_publish(&adapters[0], link, inject_packet, inject_bytes, &meta) == UCN_OK);
        assert(ucn_native_step(native[0], wire_now, 4U) == UCN_OK);
        assert(ab.owner.sessions[ab.handle.slot].c0_rx_highest_committed == replay);
    }
}
static void fragment_input(uint64_t bundle, uint8_t piece, const uint8_t body[99])
{
    ucn_i_route_fragment_t fragment = {0}; uint8_t encoded[64]; size_t bytes;
    fragment.opcode = 0x0203U; fragment.total = 99U; fragment.count = 2U;
    fragment.index = piece; fragment.bundle = bundle; fragment.length = piece ? 49U : 50U;
    memcpy(fragment.chunk, body + (piece ? 50U : 0U), fragment.length);
    assert(ucn_i_route_fragment_encode(&fragment, encoded, sizeof(encoded), &bytes) == UCN_OK);
    protected_input(1U, 0x0206U, UCN_INTERACTION_ONE_WAY, encoded, bytes, 0U);
}
static void negative_inputs(void)
{
    ucn_i_route_forward_key_t key = {0}; ucn_i_route_resolved_t resolved, after;
    ucn_i_route_rerr_payload_t error; uint8_t body[99]; uint64_t old_bundle, new_bundle, deadline;
    unsigned mutation; uint16_t discoveries, reverse, dynamic, statics;
    ucn_i_route_rreq_message_t request; uint8_t request_body[54];
    key.now_us = wire_now; key.realm = flow.realm; key.source_address = 1U;
    key.source_binding_generation = 101U; key.destination_address = 3U; key.destination_binding_generation = 103U;
    assert(ucn_i_route_forward_resolve(&routes[0], &key, &resolved) == UCN_OK);
    for (mutation = 1U; mutation <= 4U; ++mutation) {
        uint64_t high = ab.owner.sessions[ab.handle.slot].c0_rx_highest_committed;
        protected_input(1U, 0x0201U, UCN_INTERACTION_REQUEST, first_request, 54U, mutation);
        if (mutation < 4U) { assert(ab.owner.sessions[ab.handle.slot].c0_rx_highest_committed == high); }
    }
    assert(ucn_i_route_control_request_decode(first_request, 54U, 1U, &request) == UCN_OK);
    route_binding(&request.key.origin, &ba.facts.local); request.key.target_address = 1U; request.key.transaction_id = 900U;
    assert(ucn_i_route_control_request_encode(&request, 1U, request_body) == UCN_OK);
    assert(ucn_i_route_counts(&routes[0], &discoveries, &reverse, &dynamic, &statics) == UCN_OK && reverse == 0U);
    protected_input(1U, 0x0201U, UCN_INTERACTION_RESULT, request_body, 54U, 0U);
    assert(ucn_i_route_counts(&routes[0], &discoveries, &reverse, &dynamic, &statics) == UCN_OK && reverse == 0U);
    protected_input(1U, 0x0202U, UCN_INTERACTION_RESULT, first_request, 54U, 0U);
    /* Unknown opcode ACL is denied before packet state can affect Route. */
    control_acl[0][2].protocol_opcode = 0x0204U;
    protected_input(1U, 0x0201U, UCN_INTERACTION_REQUEST, first_request, 54U, 0U);
    control_acl[0][2].protocol_opcode = 0x0201U;
    assert(ucn_i_route_forward_resolve(&routes[1], &key, &after) == UCN_OK);
    assert(after.dynamic.route_generation != resolved.dynamic.route_generation);
    assert(ucn_i_route_make_rerr(&routes[1], &after.dynamic, UCN_I_ROUTE_RERR_LINK_INVALID, &error) == UCN_OK);
    error.reporter_address = 2U; error.reporter_binding_generation = 102U;
    memcpy(error.reporter_principal, ba.facts.local.principal, 16U);
    ++error.route_causal_id; /* Correct authenticated reporter, WRONG causal receipt. */
    assert(ucn_i_route_rerr_encode(&error, body) == UCN_OK);
    old_bundle = counters[1] + 1U;
    fragment_input(old_bundle, 0U, body);
    deadline = controls[0].rx[0].deadline;
    assert(controls[0].rx[0].mask == 1U && deadline > wire_now);
    fragment_input(counters[1] + 1U, 1U, body);
    assert(controls[0].rx[0].bundle == old_bundle && controls[0].rx[0].mask == 1U);
    new_bundle = counters[1] + 1U; ++wire_now;
    fragment_input(new_bundle, 0U, body);
    assert(controls[0].rx[0].deadline == deadline && controls[0].rx[0].bundle == new_bundle);
    fragment_input(old_bundle, 1U, body); /* Late old tail may not join a new first. */
    assert(controls[0].rx[0].mask == 1U);
    fragment_input(new_bundle, 1U, body);
    assert(controls[0].rx[0].mask == 0U);
    fragment_input(old_bundle, 0U, body); /* High-water survives complete/reset. */
    assert(controls[0].rx[0].mask == 0U);
    /* Wrapper authorization cannot stand in for exact inner opcode ACL. */
    control_acl[0][6].protocol_opcode = 0x0204U;
    fragment_input(counters[1] + 1U, 0U, body);
    assert(controls[0].rx[0].mask == 0U);
    control_acl[0][6].protocol_opcode = 0x0203U;
    --error.route_causal_id; ++error.route_generation;
    assert(ucn_i_route_rerr_encode(&error, body) == UCN_OK);
    new_bundle = counters[1] + 1U;
    fragment_input(new_bundle, 0U, body); fragment_input(new_bundle, 1U, body);
    key.now_us = wire_now;
    assert(ucn_i_route_forward_resolve(&routes[0], &key, &after) == UCN_OK);
    assert(memcmp(&resolved.dynamic, &after.dynamic, sizeof(resolved.dynamic)) == 0);
    protected_input(1U, 0x0201U, UCN_INTERACTION_REQUEST, request_body, 54U, 0U);
    assert(ucn_i_route_counts(&routes[0], &discoveries, &reverse, &dynamic, &statics) == UCN_OK && reverse == 1U);
}
static void assembly_timeout(void)
{
    uint8_t body[99] = {0}; uint64_t bundle = counters[1] + 1U, deadline;
    fragment_input(bundle, 0U, body); deadline = controls[0].rx[0].deadline;
    assert(controls[0].rx[0].mask == 1U && deadline > wire_now);
    wire_now = deadline;
    assert(ucn_native_step(native[0], wire_now, 4U) == UCN_OK);
    assert(controls[0].rx[0].mask == 0U && controls[0].rx[0].high_bundle == bundle);
    fragment_input(bundle, 0U, body);
    assert(controls[0].rx[0].mask == 0U);
    fragment_input(counters[1] + 1U, 0U, body);
    assert(controls[0].rx[0].mask == 1U);
}
static ucn_driver_submit_result_t wire_submit(void *ctx, ucn_link_handle_t link,
    const uint8_t *p, size_t n, ucn_driver_token_t token)
{
    unsigned from = (unsigned)(uintptr_t)ctx;
    unsigned to = from == 1U ? (link.slot == 1U ? 0U : 2U) : 1U;
    unsigned slot = to == 1U && from == 2U ? 1U : 0U;
    ucn_link_handle_t target; ucn_rx_meta_t meta = {0};
    ucn_i_c0_frame_t frame; ucn_i_route_fragment_t fragment;
    assert(n <= 127U);
    if (scenario_mode == 11U && from == 0U && p[0] == 0x61U && controls[0].jobs[1].occupied) {
        business_during_control = true;
    }
    if ((scenario_mode == 7U || scenario_mode == 8U || scenario_mode == 11U) && from == 0U && link.slot == 2U) {
        ++dropped; return scenario_mode == 11U ? UCN_DRIVER_NOT_SUBMITTED : UCN_DRIVER_COMPLETE;
    }
    if (p[0] == 0x60U) {
        assert(ucn_i_c0_decode(p, n, nodes[from].width, UCN_I_CONTROL_H1, &frame) == UCN_OK);
        if (from == 1U && frame.opcode == 0x0201U) { memcpy(forwarded_request, frame.payload, 54U); }
        if ((scenario_mode == 21U || scenario_mode == 22U) && from == 2U && frame.opcode == 0x0206U) {
            assert(ucn_i_route_fragment_decode(frame.payload, frame.payload_bytes, &fragment) == UCN_OK);
            assert(fragment.count == 2U);
            if (!delayed_reply_bytes[fragment.index]) {
                memcpy(delayed_reply[fragment.index], p, n); delayed_reply_bytes[fragment.index] = n;
            }
            return UCN_DRIVER_COMPLETE;
        }
        if (reverse_expiry && ((scenario_mode == 19U && from == 1U && to == 2U) ||
            (scenario_mode == 20U && from == 2U) || ((scenario_mode == 21U || scenario_mode == 22U) && from == 1U && to == 0U))) {
            assert(wire_now < reverse_expiry); ++late_submits;
            if (scenario_mode == 22U) {
                unknown_token = token; unknown_pointer = p;
                memcpy(first_frame, p, n); first_frame_bytes = n; return UCN_DRIVER_SUBMIT_UNKNOWN;
            }
            return UCN_DRIVER_NOT_SUBMITTED;
        }
        if (scenario_mode == 15U && frame.opcode == 0x0206U) {
            assert(ucn_i_route_fragment_decode(frame.payload, frame.payload_bytes, &fragment) == UCN_OK);
            if (fragment.opcode == 0x0203U) { assert(fragment.count == 3U && n == 112U); }
        }
        if (from == 0U && frame.opcode == 0x0201U) {
            if (!source_requests) { memcpy(first_request, frame.payload, 54U); first_transaction = frame.transaction_id; }
            else { assert(memcmp(first_request, frame.payload, 54U) == 0); }
            if (scenario_mode == 3U && not_submitted < 2U) {
                if (!not_submitted) { memcpy(first_frame, p, n); first_frame_bytes = n; }
                else { assert(first_frame_bytes == n && memcmp(first_frame, p, n) == 0); }
                ++not_submitted; ++source_requests; return UCN_DRIVER_NOT_SUBMITTED;
            }
            if (scenario_mode == 3U && source_requests == 2U) { assert(first_frame_bytes == n && memcmp(first_frame, p, n) == 0); }
            if ((scenario_mode == 1U || scenario_mode == 2U || scenario_mode == 5U) && source_requests) { assert(frame.transaction_id > first_transaction); }
            ++source_requests;
            if (scenario_mode == 6U) {
                unknown_token = token; unknown_pointer = p;
                memcpy(first_frame, p, n); first_frame_bytes = n; return UCN_DRIVER_SUBMIT_UNKNOWN;
            }
            if (scenario_mode == 4U) { ++dropped; return UCN_DRIVER_COMPLETE; }
        }
        if (frame.opcode == 0x0206U && (scenario_mode == 1U || scenario_mode == 2U) && !dropped) {
            assert(ucn_i_route_fragment_decode(frame.payload, frame.payload_bytes, &fragment) == UCN_OK);
            if (fragment.opcode == 0x0202U && from == (scenario_mode == 1U ? 2U : 1U) &&
                fragment.index == (scenario_mode == 1U ? 1U : 0U)) { ++dropped; return UCN_DRIVER_COMPLETE; }
        }
        if (scenario_mode == 10U && frame.opcode == 0x0206U && from == 1U && to == 0U) {
            unknown_token = token; unknown_pointer = p;
            memcpy(first_frame, p, n); first_frame_bytes = n; return UCN_DRIVER_SUBMIT_UNKNOWN;
        }
        if ((scenario_mode == 16U || scenario_mode == 17U) && frame.opcode == 0x0206U && from == 1U && to == 0U) {
            assert(ucn_i_route_fragment_decode(frame.payload, frame.payload_bytes, &fragment) == UCN_OK);
            if (!capped_bundle) { capped_bundle = fragment.bundle; }
            if (fragment.bundle == capped_bundle) {
                ++capped_submits;
                assert(capped_submits <= 16U && controls[1].jobs[0].total_attempts == capped_submits);
                assert(fragment.index == (capped_submits == 16U ? 1U : 0U));
                if (capped_submits < 15U) { return UCN_DRIVER_NOT_SUBMITTED; }
                if (capped_submits == 16U) {
                    if (scenario_mode == 16U) { return UCN_DRIVER_NOT_SUBMITTED; }
                    unknown_token = token; unknown_pointer = p;
                    memcpy(first_frame, p, n); first_frame_bytes = n; return UCN_DRIVER_SUBMIT_UNKNOWN;
                }
            } else { assert(capped_submits == 16U); }
        }
    }
    assert(ucn_i_adapter_link_handle(&adapters[to], (uint16_t)slot, &target) == UCN_OK);
    meta.struct_size = sizeof(meta); meta.api_version = UCN_API_VERSION;
    meta.timestamp_us = wire_now; meta.sender_discriminator = from + 1U;
    if (scenario_mode == 5U && !dropped && p[0] == 0x60U) {
        uint8_t broken[127]; memcpy(broken, p, n); broken[n - 1U] ^= 1U; ++dropped;
        assert(ucn_i_adapter_rx_publish(&adapters[to], target, broken, n, &meta) == UCN_OK);
    } else {
        ucn_result_t published = ucn_i_adapter_rx_publish(&adapters[to], target, p, n, &meta);
        assert(published == UCN_OK || ((scenario_mode == 10U || scenario_mode == 17U) && to == 1U && slot == 0U && published == UCN_ERR_STATE));
    }
    ++wire_frames;
    return UCN_DRIVER_COMPLETE;
}
static int scenario(unsigned mode)
{
    ucn_i_security_candidate_t sc; ucn_i_security_binding_t a, b, c;
    ucn_i_native_routed_config_t cfg = {0};
    ucn_native_channel_t channel = {0}; ucn_native_config_t nc = {0};
    ucn_native_handle_t pub, sub; uint16_t delivered; unsigned i, j;
    const uint8_t payload[4] = {0x11, 0x22, 0x33, 0x44};
    scenario_mode = mode; dropped = 0U; source_requests = 0U; not_submitted = 0U; wire_frames = 0U;
    business_during_control = false;
    capped_bundle = 0U; capped_submits = 0U;
    reverse_expiry = 0U; late_submits = 0U; memset(delayed_reply_bytes, 0, sizeof(delayed_reply_bytes));
    routed_address_width = mode == 14U ? 2U : mode == 15U ? 4U : 1U;
    unknown_pointer = NULL; memset(&unknown_token, 0, sizeof(unknown_token));
    memset(nodes, 0, sizeof(nodes)); memset(adapters, 0, sizeof(adapters)); memset(routes, 0, sizeof(routes));
    memset(alocks, 0, sizeof(alocks)); memset(dlocks, 0, sizeof(dlocks)); memset(rlocks, 0, sizeof(rlocks)); memset(nlocks, 0, sizeof(nlocks));
    memset(nstorage, 0, sizeof(nstorage)); memset(controls, 0, sizeof(controls)); memset(callbacks, 0, sizeof(callbacks));
    for (i = 0U; i < 5U; ++i) { counters[i] = (i + 1U) * 100U; }
    (void)routed_roundtrip; (void)independent_hop;
    binding_set(&a, 1U, 101U, 0x10U); binding_set(&b, 2U, 102U, 0x30U); binding_set(&c, 3U, 103U, 0x50U);
    make_h1_candidate(&sc, &a, &c, 1U, 1U, 21U, 22U, 0x80U); fixture_pair(&ac, &ca, &sc, 0U);
    make_h1_candidate(&sc, &a, &b, 2U, 1U, 31U, 32U, 0x90U); fixture_pair(&ab, &ba, &sc, 2U);
    make_h1_candidate(&sc, &b, &c, 3U, 1U, 41U, 42U, 0xA0U); fixture_pair(&bc, &cb, &sc, 4U);
    ab.owner.provider_gate = ac.owner.provider_gate; bc.owner.provider_gate = ba.owner.provider_gate;
    cb.owner.provider_gate = ca.owner.provider_gate;
    allow_controls();
    authorization_checks();
    endpoint(0, 0, &ac); endpoint(1, 0, &ba); endpoint(2, 0, &ca);
    neighbor(0, 0, &ab); neighbor(1, 0, &ba); neighbor(1, 1, &bc); neighbor(2, 0, &cb);
    if (mode == 18U) {
        /* A-B advertises 3, B-C only 1; the business demand requires bit 1. */
        neighbors[1][1].capability_bits = 1U; neighbors[2][0].capability_bits = 1U;
    }
    memcpy(neighbors[0][0].control_fingerprint, ab.candidate.origin_fingerprint, 16U);
    memcpy(neighbors[1][0].control_fingerprint, ba.candidate.origin_fingerprint, 16U);
    memcpy(neighbors[1][1].control_fingerprint, bc.candidate.origin_fingerprint, 16U);
    memcpy(neighbors[2][0].control_fingerprint, cb.candidate.origin_fingerprint, 16U);
    setup_node(0, &a); setup_node(1, &b); setup_node(2, &c);
    if (mode == 7U || mode == 8U || mode == 11U) {
        neighbor(0, 1, &ac); memcpy(neighbors[0][1].control_fingerprint, ac.candidate.origin_fingerprint, 16U);
        adapters[0].links[1] = adapters[0].links[0]; adapters[0].link_count = 2U;
    }
    route_binding(&flow.origin, &a); route_binding(&flow.destination, &c);
    flow.realm = 0x12345678U; flow.origin_session_generation = 17U;
    for (i = 0U; i < 3U; ++i) {
        for (j = 0U; j < adapters[i].link_count; ++j) {
            adapters[i].links[j].frame_mtu = 127U; adapters[i].links[j].tx.submit = wire_submit;
        }
        cfg.adapter = &adapters[i]; cfg.routes = &routes[i];
        cfg.gate = i == 0U ? ac.owner.provider_gate : i == 1U ? ba.owner.provider_gate : ca.owner.provider_gate;
        cfg.local.address = i + 1U; cfg.local.binding_generation = 101U + i; cfg.local.principal_digest = i + 1U;
        cfg.now_us = 1500U; cfg.expires_at_us = 3000U; cfg.relay_lifetime_us = 200U;
        cfg.endpoints = eps[i]; cfg.endpoint_count = 1U;
        cfg.neighbors = neighbors[i]; cfg.neighbor_count = i == 1U || (i == 0U && (mode == 7U || mode == 8U || mode == 11U)) ? 2U : 1U;
        cfg.forward_domains = i == 1U ? &flow : NULL; cfg.forward_count = i == 1U ? 1U : 0U; cfg.hop_limit = 3U;
        cfg.control = &controls[i]; cfg.reserve_c0 = counter_reserve;
        assert(ucn_i_native_routed_init(&nodes[i], &cfg) == UCN_OK);
    }
    channel.id = 1U; channel.service_id = 0x1201U; channel.schema_id = 1U;
    channel.schema_revision = 1U; channel.payload_bytes = 4U; channel.traffic_class = UCN_TRAFFIC_Q1;
    channel.queue_policy = UCN_NATIVE_KEEP_LATEST; channel.security_minimum = UCN_NATIVE_ORIGIN_AUTH;
    channel.max_local_age_us = 400U; channel.source_count = 1U; channel.sources[0] = nodes[0].local;
    channel.target_count = 1U; channel.targets[0].address = 3U; channel.targets[0].binding_generation = 103U;
    nc.api_version = UCN_NATIVE_API_VERSION; nc.storage_layout = UCN_NATIVE_LAYOUT;
    nc.manifest_hash = UCN_NATIVE_MANIFEST_HASH; nc.channels = &channel; nc.channel_count = 1U;
    nc.lock.struct_size = sizeof(nc.lock); nc.lock.api_version = UCN_API_VERSION;
    nc.lock.enter = lock_enter; nc.lock.leave = lock_leave;
    for (i = 0U; i < 3U; ++i) {
        nc.instance = 301U + i; nc.lock.context = &nlocks[i];
        assert(ucn_native_init_backend(&nstorage[i], sizeof(nstorage[i]), &nodes[i].port, &nc, &native[i]) == UCN_OK);
        if (i == 2U) { assert(ucn_subscription_create(native[i], 1U, received_sample, (void *)(uintptr_t)i, &sub) == UCN_OK); }
        assert(ucn_native_start(native[i], 1500U) == UCN_OK); channel.target_count = 0U;
    }
    assert(ucn_publisher_create(native[0], 1U, &pub) == UCN_OK);
    if (mode == 8U) {
        ucn_link_handle_t down;
        assert(ucn_i_adapter_link_handle(&adapters[0], 1U, &down) == UCN_OK);
        assert(ucn_i_adapter_link_event(&adapters[0], down, UCN_DRIVER_LINK_DOWN, NULL) == UCN_OK);
    }
    assert(ucn_publish(native[0], pub, payload, sizeof(payload)) == UCN_OK);
    for (wire_now = 1501U; wire_now < (mode == 6U ? 1520U : mode == 4U ? 1950U : 1840U); ++wire_now) {
        /* Fixed time distinguishes a hard submit budget from deadline-only expiry. */
        unsigned rounds = mode == 11U && wire_now == 1501U ? 64U : 1U;
        for (j = 0U; j < rounds; ++j) {
            for (i = 0U; i < 3U; ++i) { assert(ucn_native_step(native[i], wire_now, 4U) == UCN_OK); }
            assert(ucn_native_dispatch(native[2], wire_now, 4U, &delivered) == UCN_OK);
        }
        if (mode == 11U && wire_now == 1501U) { assert(dropped == 16U && !controls[0].jobs[1].occupied); }
    }
    if (mode == 4U || mode == 6U || mode == 10U || mode == 17U || mode == 21U || mode == 22U) {
        assert(callbacks[2] == 0U && ac.provider.protect_origin_calls == 0U);
    } else {
        assert(callbacks[2] == 1U); /* RED: the data-only backend cannot discover A-B-C. */
        assert(wire_frames >= 8U);
    }
    if (mode == 1U || mode == 2U || mode == 5U) { assert(dropped == 1U && source_requests >= 2U); }
    if (mode == 3U) { assert(not_submitted == 2U); }
    if (mode == 7U) { assert(dropped >= 1U); }
    if (mode == 8U) { assert(dropped == 0U); }
    if (mode == 11U) {
        assert(dropped == 16U && business_during_control && callbacks[2] == 1U);
        assert(!controls[0].jobs[1].occupied);
    }
    if (mode == 16U || mode == 17U) { assert(capped_submits == 16U); }
    if (mode >= 19U && mode <= 22U) {
        unsigned target = mode == 20U ? 2U : 1U;
        unsigned job = mode == 19U ? 1U : 0U;
        reverse_expiry = routes[target].reverse[0].expires_at_us;
        assert(routes[target].reverse[0].occupied && reverse_expiry > wire_now);
        wire_now = reverse_expiry - 2U;
        if (mode <= 20U) {
            protected_input(mode == 19U ? 0U : 2U, 0x0201U, UCN_INTERACTION_REQUEST,
                mode == 19U ? first_request : forwarded_request, 54U, 0U);
        } else {
            ucn_link_handle_t link; ucn_rx_meta_t meta = {0};
            assert(ucn_i_adapter_link_handle(&adapters[1], 1U, &link) == UCN_OK);
            meta.struct_size = sizeof(meta); meta.api_version = UCN_API_VERSION;
            meta.timestamp_us = wire_now; meta.sender_discriminator = 3U;
            for (j = 0U; j < 2U; ++j) {
                assert(delayed_reply_bytes[j]);
                assert(ucn_i_adapter_rx_publish(&adapters[1], link, delayed_reply[j], delayed_reply_bytes[j], &meta) == UCN_OK);
                assert(ucn_native_step(native[1], wire_now, 4U) == UCN_OK);
                assert(ucn_native_step(native[1], wire_now, 4U) == UCN_OK);
            }
        }
        assert(ucn_native_step(native[target], wire_now, 4U) == UCN_OK);
        assert(controls[target].jobs[job].occupied);
        assert(controls[target].jobs[job].deadline == reverse_expiry);
        assert(routes[target].reverse[0].expires_at_us == reverse_expiry);
        assert(ucn_native_step(native[target], wire_now, 4U) == UCN_OK);
        assert(late_submits > 0U);
        wire_now = reverse_expiry;
        for (j = 0U; j < 4U; ++j) { assert(ucn_native_step(native[target], wire_now, 4U) == UCN_OK); }
        if (mode != 22U) { assert(!controls[target].jobs[job].occupied); }
        else { assert(controls[target].jobs[job].live && unknown_pointer && routes[target].reverse[0].reply_pending); }
    }
    if (mode == 12U || mode == 13U) {
        unsigned before = ac.provider.protect_origin_calls;
        assert(nodes[0].port.send(&nodes[0], &channel, channel.targets[0], payload, wire_now, wire_now + 1U) == UCN_OK);
        assert(nodes[0].tx_pending && ac.provider.protect_origin_calls == before);
        if (mode == 13U) { assert(ucn_i_security_fence(&ac.owner, ac.handle) == UCN_OK); }
        assert(nodes[0].port.step(&nodes[0], mode == 12U ? wire_now + 1U : wire_now, 4U) == UCN_OK);
        assert(!nodes[0].tx_pending && ac.provider.protect_origin_calls == before);
    }
    if (mode == 0U || mode == 15U) {
        ucn_link_handle_t link; ucn_link_event_meta_t meta = {0};
        ucn_i_route_forward_key_t key = {0}; ucn_i_route_resolved_t resolved;
        if (mode == 0U) { negative_inputs(); }
        assert(ucn_i_adapter_link_handle(&adapters[1], 1U, &link) == UCN_OK);
        meta.struct_size = sizeof(meta); meta.api_version = UCN_API_VERSION;
        assert(ucn_i_adapter_link_event(&adapters[1], link, UCN_DRIVER_LINK_DOWN, &meta) == UCN_OK);
        for (; wire_now < 1940U; ++wire_now) {
            for (i = 0U; i < 3U; ++i) { assert(ucn_native_step(native[i], wire_now, 4U) == UCN_OK); }
        }
        key.now_us = wire_now; key.realm = flow.realm; key.source_address = 1U;
        key.source_binding_generation = 101U; key.destination_address = 3U; key.destination_binding_generation = 103U;
        assert(ucn_i_route_forward_resolve(&routes[0], &key, &resolved) == UCN_ERR_NOT_FOUND);
    }
    if (mode == 0U) {
        ucn_i_route_link_ref_t out, before;
        ucn_i_route_discovery_key_t key = controls[0].discoveries[UCN_I_ROUTE_DISCOVERY_COUNT - 1U].request.key;
        memset(&out, 0xA5, sizeof(out)); before = out;
        ++key.realm;
        assert(ucn_i_route_reverse_peek(&routes[1], &key, wire_now, &out) != UCN_OK);
        assert(memcmp(&out, &before, sizeof(out)) == 0);
        --key.realm; key.transaction_id += 100U;
        assert(ucn_i_route_reverse_peek(&routes[1], &key, wire_now, &out) != UCN_OK);
        assert(memcmp(&out, &before, sizeof(out)) == 0);
        key.transaction_id -= 100U;
        assert(ucn_i_route_reverse_peek(&routes[1], &key, 2500U, &out) != UCN_OK);
        assert(memcmp(&out, &before, sizeof(out)) == 0);
    }
    if (mode == 6U || mode == 10U || mode == 17U || mode == 22U) {
        ucn_tx_meta_t meta = {0};
        unsigned holder = mode == 6U ? 0U : 1U;
        ucn_handle_t completion = controls[holder].jobs[0].completion;
        assert(unknown_pointer != NULL && controls[holder].jobs[0].live);
        if (mode == 10U || mode == 17U) {
            ucn_link_handle_t link; ucn_link_event_meta_t reopen = {0};
            assert(completion.slot && routes[1].reverse[completion.slot - 1U].reply_pending);
            assert(ucn_i_adapter_link_handle(&adapters[1], 0U, &link) == UCN_OK);
            reopen.struct_size = sizeof(reopen); reopen.api_version = UCN_API_VERSION; reopen.new_link_instance = 2U;
            assert(ucn_i_adapter_link_event(&adapters[1], link, UCN_DRIVER_LINK_REOPENED, &reopen) == UCN_OK);
        }
        uncertain = true;
        assert(ucn_native_stop(native[holder]) == UCN_OK);
        for (j = 0U; j < 4U; ++j) { assert(ucn_native_step(native[holder], 2050U, 4U) == UCN_OK); }
        assert(ucn_native_deinit(native[holder]) == UCN_ERR_STATE);
        assert(controls[holder].jobs[0].live && memcmp(unknown_pointer, first_frame, first_frame_bytes) == 0);
        if (mode == 10U || mode == 17U || mode == 22U) { assert(completion.slot && routes[1].reverse[completion.slot - 1U].reply_pending); }
        meta.struct_size = sizeof(meta); meta.api_version = UCN_API_VERSION; meta.timestamp_us = 2051U;
        assert(ucn_i_adapter_tx_complete(&adapters[holder], unknown_token, UCN_OK, &meta) == UCN_OK);
        for (j = 0U; j < 4U; ++j) { assert(ucn_native_step(native[holder], 2051U, 4U) == UCN_OK); }
        assert(!controls[holder].jobs[0].live); uncertain = false;
        if (mode == 10U || mode == 17U || mode == 22U) { assert(!routes[1].reverse[completion.slot - 1U].reply_pending); }
    }
    if (mode == 9U) { assembly_timeout(); }
    for (i = 0U; i < 3U; ++i) {
        assert(ucn_native_stop(native[i]) == UCN_OK);
        for (j = 0U; j < 8U; ++j) { assert(ucn_native_step(native[i], mode == 9U ? 2500U : 2100U, 4U) == UCN_OK); }
        assert(ucn_native_deinit(native[i]) == UCN_OK);
    }
    printf("Native empty-route A-B-C / 127B scenario %u passed\n", mode);
    return 0;
}
int main(void)
{
    unsigned mode;
    for (mode = 0U; mode <= 22U; ++mode) { assert(scenario(mode) == 0); }
    printf("Caller-owned control=%zu, routed=%zu bytes (host ABI)\n",
        sizeof(ucn_i_native_route_control_t), sizeof(ucn_i_native_routed_t));
    return 0;
}
