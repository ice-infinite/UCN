#include "internal/ucn_native_join.h"
#include "internal/ucn_checked.h"
#include <string.h>

#define JOIN_MAGIC UINT32_C(0x4A4F494E)
#define TRY(x) do { ucn_result_t r_ = (x); if (r_ != UCN_OK) return r_; } while (0)

static uint32_t broadcast(uint8_t width)
{ return width == 4U ? UINT32_MAX : (UINT32_C(1) << (width * 8U)) - 1U; }
static uint8_t role(uint16_t opcode)
{ return opcode == 1U || opcode == 3U || opcode == 4U || opcode == 6U ? 1U : 2U; }
static bool all_zero(const void *p, size_t n)
{ const uint8_t *b = p; size_t i; for (i = 0U; i < n; ++i) if (b[i] != 0U) return false; return true; }

/* Compare semantic facts, not the compiler's struct padding.
 * 比较语义字段，不把编译器填充字节纳入父域身份。 */
static bool authority_equal(const ucn_i_identity_authority_view_t *a,
    const ucn_i_identity_authority_view_t *b)
{
    return a->lease_sequence == b->lease_sequence &&
        a->local_deadline_us == b->local_deadline_us && a->record_generation == b->record_generation &&
        a->foundation_transaction_id == b->foundation_transaction_id && a->witness_generation == b->witness_generation &&
        a->runtime_instance == b->runtime_instance && a->realm_id == b->realm_id &&
        a->authority_generation == b->authority_generation &&
        a->authority_owner_instance == b->authority_owner_instance &&
        a->persistence_owner_instance == b->persistence_owner_instance &&
        a->schema_id == b->schema_id && a->schema_version == b->schema_version &&
        memcmp(a->principal, b->principal, sizeof(a->principal)) == 0 &&
        memcmp(a->body_digest, b->body_digest, sizeof(a->body_digest)) == 0;
}

static bool binding_equal(const ucn_i_identity_binding_view_t *a,
    const ucn_i_identity_binding_view_t *b)
{
    return a->runtime_instance == b->runtime_instance && a->realm_id == b->realm_id &&
        a->address == b->address && a->binding_generation == b->binding_generation &&
        a->authority_generation == b->authority_generation && a->link_generation == b->link_generation &&
        a->local_deadline_us == b->local_deadline_us && a->record_generation == b->record_generation &&
        a->foundation_transaction_id == b->foundation_transaction_id && a->witness_generation == b->witness_generation &&
        a->slot_generation == b->slot_generation && a->identity_owner_instance == b->identity_owner_instance &&
        a->persistence_owner_instance == b->persistence_owner_instance &&
        a->schema_id == b->schema_id && a->schema_version == b->schema_version &&
        memcmp(a->principal, b->principal, sizeof(a->principal)) == 0 &&
        memcmp(a->body_digest, b->body_digest, sizeof(a->body_digest)) == 0;
}

static void borrowed_range(const ucn_i_native_join_config_t *c, unsigned i,
    const void **p, size_t *bytes)
{
    *p = NULL; *bytes = 0U;
    switch (i) {
    case 0U: *p = c->adapter; *bytes = sizeof(*c->adapter); break;
    case 1U: *p = c->identity; *bytes = sizeof(*c->identity); break;
    case 2U: *p = c->security; *bytes = sizeof(*c->security); break;
    case 3U: *p = c->persistence; *bytes = sizeof(*c->persistence); break;
    case 4U: *p = c->admission; *bytes = c->admission ? sizeof(*c->admission) : 0U; break;
    case 5U: *p = c->callback_gate; *bytes = sizeof(*c->callback_gate); break;
    case 6U: if (c->identity) { *p = c->identity->domain_rules;
        *bytes = sizeof(*c->identity->domain_rules)*c->identity->domain_rule_count; } break;
    case 7U: if (c->security) { *p = c->security->domain_rules;
        *bytes = sizeof(*c->security->domain_rules)*c->security->domain_rule_count; } break;
    case 8U: if (c->security) { *p = c->security->acl_rules;
        *bytes = sizeof(*c->security->acl_rules)*c->security->acl_rule_count; } break;
    case 9U: if (c->persistence) { *p = c->persistence->callback_gate;
        *bytes = sizeof(*c->persistence->callback_gate); } break;
    default: break;
    }
}
static bool overlaps_borrowed(const ucn_i_native_join_config_t *c, const void *out, size_t size)
{
    unsigned i; const void *p; size_t bytes;
    for (i = 0U; i < 10U; ++i) {
        borrowed_range(c, i, &p, &bytes);
        if (bytes && ucn_i_ranges_overlap(out, size, p, bytes)) return true;
    }
    return false;
}
static bool owners_disjoint(const ucn_i_native_join_config_t *c)
{
    unsigned i, k; const void *left, *right; size_t a, b;
    for (i = 0U; i < 6U; ++i) {
        borrowed_range(c, i, &left, &a);
        for (k = i+1U; k < 6U; ++k) {
            borrowed_range(c, k, &right, &b);
            if (a && b && ucn_i_ranges_overlap(left, a, right, b)) return false;
        }
    }
    return true;
}
static bool output_valid(const ucn_i_native_join_t *j, const void *out, size_t bytes)
{
    return out != NULL && !ucn_i_ranges_overlap(j, sizeof(*j), out, bytes) &&
        !ucn_i_ranges_overlap(j->configuration_source, sizeof(j->config), out, bytes) &&
        !(j->session_acl && ucn_i_ranges_overlap(j->session_acl,
            sizeof(*j->session_acl)*j->session_acl_count, out, bytes)) &&
        !overlaps_borrowed(&j->config, out, bytes);
}

static ucn_result_t enter_product(ucn_i_native_join_t *j)
{
    if (j->operation_id == UINT32_MAX) return UCN_ERR_EXHAUSTED;
    j->claim.owner_instance = j->config.owner_instance;
    j->claim.operation_id = ++j->operation_id;
    j->claim.operation_generation = 1U; j->claim.operation_kind = 1U;
    return ucn_i_callback_gate_enter(j->config.callback_gate, &j->claim);
}
static ucn_result_t leave_product(ucn_i_native_join_t *j, ucn_result_t result)
{
    return ucn_i_callback_gate_leave(j->config.callback_gate, &j->claim) == UCN_OK
        ? result : UCN_ERR_STATE;
}

static ucn_result_t check_time_gate(ucn_i_native_join_t *j, uint64_t now)
{
    bool busy;
    ucn_i_callback_claim_t claim;
    if (j == NULL || j->magic != JOIN_MAGIC || now < j->now_us || now == 0U)
        return UCN_ERR_STATE;
    TRY(ucn_i_callback_gate_view(j->config.callback_gate, &busy, &claim));
    if (busy) return UCN_ERR_STATE;
    j->now_us = now; return UCN_OK;
}

ucn_result_t ucn_i_native_join_retirement_check(ucn_i_native_join_t *j, uint64_t now)
{ return check_time_gate(j, now); }

ucn_result_t ucn_i_native_join_configure_policy(ucn_i_native_join_t *j)
{
    ucn_result_t result;
    if (j->config.configure_session_policy == NULL) return UCN_OK;
    TRY(enter_product(j));
    result = j->config.configure_session_policy(j->config.product_context,
        &j->current.candidate, &j->session_acl, &j->session_acl_count);
    TRY(leave_product(j, result));
    return ucn_i_security_bind_session_acl(j->config.security, j->session,
        j->session_acl, j->session_acl_count);
}

ucn_result_t ucn_i_native_join_check(ucn_i_native_join_t *j, uint64_t now, bool network)
{
    uint32_t generation; uint16_t mtu; bool ready;
    TRY(check_time_gate(j, now));
    TRY(ucn_i_adapter_link_snapshot(j->config.adapter, j->config.link_index,
        &generation, &mtu, &ready));
    if (!ready || generation != j->config.link_generation) return UCN_ERR_STATE;
    if (network && (j->stopped || j->handed_off || j->failed)) return UCN_ERR_STATE;
    if (network && now >= j->deadline_us) return UCN_ERR_TIMEOUT;
    j->now_us = now;
    return UCN_OK;
}

ucn_result_t ucn_i_native_join_parents(ucn_i_native_join_t *j, uint64_t now, bool device_binding)
{
    ucn_i_identity_binding_view_t *a = j->config.device ? &j->current.peer : &j->current.local;
    ucn_i_identity_binding_view_t *d = j->config.device ? &j->current.local : &j->current.peer;
    ucn_result_t result;
    TRY(enter_product(j));
    result = j->config.current_authority(j->config.product_context, now, &j->current.authority);
    TRY(leave_product(j, result));
    if (j->current.authority.runtime_instance != j->config.identity->runtime_instance ||
        j->current.authority.realm_id != j->config.realm_id ||
        j->current.authority.authority_owner_instance != j->config.identity->authority_owner_instance ||
        j->current.authority.persistence_owner_instance != j->config.identity->persistence_owner_instance ||
        j->current.authority.schema_id != UCN_I_IDENTITY_AUTHORITY_SCHEMA_ID ||
        j->current.authority.schema_version != 1U || j->current.authority.record_generation == 0U ||
        j->current.authority.witness_generation != j->current.authority.record_generation ||
        j->current.authority.foundation_transaction_id == 0U ||
        j->current.authority.authority_generation == 0U || j->current.authority.lease_sequence == 0U ||
        all_zero(j->current.authority.body_digest, 16U) ||
        j->current.authority.local_deadline_us <= now ||
        memcmp(j->current.authority.principal, j->config.authority_principal, 16U) != 0)
        return UCN_ERR_SECURITY;
    if (j->frozen_authority.record_generation != 0U &&
        !authority_equal(&j->frozen_authority, &j->current.authority))
        return UCN_ERR_SECURITY;
    TRY(ucn_i_identity_binding_get(j->config.identity, j->config.authority_binding, now, a));
    if (a->realm_id != j->config.realm_id || a->link_generation != j->config.link_generation ||
        memcmp(a->principal, j->config.authority_principal, 16U) != 0)
        return UCN_ERR_SECURITY;
    if (j->frozen_authority_binding.record_generation != 0U &&
        !binding_equal(&j->frozen_authority_binding, a)) return UCN_ERR_SECURITY;
    if (j->phase >= 2U && (j->transcript.authority_address != a->address ||
        j->transcript.authority_binding_generation != a->binding_generation ||
        j->transcript.authority_generation != j->current.authority.authority_generation ||
        j->transcript.authority_lease_sequence != j->current.authority.lease_sequence))
        return UCN_ERR_SECURITY;
    if (device_binding) {
        if (!j->binding_ready) return UCN_ERR_STATE;
        TRY(ucn_i_identity_binding_get(j->config.identity, j->device_binding, now, d));
        if (d->realm_id != j->config.realm_id || d->address != j->config.device_address ||
            d->binding_generation != j->config.device_binding_generation ||
            d->link_generation != j->config.link_generation ||
            d->authority_generation != j->current.authority.authority_generation ||
            memcmp(d->principal, j->config.device_principal, 16U) != 0)
            return UCN_ERR_SECURITY;
        if (j->frozen_device_binding.record_generation != 0U &&
            !binding_equal(&j->frozen_device_binding, d)) return UCN_ERR_SECURITY;
    }
    return UCN_OK;
}

ucn_result_t ucn_i_native_join_init(ucn_i_native_join_t *j,
    const ucn_i_native_join_config_t *c, uint64_t now)
{
    uint32_t generation; uint16_t mtu; bool ready;
    ucn_result_t result;
    if (j == NULL || c == NULL ||
        ucn_i_ranges_overlap(j, sizeof(*j), c, sizeof(*c))) return UCN_ERR_ARGUMENT;
    if (UCN_PERSIST_DOMAIN_COUNT < 3U) return UCN_ERR_UNSUPPORTED;
    if (!owners_disjoint(c) || overlaps_borrowed(c, j, sizeof(*j)) || overlaps_borrowed(c, c, sizeof(*c)))
        return UCN_ERR_ARGUMENT;
    if (!all_zero(j, sizeof(*j))) return UCN_ERR_STATE;
    if (c->adapter == NULL || c->identity == NULL || c->security == NULL ||
        c->persistence == NULL || c->callback_gate == NULL ||
        c->current_authority == NULL || c->sign_event == NULL ||
        c->authentication.authorize_event == NULL ||
        (!c->device && c->admission == NULL) || c->device > 1U ||
        c->security->provider_gate != c->callback_gate ||
        (!c->device && c->admission->provider_gate != c->callback_gate) ||
        c->owner_instance == 0U || c->realm_id == 0U ||
        c->realm_id != c->identity->realm_id || c->realm_id != c->security->realm_id ||
        c->address_width == 0U || c->address_width > 4U ||
        c->policy_generation == 0U || c->physical_peer == 0U ||
        c->transaction == 0U || c->device_nonce == 0U || c->freshness_nonce == 0U ||
        c->binding_domain_id == 0U || c->session_domain_id == 0U ||
        c->device_address == 0U || c->device_address >= broadcast(c->address_width) ||
        c->device_binding_generation == 0U || c->link_generation == 0U ||
        c->timeout_us == 0U || c->session_lifetime_us == 0U ||
        c->retry_interval_us == 0U || c->cookie_bucket_us == 0U || c->max_attempts < 40U ||
        now == 0U || UINT64_MAX - now < c->timeout_us ||
        UINT64_MAX - now < c->session_lifetime_us ||
        all_zero(c->identity_digest, 16U) || all_zero(c->device_principal, 16U) ||
        all_zero(c->authority_principal, 16U) ||
        memcmp(c->device_principal, c->authority_principal, 16U) == 0)
        return UCN_ERR_CONFIG;
    TRY(ucn_i_adapter_link_snapshot(c->adapter, c->link_index, &generation, &mtu, &ready));
    if (!ready || generation != c->link_generation ||
        mtu < 123U + 2U * c->address_width || UCN_ADAPTER_FRAME_BYTES < mtu)
        return UCN_ERR_CONFIG;
    j->config = *c; j->configuration_source = c; j->started_us = now; j->now_us = now;
    j->deadline_us = now + c->timeout_us; j->magic = JOIN_MAGIC;
    j->cookie_verify_refill_us = now;
    j->cookie_verify_tokens = c->device ? 0U : c->admission->token_burst;
    j->hello.transaction_id = c->transaction; j->hello.device_nonce = c->device_nonce;
    memcpy(j->hello.identity_digest, c->identity_digest, 16U);
    result = ucn_i_native_join_parents(j, now, false);
    if (result != UCN_OK) { memset(j, 0, sizeof(*j)); return result; }
    j->frozen_authority = j->current.authority;
    j->frozen_authority_binding = c->device ? j->current.peer : j->current.local;
    return UCN_OK;
}

static ucn_result_t encode_frame(ucn_i_native_join_t *j, uint16_t op,
    const uint8_t *payload, size_t bytes, uint8_t *out, size_t capacity, size_t *written)
{
    ucn_i_c0_frame_t *f = &j->scratch.wire.frame;
    memset(f, 0, sizeof(*f)); f->payload = payload; f->payload_bytes = bytes;
    f->transaction_id = j->config.transaction; f->realm_id = j->config.realm_id;
    f->destination_address = broadcast(j->config.address_width);
    f->opcode = op; f->interaction_role = role(op); f->payload_kind = UCN_I_PAYLOAD_CONTROL;
    f->hop_limit = 1U;
    return ucn_i_c0_encode(f, j->config.address_width, 0U, out, capacity, written);
}

static ucn_result_t cookie_hash(ucn_i_native_join_t *j)
{
    static const uint8_t domain[] = "UCN6-BOOTSTRAP-COOKIE-V1";
    size_t a, b, offset = sizeof(domain) - 1U + 4U;
    uint8_t *input = j->scratch.wire.hash_input;
    memcpy(input, domain, sizeof(domain) - 1U);
    TRY(ucn_i_admission_hello_encode(&j->hello, j->scratch.wire.payload));
    TRY(encode_frame(j, 1U, j->scratch.wire.payload, 40U, input + offset,
        sizeof(j->scratch.wire.hash_input) - offset, &a));
    TRY(ucn_i_admission_cookie_challenge_encode(&j->challenge, j->scratch.wire.payload));
    TRY(encode_frame(j, 2U, j->scratch.wire.payload, 40U, input + offset + a,
        sizeof(j->scratch.wire.hash_input) - offset - a, &b));
    input[sizeof(domain)-1U] = 0U; input[sizeof(domain)] = 0U;
    input[sizeof(domain)+1U] = 0U; input[sizeof(domain)+2U] = (uint8_t)(a+b);
    return ucn_i_sha256_256(input, offset+a+b, j->echo.prior_messages_hash, &j->scratch.wire.hash);
}

static void initial_transcript(ucn_i_native_join_t *j)
{
    memset(&j->transcript, 0, sizeof(j->transcript));
    j->transcript.protocol_version = 6U; j->transcript.bootstrap_header_contract = 1U;
    j->transcript.transaction_id = j->config.transaction;
    j->transcript.device_nonce = j->config.device_nonce;
    j->transcript.lease_freshness_challenge_nonce = j->config.freshness_nonce;
    j->transcript.selected_link_id = j->echo.key.link.id;
    j->transcript.selected_link_generation = j->echo.key.link.generation;
    memcpy(j->transcript.joining_device_identity_digest, j->config.identity_digest, 16U);
    memcpy(j->transcript.prior_messages_hash, j->echo.prior_messages_hash, 32U);
}

static ucn_result_t prepare_small(ucn_i_native_join_t *j, uint8_t op)
{
    size_t bytes = 40U;
    if (j->tx_live) return UCN_ERR_STATE;
    if (op == 1U) TRY(ucn_i_admission_hello_encode(&j->hello, j->tx_logical));
    else if (op == 2U) TRY(ucn_i_admission_cookie_challenge_encode(&j->challenge, j->tx_logical));
    else TRY(ucn_i_admission_hello_cookie_encode(&j->echo, j->tx_logical, sizeof(j->tx_logical), &bytes));
    j->tx_opcode = op; j->tx_total = (uint16_t)bytes; j->tx_count = 1U;
    j->tx_index = 0U; j->tx_ready = 1U; j->tx_round_complete = 0U;
    return UCN_OK;
}

static ucn_result_t validate_event(ucn_i_native_join_t *j, uint8_t event)
{
    if (!ucn_i_admission_transition_valid(&j->transcript, &j->proposed, event) ||
        j->proposed.realm_id != j->config.realm_id ||
        memcmp(j->proposed.authority_principal, j->config.authority_principal, 16U) != 0 ||
        (event >= 2U && memcmp(j->proposed.joining_device_principal,
            j->config.device_principal, 16U) != 0) ||
        (event >= 3U && (j->proposed.proposed_address != j->config.device_address ||
            j->proposed.address_binding_generation != j->config.device_binding_generation)))
        return UCN_ERR_SECURITY;
    return UCN_OK;
}

static ucn_result_t verify_device_event(ucn_i_native_join_t *j, uint8_t event)
{
    ucn_result_t result;
    TRY(enter_product(j));
    result = j->config.authentication.authorize_event(j->config.authentication.context,
        event, &j->echo.key, &j->proposed, j->now_us, &j->proposed_evidence);
    return leave_product(j, result);
}

static ucn_result_t accept_event(ucn_i_native_join_t *j, uint8_t event)
{
    TRY(validate_event(j, event));
    if (j->config.device) TRY(verify_device_event(j, event));
    else if (event == 5U) {
        TRY(ucn_i_native_join_parents(j, j->now_us, true));
        TRY(ucn_i_admission_finalize(j->config.admission, j->admission_handle,
            &j->proposed, &j->proposed_evidence, &j->current.peer, j->now_us, &j->admitted));
    } else TRY(ucn_i_admission_advance(j->config.admission, j->admission_handle,
        event, &j->proposed, &j->proposed_evidence, j->now_us));
    j->transcript = j->proposed; j->evidence = j->proposed_evidence;
    j->phase = (uint8_t)(event + 1U);
    return UCN_OK;
}

static ucn_result_t sign_next(ucn_i_native_join_t *j, uint8_t event)
{
    ucn_result_t result;
    TRY(enter_product(j));
    result = j->config.sign_event(j->config.product_context, event, &j->transcript,
        &j->proposed, &j->proposed_evidence);
    TRY(leave_product(j, result));
    TRY(validate_event(j, event));
    /* Encoding preflight before any irreversible Admission publication. */
    {
        size_t bytes;
        TRY(ucn_i_admission_logical_encode((uint16_t)(event+3U), (uint8_t)(event+1U),
            &j->proposed, &j->proposed_evidence, j->tx_logical, sizeof(j->tx_logical), &bytes));
        j->tx_total = (uint16_t)bytes;
    }
    if (event == 3U && !j->binding_ready) {
        /* Authenticate Offer before persisting its allocation. Admission itself
         * cannot advance to Offered before the actual local reload. */
        TRY(verify_device_event(j, event));
        return ucn_i_native_join_persist_binding(j, j->now_us);
    }
    TRY(accept_event(j, event));
    j->tx_opcode = (uint8_t)(event+3U); j->tx_count = (uint8_t)((j->tx_total+61U)/62U);
    j->tx_index = 0U; j->tx_ready = 1U; j->tx_round_complete = 0U;
    return UCN_OK;
}

static ucn_result_t submit_one(ucn_i_native_join_t *j)
{
    const uint8_t *payload = j->tx_logical; size_t bytes = j->tx_total;
    ucn_result_t result;
    if (j->attempts >= j->config.max_attempts) return UCN_ERR_EXHAUSTED;
    if (j->tx_opcode >= 4U) {
        ucn_i_admission_fragment_t *f = &j->scratch.wire.fragment;
        memset(f, 0, sizeof(*f)); f->transaction_id = j->config.transaction;
        memcpy(f->identity_digest, j->config.identity_digest, 16U);
        f->phase = (uint8_t)(j->tx_opcode-2U); f->chunk_bytes = 62U;
        f->fragment_count = j->tx_count; f->fragment_index = j->tx_index;
        f->total_length = j->tx_total; f->fragment_offset = (uint16_t)(j->tx_index*62U);
        f->fragment_length = (uint16_t)(j->tx_total-f->fragment_offset);
        if (f->fragment_length > 62U) f->fragment_length = 62U;
        memcpy(f->data, j->tx_logical+f->fragment_offset, f->fragment_length);
        TRY(ucn_i_admission_fragment_encode(f, j->scratch.wire.payload,
            sizeof(j->scratch.wire.payload), &bytes));
        payload = j->scratch.wire.payload;
    }
    TRY(encode_frame(j, j->tx_opcode, payload, bytes, j->tx_frame,
        sizeof(j->tx_frame), &j->tx_frame_bytes));
    TRY(ucn_i_adapter_tx_reserve(j->config.adapter, j->config.link_index,
        j->config.link_generation, 0U, &j->tx_token));
    j->tx_live = 1U; ++j->attempts;
    TRY(enter_product(j));
    result = ucn_i_adapter_tx_submit_ref(j->config.adapter, &j->tx_token,
        j->tx_frame, j->tx_frame_bytes, &j->tx_view);
    result = leave_product(j, result);
    if (result != UCN_OK) return result; /* The exact holder remains owned. */
    if (j->tx_opcode == 1U && (j->tx_view.state == UCN_I_ADAPTER_TX_SUBMITTED ||
        j->tx_view.state == UCN_I_ADAPTER_TX_COMPLETED)) j->hello_submitted = 1U;
    return UCN_OK;
}

static ucn_result_t retire_admission(ucn_i_native_join_t *j, uint64_t now)
{
    uint16_t inspected, expired;
    if (j->config.device || j->admission_handle.slot_generation == 0U ||
        j->work_kind || (!j->active && !j->stopped)) return UCN_OK;
    if (j->stopped && now >= j->deadline_us)
        TRY(ucn_i_admission_expire(j->config.admission, now, 1U, &inspected, &expired));
    TRY(ucn_i_admission_pending_get(j->config.admission, j->admission_handle, &j->pending));
    if (j->pending.phase != UCN_I_ADMISSION_FINAL_DURABLE &&
        j->pending.phase != UCN_I_ADMISSION_ABORTED) return UCN_OK;
    TRY(ucn_i_admission_retire(j->config.admission, j->admission_handle));
    memset(&j->admission_handle, 0, sizeof(j->admission_handle));
    return UCN_OK;
}

ucn_result_t ucn_i_native_join_retire_one(ucn_i_native_join_t *j, uint64_t now)
{
    TRY(check_time_gate(j, now));
    if (j->handed_off) return UCN_ERR_STATE;
    if (!j->tx_live) return retire_admission(j, now);
    TRY(ucn_i_adapter_tx_view(j->config.adapter, &j->tx_token, &j->tx_view));
    if (j->stopped && j->tx_view.state != UCN_I_ADAPTER_TX_COMPLETED &&
        j->tx_view.state != UCN_I_ADAPTER_TX_NOT_SUBMITTED &&
        j->tx_view.state != UCN_I_ADAPTER_TX_CANCELLED) {
        ucn_result_t result;
        TRY(enter_product(j));
        result = ucn_i_adapter_tx_cancel(j->config.adapter, &j->tx_token);
        TRY(leave_product(j, result));
        TRY(ucn_i_adapter_tx_view(j->config.adapter, &j->tx_token, &j->tx_view));
    }
    if (j->tx_view.state == UCN_I_ADAPTER_TX_IN_DOUBT) return UCN_ERR_IN_DOUBT;
    if (j->tx_view.state != UCN_I_ADAPTER_TX_COMPLETED &&
        j->tx_view.state != UCN_I_ADAPTER_TX_NOT_SUBMITTED &&
        j->tx_view.state != UCN_I_ADAPTER_TX_CANCELLED) return UCN_OK;
    TRY(ucn_i_adapter_tx_retire(j->config.adapter, &j->tx_token));
    j->tx_live = 0U;
    if (j->tx_view.state == UCN_I_ADAPTER_TX_COMPLETED && j->tx_view.terminal_result == UCN_OK) {
        ++j->tx_index;
        if (j->tx_index == j->tx_count) {
            j->tx_ready = 0U; j->tx_round_complete = 1U;
            if (j->tx_opcode == 3U) j->echo_complete = 1U;
            j->retry_at_us = UINT64_MAX-now < j->config.retry_interval_us ? UINT64_MAX :
                now+j->config.retry_interval_us;
        }
    }
    if (j->requeue && !j->tx_ready) {
        j->requeue = 0U; j->tx_index = 0U; j->tx_ready = 1U;
    }
    return UCN_OK;
}

ucn_result_t ucn_i_native_join_drive_one(ucn_i_native_join_t *j, uint64_t now)
{
    TRY(ucn_i_native_join_check(j, now, true));
    if (j->tx_live || j->work_kind) return UCN_OK;
    if (j->tx_ready) return submit_one(j);
    if (j->config.device && j->tx_opcode == 0U) {
        TRY(prepare_small(j, 1U)); return submit_one(j);
    }
    if (j->config.device && j->phase == 1U && j->tx_opcode == 1U) {
        TRY(prepare_small(j, 3U)); return UCN_OK;
    }
    if ((!j->config.device && j->phase == 1U) ||
        (j->config.device && j->phase == 2U && j->echo_complete) ||
        (!j->config.device && j->phase == 3U && !j->binding_ready) ||
        (j->config.device && j->phase == 4U && j->binding_ready) ||
        (!j->config.device && j->phase == 5U))
        return sign_next(j, j->phase);
    if (!j->config.device && j->phase == 3U && j->binding_ready) {
        TRY(accept_event(j, 3U));
        j->tx_opcode = 6U; j->tx_count = (uint8_t)((j->tx_total+61U)/62U);
        j->tx_index = 0U; j->tx_ready = 1U; j->tx_round_complete = 0U;
        return UCN_OK;
    }
    if (j->config.device && j->phase == 4U && !j->binding_ready)
        return ucn_i_native_join_persist_binding(j, now);
    if (j->phase == 6U && !j->active &&
        (j->config.device || (j->tx_opcode == 8U && j->tx_round_complete)))
        return ucn_i_native_join_persist_session(j, now);
    if (j->tx_opcode != 0U && now >= j->retry_at_us && j->tx_round_complete &&
        (j->tx_opcode < 4U || j->phase == (uint8_t)(j->tx_opcode-2U))) {
        j->tx_index = 0U; j->tx_ready = 1U; return UCN_OK;
    }
    return UCN_OK;
}

static ucn_result_t receive_cookie(ucn_i_native_join_t *j, const ucn_i_c0_frame_t *f)
{
    if (f->opcode == 1U && !j->config.device && f->payload_bytes == 40U) {
        ucn_i_admission_hello_t hello;
        TRY(ucn_i_admission_hello_decode(f->payload, &hello));
        if (memcmp(&hello, &j->hello, sizeof(hello)) != 0) return UCN_ERR_SECURITY;
        if (j->phase != 0U) return UCN_ERR_STATE;
        if (j->challenge.cookie_bytes == 0U) {
            ucn_i_admission_link_t link = {j->config.link_generation,
                (uint16_t)(j->config.link_index+1U), 0U};
            uint64_t bucket = j->now_us/j->config.cookie_bucket_us+1U;
            if (bucket > UINT32_MAX) return UCN_ERR_EXHAUSTED;
            TRY(ucn_i_admission_issue_cookie(j->config.admission, &j->hello, link,
                j->now_us, 40U, 40U, (uint32_t)bucket, &j->challenge));
        }
        if (j->tx_live) { j->requeue = 1U; return UCN_OK; }
        return prepare_small(j, 2U);
    }
    if (f->opcode == 2U && j->config.device && f->payload_bytes == 40U) {
        ucn_i_admission_cookie_challenge_t c;
        if (!j->hello_submitted || j->phase > 1U) return UCN_ERR_STATE;
        TRY(ucn_i_admission_cookie_challenge_decode(f->payload, &c));
        if (c.transaction_id != j->config.transaction) return UCN_ERR_SECURITY;
        if (j->phase == 1U) return memcmp(&c, &j->challenge, sizeof(c)) == 0 ? UCN_OK : UCN_ERR_REPLAY;
        j->challenge = c;
        memset(&j->echo, 0, sizeof(j->echo)); j->echo.key.link = c.issuer_link;
        j->echo.key.local_peer_discriminator = j->config.physical_peer;
        j->echo.key.transaction_id = j->config.transaction;
        memcpy(j->echo.key.identity_digest, j->config.identity_digest, 16U);
        j->echo.device_nonce = j->config.device_nonce;
        j->echo.lease_freshness_challenge_nonce = j->config.freshness_nonce;
        j->echo.cookie_bytes = c.cookie_bytes; memcpy(j->echo.cookie, c.cookie, 16U);
        TRY(cookie_hash(j)); initial_transcript(j); j->phase = 1U; return UCN_OK;
    }
    if (f->opcode == 3U && !j->config.device) {
        uint64_t elapsed;
        if (j->phase != 0U) return UCN_ERR_STATE;
        TRY(ucn_i_admission_hello_cookie_decode(f->payload, f->payload_bytes,
            j->config.physical_peer, &j->echo));
        if (j->echo.key.link.id != j->config.link_index+1U ||
            j->echo.key.link.generation != j->config.link_generation ||
            j->echo.key.transaction_id != j->config.transaction ||
            j->echo.device_nonce != j->config.device_nonce ||
            j->echo.lease_freshness_challenge_nonce != j->config.freshness_nonce ||
            memcmp(j->echo.key.identity_digest, j->config.identity_digest, 16U) != 0)
            return UCN_ERR_SECURITY;
        elapsed = (j->now_us-j->cookie_verify_refill_us)/UINT64_C(1000000);
        if (elapsed != 0U) {
            uint64_t room = j->config.admission->token_burst-j->cookie_verify_tokens;
            if (elapsed >= room || elapsed*j->config.admission->tokens_per_second >= room)
                j->cookie_verify_tokens = j->config.admission->token_burst;
            else j->cookie_verify_tokens = (uint8_t)(j->cookie_verify_tokens+
                elapsed*j->config.admission->tokens_per_second);
            j->cookie_verify_refill_us += elapsed*UINT64_C(1000000);
        }
        if (j->cookie_verify_tokens == 0U) return UCN_ERR_EXHAUSTED;
        --j->cookie_verify_tokens;
        initial_transcript(j);
        TRY(ucn_i_admission_open_after_cookie_started(j->config.admission, &j->echo,
            &j->transcript, j->started_us, j->now_us, &j->admission_handle));
        j->phase = 1U; return UCN_OK;
    }
    return UCN_ERR_MALFORMED;
}

ucn_result_t ucn_i_native_join_receive_claimed(ucn_i_native_join_t *j,
    const ucn_i_adapter_rx_view_t *rx, uint64_t now)
{
    ucn_i_c0_frame_t *f; ucn_i_admission_fragment_t *p;
    uint16_t bit; uint8_t event;
    TRY(ucn_i_native_join_check(j, now, true));
    if (rx == NULL || rx->link_slot != j->config.link_index ||
        rx->link_instance_generation != j->config.link_generation ||
        rx->meta.sender_discriminator != j->config.physical_peer ||
        rx->meta.timestamp_us < j->started_us || rx->meta.timestamp_us > now)
        return UCN_ERR_SECURITY;
    f = &j->scratch.wire.frame;
    TRY(ucn_i_c0_decode(rx->frame, rx->frame_bytes, j->config.address_width, 0U, f));
    if (f->realm_id != j->config.realm_id || f->transaction_id != j->config.transaction ||
        f->source_address != 0U || f->destination_address != broadcast(j->config.address_width) ||
        f->source_binding_generation != 0U || f->destination_binding_generation != 0U ||
        f->origin_security != 0U || f->payload_kind != UCN_I_PAYLOAD_CONTROL ||
        f->hop_limit != 1U || f->traffic_class != 0U || f->delivery_guarantee != 0U ||
        f->opcode == 0U || f->opcode > 8U || f->interaction_role != role(f->opcode))
        return UCN_ERR_MALFORMED;
    if (f->opcode <= 3U) return receive_cookie(j, f);
    event = (uint8_t)(f->opcode-3U);
    if ((j->config.device && event != 1U && event != 3U && event != 5U) ||
        (!j->config.device && event != 2U && event != 4U)) return UCN_ERR_STATE;
    p = &j->scratch.wire.fragment;
    TRY(ucn_i_admission_fragment_decode(f->payload, f->payload_bytes, p));
    if (p->phase != event+1U || p->transaction_id != j->config.transaction ||
        p->chunk_bytes != 62U || memcmp(p->identity_digest, j->config.identity_digest, 16U) != 0)
        return UCN_ERR_MALFORMED;
    if (j->rx_phase != p->phase) {
        if (j->phase != event || p->fragment_index != 0U) return UCN_ERR_STATE;
        if (event == 1U && !j->echo_complete) return UCN_ERR_STATE;
        if (event > 1U && !j->tx_round_complete &&
            !(j->tx_live && j->tx_index+1U == j->tx_count &&
              (j->tx_view.state == UCN_I_ADAPTER_TX_SUBMITTED || j->tx_view.state == UCN_I_ADAPTER_TX_COMPLETED)))
            return UCN_ERR_STATE;
        j->rx_phase = p->phase; j->rx_total = p->total_length;
        j->rx_count = p->fragment_count; j->rx_bitmap = 0U; j->rx_chunk = p->chunk_bytes;
    }
    if (p->total_length != j->rx_total || p->fragment_count != j->rx_count) return UCN_ERR_MALFORMED;
    bit = (uint16_t)(1U << p->fragment_index);
    if ((j->rx_bitmap & bit) != 0U) {
        if (memcmp(j->rx_logical+p->fragment_offset, p->data, p->fragment_length) != 0)
            return UCN_ERR_REPLAY;
        return UCN_OK;
    }
    memcpy(j->rx_logical+p->fragment_offset, p->data, p->fragment_length); j->rx_bitmap |= bit;
    if (j->rx_bitmap != (uint16_t)((1U << j->rx_count)-1U)) return UCN_OK;
    {
        ucn_result_t result = ucn_i_admission_logical_decode((uint16_t)(event+3U), (uint8_t)(event+1U),
            j->rx_logical, j->rx_total, &j->proposed, &j->proposed_evidence, &j->scratch.wire.decode);
        if (result == UCN_OK) result = accept_event(j, event);
        if (result != UCN_OK) { j->rx_phase = 0U; j->rx_bitmap = 0U; }
        return result;
    }
}

ucn_result_t ucn_i_native_join_session_current(ucn_i_native_join_t *j, uint64_t now,
    ucn_i_native_join_current_t *out)
{
    if (j == NULL || j->magic != JOIN_MAGIC || !output_valid(j, out, sizeof(*out))) return UCN_ERR_ARGUMENT;
    TRY(ucn_i_native_join_check(j, now, false));
    if (j->stopped || !j->active || j->work_kind) return UCN_ERR_STATE;
    TRY(ucn_i_native_join_parents(j, now, true));
    j->facts.local = j->current.candidate.local; j->facts.peer = j->current.candidate.peer;
    j->facts.link_generation = j->config.link_generation;
    j->facts.policy_generation = j->config.policy_generation; j->facts.now_us = now;
    TRY(ucn_i_security_c1_context_check(j->config.security, &j->session, &j->facts,
        j->current.candidate.origin_fingerprint, j->current.candidate.hop_fingerprint));
    j->current.session = j->session; *out = j->current; return UCN_OK;
}

ucn_result_t ucn_i_native_join_bindings_current(ucn_i_native_join_t *j, uint64_t now,
    ucn_i_native_join_current_t *out)
{
    if (j == NULL || j->magic != JOIN_MAGIC || !output_valid(j, out, sizeof(*out))) return UCN_ERR_ARGUMENT;
    TRY(ucn_i_native_join_check(j, now, false));
    if (j->stopped || !j->binding_ready) return UCN_ERR_STATE;
    TRY(ucn_i_native_join_parents(j, now, true));
    *out = j->current;
    memset(&out->session, 0, sizeof(out->session));
    memset(&out->candidate, 0, sizeof(out->candidate));
    return UCN_OK;
}

ucn_result_t ucn_i_native_join_handoff(ucn_i_native_join_t *j, uint64_t now,
    ucn_i_native_join_current_t *out)
{
    bool quiet;
    if (j == NULL || j->magic != JOIN_MAGIC || !output_valid(j, out, sizeof(*out))) return UCN_ERR_ARGUMENT;
    TRY(ucn_i_native_join_check(j, now, false));
    if (j->handed_off || j->tx_live || j->work_kind) return UCN_ERR_STATE;
    TRY(ucn_i_adapter_obligations_drained(j->config.adapter, &quiet));
    if (!quiet) return UCN_ERR_STATE;
    TRY(retire_admission(j, now));
    if (j->admission_handle.slot_generation != 0U) return UCN_ERR_STATE;
    TRY(ucn_i_native_join_session_current(j, now, out));
    j->handed_off = 1U; j->tx_ready = 0U; return UCN_OK;
}

ucn_result_t ucn_i_native_join_stop(ucn_i_native_join_t *j, uint64_t now)
{
    TRY(check_time_gate(j, now));
    if (j->handed_off) return UCN_ERR_STATE;
    j->stopped = 1U; j->tx_ready = 0U; j->requeue = 0U;
    if (j->session.session_generation != 0U)
        TRY(ucn_i_security_fence(j->config.security, j->session));
    return UCN_OK;
}

ucn_result_t ucn_i_native_join_quiescent(ucn_i_native_join_t *j, bool *out)
{
    if (j == NULL || j->magic != JOIN_MAGIC) return UCN_ERR_ARGUMENT;
    if (!output_valid(j, out, sizeof(*out))) return UCN_ERR_ARGUMENT;
    TRY(check_time_gate(j, j->now_us));
    *out = j->stopped && !j->tx_live && !j->work_kind &&
        j->admission_handle.slot_generation == 0U;
    return UCN_OK;
}
