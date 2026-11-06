/* Native Security sender/receiver with independent HMAC; Host-only fixture.
 * Uses the C test's Session lifecycle fixture, not a claim of physical Flash proof. */
#define main included_security_tests_main
#include "../../../tests/simplified/test_security_session.c"
#undef main
static ucn_result_t sha256_full(const uint8_t *b,size_t n,uint8_t out[32]){
    ucn_i_sha256_workspace_t w;uint8_t short_digest[16];unsigned i;
    ucn_result_t r=ucn_i_sha256_128(b,n,short_digest,&w);if(r!=UCN_OK)return r;
    for(i=0;i<8;++i){out[4*i]=(uint8_t)(w.state[i]>>24);out[4*i+1]=(uint8_t)(w.state[i]>>16);
        out[4*i+2]=(uint8_t)(w.state[i]>>8);out[4*i+3]=(uint8_t)w.state[i];}return UCN_OK;
}
static ucn_result_t mac(const ucn_i_security_key_selector_t *k,const uint8_t *aad,size_t an,
    const uint8_t *p,size_t pn,uint8_t out[16]){
    uint8_t b[1024],inner[32],outer[32];ucn_result_t r;
    if(k->suite_id!=1 || k->key_id>255 || an+pn>sizeof(b)-64)return UCN_ERR_ARGUMENT;
    memset(b,(uint8_t)(k->key_id^0x36),64);if(an!=0){memcpy(b+64,aad,an);}if(pn!=0){memcpy(b+64+an,p,pn);}
    r=sha256_full(b,64+an+pn,inner);if(r!=UCN_OK)return r;
    memset(b,(uint8_t)(k->key_id^0x5c),64);memcpy(b+64,inner,32);
    r=sha256_full(b,96,outer);if(r==UCN_OK)memcpy(out,outer,16);return r;
}
static ucn_result_t hmac_protect(void *context,const ucn_i_security_origin_protect_request_t *r){
    ucn_result_t result;(void)context;result=mac(&r->selector,r->aad,r->aad_bytes,r->plaintext,r->payload_bytes,r->origin_tag);
    if(result==UCN_OK){memcpy(r->protected_payload,r->plaintext,r->payload_bytes);}return result;
}
static ucn_result_t hmac_open(void *context,const ucn_i_security_origin_open_request_t *r){
    uint8_t tag[16];ucn_result_t result;(void)context;result=mac(&r->selector,r->aad,r->aad_bytes,r->protected_payload,r->payload_bytes,tag);
    if(result!=UCN_OK){return result;}if(memcmp(tag,r->origin_tag,16)!=0){return UCN_ERR_SECURITY;}
    memcpy(r->plaintext,r->protected_payload,r->payload_bytes);return UCN_OK;
}
int main(void){
    {
        uint8_t raw[208]={0},out[208],stage;
        ucn_i_peer_reauth_record_t record;char name[32];
        raw[0]=2U;raw[2]=2U;raw[7]=1U;raw[11]=1U;raw[15]=1U;memset(raw+16U,0x11,16U);raw[159]=1U;raw[163]=1U;
        raw[35]=2U;raw[39]=1U;memset(raw+40U,0x22,16U);raw[59]=1U;raw[63]=2U;
        raw[64]=1U;raw[65]=16U;raw[67]=1U;raw[69]=3U;raw[75]=2U;raw[79]=2U;
        raw[81]=7U;raw[87]=9U;raw[99]=3U;raw[107]=4U;raw[122]=3U;raw[123]=232U;memset(raw+124U,0x77,32U);
        for(stage=0U;stage<4U;++stage){
            raw[1]=stage;raw[83]=stage==0U?0U:8U;raw[91]=stage==0U?0U:10U;raw[115]=stage==0U?0U:5U;
            if(stage!=0U){raw[167]=1U;raw[171]=1U;raw[175]=1U;memset(raw+176U,0x77,32U);}
            if(ucn_i_peer_reauth_decode(raw,sizeof(raw),&record)!=UCN_OK ||
                ucn_i_peer_reauth_encode(&record,out,sizeof(out))!=UCN_OK)return 1;
            snprintf(name,sizeof(name),"peer_reauth_%u",(unsigned)stage);
            {size_t ri;printf("%s=",name);for(ri=0U;ri<sizeof(out);++ri)printf("%02X",out[ri]);puts("");}
            if(stage==2U){
                uint8_t logical[241],part[98],index;size_t offset,length,n,ri;
                ucn_i_peer_reauth_fragment_t fragment,decoded;
                memcpy(logical,out,208U);logical[208]=32U;memset(logical+209U,0x44,32U);
                for(index=0U;index<4U;++index){
                    offset=(size_t)index*80U;length=241U-offset;if(length>80U)length=80U;
                    memset(&fragment,0,sizeof(fragment));fragment.stage=2U;fragment.index=index;fragment.total_bytes=241U;fragment.transaction=3U;fragment.data_bytes=(uint8_t)length;memcpy(fragment.data,logical+offset,length);
                    if(ucn_i_peer_reauth_fragment_encode(&fragment,part,sizeof(part),&n)!=UCN_OK || ucn_i_peer_reauth_fragment_decode(part,n,&decoded)!=UCN_OK)return 1;
                    printf("peer_reauth_fragment_%u=",(unsigned)index);for(ri=0U;ri<n;++ri)printf("%02X",part[ri]);puts("");
                }
            }
        }
    }
    {
        uint8_t raw[208]={0},out[208];ucn_i_peer_reauth_record_t r;size_t i;
        raw[0]=2U;raw[1]=2U;raw[2]=2U;raw[7]=1U;raw[11]=1U;raw[15]=1U;memset(raw+16U,0x11,16U);
        raw[35]=2U;raw[39]=1U;memset(raw+40U,0x22,16U);raw[59]=1U;raw[63]=3U;
        raw[64]=1U;raw[65]=16U;raw[67]=1U;raw[69]=3U;raw[75]=3U;raw[79]=3U;
        raw[81]=7U;raw[83]=8U;raw[87]=9U;raw[91]=10U;raw[99]=3U;raw[107]=4U;raw[115]=5U;
        raw[122]=3U;raw[123]=232U;memset(raw+124U,0x77,32U);raw[159]=1U;raw[163]=1U;
        raw[167]=2U;raw[171]=2U;raw[175]=2U;memset(raw+176U,0x88,32U);
        if(ucn_i_peer_reauth_decode(raw,sizeof(raw),&r)!=UCN_OK||ucn_i_peer_reauth_encode(&r,out,sizeof(out))!=UCN_OK)return 1;
        printf("peer_reauth_asymmetric=");for(i=0U;i<sizeof(out);++i)printf("%02X",out[i]);puts("");
    }
    session_fixture_t s,r;ucn_i_security_candidate_t a=candidate_make(2,1),b;
    ucn_i_security_access_request_t access;ucn_i_security_tx_handle_t tx;
    ucn_i_security_c1_origin_material_t material;ucn_i_security_packet_workspace_t sw,rw;
    ucn_i_security_replay_handle_t replay;uint8_t packet[128],out[8],payload[]={97,98,99};size_t n=0,on=0,i;
    a.local.address=1;a.peer.address=2;a.local.binding_generation=1;a.peer.binding_generation=1;
    memset(a.local.principal,1,16);memset(a.peer.principal,2,16);memset(a.origin_fingerprint,5,16);
    a.address_width=2;a.origin_tx.key_id=1;a.origin_rx.key_id=2;a.origin_tx.key_generation=1;a.origin_rx.key_generation=1;
    b=a;b.local=a.peer;b.peer=a.local;b.origin_tx=a.origin_rx;b.origin_rx=a.origin_tx;
    CHECK(fixture_activate_profile(&s,&a,UCN_I_SECURITY_ACCESS_OUTBOUND,51,81,0x40,0,1,2)==0);
    CHECK(fixture_activate_profile(&r,&b,UCN_I_SECURITY_ACCESS_INBOUND,52,82,0x40,0,1,2)==0);
    s.owner.provider.protect_origin=hmac_protect;r.owner.provider.open_origin=hmac_open;
    memset(&access,0,sizeof(access));access.session=s.handle;memset(access.context_fingerprint,5,16);
    access.service_id=0x40;access.direction=UCN_I_SECURITY_ACCESS_OUTBOUND;
    CHECK(ucn_i_security_tx_reserve(&s.owner,&access,&s.facts,&tx)==UCN_OK);
    memset(&material,0,sizeof(material));material.session=s.handle;material.facts=s.facts;
    material.payload_bytes=3;material.source_address=1;material.destination_address=2;material.origin_sequence=tx.origin_sequence;
    material.service_id=0x40;material.common_header[0]=0x61;material.common_header[1]=0xa0;material.common_header[2]=0x48;
    material.direction=UCN_I_SECURITY_ACCESS_OUTBOUND;
    CHECK(ucn_i_security_c1_protect(&s.owner,tx,&material,payload,&sw,packet,sizeof(packet),&n)==UCN_OK);
    access.session=r.handle;access.direction=UCN_I_SECURITY_ACCESS_INBOUND;
    CHECK(ucn_i_security_c1_open(&r.owner,r.handle,&r.facts,&access,packet,n,&rw,out,sizeof(out),&on,&replay)==UCN_OK);
    CHECK(on==3 && memcmp(out,payload,3)==0);CHECK(ucn_i_security_replay_commit(&r.owner,&replay,&r.facts)==UCN_OK);
    printf("protected_c1=");for(i=0;i<n;++i)printf("%02X",packet[i]);puts("");
    CHECK(fixture_destroy(&s)==0);CHECK(fixture_destroy(&r)==0);return 0;
}
