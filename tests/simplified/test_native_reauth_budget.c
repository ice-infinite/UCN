#include "ucn/ucn_config.h"
#include "internal/ucn_native_reauth.h"
#if UCN_PERSIST_DOMAIN_COUNT >= 3U
#define main native_join_budget_prior_regressions
#include "test_native_join.c"
#undef main
#include "native_reauth_fixture.h"
static ucn_i_native_reauth_t initiator,responder;
static native_reauth_fixture_t initiate_product,respond_product;
static uint64_t reauth_now;
static int prepare_actual_join(void)
{
    REQUIRE(reset_pair()==0);
    for(reauth_now=301U;reauth_now<3000U;++reauth_now) {
        REQUIRE(tick(reauth_now,true)==0);
        if(device.join.active && authority.join.active) break;
    }
    REQUIRE(reauth_now<3000U);
    REQUIRE(ucn_i_native_join_handoff(&device.join,reauth_now,&current_device)==UCN_OK);
    REQUIRE(ucn_i_native_join_handoff(&authority.join,reauth_now,&current_authority)==UCN_OK);
    REQUIRE(ucn_i_security_fence(&device.security,current_device.session)==UCN_OK);
    REQUIRE(ucn_i_security_fence(&authority.security,current_authority.session)==UCN_OK);
    return 0;
}
static ucn_i_native_reauth_verify_fn verify_original;
static unsigned verify_calls;
static ucn_result_t counted_verify(void *context,const uint8_t principal[16],
    const uint8_t message[227],const uint8_t *proof,uint8_t bytes)
{
    ++verify_calls; return verify_original(context,principal,message,proof,bytes);
}
int main(void)
{
    uint8_t frames[4][UCN_ADAPTER_FRAME_BYTES]; size_t sizes[4];
    unsigned i,j,limit; ucn_link_handle_t link; ucn_rx_meta_t meta={0};
    ucn_i_adapter_rx_view_t rx; bool quiet=false;
    REQUIRE(prepare_actual_join()==0);
    native_reauth_fixture_configure(&initiate_product,&device,&initiator,true,0x1234U,101U);
    native_reauth_fixture_configure(&respond_product,&authority,&responder,false,0x5678U,0U);
    verify_original=respond_product.config.verify; respond_product.config.verify=counted_verify;
    REQUIRE(ucn_i_native_reauth_init(&initiator,&initiate_product.config,++reauth_now)==UCN_OK);
    REQUIRE(ucn_i_native_reauth_init(&responder,&respond_product.config,reauth_now)==UCN_OK);
    limit=4U*respond_product.config.max_attempts;
    for(i=0U;i<4U;++i) {
        REQUIRE(ucn_i_native_reauth_drive_one(&initiator,++reauth_now)==UCN_OK);
        REQUIRE(device.captured_bytes!=0U); sizes[i]=device.captured_bytes;
        memcpy(frames[i],device.captured,sizes[i]); device.captured_bytes=0U;
        REQUIRE(ucn_i_native_reauth_retire_one(&initiator,reauth_now)==UCN_OK);
    }
    frames[3][sizes[3]-1U]^=1U;
    REQUIRE(ucn_i_adapter_link_handle(&authority.adapter,0U,&link)==UCN_OK);
    meta.struct_size=sizeof(meta); meta.api_version=UCN_API_VERSION; meta.sender_discriminator=77U;
    for(j=0U;j<limit+2U;++j) for(i=0U;i<4U;++i) {
        ucn_result_t result;
        meta.timestamp_us=++reauth_now;
        REQUIRE(ucn_i_adapter_rx_publish(&authority.adapter,link,frames[i],sizes[i],&meta)==UCN_OK);
        REQUIRE(ucn_i_adapter_rx_claim(&authority.adapter,&rx)==UCN_OK);
        result=ucn_i_native_reauth_receive_claimed(&responder,&rx,reauth_now);
        REQUIRE(ucn_i_adapter_rx_retire(&authority.adapter,&rx.token)==UCN_OK);
        REQUIRE(result==(i==3U?(j<limit?UCN_ERR_SECURITY:UCN_ERR_EXHAUSTED):UCN_OK));
    }
    REQUIRE(verify_calls==limit && responder.attempts==0U && !responder.active);
    REQUIRE(ucn_i_native_reauth_stop(&responder,reauth_now)==UCN_OK);
    REQUIRE(ucn_i_native_reauth_retire_one(&responder,reauth_now)==UCN_OK);
    REQUIRE(ucn_i_native_reauth_quiescent(&responder,&quiet)==UCN_OK && quiet);
    return 0;
}
#else
int main(void) { return 0; } /* Dynamic JOIN/reauth explicitly unsupported. */
#endif
