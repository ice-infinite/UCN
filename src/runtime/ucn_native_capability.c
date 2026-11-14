#include "internal/ucn_native_capability.h"
#include "internal/ucn_checked.h"
#include <string.h>

#define CAP_MAGIC UINT32_C(0x4e434150)

static bool zero_storage(const void *value, size_t length)
{
    const uint8_t *bytes = value; size_t i;
    for (i = 0U; i < length; ++i) if (bytes[i] != 0U) return false;
    return true;
}

static void binding(ucn_i_security_binding_t *out, const ucn_i_identity_binding_view_t *in)
{
    out->address = in->address; out->binding_generation = in->binding_generation;
    memcpy(out->principal, in->principal, sizeof(out->principal));
}

static bool output_disjoint(const ucn_i_native_capability_t *c, const void *out, size_t bytes)
{
    const ucn_i_native_join_config_t *p = &c->config.join_parents->config;
    return !ucn_i_ranges_overlap(c, sizeof(*c), out, bytes) &&
        !ucn_i_ranges_overlap(c->config.adapter, sizeof(*c->config.adapter), out, bytes) &&
        ucn_i_security_borrowed_output_check(c->config.security, out, bytes) == UCN_OK &&
        !ucn_i_ranges_overlap(c->config.capability_owner, sizeof(*c->config.capability_owner), out, bytes) &&
        !ucn_i_ranges_overlap(c->config.join_parents, sizeof(*c->config.join_parents), out, bytes) &&
        !ucn_i_ranges_overlap(c->config.join_parents->configuration_source,
            sizeof(*c->config.join_parents->configuration_source), out, bytes) &&
        !ucn_i_ranges_overlap(c->config.callback_gate, sizeof(*c->config.callback_gate), out, bytes) &&
        !ucn_i_ranges_overlap(c->config.counter_context, 1U, out, bytes) &&
        !ucn_i_ranges_overlap(p->identity, sizeof(*p->identity), out, bytes) &&
        !ucn_i_ranges_overlap(p->identity->domain_rules,
            p->identity->domain_rule_count * sizeof(*p->identity->domain_rules), out, bytes) &&
        !ucn_i_ranges_overlap(p->persistence, sizeof(*p->persistence), out, bytes) &&
        !ucn_i_ranges_overlap(p->persistence->callback_gate,
            sizeof(*p->persistence->callback_gate), out, bytes) &&
        (!p->admission || !ucn_i_ranges_overlap(p->admission, sizeof(*p->admission), out, bytes));
}

static ucn_result_t current(ucn_i_native_capability_t *c, uint64_t now)
{
    uint32_t generation; uint16_t mtu; bool ready, busy;
    ucn_i_callback_claim_t claim;
    ucn_result_t r;
    if (!c || c->magic != CAP_MAGIC || now == 0U || now < c->now_us) return UCN_ERR_STATE;
    r = ucn_i_callback_gate_view(c->config.callback_gate, &busy, &claim);
    if (r != UCN_OK || busy) return UCN_ERR_STATE;
    if (c->stopped) return UCN_ERR_STATE;
    if (now >= c->config.deadline_us) return UCN_ERR_TIMEOUT;
    r = ucn_i_adapter_link_snapshot(c->config.adapter, c->config.link_slot, &generation, &mtu, &ready);
    if (r != UCN_OK) return r;
    if (!ready || generation != c->config.link_generation || mtu < 120U ||
        c->config.local_record.link.link_frame_mtu > mtu ||
        c->config.local_record.link.processing_frame_mtu > UCN_ADAPTER_FRAME_BYTES ||
        c->config.local_record.link.link_frame_mtu < 120U ||
        c->config.local_record.link.processing_frame_mtu < 120U) return UCN_ERR_STATE;
    r = ucn_i_native_join_bindings_current(c->config.join_parents, now, &c->parents);
    if (r != UCN_OK) return r;
    memset(&c->facts, 0, sizeof(c->facts));
    binding(&c->facts.local, &c->parents.local); binding(&c->facts.peer, &c->parents.peer);
    c->facts.now_us = now; c->facts.link_generation = generation;
    c->facts.policy_generation = c->config.candidate.policy_generation;
    r = ucn_i_security_session_get(c->config.security, c->config.session, &c->session_view);
    if (r != UCN_OK) return r;
    r = ucn_i_security_session_candidate_get(c->config.security, c->config.session, &c->actual_candidate);
    if (r != UCN_OK) return r;
    r = ucn_i_security_record_encode(c->config.security->realm_id, &c->actual_candidate, c->actual_candidate_record);
    if (r != UCN_OK) return r;
    r = ucn_i_security_record_encode(c->config.security->realm_id, &c->config.candidate, c->expected_candidate_record);
    if (r != UCN_OK) return UCN_ERR_SECURITY;
    if (memcmp(c->actual_candidate_record, c->expected_candidate_record, UCN_I_SECURITY_RECORD_BYTES) != 0)
        return UCN_ERR_SECURITY;
    if (c->session_view.phase != UCN_I_SECURITY_SESSION_ACTIVE ||
        c->session_view.session_generation != c->config.candidate.session_generation ||
        c->session_view.expires_at_us != c->config.candidate.expires_at_us ||
        c->session_view.origin_level != c->config.candidate.origin_level ||
        c->session_view.hop_profile != UCN_I_HOP_PROFILE_H1 ||
        c->session_view.origin_tx_key_generation != c->config.candidate.origin_tx.key_generation ||
        c->session_view.origin_rx_key_generation != c->config.candidate.origin_rx.key_generation ||
        memcmp(&c->session_view.peer, &c->config.candidate.peer, sizeof(c->session_view.peer)) != 0 ||
        memcmp(&c->facts.local, &c->config.candidate.local, sizeof(c->facts.local)) != 0 ||
        memcmp(&c->facts.peer, &c->config.candidate.peer, sizeof(c->facts.peer)) != 0)
        return UCN_ERR_SECURITY;
    r = ucn_i_security_c1_context_check(c->config.security, &c->config.session, &c->facts,
        c->config.candidate.origin_fingerprint, c->config.candidate.hop_fingerprint);
    if (r == UCN_OK) {
        c->now_us = now;
        memset(&c->peer, 0, sizeof(c->peer));
        c->peer.runtime_instance = c->config.security->runtime_instance;
        c->peer.realm_id = c->config.security->realm_id; c->peer.address = c->facts.peer.address;
        c->peer.binding_generation = c->facts.peer.binding_generation;
        c->peer.session_generation = c->config.session.session_generation;
        c->peer.link_id = (uint32_t)c->config.link_slot + 1U; c->peer.link_generation = c->config.link_generation;
        c->peer.expires_at_us = c->session_view.expires_at_us;
        c->peer.security_owner_instance = c->config.security->owner_instance;
        memcpy(c->peer.principal, c->facts.peer.principal, 16U);
    }
    return r;
}

ucn_result_t ucn_i_native_capability_init(ucn_i_native_capability_t *c,
    const ucn_i_native_capability_config_t *config, uint64_t now)
{
    ucn_result_t r;
    if (!c || !config || !now || ucn_i_ranges_overlap(c, sizeof(*c), config, sizeof(*config))) return UCN_ERR_ARGUMENT;
    if ((config->adapter && ucn_i_ranges_overlap(c, sizeof(*c), config->adapter, sizeof(*config->adapter))) ||
        (config->security && ucn_i_security_borrowed_output_check(config->security, c, sizeof(*c)) != UCN_OK) ||
        (config->capability_owner && ucn_i_ranges_overlap(c, sizeof(*c), config->capability_owner, sizeof(*config->capability_owner))) ||
        (config->join_parents && ucn_i_ranges_overlap(c, sizeof(*c), config->join_parents, sizeof(*config->join_parents))) ||
        (config->callback_gate && ucn_i_ranges_overlap(c, sizeof(*c), config->callback_gate, sizeof(*config->callback_gate))) ||
        (config->counter_context && ucn_i_ranges_overlap(c, sizeof(*c), config->counter_context, 1U))) return UCN_ERR_ARGUMENT;
    if (config->join_parents) {
        const ucn_i_native_join_t *j = config->join_parents;
        const ucn_i_native_join_config_t *p = &j->config;
        if (ucn_i_ranges_overlap(c, sizeof(*c), j->configuration_source, sizeof(*j->configuration_source)) ||
            (p->identity && (ucn_i_ranges_overlap(c, sizeof(*c), p->identity, sizeof(*p->identity)) ||
                ucn_i_ranges_overlap(c, sizeof(*c), p->identity->domain_rules,
                    p->identity->domain_rule_count * sizeof(*p->identity->domain_rules)))) ||
            (p->persistence && (ucn_i_ranges_overlap(c, sizeof(*c), p->persistence, sizeof(*p->persistence)) ||
                ucn_i_ranges_overlap(c, sizeof(*c), p->persistence->callback_gate,
                    sizeof(*p->persistence->callback_gate)))) ||
            (p->admission && ucn_i_ranges_overlap(c, sizeof(*c), p->admission, sizeof(*p->admission))))
            return UCN_ERR_ARGUMENT;
    }
    if (!zero_storage(c, sizeof(*c))) return UCN_ERR_STATE;
    if (!config->adapter || !config->security || !config->capability_owner ||
        !config->join_parents || !config->callback_gate || !config->reserve_c0 || !config->counter_context ||
        !config->join_parents->handed_off || config->security->provider_gate != config->callback_gate ||
        config->join_parents->config.adapter != config->adapter ||
        config->join_parents->config.security != config->security ||
        config->join_parents->config.callback_gate != config->callback_gate ||
        config->capability_owner->runtime_instance != config->security->runtime_instance ||
        config->capability_owner->realm_id != config->security->realm_id ||
        config->capability_owner->security_owner_instance != config->security->owner_instance ||
        config->candidate.link_generation != config->link_generation ||
        config->deadline_us <= now || config->deadline_us > config->candidate.expires_at_us ||
        !config->retry_interval_us || config->max_attempts < 2U || !config->owner_instance ||
        !config->physical_peer || !config->link_generation || config->device > 1U || config->has_predecessor > 1U ||
        config->local_record.link.link_instance_generation != config->link_generation)
        return UCN_ERR_CONFIG;
    memset(c, 0, sizeof(*c)); c->config = *config; c->magic = CAP_MAGIC;
    r = ucn_i_capability_digest(&c->config.local_record, c->local_digest);
    if (r == UCN_OK) r = current(c, now);
    if (r != UCN_OK) { memset(c, 0, sizeof(*c)); return r; }
    return UCN_OK;
}

ucn_result_t ucn_i_native_capability_selected(ucn_i_native_capability_t *c, uint64_t now,
    ucn_i_capability_ref_t *ref, ucn_i_cached_capability_t *cache, ucn_i_profile_select_t *profile)
{
    ucn_result_t r;
    if (!c || c->magic != CAP_MAGIC || !ref || !cache || !profile ||
        !output_disjoint(c, ref, sizeof(*ref)) ||
        !output_disjoint(c, cache, sizeof(*cache)) ||
        !output_disjoint(c, profile, sizeof(*profile)) ||
        ucn_i_ranges_overlap(ref, sizeof(*ref), cache, sizeof(*cache)) ||
        ucn_i_ranges_overlap(ref, sizeof(*ref), profile, sizeof(*profile)) ||
        ucn_i_ranges_overlap(cache, sizeof(*cache), profile, sizeof(*profile))) return UCN_ERR_ARGUMENT;
    r = current(c, now); if (r != UCN_OK) return r;
    if (!c->selected_ready) return UCN_ERR_NOT_FOUND;
    r = ucn_i_capability_get(c->config.capability_owner, &c->reference, now, &c->cached);
    if (r != UCN_OK) return r;
    if ((c->config.device &&
         (memcmp(c->profile.local_digest, c->local_digest, 16U) != 0 ||
          memcmp(c->profile.peer_digest, c->cached.digest, 16U) != 0 ||
          c->profile.local_generation != c->config.local_record.capability_generation ||
          c->profile.peer_generation != c->cached.record.capability_generation)) ||
        (!c->config.device &&
         (memcmp(c->profile.peer_digest, c->local_digest, 16U) != 0 ||
          memcmp(c->profile.local_digest, c->cached.digest, 16U) != 0 ||
          c->profile.peer_generation != c->config.local_record.capability_generation ||
          c->profile.local_generation != c->cached.record.capability_generation))) return UCN_ERR_SECURITY;
    *ref = c->reference; *cache = c->cached; *profile = c->profile;
    return UCN_OK;
}

static ucn_result_t enter_callback(ucn_i_native_capability_t *c)
{
    if (c->operation_id == UINT32_MAX) return UCN_ERR_EXHAUSTED;
    memset(&c->claim, 0, sizeof(c->claim));
    c->claim.owner_instance = c->config.owner_instance;
    c->claim.operation_id = ++c->operation_id;
    c->claim.operation_generation = 1U; c->claim.operation_kind = 1U;
    return ucn_i_callback_gate_enter(c->config.callback_gate, &c->claim);
}
static ucn_result_t leave_callback(ucn_i_native_capability_t *c, ucn_result_t result)
{
    return ucn_i_callback_gate_leave(c->config.callback_gate, &c->claim) == UCN_OK ? result : UCN_ERR_STATE;
}

static ucn_result_t select_digest(ucn_i_native_capability_t *c, bool outbound, uint64_t transaction)
{
    ucn_i_capability_select_context_t *s = &c->select_context;
    const ucn_i_security_binding_t *source = outbound ? &c->facts.local : &c->facts.peer;
    const ucn_i_security_binding_t *destination = outbound ? &c->facts.peer : &c->facts.local;
    memset(s, 0, sizeof(*s)); s->realm = c->config.security->realm_id;
    s->source_address = source->address; s->source_generation = source->binding_generation;
    memcpy(s->source_principal, source->principal, 16U);
    s->destination_address = destination->address; s->destination_generation = destination->binding_generation;
    memcpy(s->destination_principal, destination->principal, 16U);
    s->session = c->config.candidate.session_generation;
    s->key = outbound ? c->config.candidate.origin_tx.key_generation : c->config.candidate.origin_rx.key_generation;
    s->policy = c->config.candidate.policy_generation; s->origin = c->config.candidate.origin_level;
    memcpy(s->fingerprint, c->config.candidate.origin_fingerprint, 16U);
    s->transaction = transaction; s->service = c->config.service;
    return ucn_i_capability_select_transcript(s, &c->profile, c->select_digest, &c->hash);
}

static ucn_result_t prepare_control(ucn_i_native_capability_t *c)
{
    ucn_i_security_control_access_request_t *access = &c->control_access;
    ucn_i_security_c0_protect_request_t *request = &c->protect_request;
    ucn_result_t r;
    uint8_t count = (c->tx_opcode == UCN_I_OPCODE_CAPABILITY_ADVERTISE ||
        c->tx_opcode == UCN_I_OPCODE_CAPABILITY_ACK) ? 2U : 1U;
    uint8_t index;
    size_t fragment_length;
    memset(access, 0, sizeof(*access)); access->session = c->config.session;
    access->protocol_opcode = c->tx_opcode; access->direction = UCN_I_SECURITY_ACCESS_OUTBOUND;
    memcpy(access->context_fingerprint, c->config.candidate.origin_fingerprint, 16U);
    r = ucn_i_security_control_authorize(c->config.security, access, &c->facts);
    if (r != UCN_OK) return r;
    memcpy(access->context_fingerprint, c->config.candidate.hop_fingerprint, 16U);
    r = ucn_i_security_control_authorize(c->config.security, access, &c->facts);
    if (r != UCN_OK) return r;
    if (count == 2U) {
        access->protocol_opcode = UCN_I_OPCODE_CAPABILITY_FRAGMENT;
        memcpy(access->context_fingerprint, c->config.candidate.origin_fingerprint, 16U);
        r = ucn_i_security_control_authorize(c->config.security, access, &c->facts);
        if (r != UCN_OK) return r;
        memcpy(access->context_fingerprint, c->config.candidate.hop_fingerprint, 16U);
        r = ucn_i_security_control_authorize(c->config.security, access, &c->facts);
        if (r != UCN_OK) return r;
    }
    r = enter_callback(c); if (r != UCN_OK) return r;
    r = c->config.reserve_c0(c->config.counter_context, c->config.security,
        c->config.session, &c->tx_transaction[0]);
    if (r == UCN_OK && count == 2U)
        r = c->config.reserve_c0(c->config.counter_context, c->config.security,
            c->config.session, &c->tx_transaction[1]);
    r = leave_callback(c, r); if (r != UCN_OK) return r;
    if (!c->tx_transaction[0] || (count == 2U && (c->tx_transaction[0] == UINT64_MAX ||
        c->tx_transaction[1] != c->tx_transaction[0] + 1U))) return UCN_ERR_STATE;
    if (c->tx_opcode == UCN_I_OPCODE_CAPABILITY_SELECT) {
        r = select_digest(c, true, c->tx_transaction[0]); if (r != UCN_OK) return r;
    }
    memset(request, 0, sizeof(*request)); request->session = c->config.session;
    request->facts = c->facts; request->transaction_id = c->tx_transaction[0];
    request->protocol_opcode = count == 2U ? UCN_I_OPCODE_CAPABILITY_FRAGMENT : c->tx_opcode;
    memcpy(request->context_fingerprint, c->config.candidate.origin_fingerprint, 16U);
    request->traffic_class = UCN_TRAFFIC_Q2; request->delivery_guarantee = UCN_DELIVERY_BEST_EFFORT;
    request->interaction_role = c->tx_opcode == UCN_I_OPCODE_CAPABILITY_ACK ? UCN_INTERACTION_RESULT :
        (c->tx_opcode == UCN_I_OPCODE_CAPABILITY_QUERY || c->tx_opcode == UCN_I_OPCODE_CAPABILITY_SELECT) ?
        UCN_INTERACTION_REQUEST : UCN_INTERACTION_ONE_WAY;
    request->payload_kind = UCN_I_PAYLOAD_CONTROL;
    request->hop_limit = 1U; request->direction = UCN_I_SECURITY_ACCESS_OUTBOUND;
    for (index = 0U; index < count; ++index) {
        const uint8_t *payload = c->body; size_t payload_bytes = c->tx_body_bytes;
        request->transaction_id = c->tx_transaction[index];
        if (count == 2U) {
            size_t half = (c->tx_body_bytes + 1U) / 2U;
            size_t offset = index * half;
            memset(&c->fragment, 0, sizeof(c->fragment));
            c->fragment.opcode = c->tx_opcode; c->fragment.total_bytes = (uint16_t)c->tx_body_bytes;
            c->fragment.index = index; c->fragment.count = 2U;
            c->fragment.chunk_bytes = (uint8_t)(index ? c->tx_body_bytes - offset : half);
            memcpy(c->fragment.chunk, c->body + offset, c->fragment.chunk_bytes);
            r = ucn_i_capability_fragment_encode(&c->fragment, c->fragment_bytes,
                sizeof(c->fragment_bytes), &fragment_length);
            if (r != UCN_OK) return r;
            payload = c->fragment_bytes; payload_bytes = fragment_length;
        }
        r = ucn_i_security_c0_protect(c->config.security, request,
            payload, payload_bytes, &c->packet_work,
            c->tx_frame[index], sizeof(c->tx_frame[index]), &c->tx_bytes[index]);
        if (r != UCN_OK) return r;
    }
    c->tx_count = count; c->tx_index = 0U;
    c->tx_ready = 1U; c->pending_control = 0U;
    if (c->tx_opcode == UCN_I_OPCODE_CAPABILITY_SELECT) c->phase = 3U;
    return UCN_OK;
}

static ucn_result_t prepare_hello(ucn_i_native_capability_t *c)
{
    ucn_i_capability_summary_t *summary = &c->summary;
    ucn_result_t r;
    memset(summary, 0, sizeof(*summary));
    summary->capability_generation = c->config.local_record.capability_generation;
    summary->link_instance_generation = c->config.local_record.link.link_instance_generation;
    memcpy(summary->digest, c->local_digest, 16U);
    r = ucn_i_capability_summary_encode(summary, c->body); if (r != UCN_OK) return r;
    c->tx_opcode = UCN_I_OPCODE_NEIGHBOR_HELLO; c->tx_body_bytes = UCN_I_CAPABILITY_SUMMARY_BYTES;
    r = prepare_control(c); if (r == UCN_OK) c->phase = 1U;
    return r;
}

static ucn_result_t duplicate_request(ucn_i_native_capability_t *c, const ucn_i_adapter_rx_view_t *rx,
    uint16_t opcode)
{
    uint8_t index; bool match = false, reply = false;
    ucn_result_t r;
    if (opcode == c->cached_request_opcode && c->select_origin_bytes != 0U &&
        rx->frame_bytes == c->select_origin_bytes + UCN_I_SECURITY_HOP_TRAILER_BYTES &&
        memcmp(rx->frame, c->select_origin, c->select_origin_bytes) == 0) {
        match = true; reply = c->cached_reply_opcode == c->tx_opcode;
    }
    if (opcode == UCN_I_OPCODE_CAPABILITY_FRAGMENT) {
        for (index = 0U; index < 2U; ++index) {
            size_t bytes = c->fragment_origin_bytes[index];
            if ((c->fragment_origin_bitmap & (uint8_t)(1U << index)) && bytes != 0U &&
                rx->frame_bytes == bytes + UCN_I_SECURITY_HOP_TRAILER_BYTES &&
                memcmp(rx->frame, c->fragment_origin[index], bytes) == 0) {
                match = true;
                reply = !c->rx_bitmap && c->cached_reply_opcode != 0U && c->cached_reply_opcode == c->tx_opcode;
                break;
            }
        }
    }
    if (!match) return UCN_ERR_NOT_FOUND;
    memset(&c->control_access, 0, sizeof(c->control_access));
    c->control_access.session = c->config.session; c->control_access.protocol_opcode = opcode;
    c->control_access.direction = UCN_I_SECURITY_ACCESS_INBOUND;
    memcpy(c->control_access.context_fingerprint, c->config.candidate.origin_fingerprint, 16U);
    r = ucn_i_security_control_authorize(c->config.security, &c->control_access, &c->facts);
    if (r != UCN_OK) return r;
    if (opcode == UCN_I_OPCODE_CAPABILITY_FRAGMENT) {
        c->control_access.protocol_opcode = c->rx_opcode;
        r = ucn_i_security_control_authorize(c->config.security, &c->control_access, &c->facts);
        if (r != UCN_OK) return r;
        memcpy(c->control_access.context_fingerprint, c->config.candidate.hop_fingerprint, 16U);
        r = ucn_i_security_control_authorize(c->config.security, &c->control_access, &c->facts);
        if (r != UCN_OK) return r;
    }
    r = ucn_i_security_c0_hop_open(c->config.security, c->config.session, &c->facts,
        rx->frame, rx->frame_bytes, &c->path_work);
    if (r != UCN_OK) return r;
    r = ucn_i_security_hop_replay_commit(c->config.security, &c->path_work.relay.replay, &c->facts);
    if (r != UCN_OK) return r;
    if (reply && !c->pending_control) {
        if (c->tx_live || (c->tx_ready && c->tx_index != 0U)) c->requeue = 1U;
        else if (!c->tx_ready) { c->tx_index = 0U; c->tx_ready = 1U; c->tx_refreshed = 0U; }
    }
    return UCN_OK;
}

static void remember_request(ucn_i_native_capability_t *c, const ucn_i_adapter_rx_view_t *rx,
    uint16_t opcode, uint16_t reply)
{
    c->select_origin_bytes = rx->frame_bytes - UCN_I_SECURITY_HOP_TRAILER_BYTES;
    memcpy(c->select_origin, rx->frame, c->select_origin_bytes);
    c->cached_request_opcode = opcode; c->cached_reply_opcode = reply;
    c->fragment_origin_bitmap = 0U;
}

ucn_result_t ucn_i_native_capability_receive_claimed(ucn_i_native_capability_t *c,
    const ucn_i_adapter_rx_view_t *rx, uint64_t now)
{
    ucn_i_c0_frame_t *frame;
    ucn_i_capability_query_t *query;
    ucn_i_capability_summary_result_t summary_result;
    ucn_result_t r;
    if (!c || !rx || !rx->frame || rx->link_slot != c->config.link_slot ||
        rx->link_instance_generation != c->config.link_generation ||
        rx->meta.sender_discriminator != c->config.physical_peer) return UCN_ERR_ARGUMENT;
    frame = &c->rx_frame; query = &c->query;
    r = current(c, now); if (r != UCN_OK) return r;
    r = ucn_i_c0_decode(rx->frame, rx->frame_bytes, c->config.candidate.address_width, UCN_I_CONTROL_H1, frame);
    if (r != UCN_OK) return r;
    r = duplicate_request(c, rx, frame->opcode);
    if (r != UCN_ERR_NOT_FOUND) return r;
    if (c->tx_ready || c->tx_live || c->pending_control) return UCN_ERR_STATE;
    if (!((!c->config.device && c->phase == 0U && frame->opcode == UCN_I_OPCODE_NEIGHBOR_HELLO) ||
          (c->config.device && c->phase == 1U && frame->opcode == UCN_I_OPCODE_CAPABILITY_QUERY) ||
          (((c->phase == 2U) || (c->config.device && c->phase == 3U)) && frame->opcode == UCN_I_OPCODE_CAPABILITY_FRAGMENT) ||
          (c->phase == 4U && frame->opcode == UCN_I_OPCODE_CAPABILITY_REVOKE) ||
          (c->phase == 4U && (frame->opcode == UCN_I_OPCODE_NEIGHBOR_KEEPALIVE ||
           frame->opcode == UCN_I_OPCODE_CAPABILITY_ADVERTISE || frame->opcode == UCN_I_OPCODE_CAPABILITY_FRAGMENT)) ||
          (!c->config.device && c->phase == 2U && frame->opcode == UCN_I_OPCODE_CAPABILITY_SELECT)))
        return UCN_ERR_UNSUPPORTED;
    memset(&c->control_access, 0, sizeof(c->control_access));
    c->control_access.session = c->config.session; c->control_access.protocol_opcode = frame->opcode;
    c->control_access.direction = UCN_I_SECURITY_ACCESS_INBOUND;
    memcpy(c->control_access.context_fingerprint, c->config.candidate.origin_fingerprint, 16U);
    r = ucn_i_security_control_authorize(c->config.security, &c->control_access, &c->facts);
    if (r != UCN_OK) return r;
    memcpy(c->control_access.context_fingerprint, c->config.candidate.hop_fingerprint, 16U);
    r = ucn_i_security_control_authorize(c->config.security, &c->control_access, &c->facts);
    if (r != UCN_OK) return r;
    memcpy(c->control_access.context_fingerprint, c->config.candidate.origin_fingerprint, 16U);
    r = ucn_i_security_c0_open(c->config.security, &c->control_access, &c->facts,
        rx->frame, rx->frame_bytes, &c->packet_work,
        c->plaintext, sizeof(c->plaintext), &c->rx_body_bytes, &c->authenticated);
    if (r != UCN_OK) return r;
    if (c->authenticated.protocol_opcode != frame->opcode ||
        c->authenticated.traffic_class != UCN_TRAFFIC_Q2 ||
        c->authenticated.delivery_guarantee != UCN_DELIVERY_BEST_EFFORT ||
        (frame->opcode != UCN_I_OPCODE_CAPABILITY_FRAGMENT &&
         c->authenticated.interaction_role != ((frame->opcode == UCN_I_OPCODE_CAPABILITY_QUERY ||
         frame->opcode == UCN_I_OPCODE_CAPABILITY_SELECT) ? UCN_INTERACTION_REQUEST : UCN_INTERACTION_ONE_WAY)) ||
        c->authenticated.payload_kind != UCN_I_PAYLOAD_CONTROL || c->authenticated.hop_limit != 1U) {
        (void)ucn_i_security_replay_abort(c->config.security, &c->authenticated.replay);
        return UCN_ERR_MALFORMED;
    }
    if (c->phase == 4U && frame->opcode == UCN_I_OPCODE_NEIGHBOR_KEEPALIVE) {
        if (c->rx_body_bytes != UCN_I_CAPABILITY_SUMMARY_BYTES ||
            ucn_i_capability_summary_decode(c->plaintext, &c->summary) != UCN_OK) {
            (void)ucn_i_security_replay_abort(c->config.security, &c->authenticated.replay);
            return UCN_ERR_MALFORMED;
        }
        r = ucn_i_capability_summary_ingest(c->config.capability_owner, &c->peer, &c->summary, now, &summary_result);
        if (r == UCN_OK) r = ucn_i_security_control_replay_complete(c->config.security, &c->authenticated.replay, &c->facts);
        if (r == UCN_OK) r = ucn_i_security_control_replay_commit(c->config.security, &c->authenticated.replay, &c->facts);
        if (r != UCN_OK) return r;
        if (summary_result != UCN_I_CAPABILITY_SUMMARY_MATCHED) { c->selected_ready = 0U; c->phase = 5U; }
        return UCN_OK;
    }
    if (c->phase == 4U && (frame->opcode == UCN_I_OPCODE_CAPABILITY_ADVERTISE ||
                           frame->opcode == UCN_I_OPCODE_CAPABILITY_FRAGMENT)) {
        /* Updated record re-selection is not implemented; fence on an actual
         * authenticated update, never keep using a potentially stale profile. */
        r = ucn_i_security_control_replay_complete(c->config.security, &c->authenticated.replay, &c->facts);
        if (r == UCN_OK) r = ucn_i_security_control_replay_commit(c->config.security, &c->authenticated.replay, &c->facts);
        c->selected_ready = 0U; c->phase = 5U;
        return r == UCN_OK ? UCN_ERR_UNSUPPORTED : r;
    }
    if (frame->opcode == UCN_I_OPCODE_CAPABILITY_REVOKE) {
        r = c->rx_body_bytes == UCN_I_CAPABILITY_QUERY_BYTES ?
            ucn_i_capability_query_decode(c->plaintext, query) : UCN_ERR_MALFORMED;
        if (r == UCN_OK) r = ucn_i_capability_get(c->config.capability_owner, &c->reference, now, &c->cached);
        if (r != UCN_OK || query->requested_generation != c->cached.record.capability_generation ||
            memcmp(query->known_digest, c->cached.digest, 16U) != 0) {
            (void)ucn_i_security_replay_abort(c->config.security, &c->authenticated.replay);
            return UCN_ERR_SECURITY;
        }
        r = ucn_i_security_control_replay_complete(c->config.security, &c->authenticated.replay, &c->facts);
        if (r == UCN_OK) r = ucn_i_security_control_replay_commit(c->config.security, &c->authenticated.replay, &c->facts);
        c->selected_ready = 0U; c->phase = 5U;
        if (r == UCN_OK) r = ucn_i_capability_revoke(c->config.capability_owner, &c->reference, query);
        return r;
    }
    if (frame->opcode == UCN_I_OPCODE_CAPABILITY_SELECT) {
        r = ucn_i_capability_digest(&c->remote_record, c->remote_digest);
        if (r == UCN_OK) r = ucn_i_profile_select_build(&c->remote_record, c->remote_digest,
            &c->config.local_record, c->local_digest, &c->config.requirements, &c->profile);
        if (r == UCN_OK) r = ucn_i_profile_select_encode(&c->profile, c->body);
        if (r != UCN_OK || c->rx_body_bytes != UCN_I_PROFILE_SELECT_BYTES ||
            memcmp(c->body, c->plaintext, UCN_I_PROFILE_SELECT_BYTES) != 0 ||
            select_digest(c, false, c->authenticated.transaction_id) != UCN_OK) {
            (void)ucn_i_security_replay_abort(c->config.security, &c->authenticated.replay);
            return UCN_ERR_SECURITY;
        }
        c->ack.selected = c->profile; memcpy(c->ack.transcript_digest, c->select_digest, 16U);
        r = ucn_i_profile_ack_encode(&c->ack, c->body);
        if (r == UCN_OK) r = ucn_i_security_control_replay_complete(c->config.security, &c->authenticated.replay, &c->facts);
        if (r == UCN_OK) r = ucn_i_security_control_replay_commit(c->config.security, &c->authenticated.replay, &c->facts);
        if (r != UCN_OK) return r;
        remember_request(c, rx, UCN_I_OPCODE_CAPABILITY_SELECT, UCN_I_OPCODE_CAPABILITY_ACK);
        c->tx_opcode = UCN_I_OPCODE_CAPABILITY_ACK; c->tx_body_bytes = UCN_I_PROFILE_ACK_BYTES;
        c->pending_control = 1U; c->phase = 3U; return UCN_OK;
    }
    if (frame->opcode == UCN_I_OPCODE_CAPABILITY_FRAGMENT) {
        uint64_t base;
        size_t offset;
        r = ucn_i_capability_fragment_decode(c->plaintext, c->rx_body_bytes, &c->fragment);
        if (r != UCN_OK ||
            c->fragment.opcode != ((c->config.device && c->phase == 3U) ? UCN_I_OPCODE_CAPABILITY_ACK : UCN_I_OPCODE_CAPABILITY_ADVERTISE) ||
            c->authenticated.interaction_role != (c->fragment.opcode == UCN_I_OPCODE_CAPABILITY_ACK ?
                UCN_INTERACTION_RESULT : UCN_INTERACTION_ONE_WAY) ||
            c->authenticated.transaction_id <= c->fragment.index) goto reject_fragment;
        base = c->authenticated.transaction_id - c->fragment.index;
        if (base == UINT64_MAX || base <= c->rx_highest_end ||
            (c->rx_bitmap && (base != c->rx_base || c->rx_opcode != c->fragment.opcode)) ||
            (c->rx_bitmap & (uint8_t)(1U << c->fragment.index))) goto reject_fragment;
        c->control_access.protocol_opcode = c->fragment.opcode;
        r = ucn_i_security_control_authorize(c->config.security, &c->control_access, &c->facts);
        if (r != UCN_OK) goto reject_fragment;
        memcpy(c->control_access.context_fingerprint, c->config.candidate.hop_fingerprint, 16U);
        r = ucn_i_security_control_authorize(c->config.security, &c->control_access, &c->facts);
        if (r != UCN_OK) goto reject_fragment;
        r = ucn_i_security_control_replay_complete(c->config.security, &c->authenticated.replay, &c->facts);
        if (r == UCN_OK) r = ucn_i_security_control_replay_commit(c->config.security, &c->authenticated.replay, &c->facts);
        if (r != UCN_OK) return r;
        if (!c->rx_bitmap) {
            c->rx_base = base; c->rx_opcode = c->fragment.opcode;
            c->fragment_origin_bitmap = 0U;
            c->cached_request_opcode = 0U; c->cached_reply_opcode = 0U;
        }
        c->fragment_origin_bytes[c->fragment.index] = rx->frame_bytes - UCN_I_SECURITY_HOP_TRAILER_BYTES;
        memcpy(c->fragment_origin[c->fragment.index], rx->frame, c->fragment_origin_bytes[c->fragment.index]);
        c->fragment_origin_bitmap |= (uint8_t)(1U << c->fragment.index);
        offset = (size_t)c->fragment.index * ((c->fragment.total_bytes + 1U) / 2U);
        memcpy(c->rx_body + offset, c->fragment.chunk, c->fragment.chunk_bytes);
        c->rx_bitmap |= (uint8_t)(1U << c->fragment.index);
        if (c->rx_bitmap != 3U) return UCN_OK;
        if (c->fragment.opcode == UCN_I_OPCODE_CAPABILITY_ACK) {
            r = ucn_i_profile_ack_decode(c->rx_body, &c->ack);
            if (r == UCN_OK) r = ucn_i_profile_ack_verify_exact(&c->ack, &c->profile, c->select_digest);
            if (r != UCN_OK) { c->rx_bitmap = 0U; return r; }
            c->rx_highest_end = base + 1U; c->rx_bitmap = 0U;
            c->cached_reply_opcode = 0U; c->selected_ready = 1U; c->phase = 4U; return UCN_OK;
        }
        r = ucn_i_capability_record_decode(c->rx_body, &c->remote_record);
        if (r == UCN_OK) r = ucn_i_capability_digest(&c->remote_record, c->remote_digest);
        if (r != UCN_OK || (!c->config.device &&
            (c->remote_record.capability_generation != c->summary.capability_generation ||
             c->remote_record.link.link_instance_generation != c->summary.link_instance_generation ||
             memcmp(c->remote_digest, c->summary.digest, 16U) != 0))) { c->rx_bitmap = 0U; return UCN_ERR_SECURITY; }
        if (c->config.has_predecessor)
            r = ucn_i_capability_replace_session(c->config.capability_owner, &c->config.predecessor,
                &c->peer, &c->remote_record, now, &c->reference);
        else
            r = ucn_i_capability_advertise_ingest(c->config.capability_owner, &c->peer,
                &c->remote_record, now, &c->reference);
        if (r != UCN_OK) return r;
        c->rx_highest_end = base + 1U; c->rx_bitmap = 0U;
        if (c->config.device) {
            r = ucn_i_profile_select_build(&c->config.local_record, c->local_digest,
                &c->remote_record, c->remote_digest, &c->config.requirements, &c->profile);
            if (r == UCN_OK) r = ucn_i_profile_select_encode(&c->profile, c->body);
            if (r != UCN_OK) return r;
            c->tx_opcode = UCN_I_OPCODE_CAPABILITY_SELECT; c->tx_body_bytes = UCN_I_PROFILE_SELECT_BYTES;
        } else {
            r = ucn_i_capability_record_encode(&c->config.local_record, c->body);
            if (r != UCN_OK) return r;
            c->tx_opcode = UCN_I_OPCODE_CAPABILITY_ADVERTISE; c->tx_body_bytes = UCN_I_CAPABILITY_RECORD_BYTES;
        }
        c->pending_control = 1U;
        c->cached_reply_opcode = c->tx_opcode;
        return UCN_OK;
reject_fragment:
        (void)ucn_i_security_replay_abort(c->config.security, &c->authenticated.replay);
        return UCN_ERR_SECURITY;
    }
    if (frame->opcode == UCN_I_OPCODE_CAPABILITY_QUERY) {
        if (c->rx_body_bytes != UCN_I_CAPABILITY_QUERY_BYTES ||
            ucn_i_capability_query_decode(c->plaintext, query) != UCN_OK ||
            query->requested_generation != c->config.local_record.capability_generation ||
            memcmp(query->known_digest, c->local_digest, 16U) != 0 ||
            ucn_i_capability_record_encode(&c->config.local_record, c->body) != UCN_OK) {
            (void)ucn_i_security_replay_abort(c->config.security, &c->authenticated.replay);
            return UCN_ERR_SECURITY;
        }
        r = ucn_i_security_control_replay_complete(c->config.security, &c->authenticated.replay, &c->facts);
        if (r == UCN_OK) r = ucn_i_security_control_replay_commit(c->config.security, &c->authenticated.replay, &c->facts);
        if (r != UCN_OK) return r;
        remember_request(c, rx, UCN_I_OPCODE_CAPABILITY_QUERY, UCN_I_OPCODE_CAPABILITY_ADVERTISE);
        c->tx_opcode = UCN_I_OPCODE_CAPABILITY_ADVERTISE; c->tx_body_bytes = UCN_I_CAPABILITY_RECORD_BYTES;
        c->pending_control = 1U; c->phase = 2U;
        return UCN_OK;
    }
    if (c->rx_body_bytes != UCN_I_CAPABILITY_SUMMARY_BYTES ||
        ucn_i_capability_summary_decode(c->plaintext, &c->summary) != UCN_OK) {
        (void)ucn_i_security_replay_abort(c->config.security, &c->authenticated.replay);
        return UCN_ERR_MALFORMED;
    }
    memset(&c->peer, 0, sizeof(c->peer));
    c->peer.runtime_instance = c->config.security->runtime_instance;
    c->peer.realm_id = c->config.security->realm_id; c->peer.address = c->facts.peer.address;
    c->peer.binding_generation = c->facts.peer.binding_generation;
    c->peer.session_generation = c->config.session.session_generation;
    c->peer.link_id = (uint32_t)c->config.link_slot + 1U; c->peer.link_generation = c->config.link_generation;
    c->peer.expires_at_us = c->session_view.expires_at_us;
    c->peer.security_owner_instance = c->config.security->owner_instance;
    memcpy(c->peer.principal, c->facts.peer.principal, 16U);
    r = ucn_i_capability_summary_ingest(c->config.capability_owner, &c->peer, &c->summary, now, &summary_result);
    if (r != UCN_OK || summary_result != UCN_I_CAPABILITY_SUMMARY_QUERY_REQUIRED) {
        (void)ucn_i_security_replay_abort(c->config.security, &c->authenticated.replay);
        return r != UCN_OK ? r : UCN_ERR_STATE;
    }
    memset(query, 0, sizeof(*query)); query->requested_generation = c->summary.capability_generation;
    memcpy(query->known_digest, c->summary.digest, 16U);
    r = ucn_i_capability_query_encode(query, c->body);
    if (r != UCN_OK) {
        (void)ucn_i_security_replay_abort(c->config.security, &c->authenticated.replay);
        return r;
    }
    r = ucn_i_security_control_replay_complete(c->config.security, &c->authenticated.replay, &c->facts);
    if (r == UCN_OK) r = ucn_i_security_control_replay_commit(c->config.security, &c->authenticated.replay, &c->facts);
    if (r != UCN_OK) return r;
    remember_request(c, rx, UCN_I_OPCODE_NEIGHBOR_HELLO, UCN_I_OPCODE_CAPABILITY_QUERY);
    c->tx_opcode = UCN_I_OPCODE_CAPABILITY_QUERY; c->tx_body_bytes = UCN_I_CAPABILITY_QUERY_BYTES;
    c->pending_control = 1U; c->phase = 2U;
    return UCN_OK;
}

ucn_result_t ucn_i_native_capability_drive_one(ucn_i_native_capability_t *c, uint64_t now)
{
    ucn_result_t r = current(c, now);
    if (r != UCN_OK) return r;
    if (c->tx_live) return UCN_OK;
    if (c->tx_ready) {
        if (c->attempts >= c->config.max_attempts) return UCN_ERR_EXHAUSTED;
        if (c->tx_count == 2U && c->tx_index == 0U &&
            (uint16_t)(c->config.max_attempts - c->attempts) < 2U) return UCN_ERR_EXHAUSTED;
        if (!c->tx_refreshed) {
            r = ucn_i_security_c0_refresh_hop(c->config.security, c->config.session, &c->facts,
                c->tx_frame[c->tx_index], c->tx_bytes[c->tx_index], &c->packet_work,
                c->tx_refresh, sizeof(c->tx_refresh), &c->tx_refresh_bytes);
            if (r != UCN_OK) return r;
            c->tx_refreshed = 1U;
        }
        r = ucn_i_adapter_tx_reserve(c->config.adapter, c->config.link_slot,
            c->config.link_generation, 0U, &c->tx_token);
        if (r != UCN_OK) return r;
        c->tx_live = 1U;
        r = enter_callback(c);
        if (r != UCN_OK) return r;
        ++c->attempts;
        r = ucn_i_adapter_tx_submit_ref(c->config.adapter, &c->tx_token,
            c->tx_refresh, c->tx_refresh_bytes, &c->tx_view);
        return leave_callback(c, r);
    }
    if (c->pending_control) return prepare_control(c);
    if (c->phase < 4U && c->tx_opcode != 0U && c->tx_count != 0U &&
        c->retry_at_us != 0U && now >= c->retry_at_us) {
        c->tx_index = 0U; c->tx_ready = 1U; c->tx_refreshed = 0U;
        return UCN_OK;
    }
    if (!c->config.device || c->phase != 0U) return UCN_OK;
    return prepare_hello(c);
}

ucn_result_t ucn_i_native_capability_retire_one(ucn_i_native_capability_t *c, uint64_t now)
{
    ucn_result_t r; bool busy; ucn_i_callback_claim_t claim;
    if (!c || c->magic != CAP_MAGIC || !now || now < c->now_us) return UCN_ERR_STATE;
    /* Retirement is not live admission: expired parents or a reopened Link
     * must not prevent the exact physical token from draining. */
    r = ucn_i_callback_gate_view(c->config.callback_gate, &busy, &claim);
    if (r != UCN_OK || busy) return UCN_ERR_STATE;
    c->now_us = now;
    if (!c->tx_live) return UCN_OK;
    r = ucn_i_adapter_tx_view(c->config.adapter, &c->tx_token, &c->tx_view);
    if (r != UCN_OK) return r;
    if (c->stopped && c->tx_view.state != UCN_I_ADAPTER_TX_COMPLETED &&
        c->tx_view.state != UCN_I_ADAPTER_TX_NOT_SUBMITTED && c->tx_view.state != UCN_I_ADAPTER_TX_CANCELLED) {
        r = enter_callback(c); if (r != UCN_OK) return r;
        r = ucn_i_adapter_tx_cancel(c->config.adapter, &c->tx_token);
        r = leave_callback(c, r); if (r != UCN_OK) return r;
        r = ucn_i_adapter_tx_view(c->config.adapter, &c->tx_token, &c->tx_view);
        if (r != UCN_OK) return r;
    }
    if (c->tx_view.state == UCN_I_ADAPTER_TX_IN_DOUBT) return UCN_ERR_IN_DOUBT;
    if (c->tx_view.state != UCN_I_ADAPTER_TX_COMPLETED && c->tx_view.state != UCN_I_ADAPTER_TX_NOT_SUBMITTED &&
        c->tx_view.state != UCN_I_ADAPTER_TX_CANCELLED) return UCN_OK;
    r = ucn_i_adapter_tx_retire(c->config.adapter, &c->tx_token);
    if (r != UCN_OK) return r;
    c->tx_live = 0U;
    if (c->stopped) return UCN_OK;
    if (c->tx_view.state == UCN_I_ADAPTER_TX_NOT_SUBMITTED) return UCN_OK;
    if (c->tx_view.state != UCN_I_ADAPTER_TX_COMPLETED || c->tx_view.terminal_result != UCN_OK) return UCN_ERR_STATE;
    c->tx_refreshed = 0U; ++c->tx_index;
    if (c->tx_index == c->tx_count) {
        c->tx_ready = 0U;
        if (c->requeue) { c->requeue = 0U; c->tx_index = 0U; c->tx_ready = 1U; }
        if (!c->config.device && c->tx_opcode == UCN_I_OPCODE_CAPABILITY_ACK && c->phase == 3U) {
            c->selected_ready = 1U; c->phase = 4U;
        }
        c->retry_at_us = UINT64_MAX - now < c->config.retry_interval_us ? UINT64_MAX : now + c->config.retry_interval_us;
    }
    return UCN_OK;
}

ucn_result_t ucn_i_native_capability_stop(ucn_i_native_capability_t *c, uint64_t now)
{
    bool busy; ucn_i_callback_claim_t claim; ucn_result_t r;
    if (!c || c->magic != CAP_MAGIC || !now || now < c->now_us) return UCN_ERR_STATE;
    r = ucn_i_callback_gate_view(c->config.callback_gate, &busy, &claim);
    if (r != UCN_OK || busy) return UCN_ERR_STATE;
    c->now_us = now; c->stopped = 1U; c->selected_ready = 0U;
    c->rx_bitmap = 0U; c->tx_ready = 0U;
    return UCN_OK;
}

ucn_result_t ucn_i_native_capability_quiescent(ucn_i_native_capability_t *c, bool *out)
{
    bool busy; ucn_i_callback_claim_t claim; ucn_result_t r;
    if (!c || c->magic != CAP_MAGIC || !out ||
        !output_disjoint(c, out, sizeof(*out))) return UCN_ERR_ARGUMENT;
    r = ucn_i_callback_gate_view(c->config.callback_gate, &busy, &claim);
    if (r != UCN_OK || busy) return UCN_ERR_STATE;
    *out = c->stopped && !c->tx_live && !c->tx_ready && !c->rx_bitmap;
    return UCN_OK;
}
