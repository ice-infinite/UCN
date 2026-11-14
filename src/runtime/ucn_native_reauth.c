#include "internal/ucn_native_reauth.h"
#include "internal/ucn_checked.h"
#include <string.h>

#define MAGIC UINT32_C(0x52415554)
#define TRY(x) do { ucn_result_t r_ = (x); if (r_ != UCN_OK) return r_; } while (0)
#define P(r) ((r)->config.parents->config)
enum { IDLE, HELLO, CHALLENGE, RESPONSE, PERSIST_I, PERSIST_R, ACCEPT, PUBLISH, ESTABLISHED };
static const uint8_t domain[] = "UCN6-PEER-REAUTH-V2";
static uint32_t get32(const uint8_t *b)
{ return (uint32_t)b[0]<<24 | (uint32_t)b[1]<<16 | (uint32_t)b[2]<<8 | b[3]; }
static uint64_t get64(const uint8_t *b)
{ return (uint64_t)get32(b)<<32 | get32(b+4); }
static void put32(uint8_t *b, uint32_t n)
{ b[0]=(uint8_t)(n>>24); b[1]=(uint8_t)(n>>16); b[2]=(uint8_t)(n>>8); b[3]=(uint8_t)n; }
static void put64(uint8_t *b, uint64_t n) { put32(b,(uint32_t)(n>>32)); put32(b+4,(uint32_t)n); }
static void put16(uint8_t *b, uint16_t n) { b[0]=(uint8_t)(n>>8); b[1]=(uint8_t)n; }
static bool zero(const void *v, size_t n)
{ const uint8_t *b=v; size_t i; for(i=0;i<n;++i) if(b[i]) return false; return true; }
static bool parent_disjoint(const ucn_i_native_join_t *j, const void *out, size_t n)
{
    const ucn_i_native_join_config_t *c=&j->config;
    if(!out || ucn_i_ranges_overlap(j,sizeof(*j),out,n) ||
       ucn_i_ranges_overlap(j->configuration_source,sizeof(j->config),out,n) ||
       ucn_i_ranges_overlap(c->adapter,sizeof(*c->adapter),out,n) ||
       ucn_i_ranges_overlap(c->identity,sizeof(*c->identity),out,n) ||
       ucn_i_ranges_overlap(c->security,sizeof(*c->security),out,n) ||
       ucn_i_ranges_overlap(c->persistence,sizeof(*c->persistence),out,n) ||
       ucn_i_ranges_overlap(c->callback_gate,sizeof(*c->callback_gate),out,n) ||
       (c->admission && ucn_i_ranges_overlap(c->admission,sizeof(*c->admission),out,n)) ||
       ucn_i_ranges_overlap(c->persistence->callback_gate,sizeof(*c->persistence->callback_gate),out,n)) return false;
    if((j->session_acl && ucn_i_ranges_overlap(j->session_acl,sizeof(*j->session_acl)*j->session_acl_count,out,n)) ||
       ucn_i_ranges_overlap(c->security->acl_rules,sizeof(*c->security->acl_rules)*c->security->acl_rule_count,out,n) ||
       ucn_i_ranges_overlap(c->security->domain_rules,sizeof(*c->security->domain_rules)*c->security->domain_rule_count,out,n) ||
       ucn_i_ranges_overlap(c->identity->domain_rules,sizeof(*c->identity->domain_rules)*c->identity->domain_rule_count,out,n)) return false;
    return true;
}
static bool disjoint(const ucn_i_native_reauth_t *r, const void *out, size_t n)
{
    return out && !ucn_i_ranges_overlap(r,sizeof(*r),out,n) &&
        !ucn_i_ranges_overlap(r->configuration_source,sizeof(r->config),out,n) &&
        !(r->session_acl && ucn_i_ranges_overlap(r->session_acl,sizeof(*r->session_acl)*r->session_acl_count,out,n)) &&
        parent_disjoint(r->config.parents,out,n);
}
ucn_result_t ucn_i_native_reauth_enter(ucn_i_native_reauth_t *r)
{
    if(r->operation_id==UINT32_MAX) return UCN_ERR_EXHAUSTED;
    r->claim.owner_instance=r->config.owner_instance; r->claim.operation_id=++r->operation_id;
    r->claim.operation_generation=1U; r->claim.operation_kind=2U;
    return ucn_i_callback_gate_enter(P(r).callback_gate,&r->claim);
}
ucn_result_t ucn_i_native_reauth_leave(ucn_i_native_reauth_t *r, ucn_result_t result)
{ return ucn_i_callback_gate_leave(P(r).callback_gate,&r->claim)==UCN_OK ? result : UCN_ERR_STATE; }
ucn_result_t ucn_i_native_reauth_provider_enter(ucn_i_native_reauth_t *r)
{
    ucn_result_t result;
    if(!r->callbacks_remaining) return UCN_ERR_EXHAUSTED;
    result=ucn_i_native_reauth_enter(r);
    if(result==UCN_OK) --r->callbacks_remaining;
    return result;
}
ucn_result_t ucn_i_native_reauth_check(ucn_i_native_reauth_t *r,uint64_t now,bool live)
{
    bool busy; ucn_i_callback_claim_t claim;
    if(!r || r->magic!=MAGIC || !now || now<r->now_us) return UCN_ERR_STATE;
    TRY(ucn_i_callback_gate_view(P(r).callback_gate,&busy,&claim));
    if(busy) return UCN_ERR_STATE;
    r->now_us=now;
    if(live) {
        if(r->stopped || r->failed) return UCN_ERR_STATE;
        TRY(ucn_i_native_join_bindings_current(r->config.parents,now,&r->current));
    }
    return UCN_OK;
}
static void binding(uint8_t *b,const ucn_i_security_binding_t *v)
{ put32(b,v->address); put32(b+4,v->binding_generation); memcpy(b+8,v->principal,16); }
static void history(uint8_t *b,const ucn_i_security_candidate_t *c,bool initiator)
{
    put32(b+(initiator?56:164),c->session_generation);
    put32(b+(initiator?156:168),c->origin_tx.key_generation);
    put32(b+(initiator?160:172),c->hop_tx.key_generation);
    memcpy(b+(initiator?124:176),c->transcript_digest,32);
}
static ucn_result_t successor(uint8_t *b)
{
    unsigned i; static const uint16_t a[3]={56,156,160},z[3]={164,168,172},o[3]={60,72,76};
    for(i=0;i<3;++i) { uint32_t x=get32(b+a[i]),y=get32(b+z[i]); if(y>x)x=y;
        if(x==UINT32_MAX)return UCN_ERR_EXHAUSTED;
        put32(b+o[i],x+1U); }
    return UCN_OK;
}
static ucn_result_t make_record(ucn_i_native_reauth_t *r)
{
    uint8_t *b=r->record.canonical;
    const ucn_i_security_candidate_t *p=&r->previous;
    uint16_t origin=p->origin_tx.key_id<p->origin_rx.key_id?p->origin_tx.key_id:p->origin_rx.key_id;
    uint16_t hop=p->hop_tx.key_id<p->hop_rx.key_id?p->hop_tx.key_id:p->hop_rx.key_id;
    if((p->origin_tx.key_id<p->origin_rx.key_id)!=!!r->config.initiator ||
       (p->hop_tx.key_id<p->hop_rx.key_id)!=!!r->config.initiator ||
       (uint32_t)p->origin_tx.key_id+p->origin_rx.key_id!=(uint32_t)origin*2U+1U ||
       (uint32_t)p->hop_tx.key_id+p->hop_rx.key_id!=(uint32_t)hop*2U+1U ||
       p->origin_tx.suite_id!=p->origin_rx.suite_id || p->hop_tx.suite_id!=16U || p->hop_rx.suite_id!=16U ||
       p->origin_tx.key_generation!=p->origin_rx.key_generation || p->hop_tx.key_generation!=p->hop_rx.key_generation)
        return UCN_ERR_CONFIG;
    memset(b,0,208); b[0]=2U; b[2]=p->address_width; put32(b+4,P(r).realm_id);
    binding(b+8,r->config.initiator?&p->local:&p->peer);
    binding(b+32,r->config.initiator?&p->peer:&p->local);
    b[64]=p->origin_tx.suite_id; b[65]=16U; put16(b+66,origin); put16(b+68,hop);
    put64(b+116,r->config.session_lifetime_us);
    if(r->config.initiator) {
        history(b,p,true); TRY(successor(b)); put16(b+80,(uint16_t)(P(r).link_index+1U));
        put32(b+84,P(r).link_generation); put64(b+92,r->config.transaction); put64(b+100,r->config.nonce);
    }
    return UCN_OK;
}
ucn_result_t ucn_i_native_reauth_init(ucn_i_native_reauth_t *r,const ucn_i_native_reauth_config_t *c,uint64_t now)
{
    ucn_result_t result; bool drained; uint32_t generation; uint16_t mtu; bool ready;
    if(!r || !c || !c->parents || ucn_i_ranges_overlap(r,sizeof(*r),c,sizeof(*c)) ||
       ucn_i_ranges_overlap(r,sizeof(*r),c->parents,sizeof(*c->parents))) return UCN_ERR_ARGUMENT;
    if(UCN_PERSIST_DOMAIN_COUNT<3U) return UCN_ERR_UNSUPPORTED;
    if(!zero(r,sizeof(*r))) return UCN_ERR_STATE;
    if(!c->parents->handed_off || !c->nonce || !c->sign || !c->verify || !c->configure_session_policy ||
       c->initiator>1U || (!!c->transaction)!=!!c->initiator || !c->policy_generation || !c->owner_instance ||
       c->proof_bytes<16U || c->proof_bytes>64U || !now || !c->timeout_us || !c->session_lifetime_us ||
       c->session_lifetime_us>INT64_MAX || !c->retry_interval_us || c->max_attempts<16U || c->max_attempts>UINT16_MAX/4U ||
       now>UINT64_MAX-c->timeout_us || now>UINT64_MAX-c->session_lifetime_us) return UCN_ERR_CONFIG;
    /* No output into borrowed storage: validate via a small config-only probe,
     * not a full Owner/root copy. The root is unpublished until all checks pass. */
    if(!parent_disjoint(c->parents,r,sizeof(*r)) || !parent_disjoint(c->parents,c,sizeof(*c)))
        return UCN_ERR_ARGUMENT;
    r->config=*c; r->configuration_source=c;
    result=ucn_i_native_join_bindings_current(c->parents,now,&r->current);
    if(result!=UCN_OK) goto fail;
    result=ucn_i_adapter_obligations_drained(P(r).adapter,&drained);
    if(result!=UCN_OK || !drained) { result=UCN_ERR_STATE; goto fail; }
    result=ucn_i_adapter_link_snapshot(P(r).adapter,P(r).link_index,&generation,&mtu,&ready);
    if(result!=UCN_OK || !ready || generation!=P(r).link_generation || mtu<123U+2U*P(r).address_width) {
        result=UCN_ERR_CONFIG; goto fail;
    }
    r->callbacks_remaining=(uint16_t)(4U*c->max_attempts);
    r->started_us=now; r->now_us=now; r->deadline_us=now+c->timeout_us;
    if(r->current.authority.local_deadline_us<r->deadline_us)r->deadline_us=r->current.authority.local_deadline_us;
    if(r->current.local.local_deadline_us<r->deadline_us)r->deadline_us=r->current.local.local_deadline_us;
    if(r->current.peer.local_deadline_us<r->deadline_us)r->deadline_us=r->current.peer.local_deadline_us;
    result=ucn_i_native_reauth_load_base(r); if(result!=UCN_OK)goto fail;
    result=make_record(r); if(result!=UCN_OK)goto fail;
    r->phase=c->initiator?IDLE:HELLO; r->tx_stage=255U; r->rx_stage=255U;
    r->magic=MAGIC; return UCN_OK;
fail: memset(r,0,sizeof(*r)); return result;
}

static ucn_result_t message(ucn_i_native_reauth_t *r,const ucn_i_peer_reauth_record_t *v)
{
    memcpy(r->scratch.crypto.message,domain,sizeof(domain)-1U);
    return ucn_i_peer_reauth_encode(v,r->scratch.crypto.message+sizeof(domain)-1U,208U);
}
static ucn_result_t queue(ucn_i_native_reauth_t *r,uint8_t stage)
{
    ucn_result_t result; uint8_t n=0;
    r->record.canonical[1]=stage; TRY(message(r,&r->record)); TRY(ucn_i_native_reauth_provider_enter(r));
    result=r->config.sign(r->config.product_context,r->current.local.principal,
        r->scratch.crypto.message,r->scratch.crypto.proof,&n);
    TRY(ucn_i_native_reauth_leave(r,result));
    if(n!=r->config.proof_bytes)return UCN_ERR_SECURITY;
    TRY(ucn_i_native_reauth_provider_enter(r));
    result=r->config.verify(r->config.product_context,r->current.local.principal,
        r->scratch.crypto.message,r->scratch.crypto.proof,n);
    TRY(ucn_i_native_reauth_leave(r,result));
    memcpy(r->tx_logical,r->record.canonical,208U); r->tx_logical[208]=n;
    memcpy(r->tx_logical+209,r->scratch.crypto.proof,n);
    if(stage==1U)memcpy(r->challenge_proof,r->scratch.crypto.proof,n);
    if(stage==2U)memcpy(r->response_proof,r->scratch.crypto.proof,n);
    r->tx_bytes=(uint16_t)(209U+n); r->tx_stage=stage; r->tx_index=0U;
    r->tx_count=(uint8_t)((r->tx_bytes+79U)/80U); r->tx_ready=1U; r->tx_complete=0U;
    return UCN_OK;
}
static uint16_t opcode(uint8_t stage) { return stage==3U?0x14U:(uint16_t)(0x10U+stage); }
static ucn_result_t submit(ucn_i_native_reauth_t *r)
{
    ucn_i_peer_reauth_fragment_t *p=&r->scratch.wire.fragment;
    ucn_i_c0_frame_t *f=&r->scratch.wire.frame;
    size_t bytes,offset=(size_t)r->tx_index*80U;
    ucn_result_t result;
    if(r->attempts>=r->config.max_attempts)return UCN_ERR_EXHAUSTED;
    memset(p,0,sizeof(*p)); p->stage=r->tx_stage; p->index=r->tx_index;
    p->transaction=get64(r->record.canonical+92); p->total_bytes=r->tx_bytes;
    p->data_bytes=(uint8_t)(r->tx_bytes-offset>80U?80U:r->tx_bytes-offset);
    memcpy(p->data,r->tx_logical+offset,p->data_bytes);
    TRY(ucn_i_peer_reauth_fragment_encode(p,r->scratch.wire.payload,sizeof(r->scratch.wire.payload),&bytes));
    memset(f,0,sizeof(*f)); f->payload=r->scratch.wire.payload; f->payload_bytes=bytes;
    f->transaction_id=p->transaction; f->realm_id=P(r).realm_id;
    f->source_address=r->current.local.address; f->source_binding_generation=r->current.local.binding_generation;
    f->destination_address=r->current.peer.address; f->destination_binding_generation=r->current.peer.binding_generation;
    f->opcode=opcode(r->tx_stage); f->traffic_class=2U; f->hop_limit=1U;
    f->interaction_role=(r->tx_stage&1U)?2U:1U; f->payload_kind=UCN_I_PAYLOAD_CONTROL;
    TRY(ucn_i_c0_encode(f,P(r).address_width,0U,r->tx_frame,sizeof(r->tx_frame),&r->tx_frame_bytes));
    TRY(ucn_i_adapter_tx_reserve(P(r).adapter,P(r).link_index,P(r).link_generation,0U,&r->tx_token));
    r->tx_live=1U; ++r->attempts; TRY(ucn_i_native_reauth_enter(r));
    result=ucn_i_adapter_tx_submit_ref(P(r).adapter,&r->tx_token,r->tx_frame,r->tx_frame_bytes,&r->tx_view);
    return ucn_i_native_reauth_leave(r,result);
}
ucn_result_t ucn_i_native_reauth_retire_one(ucn_i_native_reauth_t *r,uint64_t now)
{
    TRY(ucn_i_native_reauth_check(r,now,false)); if(!r->tx_live)return UCN_OK;
    TRY(ucn_i_adapter_tx_view(P(r).adapter,&r->tx_token,&r->tx_view));
    if(r->stopped && r->tx_view.state!=UCN_I_ADAPTER_TX_COMPLETED &&
       r->tx_view.state!=UCN_I_ADAPTER_TX_NOT_SUBMITTED && r->tx_view.state!=UCN_I_ADAPTER_TX_CANCELLED) {
        ucn_result_t result; TRY(ucn_i_native_reauth_enter(r));
        result=ucn_i_adapter_tx_cancel(P(r).adapter,&r->tx_token); TRY(ucn_i_native_reauth_leave(r,result));
        TRY(ucn_i_adapter_tx_view(P(r).adapter,&r->tx_token,&r->tx_view));
    }
    if(r->tx_view.state==UCN_I_ADAPTER_TX_IN_DOUBT)return UCN_ERR_IN_DOUBT;
    if(r->tx_view.state!=UCN_I_ADAPTER_TX_COMPLETED && r->tx_view.state!=UCN_I_ADAPTER_TX_NOT_SUBMITTED &&
       r->tx_view.state!=UCN_I_ADAPTER_TX_CANCELLED)return UCN_OK;
    TRY(ucn_i_adapter_tx_retire(P(r).adapter,&r->tx_token)); r->tx_live=0U;
    if(r->tx_view.state==UCN_I_ADAPTER_TX_COMPLETED && r->tx_view.terminal_result==UCN_OK) {
        if(++r->tx_index==r->tx_count) {
            r->tx_complete=1U; r->tx_ready=0U;
            if(r->phase==PUBLISH)r->phase=ESTABLISHED;
            r->retry_at_us=UINT64_MAX-now<r->config.retry_interval_us?UINT64_MAX:now+r->config.retry_interval_us;
        }
    }
    return UCN_OK;
}
ucn_result_t ucn_i_native_reauth_drive_one(ucn_i_native_reauth_t *r,uint64_t now)
{
    TRY(ucn_i_native_reauth_check(r,now,true));
    if(r->handed_off)return UCN_ERR_STATE;
    if(now>=r->deadline_us || r->attempts>=r->config.max_attempts)
        return r->phase==ESTABLISHED?UCN_OK:(now>=r->deadline_us?UCN_ERR_TIMEOUT:UCN_ERR_EXHAUSTED);
    if(r->tx_live || r->work)return UCN_OK;
    if(r->phase==IDLE) { TRY(queue(r,0U)); r->phase=CHALLENGE; }
    if(r->phase==PERSIST_I || r->phase==PERSIST_R)return ucn_i_native_reauth_persist(r,now);
    if(r->phase==PUBLISH && r->tx_stage!=3U)TRY(queue(r,3U));
    if(r->phase==ESTABLISHED && (r->config.initiator || r->peer_observed))return UCN_OK;
    if(r->tx_stage!=255U && r->tx_complete && now>=r->retry_at_us) {
        r->tx_index=0U; r->tx_ready=1U; r->tx_complete=0U;
    }
    return r->tx_ready?submit(r):UCN_OK;
}

static bool expected(ucn_i_native_reauth_t *r)
{
    const uint8_t *b=r->incoming.canonical,*e=r->record.canonical; unsigned i;
    for(i=0;i<208U;++i) {
        if(i==1U)continue;
        if(r->phase==HELLO && ((i>=56U&&i<64U)||(i>=72U&&i<116U)||i>=124U))continue;
        if((r->phase==CHALLENGE || b[1]==0U) &&
           ((i>=60U&&i<64U)||(i>=72U&&i<80U)||(i>=82U&&i<84U)||
            (i>=88U&&i<92U)||(i>=108U&&i<116U)||i>=164U))continue;
        if(b[i]!=e[i])return false;
    }
    return true;
}
static ucn_result_t accept(ucn_i_native_reauth_t *r,uint8_t stage)
{
    ucn_result_t result; uint8_t *b=r->incoming.canonical;
    if(b[1]!=stage || !expected(r) || r->rx_logical[208]!=r->config.proof_bytes)return UCN_ERR_SECURITY;
    TRY(message(r,&r->incoming)); TRY(ucn_i_native_reauth_provider_enter(r));
    result=r->config.verify(r->config.product_context,r->current.peer.principal,
        r->scratch.crypto.message,r->rx_logical+209,r->config.proof_bytes);
    TRY(ucn_i_native_reauth_leave(r,result));
    if(r->tx_live)return UCN_ERR_STATE;
    if(r->phase==HELLO && stage==0U) {
        r->record=r->incoming; history(r->record.canonical,&r->previous,false);
        put16(r->record.canonical+82,(uint16_t)(P(r).link_index+1U));
        put32(r->record.canonical+88,P(r).link_generation); put64(r->record.canonical+108,r->config.nonce);
        TRY(successor(r->record.canonical)); TRY(queue(r,1U)); r->phase=RESPONSE;
    } else if(r->phase==CHALLENGE && stage==1U) {
        r->record=r->incoming; memcpy(r->challenge_proof,r->rx_logical+209,r->config.proof_bytes);
        TRY(queue(r,2U)); r->phase=PERSIST_I;
    } else if(r->phase==RESPONSE && stage==2U) {
        r->record=r->incoming; memcpy(r->response_proof,r->rx_logical+209,r->config.proof_bytes);
        r->tx_ready=0U; r->phase=PERSIST_R;
    } else if(r->phase==ACCEPT && stage==3U) {
        r->record=r->incoming; r->phase=ESTABLISHED; r->tx_ready=0U;
    } else if((stage==0U && r->phase==RESPONSE) ||
              (stage==2U && (r->phase==PUBLISH || r->phase==ESTABLISHED)) ||
              (stage==1U && (r->phase==ACCEPT || r->phase==PERSIST_I))) {
        r->tx_index=0U; r->tx_ready=1U; r->tx_complete=0U;
    } else return UCN_ERR_STATE;
    return UCN_OK;
}
ucn_result_t ucn_i_native_reauth_receive_claimed(ucn_i_native_reauth_t *r,const ucn_i_adapter_rx_view_t *rx,uint64_t now)
{
    ucn_i_c0_frame_t *f; ucn_i_peer_reauth_fragment_t *p;
    uint8_t stage,bit; ucn_result_t result; uint64_t transaction;
    TRY(ucn_i_native_reauth_check(r,now,true));
    if(r->handed_off)return UCN_ERR_STATE;
    if(now>=r->deadline_us)return UCN_ERR_TIMEOUT;
    if(r->attempts>=r->config.max_attempts)return UCN_ERR_EXHAUSTED;
    if(!rx || rx->link_slot!=P(r).link_index || rx->link_instance_generation!=P(r).link_generation ||
       rx->meta.sender_discriminator!=P(r).physical_peer || rx->meta.timestamp_us<r->started_us ||
       rx->meta.timestamp_us>now)return UCN_ERR_SECURITY;
    f=&r->scratch.wire.frame; TRY(ucn_i_c0_decode(rx->frame,rx->frame_bytes,P(r).address_width,0U,f));
    if(f->opcode==0x14U)stage=3U;
    else if(f->opcode>=0x10U && f->opcode<=0x12U)stage=(uint8_t)(f->opcode-0x10U);
    else return UCN_ERR_MALFORMED;
    transaction=f->transaction_id;
    if(f->realm_id!=P(r).realm_id || f->source_address!=r->current.peer.address ||
       f->source_binding_generation!=r->current.peer.binding_generation ||
       f->destination_address!=r->current.local.address || f->destination_binding_generation!=r->current.local.binding_generation ||
       f->origin_security || f->traffic_class!=2U || f->delivery_guarantee || f->hop_limit!=1U ||
       f->payload_kind!=UCN_I_PAYLOAD_CONTROL || f->interaction_role!=((stage&1U)?2U:1U) ||
       !!r->config.initiator==!(stage&1U))return UCN_ERR_SECURITY;
    p=&r->scratch.wire.fragment; TRY(ucn_i_peer_reauth_fragment_decode(f->payload,f->payload_bytes,p));
    if(p->stage!=stage || p->transaction!=transaction || p->total_bytes!=209U+r->config.proof_bytes ||
       (r->phase!=HELLO && transaction!=get64(r->record.canonical+92)))return UCN_ERR_SECURITY;
    /* Full repeated messages are authenticated again; retransmission never
     * changes cached output or republishes a Session. */
    if(p->index==0U) { r->rx_bitmap=0U; r->rx_stage=stage; r->rx_transaction=transaction;
        r->rx_bytes=p->total_bytes; r->rx_count=(uint8_t)((p->total_bytes+79U)/80U); }
    if(r->rx_stage!=stage || r->rx_transaction!=transaction)return UCN_ERR_STATE;
    bit=(uint8_t)(1U<<p->index);
    if(r->rx_bitmap&bit)return memcmp(r->rx_logical+(size_t)p->index*80U,p->data,p->data_bytes)==0?UCN_OK:UCN_ERR_REPLAY;
    memcpy(r->rx_logical+(size_t)p->index*80U,p->data,p->data_bytes); r->rx_bitmap|=bit;
    if(r->rx_bitmap!=(uint8_t)((1U<<r->rx_count)-1U))return UCN_OK;
    TRY(ucn_i_peer_reauth_decode(r->rx_logical,208U,&r->incoming));
    if(get64(r->incoming.canonical+92)!=transaction)return UCN_ERR_SECURITY;
    result=accept(r,stage); r->rx_bitmap=0U; r->rx_stage=255U; return result;
}
ucn_result_t ucn_i_native_reauth_session_current(ucn_i_native_reauth_t *r,uint64_t now,ucn_i_native_join_current_t *out)
{
    if(!r || r->magic!=MAGIC || !disjoint(r,out,sizeof(*out)))return UCN_ERR_ARGUMENT;
    TRY(ucn_i_native_reauth_check(r,now,true));
    if(r->phase!=ESTABLISHED || !r->active || r->work)return UCN_ERR_STATE;
    TRY(ucn_i_security_session_candidate_get(P(r).security,r->session,&r->current.candidate));
    r->scratch.durable.facts.local=r->current.candidate.local; r->scratch.durable.facts.peer=r->current.candidate.peer;
    r->scratch.durable.facts.link_generation=P(r).link_generation;
    r->scratch.durable.facts.policy_generation=r->config.policy_generation; r->scratch.durable.facts.now_us=now;
    TRY(ucn_i_security_c1_context_check(P(r).security,&r->session,&r->scratch.durable.facts,
        r->current.candidate.origin_fingerprint,r->current.candidate.hop_fingerprint));
    r->current.session=r->session; *out=r->current; return UCN_OK;
}
ucn_result_t ucn_i_native_reauth_bindings_current(ucn_i_native_reauth_t *r,uint64_t now,ucn_i_native_join_current_t *out)
{
    if(!r || r->magic!=MAGIC || !disjoint(r,out,sizeof(*out)))return UCN_ERR_ARGUMENT;
    TRY(ucn_i_native_reauth_check(r,now,true)); *out=r->current; return UCN_OK;
}
ucn_result_t ucn_i_native_reauth_stop(ucn_i_native_reauth_t *r,uint64_t now)
{
    uint32_t generation; uint16_t mtu; bool ready;
    TRY(ucn_i_native_reauth_check(r,now,false));
    if(r->handed_off)return UCN_ERR_STATE;
    TRY(ucn_i_adapter_link_snapshot(P(r).adapter,P(r).link_index,&generation,&mtu,&ready));
    if(generation!=P(r).link_generation)return UCN_ERR_STATE;
    r->stopped=1U; r->tx_ready=0U;
    if(r->session.session_generation)TRY(ucn_i_security_fence(P(r).security,r->session));
    return UCN_OK;
}
ucn_result_t ucn_i_native_reauth_quiescent(ucn_i_native_reauth_t *r,bool *out)
{
    if(!r || r->magic!=MAGIC || !disjoint(r,out,sizeof(*out)))return UCN_ERR_ARGUMENT;
    TRY(ucn_i_native_reauth_check(r,r->now_us,false));
    *out=(r->stopped || r->handed_off) && !r->tx_live && !r->work;
    return UCN_OK;
}
ucn_result_t ucn_i_native_reauth_handoff(ucn_i_native_reauth_t *r,uint64_t now,ucn_i_native_join_current_t *out)
{
    if(!r || r->magic!=MAGIC || !disjoint(r,out,sizeof(*out)))return UCN_ERR_ARGUMENT;
    TRY(ucn_i_native_reauth_check(r,now,true));
    if(r->handed_off || r->phase!=ESTABLISHED || r->tx_live || r->work)return UCN_ERR_STATE;
    TRY(ucn_i_native_reauth_session_current(r,now,out));
    r->handed_off=1U; r->tx_ready=0U;
    return UCN_OK;
}
