#include "internal/ucn_native_routed.h"
#include "internal/ucn_stack.h"
#include "internal/ucn_native_route_control.h"
#include "internal/ucn_checked.h"
#include <string.h>

static uint32_t address_read(const uint8_t *p, uint8_t width)
{
    uint32_t v = 0U;
    while (width--) { v = (v << 8U) | *p++; }
    return v;
}
static bool binding_matches(const ucn_i_route_binding_t *r,
    const ucn_i_security_binding_t *s)
{
    return r->address == s->address && r->generation == s->binding_generation &&
        memcmp(r->principal, s->principal, 16U) == 0;
}
static void binding_copy(ucn_i_route_binding_t *r, const ucn_i_security_binding_t *s)
{
    r->address = s->address; r->generation = s->binding_generation;
    memcpy(r->principal, s->principal, 16U);
}
static bool live(const ucn_i_native_routed_t *b, uint64_t now)
{ return now >= b->now && now < b->expires_at; }
static UCN_I_STACK_PHASE bool gate_idle(const ucn_i_native_routed_t *b)
{
    bool active = true; ucn_i_callback_claim_t claim;
    return ucn_i_callback_gate_view(b->gate, &active, &claim) == UCN_OK && !active;
}
static bool output_safe(const ucn_i_native_routed_t *b, const void *out, size_t n)
{
    uint16_t i;
    if (!out || !n || ucn_i_ranges_overlap(b, sizeof(*b), out, n) ||
        ucn_i_ranges_overlap(b->adapter, sizeof(*b->adapter), out, n) ||
        ucn_i_ranges_overlap(b->routes, sizeof(*b->routes), out, n) ||
        (b->control && ucn_i_ranges_overlap(b->control, sizeof(*b->control), out, n)) ||
        ucn_i_ranges_overlap(b->gate, sizeof(*b->gate), out, n)) { return false; }
    for (i = 0U; i < b->endpoint_count; ++i) {
        if (ucn_i_ranges_overlap(b->endpoints[i].security, sizeof(*b->endpoints[i].security), out, n)) { return false; }
    }
    for (i = 0U; i < b->neighbor_count; ++i) {
        if (ucn_i_ranges_overlap(b->neighbors[i].security, sizeof(*b->neighbors[i].security), out, n)) { return false; }
    }
    return true;
}

static UCN_I_STACK_PHASE ucn_result_t endpoint_current(ucn_i_native_routed_t *b, uint16_t slot,
    uint8_t direction, uint64_t now)
{
    const ucn_i_native_endpoint_t *e = &b->endpoints[slot];
    ucn_result_t r;
    if (!live(b, now)) { return UCN_ERR_STATE; }
    b->current_facts = e->facts; b->current_facts.now_us = now;
    r = ucn_i_security_c1_context_check(e->security, e->session, &b->current_facts, e->fingerprint, NULL);
    if (r != UCN_OK) { return r; }
    memset(&b->access, 0, sizeof(b->access));
    b->access.session = e->session; b->access.service_id = e->service;
    b->access.direction = direction; memcpy(b->access.context_fingerprint, e->fingerprint, 16U);
    r = ucn_i_security_authorize(e->security, &b->access, &b->current_facts);
    if (r != UCN_OK) { return r; }
    r = ucn_i_security_session_get(e->security, e->session, &b->session_view);
    if (r != UCN_OK || b->session_view.origin_level + 1U < e->minimum) { return UCN_ERR_SECURITY; }
    return UCN_OK;
}
static ucn_result_t neighbor_current(ucn_i_native_routed_t *b, uint16_t slot,
    uint16_t service, uint8_t direction, size_t bytes)
{
    const ucn_i_native_neighbor_t *n = &b->neighbors[slot];
    uint32_t generation; uint16_t mtu; bool ready;
    ucn_result_t r;
    if (!live(b, b->now)) { return UCN_ERR_STATE; }
    r = ucn_i_adapter_link_snapshot(b->adapter, n->link_slot, &generation, &mtu, &ready);
    if (r != UCN_OK || !ready || generation != n->facts.link_generation) { return UCN_ERR_STATE; }
    if (bytes > mtu) { return UCN_ERR_NO_SPACE; }
    b->current_facts = n->facts; b->current_facts.now_us = b->now;
    r = ucn_i_security_c1_context_check(n->security, n->session, &b->current_facts, NULL, n->fingerprint);
    if (r != UCN_OK) { return r; }
    memset(&b->access, 0, sizeof(b->access));
    b->access.session = n->session; b->access.service_id = service; b->access.direction = direction;
    memcpy(b->access.context_fingerprint, n->fingerprint, 16U);
    r = ucn_i_security_authorize(n->security, &b->access, &b->current_facts);
    if (r != UCN_OK) { return r; }
    r = ucn_i_security_session_get(n->security, n->session, &b->session_view);
    if (r != UCN_OK || b->session_view.hop_profile != UCN_I_HOP_PROFILE_H1) { return UCN_ERR_SECURITY; }
    return UCN_OK;
}
static ucn_result_t route_next(ucn_i_native_routed_t *b,
    const ucn_i_route_domain_t *d, size_t bytes, uint16_t *neighbor)
{
    const ucn_i_route_link_ref_t *link;
    uint16_t i, mtu; ucn_result_t r;
    memset(&b->route_key, 0, sizeof(b->route_key));
    b->route_key.now_us = b->now; b->route_key.realm = d->realm;
    b->route_key.source_address = d->origin.address; b->route_key.source_binding_generation = d->origin.generation;
    b->route_key.destination_address = d->destination.address; b->route_key.destination_binding_generation = d->destination.generation;
    r = ucn_i_route_forward_resolve(b->routes, &b->route_key, &b->resolved);
    if (r != UCN_OK) { return r; }
    if (b->resolved.kind == UCN_I_ROUTE_RESOLVED_STATIC) {
        if (memcmp(&b->resolved.static_route.destination, &d->destination, sizeof(d->destination)) != 0) { return UCN_ERR_ACCESS; }
        link = &b->resolved.static_route.next_hop; mtu = b->resolved.static_route.path_frame_mtu;
    } else {
        if (memcmp(&b->resolved.dynamic.domain, d, sizeof(*d)) != 0) { return UCN_ERR_ACCESS; }
        link = &b->resolved.dynamic.next_hop; mtu = b->resolved.dynamic.path_frame_mtu;
    }
    if (bytes > mtu || bytes > link->frame_mtu) { return UCN_ERR_NO_SPACE; }
    for (i = 0U; i < b->neighbor_count; ++i) {
        const ucn_i_native_neighbor_t *n = &b->neighbors[i];
        /* Existing Route Owner uses positive logical Link IDs; this assembly
         * contract maps id = Adapter slot + 1, with a dedicated physical Link. */
        if (link->link_id == n->link_slot + 1U && link->link_generation == n->facts.link_generation &&
            binding_matches(&link->peer, &n->facts.peer) &&
            (link->capability_bits & n->capability_bits) == n->capability_bits) {
            *neighbor = i; return UCN_OK;
        }
    }
    return UCN_ERR_NOT_FOUND;
}
static ucn_result_t local_view(void *ctx, ucn_node_view_t *out)
{
    ucn_i_native_routed_t *b = ctx;
    if (!output_safe(b, out, sizeof(*out))) { return UCN_ERR_ARGUMENT; }
    if (!gate_idle(b)) { return UCN_ERR_STATE; }
    memset(out, 0, sizeof(*out)); out->local_address = b->local.address;
    out->local_binding_generation = b->local.binding_generation; out->lifecycle = b->lifecycle;
    return UCN_OK;
}
static ucn_result_t binding(void *ctx, uint32_t address, ucn_native_source_t *out)
{
    ucn_i_native_routed_t *b = ctx; uint16_t i;
    if (!output_safe(b, out, sizeof(*out))) { return UCN_ERR_ARGUMENT; }
    if (!gate_idle(b)) { return UCN_ERR_STATE; }
    if (address == b->local.address) { *out = b->local; return UCN_OK; }
    for (i = 0U; i < b->endpoint_count; ++i) {
        if (b->endpoints[i].source.address == address) { *out = b->endpoints[i].source; return UCN_OK; }
    }
    return UCN_ERR_NOT_FOUND;
}
static ucn_result_t preflight(void *ctx, const ucn_native_channel_t *c)
{
    const ucn_i_native_routed_t *b = ctx; uint16_t i; bool matched = false;
    if (!gate_idle(b)) { return UCN_ERR_STATE; }
    if (!c || c->payload_bytes > UCN_NATIVE_PAYLOAD_BYTES ||
        (c->security_minimum != UCN_NATIVE_ORIGIN_AUTH && c->security_minimum != UCN_NATIVE_ENCRYPTED)) { return UCN_ERR_UNSUPPORTED; }
    for (i = 0U; i < b->endpoint_count; ++i) {
        if (b->endpoints[i].service == c->service_id) {
            ucn_i_security_session_view_t view;
            if (ucn_i_security_session_get(b->endpoints[i].security, b->endpoints[i].session, &view) != UCN_OK ||
                view.origin_level + 1U < c->security_minimum) { return UCN_ERR_UNSUPPORTED; }
            matched = true;
        }
    }
    if (!matched) { return UCN_ERR_UNSUPPORTED; }
    if (9U + 2U * b->width + 32U + c->payload_bytes > UCN_ADAPTER_FRAME_BYTES) { return UCN_ERR_NO_SPACE; }
    return UCN_OK;
}
static ucn_result_t start(void *ctx, uint64_t now)
{
    ucn_i_native_routed_t *b = ctx; ucn_result_t r;
    if (!gate_idle(b)) { return UCN_ERR_STATE; }
    if (b->lifecycle != UCN_LIFECYCLE_INITIALIZED || !live(b, now)) { return UCN_ERR_STATE; }
    r = ucn_i_adapter_set_rx_enabled(b->adapter, true);
    if (r == UCN_OK) { b->now = now; b->lifecycle = UCN_LIFECYCLE_RUNNING; }
    return r;
}
static ucn_result_t validate(void *ctx, const ucn_native_scope_t *scope,
    const ucn_native_source_t *source, uint16_t service, uint8_t minimum,
    uint64_t now, uint8_t principal[16])
{
    ucn_i_native_routed_t *b = ctx; uint16_t i; ucn_result_t r;
    if (!scope || !source || !output_safe(b, principal, 16U) ||
        ucn_i_ranges_overlap(scope, sizeof(*scope), principal, 16U) ||
        ucn_i_ranges_overlap(source, sizeof(*source), principal, 16U)) { return UCN_ERR_ARGUMENT; }
    if (!gate_idle(b)) { return UCN_ERR_STATE; }
    if (b->lifecycle != UCN_LIFECYCLE_RUNNING || !live(b, now) || scope->reserved_zero ||
        (minimum != UCN_NATIVE_ORIGIN_AUTH && minimum != UCN_NATIVE_ENCRYPTED)) { return UCN_ERR_ACCESS; }
    if (!scope->instance && !scope->slot && !scope->generation) {
        if (memcmp(source, &b->local, sizeof(*source)) != 0) { return UCN_ERR_ACCESS; }
        for (i = 0U; i < b->endpoint_count; ++i) {
            if (b->endpoints[i].service == service && endpoint_current(b, i, UCN_I_SECURITY_ACCESS_OUTBOUND, now) == UCN_OK &&
                b->session_view.origin_level + 1U >= minimum) {
                memset(principal, 0, 16U); return UCN_OK;
            }
        }
        return UCN_ERR_ACCESS;
    }
    if (scope->instance != b->instance || scope->generation != 1U || !scope->slot || scope->slot > b->endpoint_count) { return UCN_ERR_ACCESS; }
    i = (uint16_t)(scope->slot - 1U);
    if (service != b->endpoints[i].service || memcmp(source, &b->endpoints[i].source, sizeof(*source)) != 0) { return UCN_ERR_ACCESS; }
    r = endpoint_current(b, i, UCN_I_SECURITY_ACCESS_INBOUND, now);
    if (r != UCN_OK) { return r; }
    if (b->session_view.origin_level + 1U < minimum) { return UCN_ERR_SECURITY; }
    memcpy(principal, b->current_facts.peer.principal, 16U); return UCN_OK;
}
static ucn_result_t physical(ucn_i_native_routed_t *b, bool cancel)
{
    ucn_result_t r, end;
    b->open_work.callback.owner_instance = b->instance; b->open_work.callback.operation_id = b->tx.generation;
    b->open_work.callback.operation_generation = 1U; b->open_work.callback.operation_kind = cancel ? 0x0713U : 0x0712U;
    r = ucn_i_callback_gate_enter(b->gate, &b->open_work.callback);
    if (r != UCN_OK) { return r; }
    r = cancel ? ucn_i_adapter_tx_cancel(b->adapter, &b->tx) :
        ucn_i_adapter_tx_submit_ref(b->adapter, &b->tx, b->frame, b->frame_bytes, &b->tx_view);
    end = ucn_i_callback_gate_leave(b->gate, &b->open_work.callback);
    if (end != UCN_OK) { b->lifecycle = UCN_LIFECYCLE_FAULT; return end; }
    return r;
}
static void retire(ucn_i_native_routed_t *b, bool preserve)
{
    if (ucn_i_adapter_tx_retire(b->adapter, &b->tx) == UCN_OK) {
        b->tx_live = 0U;
        if (!preserve) { b->tx_prepared = 0U; b->frame_bytes = 0U; }
    }
}
static ucn_result_t stop(void *ctx)
{
    ucn_i_native_routed_t *b = ctx;
    if (!gate_idle(b)) { return UCN_ERR_STATE; }
    b->lifecycle = UCN_LIFECYCLE_STOPPING; b->tx_prepared = 0U; b->rx_ready = 0U; b->tx_pending = 0U;
    return ucn_i_adapter_set_rx_enabled(b->adapter, false);
}
static UCN_I_STACK_PHASE bool tx_current(ucn_i_native_routed_t *b)
{
    uint16_t next;
    if (b->lifecycle != UCN_LIFECYCLE_RUNNING || b->now >= b->tx_deadline || !live(b, b->now)) { return false; }
    if (b->tx_endpoint != UINT16_MAX && endpoint_current(b, b->tx_endpoint, UCN_I_SECURITY_ACCESS_OUTBOUND, b->now) != UCN_OK) { return false; }
    if (route_next(b, &b->tx_domain, b->frame_bytes, &next) != UCN_OK || next != b->tx_neighbor) { return false; }
    return neighbor_current(b, next, b->tx_service, UCN_I_SECURITY_ACCESS_OUTBOUND, b->frame_bytes) == UCN_OK;
}
static UCN_I_STACK_PHASE ucn_result_t tx_progress(ucn_i_native_routed_t *b)
{
    ucn_result_t r;
    if (b->tx_live) {
        r = ucn_i_adapter_tx_view(b->adapter, &b->tx, &b->tx_view);
        if (r != UCN_OK) { return r; }
        if (b->tx_view.state == UCN_I_ADAPTER_TX_COMPLETED || b->tx_view.state == UCN_I_ADAPTER_TX_CANCELLED) { retire(b, false); }
        else if (!b->tx_prepared || !tx_current(b)) {
            b->tx_prepared = 0U;
            if (physical(b, true) == UCN_OK) { retire(b, false); }
        } else if (b->tx_view.state == UCN_I_ADAPTER_TX_NOT_SUBMITTED) { retire(b, true); }
        else if (b->tx_view.state == UCN_I_ADAPTER_TX_RESERVED) { (void)physical(b, false); }
    }
    if (!b->tx_live && b->tx_prepared) {
        const ucn_i_native_neighbor_t *n = &b->neighbors[b->tx_neighbor];
        if (!tx_current(b)) { b->tx_prepared = 0U; return UCN_OK; }
        r = ucn_i_adapter_tx_reserve(b->adapter, n->link_slot, n->facts.link_generation, 0U, &b->tx);
        if (r == UCN_OK) { b->tx_live = 1U; (void)physical(b, false); }
        else if (r != UCN_ERR_NO_SPACE && r != UCN_ERR_STATE) { return r; }
    }
    return UCN_OK;
}
static UCN_I_STACK_PHASE ucn_result_t protect_business(ucn_i_native_routed_t *b);
static UCN_I_STACK_PHASE void protect_forward(ucn_i_native_routed_t *b);
static UCN_I_STACK_PHASE ucn_result_t drain(ucn_i_native_routed_t *b, uint16_t budget)
{
    ucn_result_t r; uint16_t i; bool quiet = false;
    r = ucn_i_adapter_set_rx_enabled(b->adapter, false);
    if (r != UCN_OK) { return r; }
    for (i = 0U; i < budget; ++i) {
        if (!b->rx_live) {
            r = ucn_i_adapter_rx_claim(b->adapter, &b->rx);
            if (r == UCN_ERR_NOT_FOUND) { break; }
            if (r != UCN_OK) { return r; }
            b->rx_live = 1U;
        }
        r = ucn_i_adapter_rx_retire(b->adapter, &b->rx.token);
        if (r != UCN_OK) { return r; }
        b->rx_live = 0U; b->rx_ready = 0U;
    }
    if (!b->tx_live && ucn_i_native_route_control_quiet(b) &&
        ucn_i_adapter_quiescent(b->adapter, &quiet) == UCN_OK && quiet) { b->lifecycle = UCN_LIFECYCLE_QUIESCENT; }
    return UCN_OK;
}
static ucn_result_t step(void *ctx, uint64_t now, uint16_t budget)
{
    ucn_i_native_routed_t *b = ctx; ucn_result_t r;
    if (!gate_idle(b)) { return UCN_ERR_STATE; }
    if (!budget || now < b->now) { return UCN_ERR_STATE; }
    b->now = now;
    if (b->tx_pending) {
        b->tx_pending = 0U;
        if (tx_current(b) && endpoint_current(b, b->tx_endpoint, UCN_I_SECURITY_ACCESS_OUTBOUND, now) == UCN_OK) {
            if (protect_business(b) == UCN_OK) { protect_forward(b); }
        }
    }
    r = tx_progress(b);
    if (r != UCN_OK) { return r; }
    if (b->control) {
        ucn_i_native_route_control_dispatch(b);
        ucn_i_native_route_control_prepare(b);
        ucn_i_native_route_control_drive(b);
        ucn_i_native_route_control_discovery_step(b);
        ucn_i_native_route_control_receipt_step(b);
        r = ucn_i_native_route_control_step(b); if (r != UCN_OK) { return r; }
    }
    if (b->lifecycle == UCN_LIFECYCLE_STOPPING) {
        r = drain(b, budget); if (r != UCN_OK) { return r; }
    }
    return b->lifecycle == UCN_LIFECYCLE_FAULT ? UCN_ERR_STATE : UCN_OK;
}
static UCN_I_STACK_PHASE ucn_result_t protect_business(ucn_i_native_routed_t *b)
{
    const ucn_i_native_endpoint_t *e = &b->endpoints[b->tx_endpoint];
    ucn_result_t r;
    r = ucn_i_security_tx_reserve(e->security, &b->access, &b->current_facts, &b->origin_tx);
    if (r != UCN_OK) { return r; }
    memset(&b->open_work.material, 0, sizeof(b->open_work.material)); b->open_work.material.session = e->session; b->open_work.material.facts = b->current_facts;
    b->open_work.material.payload_bytes = b->tx_payload_bytes; b->open_work.material.source_address = b->local.address;
    b->open_work.material.destination_address = b->tx_domain.destination.address; b->open_work.material.origin_sequence = b->origin_tx.origin_sequence;
    b->open_work.material.service_id = b->tx_service; b->open_work.material.direction = UCN_I_SECURITY_ACCESS_OUTBOUND;
    b->open_work.material.common_header[0] = 0x61U; b->open_work.material.common_header[1] = (uint8_t)(b->tx_traffic_class << 6U);
    b->open_work.material.common_header[2] = (uint8_t)((b->session_view.origin_level << 6U) | b->hop_limit);
    r = ucn_i_security_c1_seal_layer(true, e->security, b->origin_tx, &b->open_work.material, b->frame,
        &b->workspace, b->origin, sizeof(b->origin), &b->origin_bytes);
    if (r != UCN_OK) { (void)ucn_i_security_tx_abort(e->security, b->origin_tx); return r; }
    return UCN_OK;
}
static ucn_result_t send(void *ctx, const ucn_native_channel_t *c,
    ucn_native_target_t target, const uint8_t *payload, uint64_t now, uint64_t deadline)
{
    ucn_i_native_routed_t *b = ctx; uint16_t i, next; ucn_result_t r;
    const ucn_i_native_endpoint_t *e;
    if (!gate_idle(b)) { return UCN_ERR_STATE; }
    if (!c || !payload || ucn_i_ranges_overlap(b, sizeof(*b), payload, c->payload_bytes) ||
        ucn_i_ranges_overlap(b, sizeof(*b), c, sizeof(*c)) ||
        (b->control && (ucn_i_ranges_overlap(b->control, sizeof(*b->control), payload, c->payload_bytes) ||
            ucn_i_ranges_overlap(b->control, sizeof(*b->control), c, sizeof(*c))))) { return UCN_ERR_ARGUMENT; }
    if (b->lifecycle != UCN_LIFECYCLE_RUNNING || !live(b, now) || now >= deadline) { return UCN_ERR_STATE; }
    if (b->tx_live || b->tx_prepared || b->tx_pending) { return UCN_ERR_NO_SPACE; }
    r = preflight(b, c); if (r != UCN_OK) { return r; }
    for (i = 0U; i < b->endpoint_count; ++i) {
        if (b->endpoints[i].source.address == target.address && b->endpoints[i].source.binding_generation == target.binding_generation &&
            b->endpoints[i].service == c->service_id) { break; }
    }
    if (i == b->endpoint_count) { return UCN_ERR_ACCESS; }
    b->now = now; e = &b->endpoints[i];
    r = endpoint_current(b, i, UCN_I_SECURITY_ACCESS_OUTBOUND, now); if (r != UCN_OK) { return r; }
    if (b->session_view.origin_level + 1U < c->security_minimum) { return UCN_ERR_SECURITY; }
    memset(&b->domain, 0, sizeof(b->domain)); binding_copy(&b->domain.origin, &e->facts.local);
    binding_copy(&b->domain.destination, &e->facts.peer); b->domain.realm = e->security->realm_id;
    /* Route scope belongs to Route Owner, not the final business Session. */
    b->domain.origin_session_generation = b->routes->local_session_generation;
    r = route_next(b, &b->domain, 9U + 2U * b->width + 32U + c->payload_bytes, &next);
    if (r == UCN_ERR_NOT_FOUND && b->control) {
        return ucn_i_native_route_control_discover(b, target.address,
            (uint16_t)(9U + 2U * b->width + 32U + c->payload_bytes), deadline);
    }
    if (r != UCN_OK) { return r; }
    r = neighbor_current(b, next, c->service_id, UCN_I_SECURITY_ACCESS_OUTBOUND, 9U + 2U * b->width + 32U + c->payload_bytes);
    if (r != UCN_OK) { return r; }
    r = endpoint_current(b, i, UCN_I_SECURITY_ACCESS_OUTBOUND, now); if (r != UCN_OK) { return r; }
    /* Stage selected metadata without admitting a physical obligation. The
     * prepared flag remains clear until Origin and Hop protection both pass. */
    b->tx_neighbor = next; b->tx_endpoint = i; b->tx_service = c->service_id;
    b->tx_domain = b->domain; b->tx_deadline = deadline;
    b->tx_payload_bytes = c->payload_bytes; b->tx_traffic_class = c->traffic_class;
    b->frame_bytes = 9U + 2U * b->width + 32U + c->payload_bytes;
    memcpy(b->frame, payload, c->payload_bytes); b->tx_pending = 1U;
    return UCN_OK;
}

static UCN_I_STACK_PHASE ucn_result_t receive_endpoint(ucn_i_native_routed_t *b, uint32_t source, uint16_t service)
{
    uint16_t i; const ucn_i_native_endpoint_t *e; ucn_result_t r;
    for (i = 0U; i < b->endpoint_count; ++i) {
        if (b->endpoints[i].source.address == source && b->endpoints[i].service == service) { break; }
    }
    if (i == b->endpoint_count) { return UCN_ERR_ACCESS; }
    e = &b->endpoints[i];
    r = endpoint_current(b, i, UCN_I_SECURITY_ACCESS_INBOUND, b->now); if (r != UCN_OK) { return r; }
    memset(&b->open_work.open, 0, sizeof(b->open_work.open));
    b->open_work.open.session = e->session; b->open_work.open.facts = &b->current_facts;
    b->open_work.open.access = &b->access; b->open_work.open.packet = b->rx.frame;
    b->open_work.open.packet_bytes = b->rx.frame_bytes - 16U;
    b->open_work.open.plaintext_output = b->plaintext; b->open_work.open.plaintext_capacity = sizeof(b->plaintext);
    b->open_work.open.plaintext_bytes = &b->plaintext_bytes; b->open_work.open.replay_out = &b->origin_replay;
    b->open_work.open.origin_only = 1U;
    r = ucn_i_security_c1_open_borrowed(e->security, &b->open_work.open, &b->workspace);
    if (r != UCN_OK) { return r; }
    r = ucn_i_security_replay_commit(e->security, &b->origin_replay, &b->current_facts);
    if (r != UCN_OK) { (void)ucn_i_security_replay_abort(e->security, &b->origin_replay); return r; }
    memset(&b->received, 0, sizeof(b->received)); b->received.source = e->source;
    b->received.scope.instance = b->instance; b->received.scope.generation = 1U; b->received.scope.slot = (uint16_t)(i + 1U);
    b->received.receive_time_us = b->rx.meta.timestamp_us;
    b->received.origin_sequence = address_read(b->rx.frame + 5U + 2U * b->width, 4U);
    b->received.service_id = service; b->received.payload_bytes = (uint16_t)b->plaintext_bytes;
    b->received.traffic_class = b->rx.frame[1] >> 6U; b->received.protection = (uint8_t)((b->rx.frame[2] >> 6U) + 1U);
    b->rx_ready = 1U; return UCN_OK;
}
static UCN_I_STACK_PHASE ucn_result_t receive_forward(ucn_i_native_routed_t *b, uint32_t source, uint32_t dest, uint16_t service)
{
    uint16_t i, next; ucn_result_t r;
    if ((b->rx.frame[2] & 0x3FU) <= 1U || b->tx_live || b->tx_prepared || b->tx_pending) { return UCN_ERR_STATE; }
    for (i = 0U; i < b->forward_count; ++i) {
        if (b->forward_domains[i].origin.address == source && b->forward_domains[i].destination.address == dest) { break; }
    }
    if (i == b->forward_count) { return UCN_ERR_NOT_FOUND; }
    r = route_next(b, &b->forward_domains[i], b->rx.frame_bytes, &next); if (r != UCN_OK) { return r; }
    r = neighbor_current(b, next, service, UCN_I_SECURITY_ACCESS_OUTBOUND, b->rx.frame_bytes); if (r != UCN_OK) { return r; }
    if (b->rx.meta.timestamp_us > UINT64_MAX - b->relay_lifetime || b->now >= b->rx.meta.timestamp_us + b->relay_lifetime) { return UCN_ERR_STATE; }
    b->origin_bytes = b->rx.frame_bytes - 16U; memcpy(b->origin, b->rx.frame, b->origin_bytes);
    b->origin[2] = (uint8_t)(b->origin[2] - 1U);
    b->tx_neighbor = next; b->tx_endpoint = UINT16_MAX; b->tx_service = service;
    b->tx_domain = b->forward_domains[i]; b->tx_deadline = b->rx.meta.timestamp_us + b->relay_lifetime;
    b->tx_pending = 2U; return UCN_OK;
}
static UCN_I_STACK_PHASE void protect_forward(ucn_i_native_routed_t *b)
{
    const ucn_i_native_neighbor_t *n = &b->neighbors[b->tx_neighbor];
    b->tx_pending = 0U;
    if (neighbor_current(b, b->tx_neighbor, b->tx_service, UCN_I_SECURITY_ACCESS_OUTBOUND,
        b->origin_bytes + 16U) != UCN_OK) { return; }
    if (ucn_i_security_c1_hop_wrap(n->security, n->session, &b->current_facts,
        b->origin, b->origin_bytes, &b->workspace, b->frame, sizeof(b->frame), &b->frame_bytes) == UCN_OK) {
        b->tx_prepared = 1U;
    }
}
static UCN_I_STACK_PHASE ucn_result_t receive(ucn_i_native_routed_t *b)
{
    uint16_t i, service; uint32_t source, dest; ucn_result_t r; const ucn_i_native_neighbor_t *n;
    if (!live(b, b->now) || b->rx.meta.timestamp_us > b->now ||
        b->rx.frame_bytes < 9U + 2U * b->width + 32U || b->rx.frame[0] != 0x61U ||
        (b->rx.frame[1] & 0x3FU) != 0U) { return UCN_ERR_MALFORMED; }
    for (i = 0U; i < b->neighbor_count; ++i) {
        if (b->neighbors[i].link_slot == b->rx.link_slot && b->neighbors[i].facts.link_generation == b->rx.link_instance_generation &&
            b->neighbors[i].sender_discriminator == b->rx.meta.sender_discriminator) { break; }
    }
    if (i == b->neighbor_count) { return UCN_ERR_ACCESS; }
    service = (uint16_t)address_read(b->rx.frame + 3U + 2U * b->width, 2U);
    r = neighbor_current(b, i, service, UCN_I_SECURITY_ACCESS_INBOUND, b->rx.frame_bytes);
    if (r != UCN_OK) { return r; }
    n = &b->neighbors[i];
    r = ucn_i_security_c1_hop_open(n->security, n->session, &b->current_facts,
        b->rx.frame, b->rx.frame_bytes, &b->workspace, &b->hop_replay);
    if (r != UCN_OK) { return r; }
    r = ucn_i_security_hop_replay_commit(n->security, &b->hop_replay, &b->current_facts);
    if (r != UCN_OK) { (void)ucn_i_security_hop_replay_abort(n->security, &b->hop_replay); return r; }
    source = address_read(b->rx.frame + 3U, b->width); dest = address_read(b->rx.frame + 3U + b->width, b->width);
    b->rx_source = source; b->rx_service = service;
    b->rx_endpoint_pending = dest == b->local.address ? 1U : 2U;
    return UCN_OK;
}
static ucn_result_t poll(void *ctx, ucn_native_backend_message_t *out, uint8_t *payload, size_t cap)
{
    ucn_i_native_routed_t *b = ctx; ucn_result_t r;
    if (!output_safe(b, out, sizeof(*out)) || !output_safe(b, payload, cap) || cap < UCN_NATIVE_PAYLOAD_BYTES ||
        ucn_i_ranges_overlap(out, sizeof(*out), payload, cap)) { return UCN_ERR_ARGUMENT; }
    if (!gate_idle(b)) { return UCN_ERR_STATE; }
    if (b->lifecycle != UCN_LIFECYCLE_RUNNING) { return UCN_ERR_STATE; }
    if (!b->rx_live) {
        r = ucn_i_adapter_rx_claim(b->adapter, &b->rx); if (r != UCN_OK) { return r; }
        b->rx_live = 1U; b->rx_ready = 0U; b->rx_endpoint_pending = 0U;
        if (b->control && b->rx.frame_bytes && b->rx.frame[0] == 0x60U) {
            if (ucn_i_native_route_control_receive_context(b) == UCN_OK) {
                if (ucn_i_native_route_control_receive(b) == UCN_OK) {
                    (void)ucn_i_native_route_control_receive_finish(b);
                }
            }
        } else { (void)receive(b); }
        if (b->rx_endpoint_pending) {
            if (b->rx_endpoint_pending == 1U) { (void)receive_endpoint(b, b->rx_source, b->rx_service); }
            else { (void)receive_forward(b, b->rx_source, address_read(b->rx.frame + 3U + b->width, b->width), b->rx_service); }
            b->rx_endpoint_pending = 0U;
        }
        if (b->tx_pending == 2U) { protect_forward(b); }
        if (b->tx_prepared && !b->tx_live) { (void)tx_progress(b); }
    }
    r = ucn_i_adapter_rx_retire(b->adapter, &b->rx.token); if (r != UCN_OK) { return r; }
    b->rx_live = 0U;
    if (!b->rx_ready) { return UCN_ERR_SECURITY; }
    b->rx_ready = 0U; *out = b->received; memcpy(payload, b->plaintext, b->plaintext_bytes);
    memset(b->plaintext, 0, sizeof(b->plaintext)); return UCN_OK;
}

static bool config_ranges(ucn_i_native_routed_t *b, const ucn_i_native_routed_config_t *c)
{
    uint16_t i;
    if (!b || !c || !c->adapter || !c->routes || !c->gate ||
        c->endpoint_count > UCN_I_NATIVE_ENDPOINTS || !c->neighbor_count || c->neighbor_count > UCN_I_NATIVE_NEIGHBORS ||
        c->forward_count > UCN_I_NATIVE_FORWARD_DOMAINS ||
        (c->endpoint_count && !c->endpoints) || !c->neighbors || (c->forward_count && !c->forward_domains) ||
        !c->now_us || c->expires_at_us <= c->now_us || !c->relay_lifetime_us ||
        !c->local.address || !c->local.binding_generation || !c->local.principal_digest ||
        !c->hop_limit || c->hop_limit > 63U || c->reserved_zero ||
        ucn_i_ranges_overlap(b, sizeof(*b), c, sizeof(*c)) ||
        ucn_i_ranges_overlap(b, sizeof(*b), c->adapter, sizeof(*c->adapter)) ||
        ucn_i_ranges_overlap(b, sizeof(*b), c->routes, sizeof(*c->routes)) ||
        ucn_i_ranges_overlap(b, sizeof(*b), c->gate, sizeof(*c->gate)) ||
        ucn_i_ranges_overlap(b, sizeof(*b), c->endpoints, c->endpoint_count * sizeof(*c->endpoints)) ||
        ucn_i_ranges_overlap(b, sizeof(*b), c->neighbors, c->neighbor_count * sizeof(*c->neighbors)) ||
        ucn_i_ranges_overlap(b, sizeof(*b), c->forward_domains, c->forward_count * sizeof(*c->forward_domains))) { return false; }
    for (i = 0U; i < c->endpoint_count; ++i) {
        if (!c->endpoints[i].security || ucn_i_ranges_overlap(b, sizeof(*b), c->endpoints[i].security, sizeof(*c->endpoints[i].security))) { return false; }
        if (c->control && ucn_i_ranges_overlap(c->control, sizeof(*c->control), c->endpoints[i].security, sizeof(*c->endpoints[i].security))) { return false; }
    }
    if (c->control && (ucn_i_ranges_overlap(b, sizeof(*b), c->control, sizeof(*c->control)) ||
        ucn_i_ranges_overlap(c, sizeof(*c), c->control, sizeof(*c->control)) ||
        ucn_i_ranges_overlap(c->adapter, sizeof(*c->adapter), c->control, sizeof(*c->control)) ||
        ucn_i_ranges_overlap(c->routes, sizeof(*c->routes), c->control, sizeof(*c->control)) ||
        ucn_i_ranges_overlap(c->gate, sizeof(*c->gate), c->control, sizeof(*c->control)) ||
        ucn_i_ranges_overlap(c->endpoints, c->endpoint_count * sizeof(*c->endpoints), c->control, sizeof(*c->control)) ||
        ucn_i_ranges_overlap(c->neighbors, c->neighbor_count * sizeof(*c->neighbors), c->control, sizeof(*c->control)) ||
        ucn_i_ranges_overlap(c->forward_domains, c->forward_count * sizeof(*c->forward_domains), c->control, sizeof(*c->control)))) { return false; }
    for (i = 0U; i < c->neighbor_count; ++i) {
        if (!c->neighbors[i].security || ucn_i_ranges_overlap(b, sizeof(*b), c->neighbors[i].security, sizeof(*c->neighbors[i].security))) { return false; }
        if (c->control && ucn_i_ranges_overlap(c->control, sizeof(*c->control), c->neighbors[i].security, sizeof(*c->neighbors[i].security))) { return false; }
    }
    return true;
}
static bool compact_binding_consistent(const ucn_i_route_binding_t *a,
    const ucn_i_route_binding_t *b)
{ return a->address != b->address || memcmp(a, b, sizeof(*a)) == 0; }
static bool domain_binding_valid(const ucn_i_native_routed_t *b,
    const ucn_i_route_binding_t *r)
{
    uint16_t i; uint8_t nonzero = 0U;
    const uint32_t limit = b->width == 4U ? UINT32_MAX : (UINT32_C(1) << (8U * b->width)) - 1U;
    if (!r->address || r->address >= limit || !r->generation ||
        !compact_binding_consistent(r, &b->routes->local)) { return false; }
    for (i = 0U; i < 16U; ++i) { nonzero |= r->principal[i]; }
    if (!nonzero) { return false; }
    for (i = 0U; i < b->endpoint_count; ++i) {
        if (r->address == b->endpoints[i].facts.peer.address && !binding_matches(r, &b->endpoints[i].facts.peer)) { return false; }
    }
    for (i = 0U; i < b->neighbor_count; ++i) {
        if (r->address == b->neighbors[i].facts.peer.address && !binding_matches(r, &b->neighbors[i].facts.peer)) { return false; }
    }
    return true;
}
static bool assembly_valid(ucn_i_native_routed_t *b)
{
    uint16_t i, j;
    if (b->routes->local.address != b->local.address || b->routes->local.generation != b->local.binding_generation) { return false; }
    for (i = 0U; i < b->endpoint_count; ++i) {
        const ucn_i_native_endpoint_t *e = &b->endpoints[i];
        if (e->security->provider_gate != b->gate || e->security->realm_id != b->routes->realm || e->security->address_width != b->width ||
            !binding_matches(&b->routes->local, &e->facts.local) || e->source.address == b->local.address || e->source.address != e->facts.peer.address ||
            e->source.binding_generation != e->facts.peer.binding_generation || !e->source.principal_digest ||
            e->reserved_zero || (e->minimum != UCN_NATIVE_ORIGIN_AUTH && e->minimum != UCN_NATIVE_ENCRYPTED) ||
            (endpoint_current(b, i, UCN_I_SECURITY_ACCESS_INBOUND, b->now) != UCN_OK &&
             endpoint_current(b, i, UCN_I_SECURITY_ACCESS_OUTBOUND, b->now) != UCN_OK)) { return false; }
        for (j = 0U; j < i; ++j) { if (e->source.address == b->endpoints[j].source.address) { return false; } }
    }
    for (i = 0U; i < b->neighbor_count; ++i) {
        const ucn_i_native_neighbor_t *n = &b->neighbors[i];
        uint32_t generation; uint16_t mtu; bool ready;
        if (n->security->provider_gate != b->gate || n->security->realm_id != b->routes->realm || n->security->address_width != b->width ||
            !binding_matches(&b->routes->local, &n->facts.local) || n->facts.peer.address == b->local.address || !n->capability_bits ||
            ucn_i_adapter_link_snapshot(b->adapter, n->link_slot, &generation, &mtu, &ready) != UCN_OK ||
            !ready || generation != n->facts.link_generation ||
            ucn_i_security_session_get(n->security, n->session, &b->session_view) != UCN_OK ||
            b->session_view.phase != UCN_I_SECURITY_SESSION_ACTIVE || b->session_view.hop_profile != UCN_I_HOP_PROFILE_H1 ||
            memcmp(&b->session_view.peer, &n->facts.peer, sizeof(n->facts.peer)) != 0) { return false; }
        b->current_facts = n->facts; b->current_facts.now_us = b->now;
        if (ucn_i_security_c1_context_check(n->security, n->session, &b->current_facts, NULL, n->fingerprint) != UCN_OK) { return false; }
        for (j = 0U; j < i; ++j) {
            if (n->link_slot == b->neighbors[j].link_slot || n->facts.peer.address == b->neighbors[j].facts.peer.address) { return false; }
        }
        for (j = 0U; j < b->endpoint_count; ++j) {
            if (n->facts.peer.address == b->endpoints[j].facts.peer.address &&
                memcmp(&n->facts.peer, &b->endpoints[j].facts.peer, sizeof(n->facts.peer)) != 0) { return false; }
        }
    }
    for (i = 0U; i < b->forward_count; ++i) {
        const ucn_i_route_domain_t *d = &b->forward_domains[i];
        if (d->realm != b->routes->realm || !domain_binding_valid(b, &d->origin) || !domain_binding_valid(b, &d->destination) ||
            !d->origin_session_generation || d->destination.address == b->local.address || d->origin.address == d->destination.address) { return false; }
        for (j = 0U; j < i; ++j) {
            if (d->origin.address == b->forward_domains[j].origin.address &&
                d->destination.address == b->forward_domains[j].destination.address) { return false; }
            if (!compact_binding_consistent(&d->origin, &b->forward_domains[j].origin) ||
                !compact_binding_consistent(&d->origin, &b->forward_domains[j].destination) ||
                !compact_binding_consistent(&d->destination, &b->forward_domains[j].origin) ||
                !compact_binding_consistent(&d->destination, &b->forward_domains[j].destination)) { return false; }
        }
    }
    return true;
}
ucn_result_t ucn_i_native_routed_init(ucn_i_native_routed_t *b, const ucn_i_native_routed_config_t *c)
{
    size_t i; bool quiet = false;
    if (!config_ranges(b, c)) { return UCN_ERR_ARGUMENT; }
    for (i = 0U; i < sizeof(*b); ++i) { if (((const uint8_t *)b)[i]) { return UCN_ERR_STATE; } }
    if (ucn_i_adapter_quiescent(c->adapter, &quiet) != UCN_OK || !quiet || !c->routes->address_width || c->routes->address_width > 4U) { return UCN_ERR_STATE; }
    b->adapter = c->adapter; b->routes = c->routes; b->gate = c->gate; b->local = c->local;
    b->now = c->now_us; b->expires_at = c->expires_at_us; b->relay_lifetime = c->relay_lifetime_us;
    b->width = c->routes->address_width; b->instance = c->adapter->runtime_instance;
    b->hop_limit = c->hop_limit; b->endpoint_count = c->endpoint_count; b->neighbor_count = c->neighbor_count; b->forward_count = c->forward_count;
    if (c->endpoint_count) { memcpy(b->endpoints, c->endpoints, c->endpoint_count * sizeof(*c->endpoints)); }
    memcpy(b->neighbors, c->neighbors, c->neighbor_count * sizeof(*c->neighbors));
    if (c->forward_count) { memcpy(b->forward_domains, c->forward_domains, c->forward_count * sizeof(*c->forward_domains)); }
    if (!gate_idle(b) || !assembly_valid(b)) { memset(b, 0, sizeof(*b)); return UCN_ERR_STATE; }
    if (ucn_i_native_route_control_init(b, c) != UCN_OK) { memset(b, 0, sizeof(*b)); return UCN_ERR_STATE; }
    b->lifecycle = UCN_LIFECYCLE_INITIALIZED;
    b->port.struct_size = sizeof(b->port); b->port.api_version = UCN_NATIVE_API_VERSION;
    b->port.context = b; b->port.context_bytes = sizeof(*b); b->port.preflight = preflight;
    b->port.local_view = local_view; b->port.binding = binding; b->port.start = start; b->port.stop = stop;
    b->port.step = step; b->port.send = send; b->port.poll = poll; b->port.validate = validate;
    return UCN_OK;
}
