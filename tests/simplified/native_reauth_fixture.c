#include "native_reauth_fixture.h"
#include <string.h>
static uint32_t be32(const uint8_t *b)
{ return (uint32_t)b[0]<<24 | (uint32_t)b[1]<<16 | (uint32_t)b[2]<<8 | b[3]; }
static uint16_t be16(const uint8_t *b) { return (uint16_t)((uint16_t)b[0]<<8 | b[1]); }
static uint64_t be64(const uint8_t *b) { return (uint64_t)be32(b)<<32 | be32(b+4); }
static ucn_result_t mac(native_reauth_fixture_t *f,const uint8_t principal[16],const uint8_t message[227],uint8_t out[32])
{
    uint8_t key; unsigned i; ucn_result_t result;
    if(memcmp(principal,f->prior->config.device_principal,16)==0)key=0x91U;
    else if(memcmp(principal,f->prior->config.authority_principal,16)==0)key=0xA7U;
    else return UCN_ERR_SECURITY;
    if(memcmp(message,"UCN6-PEER-REAUTH-V2",19U)!=0 ||
       ucn_i_peer_reauth_decode(message+19U,208U,&f->decoded)!=UCN_OK)return UCN_ERR_SECURITY;
    memset(f->input,0x36,64U); for(i=0;i<32U;++i)f->input[i]^=key;
    memcpy(f->input+64,message,227U);
    result=ucn_i_sha256_256(f->input,291U,f->inner,&f->hash); if(result!=UCN_OK)return result;
    memset(f->input,0x5c,64U); for(i=0;i<32U;++i)f->input[i]^=key;
    memcpy(f->input+64,f->inner,32U);
    return ucn_i_sha256_256(f->input,96U,out,&f->hash);
}
static ucn_result_t sign_record(void *context,const uint8_t principal[16],const uint8_t message[227],uint8_t proof[64],uint8_t *n)
{
    native_reauth_fixture_t *f=context; ucn_result_t result=mac(f,principal,message,proof);
    if(result!=UCN_OK)return result;
    ++f->sign_calls[message[20]]; *n=32U;
    if(message[20]==2U) { memcpy(f->final_record.canonical,message+19,208U); f->final_valid=1U; }
    if(f->corrupt_stage==message[20]+1U)proof[0]^=1U;
    return UCN_OK;
}
static ucn_result_t verify_record(void *context,const uint8_t principal[16],const uint8_t message[227],const uint8_t *proof,uint8_t n)
{
    native_reauth_fixture_t *f=context; uint8_t expected[32],diff=0U; unsigned i;
    if(n!=32U || !proof || mac(f,principal,message,expected)!=UCN_OK)return UCN_ERR_SECURITY;
    for(i=0;i<32U;++i)diff|=(uint8_t)(proof[i]^expected[i]);
    if(diff)return UCN_ERR_SECURITY;
    if(message[20]==2U) { memcpy(f->final_record.canonical,message+19,208U); f->final_valid=1U; }
    return UCN_OK;
}
static ucn_result_t verify_session(void *context,const ucn_i_security_candidate_t *c,const uint8_t *proof,size_t n)
{
    native_reauth_fixture_t *f=context; const uint8_t *b=f->final_record.canonical;
    native_join_fixture_t *p=f->prior; ucn_i_native_reauth_t *r=f->root;
    const ucn_i_security_binding_t *a=f->config.initiator?&c->local:&c->peer;
    const ucn_i_security_binding_t *z=f->config.initiator?&c->peer:&c->local;
    uint64_t deadline; unsigned i;
    if(!f->final_valid || n!=64U || !proof || b[1]!=2U || !r->started_us || r->started_us>r->now_us ||
       be64(b+116)>UINT64_MAX-r->started_us)return UCN_ERR_SECURITY;
    memcpy(f->message,"UCN6-PEER-REAUTH-V2",19U); memcpy(f->message+19,b,208U);
    f->message[20]=1U;
    if(verify_record(f,z->principal,f->message,proof,32U)!=UCN_OK)return UCN_ERR_SECURITY;
    f->message[20]=2U;
    if(verify_record(f,a->principal,f->message,proof+32,32U)!=UCN_OK)return UCN_ERR_SECURITY;
    if(ucn_i_sha256_256(f->message,227U,f->digest,&f->hash)!=UCN_OK)return UCN_ERR_SECURITY;
    if(ucn_i_identity_binding_get(&p->identity,p->device?p->join.device_binding:p->authority_binding,r->now_us,&f->local)!=UCN_OK ||
       ucn_i_identity_binding_get(&p->identity,p->device?p->authority_binding:p->join.device_binding,r->now_us,&f->peer)!=UCN_OK)return UCN_ERR_SECURITY;
    deadline=r->started_us+be64(b+116);
    if(f->local.local_deadline_us<deadline)deadline=f->local.local_deadline_us;
    if(f->peer.local_deadline_us<deadline)deadline=f->peer.local_deadline_us;
    if(p->authority.local_deadline_us<deadline)deadline=p->authority.local_deadline_us;
    if(c->expires_at_us!=deadline || c->expires_at_us<=r->now_us || c->policy_generation!=f->config.policy_generation ||
       c->link_generation!=p->config.link_generation || c->session_generation!=be32(b+60) ||
       c->address_width!=b[2] || c->origin_level!=1U || c->hop_profile!=UCN_I_HOP_PROFILE_H1 ||
       a->address!=be32(b+8) || a->binding_generation!=be32(b+12) || memcmp(a->principal,b+16,16U) ||
       z->address!=be32(b+32) || z->binding_generation!=be32(b+36) || memcmp(z->principal,b+40,16U) ||
       c->local.address!=f->local.address || c->local.binding_generation!=f->local.binding_generation ||
       c->peer.address!=f->peer.address || c->peer.binding_generation!=f->peer.binding_generation ||
       c->origin_tx.key_generation!=be32(b+72) || c->origin_rx.key_generation!=be32(b+72) ||
       c->hop_tx.key_generation!=be32(b+76) || c->hop_rx.key_generation!=be32(b+76) ||
       c->origin_tx.key_id!=be16(b+66)+(f->config.initiator?0U:1U) ||
       c->origin_rx.key_id!=be16(b+66)+(f->config.initiator?1U:0U) ||
       c->hop_tx.key_id!=be16(b+68)+(f->config.initiator?0U:1U) ||
       c->hop_rx.key_id!=be16(b+68)+(f->config.initiator?1U:0U))return UCN_ERR_SECURITY;
    for(i=0;i<32U;++i)if(c->transcript_digest[i]!=f->digest[i])return UCN_ERR_SECURITY;
    if(memcmp(c->origin_fingerprint,f->digest,16U) || memcmp(c->hop_fingerprint,f->digest+16,16U))return UCN_ERR_SECURITY;
    return UCN_OK;
}
static ucn_result_t policy(void *context,const ucn_i_security_candidate_t *c,const ucn_i_security_acl_rule_t **out,uint16_t *count)
{
    native_reauth_fixture_t *f=context; unsigned i; ucn_i_security_acl_rule_t *rules;
    if(c->session_generation>=8U)return UCN_ERR_NO_SPACE;
    rules=f->rules[c->session_generation]; memset(rules,0,2U*sizeof(*rules));
    for(i=0;i<2U;++i) { memcpy(rules[i].peer_principal,c->peer.principal,16U);
        memcpy(rules[i].context_fingerprint,c->origin_fingerprint,16U);
        rules[i].peer_binding_generation=c->peer.binding_generation; rules[i].service_id=1U;
        rules[i].direction=i?UCN_I_SECURITY_ACCESS_OUTBOUND:UCN_I_SECURITY_ACCESS_INBOUND; }
    *out=rules; *count=2U; return UCN_OK;
}
void native_reauth_fixture_configure(native_reauth_fixture_t *f,native_join_fixture_t *prior,
    ucn_i_native_reauth_t *root,bool initiator,uint64_t nonce,uint64_t transaction)
{
    memset(f,0,sizeof(*f)); f->prior=prior; f->root=root;
    f->config.parents=&prior->join; f->config.product_context=f;
    f->config.sign=sign_record; f->config.verify=verify_record; f->config.configure_session_policy=policy;
    f->config.nonce=nonce; f->config.transaction=transaction; f->config.initiator=initiator?1U:0U;
    f->config.timeout_us=100000U; f->config.session_lifetime_us=1000000U; f->config.retry_interval_us=100U;
    f->config.owner_instance=30U; f->config.max_attempts=200U; f->config.proof_bytes=32U; f->config.policy_generation=1U;
    prior->additional_verify_session=verify_session; prior->additional_verify_context=f;
}
