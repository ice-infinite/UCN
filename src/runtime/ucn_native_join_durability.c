#include "internal/ucn_native_join.h"
#include <string.h>

#define TRY(x) do { ucn_result_t r_ = (x); if (r_ != UCN_OK) return r_; } while (0)

static ucn_result_t empty_base(ucn_i_native_join_t *j, uint16_t kind, uint64_t id)
{
    ucn_persist_domain_key_t key = {0};
    key.domain_kind = kind; key.domain_id = id;
    TRY(ucn_i_persistence_domain_get(j->config.persistence, key, &j->scratch.durability.domain));
    /* First JOIN cannot erase recovered generation/history. Reauth owns that path. */
    if (j->scratch.durability.domain.state != UCN_PERSIST_DOMAIN_READY ||
        j->scratch.durability.domain.pending || j->scratch.durability.domain.record_generation != 0U ||
        j->scratch.durability.domain.body_bytes != 0U) return UCN_ERR_STATE;
    return UCN_OK;
}

static void request_common(ucn_i_native_join_t *j, uint16_t kind, uint64_t domain)
{
    ucn_persistence_request_t *r = &j->scratch.durability.request;
    memset(r, 0, sizeof(*r)); r->struct_size = sizeof(*r); r->api_version = UCN_PERSIST_API_VERSION;
    r->domain.domain_kind = kind; r->domain.domain_id = domain;
}

static ucn_result_t submit_request(ucn_i_native_join_t *j, uint64_t now)
{
    const ucn_persistence_request_t *r = &j->scratch.durability.request;
    ucn_i_persist_record_meta_t *m = &j->scratch.durability.meta;
    memset(m, 0, sizeof(*m)); m->domain = r->domain;
    m->record_generation = r->expected_record_generation+1U;
    m->transaction_id = r->foundation_transaction_id; m->body_bytes = r->body_bytes;
    m->schema_id = r->schema_id; m->schema_version = r->schema_version;
    m->operation_kind = r->operation_kind;
    TRY(ucn_i_persist_body_digest(m, r->canonical_body,
        j->scratch.durability.expected_digest, &j->scratch.durability.codec));
    return ucn_i_persistence_submit(j->config.persistence, r, now, &j->persistence_handle);
}

ucn_result_t ucn_i_native_join_persist_binding(ucn_i_native_join_t *j, uint64_t now)
{
    ucn_i_identity_binding_issue_t *i;
    ucn_i_identity_durability_base_t *b;
    ucn_i_identity_requirement_view_t *q;
    ucn_persistence_request_t *r;
    ucn_result_t result;
    if (j->work_kind || j->binding_ready ||
        (j->config.device ? j->phase != 4U : j->phase != 3U)) return UCN_ERR_STATE;
    TRY(ucn_i_native_join_parents(j, now, false));
    /* The proposal is the authenticated Offer on both sides, never caller DTO. */
    if (!ucn_i_admission_transcript_valid(&j->proposed, 4U)) return UCN_ERR_SECURITY;
    TRY(empty_base(j, UCN_PERSIST_DOMAIN_IDENTITY_BINDING, j->config.binding_domain_id));
    i = &j->scratch.durability.issue; b = &j->scratch.durability.binding_base;
    memset(i, 0, sizeof(*i)); memset(b, 0, sizeof(*b));
    i->transaction_id = j->config.transaction; i->challenge_started_local_us = j->started_us;
    i->challenge_deadline_us = j->deadline_us;
    i->lease_duration_us = j->proposed.binding_lease_duration_us;
    i->authority_lease_sequence = j->proposed.authority_lease_sequence;
    i->runtime_instance = j->config.identity->runtime_instance; i->realm_id = j->config.realm_id;
    i->address = j->config.device_address; i->binding_generation = j->config.device_binding_generation;
    i->authority_generation = j->proposed.authority_generation;
    i->link_generation = j->config.link_generation;
    i->admission_generation = j->config.device ? 1U : j->admission_handle.slot_generation;
    i->admission_owner_instance = j->config.device ? j->config.owner_instance : j->admission_handle.owner_instance;
    i->address_width = j->config.address_width; i->mode = j->proposed.binding_mode;
    memcpy(i->principal, j->config.device_principal, 16U);
    memcpy(i->authority_principal, j->config.authority_principal, 16U);
    memcpy(i->lease_id, j->proposed.binding_lease_id, 16U);
    /* Both inputs already passed the canonical codec: received Offer on Device,
     * sign_next's encoding preflight on Authority. Preserve the inbound proof
     * cache so exact duplicates remain valid while persistence is pending. */
    TRY(ucn_i_sha256_256(j->config.device ? j->rx_logical : j->tx_logical,
        UCN_I_ADMISSION_TRANSCRIPT_BYTES, i->transcript_digest, &j->scratch.durability.hash));
    b->domain_id = j->config.binding_domain_id; b->next_transaction_id = 1U;
    b->absolute_deadline_us = j->deadline_us;
    b->domain_generation = j->scratch.durability.domain.domain_generation;
    b->volatile_continuation.runtime_instance = i->runtime_instance;
    b->volatile_continuation.owner_instance = j->config.identity->owner_instance;
    b->volatile_continuation.object_kind = UCN_OBJECT_KIND_SEND;
    b->volatile_continuation.generation = 1U; b->volatile_continuation.slot = 0U;
    TRY(ucn_i_identity_prepare_binding(j->config.identity, i, &j->current.authority,
        b, now, &j->device_binding));
    q = &j->binding_requirement;
    TRY(ucn_i_identity_requirement_get(j->config.identity, j->device_binding, q));
    request_common(j, UCN_PERSIST_DOMAIN_IDENTITY_BINDING, q->domain_id);
    r = &j->scratch.durability.request;
    r->runtime_instance = q->runtime_instance; r->caller_owner_instance = q->caller_owner_instance;
    r->domain_generation = q->domain_generation; r->foundation_transaction_id = q->foundation_transaction_id;
    r->expected_record_generation = q->expected_record_generation; r->absolute_deadline_us = q->absolute_deadline_us;
    r->business_transition_digest = q->transition_fingerprint; r->canonical_body = q->canonical_body;
    r->body_bytes = q->body_bytes; r->schema_id = q->schema_id; r->schema_version = q->schema_version;
    r->operation_kind = q->operation_kind; r->volatile_continuation = q->volatile_continuation;
    memcpy(r->expected_body_digest, q->expected_body_digest, 16U);
    result = submit_request(j, now);
    if (result != UCN_OK) {
        (void)ucn_i_identity_abort_unsubmitted(j->config.identity, j->device_binding);
        memset(&j->device_binding, 0, sizeof(j->device_binding)); return result;
    }
    j->work_kind = 1U;
    return ucn_i_identity_bind_persistence(j->config.identity, j->device_binding,
        j->persistence_handle, j->scratch.durability.expected_digest);
}

static void security_binding(ucn_i_security_binding_t *out, const ucn_i_identity_binding_view_t *v)
{
    out->address = v->address; out->binding_generation = v->binding_generation;
    memcpy(out->principal, v->principal, 16U);
}

ucn_result_t ucn_i_native_join_persist_session(ucn_i_native_join_t *j, uint64_t now)
{
    ucn_i_admission_session_seed_t seed;
    ucn_i_security_candidate_t *c = &j->current.candidate;
    ucn_i_security_durability_base_t *b;
    ucn_i_security_requirement_view_t *q;
    ucn_persistence_request_t *r;
    uint64_t deadline;
    ucn_result_t result;
    if (j->phase != 6U || j->work_kind || j->active || j->tx_live ||
        (!j->config.device && (j->tx_opcode != 8U || !j->tx_round_complete))) return UCN_ERR_STATE;
    TRY(ucn_i_native_join_parents(j, now, true));
    deadline = j->current.local.local_deadline_us;
    if (j->current.peer.local_deadline_us < deadline) deadline = j->current.peer.local_deadline_us;
    if (j->current.authority.local_deadline_us < deadline) deadline = j->current.authority.local_deadline_us;
    TRY(ucn_i_admission_session_seed(&j->transcript, j->started_us,
        j->config.session_lifetime_us, deadline, &seed, &j->scratch.seed));
    memset(c, 0, sizeof(*c)); security_binding(&c->local, &j->current.local);
    security_binding(&c->peer, &j->current.peer); c->expires_at_us = seed.expires_at_us;
    c->link_generation = j->config.link_generation; c->policy_generation = j->config.policy_generation;
    c->session_generation = j->transcript.selected_session_generation;
    c->origin_level = j->transcript.selected_e2e_mode; c->hop_profile = UCN_I_HOP_PROFILE_H1;
    c->address_width = j->config.address_width;
    c->origin_tx.suite_id = j->transcript.selected_e2e_suite;
    c->origin_tx.key_generation = j->transcript.selected_e2e_key_generation;
    c->origin_tx.key_id = j->config.device ? seed.origin_forward_key_id : seed.origin_reverse_key_id;
    c->origin_rx = c->origin_tx;
    c->origin_rx.key_id = j->config.device ? seed.origin_reverse_key_id : seed.origin_forward_key_id;
    c->hop_tx.suite_id = 16U; c->hop_tx.key_generation = j->transcript.selected_hop_key_generation;
    c->hop_tx.key_id = j->config.device ? seed.hop_forward_key_id : seed.hop_reverse_key_id;
    c->hop_rx = c->hop_tx;
    c->hop_rx.key_id = j->config.device ? seed.hop_reverse_key_id : seed.hop_forward_key_id;
    memcpy(c->transcript_digest, seed.transcript_digest, 32U);
    memcpy(c->origin_fingerprint, seed.transcript_digest, 16U);
    memcpy(c->hop_fingerprint, seed.transcript_digest+16U, 16U);
    TRY(empty_base(j, UCN_PERSIST_DOMAIN_SECURITY_HIGH_WATER, j->config.session_domain_id));
    b = &j->scratch.durability.session_base; memset(b, 0, sizeof(*b));
    b->domain_id = j->config.session_domain_id; b->next_transaction_id = 1U;
    b->domain_generation = j->scratch.durability.domain.domain_generation;
    b->absolute_deadline_us = c->expires_at_us; b->volatile_continuation = 1U;
    TRY(ucn_i_security_prepare_static_session(j->config.security, c,
        j->evidence.bytes, j->evidence.length, b, now, &j->session));
    result = ucn_i_native_join_configure_policy(j);
    if (result != UCN_OK) {
        (void)ucn_i_security_fence(j->config.security, j->session);
        (void)ucn_i_security_retire_fenced(j->config.security, j->session);
        memset(&j->session, 0, sizeof(j->session)); return result;
    }
    q = &j->session_requirement;
    TRY(ucn_i_security_requirement_get(j->config.security, j->session, q));
    if (q->volatile_continuation > UINT16_MAX) {
        result = UCN_ERR_CONFIG;
        goto unsubmitted;
    }
    request_common(j, UCN_PERSIST_DOMAIN_SECURITY_HIGH_WATER, q->domain_id);
    r = &j->scratch.durability.request;
    r->runtime_instance = q->runtime_instance; r->caller_owner_instance = q->caller_owner_instance;
    r->domain_generation = q->domain_generation; r->foundation_transaction_id = q->foundation_transaction_id;
    r->expected_record_generation = q->expected_record_generation;
    r->absolute_deadline_us = j->deadline_us < q->absolute_deadline_us ? j->deadline_us : q->absolute_deadline_us;
    r->business_transition_digest = q->transition_fingerprint; r->canonical_body = q->canonical_body;
    r->body_bytes = q->body_bytes; r->schema_id = q->schema_id; r->schema_version = q->schema_version;
    r->operation_kind = q->operation_kind;
    r->volatile_continuation.runtime_instance = q->runtime_instance;
    r->volatile_continuation.owner_instance = q->caller_owner_instance;
    r->volatile_continuation.generation = (uint16_t)q->volatile_continuation;
    r->volatile_continuation.object_kind = UCN_OBJECT_KIND_SEND;
    memcpy(r->expected_body_digest, q->expected_body_digest, 16U);
    result = submit_request(j, now);
    if (result != UCN_OK) goto unsubmitted;
    j->work_kind = 2U;
    return ucn_i_security_bind_persistence(j->config.security, j->session,
        j->persistence_handle, j->scratch.durability.expected_digest);
unsubmitted:
    (void)ucn_i_security_fence(j->config.security, j->session);
    (void)ucn_i_security_retire_fenced(j->config.security, j->session);
    memset(&j->session, 0, sizeof(j->session)); return result;
}

static ucn_result_t binding_proof(ucn_i_native_join_t *j)
{
    const ucn_i_identity_requirement_view_t *q = &j->binding_requirement;
    ucn_i_identity_durability_proof_t *out = &j->scratch.durability.binding_proof;
    const ucn_persistence_proof_t *p = &j->scratch.durability.proof;
    memset(out, 0, sizeof(*out)); out->persistence_handle = j->persistence_handle;
    out->domain_id = p->domain.domain_id; out->record_generation = p->record_generation;
    out->foundation_transaction_id = p->foundation_transaction_id; out->witness_generation = p->witness_generation;
    out->transition_fingerprint = q->transition_fingerprint; out->runtime_instance = p->runtime_instance;
    out->body_bytes = p->body_bytes; out->volatile_continuation = q->volatile_continuation;
    out->persistence_owner_instance = p->persistence_owner_instance; out->caller_owner_instance = p->caller_owner_instance;
    out->domain_generation = p->domain_generation; out->schema_id = q->schema_id;
    out->schema_version = q->schema_version; out->operation_kind = p->operation_kind;
    memcpy(out->body_digest, p->body_digest, 16U);
    return UCN_OK;
}

static ucn_result_t session_proof(ucn_i_native_join_t *j)
{
    const ucn_i_security_requirement_view_t *q = &j->session_requirement;
    ucn_i_security_durability_proof_t *out = &j->scratch.durability.session_proof;
    const ucn_persistence_proof_t *p = &j->scratch.durability.proof;
    memset(out, 0, sizeof(*out)); out->persistence_handle = j->persistence_handle;
    out->domain_id = p->domain.domain_id; out->record_generation = p->record_generation;
    out->foundation_transaction_id = p->foundation_transaction_id; out->witness_generation = p->witness_generation;
    out->transition_fingerprint = q->transition_fingerprint; out->runtime_instance = p->runtime_instance;
    out->body_bytes = p->body_bytes; out->volatile_continuation = q->volatile_continuation;
    out->persistence_owner_instance = p->persistence_owner_instance; out->caller_owner_instance = p->caller_owner_instance;
    out->domain_generation = p->domain_generation; out->schema_id = q->schema_id;
    out->schema_version = q->schema_version; out->operation_kind = p->operation_kind;
    memcpy(out->body_digest, p->body_digest, 16U);
    return UCN_OK;
}

static ucn_result_t retire_failed(ucn_i_native_join_t *j)
{
    const ucn_i_identity_requirement_view_t *q = &j->binding_requirement;
    ucn_i_identity_persistence_failure_t *f = &j->scratch.durability.binding_failure;
    ucn_result_t terminal = j->scratch.durability.request_view.terminal_result;
    j->failed = 1U;
    if (!j->proof_consumed) {
        if (j->work_kind == 1U) {
            memset(f, 0, sizeof(*f)); f->persistence_handle = j->persistence_handle;
            f->volatile_continuation = q->volatile_continuation; f->domain_id = q->domain_id;
            f->foundation_transaction_id = q->foundation_transaction_id;
            f->transition_fingerprint = q->transition_fingerprint; f->runtime_instance = q->runtime_instance;
            f->terminal_result = terminal; f->persistence_owner_instance = j->persistence_handle.owner_instance;
            f->caller_owner_instance = q->caller_owner_instance; f->domain_generation = q->domain_generation;
            f->schema_id = q->schema_id; f->schema_version = q->schema_version;
            f->operation_kind = q->operation_kind; f->request_state = UCN_PERSIST_REQUEST_FAILED;
            TRY(ucn_i_identity_fail_persistence(j->config.identity, j->device_binding, f));
        } else {
            TRY(ucn_i_security_fence(j->config.security, j->session));
        }
        j->proof_consumed = 1U;
    }
    TRY(ucn_i_persistence_request_retire(j->config.persistence, j->persistence_handle));
    j->work_kind = 0U; j->proof_consumed = 0U;
    return terminal;
}

ucn_result_t ucn_i_native_join_finish_durability(ucn_i_native_join_t *j, uint64_t now)
{
    TRY(ucn_i_native_join_retirement_check(j, now));
    if (j->handed_off || j->work_kind == 0U) return UCN_ERR_STATE;
    TRY(ucn_i_persistence_request_get(j->config.persistence, j->persistence_handle,
        &j->scratch.durability.request_view));
    if (j->scratch.durability.request_view.state == UCN_PERSIST_REQUEST_FAILED) {
        return retire_failed(j);
    }
    if (j->scratch.durability.request_view.state != UCN_PERSIST_REQUEST_PROOF_READY) return UCN_OK;
    TRY(ucn_i_persistence_proof_get(j->config.persistence, j->persistence_handle,
        &j->scratch.durability.proof));
    if (j->stopped || now >= j->deadline_us ||
        ucn_i_native_join_check(j, now, false) != UCN_OK) {
        j->stopped = 1U;
        if (!j->proof_consumed && j->work_kind == 1U) {
            TRY(binding_proof(j));
            TRY(ucn_i_identity_discard_durable_binding(j->config.identity,
                j->device_binding, &j->scratch.durability.binding_proof));
        } else if (j->work_kind == 2U) {
            TRY(ucn_i_security_fence(j->config.security, j->session));
        }
        j->proof_consumed = 1U;
        TRY(ucn_i_persistence_proof_retire(j->config.persistence, j->persistence_handle));
        j->work_kind = 0U; j->proof_consumed = 0U;
        return UCN_OK;
    }
    if (j->proof_consumed) {
        TRY(ucn_i_persistence_proof_retire(j->config.persistence, j->persistence_handle));
        j->work_kind = 0U; j->proof_consumed = 0U; return UCN_OK;
    }
    TRY(ucn_i_native_join_parents(j, now, j->work_kind == 2U));
    if (j->work_kind == 1U) {
        TRY(binding_proof(j));
        TRY(ucn_i_identity_activate_binding(j->config.identity, j->device_binding,
            &j->scratch.durability.binding_proof, &j->current.authority, now,
            j->config.device ? &j->current.local : &j->current.peer));
        j->binding_ready = 1U;
        j->frozen_device_binding = j->config.device ? j->current.local : j->current.peer;
    } else {
        TRY(session_proof(j));
        j->facts.local = j->current.candidate.local; j->facts.peer = j->current.candidate.peer;
        j->facts.link_generation = j->config.link_generation;
        j->facts.policy_generation = j->config.policy_generation; j->facts.now_us = now;
        TRY(ucn_i_security_activate(j->config.security, j->session,
            &j->scratch.durability.session_proof, &j->facts));
        j->active = 1U;
    }
    j->proof_consumed = 1U;
    TRY(ucn_i_persistence_proof_retire(j->config.persistence, j->persistence_handle));
    j->work_kind = 0U; j->proof_consumed = 0U;
    return UCN_OK;
}
