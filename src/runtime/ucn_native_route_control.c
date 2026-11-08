#include "internal/ucn_native_route_control.h"
#include "internal/ucn_stack.h"
#include "internal/ucn_checked.h"
#include <string.h>

static uint8_t role(uint16_t opcode)
{
    return opcode == 0x0201U ? UCN_INTERACTION_REQUEST :
        opcode == 0x0202U ? UCN_INTERACTION_RESULT :
        opcode == 0x0203U ? UCN_INTERACTION_ERROR : UCN_INTERACTION_ONE_WAY;
}
static bool same_binding(const ucn_i_route_binding_t *a, const ucn_i_security_binding_t *b)
{ return a->address == b->address && a->generation == b->binding_generation && memcmp(a->principal, b->principal, 16U) == 0; }
static UCN_I_STACK_PHASE ucn_result_t peer(ucn_i_native_routed_t *b, uint16_t index,
    uint16_t opcode, uint8_t direction)
{
    ucn_i_native_route_control_t *c = b->control;
    const ucn_i_native_neighbor_t *n = &b->neighbors[index];
    bool ready; ucn_result_t r;
    if (b->now >= b->expires_at) { return UCN_ERR_STATE; }
    memset(&c->ingress, 0, sizeof(c->ingress));
    r = ucn_i_adapter_link_snapshot(b->adapter, n->link_slot,
        &c->ingress.link_generation, &c->ingress.frame_mtu, &ready);
    if (r != UCN_OK || !ready || c->ingress.link_generation != n->facts.link_generation || c->ingress.frame_mtu < 127U) { return UCN_ERR_STATE; }
    c->facts = n->facts; c->facts.now_us = b->now;
    r = ucn_i_security_c1_context_check(n->security, n->session, &c->facts, n->control_fingerprint, n->fingerprint);
    if (r != UCN_OK) { return r; }
    r = ucn_i_security_session_get(n->security, n->session, &c->session);
    if (r != UCN_OK || c->session.origin_level != UCN_I_ORIGIN_O1 || c->session.hop_profile != UCN_I_HOP_PROFILE_H1) { return UCN_ERR_SECURITY; }
    memset(&c->access, 0, sizeof(c->access)); c->access.session = n->session;
    c->access.protocol_opcode = opcode; c->access.direction = direction;
    memcpy(c->access.context_fingerprint, n->control_fingerprint, 16U);
    r = ucn_i_security_control_authorize(n->security, &c->access, &c->facts);
    if (r != UCN_OK) { return r; }
    c->ingress.peer.address = n->facts.peer.address; c->ingress.peer.generation = n->facts.peer.binding_generation;
    memcpy(c->ingress.peer.principal, n->facts.peer.principal, 16U);
    c->ingress.link_id = (uint16_t)(n->link_slot + 1U);
    c->ingress.cost = 1U;
    if (c->ingress.frame_mtu > UCN_ADAPTER_FRAME_BYTES) { c->ingress.frame_mtu = UCN_ADAPTER_FRAME_BYTES; }
    c->ingress.capability_bits = n->capability_bits;
    return UCN_OK;
}
static int neighbor_for(ucn_i_native_routed_t *b, const ucn_i_route_link_ref_t *link)
{
    uint16_t i;
    for (i = 0U; i < b->neighbor_count; ++i) {
        if (b->neighbors[i].link_slot + 1U == link->link_id &&
            b->neighbors[i].facts.link_generation == link->link_generation &&
            same_binding(&link->peer, &b->neighbors[i].facts.peer)) { return (int)i; }
    }
    return -1;
}
static uint8_t pieces(ucn_i_native_routed_t *b, uint16_t length)
{
    uint16_t base = (uint16_t)(57U + 2U * b->width), chunk;
    uint8_t count;
    if (base + length <= b->control->ingress.frame_mtu) { return 1U; }
    if (length == 54U || b->control->ingress.frame_mtu <= base + 14U) { return 0U; }
    chunk = (uint16_t)(b->control->ingress.frame_mtu - base - 14U);
    if (chunk > 50U) { chunk = 50U; }
    count = (uint8_t)((length + chunk - 1U) / chunk);
    return count >= 2U && count <= 4U ? count : 0U;
}
static ucn_result_t available(ucn_i_native_routed_t *b, uint16_t index, uint16_t opcode, uint16_t length)
{
    ucn_result_t r;
    if (b->control->jobs[index].occupied) { return UCN_ERR_NO_SPACE; }
    r = peer(b, index, opcode, UCN_I_SECURITY_ACCESS_OUTBOUND); if (r != UCN_OK) { return r; }
    if (!pieces(b, length)) { return UCN_ERR_NO_SPACE; }
    if (pieces(b, length) > 1U) { return peer(b, index, 0x0206U, UCN_I_SECURITY_ACCESS_OUTBOUND); }
    return UCN_OK;
}
static void enqueue(ucn_i_native_routed_t *b, uint16_t index, uint16_t opcode,
    const uint8_t *body, uint16_t length, uint64_t deadline)
{
    ucn_i_native_control_job_t *j = &b->control->jobs[index];
    memset(j, 0, sizeof(*j)); memcpy(j->body, body, length);
    j->length = length; j->opcode = opcode; j->deadline = deadline; j->occupied = 1U;
    j->count = pieces(b, length);
}
static uint64_t deadline_at(ucn_i_native_routed_t *b, uint64_t at, uint64_t duration)
{
    uint64_t end = at > UINT64_MAX - duration ? UINT64_MAX : at + duration;
    return end < b->expires_at ? end : b->expires_at;
}
static unsigned flood(ucn_i_native_routed_t *b, const ucn_i_route_rreq_message_t *request,
    uint16_t excluded, uint64_t deadline, bool commit)
{
    uint16_t i; unsigned count = 0U;
    for (i = 0U; i < b->neighbor_count; ++i) {
        /* RREQ is never fragmented: its preflight is the exact single-frame
         * check, not the generic RREP/RERR fragmentation queue helper. */
        if (i == excluded || b->control->jobs[i].occupied ||
            peer(b, i, 0x0201U, UCN_I_SECURITY_ACCESS_OUTBOUND) != UCN_OK || pieces(b, 54U) != 1U ||
            b->control->ingress.frame_mtu < request->payload.minimum_frame_mtu ||
            (request->payload.required_capability_bits & b->control->ingress.capability_bits) != request->payload.required_capability_bits) { continue; }
        ++count;
        if (commit) { enqueue(b, i, 0x0201U, b->control->encoded, 54U, deadline); }
    }
    return count;
}
ucn_result_t ucn_i_native_route_control_discover(ucn_i_native_routed_t *b,
    uint32_t target, uint16_t bytes, uint64_t deadline)
{
    ucn_i_native_route_control_t *c = b->control;
    uint16_t i, free_slot = UCN_I_ROUTE_DISCOVERY_COUNT;
    for (i = 0U; i < UCN_I_ROUTE_DISCOVERY_COUNT; ++i) {
        if (!c->discoveries[i].occupied) { free_slot = i; continue; }
        if (c->discoveries[i].request.key.target_address == target &&
            c->discoveries[i].request.payload.minimum_frame_mtu == bytes) { return UCN_ERR_NO_SPACE; }
    }
    if (free_slot == UCN_I_ROUTE_DISCOVERY_COUNT) { return UCN_ERR_NO_SPACE; }
    /* An unallocated demand is not a second discovery FSM or identity.
     * The subsequent bounded step preflights peers then asks Route Owner to
     * allocate/merge the real discovery. Native retains the original job. */
    memset(&c->discoveries[free_slot], 0, sizeof(c->discoveries[free_slot]));
    c->discoveries[free_slot].request.key.target_address = target;
    c->discoveries[free_slot].request.payload.minimum_frame_mtu = bytes;
    c->discoveries[free_slot].request.payload.required_capability_bits = 1U;
    c->discoveries[free_slot].deadline = deadline;
    c->discoveries[free_slot].occupied = 1U;
    return UCN_ERR_NO_SPACE;
}
static UCN_I_STACK_PHASE ucn_result_t reserve_counter(ucn_i_native_routed_t *b, uint16_t index, uint64_t *out)
{
    ucn_i_callback_claim_t claim; ucn_result_t r, end;
    claim.owner_instance = b->instance; claim.operation_id = (uint32_t)index + 1U;
    claim.operation_generation = 1U; claim.operation_kind = 0x0714U;
    r = ucn_i_callback_gate_enter(b->gate, &claim); if (r != UCN_OK) { return r; }
    r = b->control->reserve(b->control->counter_context, b->neighbors[index].security, b->neighbors[index].session, out);
    end = ucn_i_callback_gate_leave(b->gate, &claim);
    return end == UCN_OK ? r : end;
}
static UCN_I_STACK_PHASE ucn_result_t prepare(ucn_i_native_routed_t *b, uint16_t index)
{
    ucn_i_native_route_control_t *c = b->control;
    ucn_i_native_control_job_t *j = &c->jobs[index];
    const ucn_i_native_neighbor_t *n = &b->neighbors[index];
    uint16_t width, offset; ucn_result_t r;
    r = peer(b, index, j->opcode, UCN_I_SECURITY_ACCESS_OUTBOUND); if (r != UCN_OK) { return r; }
    if (j->count > 1U) { r = peer(b, index, 0x0206U, UCN_I_SECURITY_ACCESS_OUTBOUND); if (r != UCN_OK) { return r; } }
    memset(&c->crypto_work.protect, 0, sizeof(c->crypto_work.protect));
    r = reserve_counter(b, index, &c->crypto_work.protect.transaction_id); if (r != UCN_OK) { return r; }
    if (!c->crypto_work.protect.transaction_id) { return UCN_ERR_EXHAUSTED; }
    c->crypto_work.protect.session = n->session; c->crypto_work.protect.facts = c->facts;
    c->crypto_work.protect.protocol_opcode = j->count == 1U ? j->opcode : 0x0206U;
    c->crypto_work.protect.interaction_role = role(c->crypto_work.protect.protocol_opcode);
    c->crypto_work.protect.payload_kind = UCN_I_PAYLOAD_CONTROL; c->crypto_work.protect.hop_limit = 1U;
    c->crypto_work.protect.direction = UCN_I_SECURITY_ACCESS_OUTBOUND;
    memcpy(c->crypto_work.protect.context_fingerprint, n->control_fingerprint, 16U);
    if (j->count == 1U) { memcpy(c->encoded, j->body, j->length); c->encoded_bytes = j->length; }
    else {
        if (!j->bundle) { j->bundle = c->crypto_work.protect.transaction_id; }
        memset(&c->fragment, 0, sizeof(c->fragment));
        c->fragment.bundle = j->bundle; c->fragment.opcode = j->opcode;
        c->fragment.total = j->length; c->fragment.index = j->piece; c->fragment.count = j->count;
        width = (uint16_t)((j->length + j->count - 1U) / j->count); offset = (uint16_t)(width * j->piece);
        c->fragment.length = (uint8_t)(j->length - offset < width ? j->length - offset : width);
        memcpy(c->fragment.chunk, j->body + offset, c->fragment.length);
        r = ucn_i_route_fragment_encode(&c->fragment, c->encoded, sizeof(c->encoded), &c->encoded_bytes);
        if (r != UCN_OK) { return r; }
    }
    return UCN_OK;
}
static ucn_result_t physical_control(ucn_i_native_routed_t *b, ucn_i_native_control_job_t *j, bool cancel)
{
    ucn_result_t r, end;
    b->open_work.callback.owner_instance = b->instance; b->open_work.callback.operation_id = j->token.generation;
    b->open_work.callback.operation_generation = 1U; b->open_work.callback.operation_kind = cancel ? 0x0716U : 0x0715U;
    r = ucn_i_callback_gate_enter(b->gate, &b->open_work.callback); if (r != UCN_OK) { return r; }
    if (!cancel) { ++j->total_attempts; }
    r = cancel ? ucn_i_adapter_tx_cancel(b->adapter, &j->token) :
        ucn_i_adapter_tx_submit_ref(b->adapter, &j->token, j->frame, j->frame_bytes, &b->control->tx_view);
    end = ucn_i_callback_gate_leave(b->gate, &b->open_work.callback); return end == UCN_OK ? r : end;
}
static UCN_I_STACK_PHASE void finish(ucn_i_native_routed_t *b, ucn_i_native_control_job_t *j, uint8_t delivered)
{
    if (j->completion.slot && ucn_i_route_complete_rrep_forward(b->routes, j->completion, delivered) != UCN_OK) {
        b->lifecycle = UCN_LIFECYCLE_FAULT; return;
    }
    memset(j, 0, sizeof(*j));
}
void ucn_i_native_route_control_drive(ucn_i_native_routed_t *b)
{
    ucn_i_native_route_control_t *c = b->control;
    uint16_t index = c->tx_cursor;
    ucn_i_native_control_job_t *j = &c->jobs[index]; ucn_result_t r;
    bool valid;
    c->tx_cursor = (uint16_t)((index + 1U) % b->neighbor_count);
    if (!j->occupied) { return; }
    valid = b->lifecycle == UCN_LIFECYCLE_RUNNING && b->now < j->deadline &&
        peer(b, index, j->opcode, UCN_I_SECURITY_ACCESS_OUTBOUND) == UCN_OK;
    if (valid && j->count > 1U) { valid = peer(b, index, 0x0206U, UCN_I_SECURITY_ACCESS_OUTBOUND) == UCN_OK; }
    if (j->live) {
        r = ucn_i_adapter_tx_view(b->adapter, &j->token, &c->tx_view); if (r != UCN_OK) { return; }
        if (c->tx_view.state == UCN_I_ADAPTER_TX_COMPLETED || c->tx_view.state == UCN_I_ADAPTER_TX_CANCELLED) {
            bool delivered = c->tx_view.state == UCN_I_ADAPTER_TX_COMPLETED && c->tx_view.terminal_result == UCN_OK;
            if (ucn_i_adapter_tx_retire(b->adapter, &j->token) != UCN_OK) { return; }
            j->live = 0U; j->prepared = 0U;
            if (!delivered || !valid || ++j->piece == j->count) { finish(b, j, delivered && valid ? 1U : 0U); return; }
        } else if (!valid) {
            /* Cancellation is not terminal proof: retain Unknown until view says terminal. */
            (void)physical_control(b, j, true); return;
        } else if (c->tx_view.state == UCN_I_ADAPTER_TX_NOT_SUBMITTED) {
            if (ucn_i_adapter_tx_retire(b->adapter, &j->token) != UCN_OK) { return; }
            j->live = 0U; /* Exactly the same protected bytes are retained. */
        } else { return; }
    }
    if (!valid) { finish(b, j, 0U); return; }
    /* Total physical submit budget is shared by ALL pieces of this job.
     * Only enforce it after an outstanding token was genuinely retired. */
    if (j->total_attempts >= UCN_I_NATIVE_CONTROL_MAX_ATTEMPTS) { finish(b, j, 0U); return; }
    if (!j->prepared) { return; }
    r = ucn_i_adapter_tx_reserve(b->adapter, b->neighbors[index].link_slot,
        b->neighbors[index].facts.link_generation, 0U, &j->token);
    if (r == UCN_OK) { j->live = 1U; (void)physical_control(b, j, false); }
}

void ucn_i_native_route_control_prepare(ucn_i_native_routed_t *b)
{
    ucn_result_t r;
    uint16_t i = b->control->tx_cursor;
    ucn_i_native_control_job_t *j = &b->control->jobs[i];
    if (j->occupied && !j->live && !j->prepared &&
        b->lifecycle == UCN_LIFECYCLE_RUNNING && b->now < j->deadline) {
        /* Material preparation and cryptographic protection have disjoint
         * temporary lifetimes; both complete in this bounded actor turn. */
        r = prepare(b, i);
        if (r == UCN_OK) {
            r = ucn_i_security_c0_protect(b->neighbors[i].security, &b->control->crypto_work.protect,
                b->control->encoded, b->control->encoded_bytes, &b->control->packet,
                j->frame, sizeof(j->frame), &j->frame_bytes);
        }
        if (r == UCN_OK) { j->prepared = 1U; }
        else { finish(b, j, 0U); }
    }
}

static UCN_I_STACK_PHASE ucn_result_t request_received(ucn_i_native_routed_t *b, uint16_t index)
{
    ucn_i_native_route_control_t *c = b->control; ucn_result_t r;
    uint64_t deadline = 0U;
    r = ucn_i_route_on_rreq(b->routes, &c->request, &c->ingress, b->now, &c->request_action);
    if (r != UCN_OK) { return r; }
    deadline = deadline_at(b, c->request_action.reverse_expires_at_us, 0U);
    if (c->request_action.kind == UCN_I_ROUTE_REQUEST_LOCAL_TARGET) {
        r = ucn_i_route_make_rrep(b->routes, &c->request, c->ingress.capability_bits, c->ingress.frame_mtu, b->now, &c->reply);
        if (r != UCN_OK) { return r; }
        r = ucn_i_route_control_reply_encode(&c->reply, 0U, b->width, c->encoded);
        if (r == UCN_OK) { enqueue(b, index, 0x0202U, c->encoded, 77U, deadline); }
    } else if (c->request_action.kind == UCN_I_ROUTE_REQUEST_FORWARD) {
        r = ucn_i_route_control_request_encode(&c->request_action.forwarded, b->width, c->encoded);
    }
    return r;
}
static UCN_I_STACK_PHASE ucn_result_t reply_decode(ucn_i_native_routed_t *b, uint16_t index)
{
    ucn_i_native_route_control_t *c = b->control; ucn_result_t r;
    r = ucn_i_route_control_reply_decode(c->plaintext, c->plaintext_bytes, b->width, &c->reply, &c->reporter_generation);
    if (r != UCN_OK) { return r; }
    if (c->reply.payload.hop_count == 0U && (c->reply.key.target_address != b->neighbors[index].facts.peer.address ||
        c->reply.payload.destination_binding_generation != b->neighbors[index].facts.peer.binding_generation ||
        memcmp(c->reply.payload.destination_principal, b->neighbors[index].facts.peer.principal, 16U) != 0)) { return UCN_ERR_SECURITY; }
    if (c->reply.key.origin.address != b->local.address) {
        r = ucn_i_route_reverse_peek(b->routes, &c->reply.key, b->now, &c->upstream); if (r != UCN_OK) { return r; }
    }
    return UCN_OK;
}
static UCN_I_STACK_PHASE ucn_result_t reply_received(ucn_i_native_routed_t *b)
{
    ucn_i_native_route_control_t *c = b->control; ucn_result_t r;
    int upstream = c->reply.key.origin.address == b->local.address ? -1 : neighbor_for(b, &c->upstream);
    uint16_t i, slot = UCN_I_ROUTE_DYNAMIC_COUNT;
    for (i = 0U; i < UCN_I_ROUTE_DYNAMIC_COUNT; ++i) {
        if (!c->receipts[i].occupied) { slot = i; }
        else if (c->receipts[i].route.domain.origin.address == c->reply.key.origin.address &&
            c->receipts[i].route.domain.destination.address == c->reply.key.target_address) { slot = i; break; }
    }
    if (slot == UCN_I_ROUTE_DYNAMIC_COUNT) { return UCN_ERR_NO_SPACE; }
    r = ucn_i_route_apply_reply(b->routes, &c->reply, &c->ingress, b->now, &c->reply_action, true, c->reporter_generation);
    if (r != UCN_OK) { return r; }
    c->receipts[slot].route = c->reply_action.installed; c->receipts[slot].occupied = 1U;
    c->receipts[slot].error_pending = 0U;
    c->receipts[slot].upstream = upstream < 0 ? UINT16_MAX : (uint16_t)upstream;
    if (c->reply_action.kind != UCN_I_ROUTE_REPLY_FORWARD) {
        for (i = 0U; i < UCN_I_ROUTE_DISCOVERY_COUNT; ++i) {
            if (c->discoveries[i].occupied && c->discoveries[i].request.key.transaction_id == c->reply.key.transaction_id) {
                c->discoveries[i].occupied = 0U;
            }
        }
    }
    return r;
}
static UCN_I_STACK_PHASE void reply_forward(ucn_i_native_routed_t *b, uint16_t upstream)
{
    ucn_i_native_route_control_t *c = b->control;
    if (ucn_i_route_control_reply_encode(&c->reply_action.forwarded,
        c->reply_action.installed.route_generation, b->width, c->encoded) == UCN_OK) {
        enqueue(b, upstream, 0x0202U, c->encoded, 77U,
            deadline_at(b, c->reply_action.reverse_expires_at_us, 0U));
        c->jobs[upstream].completion = c->reply_action.completion;
    } else { (void)ucn_i_route_complete_rrep_forward(b->routes, c->reply_action.completion, 0U); }
}
static UCN_I_STACK_PHASE ucn_result_t error_received(ucn_i_native_routed_t *b, uint16_t index)
{
    ucn_i_native_route_control_t *c = b->control; uint8_t invalidated = 0U; ucn_result_t r; uint16_t i;
    if (c->plaintext_bytes != 99U) { return UCN_ERR_MALFORMED; }
    r = ucn_i_route_rerr_decode(c->plaintext, &c->error); if (r != UCN_OK) { return r; }
    if (c->error.reporter_address != b->neighbors[index].facts.peer.address ||
        c->error.reporter_binding_generation != b->neighbors[index].facts.peer.binding_generation ||
        memcmp(c->error.reporter_principal, b->neighbors[index].facts.peer.principal, 16U) != 0) { return UCN_ERR_SECURITY; }
    r = ucn_i_route_on_wire_rerr(b->routes, &c->error, &invalidated);
    if (r == UCN_OK && invalidated) {
        for (i = 0U; i < UCN_I_ROUTE_DYNAMIC_COUNT; ++i) {
            const ucn_i_route_soft_view_t *route = &c->receipts[i].route;
            if (c->receipts[i].occupied && route->route_causal_id == c->error.route_causal_id &&
                route->domain.realm == c->error.realm && route->domain.origin_session_generation == c->error.origin_session_generation &&
                route->domain.origin.address == c->error.origin_address && route->domain.origin.generation == c->error.origin_binding_generation &&
                route->domain.destination.address == c->error.destination_address && route->domain.destination.generation == c->error.destination_binding_generation &&
                memcmp(route->domain.origin.principal, c->error.origin_principal, 16U) == 0 &&
                memcmp(route->domain.destination.principal, c->error.destination_principal, 16U) == 0 &&
                neighbor_for(b, &route->next_hop) == (int)index) { c->receipts[i].error_pending = c->error.reason; }
        }
    }
    return r;
}
void ucn_i_native_route_control_receipt_step(ucn_i_native_routed_t *b)
{
    ucn_i_native_route_control_t *c = b->control;
    ucn_i_native_control_receipt_t *receipt = &c->receipts[c->receipt_cursor];
    uint32_t generation; uint16_t mtu; bool ready; uint8_t invalidated; int next; ucn_result_t r;
    c->receipt_cursor = (uint16_t)((c->receipt_cursor + 1U) % UCN_I_ROUTE_DYNAMIC_COUNT);
    if (!receipt->occupied) { return; }
    if (b->lifecycle != UCN_LIFECYCLE_RUNNING) { receipt->occupied = 0U; return; }
    if (!receipt->error_pending) {
        next = neighbor_for(b, &receipt->route.next_hop);
        if (next < 0) { receipt->error_pending = UCN_I_ROUTE_RERR_LINK_INVALID; }
        else {
            r = ucn_i_adapter_link_snapshot(b->adapter, b->neighbors[next].link_slot, &generation, &mtu, &ready);
            if (r != UCN_OK || !ready || generation != receipt->route.next_hop.link_generation) { receipt->error_pending = UCN_I_ROUTE_RERR_LINK_INVALID; }
            else if (b->now >= receipt->route.expires_at_us) { receipt->error_pending = UCN_I_ROUTE_RERR_ROUTE_EXPIRED; }
        }
        if (receipt->error_pending && ucn_i_route_make_rerr(b->routes, &receipt->route,
            receipt->error_pending, &c->error) == UCN_OK) {
            (void)ucn_i_route_on_rerr(b->routes, &c->error, &invalidated);
        }
    }
    if (!receipt->error_pending) { return; }
    if (receipt->upstream == UINT16_MAX) { receipt->occupied = 0U; return; }
    if (available(b, receipt->upstream, 0x0203U, 99U) != UCN_OK) { return; }
    r = ucn_i_route_make_rerr(b->routes, &receipt->route, receipt->error_pending, &c->error);
    if (r != UCN_OK) { return; }
    /* Local error payload was an exact invalidation key. On wire this router
     * is the authenticated reporter, with ITS generation (never downstream's). */
    c->error.reporter_address = b->routes->local.address;
    c->error.reporter_binding_generation = b->routes->local.generation;
    memcpy(c->error.reporter_principal, b->routes->local.principal, 16U);
    if (ucn_i_route_rerr_encode(&c->error, c->encoded) == UCN_OK) {
        enqueue(b, receipt->upstream, 0x0203U, c->encoded, 99U,
            deadline_at(b, b->now, b->relay_lifetime));
        receipt->occupied = 0U;
    }
}
static UCN_I_STACK_PHASE ucn_result_t reassemble(ucn_i_native_routed_t *b, uint16_t index)
{
    ucn_i_native_route_control_t *c = b->control;
    ucn_i_native_control_rx_t *rx = &c->rx[index];
    ucn_i_route_fragment_t *f = &c->fragment; uint16_t width, offset; ucn_result_t r;
    r = ucn_i_route_fragment_decode(c->plaintext, c->plaintext_bytes, f); if (r != UCN_OK) { return r; }
    r = peer(b, index, f->opcode, UCN_I_SECURITY_ACCESS_INBOUND); if (r != UCN_OK) { return r; }
    if (f->bundle > c->authenticated.transaction_id || (rx->deadline && b->now >= rx->deadline)) {
        rx->mask = 0U; rx->bundle = 0U; return UCN_ERR_TIMEOUT;
    }
    if (f->bundle != rx->bundle) {
        if (f->index || f->bundle <= rx->high_bundle) { return UCN_ERR_REPLAY; }
        memset(rx->body, 0, sizeof(rx->body)); rx->mask = 0U;
        rx->bundle = f->bundle; rx->high_bundle = f->bundle;
        rx->opcode = f->opcode; rx->total = f->total; rx->count = f->count;
        if (!rx->deadline) {
            rx->deadline = deadline_at(b, b->rx.meta.timestamp_us, b->routes->reverse_lifetime_us);
            rx->received_at = b->rx.meta.timestamp_us;
        }
    }
    if (rx->opcode != f->opcode || rx->total != f->total || rx->count != f->count ||
        (rx->mask & (1U << f->index))) { return UCN_ERR_MALFORMED; }
    width = (uint16_t)((f->total + f->count - 1U) / f->count); offset = (uint16_t)(width * f->index);
    memcpy(rx->body + offset, f->chunk, f->length); rx->mask = (uint8_t)(rx->mask | (1U << f->index));
    if (rx->mask != (1U << f->count) - 1U) { return UCN_ERR_NO_SPACE; }
    memcpy(c->plaintext, rx->body, rx->total); c->plaintext_bytes = rx->total;
    c->authenticated.protocol_opcode = rx->opcode;
    c->message_time = rx->received_at;
    rx->mask = 0U; rx->bundle = 0U; rx->deadline = 0U; memset(rx->body, 0, sizeof(rx->body));
    return UCN_OK;
}
ucn_result_t ucn_i_native_route_control_receive_context(ucn_i_native_routed_t *b)
{
    ucn_i_native_route_control_t *c = b->control;
    uint16_t i; ucn_result_t r;
    if (b->rx.meta.timestamp_us > b->now || b->now >= b->expires_at) { return UCN_ERR_STATE; }
    for (i = 0U; i < b->neighbor_count; ++i) {
        if (b->neighbors[i].link_slot == b->rx.link_slot && b->neighbors[i].facts.link_generation == b->rx.link_instance_generation &&
            b->neighbors[i].sender_discriminator == b->rx.meta.sender_discriminator) { break; }
    }
    if (i == b->neighbor_count) { return UCN_ERR_ACCESS; }
    /* This assembly seals one exact Session per dedicated Link. Decode only
     * for exact opcode preflight; the following open still authenticates the
     * complete frame, context, current facts and replay state. */
    r = ucn_i_c0_decode(b->rx.frame, b->rx.frame_bytes, b->width, UCN_I_CONTROL_H1, &c->packet.control_frame);
    if (r != UCN_OK) { return r; }
    r = peer(b, i, c->packet.control_frame.opcode, UCN_I_SECURITY_ACCESS_INBOUND);
    if (r == UCN_OK) { c->control_access = c->access; }
    return r;
}
ucn_result_t ucn_i_native_route_control_receive(ucn_i_native_routed_t *b)
{
    ucn_i_native_route_control_t *c = b->control; uint16_t i;
    const ucn_i_native_neighbor_t *n;
    if (b->rx.meta.timestamp_us > b->now || b->now >= b->expires_at) { return UCN_ERR_STATE; }
    for (i = 0U; i < b->neighbor_count; ++i) {
        if (b->neighbors[i].link_slot == b->rx.link_slot && b->neighbors[i].facts.link_generation == b->rx.link_instance_generation &&
            b->neighbors[i].sender_discriminator == b->rx.meta.sender_discriminator) { break; }
    }
    if (i == b->neighbor_count) { return UCN_ERR_ACCESS; }
    n = &b->neighbors[i];
    c->crypto_work.open.access = &c->control_access; c->crypto_work.open.facts = &c->facts;
    c->crypto_work.open.packet = b->rx.frame; c->crypto_work.open.packet_bytes = b->rx.frame_bytes;
    c->crypto_work.open.plaintext_output = c->plaintext; c->crypto_work.open.plaintext_capacity = sizeof(c->plaintext);
    c->crypto_work.open.plaintext_bytes = &c->plaintext_bytes; c->crypto_work.open.fact_out = &c->authenticated;
    return ucn_i_security_c0_open_borrowed(n->security, &c->crypto_work.open, &c->packet);
}
ucn_result_t ucn_i_native_route_control_receive_finish(ucn_i_native_routed_t *b)
{
    ucn_i_native_route_control_t *c = b->control; uint16_t i; ucn_result_t r;
    const ucn_i_native_neighbor_t *n;
    for (i = 0U; i < b->neighbor_count; ++i) {
        if (b->neighbors[i].link_slot == b->rx.link_slot && b->neighbors[i].facts.link_generation == b->rx.link_instance_generation &&
            b->neighbors[i].sender_discriminator == b->rx.meta.sender_discriminator) { break; }
    }
    if (i == b->neighbor_count) { return UCN_ERR_ACCESS; }
    n = &b->neighbors[i];
    if (c->authenticated.traffic_class != UCN_TRAFFIC_Q0 || c->authenticated.delivery_guarantee != UCN_DELIVERY_BEST_EFFORT ||
        c->authenticated.payload_kind != UCN_I_PAYLOAD_CONTROL || c->authenticated.origin_security != UCN_I_ORIGIN_O1 ||
        c->authenticated.hop_limit != 1U || !c->authenticated.replay.has_hop ||
        c->authenticated.interaction_role != role(c->authenticated.protocol_opcode) ||
        (c->authenticated.protocol_opcode != 0x0201U && c->authenticated.protocol_opcode != 0x0202U &&
         c->authenticated.protocol_opcode != 0x0203U && c->authenticated.protocol_opcode != 0x0206U)) {
        (void)ucn_i_security_replay_abort(n->security, &c->authenticated.replay); return UCN_ERR_MALFORMED;
    }
    r = ucn_i_security_replay_commit(n->security, &c->authenticated.replay, &c->facts);
    if (r != UCN_OK) { (void)ucn_i_security_replay_abort(n->security, &c->authenticated.replay); return r; }
    c->message_time = b->rx.meta.timestamp_us;
    if (c->authenticated.protocol_opcode == 0x0206U) { r = reassemble(b, i); if (r != UCN_OK) { return r; } }
    if (c->input[i].length) { return UCN_ERR_NO_SPACE; }
    memcpy(c->input[i].body, c->plaintext, c->plaintext_bytes);
    c->input[i].length = (uint16_t)c->plaintext_bytes;
    c->input[i].opcode = c->authenticated.protocol_opcode;
    c->input[i].received_at = c->message_time;
    c->input[i].fragmented = c->control_access.protocol_opcode == 0x0206U ? 1U : 0U;
    return UCN_OK;
}
void ucn_i_native_route_control_dispatch(ucn_i_native_routed_t *b)
{
    ucn_i_native_route_control_t *c = b->control;
    ucn_result_t r;
    uint16_t i = c->input_cursor; c->input_cursor = (uint16_t)((i + 1U) % b->neighbor_count);
    if (!c->input[i].length) { return; }
    c->message_time = c->input[i].received_at;
    c->plaintext_bytes = c->input[i].length; c->input[i].length = 0U;
    if (b->lifecycle != UCN_LIFECYCLE_RUNNING ||
        b->now >= deadline_at(b, c->message_time, b->routes->reverse_lifetime_us) ||
        peer(b, i, c->input[i].opcode, UCN_I_SECURITY_ACCESS_INBOUND) != UCN_OK) { return; }
    if (c->input[i].fragmented && peer(b, i, 0x0206U, UCN_I_SECURITY_ACCESS_INBOUND) != UCN_OK) { return; }
    memcpy(c->plaintext, c->input[i].body, c->plaintext_bytes);
    if (c->input[i].opcode == 0x0201U) {
        r = ucn_i_route_control_request_decode(c->plaintext, c->plaintext_bytes, b->width, &c->request);
        /* Capacity/auth preflight, Route commit, and outbound flooding are
         * successive actor phases, not nested lifetimes. */
        if (r == UCN_OK) {
            if (c->request.key.target_address == b->local.address) { r = available(b, i, 0x0202U, 77U); }
            else if (!flood(b, &c->request, i, 0U, false)) { r = UCN_ERR_NO_SPACE; }
        }
        if (r == UCN_OK) { r = peer(b, i, 0x0201U, UCN_I_SECURITY_ACCESS_INBOUND); }
        if (r == UCN_OK) { r = request_received(b, i); }
        if (r == UCN_OK && c->request_action.kind == UCN_I_ROUTE_REQUEST_FORWARD) {
            (void)flood(b, &c->request_action.forwarded, i,
                deadline_at(b, c->request_action.reverse_expires_at_us, 0U), true);
        }
    }
    else if (c->input[i].opcode == 0x0202U) {
        r = reply_decode(b, i);
        if (r == UCN_OK && c->reply.key.origin.address != b->local.address) {
            int upstream = neighbor_for(b, &c->upstream);
            r = upstream < 0 ? UCN_ERR_NOT_FOUND : available(b, (uint16_t)upstream, 0x0202U, 77U);
        }
        if (r == UCN_OK) { r = peer(b, i, 0x0202U, UCN_I_SECURITY_ACCESS_INBOUND); }
        if (r == UCN_OK) { r = reply_received(b); }
        if (r == UCN_OK && c->reply_action.kind == UCN_I_ROUTE_REPLY_FORWARD) {
            int upstream = neighbor_for(b, &c->upstream);
            if (upstream >= 0) {
                (void)peer(b, (uint16_t)upstream, 0x0202U, UCN_I_SECURITY_ACCESS_OUTBOUND);
                reply_forward(b, (uint16_t)upstream);
            }
        }
    }
    else { (void)error_received(b, i); }
    memset(c->input[i].body, 0, sizeof(c->input[i].body));
}
static UCN_I_STACK_PHASE ucn_result_t discovery_allocate(ucn_i_native_routed_t *b, uint16_t i)
{
    ucn_i_native_route_control_t *c = b->control; uint8_t created; ucn_result_t r;
    r = ucn_i_route_ensure_discovery(b->routes, c->discoveries[i].request.key.target_address,
        c->discoveries[i].request.payload.required_capability_bits, c->discoveries[i].request.payload.minimum_frame_mtu,
        0U, b->now, &c->discoveries[i].handle, &c->request, &created);
    if (r == UCN_OK) { c->discoveries[i].request = c->request; }
    return r;
}
void ucn_i_native_route_control_discovery_step(ucn_i_native_routed_t *b)
{
    ucn_i_native_route_control_t *c = b->control; uint16_t i; ucn_result_t r;
    i = c->discovery_cursor; c->discovery_cursor = (uint16_t)((i + 1U) % UCN_I_ROUTE_DISCOVERY_COUNT);
    if (c->discoveries[i].occupied) {
        if (b->lifecycle != UCN_LIFECYCLE_RUNNING || b->now >= c->discoveries[i].deadline) { c->discoveries[i].occupied = 0U; }
        else if (flood(b, &c->discoveries[i].request, UINT16_MAX, c->discoveries[i].deadline, false)) {
            if (!c->discoveries[i].handle.slot) {
                r = discovery_allocate(b, i);
            } else { r = ucn_i_route_retry_discovery(b->routes, c->discoveries[i].handle, b->now, &c->request); }
            if (r == UCN_OK && ucn_i_route_control_request_encode(&c->request, b->width, c->encoded) == UCN_OK) {
                (void)flood(b, &c->request, UINT16_MAX, c->discoveries[i].deadline, true);
            } else if (r == UCN_ERR_NOT_FOUND || r == UCN_ERR_TIMEOUT) { c->discoveries[i].occupied = 0U; }
        }
    }
}
ucn_result_t ucn_i_native_route_control_step(ucn_i_native_routed_t *b)
{
    ucn_i_native_route_control_t *c = b->control; uint16_t i, inspected, expired;
    for (i = 0U; i < b->neighbor_count; ++i) {
        if (b->lifecycle != UCN_LIFECYCLE_RUNNING || (c->rx[i].deadline && b->now >= c->rx[i].deadline)) {
            c->rx[i].mask = 0U; c->rx[i].bundle = 0U; c->rx[i].deadline = 0U;
            memset(c->rx[i].body, 0, sizeof(c->rx[i].body));
        }
    }
    (void)ucn_i_route_maintain(b->routes, b->now, 1U, &inspected, &expired);
    return b->lifecycle == UCN_LIFECYCLE_FAULT ? UCN_ERR_STATE : UCN_OK;
}
bool ucn_i_native_route_control_quiet(const ucn_i_native_routed_t *b)
{
    uint16_t i; if (!b->control) { return true; }
    for (i = 0U; i < b->neighbor_count; ++i) {
        if (b->control->jobs[i].occupied || b->control->rx[i].mask || b->control->input[i].length) { return false; }
    }
    return true;
}
ucn_result_t ucn_i_native_route_control_init(ucn_i_native_routed_t *b, const ucn_i_native_routed_config_t *config)
{
    size_t i; ucn_i_native_route_control_t *c = config->control;
    if (!c) { return UCN_OK; }
    if (!config->reserve_c0) { return UCN_ERR_ARGUMENT; }
    for (i = 0U; i < sizeof(*c); ++i) { if (((const uint8_t *)c)[i]) { return UCN_ERR_STATE; } }
    c->root = b; c->reserve = config->reserve_c0; c->counter_context = config->counter_context; b->control = c;
    for (i = 0U; i < b->neighbor_count; ++i) {
        /* Outbound RREQ is the minimum usable control capability. Other exact
         * roles/directions are checked at their actual use, never inferred. */
        if (peer(b, (uint16_t)i, 0x0201U, UCN_I_SECURITY_ACCESS_OUTBOUND) != UCN_OK) {
            memset(c, 0, sizeof(*c)); b->control = NULL; return UCN_ERR_ACCESS;
        }
    }
    return UCN_OK;
}
