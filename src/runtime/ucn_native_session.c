#include "internal/ucn_native_session.h"
#include "internal/ucn_stack.h"
#include "internal/ucn_checked.h"
#include <string.h>

static ucn_result_t current(ucn_i_native_session_backend_t *b, uint64_t now)
{
    uint32_t generation; uint16_t mtu; bool ready;
    ucn_result_t r;
    if (now < b->now) { return UCN_ERR_STATE; }
    r = ucn_i_adapter_link_snapshot(b->adapter, 0U, &generation, &mtu, &ready);
    if (r != UCN_OK || !ready || generation != b->facts.link_generation) { return UCN_ERR_STATE; }
    b->facts.now_us = now;
    r = ucn_i_security_session_get(b->security, b->session, &b->session_view);
    if (r != UCN_OK || b->session_view.hop_profile != UCN_I_HOP_PROFILE_H0 ||
        b->session_view.phase != UCN_I_SECURITY_SESSION_ACTIVE ||
        now >= b->session_view.expires_at_us ||
        memcmp(&b->session_view.peer, &b->facts.peer, sizeof(b->facts.peer)) != 0) { return UCN_ERR_STATE; }
    return UCN_OK;
}
static UCN_I_STACK_PHASE ucn_result_t authorize(ucn_i_native_session_backend_t *b, uint8_t direction, uint64_t now)
{
    ucn_result_t r = current(b, now);
    if (r != UCN_OK) { return r; }
    b->access.direction = direction;
    return ucn_i_security_authorize(b->security, &b->access, &b->facts);
}
static ucn_result_t local_view(void *ctx, ucn_node_view_t *out)
{
    ucn_i_native_session_backend_t *b = ctx;
    memset(out, 0, sizeof(*out)); out->local_address = b->local.address;
    out->local_binding_generation = b->local.binding_generation;
    out->lifecycle = b->lifecycle;
    return UCN_OK;
}
static UCN_I_STACK_PHASE ucn_result_t preflight(void *ctx, const ucn_native_channel_t *channel)
{
    ucn_i_native_session_backend_t *b = ctx;
    uint32_t generation; uint16_t mtu; bool ready;
    ucn_result_t r;
    if (channel->service_id != b->service || channel->payload_bytes > UCN_NATIVE_PAYLOAD_BYTES ||
        b->session_view.origin_level + 1U < channel->security_minimum) { return UCN_ERR_UNSUPPORTED; }
    r = ucn_i_adapter_link_snapshot(b->adapter, 0U, &generation, &mtu, &ready);
    if (r != UCN_OK) { return r; }
    if (!ready || generation != b->facts.link_generation) { return UCN_ERR_STATE; }
    if (9U + 2U * b->width + 16U + channel->payload_bytes > mtu) { return UCN_ERR_NO_SPACE; }
    return UCN_OK;
}
static ucn_result_t binding(void *ctx, uint32_t address, ucn_native_source_t *out)
{
    ucn_i_native_session_backend_t *b = ctx;
    if (address == b->local.address) { *out = b->local; return UCN_OK; }
    if (address == b->peer.address) { *out = b->peer; return UCN_OK; }
    return UCN_ERR_NOT_FOUND;
}
static ucn_result_t start(void *ctx, uint64_t now)
{
    ucn_i_native_session_backend_t *b = ctx;
    ucn_result_t r;
    if (b->lifecycle != UCN_LIFECYCLE_INITIALIZED) { return UCN_ERR_STATE; }
    r = current(b, now);
    if (r != UCN_OK) { return r; }
    r = ucn_i_adapter_set_rx_enabled(b->adapter, true);
    if (r == UCN_OK) { b->lifecycle = UCN_LIFECYCLE_RUNNING; b->now = now; }
    return r;
}
static ucn_result_t validate(void *ctx, const ucn_native_scope_t *scope,
    const ucn_native_source_t *source, uint16_t service, uint8_t minimum,
    uint64_t now, uint8_t principal[16])
{
    ucn_i_native_session_backend_t *b = ctx;
    bool local = scope->instance == 0U && scope->generation == 0U && scope->slot == 0U;
    ucn_result_t r;
    if (b->lifecycle != UCN_LIFECYCLE_RUNNING || service != b->service || scope->reserved_zero ||
        (minimum != UCN_NATIVE_ORIGIN_AUTH && minimum != UCN_NATIVE_ENCRYPTED) ||
        (!local && (scope->instance != b->session.runtime_instance || scope->slot != b->session.slot ||
            scope->generation != b->session.slot_generation)) ||
        memcmp(source, local ? &b->local : &b->peer, sizeof(*source)) != 0) { return UCN_ERR_ACCESS; }
    r = authorize(b, local ? UCN_I_SECURITY_ACCESS_OUTBOUND : UCN_I_SECURITY_ACCESS_INBOUND, now);
    if (r != UCN_OK) { return r; }
    if (b->session_view.origin_level + 1U < minimum) { return UCN_ERR_SECURITY; }
    if (local) { memset(principal, 0, 16U); }
    else { memcpy(principal, b->facts.peer.principal, 16U); }
    return UCN_OK;
}
static void retire_tx(ucn_i_native_session_backend_t *b)
{
    if (ucn_i_adapter_tx_retire(b->adapter, &b->tx) == UCN_OK) {
        b->tx_live = 0U; b->tx_prepared = 0U; b->frame_bytes = 0U;
    }
}
/* EN: Share the Security/Coordinator callback domain across physical Driver calls.
 * ZH: Driver回调与Security/Coordinator共享控制回调域，短状态锁不跨Driver。 */
static ucn_result_t physical(ucn_i_native_session_backend_t *b, bool cancel)
{
    ucn_i_callback_claim_t *claim = &b->open_work.callback;
    ucn_result_t r, end;
    /* EN: Crypto is complete; the serialized physical phase owns this union.
     * ZH: 密码阶段已经返回；串行物理阶段独占复用同一固定工作区。 */
    claim->owner_instance = b->session.runtime_instance;
    claim->operation_id = b->tx.generation;
    claim->operation_generation = 1U;
    claim->operation_kind = cancel ? UINT16_C(0x0703) : UINT16_C(0x0702);
    r = ucn_i_callback_gate_enter(b->security->provider_gate, claim);
    if (r != UCN_OK) { return r; }
    r = cancel ? ucn_i_adapter_tx_cancel(b->adapter, &b->tx) :
        ucn_i_adapter_tx_submit_ref(b->adapter, &b->tx, b->frame, b->frame_bytes, &b->tx_view);
    end = ucn_i_callback_gate_leave(b->security->provider_gate, claim);
    if (end != UCN_OK) { b->lifecycle = UCN_LIFECYCLE_FAULT; return end; }
    return r;
}
static ucn_result_t stop(void *ctx)
{
    ucn_i_native_session_backend_t *b = ctx;
    b->lifecycle = UCN_LIFECYCLE_STOPPING; b->tx_prepared = 0U;
    return ucn_i_adapter_set_rx_enabled(b->adapter, false);
}
static ucn_result_t step(void *ctx, uint64_t now, uint16_t budget)
{
    ucn_i_native_session_backend_t *b = ctx;
    ucn_result_t r;
    if (!budget || now < b->now) { return UCN_ERR_STATE; }
    b->now = now;
    if (b->tx_live) {
        r = ucn_i_adapter_tx_view(b->adapter, &b->tx, &b->tx_view);
        if (r != UCN_OK) { return r; }
        if (b->tx_view.state == UCN_I_ADAPTER_TX_COMPLETED || b->tx_view.state == UCN_I_ADAPTER_TX_CANCELLED) { retire_tx(b); }
        else if (!b->tx_prepared || b->lifecycle == UCN_LIFECYCLE_STOPPING || now >= b->tx_deadline ||
            authorize(b, UCN_I_SECURITY_ACCESS_OUTBOUND, now) != UCN_OK) {
            if (physical(b, true) == UCN_OK) { retire_tx(b); }
        } else if (b->tx_view.state == UCN_I_ADAPTER_TX_NOT_SUBMITTED) {
            /* Adapter tokens are single-submit. Re-reserve a physical attempt,
             * but preserve the exact authenticated frame and Origin sequence. */
            r = ucn_i_adapter_tx_retire(b->adapter, &b->tx);
            if (r != UCN_OK) { return r; }
            b->tx_live = 0U;
        } else if (b->tx_view.state == UCN_I_ADAPTER_TX_RESERVED) {
            (void)physical(b, false);
        }
    }
    if (!b->tx_live && b->tx_prepared) {
        if (b->lifecycle == UCN_LIFECYCLE_STOPPING || now >= b->tx_deadline ||
            authorize(b, UCN_I_SECURITY_ACCESS_OUTBOUND, now) != UCN_OK) { b->tx_prepared = 0U; }
        else {
            r = ucn_i_adapter_tx_reserve(b->adapter, 0U, b->facts.link_generation, 0U, &b->tx);
            if (r == UCN_OK) {
                b->tx_live = 1U;
                (void)physical(b, false);
            } else if (r != UCN_ERR_NO_SPACE && r != UCN_ERR_STATE) { return r; }
        }
    }
    if (b->lifecycle == UCN_LIFECYCLE_STOPPING) {
        uint16_t visit; bool drained = false;
        r = ucn_i_adapter_set_rx_enabled(b->adapter, false);
        if (r != UCN_OK) { return r; }
        for (visit = 0U; visit < budget; ++visit) {
            if (!b->rx_live) {
                r = ucn_i_adapter_rx_claim(b->adapter, &b->rx);
                if (r == UCN_ERR_NOT_FOUND) { drained = true; break; }
                if (r != UCN_OK) { return r; }
                b->rx_live = 1U;
            }
            r = ucn_i_adapter_rx_retire(b->adapter, &b->rx.token);
            if (r != UCN_OK) { return r; }
            b->rx_live = 0U; b->rx_ready = 0U;
        }
        if (!b->tx_live && drained) {
            bool quiet = false;
            r = ucn_i_adapter_quiescent(b->adapter, &quiet);
            if (r != UCN_OK) { return r; }
            if (quiet) { b->lifecycle = UCN_LIFECYCLE_QUIESCENT; }
        }
    }
    return b->lifecycle == UCN_LIFECYCLE_FAULT ? UCN_ERR_STATE : UCN_OK;
}
static ucn_result_t send(void *ctx, const ucn_native_channel_t *channel,
    ucn_native_target_t target, const uint8_t *payload, uint64_t now, uint64_t deadline)
{
    ucn_i_native_session_backend_t *b = ctx;
    ucn_result_t r;
    if (b->lifecycle != UCN_LIFECYCLE_RUNNING) { return UCN_ERR_STATE; }
    if (b->tx_live || b->tx_prepared) { return UCN_ERR_NO_SPACE; }
    if (target.address != b->peer.address || target.binding_generation != b->peer.binding_generation ||
        channel->service_id != b->service || channel->payload_bytes > sizeof(b->plaintext) || now >= deadline) { return UCN_ERR_ACCESS; }
    r = authorize(b, UCN_I_SECURITY_ACCESS_OUTBOUND, now);
    if (r != UCN_OK) { return r; }
    r = preflight(b, channel);
    if (r != UCN_OK) { return r; }
    if (b->session_view.origin_level + 1U < channel->security_minimum) { return UCN_ERR_SECURITY; }
    b->tx_deadline = deadline;
    r = ucn_i_security_tx_reserve(b->security, &b->access, &b->facts, &b->security_tx);
    if (r != UCN_OK) { return r; }
    memset(&b->open_work.material, 0, sizeof(b->open_work.material));
    b->open_work.material.session = b->session; b->open_work.material.facts = b->facts;
    b->open_work.material.payload_bytes = channel->payload_bytes; b->open_work.material.source_address = b->local.address;
    b->open_work.material.destination_address = b->peer.address; b->open_work.material.origin_sequence = b->security_tx.origin_sequence;
    b->open_work.material.service_id = b->service; b->open_work.material.direction = UCN_I_SECURITY_ACCESS_OUTBOUND;
    b->open_work.material.common_header[0] = 0x61U;
    b->open_work.material.common_header[1] = (uint8_t)(channel->traffic_class << 6U);
    b->open_work.material.common_header[2] = (uint8_t)((b->session_view.origin_level << 6U) | 1U);
    r = ucn_i_security_c1_seal_layer(false, b->security, b->security_tx, &b->open_work.material, payload,
        &b->workspace, b->frame, sizeof(b->frame), &b->frame_bytes);
    if (r != UCN_OK) {
        (void)ucn_i_security_tx_abort(b->security, b->security_tx);
        return r;
    }
    b->tx_prepared = 1U;
    /* The actor step submits this RESERVED holder after command admission.
     * send only copies/protects; it never nests Driver work under admission.
     * step同轮接续提交，接纳栈不再嵌套物理Driver链。 */
    /* Once copied/prepared the backend owns the exact frame until retirement.
     * NOT_SUBMITTED/Unknown must not make Native manufacture a second sequence. */
    return UCN_OK;
}
static ucn_result_t poll(void *ctx, ucn_native_backend_message_t *message, uint8_t *out, size_t cap)
{
    ucn_i_native_session_backend_t *b = ctx;
    ucn_result_t r;
    if (b->lifecycle != UCN_LIFECYCLE_RUNNING || cap < UCN_NATIVE_PAYLOAD_BYTES) { return UCN_ERR_STATE; }
    if (!b->rx_live) {
        /* This Root exclusively owns ingress. Unsupported bytes must also be
         * claimed and retired, otherwise they can permanently fill the ring. */
        r = ucn_i_adapter_rx_claim(b->adapter, &b->rx);
        if (r != UCN_OK) { return r; }
        b->rx_live = 1U; b->rx_ready = 0U;
        r = authorize(b, UCN_I_SECURITY_ACCESS_INBOUND, b->now);
        if (r == UCN_OK && (b->rx.link_slot != 0U || b->rx.link_instance_generation != b->facts.link_generation ||
            b->rx.meta.timestamp_us > b->now)) { r = UCN_ERR_STATE; }
        if (r == UCN_OK) {
            if (b->rx.frame_bytes < 3U || (b->rx.frame[1] & 0x3FU) != 0U ||
                (b->rx.frame[2] & 0x3FU) != 1U) { r = UCN_ERR_UNSUPPORTED; }
        }
        if (r == UCN_OK) {
            ucn_i_security_c1_open_call_t *call = &b->open_work.open;
            memset(call, 0, sizeof(*call));
            call->session = b->session; call->facts = &b->facts; call->access = &b->access;
            call->packet = b->rx.frame; call->packet_bytes = b->rx.frame_bytes;
            call->plaintext_output = b->plaintext; call->plaintext_capacity = sizeof(b->plaintext);
            call->plaintext_bytes = &b->plaintext_bytes; call->replay_out = &b->replay;
            r = ucn_i_security_c1_open_borrowed(b->security, call, &b->workspace);
        }
        if (r == UCN_OK) {
            r = ucn_i_security_replay_commit(b->security, &b->replay, &b->facts);
            if (r != UCN_OK) { (void)ucn_i_security_replay_abort(b->security, &b->replay); }
        }
        if (r == UCN_OK) {
            const size_t sequence_offset = 5U + 2U * b->width;
            memset(&b->received, 0, sizeof(b->received)); b->received.source = b->peer;
            b->received.scope.instance = b->session.runtime_instance; b->received.scope.slot = b->session.slot;
            b->received.scope.generation = b->session.slot_generation;
            b->received.receive_time_us = b->rx.meta.timestamp_us;
            b->received.origin_sequence = ((uint32_t)b->rx.frame[sequence_offset] << 24U) |
                ((uint32_t)b->rx.frame[sequence_offset + 1U] << 16U) |
                ((uint32_t)b->rx.frame[sequence_offset + 2U] << 8U) | b->rx.frame[sequence_offset + 3U];
            b->received.service_id = b->service; b->received.payload_bytes = (uint16_t)b->plaintext_bytes;
            b->received.traffic_class = b->rx.frame[1] >> 6U;
            b->received.protection = (uint8_t)(b->session_view.origin_level + 1U); b->rx_ready = 1U;
        }
    }
    r = ucn_i_adapter_rx_retire(b->adapter, &b->rx.token);
    if (r != UCN_OK) { return r; }
    b->rx_live = 0U;
    if (!b->rx_ready) { return UCN_ERR_SECURITY; }
    b->rx_ready = 0U; *message = b->received;
    memcpy(out, b->plaintext, b->plaintext_bytes); memset(b->plaintext, 0, sizeof(b->plaintext));
    return UCN_OK;
}
ucn_result_t ucn_i_native_session_init(ucn_i_native_session_backend_t *b,
    ucn_i_security_owner_t *security, ucn_i_adapter_t *adapter, ucn_i_security_handle_t session,
    const ucn_i_security_current_facts_t *facts, const uint8_t fingerprint[16], uint8_t width,
    uint16_t service, uint64_t local_label, uint64_t peer_label)
{
    size_t i;
    bool quiet = false;
    if (b == NULL || security == NULL || adapter == NULL || facts == NULL || fingerprint == NULL ||
        !service || service == UINT16_MAX || !local_label || !peer_label || !width || width > 4U || width != security->address_width ||
        ucn_i_ranges_overlap(b, sizeof(*b), security, sizeof(*security)) ||
        ucn_i_ranges_overlap(b, sizeof(*b), adapter, sizeof(*adapter)) ||
        ucn_i_ranges_overlap(b, sizeof(*b), facts, sizeof(*facts)) ||
        ucn_i_ranges_overlap(b, sizeof(*b), fingerprint, 16U)) { return UCN_ERR_ARGUMENT; }
    for (i = 0U; i < sizeof(*b); ++i) { if (((const uint8_t *)b)[i] != 0U) { return UCN_ERR_STATE; } }
    if (ucn_i_adapter_quiescent(adapter, &quiet) != UCN_OK || !quiet) { return UCN_ERR_STATE; }
    /* No authentication is created here: the existing ACTIVE Session is mandatory. */
    b->security = security; b->adapter = adapter; b->session = session; b->facts = *facts;
    b->width = width; b->service = service; b->now = facts->now_us;
    memcpy(b->fingerprint, fingerprint, 16U);
    b->access.session = session; b->access.service_id = service;
    memcpy(b->access.context_fingerprint, fingerprint, 16U);
    if (current(b, facts->now_us) != UCN_OK) { memset(b, 0, sizeof(*b)); return UCN_ERR_STATE; }
    if (ucn_i_security_c1_context_check(security, &session, facts, fingerprint, NULL) != UCN_OK) {
        memset(b, 0, sizeof(*b)); return UCN_ERR_STATE;
    }
    /* Exact fingerprint and at least one usable Service direction are required
     * at assembly; never infer authorization from a configuration label. */
    if (authorize(b, UCN_I_SECURITY_ACCESS_INBOUND, facts->now_us) != UCN_OK &&
        authorize(b, UCN_I_SECURITY_ACCESS_OUTBOUND, facts->now_us) != UCN_OK) {
        memset(b, 0, sizeof(*b)); return UCN_ERR_STATE;
    }
    b->local.address = facts->local.address; b->local.binding_generation = facts->local.binding_generation; b->local.principal_digest = local_label;
    b->peer.address = facts->peer.address; b->peer.binding_generation = facts->peer.binding_generation; b->peer.principal_digest = peer_label;
    b->lifecycle = UCN_LIFECYCLE_INITIALIZED;
    b->port.struct_size = sizeof(b->port); b->port.api_version = UCN_NATIVE_API_VERSION; b->port.context = b;
    b->port.context_bytes = sizeof(*b); b->port.preflight = preflight;
    b->port.local_view = local_view; b->port.binding = binding; b->port.start = start; b->port.stop = stop;
    b->port.step = step; b->port.send = send; b->port.poll = poll; b->port.validate = validate;
    return UCN_OK;
}
