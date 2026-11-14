#include "internal/ucn_native_reauth.h"
#include <string.h>
#define TRY(x) do { ucn_result_t r_=(x); if(r_!=UCN_OK)return r_; } while(0)
#define P(r) ((r)->config.parents->config)
static const uint8_t domain[]="UCN6-PEER-REAUTH-V2";
static uint32_t get32(const uint8_t *b)
{ return (uint32_t)b[0]<<24 | (uint32_t)b[1]<<16 | (uint32_t)b[2]<<8 | b[3]; }
static uint16_t get16(const uint8_t *b) { return (uint16_t)((uint16_t)b[0]<<8 | b[1]); }
static bool same_binding(const ucn_i_security_binding_t *a,const ucn_i_identity_binding_view_t *b)
{ return a->address==b->address && a->binding_generation==b->binding_generation && memcmp(a->principal,b->principal,16)==0; }
static ucn_result_t read_base(ucn_i_native_reauth_t *r)
{
    ucn_persist_domain_key_t key={0}; size_t bytes; uint32_t realm;
    ucn_persistence_domain_view_t *v=&r->scratch.durable.domain;
    key.domain_kind=UCN_PERSIST_DOMAIN_SECURITY_HIGH_WATER; key.domain_id=P(r).session_domain_id;
    TRY(ucn_i_persistence_domain_get(P(r).persistence,key,v));
    if(v->state!=UCN_PERSIST_DOMAIN_READY || v->pending || v->body_bytes!=192U ||
       v->schema_id!=UCN_I_SECURITY_RECORD_SCHEMA_ID || v->schema_version!=1U ||
       !v->record_generation || v->record_generation==UINT64_MAX ||
       !v->current_transaction_id || v->current_transaction_id>=UINT64_MAX-1U)return UCN_ERR_STATE;
    TRY(ucn_i_persistence_domain_copy_body(P(r).persistence,key,r->scratch.durable.body,192U,&bytes));
    if(bytes!=192U)return UCN_ERR_STATE;
    TRY(ucn_i_security_record_decode(r->scratch.durable.body,&r->scratch.durable.security_codec,
        &realm,&r->scratch.durable.decoded));
    if(realm!=P(r).realm_id || !same_binding(&r->scratch.durable.decoded.local,&r->current.local) ||
       !same_binding(&r->scratch.durable.decoded.peer,&r->current.peer) ||
       r->scratch.durable.decoded.address_width!=P(r).address_width ||
       r->scratch.durable.decoded.hop_profile!=UCN_I_HOP_PROFILE_H1)return UCN_ERR_SECURITY;
    return UCN_OK;
}
ucn_result_t ucn_i_native_reauth_load_base(ucn_i_native_reauth_t *r)
{
    bool fenced; TRY(read_base(r)); r->previous=r->scratch.durable.decoded;
    TRY(ucn_i_security_peer_is_fenced(P(r).security,&r->previous.local,&r->previous.peer,&fenced));
    if(!fenced)return UCN_ERR_STATE;
    r->base.domain_id=P(r).session_domain_id;
    r->base.domain_generation=r->scratch.durable.domain.domain_generation;
    r->base.expected_record_generation=r->scratch.durable.domain.record_generation;
    r->base.next_transaction_id=r->scratch.durable.domain.current_transaction_id+1U;
    r->base.prior_session_generation=r->previous.session_generation;
    r->base.absolute_deadline_us=r->deadline_us; r->base.volatile_continuation=1U;
    memcpy(r->base.expected_body_digest,r->scratch.durable.domain.body_digest,16U);
    return UCN_OK;
}
ucn_result_t ucn_i_native_reauth_candidate(ucn_i_native_reauth_t *r)
{
    const uint8_t *b=r->record.canonical; ucn_i_security_candidate_t *c=&r->candidate;
    uint64_t deadline=r->started_us+r->config.session_lifetime_us;
    if(r->record.canonical[1]!=2U)return UCN_ERR_STATE;
    *c=r->previous; c->session_generation=get32(b+60); c->policy_generation=r->config.policy_generation;
    c->link_generation=P(r).link_generation;
    if(r->current.authority.local_deadline_us<deadline)deadline=r->current.authority.local_deadline_us;
    if(r->current.local.local_deadline_us<deadline)deadline=r->current.local.local_deadline_us;
    if(r->current.peer.local_deadline_us<deadline)deadline=r->current.peer.local_deadline_us;
    c->expires_at_us=deadline;
    c->origin_tx.key_generation=c->origin_rx.key_generation=get32(b+72);
    c->hop_tx.key_generation=c->hop_rx.key_generation=get32(b+76);
    c->origin_tx.key_id=(uint16_t)(get16(b+66)+(r->config.initiator?0U:1U));
    c->origin_rx.key_id=(uint16_t)(get16(b+66)+(r->config.initiator?1U:0U));
    c->hop_tx.key_id=(uint16_t)(get16(b+68)+(r->config.initiator?0U:1U));
    c->hop_rx.key_id=(uint16_t)(get16(b+68)+(r->config.initiator?1U:0U));
    memcpy(r->scratch.crypto.message,domain,sizeof(domain)-1U);
    TRY(ucn_i_peer_reauth_encode(&r->record,r->scratch.crypto.message+sizeof(domain)-1U,208U));
    TRY(ucn_i_sha256_256(r->scratch.crypto.message,sizeof(r->scratch.crypto.message),c->transcript_digest,&r->scratch.crypto.hash));
    memcpy(c->origin_fingerprint,c->transcript_digest,16U); memcpy(c->hop_fingerprint,c->transcript_digest+16U,16U);
    return UCN_OK;
}
static uint64_t parent_deadline(const ucn_i_native_reauth_t *r)
{
    uint64_t n=r->current.authority.local_deadline_us;
    if(r->current.local.local_deadline_us<n)n=r->current.local.local_deadline_us;
    if(r->current.peer.local_deadline_us<n)n=r->current.peer.local_deadline_us;
    return n;
}
static ucn_result_t verify_with_budget(void *context,const uint8_t principal[16],
    const uint8_t message[227],const uint8_t *proof,uint8_t bytes)
{
    ucn_i_native_reauth_t *r=context;
    /* Security already owns the real callback gate; never enter it twice. */
    if(!r->callbacks_remaining) return UCN_ERR_EXHAUSTED;
    --r->callbacks_remaining;
    return r->config.verify(r->config.product_context,principal,message,proof,bytes);
}
ucn_result_t ucn_i_native_reauth_persist(ucn_i_native_reauth_t *r,uint64_t now)
{
    ucn_i_security_reauth_call_t *call; ucn_persistence_request_t *request;
    const ucn_i_security_requirement_view_t *q; ucn_i_persist_record_meta_t *m;
    ucn_result_t result;
    if(r->work || r->active || r->tx_live || (r->phase!=4U && r->phase!=5U))return UCN_ERR_STATE;
    TRY(ucn_i_native_reauth_check(r,now,true)); TRY(ucn_i_native_reauth_candidate(r));
    /* Handshake TTL limits IO/replies, not an already durable Session. The
     * actual Foundation request below is still bounded by the original TTL. */
    r->base.absolute_deadline_us=r->candidate.expires_at_us;
    TRY(read_base(r));
    if(r->scratch.durable.domain.record_generation!=r->base.expected_record_generation ||
       r->scratch.durable.domain.current_transaction_id+1U!=r->base.next_transaction_id ||
       r->scratch.durable.domain.domain_generation!=r->base.domain_generation ||
       memcmp(r->scratch.durable.domain.body_digest,r->base.expected_body_digest,16U)!=0 ||
       memcmp(&r->scratch.durable.decoded,&r->previous,sizeof(r->previous))!=0)return UCN_ERR_SECURITY;
    call=&r->scratch.durable.call; memset(call,0,sizeof(*call));
    call->candidate=&r->candidate; call->previous=&r->previous; call->local_record=r->scratch.durable.body;
    call->durability=&r->base; call->record=&r->record; call->challenge_proof=r->challenge_proof;
    call->response_proof=r->response_proof; call->verify_context=r;
    call->verify_identity=verify_with_budget; call->started_us=r->started_us;
    call->parent_deadline_us=parent_deadline(r); call->now_us=now; call->initiator=r->config.initiator;
    call->challenge_proof_bytes=call->response_proof_bytes=r->config.proof_bytes;
    if(r->callbacks_remaining<4U) return UCN_ERR_EXHAUSTED;
    --r->callbacks_remaining; /* Security's own verify_session Provider call. */
    TRY(ucn_i_security_prepare_reauth_session(P(r).security,call,&r->scratch.durable.reauth,&r->session));
    TRY(ucn_i_native_reauth_provider_enter(r));
    result=r->config.configure_session_policy(r->config.product_context,&r->candidate,&r->session_acl,&r->session_acl_count);
    result=ucn_i_native_reauth_leave(r,result);
    if(result==UCN_OK)result=ucn_i_security_bind_session_acl(P(r).security,r->session,r->session_acl,r->session_acl_count);
    if(result!=UCN_OK)goto unsubmitted;
    TRY(ucn_i_security_requirement_get(P(r).security,r->session,&r->requirement)); q=&r->requirement;
    if (q->volatile_continuation > UINT16_MAX) { result=UCN_ERR_CONFIG; goto unsubmitted; }
    request=&r->scratch.durable.request; memset(request,0,sizeof(*request));
    request->struct_size=sizeof(*request); request->api_version=UCN_PERSIST_API_VERSION;
    request->domain.domain_kind=UCN_PERSIST_DOMAIN_SECURITY_HIGH_WATER; request->domain.domain_id=q->domain_id;
    request->runtime_instance=q->runtime_instance; request->caller_owner_instance=q->caller_owner_instance;
    request->domain_generation=q->domain_generation; request->foundation_transaction_id=q->foundation_transaction_id;
    request->expected_record_generation=q->expected_record_generation;
    request->absolute_deadline_us=r->deadline_us<q->absolute_deadline_us?r->deadline_us:q->absolute_deadline_us;
    request->business_transition_digest=q->transition_fingerprint; request->canonical_body=q->canonical_body;
    request->body_bytes=q->body_bytes; request->schema_id=q->schema_id; request->schema_version=q->schema_version;
    request->operation_kind=q->operation_kind; memcpy(request->expected_body_digest,q->expected_body_digest,16U);
    request->volatile_continuation.runtime_instance=q->runtime_instance;
    request->volatile_continuation.owner_instance=q->caller_owner_instance;
    request->volatile_continuation.generation=(uint16_t)q->volatile_continuation;
    request->volatile_continuation.object_kind=UCN_OBJECT_KIND_SEND;
    m=&r->scratch.durable.meta; memset(m,0,sizeof(*m)); m->domain=request->domain;
    m->record_generation=q->expected_record_generation+1U; m->transaction_id=q->foundation_transaction_id;
    m->body_bytes=q->body_bytes; m->schema_id=q->schema_id; m->schema_version=q->schema_version; m->operation_kind=q->operation_kind;
    result=ucn_i_persist_body_digest(m,q->canonical_body,r->scratch.durable.expected_digest,&r->scratch.durable.codec);
    if(result!=UCN_OK)goto unsubmitted;
    result=ucn_i_persistence_submit(P(r).persistence,request,now,&r->persistence_handle);
    if(result!=UCN_OK)goto unsubmitted;
    r->work=1U;
    return ucn_i_security_bind_persistence(P(r).security,r->session,r->persistence_handle,r->scratch.durable.expected_digest);
unsubmitted:
    (void)ucn_i_security_fence(P(r).security,r->session);
    (void)ucn_i_security_retire_fenced(P(r).security,r->session);
    memset(&r->session,0,sizeof(r->session)); return result;
}
static void project(ucn_i_native_reauth_t *r)
{
    const ucn_persistence_proof_t *p=&r->scratch.durable.proof;
    const ucn_i_security_requirement_view_t *q=&r->requirement;
    ucn_i_security_durability_proof_t *d=&r->scratch.durable.projected;
    memset(d,0,sizeof(*d)); d->persistence_handle=r->persistence_handle;
    d->domain_id=p->domain.domain_id; d->record_generation=p->record_generation;
    d->foundation_transaction_id=p->foundation_transaction_id; d->witness_generation=p->witness_generation;
    d->transition_fingerprint=q->transition_fingerprint; d->runtime_instance=p->runtime_instance;
    d->body_bytes=p->body_bytes; d->volatile_continuation=q->volatile_continuation;
    d->persistence_owner_instance=p->persistence_owner_instance; d->caller_owner_instance=p->caller_owner_instance;
    d->domain_generation=p->domain_generation; d->schema_id=q->schema_id; d->schema_version=q->schema_version;
    d->operation_kind=p->operation_kind; memcpy(d->body_digest,p->body_digest,16U);
}
ucn_result_t ucn_i_native_reauth_finish_durability(ucn_i_native_reauth_t *r,uint64_t now)
{
    ucn_result_t terminal;
    TRY(ucn_i_native_reauth_check(r,now,false)); if(!r->work)return UCN_ERR_STATE;
    TRY(ucn_i_persistence_request_get(P(r).persistence,r->persistence_handle,&r->scratch.durable.view));
    if(r->scratch.durable.view.state==UCN_PERSIST_REQUEST_FAILED) {
        terminal=r->scratch.durable.view.terminal_result;
        TRY(ucn_i_security_fence(P(r).security,r->session));
        TRY(ucn_i_persistence_request_retire(P(r).persistence,r->persistence_handle));
        r->work=0U; r->failed=1U; return terminal;
    }
    if(r->scratch.durable.view.state!=UCN_PERSIST_REQUEST_PROOF_READY)return UCN_OK;
    if(!r->proof_consumed) {
        if(r->stopped || now>=r->deadline_us || ucn_i_native_reauth_check(r,now,true)!=UCN_OK) {
            TRY(ucn_i_security_fence(P(r).security,r->session)); r->stopped=1U;
        } else {
            TRY(ucn_i_persistence_proof_get(P(r).persistence,r->persistence_handle,&r->scratch.durable.proof));
            project(r);
            r->scratch.durable.facts.local=r->candidate.local; r->scratch.durable.facts.peer=r->candidate.peer;
            r->scratch.durable.facts.link_generation=P(r).link_generation;
            r->scratch.durable.facts.policy_generation=r->config.policy_generation; r->scratch.durable.facts.now_us=now;
            TRY(ucn_i_security_activate(P(r).security,r->session,&r->scratch.durable.projected,&r->scratch.durable.facts));
            r->active=1U;
        }
        r->proof_consumed=1U;
    }
    TRY(ucn_i_persistence_proof_retire(P(r).persistence,r->persistence_handle));
    r->work=0U; r->proof_consumed=0U;
    if(!r->stopped)r->phase=r->config.initiator?6U:7U;
    return UCN_OK;
}
