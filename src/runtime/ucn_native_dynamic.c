#include "internal/ucn_native_dynamic.h"
#include "internal/ucn_checked.h"
#include <string.h>

#define DYNAMIC_MAGIC UINT32_C(0x554e4459)
static bool same_handle(ucn_i_security_handle_t a, ucn_i_security_handle_t b)
{ return memcmp(&a, &b, sizeof(a)) == 0; }
static bool same_binding(const ucn_i_security_binding_t *a, const ucn_i_security_binding_t *b)
{ return memcmp(a, b, sizeof(*a)) == 0; }
static bool borrowed_safe(const ucn_i_native_capability_t *c,const void *out,size_t bytes)
{
    const ucn_i_native_join_t *j;
    const ucn_i_native_join_config_t *p;
    if(!c || !(j=c->config.join_parents) || !c->config.capability_owner) return false;
    p=&j->config;
    if(!p->identity || !p->security || !p->persistence || !p->callback_gate) return false;
    if(ucn_i_security_borrowed_output_check(p->security,out,bytes)!=UCN_OK) return false;
    return !ucn_i_ranges_overlap(c,sizeof(*c),out,bytes) &&
        !ucn_i_ranges_overlap(c->config.capability_owner,sizeof(*c->config.capability_owner),out,bytes) &&
        !ucn_i_ranges_overlap(j,sizeof(*j),out,bytes) &&
        !ucn_i_ranges_overlap(j->configuration_source,sizeof(*j->configuration_source),out,bytes) &&
        !ucn_i_ranges_overlap(p->identity,sizeof(*p->identity),out,bytes) &&
        !ucn_i_ranges_overlap(p->identity->domain_rules,p->identity->domain_rule_count*sizeof(*p->identity->domain_rules),out,bytes) &&
        !ucn_i_ranges_overlap(p->security,sizeof(*p->security),out,bytes) &&
        !ucn_i_ranges_overlap(p->persistence,sizeof(*p->persistence),out,bytes) &&
        !ucn_i_ranges_overlap(p->persistence->callback_gate,sizeof(*p->persistence->callback_gate),out,bytes) &&
        !ucn_i_ranges_overlap(p->callback_gate,sizeof(*p->callback_gate),out,bytes) &&
        (!p->admission || !ucn_i_ranges_overlap(p->admission,sizeof(*p->admission),out,bytes)) &&
        !ucn_i_ranges_overlap(p->security->acl_rules,p->security->acl_rule_count*sizeof(*p->security->acl_rules),out,bytes) &&
        !ucn_i_ranges_overlap(j->session_acl,j->session_acl_count*sizeof(*j->session_acl),out,bytes);
}
static bool actor_idle(ucn_i_native_dynamic_t *d, uint64_t now)
{
    bool active = true;
    return d && d->magic == DYNAMIC_MAGIC && d->root->lifecycle==UCN_LIFECYCLE_RUNNING &&
        now >= d->root->now && now < d->root->expires_at &&
        ucn_i_callback_gate_view(d->root->gate, &active, &d->claim) == UCN_OK && !active &&
        ucn_native_backend_check_binding(d->native, &d->root->port) == UCN_OK;
}
bool ucn_i_native_dynamic_output_safe(const ucn_i_native_dynamic_t *d, const void *out, size_t bytes)
{
    uint16_t i;
    if (!d || !out || !bytes || ucn_i_ranges_overlap(d, sizeof(*d), out, bytes) ||
        ucn_i_ranges_overlap(d->facade_lifecycle.address,d->facade_lifecycle.bytes,out,bytes)) return false;
    for (i = 0U; i < d->root->neighbor_count; ++i) {
        const ucn_i_native_capability_t *c[2] = {d->current[i], d->pending[i]};
        unsigned j;
        if(d->reauth[i] && (ucn_i_ranges_overlap(d->reauth[i],sizeof(*d->reauth[i]),out,bytes) ||
            ucn_i_ranges_overlap(d->reauth[i]->configuration_source,sizeof(*d->reauth[i]->configuration_source),out,bytes))) return false;
        for (j = 0U; j < 2U; ++j) if (c[j]) {
            if(!borrowed_safe(c[j],out,bytes)) return false;
        }
    }
    return true;
}
static ucn_result_t selected(ucn_i_native_dynamic_t *d, uint16_t peer, uint64_t now)
{
    return ucn_i_native_capability_selected(d->current[peer], now,&d->reference,&d->cached,&d->profile);
}
static bool profile_supports(const ucn_i_native_dynamic_t *d,const ucn_i_security_candidate_t *c)
{
    /* Capability Hop bitmap bit1 denotes HMAC suite16; it is not bit16.
     * 能力位图使用Registry索引，不能把线上Suite ID直接作位移。 */
    return c->hop_profile==UCN_I_HOP_PROFILE_H1 && c->origin_tx.suite_id<32U && c->origin_rx.suite_id<32U &&
        c->hop_tx.suite_id==16U && c->hop_rx.suite_id==16U &&
        (d->profile.feature_bits&UCN_I_CAPABILITY_REQUIRED_BASE_FEATURES)==UCN_I_CAPABILITY_REQUIRED_BASE_FEATURES &&
        (d->profile.hop_suite_bits&UINT32_C(2)) &&
        (d->profile.e2e_suite_bits&(UINT32_C(1)<<c->origin_tx.suite_id)) &&
        (d->profile.e2e_suite_bits&(UINT32_C(1)<<c->origin_rx.suite_id));
}
static ucn_result_t history(ucn_i_native_dynamic_t *d,uint16_t peer,ucn_i_route_link_ref_t *out,uint16_t *physical)
{
    const ucn_i_native_neighbor_t *n=&d->root->neighbors[peer];
    uint32_t generation; uint16_t mtu; bool ready; ucn_result_t r;
    r=ucn_i_adapter_link_snapshot(d->root->adapter,n->link_slot,&generation,&mtu,&ready);
    if(r!=UCN_OK || !ready || generation!=n->facts.link_generation) return UCN_ERR_STATE;
    *physical=mtu;
    memset(out,0,sizeof(*out)); out->peer.address=n->facts.peer.address;
    out->peer.generation=n->facts.peer.binding_generation; memcpy(out->peer.principal,n->facts.peer.principal,16U);
    out->link_id=(uint16_t)(n->link_slot+1U); out->link_generation=generation;
    out->cost=1U; out->capability_bits=n->capability_bits;
    if(d->cached.record.link.link_frame_mtu<mtu) mtu=(uint16_t)d->cached.record.link.link_frame_mtu;
    if(d->cached.record.link.processing_frame_mtu<mtu) mtu=(uint16_t)d->cached.record.link.processing_frame_mtu;
    out->frame_mtu=mtu; return UCN_OK;
}
ucn_result_t ucn_i_native_dynamic_init(ucn_i_native_dynamic_t *d,
    ucn_i_native_routed_t *b, ucn_native_t *native,
    ucn_i_native_capability_t *const *peers, uint16_t count, uint64_t now)
{
    uint16_t i; const uint8_t *raw; size_t n; ucn_native_borrowed_range_t range;
    if (!d || !b || !native || !peers || count != b->neighbor_count || !count || count > UCN_I_NATIVE_NEIGHBORS ||
        ucn_i_ranges_overlap(d,sizeof(*d),b,sizeof(*b)) ||
        ucn_i_ranges_overlap(d,sizeof(*d),native,UCN_NATIVE_STORAGE_BYTES) ||
        ucn_i_ranges_overlap(d,sizeof(*d),b->adapter,sizeof(*b->adapter)) ||
        ucn_i_ranges_overlap(d,sizeof(*d),b->routes,sizeof(*b->routes)) ||
        ucn_i_ranges_overlap(d,sizeof(*d),b->gate,sizeof(*b->gate)) ||
        ucn_i_ranges_overlap(d,sizeof(*d),peers,count*sizeof(*peers))) return UCN_ERR_ARGUMENT;
    raw = (const uint8_t *)d;
    for (n=0U;n<sizeof(*d);++n) if (raw[n]) return UCN_ERR_STATE;
    if (b->dynamic || b->lifecycle != UCN_LIFECYCLE_INITIALIZED || now != b->now ||
        ucn_native_backend_check_binding(native,&b->port) != UCN_OK) return UCN_ERR_STATE;
    if(ucn_native_backend_lifecycle_range(native,&range)!=UCN_OK) return UCN_ERR_STATE;
    if(ucn_i_ranges_overlap(d,sizeof(*d),range.address,range.bytes)) return UCN_ERR_ARGUMENT;
    for (i=0U;i<count;++i) {
        const ucn_i_native_neighbor_t *p=&b->neighbors[i];
        if (!peers[i] || !borrowed_safe(peers[i],d,sizeof(*d)) ||
            peers[i]->config.adapter != b->adapter || peers[i]->config.callback_gate != b->gate ||
            peers[i]->config.security != p->security || !same_handle(peers[i]->config.session,p->session) ||
            peers[i]->config.link_slot != p->link_slot || peers[i]->config.link_generation != p->facts.link_generation ||
            peers[i]->config.physical_peer != p->sender_discriminator) return UCN_ERR_ARGUMENT;
    }
    d->root=b; d->native=native; d->facade_lifecycle=range;
    for (i=0U;i<count;++i) {
        d->current[i]=peers[i];
        if (selected(d,i,now) != UCN_OK) { memset(d,0,sizeof(*d)); return UCN_ERR_STATE; }
        if(!profile_supports(d,&peers[i]->config.candidate) || b->neighbors[i].capability_bits!=3U) {
            memset(d,0,sizeof(*d)); return UCN_ERR_UNSUPPORTED;
        }
        d->predecessor[i]=d->reference;
        if(history(d,i,&d->peer_history[i],&d->physical_mtu[i])!=UCN_OK) { memset(d,0,sizeof(*d)); return UCN_ERR_STATE; }
    }
    d->magic=DYNAMIC_MAGIC; b->dynamic=d;
    return UCN_OK;
}
ucn_result_t ucn_i_native_dynamic_neighbor(ucn_i_native_dynamic_t *d,uint16_t peer,size_t bytes,uint64_t now)
{
    ucn_result_t r; uint32_t generation; uint16_t mtu; bool ready;
    if (!d || d->magic != DYNAMIC_MAGIC || peer>=d->root->neighbor_count) return UCN_ERR_STATE;
    r=selected(d,peer,now); if (r != UCN_OK) return r;
    r=ucn_i_adapter_link_snapshot(d->root->adapter,d->root->neighbors[peer].link_slot,&generation,&mtu,&ready);
    if(r!=UCN_OK || !ready || mtu!=d->physical_mtu[peer] || generation!=d->peer_history[peer].link_generation) return UCN_ERR_STATE;
    if(!profile_supports(d,&d->current[peer]->config.candidate)) return UCN_ERR_UNSUPPORTED;
    return bytes<=d->cached.record.link.link_frame_mtu && bytes<=d->cached.record.link.processing_frame_mtu ? UCN_OK : UCN_ERR_NO_SPACE;
}
ucn_result_t ucn_i_native_dynamic_endpoint(ucn_i_native_dynamic_t *d,uint16_t endpoint,uint64_t now)
{
    uint16_t i; const ucn_i_native_endpoint_t *e=&d->root->endpoints[endpoint];
    for (i=0U;i<d->root->neighbor_count;++i)
        if (d->root->neighbors[i].security==e->security && same_handle(d->root->neighbors[i].session,e->session))
            return ucn_i_native_dynamic_neighbor(d,i,0U,now);
    /* Independent multi-hop Origin Session remains owned by its own endpoint. */
    return UCN_OK;
}
ucn_result_t ucn_i_native_dynamic_message(ucn_i_native_dynamic_t *d,uint16_t endpoint,size_t bytes,uint64_t now)
{
    uint16_t i; const ucn_i_native_endpoint_t *e=&d->root->endpoints[endpoint];
    for(i=0U;i<d->root->neighbor_count;++i) if(d->root->neighbors[i].security==e->security &&
        same_handle(d->root->neighbors[i].session,e->session)) {
        ucn_result_t r=ucn_i_native_dynamic_neighbor(d,i,0U,now); if(r!=UCN_OK) return r;
        if(d->profile.max_message_class>UCN_I_MESSAGE_T8K || bytes>(32U<<d->profile.max_message_class)) return UCN_ERR_NO_SPACE;
    }
    return UCN_OK;
}
void ucn_i_native_dynamic_stop_session(ucn_i_native_dynamic_t *d,ucn_i_security_owner_t *owner,
    ucn_i_security_handle_t session,uint64_t now)
{
    uint16_t i;
    for (i=0U;i<d->root->neighbor_count;++i)
        if (d->current[i]->config.security==owner && same_handle(d->current[i]->config.session,session))
            (void)ucn_i_native_capability_stop(d->current[i],now);
}
ucn_result_t ucn_i_native_dynamic_step(ucn_i_native_dynamic_t *d)
{
    uint16_t slot; ucn_i_native_capability_t *c; ucn_result_t r;
    if (!d || d->magic!=DYNAMIC_MAGIC) return UCN_ERR_STATE;
    slot=(uint16_t)(d->cursor++ % (4U*d->root->neighbor_count));
    if ((slot%4U)>=2U) {
        ucn_i_native_reauth_t *reauth=d->reauth[slot/4U];
        if (!reauth) return UCN_OK;
        if (d->root->lifecycle!=UCN_LIFECYCLE_RUNNING) (void)ucn_i_native_reauth_stop(reauth,d->root->now);
        if ((slot%4U)==2U) {
            (void)ucn_i_native_reauth_retire_one(reauth,d->root->now);
            if (d->root->lifecycle==UCN_LIFECYCLE_RUNNING) (void)ucn_i_native_reauth_drive_one(reauth,d->root->now);
        } else (void)ucn_i_native_reauth_finish_durability(reauth,d->root->now);
        return UCN_OK;
    }
    c=(slot&1U)?d->pending[slot/4U]:d->current[slot/4U];
    if (!c) return UCN_OK;
    if (d->root->lifecycle!=UCN_LIFECYCLE_RUNNING) (void)ucn_i_native_capability_stop(c,d->root->now);
    r=ucn_i_native_capability_retire_one(c,d->root->now);
    if (r!=UCN_OK && r!=UCN_ERR_STATE && r!=UCN_ERR_NOT_FOUND) return r;
    if (d->root->lifecycle==UCN_LIFECYCLE_RUNNING) (void)ucn_i_native_capability_drive_one(c,d->root->now);
    return UCN_OK;
}
bool ucn_i_native_dynamic_quiet(ucn_i_native_dynamic_t *d)
{
    uint16_t i; bool q;
    for(i=0U;i<d->root->neighbor_count;++i) {
        if (ucn_i_native_capability_quiescent(d->current[i],&q)!=UCN_OK || !q) return false;
        if (d->pending[i] && (ucn_i_native_capability_quiescent(d->pending[i],&q)!=UCN_OK || !q)) return false;
        if (d->reauth[i] && (ucn_i_native_reauth_quiescent(d->reauth[i],&q)!=UCN_OK || !q)) return false;
    }
    return true;
}
ucn_result_t ucn_i_native_dynamic_receive(ucn_i_native_dynamic_t *d)
{
    uint16_t i; ucn_i_native_routed_t *b=d->root; ucn_i_native_capability_t *c;
    if (ucn_i_c0_decode(b->rx.frame,b->rx.frame_bytes,b->width,UCN_I_CONTROL_H1,&d->decoded)!=UCN_OK &&
        ucn_i_c0_decode(b->rx.frame,b->rx.frame_bytes,b->width,UCN_I_CONTROL_H0,&d->decoded)!=UCN_OK) return UCN_ERR_MALFORMED;
    for(i=0U;i<b->neighbor_count;++i)
        if(b->neighbors[i].link_slot==b->rx.link_slot && b->neighbors[i].sender_discriminator==b->rx.meta.sender_discriminator) break;
    if(i==b->neighbor_count) return UCN_ERR_ACCESS;
    if(d->decoded.opcode>=0x10U && d->decoded.opcode<=0x14U)
        return d->reauth[i]?ucn_i_native_reauth_receive_claimed(d->reauth[i],&b->rx,b->now):UCN_ERR_STATE;
    if (d->decoded.opcode<0x101U || d->decoded.opcode>0x108U) return UCN_ERR_NOT_FOUND;
    c=d->pending[i]?d->pending[i]:d->current[i];
    return ucn_i_native_capability_receive_claimed(c,&b->rx,b->now);
}
ucn_result_t ucn_i_native_dynamic_attach_reauth(ucn_i_native_dynamic_t *d,uint16_t peer,
    ucn_i_native_reauth_t *reauth,uint64_t now)
{
    bool drained=false,quiet=false; ucn_i_native_neighbor_t *n; ucn_result_t r;
    if(!actor_idle(d,now) || peer>=d->root->neighbor_count || !reauth || d->reauth[peer]) return UCN_ERR_STATE;
    n=&d->root->neighbors[peer];
    if(reauth->config.parents!=d->current[peer]->config.join_parents) return UCN_ERR_ARGUMENT;
    r=ucn_i_native_routed_session_drained(d->root,d->native,n->security,n->session,&drained);
    if(r!=UCN_OK || !drained || ucn_i_native_capability_quiescent(d->current[peer],&quiet)!=UCN_OK || !quiet) return UCN_ERR_STATE;
    r=ucn_i_native_reauth_bindings_current(reauth,now,&d->established); if(r!=UCN_OK) return r;
    d->reauth[peer]=reauth; return UCN_OK;
}
ucn_result_t ucn_i_native_dynamic_stage(ucn_i_native_dynamic_t *d,uint16_t peer,
    ucn_i_native_capability_t *next,uint64_t now)
{
    ucn_i_native_neighbor_t *old; bool drained=false,q=false; ucn_result_t r;
    if (!actor_idle(d,now) || peer>=d->root->neighbor_count || !next || d->pending[peer]) return UCN_ERR_STATE;
    old=&d->root->neighbors[peer];
    if (!d->reauth[peer] || ucn_i_native_reauth_session_current(d->reauth[peer],now,&d->established)!=UCN_OK ||
        !same_handle(d->established.session,next->config.session)) return UCN_ERR_STATE;
    if (!d->root->neighbor_fenced[peer] || next==d->current[peer] || next->stopped ||
        next->config.adapter!=d->root->adapter || next->config.callback_gate!=d->root->gate ||
        next->config.security!=old->security || next->config.join_parents!=d->current[peer]->config.join_parents ||
        next->config.capability_owner!=d->current[peer]->config.capability_owner ||
        next->config.link_slot!=old->link_slot || next->config.link_generation!=old->facts.link_generation ||
        next->config.physical_peer!=old->sender_discriminator || !next->config.has_predecessor ||
        memcmp(&next->config.predecessor,&d->predecessor[peer],sizeof(d->predecessor[peer])) ||
        !same_binding(&next->config.candidate.local,&old->facts.local) ||
        !same_binding(&next->config.candidate.peer,&old->facts.peer)) return UCN_ERR_ARGUMENT;
    r=ucn_i_native_routed_session_drained(d->root,d->native,old->security,old->session,&drained);
    if(r!=UCN_OK || !drained || ucn_i_native_capability_quiescent(d->current[peer],&q)!=UCN_OK || !q) return UCN_ERR_STATE;
    r=ucn_i_security_session_candidate_get(old->security,next->config.session,&d->candidate);
    if(r!=UCN_OK || d->candidate.session_generation<=d->predecessor[peer].session_generation ||
        memcmp(&d->candidate,&next->config.candidate,sizeof(d->candidate))) return UCN_ERR_STATE;
    d->pending[peer]=next; return UCN_OK;
}
ucn_result_t ucn_i_native_dynamic_commit(ucn_i_native_dynamic_t *d,uint16_t peer,uint64_t now)
{
    ucn_i_native_routed_t *b; ucn_i_native_neighbor_t *old; ucn_i_native_capability_t *next;
    uint16_t i,retired,physical; bool drained=false; ucn_result_t r;
    if(!actor_idle(d,now) || peer>=d->root->neighbor_count || !(next=d->pending[peer])) return UCN_ERR_STATE;
    b=d->root; old=&b->neighbors[peer];
    r=ucn_i_native_routed_session_drained(b,d->native,old->security,old->session,&drained);
    if(r!=UCN_OK || !drained) return UCN_ERR_STATE;
    r=ucn_i_native_capability_selected(next,now,&d->reference,&d->cached,&d->profile);
    if(r!=UCN_OK) return r;
    if(!profile_supports(d,&next->config.candidate)) return UCN_ERR_UNSUPPORTED;
    r=ucn_i_security_session_candidate_get(old->security,next->config.session,&d->candidate);
    if(r!=UCN_OK) return r;
    for(i=0U;i<b->endpoint_count;++i)
        if(b->endpoints[i].security==old->security && same_handle(b->endpoints[i].session,old->session) &&
            b->endpoint_generation[i]==UINT32_MAX) return UCN_ERR_EXHAUSTED;
    r=history(d,peer,&d->retired_peer,&physical); if(r!=UCN_OK || physical!=d->physical_mtu[peer]) return UCN_ERR_STATE;
    r=ucn_i_route_retire_peer_history(b->routes,&d->peer_history[peer],&retired); if(r!=UCN_OK) return r;
    r=ucn_i_native_reauth_handoff(d->reauth[peer],now,&d->established); if(r!=UCN_OK) return r;
    for(i=0U;i<b->endpoint_count;++i) {
        ucn_i_native_endpoint_t *e=&b->endpoints[i];
        if(e->security==old->security && same_handle(e->session,old->session)) {
            e->session=next->config.session; e->facts.now_us=now; e->facts.policy_generation=d->candidate.policy_generation;
            memcpy(e->fingerprint,d->candidate.origin_fingerprint,16U); ++b->endpoint_generation[i]; b->endpoint_fenced[i]=0U;
        }
    }
    old->session=next->config.session; old->facts.now_us=now; old->facts.policy_generation=d->candidate.policy_generation;
    memcpy(old->fingerprint,d->candidate.hop_fingerprint,16U); memcpy(old->control_fingerprint,d->candidate.origin_fingerprint,16U);
    b->neighbor_fenced[peer]=0U; b->now=now;
    d->current[peer]=next; d->pending[peer]=NULL; d->predecessor[peer]=d->reference;
    d->peer_history[peer]=d->retired_peer; d->physical_mtu[peer]=physical;
    /* Peer authenticated traffic under the successor proves ACCEPT reception.
     * It is now safe to stop the cached reply, not to discard its real token. */
    return UCN_OK;
}
ucn_result_t ucn_i_native_dynamic_detach_reauth(ucn_i_native_dynamic_t *d,uint16_t peer,uint64_t now)
{
    bool quiet=false;
    if(!actor_idle(d,now) || peer>=d->root->neighbor_count || !d->reauth[peer] ||
        d->pending[peer] || d->root->neighbor_fenced[peer] || !d->reauth[peer]->handed_off) return UCN_ERR_STATE;
    if(ucn_i_native_reauth_quiescent(d->reauth[peer],&quiet)!=UCN_OK || !quiet) return UCN_ERR_STATE;
    d->reauth[peer]=NULL; return UCN_OK;
}
