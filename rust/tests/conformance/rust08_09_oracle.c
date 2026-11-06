/* Native C codec peer; 独立原生C编解码互操作探针。 */
#include "internal/ucn_transport.h"
#include "internal/ucn_realtime.h"
#include "internal/ucn_service.h"
#include "internal/ucn_operation_wire.h"
#include "internal/ucn_control_wire.h"
#include "internal/ucn_group.h"
#include "internal/ucn_capability.h"
#include "internal/ucn_admission.h"
#include "internal/ucn_wire.h"
#include "internal/ucn_route.h"
#include <stdio.h>
#include <string.h>
static void emit(const char *name,const uint8_t *bytes,size_t n){size_t i;printf("%s=",name);for(i=0;i<n;++i){printf("%02X",bytes[i]);}puts("");}
int main(void){
    {
        ucn_i_route_rreq_message_t q;ucn_i_route_rrep_message_t r;ucn_i_route_fragment_t f;
        uint8_t raw[99];size_t n,i,part,offset,total;uint8_t count,index;char name[64];
        memset(&q,0,sizeof(q));q.key.realm=7U;q.key.origin.address=1U;q.key.origin.generation=2U;memset(q.key.origin.principal,0xa1,16U);
        q.key.origin_session_generation=3U;q.key.target_address=9U;q.key.transaction_id=UINT64_C(0x0102030405060708);q.remaining_hops=8U;
        q.payload.accumulated_cost=UINT32_C(0x01020304);q.payload.minimum_frame_mtu=100U;q.payload.required_capability_bits=3U;
        if(ucn_i_route_control_request_encode(&q,2U,raw)!=UCN_OK){return 1;}emit("route_request_full",raw,54U);
        memset(&r,0,sizeof(r));r.key=q.key;memset(r.payload.destination_principal,0xc3,16U);r.payload.destination_binding_generation=4U;r.payload.hop_count=1U;r.payload.accumulated_cost=12U;r.payload.path_frame_mtu=127U;r.payload.capability_bits=3U;
        if(ucn_i_route_control_reply_encode(&r,UINT32_C(0x11223344),2U,raw)!=UCN_OK){return 1;}emit("route_reply_full",raw,77U);
        for(i=0U;i<3U;++i){total=i==0U?77U:99U;count=i==2U?3U:2U;part=(total+count-1U)/count;
            for(index=0U;index<count;++index){size_t j;memset(&f,0,sizeof(f));f.bundle=q.key.transaction_id;f.opcode=total==77U?0x0202U:0x0203U;f.total=(uint16_t)total;f.index=index;f.count=count;
                offset=index*part;f.length=(uint8_t)((total-offset)<part?(total-offset):part);for(j=0U;j<f.length;++j){f.chunk[j]=(uint8_t)(offset+j);}
                if(ucn_i_route_fragment_encode(&f,raw,sizeof(raw),&n)!=UCN_OK){return 1;}snprintf(name,sizeof(name),"route_fragment_%u_%u_%u",(unsigned)total,(unsigned)count,(unsigned)index);emit(name,raw,n);
            }
        }
    }
    {
        ucn_i_capability_fragment_t v;uint8_t raw[44],index;size_t total,i,n;char name[32];
        for(total=68U;total<=75U;total+=7U){for(index=0U;index<2U;++index){
            size_t half=(total+1U)/2U;memset(&v,0,sizeof(v));v.opcode=total==68U?0x0103U:0x0105U;
            v.total_bytes=(uint16_t)total;v.index=index;v.count=2U;v.chunk_bytes=(uint8_t)(index==0U?half:total-half);
            for(i=0U;i<v.chunk_bytes;++i){v.chunk[i]=(uint8_t)(index*half+i);}
            if(ucn_i_capability_fragment_encode(&v,raw,sizeof(raw),&n)!=UCN_OK){return 1;}
            snprintf(name,sizeof(name),"cap_fragment_%u_%u",(unsigned)total,(unsigned)index);emit(name,raw,n);
        }}
    }
    {
        ucn_i_admission_transcript_t v={0};
        ucn_i_admission_evidence_t evidence={0};
        ucn_i_admission_fragment_t fragment={0};
        ucn_i_c0_frame_t frame={0};
        uint8_t logical[508],raw[164],packet[128];size_t n,rn,pn,i;uint8_t phase;char name[64];
        v.protocol_version = UINT8_C(6);
        v.bootstrap_header_contract = UINT16_C(1);
        memset(v.joining_device_identity_digest,17U,16U);
        memset(v.authority_principal,34U,16U);
        v.authority_generation = UINT32_C(3);
        v.device_nonce = UINT64_C(0x0102030405060708);
        v.authority_nonce = UINT64_C(0x3132333435363738);
        v.transaction_id = UINT64_C(0x1112131415161718);
        v.lease_freshness_challenge_nonce = UINT64_C(0x2122232425262728);
        v.realm_id = UINT32_C(42);
        v.authority_address = UINT32_C(2);
        v.authority_binding_generation = UINT32_C(5);
        v.selected_link_id = UINT16_C(7);
        v.authority_lease_sequence = UINT64_C(6);
        v.authority_lease_duration_us = UINT64_C(10000);
        v.freshness_max_remaining_lease_us = UINT64_C(9000);
        memset(v.durable_fence_token,1U,16U);
        memset(v.allocation_high_water_digest,2U,16U);
        memset(v.quorum_config_digest,3U,32U);
        memset(v.signer_set_digest,4U,32U);
        memset(v.threshold_proof_digest,5U,32U);
        memset(v.freshness_proof_transcript_hash,6U,32U);
        v.authority_signer_count = UINT16_C(3);
        v.authority_quorum_threshold = UINT16_C(2);
        v.selected_link_generation = UINT32_C(9);
        memset(v.prior_messages_hash,49U,32U);
        evidence.length=128U;memset(evidence.bytes,0x77,128U);
        for(phase=2U;phase<=6U;++phase) {
            if(phase==3U) {
                memset(v.joining_device_principal,0x44,16U);v.selected_hop_suite=16U;
                v.selected_hop_key_id=1U;v.selected_hop_key_generation=2U;v.selected_e2e_mode=1U;
                v.selected_e2e_suite=1U;v.selected_e2e_key_id=3U;v.selected_e2e_key_generation=4U;
                v.selected_session_generation=5U;
            }
            if(phase==4U) {
                v.proposed_address=9U;v.address_binding_generation=11U;memset(v.binding_lease_id,0x55,16U);
                v.binding_lease_duration_us=2000U;v.binding_mode=1U;
            }
            if(ucn_i_admission_logical_encode((uint16_t)(phase+2U),phase,&v,&evidence,logical,sizeof(logical),&n)!=UCN_OK)return 51;
            (void)snprintf(name,sizeof(name),"bootstrap_event_%u",(unsigned)phase);emit(name,logical,n);
            if(phase==6U) {
                static ucn_i_admission_seed_workspace_t work;
                ucn_i_admission_session_seed_t seed;
                uint8_t packed[48];unsigned j;
                if(ucn_i_admission_session_seed(&v,100U,5000U,2000U,&seed,&work)!=UCN_OK)return 54;
                for(j=0U;j<8U;++j)packed[j]=(uint8_t)(seed.expires_at_us>>((7U-j)*8U));
                packed[8]=(uint8_t)(seed.origin_forward_key_id>>8U);packed[9]=(uint8_t)seed.origin_forward_key_id;
                packed[10]=(uint8_t)(seed.origin_reverse_key_id>>8U);packed[11]=(uint8_t)seed.origin_reverse_key_id;
                packed[12]=(uint8_t)(seed.hop_forward_key_id>>8U);packed[13]=(uint8_t)seed.hop_forward_key_id;
                packed[14]=(uint8_t)(seed.hop_reverse_key_id>>8U);packed[15]=(uint8_t)seed.hop_reverse_key_id;
                memcpy(packed+16U,seed.transcript_digest,32U);emit("bootstrap_session_seed",packed,sizeof(packed));
            }
            if(phase==2U) for(i=0U;i<9U;++i) {
                memset(&fragment,0,sizeof(fragment));fragment.phase=2U;fragment.chunk_bytes=63U;
                fragment.fragment_count=9U;fragment.fragment_index=(uint8_t)i;fragment.total_length=508U;
                fragment.fragment_offset=(uint16_t)(i*63U);fragment.fragment_length=(uint16_t)(508U-i*63U);
                if(fragment.fragment_length>63U)fragment.fragment_length=63U;
                fragment.transaction_id=v.transaction_id;memcpy(fragment.identity_digest,v.joining_device_identity_digest,16U);
                memcpy(fragment.data,logical+fragment.fragment_offset,fragment.fragment_length);
                if(ucn_i_admission_fragment_encode(&fragment,raw,sizeof(raw),&rn)!=UCN_OK)return 52;
                (void)snprintf(name,sizeof(name),"bootstrap_auth_fragment_%u",(unsigned)i);emit(name,raw,rn);
                if(i==0U || i==8U) {
                    frame.realm_id=42U;frame.destination_address=65535U;frame.transaction_id=v.transaction_id;
                    frame.opcode=4U;frame.interaction_role=UCN_INTERACTION_REQUEST;frame.payload_kind=UCN_I_PAYLOAD_CONTROL;
                    frame.hop_limit=1U;frame.payload=raw;frame.payload_bytes=rn;
                    if(ucn_i_c0_encode(&frame,2U,0U,packet,sizeof(packet),&pn)!=UCN_OK)return 53;
                    (void)snprintf(name,sizeof(name),"bootstrap_auth_frame_%u",(unsigned)i);emit(name,packet,pn);
                }
            }
        }
    }
    {
        ucn_i_admission_hello_t h={0};ucn_i_admission_cookie_challenge_t c={0};
        ucn_i_admission_hello_cookie_t echo={0};ucn_i_c0_frame_t frame={0};
        uint8_t body[98],hframe[73],cframe[73],eframe[128],chain[174],hash[32];
        ucn_i_sha256_workspace_t work;size_t hn,cn,en,bn,offset;
        const char domain[]="UCN6-BOOTSTRAP-COOKIE-V1";
        memset(h.identity_digest,0x11,16);h.device_nonce=UINT64_C(0x0102030405060708);h.transaction_id=UINT64_C(0x1112131415161718);
        c.transaction_id=h.transaction_id;c.cookie_time_bucket=0x01020304;c.issuer_link.id=7;c.issuer_link.generation=9;
        c.cookie_bytes=4;c.cookie[0]=1;c.cookie[1]=2;c.cookie[2]=3;c.cookie[3]=4;
        frame.realm_id=1;frame.destination_address=65535;frame.transaction_id=h.transaction_id;
        frame.hop_limit=1;frame.traffic_class=UCN_TRAFFIC_Q0;frame.delivery_guarantee=UCN_DELIVERY_BEST_EFFORT;
        frame.payload_kind=UCN_I_PAYLOAD_CONTROL;frame.origin_security=UCN_I_ORIGIN_O0;
        frame.interaction_role=UCN_INTERACTION_REQUEST;frame.opcode=UCN_I_OPCODE_BOOTSTRAP_HELLO;frame.payload=body;frame.payload_bytes=40;
        if(ucn_i_admission_hello_encode(&h,body)!=UCN_OK || ucn_i_c0_encode(&frame,2,UCN_I_CONTROL_H0,hframe,sizeof(hframe),&hn)!=UCN_OK)return 41;
        frame.interaction_role=UCN_INTERACTION_RESULT;frame.opcode=UCN_I_OPCODE_BOOTSTRAP_COOKIE_CHALLENGE;
        if(ucn_i_admission_cookie_challenge_encode(&c,body)!=UCN_OK || ucn_i_c0_encode(&frame,2,UCN_I_CONTROL_H0,cframe,sizeof(cframe),&cn)!=UCN_OK)return 42;
        offset=sizeof(domain)-1;memcpy(chain,domain,offset);chain[offset]=0;chain[offset+1]=0;chain[offset+2]=0;chain[offset+3]=(uint8_t)(hn+cn);offset+=4;
        memcpy(chain+offset,hframe,hn);memcpy(chain+offset+hn,cframe,cn);
        if(ucn_i_sha256_256(chain,offset+hn+cn,hash,&work)!=UCN_OK)return 43;
        echo.key.link=c.issuer_link;echo.key.local_peer_discriminator=1;echo.key.transaction_id=h.transaction_id;memcpy(echo.key.identity_digest,h.identity_digest,16);
        echo.device_nonce=h.device_nonce;echo.lease_freshness_challenge_nonce=UINT64_C(0x2122232425262728);
        memcpy(echo.prior_messages_hash,hash,32);echo.cookie_bytes=4;memcpy(echo.cookie,c.cookie,4);
        if(ucn_i_admission_hello_cookie_encode(&echo,body,sizeof(body),&bn)!=UCN_OK)return 44;
        frame.opcode=UCN_I_OPCODE_BOOTSTRAP_HELLO_COOKIE;frame.interaction_role=UCN_INTERACTION_REQUEST;frame.payload_bytes=bn;
        if(ucn_i_c0_encode(&frame,2,UCN_I_CONTROL_H0,eframe,sizeof(eframe),&en)!=UCN_OK)return 45;
        emit("bootstrap_hello",hframe,hn);emit("bootstrap_challenge",cframe,cn);emit("bootstrap_echo",eframe,en);emit("bootstrap_prior_hash",hash,32);
    }
    {
        ucn_i_capability_record_t local={0},peer;
        ucn_i_profile_requirements_t req={0};ucn_i_profile_select_t select;
        ucn_i_profile_ack_t ack;ucn_i_capability_summary_t summary;
        ucn_i_capability_query_t query;ucn_i_capability_select_context_t ctx={0};
        ucn_i_sha256_workspace_t work;uint8_t bytes[75],ld[16],pd[16],transcript[16];
        local.capability_generation=1;local.link.link_instance_generation=1;
        local.link.carrier_mtu=140;local.link.link_frame_mtu=128;local.link.processing_frame_mtu=128;
        local.link.carrier_header_bytes=4;local.link.carrier_crc_bytes=4;local.link.carrier_max_fragments=1;
        local.link.link_flags=8;local.link.nominal_rate_bps=1000000;local.link.hardware_priority_count=1;
        local.peer.feature_bits=0x010b;local.peer.hop_suite_bits=2;local.peer.e2e_suite_bits=14;
        local.peer.max_message_class=2;local.peer.max_rx_window=4;local.peer.max_concurrent_transfers=2;
        peer=local;peer.capability_generation=7;peer.peer.max_rx_window=2;
        req.required_feature_bits=0x010b;req.required_hop_suite_bits=2;req.required_e2e_suite_bits=14;
        req.minimum_message_class=2;req.minimum_rx_window=1;req.minimum_concurrent_transfers=1;
        if(ucn_i_capability_digest(&local,ld)!=UCN_OK || ucn_i_capability_digest(&peer,pd)!=UCN_OK ||
           ucn_i_profile_select_build(&local,ld,&peer,pd,&req,&select)!=UCN_OK)return 31;
        ctx.realm=1;ctx.source_address=1;ctx.source_generation=1;memset(ctx.source_principal,1,16);
        ctx.destination_address=2;ctx.destination_generation=1;memset(ctx.destination_principal,2,16);
        ctx.session=1;ctx.key=1;ctx.policy=1;ctx.origin=1;memset(ctx.fingerprint,5,16);
        ctx.transaction=UINT64_C(0x0102030405060708);ctx.service=0x40;
        if(ucn_i_capability_select_transcript(&ctx,&select,transcript,&work)!=UCN_OK)return 32;
        if(ucn_i_capability_record_encode(&local,bytes)!=UCN_OK){return 33;}emit("capability_record",bytes,68);
        emit("capability_digest",ld,16);
        summary.capability_generation=1;summary.link_instance_generation=1;memcpy(summary.digest,ld,16);
        if(ucn_i_capability_summary_encode(&summary,bytes)!=UCN_OK){return 34;}emit("capability_summary",bytes,24);
        query.requested_generation=1;memcpy(query.known_digest,ld,16);
        if(ucn_i_capability_query_encode(&query,bytes)!=UCN_OK){return 35;}emit("capability_query",bytes,20);
        if(ucn_i_profile_select_encode(&select,bytes)!=UCN_OK){return 36;}emit("capability_select",bytes,59);
        emit("capability_select_transcript",transcript,16);
        ack.selected=select;memcpy(ack.transcript_digest,transcript,16);
        if(ucn_i_profile_ack_encode(&ack,bytes)!=UCN_OK){return 37;}emit("capability_ack",bytes,75);
    }
    { uint8_t codes[4];
      codes[0]=(uint8_t)(UCN_I_DURABLE_RPC_NO_EFFECT>>8);codes[1]=(uint8_t)UCN_I_DURABLE_RPC_NO_EFFECT;
      codes[2]=(uint8_t)(UCN_I_DURABLE_RPC_IN_DOUBT>>8);codes[3]=(uint8_t)UCN_I_DURABLE_RPC_IN_DOUBT;
      emit("durable_result_codes",codes,4);
    }
    /* Independent C byte fixture for the existing conditional Data Envelope.
     * This is not a claim that the C Runtime already dispatches raw RPC. */
    {
        uint8_t payload[14]; unsigned role, i; uint64_t operation=UINT64_C(0x0102030405060708);
        for(role=1;role<=3;++role){
            size_t prefix=role==1?9U:11U;
            for(i=0;i<8;++i){payload[i]=(uint8_t)(operation>>(56U-8U*i));}
            payload[8]=role==1?1U:5U;
            if(!ucn_i_operation_flags_valid((uint8_t)role,payload[8])){return 24;}
            if(role!=1){payload[9]=role==3?255U:0U;payload[10]=role==3?255U:7U;}
            memcpy(payload+prefix,"abc",3);
            emit(role==1?"rpc_request":role==2?"rpc_result":"rpc_error",payload,prefix+3);
        }
    }
    {
        ucn_i_group_context_config_t g={0};uint8_t record[UCN_I_GROUP_RECORD_BYTES];
        ucn_i_group_allocator_record_t allocation={1,10,3}, decoded;uint8_t allocator_body[16];
        if(ucn_i_group_allocator_record_encode(&allocation,allocator_body)!=UCN_OK){return 22;}
        if(ucn_i_group_allocator_record_decode(allocator_body,&decoded)!=UCN_OK || decoded.high_water!=10){return 23;}
        emit("group_allocator",allocator_body,16);
        g.realm_id=1;g.group_id=1;g.group_generation=1;g.policy_generation=1;g.member_generation=1;
        g.endpoint=0x40;g.opcode=9;g.mode=UCN_I_GROUP_DYNAMIC;g.member_count=1;g.unicast_fanout_limit=1;
        g.quorum_weight=1;g.allowed_scope_mask=31;g.secure_required=1;
        g.security.runtime_instance=8;g.security.owner_instance=9;g.security.generation=10;g.security.slot=11;
        g.security.valid=1;g.security.exact_principal=1;g.security.key_generation=1;memset(g.security.context_digest,5,16);
        g.members[0].address=2;g.members[0].binding_generation=1;g.members[0].sender_slot=1;g.members[0].weight=1;memset(g.members[0].principal,2,16);
        if(ucn_i_group_record_encode(&g,UCN_I_GROUP_ACTIVE,1,record)!=UCN_OK){return 21;}
        emit("group_context",record,sizeof(record));
    }
    {uint8_t flags[1024];unsigned role,value;for(role=0;role<4;++role){for(value=0;value<256;++value){flags[role*256+value]=(uint8_t)ucn_i_operation_flags_valid((uint8_t)role,(uint8_t)value);}}emit("operation_flags_matrix",flags,sizeof(flags));}
    uint8_t bytes[200];size_t n;unsigned kind;
    ucn_i_transport_delivery_ack_t ack={0};ucn_i_transport_transfer_setup_t setup={0};
    ucn_i_realtime_time_key_t key={0};ucn_i_realtime_time_follow_up_t stamp={0};ucn_i_realtime_envelope_t envelope={0};
    ack.service_id=0x1234;ack.origin_sequence=0x01020304;ack.receive_credit=7;if(ucn_i_transport_delivery_ack_encode(&ack,bytes)!=UCN_OK){return 1;}emit("ack",bytes,9);
    setup.parent_kind=1;setup.parent_generation=2;setup.transfer_id=3;setup.service_id=0x1234;setup.total_length=5;memset(setup.message_digest,0xa5,16);setup.delivery=2;setup.fragment_count=2;setup.fragment_budget=3;setup.lifetime_ms=1000;
    if(ucn_i_transport_transfer_setup_encode(&setup,bytes,sizeof(bytes),&n)!=UCN_OK){return 2;}emit("setup",bytes,n);
    setup.interaction=1;setup.operation_id=UINT64_C(0x0102030405060708);setup.operation_flags=1;
    if(ucn_i_transport_transfer_setup_encode(&setup,bytes,sizeof(bytes),&n)!=UCN_OK){return 3;}emit("request_setup",bytes,n);
    {
        ucn_i_transport_transfer_key_t k={0};
        ucn_i_transport_transfer_setup_ack_t response={0};
        ucn_i_transport_transfer_abort_body_t abort_body={0};
        ucn_i_transport_transfer_terminal_body_t terminal={0};
        ucn_i_service_operation_wire_key_t op={0};
        ucn_i_service_operation_receipt_body_t receipt={0};
        ucn_i_service_operation_cancel_body_t cancel={0};
        ucn_i_sha256_workspace_t workspace={0};
        unsigned i;
        k.parent_kind=1;k.parent_generation=0x01020304;k.transfer_id=0x05060708;k.service_id=0x1234;
        for(i=0;i<16;++i){k.setup_digest[i]=(uint8_t)i;}
        response.key=k;response.accepted_fragment_budget=0x0102;response.receive_credit=0x0304;
        if(ucn_i_transport_transfer_setup_ack_encode(&response,bytes)!=UCN_OK){return 9;}emit("setup_ack",bytes,33);
        abort_body.key=k;abort_body.reason=7;if(ucn_i_transport_transfer_abort_body_encode(&abort_body,bytes)!=UCN_OK){return 10;}emit("transfer_abort",bytes,33);
        terminal.key=k;terminal.terminal_state=2;terminal.result_code=0x5678;if(ucn_i_transport_transfer_terminal_body_encode(&terminal,bytes)!=UCN_OK){return 11;}emit("transfer_terminal",bytes,32);
        op.operation_id=UINT64_C(0x0102030405060708);op.service_id=0x1234;op.service_opcode=0x2345;memcpy(op.request_digest,k.setup_digest,16);
        if(ucn_i_service_operation_wire_key_encode(&op,bytes)!=UCN_OK){return 12;}emit("operation_key",bytes,28);
        op.service_opcode=0;
        if(ucn_i_service_operation_wire_key_encode(&op,bytes)!=UCN_OK){return 25;}emit("operation_data_key",bytes,28);
        op.service_opcode=0x2345;
        if(ucn_i_service_operation_wire_key_encode(&op,bytes)!=UCN_OK){return 26;}
        { uint8_t result_body[37],result_digest[16];
          memcpy(result_body,bytes,28);result_body[28]=3;
          result_body[29]=0xff;result_body[30]=0xff;result_body[31]=0xff;result_body[32]=0xf4;
          result_body[33]=0;result_body[34]=2;result_body[35]='o';result_body[36]='k';
          if(UCN_I_DIGEST_CANONICAL("UCN6-OPERATION-RESULT-V1",result_body,sizeof(result_body),result_digest,&workspace)!=UCN_OK){return 23;}
          emit("operation_result_digest",result_digest,16);
        }
        receipt.key=op;memset(receipt.result_digest,0xa5,16);receipt.terminal_phase=3;if(ucn_i_service_operation_receipt_encode(&receipt,bytes)!=UCN_OK){return 13;}emit("operation_receipt",bytes,48);
        /* New control contract, independent byte producer, not C Runtime recovery support. */
        emit("operation_result_ack",bytes,48);
        emit("operation_resume",bytes,28);
        bytes[48]=0xff;bytes[49]=0xff;bytes[50]=0xff;bytes[51]=0xf4;
        bytes[52]=0;bytes[53]=2;bytes[54]='o';bytes[55]='k';emit("operation_recovered_result",bytes,56);
        cancel.key=op;cancel.reason=1;if(ucn_i_service_operation_cancel_encode(&cancel,bytes)!=UCN_OK){return 14;}emit("operation_cancel",bytes,32);
        if(ucn_i_transport_transfer_key_from_setup(&setup,&k)!=UCN_OK){return 15;}emit("setup_digest",k.setup_digest,16);
        if(UCN_I_DIGEST_CANONICAL("UCN6-AAD-V1",(const uint8_t *)"abc",3,bytes,&workspace)!=UCN_OK){return 16;}emit("aad_digest",bytes,16);
        if(UCN_I_DIGEST_CANONICAL("UCN6-PAYLOAD-V1",(const uint8_t *)"abc",3,bytes,&workspace)!=UCN_OK){return 17;}emit("payload_digest",bytes,16);
        if(UCN_I_DIGEST_CANONICAL("UCN6-TRANSFER-MESSAGE-V1",(const uint8_t *)"abc",3,bytes,&workspace)!=UCN_OK){return 18;}emit("message_digest",bytes,16);
    }
    for(kind=1;kind<=2;++kind){ucn_i_transport_fragment_prefix_t fragment={0};ucn_i_transport_sack_t sack={0};fragment.parent_kind=(uint8_t)kind;fragment.parent_generation=kind==1?2:0;fragment.transfer_id=3;fragment.fragment_index=1;fragment.fragment_count=2;
        if(ucn_i_transport_fragment_prefix_encode(&fragment,bytes,sizeof(bytes),&n)!=UCN_OK){return 4;}emit(kind==1?"c1_fragment":"flow_fragment",bytes,n);
        sack.parent_kind=(uint8_t)kind;sack.parent_generation=kind==1?2:0;sack.transfer_id=3;sack.original_service_id=kind==1?0x1234:0;sack.bitmap=3;sack.receive_credit=4;
        if(ucn_i_transport_sack_encode(&sack,bytes,sizeof(bytes),&n)!=UCN_OK){return 5;}emit(kind==1?"c1_sack":"flow_sack",bytes,n);
    }
    key.clock_domain_id=1;key.domain_generation=2;key.sync_sequence=3;if(ucn_i_realtime_time_key_encode(&key,bytes)!=UCN_OK){return 6;}emit("time_key",bytes,12);
    stamp.key=key;stamp.t1_master_tx_us=UINT64_C(0x0102030405060708);stamp.t1_uncertainty_us=7;if(ucn_i_realtime_time_follow_up_encode(&stamp,bytes)!=UCN_OK){return 7;}emit("time_stamp",bytes,24);
    envelope.capture_time_us=UINT64_C(0x0102030405060708);envelope.domain_generation=2;envelope.clock_domain_id=1;envelope.mode=3;envelope.uncertainty_class=11;envelope.sample_capture_hardware=1;envelope.domain_time_valid=1;
    if(ucn_i_realtime_envelope_encode(&envelope,bytes)!=UCN_OK){return 8;}emit("envelope",bytes,16);
    {
        const char *names[5]={"parent_prepare","parent_prepared","parent_committed","parent_commit","parent_abort"};
        const uint16_t ops[5]={0x0410U,0x0411U,0x0411U,0x0412U,0x0413U};
        const uint8_t dispositions[5]={0U,1U,2U,0U,7U};
        ucn_i_parent_publication_t value={0};size_t index,part;
        value.proposal_transaction=UINT64_C(0x0102030405060708);value.parent_generation=UINT32_C(0x11223344);
        for(part=0U;part<16U;++part){value.context_digest[part]=(uint8_t)part;}
        for(index=0U;index<5U;++index){value.disposition=dispositions[index];
            if(ucn_i_parent_publication_encode(ops[index],&value,bytes)!=UCN_OK){return 90;}
            emit(names[index],bytes,32U);}
    }
    {
        ucn_i_transport_parent_context_t parent={0};ucn_i_sha256_workspace_t workspace={0};
        parent.realm=1U;parent.source.address=1U;parent.destination.address=2U;
        parent.source.generation=1U;parent.destination.generation=1U;
        memset(parent.source.principal,1,16U);memset(parent.destination.principal,2,16U);
        parent.origin_security=1U;parent.transport_policy_generation=1U;
        parent.security_session_generation=1U;parent.security_key_generation=1U;parent.parent_generation=7U;
        if(ucn_i_transport_parent_record_encode(&parent,UINT32_C(0x05060708),UINT64_C(0x0102030405060708),bytes)!=UCN_OK){return 91;}
        emit("parent_record",bytes,88U);
        if(ucn_i_transport_parent_context_digest(&parent,bytes,&workspace)!=UCN_OK){return 92;}
        emit("parent_context_digest",bytes,16U);
    }
    {
        const uint16_t ops[5]={0x0410U,0x0411U,0x0412U,0x0413U,0x0901U};
        size_t source,phase,op,disposition,sent;
        printf("parent_phase_matrix=");
        for(source=0U;source<2U;++source){for(phase=0U;phase<6U;++phase){for(op=0U;op<5U;++op){
            for(disposition=0U;disposition<4U;++disposition){for(sent=0U;sent<4U;++sent){
                uint8_t next=255U,reply=255U;
                (void)ucn_i_parent_phase_transition(source!=0U,(uint8_t)phase,ops[op],(uint8_t)disposition,
                    (uint8_t)sent,&next,&reply);printf("%02X%02X",next,reply);
            }}}}}puts("");
    }
    return 0;
}
