/* Real raw JOIN creates the durable predecessor; no hand-made Session proof. */
#define main native_join_prior_regressions
#include "test_native_join.c"
#undef main
#include "internal/ucn_native_reauth.h"
#include "native_reauth_fixture.h"

#if UCN_PERSIST_DOMAIN_COUNT >= 3U
static ucn_i_native_reauth_t initiator, responder;
static native_reauth_fixture_t initiate_product, respond_product;
static uint64_t reauth_now;
static unsigned reauth_device_sent, reauth_authority_sent, reauth_opcodes[4];
static bool drop_accept;
static unsigned dropped_accepts;
static ucn_i_native_reauth_t *reauth_i=&initiator,*reauth_r=&responder;
static bool drop_response;
static uint8_t reauth_tamper_stage=255U;
static ucn_result_t reauth_rejected;
static ucn_i_native_reauth_t successor_i,successor_r;
static native_reauth_fixture_t successor_product_i,successor_product_r;
static native_join_fixture_t *reauth_left=&device,*reauth_right=&authority;
static native_join_fixture_t independent_i,independent_r;
static ucn_i_native_reauth_t reverse_i,reverse_r;
static native_reauth_fixture_t reverse_product_i,reverse_product_r;
static ucn_i_native_reauth_t untouched_reauth,zero_reauth;
static ucn_i_security_owner_t reauth_security_before;
static ucn_i_native_reauth_t stale_i,stale_peer;
static ucn_i_native_reauth_config_t stale_config;
static native_reauth_fixture_t stale_peer_product;

static int prepare_actual_join(void)
{
    REQUIRE(reset_pair() == 0);
    for (reauth_now = 301U; reauth_now < 3000U; ++reauth_now) {
        REQUIRE(tick(reauth_now, true) == 0);
        if (device.join.active && authority.join.active) break;
    }
    REQUIRE(reauth_now < 3000U);
    REQUIRE(ucn_i_native_join_handoff(&device.join, reauth_now, &current_device) == UCN_OK);
    REQUIRE(ucn_i_native_join_handoff(&authority.join, reauth_now, &current_authority) == UCN_OK);
    REQUIRE(ucn_i_security_fence(&device.security, current_device.session) == UCN_OK);
    REQUIRE(ucn_i_security_fence(&authority.security, current_authority.session) == UCN_OK);
    return 0;
}

#ifdef UCN_NATIVE_REAUTH_BASELINE_RED
/* Existing JOIN cannot be reused as a successor engine; this baseline exposes
 * its actual irreversible handoff/reinit rejection rather than a synthetic
 * successful Session DTO. New Reauth must preserve this original root. */
static ucn_result_t baseline_start(ucn_i_native_reauth_t *r,
    const ucn_i_native_reauth_config_t *c, uint64_t now)
{ (void)r; return ucn_i_native_join_init(c->parents, &c->parents->config, now); }
#define start_reauth baseline_start
#else
#define start_reauth ucn_i_native_reauth_init
#endif

static int reauth_prepare_pair(void)
{
    REQUIRE(prepare_actual_join() == 0);
    memset(&initiator,0,sizeof(initiator)); memset(&responder,0,sizeof(responder));
    native_reauth_fixture_configure(&initiate_product,&device,&initiator,true,0x1234U,101U);
    native_reauth_fixture_configure(&respond_product,&authority,&responder,false,0x5678U,0U);
    REQUIRE(start_reauth(&initiator, &initiate_product.config, ++reauth_now) == UCN_OK);
    REQUIRE(start_reauth(&responder, &respond_product.config, reauth_now) == UCN_OK);
    reauth_device_sent=device.submit_calls; reauth_authority_sent=authority.submit_calls;
    memset(reauth_opcodes,0,sizeof(reauth_opcodes)); dropped_accepts=0U; drop_accept=false;
    drop_response=false; reauth_i=&initiator; reauth_r=&responder;
    reauth_left=&device; reauth_right=&authority;
    reauth_tamper_stage=255U; reauth_rejected=UCN_OK;
    return 0;
}
#ifndef UCN_NATIVE_REAUTH_BASELINE_RED
static int reauth_transfer(native_join_fixture_t *from,native_join_fixture_t *to,
    ucn_i_native_reauth_t *root,unsigned *seen,uint64_t now)
{
    ucn_link_handle_t link; ucn_rx_meta_t meta={0}; ucn_i_adapter_rx_view_t rx;
    ucn_i_c0_frame_t frame; ucn_result_t result; unsigned stage;
    if(*seen==from->submit_calls)return 0;
    REQUIRE(from->submit_calls==*seen+1U); *seen=from->submit_calls;
    REQUIRE(ucn_i_c0_decode(from->captured,from->captured_bytes,2U,0U,&frame)==UCN_OK);
    stage=frame.opcode==0x14U?3U:frame.opcode-0x10U; REQUIRE(stage<4U); ++reauth_opcodes[stage];
    if(drop_accept && stage==3U) { ++dropped_accepts; from->captured_bytes=0U; return 0; }
    if(drop_response && stage==2U) { from->captured_bytes=0U; return 0; }
    if(stage==reauth_tamper_stage && frame.payload[2]+1U==frame.payload[3])
        from->captured[from->captured_bytes-1U]^=1U;
    REQUIRE(ucn_i_adapter_link_handle(&to->adapter,0U,&link)==UCN_OK);
    meta.struct_size=sizeof(meta); meta.api_version=UCN_API_VERSION; meta.timestamp_us=now;
    meta.sender_discriminator=77U;
    REQUIRE(ucn_i_adapter_rx_publish(&to->adapter,link,from->captured,from->captured_bytes,&meta)==UCN_OK);
    from->captured_bytes=0U; REQUIRE(ucn_i_adapter_rx_claim(&to->adapter,&rx)==UCN_OK);
    result=ucn_i_native_reauth_receive_claimed(root,&rx,now);
    REQUIRE(ucn_i_adapter_rx_retire(&to->adapter,&rx.token)==UCN_OK);
    if(stage==reauth_tamper_stage && result!=UCN_OK) { reauth_rejected=result; return 0; }
    if(result!=UCN_OK)fprintf(stderr,"Reauth RX stage%u phase%u result%ld\n",stage,root->phase,(long)result);
    REQUIRE(result==UCN_OK); return 0;
}
static int reauth_media(native_join_fixture_t *f,ucn_i_native_reauth_t *root,uint64_t now,bool allowed)
{
    ucn_persistence_step_result_t step; ucn_result_t result;
    if(!allowed || !root->work)return 0;
    REQUIRE(ucn_i_persistence_step(f->persistence,now,1U,&step)==UCN_OK);
    result=ucn_i_native_reauth_finish_durability(root,now);
    if(result!=UCN_OK)fprintf(stderr,"Reauth finish phase%u result%ld\n",root->phase,(long)result);
    REQUIRE(result==UCN_OK); return 0;
}
static int reauth_advance_pair(uint64_t now,bool allow_initiator,bool allow_responder)
{
    ucn_result_t result;
    REQUIRE(ucn_i_native_reauth_retire_one(reauth_i,now)==UCN_OK);
    REQUIRE(ucn_i_native_reauth_retire_one(reauth_r,now)==UCN_OK);
    REQUIRE(reauth_media(reauth_left,reauth_i,now,allow_initiator)==0);
    REQUIRE(reauth_media(reauth_right,reauth_r,now,allow_responder)==0);
    result=ucn_i_native_reauth_drive_one(reauth_i,now);
    if(result!=UCN_OK)fprintf(stderr,"Reauth initiator phase%u drive%ld\n",reauth_i->phase,(long)result);
    REQUIRE(result==UCN_OK);
    result=ucn_i_native_reauth_drive_one(reauth_r,now);
    if(result!=UCN_OK)fprintf(stderr,"Reauth responder phase%u drive%ld\n",reauth_r->phase,(long)result);
    REQUIRE(result==UCN_OK);
    REQUIRE(reauth_transfer(reauth_left,reauth_right,reauth_r,&reauth_device_sent,now)==0);
    REQUIRE(reauth_transfer(reauth_right,reauth_left,reauth_i,&reauth_authority_sent,now)==0);
    return 0;
}
static int actual_raw_success(void)
{
    ucn_persistence_domain_view_t view; ucn_persist_domain_key_t key={0};
    REQUIRE(reauth_prepare_pair()==0);
    for(++reauth_now;reauth_now<10000U && !initiator.work;++reauth_now)
        REQUIRE(reauth_advance_pair(reauth_now,false,false)==0);
    REQUIRE(initiator.work && !initiator.active && reauth_opcodes[2]==0U);
    REQUIRE(ucn_i_native_reauth_session_current(&initiator,reauth_now,&current_device)!=UCN_OK);
    for(++reauth_now;reauth_now<10000U && !responder.work;++reauth_now)
        REQUIRE(reauth_advance_pair(reauth_now,true,false)==0);
    REQUIRE(responder.work && !responder.active && reauth_opcodes[3]==0U);
    REQUIRE(ucn_i_native_reauth_session_current(&initiator,reauth_now,&current_device)!=UCN_OK);
    drop_accept=true;
    for(++reauth_now;reauth_now<10000U && responder.phase!=8U;++reauth_now)
        REQUIRE(reauth_advance_pair(reauth_now,true,true)==0);
    REQUIRE(responder.phase==8U && dropped_accepts==4U);
    REQUIRE(ucn_i_native_reauth_session_current(&initiator,reauth_now,&current_device)!=UCN_OK);
    REQUIRE(ucn_i_native_reauth_session_current(&responder,reauth_now,&current_authority)==UCN_OK);
    drop_accept=false;
    for(++reauth_now;reauth_now<12000U && initiator.phase!=8U;++reauth_now)
        REQUIRE(reauth_advance_pair(reauth_now,true,true)==0);
    REQUIRE(initiator.phase==8U);
    REQUIRE(ucn_i_native_reauth_session_current(&initiator,reauth_now,&current_device)==UCN_OK);
    REQUIRE(ucn_i_native_reauth_session_current(&responder,reauth_now,&current_authority)==UCN_OK);
    REQUIRE(current_device.candidate.session_generation==2U && current_authority.candidate.session_generation==2U);
    REQUIRE(current_device.candidate.origin_tx.key_generation==5U && current_device.candidate.hop_tx.key_generation==3U);
    REQUIRE(memcmp(current_device.candidate.transcript_digest,current_authority.candidate.transcript_digest,32U)==0);
    REQUIRE(respond_product.sign_calls[3]==1U && initiate_product.sign_calls[2]==1U);
    key.domain_kind=UCN_PERSIST_DOMAIN_SECURITY_HIGH_WATER; key.domain_id=device.config.session_domain_id;
    REQUIRE(ucn_i_persistence_domain_get(device.persistence,key,&view)==UCN_OK);
    REQUIRE(view.record_generation==2U && view.current_transaction_id==2U && !view.pending);
    REQUIRE(device.join.handed_off && device.join.started_us==300U);
    REQUIRE(ucn_i_native_reauth_handoff(&initiator,reauth_now,&current_device)==UCN_OK);
    REQUIRE(ucn_i_native_reauth_stop(&initiator,reauth_now)==UCN_ERR_STATE);
    REQUIRE(ucn_i_native_reauth_handoff(&initiator,reauth_now,&current_device)==UCN_ERR_STATE);
    REQUIRE(ucn_i_native_reauth_drive_one(&initiator,reauth_now)==UCN_ERR_STATE);
    REQUIRE(ucn_i_native_reauth_session_current(&initiator,initiator.deadline_us,&current_device)==UCN_OK);
    puts("Reauth real raw 1/1 -> 2, dual Foundation gates and lost ACCEPT passed");
    return 0;
}
static int asymmetric_history(void)
{
    bool quiet; ucn_persistence_domain_view_t view; ucn_persist_domain_key_t key={0};
    REQUIRE(reauth_prepare_pair()==0); drop_response=true;
    for(++reauth_now;reauth_now<10000U && !initiator.active;++reauth_now)
        REQUIRE(reauth_advance_pair(reauth_now,true,true)==0);
    REQUIRE(initiator.active && !responder.active && !responder.work);
    REQUIRE(ucn_i_native_reauth_stop(&initiator,reauth_now)==UCN_OK);
    REQUIRE(ucn_i_native_reauth_stop(&responder,reauth_now)==UCN_OK);
    REQUIRE(ucn_i_native_reauth_retire_one(&initiator,reauth_now)==UCN_OK);
    REQUIRE(ucn_i_native_reauth_retire_one(&responder,reauth_now)==UCN_OK);
    REQUIRE(ucn_i_native_reauth_quiescent(&initiator,&quiet)==UCN_OK && quiet);
    REQUIRE(ucn_i_native_reauth_quiescent(&responder,&quiet)==UCN_OK && quiet);
    key.domain_kind=UCN_PERSIST_DOMAIN_SECURITY_HIGH_WATER; key.domain_id=device.config.session_domain_id;
    REQUIRE(ucn_i_persistence_domain_get(device.persistence,key,&view)==UCN_OK && view.record_generation==2U);
    key.domain_id=authority.config.session_domain_id;
    REQUIRE(ucn_i_persistence_domain_get(authority.persistence,key,&view)==UCN_OK && view.record_generation==1U);
    native_reauth_fixture_configure(&successor_product_i,&device,&successor_i,true,0x3333U,102U);
    native_reauth_fixture_configure(&successor_product_r,&authority,&successor_r,false,0x4444U,0U);
    REQUIRE(ucn_i_native_reauth_init(&successor_i,&successor_product_i.config,++reauth_now)==UCN_OK);
    REQUIRE(ucn_i_native_reauth_init(&successor_r,&successor_product_r.config,reauth_now)==UCN_OK);
    reauth_i=&successor_i; reauth_r=&successor_r; drop_response=false;
    for(++reauth_now;reauth_now<12000U && (successor_i.phase!=8U || successor_r.phase!=8U);++reauth_now)
        REQUIRE(reauth_advance_pair(reauth_now,true,true)==0);
    REQUIRE(ucn_i_native_reauth_session_current(&successor_i,reauth_now,&current_device)==UCN_OK);
    REQUIRE(ucn_i_native_reauth_session_current(&successor_r,reauth_now,&current_authority)==UCN_OK);
    REQUIRE(current_device.candidate.session_generation==3U && current_authority.candidate.session_generation==3U);
    REQUIRE(current_device.candidate.origin_tx.key_generation==6U && current_device.candidate.hop_tx.key_generation==4U);
    REQUIRE(memcmp(current_device.candidate.transcript_digest,current_authority.candidate.transcript_digest,32U)==0);
    REQUIRE(ucn_i_native_reauth_drive_one(&initiator,reauth_now)==UCN_ERR_STATE);
    puts("Reauth real interrupted 2/1 -> 3 without reset/rollback passed");
    return 0;
}
static int proof_and_holder_rejection(void)
{
    unsigned stage; bool quiet; ucn_tx_meta_t completion={0};
    for(stage=0;stage<4U;++stage) {
        REQUIRE(reauth_prepare_pair()==0); reauth_tamper_stage=(uint8_t)stage;
        for(++reauth_now;reauth_now<10000U && reauth_rejected==UCN_OK;++reauth_now)
            REQUIRE(reauth_advance_pair(reauth_now,true,true)==0);
        REQUIRE(reauth_rejected==UCN_ERR_SECURITY);
        REQUIRE(ucn_i_native_reauth_session_current(&initiator,reauth_now,&current_device)!=UCN_OK);
    }
    REQUIRE(reauth_prepare_pair()==0); device.driver_mode=2U; device.cancel_result=UCN_ERR_IN_DOUBT;
    REQUIRE(ucn_i_native_reauth_drive_one(&initiator,++reauth_now)==UCN_OK && initiator.tx_live);
    REQUIRE(ucn_i_native_reauth_stop(&initiator,reauth_now)==UCN_OK);
    REQUIRE(ucn_i_native_reauth_retire_one(&initiator,reauth_now)==UCN_ERR_IN_DOUBT);
    REQUIRE(ucn_i_native_reauth_quiescent(&initiator,&quiet)==UCN_OK && !quiet);
    completion.struct_size=sizeof(completion); completion.api_version=UCN_API_VERSION; completion.timestamp_us=reauth_now;
    REQUIRE(ucn_i_adapter_tx_complete(&device.adapter,initiator.tx_token,UCN_ERR_IN_DOUBT,&completion)==UCN_OK);
    REQUIRE(ucn_i_native_reauth_retire_one(&initiator,reauth_now)==UCN_OK);
    REQUIRE(ucn_i_native_reauth_quiescent(&initiator,&quiet)==UCN_OK && quiet);
    REQUIRE(ucn_i_native_reauth_drive_one(&initiator,reauth_now)==UCN_ERR_STATE);
    REQUIRE(reauth_prepare_pair()==0); device.driver_mode=1U;
    REQUIRE(ucn_i_native_reauth_drive_one(&initiator,++reauth_now)==UCN_OK);
    REQUIRE(initiator.attempts==1U && initiator.tx_live && !initiator.tx_complete);
    REQUIRE(ucn_i_native_reauth_retire_one(&initiator,reauth_now)==UCN_OK && !initiator.tx_live);
    REQUIRE(ucn_i_native_reauth_drive_one(&initiator,reauth_now)==UCN_OK && initiator.attempts==2U);
    REQUIRE(initiator.started_us==initiator.now_us-1U && initiate_product.sign_calls[0]==1U);
    return 0;
}
/* Independent real media builds the reverse asymmetric starting history. It
 * never rewinds either existing Owner, copies a proof or edits durable bytes. */
static int independent_join_pair(void)
{
    uint64_t now; unsigned left_seen=0U,right_seen=0U;
    REQUIRE(native_join_fixture_init(&independent_i,true)==UCN_OK);
    REQUIRE(native_join_fixture_init(&independent_r,false)==UCN_OK);
    REQUIRE(ucn_i_native_join_init(&independent_i.join,&independent_i.config,300U)==UCN_OK);
    REQUIRE(ucn_i_native_join_init(&independent_r.join,&independent_r.config,300U)==UCN_OK);
    tamper_opcode=0U; delayed_final_time=0U;
    for(now=301U;now<3000U;++now) {
        REQUIRE(ucn_i_native_join_retire_one(&independent_i.join,now)==UCN_OK);
        REQUIRE(ucn_i_native_join_retire_one(&independent_r.join,now)==UCN_OK);
        REQUIRE(media(&independent_i,now,true)==0 && media(&independent_r,now,true)==0);
        REQUIRE(ucn_i_native_join_drive_one(&independent_i.join,now)==UCN_OK);
        REQUIRE(ucn_i_native_join_drive_one(&independent_r.join,now)==UCN_OK);
        REQUIRE(transfer(&independent_i,&independent_r,&left_seen,now)==0);
        REQUIRE(transfer(&independent_r,&independent_i,&right_seen,now)==0);
        if(independent_i.join.active && independent_r.join.active)break;
    }
    REQUIRE(now<3000U);
    REQUIRE(ucn_i_native_join_handoff(&independent_i.join,now,&current_device)==UCN_OK);
    REQUIRE(ucn_i_native_join_handoff(&independent_r.join,now,&current_authority)==UCN_OK);
    REQUIRE(ucn_i_security_fence(&independent_i.security,current_device.session)==UCN_OK);
    REQUIRE(ucn_i_security_fence(&independent_r.security,current_authority.session)==UCN_OK);
    return 0;
}
static int reverse_asymmetric_history(void)
{
    REQUIRE(reauth_prepare_pair()==0);
    for(++reauth_now;reauth_now<10000U && (initiator.phase!=8U || responder.phase!=8U);++reauth_now)
        REQUIRE(reauth_advance_pair(reauth_now,true,true)==0);
    REQUIRE(initiator.phase==8U && responder.phase==8U);
    REQUIRE(ucn_i_native_reauth_stop(&initiator,reauth_now)==UCN_OK);
    REQUIRE(ucn_i_native_reauth_stop(&responder,reauth_now)==UCN_OK);
    REQUIRE(independent_join_pair()==0);
    native_reauth_fixture_configure(&reverse_product_i,&independent_i,&reverse_i,true,0x5555U,103U);
    native_reauth_fixture_configure(&reverse_product_r,&authority,&reverse_r,false,0x6666U,0U);
    REQUIRE(ucn_i_native_reauth_init(&reverse_i,&reverse_product_i.config,++reauth_now)==UCN_OK);
    REQUIRE(ucn_i_native_reauth_init(&reverse_r,&reverse_product_r.config,reauth_now)==UCN_OK);
    REQUIRE(reverse_i.previous.session_generation==1U && reverse_r.previous.session_generation==2U);
    reauth_i=&reverse_i; reauth_r=&reverse_r; reauth_left=&independent_i; reauth_right=&authority;
    reauth_device_sent=independent_i.submit_calls; reauth_authority_sent=authority.submit_calls;
    for(++reauth_now;reauth_now<12000U && (reverse_i.phase!=8U || reverse_r.phase!=8U);++reauth_now)
        REQUIRE(reauth_advance_pair(reauth_now,true,true)==0);
    REQUIRE(ucn_i_native_reauth_session_current(&reverse_i,reauth_now,&current_device)==UCN_OK);
    REQUIRE(ucn_i_native_reauth_session_current(&reverse_r,reauth_now,&current_authority)==UCN_OK);
    REQUIRE(current_device.candidate.session_generation==3U && current_authority.candidate.session_generation==3U);
    puts("Reauth independent actual Foundation histories 1/2 -> 3 passed");
    return 0;
}
static int pending_reload_stop_and_contract_checks(void)
{
    unsigned scenario; ucn_persistence_step_result_t step; ucn_persistence_request_view_t request;
    ucn_result_t result; bool quiet; ucn_i_callback_claim_t claim={99U,1U,1U,1U};
    for(scenario=0U;scenario<3U;++scenario) {
        REQUIRE(reauth_prepare_pair()==0);
        for(++reauth_now;reauth_now<10000U && !initiator.work;++reauth_now)
            REQUIRE(reauth_advance_pair(reauth_now,false,false)==0);
        REQUIRE(initiator.work && !initiator.active);
        if(scenario==1U)REQUIRE(ucn_i_native_reauth_stop(&initiator,reauth_now)==UCN_OK);
        if(scenario==2U)device.fake.corrupt_readback_once=1U;
        for(++reauth_now;reauth_now<12000U && initiator.work;++reauth_now) {
            REQUIRE(ucn_i_persistence_request_get(device.persistence,initiator.persistence_handle,&request)==UCN_OK);
            if(scenario==0U && request.provider_phase==UCN_I_PERSIST_SUBMIT_RELOAD_SLOT0)
                device.fake.fail_once_phase=UCN_PERSIST_IO_LOAD_SLOT;
            (void)ucn_i_persistence_step(device.persistence,reauth_now,1U,&step);
            result=ucn_i_native_reauth_finish_durability(&initiator,reauth_now);
            if(result!=UCN_OK) { REQUIRE(!initiator.work && scenario!=1U); break; }
        }
        REQUIRE(!initiator.work && !initiator.active && reauth_opcodes[2]==0U);
        REQUIRE(ucn_i_native_reauth_session_current(&initiator,reauth_now,&current_device)!=UCN_OK);
        if(scenario==1U)REQUIRE(ucn_i_native_reauth_quiescent(&initiator,&quiet)==UCN_OK && quiet);
    }
    REQUIRE(reauth_prepare_pair()==0);
    REQUIRE(ucn_i_callback_gate_enter(&device.callback_gate,&claim)==UCN_OK);
    quiet=true;
    REQUIRE(ucn_i_native_reauth_quiescent(&initiator,&quiet)==UCN_ERR_STATE && quiet);
    REQUIRE(ucn_i_native_reauth_drive_one(&initiator,reauth_now)==UCN_ERR_STATE);
    REQUIRE(ucn_i_callback_gate_leave(&device.callback_gate,&claim)==UCN_OK);
    reauth_security_before=device.security;
    REQUIRE(ucn_i_native_reauth_session_current(&initiator,reauth_now,
        (ucn_i_native_join_current_t *)(void *)&device.security)==UCN_ERR_ARGUMENT);
    REQUIRE(memcmp(&reauth_security_before,&device.security,sizeof(device.security))==0);
    REQUIRE(ucn_i_native_reauth_quiescent(&initiator,(bool *)(void *)&initiate_product.config)==UCN_ERR_ARGUMENT);
    REQUIRE(ucn_i_native_reauth_init(&untouched_reauth,&initiate_product.config,300U)!=UCN_OK);
    REQUIRE(memcmp(&untouched_reauth,&zero_reauth,sizeof(zero_reauth))==0);
    REQUIRE(ucn_i_native_reauth_drive_one(&initiator,initiator.deadline_us)==UCN_ERR_TIMEOUT);
    REQUIRE(initiator.started_us<initiator.deadline_us && !initiator.active);
    puts("Reauth failed/corrupt reload, stopped pending proof, alias/reentry and expiry passed");
    return 0;
}
static int captured_local_history_cannot_follow_new_media(void)
{
    ucn_persist_domain_key_t key={0}; ucn_persistence_domain_view_t view;
    REQUIRE(reauth_prepare_pair()==0);
    stale_config=initiate_product.config; stale_config.nonce=0xABCDU; stale_config.transaction=104U;
    REQUIRE(ucn_i_native_reauth_init(&stale_i,&stale_config,reauth_now)==UCN_OK);
    REQUIRE(stale_i.previous.session_generation==1U);
    for(++reauth_now;reauth_now<10000U && (initiator.phase!=8U || responder.phase!=8U);++reauth_now)
        REQUIRE(reauth_advance_pair(reauth_now,true,true)==0);
    REQUIRE(initiator.phase==8U && responder.phase==8U);
    REQUIRE(ucn_i_native_reauth_stop(&initiator,reauth_now)==UCN_OK);
    REQUIRE(ucn_i_native_reauth_stop(&responder,reauth_now)==UCN_OK);
    native_reauth_fixture_configure(&stale_peer_product,&authority,&stale_peer,false,0xBCDEU,0U);
    REQUIRE(ucn_i_native_reauth_init(&stale_peer,&stale_peer_product.config,++reauth_now)==UCN_OK);
    reauth_i=&stale_i; reauth_r=&stale_peer;
    for(++reauth_now;reauth_now<12000U && stale_i.phase!=4U;++reauth_now)
        REQUIRE(reauth_advance_pair(reauth_now,true,true)==0);
    REQUIRE(stale_i.phase==4U);
    REQUIRE(ucn_i_native_reauth_drive_one(&stale_i,reauth_now)==UCN_ERR_SECURITY);
    REQUIRE(!stale_i.work && !stale_i.active);
    key.domain_kind=UCN_PERSIST_DOMAIN_SECURITY_HIGH_WATER; key.domain_id=device.config.session_domain_id;
    REQUIRE(ucn_i_persistence_domain_get(device.persistence,key,&view)==UCN_OK);
    REQUIRE(view.record_generation==2U && view.current_transaction_id==2U && !view.pending);
    puts("Reauth rejects a captured local history changed by real intervening durability");
    return 0;
}
#endif
#ifndef NATIVE_REAUTH_FIXTURE_ONLY
int main(void)
{
#ifdef UCN_NATIVE_REAUTH_BASELINE_RED
    REQUIRE(reauth_prepare_pair()==0);
#else
    REQUIRE(actual_raw_success()==0);
    REQUIRE(asymmetric_history()==0);
    REQUIRE(proof_and_holder_rejection()==0);
    REQUIRE(reverse_asymmetric_history()==0);
    REQUIRE(pending_reload_stop_and_contract_checks()==0);
    REQUIRE(captured_local_history_cannot_follow_new_media()==0);
    printf("Reauth caller workspace %zu bytes\n",sizeof(ucn_i_native_reauth_t));
#endif
    return 0;
}
#endif
#else
int main(void)
{
    static ucn_i_native_reauth_t root;
    static ucn_i_native_join_t parent;
    static ucn_i_native_reauth_config_t config;
    config.parents=&parent;
    REQUIRE(ucn_i_native_reauth_init(&root,&config,300U)==UCN_ERR_UNSUPPORTED);
    REQUIRE(root.magic==0U);
    puts("Reauth requires the three-domain JOIN composition; default NANO unsupported");
    return 0;
}
#endif
